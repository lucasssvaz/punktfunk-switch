// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

//! Links punktfunk-ffi's `#[no_mangle]` C ABI into `libpf_switch.a`, plus the host
//! management-API client the Switch app needs (`pfsw_mgmt_*`, see `include/pf_switch.h`).
//!
//! The management API is HTTPS with mutual TLS: the client presents the identity it
//! paired with, and the host is trusted by the SHA-256 of its self-signed leaf, not by a
//! CA. Neither is expressible with the Switch's curl + mbedTLS, so the requests go through
//! the same rustls `PinVerify` the QUIC plane uses.
pub use punktfunk_ffi::*;

use std::ffi::{c_char, CStr};
use std::sync::Arc;
use std::time::Duration;

/// 16 MiB, the desktop client's art ceiling; also bounds a library page.
const BODY_MAX_BYTES: u64 = 16 * 1024 * 1024;

pub const PFSW_OK: i32 = 0;
pub const PFSW_ERR_ARG: i32 = -1;
pub const PFSW_ERR_PIN: i32 = -2;
pub const PFSW_ERR_IO: i32 = -3;

pub struct PfswMgmt {
    pinned: ureq::Agent,
    base: String,
}

unsafe fn str_arg<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    CStr::from_ptr(p).to_str().ok()
}

unsafe fn write_err(out: *mut c_char, cap: usize, msg: &str) {
    if out.is_null() || cap == 0 {
        return;
    }
    let n = msg.len().min(cap - 1);
    std::ptr::copy_nonoverlapping(msg.as_ptr(), out as *mut u8, n);
    *out.add(n) = 0;
}

fn base_url(addr: &str, port: u16) -> String {
    if addr.contains(':') {
        format!("https://[{addr}]:{port}")
    } else {
        format!("https://{addr}:{port}")
    }
}

fn agent_config() -> ureq::config::Config {
    ureq::Agent::config_builder()
        .timeout_connect(Some(Duration::from_secs(5)))
        .timeout_global(Some(Duration::from_secs(15)))
        .http_status_as_error(false)
        .build()
}

fn build_agent(pin: Option<[u8; 32]>, cert_pem: &str, key_pem: &str) -> Result<ureq::Agent, String> {
    use rustls::pki_types::pem::PemObject;
    let builder = punktfunk_core::tls::pinned_builder(punktfunk_core::tls::PinVerify::new(pin))
        .map_err(|e| format!("tls config: {e}"))?;
    let cert = rustls::pki_types::CertificateDer::from_pem_slice(cert_pem.as_bytes())
        .map_err(|e| format!("client cert pem: {e}"))?;
    let key = rustls::pki_types::PrivateKeyDer::from_pem_slice(key_pem.as_bytes())
        .map_err(|e| format!("client key pem: {e}"))?;
    let cfg = builder
        .with_client_auth_cert(vec![cert], key)
        .map_err(|e| format!("client auth: {e}"))?;
    Ok(punktfunk_core::tls::ureq_agent::agent(Arc::new(cfg), agent_config()))
}

/// A pin mismatch surfaces as a rustls `InvalidCertificate` somewhere in the error chain.
fn classify(e: &ureq::Error) -> i32 {
    let text = format!("{e:?}");
    if text.contains("InvalidCertificate") || text.contains("ApplicationVerificationFailure") {
        PFSW_ERR_PIN
    } else {
        PFSW_ERR_IO
    }
}

/// Open a management-API client for `addr:port`. `pin_sha256` is null (accept any host
/// certificate) or 32 bytes. Returns null and fills `err` on failure. Free with
/// [`pfsw_mgmt_free`].
///
/// # Safety
/// String arguments are NUL-terminated UTF-8; `pin_sha256` is null or 32 readable bytes;
/// `err` is null or writable for `err_cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn pfsw_mgmt_new(
    addr: *const c_char,
    port: u16,
    pin_sha256: *const u8,
    client_cert_pem: *const c_char,
    client_key_pem: *const c_char,
    err: *mut c_char,
    err_cap: usize,
) -> *mut PfswMgmt {
    let (Some(addr), Some(cert), Some(key)) =
        (str_arg(addr), str_arg(client_cert_pem), str_arg(client_key_pem))
    else {
        write_err(err, err_cap, "invalid argument");
        return std::ptr::null_mut();
    };
    let pin = if pin_sha256.is_null() {
        None
    } else {
        let mut p = [0u8; 32];
        std::ptr::copy_nonoverlapping(pin_sha256, p.as_mut_ptr(), 32);
        Some(p)
    };
    match build_agent(pin, cert, key) {
        Ok(pinned) => Box::into_raw(Box::new(PfswMgmt {
            pinned,
            base: base_url(addr, port),
        })),
        Err(msg) => {
            write_err(err, err_cap, &msg);
            std::ptr::null_mut()
        }
    }
}

/// # Safety
/// `m` is null or was returned by [`pfsw_mgmt_new`] and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn pfsw_mgmt_free(m: *mut PfswMgmt) {
    if !m.is_null() {
        drop(Box::from_raw(m));
    }
}

/// One blocking request. `path_or_url` is a host path (`/api/v1/library`) or an absolute URL;
/// URLs on the host's origin use the pinned mTLS agent, any other origin (cover-art CDNs) the
/// webpki-rooted default agent. `method` is `"GET"` or `"POST"`; `body` (nullable) is sent as
/// JSON. On [`PFSW_OK`] any HTTP status is reported in `*status_out` and the response body is
/// in `*body_out`/`*len_out` (free with [`pfsw_buffer_free`]). Callable from any thread.
///
/// # Safety
/// `m` is a live handle; strings are NUL-terminated UTF-8 (or null where noted); the out
/// pointers are writable; `err` is null or writable for `err_cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn pfsw_mgmt_request(
    m: *const PfswMgmt,
    method: *const c_char,
    path_or_url: *const c_char,
    body: *const c_char,
    status_out: *mut u16,
    body_out: *mut *mut u8,
    len_out: *mut usize,
    err: *mut c_char,
    err_cap: usize,
) -> i32 {
    let (Some(m), Some(method), Some(target)) = (m.as_ref(), str_arg(method), str_arg(path_or_url))
    else {
        write_err(err, err_cap, "invalid argument");
        return PFSW_ERR_ARG;
    };
    if status_out.is_null() || body_out.is_null() || len_out.is_null() {
        write_err(err, err_cap, "invalid argument");
        return PFSW_ERR_ARG;
    }
    *body_out = std::ptr::null_mut();
    *len_out = 0;

    let url = if target.starts_with('/') {
        format!("{}{}", m.base, target)
    } else {
        target.to_string()
    };
    let foreign = !url.starts_with(&m.base);
    let fallback;
    let agent = if foreign {
        punktfunk_core::tls::install_default_provider();
        fallback = ureq::Agent::new_with_config(agent_config());
        &fallback
    } else {
        &m.pinned
    };

    let answer = match (method, str_arg(body)) {
        ("POST", Some(b)) => agent
            .post(&url)
            .header("Content-Type", "application/json")
            .send(b.as_bytes()),
        ("POST", None) => agent.post(&url).send_empty(),
        _ => agent.get(&url).call(),
    };
    let mut resp = match answer {
        Ok(r) => r,
        Err(e) => {
            write_err(err, err_cap, &e.to_string());
            return classify(&e);
        }
    };
    *status_out = resp.status().as_u16();
    match resp.body_mut().with_config().limit(BODY_MAX_BYTES).read_to_vec() {
        Ok(bytes) => {
            let boxed = bytes.into_boxed_slice();
            *len_out = boxed.len();
            *body_out = Box::into_raw(boxed) as *mut u8;
            PFSW_OK
        }
        Err(e) => {
            write_err(err, err_cap, &format!("read body: {e}"));
            PFSW_ERR_IO
        }
    }
}

/// Homebrew NROs get a ~1 MiB main-thread stack from hbloader. aws-lc P-256 keygen plus
/// rcgen exceeds that and aborts with no Rust panic. Same work as
/// [`punktfunk_generate_identity`], on a 2 MiB thread.
///
/// # Safety
/// `cert_pem_out`/`key_pem_out` are writable for `cert_cap`/`key_cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn pfsw_generate_identity(
    cert_pem_out: *mut c_char,
    cert_cap: usize,
    key_pem_out: *mut c_char,
    key_cap: usize,
) -> i32 {
    fn flush() {
        use std::io::Write;
        let _ = std::io::stdout().flush();
    }
    if cert_pem_out.is_null() || key_pem_out.is_null() {
        return -1;
    }
    println!("identity: spawning 2 MiB worker");
    flush();
    let cert_pem_out = cert_pem_out as usize;
    let key_pem_out = key_pem_out as usize;
    match std::thread::Builder::new()
        .name("pf-identity".into())
        .stack_size(2 * 1024 * 1024)
        .spawn(move || {
            println!("identity: worker, calling rcgen");
            flush();
            let st = unsafe {
                punktfunk_generate_identity(
                    cert_pem_out as *mut c_char,
                    cert_cap,
                    key_pem_out as *mut c_char,
                    key_cap,
                )
            };
            println!("identity: rcgen returned {}", st as i32);
            flush();
            st as i32
        }) {
        Ok(h) => match h.join() {
            Ok(st) => st,
            Err(_) => -2,
        },
        Err(e) => {
            println!("identity: spawn failed: {e}");
            flush();
            -1
        }
    }
}

/// Appends the message and location of any Rust panic to `path` before the process aborts
/// (`panic = "abort"`: without this a panic is only a crash report with no reason).
///
/// # Safety
/// `path` is a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn pfsw_install_panic_log(path: *const c_char) {
    let Some(path) = str_arg(path).map(str::to_owned) else {
        return;
    };
    let default = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        use std::io::Write;
        if let Ok(mut f) = std::fs::OpenOptions::new().append(true).create(true).open(&path) {
            let _ = writeln!(f, "PANIC {info}");
        }
        default(info);
    }));
}

/// # Safety
/// `ptr`/`len` came from one [`pfsw_mgmt_request`] and are not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn pfsw_buffer_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)));
    }
}
