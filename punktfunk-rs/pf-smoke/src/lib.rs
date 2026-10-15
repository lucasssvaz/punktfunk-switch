// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

//! On-device smoke tests for Rust std, tokio and the socket crates punktfunk-ffi depends on.
//!
//! `pf_smoke_run` prints one `PASS`/`FAIL` line per test to stdout (the libnx console, mirrored to
//! the PC when the NRO is started through `nxlink -s`) and returns the number of failures.

use std::ffi::{c_char, c_int, CStr};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Barrier, Condvar, Mutex, RwLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

type TestResult = Result<String, String>;

macro_rules! ensure {
    ($cond:expr, $($fmt:tt)+) => {
        if !$cond {
            return Err(format!($($fmt)+));
        }
    };
}

fn e<E: std::fmt::Display>(what: &'static str) -> impl FnOnce(E) -> String {
    move |err| format!("{what}: {err}")
}

fn threads() -> TestResult {
    let counter = Arc::new(AtomicUsize::new(0));
    let handles: Vec<_> = (0..8)
        .map(|_| {
            let c = counter.clone();
            std::thread::Builder::new()
                .stack_size(256 * 1024)
                .spawn(move || {
                    for _ in 0..1000 {
                        c.fetch_add(1, Ordering::Relaxed);
                    }
                })
                .map_err(e("spawn"))
        })
        .collect::<Result<_, _>>()?;
    for h in handles {
        h.join().map_err(|_| "join: thread panicked".to_string())?;
    }
    let n = counter.load(Ordering::Relaxed);
    ensure!(n == 8000, "counter = {n}");
    let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0);
    Ok(format!("8 threads joined, available_parallelism = {cores}"))
}

fn sync_primitives() -> TestResult {
    let pair = Arc::new((Mutex::new(false), Condvar::new()));
    let p2 = pair.clone();
    let t = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(20));
        *p2.0.lock().unwrap() = true;
        p2.1.notify_one();
    });
    let (lock, cv) = &*pair;
    let (guard, timeout) = cv
        .wait_timeout_while(lock.lock().unwrap(), Duration::from_secs(2), |ready| !*ready)
        .unwrap();
    ensure!(*guard && !timeout.timed_out(), "condvar notify missed");
    drop(guard);
    t.join().unwrap();

    let start = Instant::now();
    let (g, r) = cv.wait_timeout(lock.lock().unwrap(), Duration::from_millis(50)).unwrap();
    drop(g);
    let waited = start.elapsed();
    ensure!(r.timed_out() && waited >= Duration::from_millis(45), "wait_timeout returned after {waited:?}");

    let rw = RwLock::new(1);
    {
        let a = rw.read().unwrap();
        let b = rw.read().unwrap();
        ensure!(*a + *b == 2, "rwlock read");
    }
    *rw.write().unwrap() = 5;

    let barrier = Arc::new(Barrier::new(4));
    let hs: Vec<_> = (0..3)
        .map(|_| {
            let b = barrier.clone();
            std::thread::spawn(move || {
                b.wait();
            })
        })
        .collect();
    barrier.wait();
    hs.into_iter().for_each(|h| h.join().unwrap());

    let (tx, rx) = mpsc::sync_channel::<u32>(4);
    std::thread::spawn(move || (0..100).for_each(|i| tx.send(i).unwrap()));
    let sum: u32 = rx.iter().sum();
    ensure!(sum == 4950, "mpsc sum = {sum}");

    thread_local!(static TL: std::cell::Cell<u32> = const { std::cell::Cell::new(7) });
    TL.with(|v| v.set(11));
    let other = std::thread::spawn(|| TL.with(|v| v.get())).join().unwrap();
    ensure!(TL.with(|v| v.get()) == 11 && other == 7, "thread_local not per-thread");
    Ok(format!("condvar timeout waited {waited:?}"))
}

fn time() -> TestResult {
    let t0 = Instant::now();
    std::thread::sleep(Duration::from_millis(100));
    let slept = t0.elapsed();
    ensure!(
        slept >= Duration::from_millis(95) && slept < Duration::from_millis(300),
        "sleep(100ms) took {slept:?}"
    );
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map_err(e("SystemTime"))?;
    ensure!(now.as_secs() > 1_700_000_000, "SystemTime looks unset: {now:?}");
    Ok(format!("sleep(100ms) = {slept:?}, unix time = {}", now.as_secs()))
}

fn random() -> TestResult {
    let mut a = [0u8; 32];
    let mut b = [0u8; 32];
    getrandom02::getrandom(&mut a).map_err(e("getrandom 0.2"))?;
    getrandom03::fill(&mut b).map_err(e("getrandom 0.3"))?;
    ensure!(a != [0; 32] && b != [0; 32] && a != b, "random bytes look constant");
    let mut c = [0u8; 300];
    getrandom04::fill(&mut c).map_err(e("getrandom 0.4 (300 bytes)"))?;
    let h1 = std::collections::hash_map::RandomState::new();
    let h2 = std::collections::hash_map::RandomState::new();
    use std::hash::BuildHasher;
    ensure!(h1.hash_one(1u32) != h2.hash_one(1u32), "HashMap seeds identical");
    Ok("getrandom 0.2/0.3/0.4 + HashMap seeding".into())
}

fn udp_loopback() -> TestResult {
    let a = UdpSocket::bind("127.0.0.1:0").map_err(e("bind a"))?;
    let b = UdpSocket::bind("127.0.0.1:0").map_err(e("bind b"))?;
    let b_addr = b.local_addr().map_err(e("local_addr"))?;
    b.set_read_timeout(Some(Duration::from_secs(2))).map_err(e("set_read_timeout"))?;
    let payload = [0xa5u8; 1400];
    for _ in 0..64 {
        a.send_to(&payload, b_addr).map_err(e("send_to"))?;
    }
    let mut buf = [0u8; 2048];
    let mut got = 0;
    while got < 64 {
        match b.recv_from(&mut buf) {
            Ok((n, _)) if n == payload.len() => got += 1,
            Ok((n, _)) => return Err(format!("short datagram: {n}")),
            Err(err) => return Err(format!("recv_from after {got}: {err}")),
        }
    }
    b.set_nonblocking(true).map_err(e("set_nonblocking"))?;
    match b.recv_from(&mut buf) {
        Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {}
        other => return Err(format!("nonblocking recv: {other:?}")),
    }
    Ok(format!("64 x 1400B datagrams via {b_addr}"))
}

fn tcp_loopback() -> TestResult {
    let l = TcpListener::bind("127.0.0.1:0").map_err(e("listen"))?;
    let addr = l.local_addr().map_err(e("local_addr"))?;
    let t = std::thread::spawn(move || -> std::io::Result<Vec<u8>> {
        let (mut s, _) = l.accept()?;
        let mut v = Vec::new();
        s.read_to_end(&mut v)?;
        s.write_all(b"ack")?;
        Ok(v)
    });
    let mut c = TcpStream::connect_timeout(&addr, Duration::from_secs(2)).map_err(e("connect"))?;
    c.set_nodelay(true).map_err(e("set_nodelay"))?;
    c.write_all(&vec![7u8; 256 * 1024]).map_err(e("write"))?;
    c.shutdown(std::net::Shutdown::Write).map_err(e("shutdown"))?;
    let mut ack = Vec::new();
    c.read_to_end(&mut ack).map_err(e("read ack"))?;
    let got = t.join().unwrap().map_err(e("server"))?;
    ensure!(got.len() == 256 * 1024 && ack == b"ack", "got {} bytes, ack {ack:?}", got.len());
    Ok("256 KiB round trip".into())
}

/// What libnx's resolver returned, as std sees it: family, length and the first sockaddr bytes.
fn raw_addrinfo(host: &str) -> String {
    let node = std::ffi::CString::new(host).unwrap();
    let mut hints: libc::addrinfo = unsafe { std::mem::zeroed() };
    hints.ai_socktype = libc::SOCK_STREAM;
    let mut res = std::ptr::null_mut();
    let rc = unsafe { libc::getaddrinfo(node.as_ptr(), std::ptr::null(), &hints, &mut res) };
    if rc != 0 {
        return format!("raw getaddrinfo = {rc}");
    }
    let mut out = Vec::new();
    let mut cur = res;
    while let Some(ai) = unsafe { cur.as_ref() } {
        let bytes = if ai.ai_addr.is_null() {
            "null".to_string()
        } else {
            let b = unsafe { std::slice::from_raw_parts(ai.ai_addr as *const u8, 8) };
            b.iter().map(|x| format!("{x:02x}")).collect()
        };
        out.push(format!("family {} addrlen {} addr {bytes}", ai.ai_family, ai.ai_addrlen));
        cur = ai.ai_next;
    }
    unsafe { libc::freeaddrinfo(res) };
    format!("raw: [{}]", out.join("; "))
}

fn dns() -> TestResult {
    let addrs: Vec<_> = ("example.com", 443).to_socket_addrs().map_err(e("resolve"))?.collect();
    ensure!(!addrs.is_empty(), "no addresses ({})", raw_addrinfo("example.com"));
    Ok(format!("example.com -> {}", addrs[0]))
}

fn sockets_crates() -> TestResult {
    use socket2::{Domain, Protocol, Socket, Type};
    let s = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP)).map_err(e("socket2 new"))?;
    s.set_recv_buffer_size(1 << 20).map_err(e("SO_RCVBUF"))?;
    let rcv = s.recv_buffer_size().map_err(e("get SO_RCVBUF"))?;
    s.set_reuse_address(true).map_err(e("SO_REUSEADDR"))?;
    s.bind(&"0.0.0.0:0".parse::<std::net::SocketAddr>().unwrap().into()).map_err(e("bind"))?;
    s.set_tos_v4(0xb8).map_err(e("IP_TOS"))?;
    let ifs = if_addrs::get_if_addrs().map_err(e("if-addrs"))?;
    let list: Vec<_> = ifs.iter().map(|i| format!("{}={}", i.name, i.ip())).collect();
    Ok(format!("SO_RCVBUF = {rcv}, interfaces: {}", list.join(" ")))
}

fn tokio_runtime(echo: Option<String>) -> TestResult {
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .thread_stack_size(256 * 1024)
        .enable_all()
        .build()
        .map_err(e("runtime"))?;
    rt.block_on(async move {
        let t0 = Instant::now();
        tokio::time::sleep(Duration::from_millis(50)).await;
        let slept = t0.elapsed();
        let a = tokio::net::UdpSocket::bind("127.0.0.1:0").await.map_err(e("bind a"))?;
        let b = tokio::net::UdpSocket::bind("127.0.0.1:0").await.map_err(e("bind b"))?;
        let b_addr = b.local_addr().map_err(e("local_addr"))?;
        let recv = tokio::spawn(async move {
            let mut buf = [0u8; 1500];
            let mut n = 0;
            while n < 256 {
                b.recv_from(&mut buf).await?;
                n += 1;
            }
            Ok::<_, std::io::Error>(n)
        });
        for _ in 0..256 {
            a.send_to(&[1u8; 1200], b_addr).await.map_err(e("send_to"))?;
            tokio::task::yield_now().await;
        }
        let n = tokio::time::timeout(Duration::from_secs(3), recv)
            .await
            .map_err(|_| "loopback recv timed out".to_string())?
            .map_err(e("join"))?
            .map_err(e("recv"))?;
        let mut msg = format!("sleep(50ms) = {slept:?}, {n} datagrams over loopback");
        if let Some(host) = echo {
            let s = tokio::net::UdpSocket::bind("0.0.0.0:0").await.map_err(e("bind echo"))?;
            s.connect(&host).await.map_err(e("connect echo"))?;
            let t = Instant::now();
            s.send(b"pf-smoke").await.map_err(e("send echo"))?;
            let mut buf = [0u8; 64];
            let n = tokio::time::timeout(Duration::from_secs(2), s.recv(&mut buf))
                .await
                .map_err(|_| format!("no echo from {host}"))?
                .map_err(e("recv echo"))?;
            ensure!(&buf[..n] == b"pf-smoke", "echo payload mismatch");
            msg += &format!(", LAN echo RTT {:?}", t.elapsed());
        }
        Ok(msg)
    })
}

/// The raw `mkdir`/`stat` results std's `create_dir_all` bases its decision on.
fn dir_report(path: &str) -> String {
    let c = std::ffi::CString::new(path).unwrap();
    let mkdir = match unsafe { libc::mkdir(c.as_ptr(), 0o777) } {
        0 => "created".to_string(),
        _ => std::io::Error::last_os_error().to_string(),
    };
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    let stat = match unsafe { libc::stat(c.as_ptr(), &mut st) } {
        0 => format!("st_mode {:o}", st.st_mode),
        _ => std::io::Error::last_os_error().to_string(),
    };
    let meta = std::fs::metadata(path).map(|m| m.is_dir());
    format!("mkdir: {mkdir}; stat: {stat}; metadata().is_dir: {meta:?}")
}

fn filesystem() -> TestResult {
    let dir = "sdmc:/switch/punktfunk";
    std::fs::create_dir_all(dir).map_err(|err| format!("create_dir_all: {err} ({})", dir_report(dir)))?;
    let path = format!("{dir}/smoke.txt");
    std::fs::write(&path, b"punktfunk").map_err(e("write"))?;
    let back = std::fs::read(&path).map_err(e("read"))?;
    std::fs::remove_file(&path).map_err(e("remove"))?;
    ensure!(back == b"punktfunk", "read back {back:?}");
    Ok(path)
}

extern "C" {
    fn consoleUpdate(console: *mut std::ffi::c_void);
}

fn opt_str(p: *const c_char) -> Option<String> {
    (!p.is_null())
        .then(|| unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
        .filter(|s| !s.is_empty())
}

/// Runs every test. `echo_host` (`"ip:port"`) is a UDP echo server on the LAN, e.g.
/// `scripts/udp-echo.py` on the PC; `log_path` receives a copy of the report. Either may be NULL.
///
/// # Safety
/// Both pointers must be NULL or valid NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn pf_smoke_run(echo_host: *const c_char, log_path: *const c_char) -> c_int {
    let echo = opt_str(echo_host);
    let log_file = opt_str(log_path);
    if let Some(p) = log_file.clone() {
        let default = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            if let Ok(mut f) = std::fs::OpenOptions::new().append(true).open(&p) {
                let _ = writeln!(f, "PANIC {info}");
            }
            default(info);
        }));
    }
    let mut log = log_file.and_then(|p| {
        if let Some(dir) = std::path::Path::new(&p).parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        std::fs::File::create(p).ok()
    });
    let mut report = |line: String| {
        println!("{line}");
        if let Some(f) = log.as_mut() {
            let _ = writeln!(f, "{line}");
        }
        unsafe { consoleUpdate(std::ptr::null_mut()) };
    };
    let tests: Vec<(&str, Box<dyn FnOnce() -> TestResult>)> = vec![
        ("threads", Box::new(threads)),
        ("sync", Box::new(sync_primitives)),
        ("time", Box::new(time)),
        ("random", Box::new(random)),
        ("udp", Box::new(udp_loopback)),
        ("tcp", Box::new(tcp_loopback)),
        ("dns", Box::new(dns)),
        ("socket2/if-addrs", Box::new(sockets_crates)),
        ("tokio", Box::new(move || tokio_runtime(echo))),
        ("fs", Box::new(filesystem)),
    ];
    let mut failed = 0;
    for (name, test) in tests {
        let t = Instant::now();
        match test() {
            Ok(detail) => report(format!("PASS {name:<18} {detail} ({:?})", t.elapsed())),
            Err(err) => {
                failed += 1;
                report(format!("FAIL {name:<18} {err}"));
            }
        }
    }
    report(format!("{failed} failed"));
    failed
}
