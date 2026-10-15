//! AArch64 Nintendo Switch: devkitA64 newlib plus libnx's FreeBSD-derived BSD socket headers.
//!
//! Shares `target_os = "horizon"` with the 3DS but not its ABI: the socket layer is FreeBSD's
//! (`sa_len` bytes, `AF_INET6 = 28`, short `pollfd` events), the pthread types are libnx's
//! (`sys/_pthreadtypes.h` over `sys/lock.h`), and `long` is 64-bit. Every layout and value here is
//! checked against the headers by `punktfunk-rs/abi-check/check.py`.
//!
//! `bind`/`connect`/`sendto`/`sendmsg` link to `pf_nx_*` in horizon_shims.c, which fill in the
//! `sa_len` that Rust leaves at 0 and a FreeBSD kernel would derive from the address length.
//!
//! std is built against this fork too (docker/Dockerfile patches the sysroot's `library/`
//! workspace), but `-Zbuild-std` never notices edits here: scripts/fetch-patched-crates.sh wipes
//! punktfunk-rs/target when this crate changes.

use crate::off_t;
use crate::prelude::*;

pub type wchar_t = c_uint;

pub type u_register_t = c_ulong;
pub type u_char = c_uchar;
pub type u_short = c_ushort;
pub type u_int = c_uint;
pub type u_long = c_ulong;
pub type ushort = c_ushort;
pub type uint = c_uint;
pub type ulong = c_ulong;
pub type clock_t = c_ulong;
pub type daddr_t = c_long;
pub type caddr_t = *mut c_char;
pub type sbintime_t = c_longlong;
pub type sigset_t = c_ulong;

pub type blkcnt_t = c_long;
pub type blksize_t = c_long;
pub type dev_t = c_short;
pub type ino_t = c_ushort;
pub type key_t = c_long;
pub type sa_family_t = u8;
pub type suseconds_t = c_long;
pub type useconds_t = c_ulong;

s! {
    pub struct hostent {
        pub h_name: *mut c_char,
        pub h_aliases: *mut *mut c_char,
        pub h_addrtype: c_int,
        pub h_length: c_int,
        pub h_addr_list: *mut *mut c_char,
    }

    pub struct pollfd {
        pub fd: c_int,
        pub events: c_short,
        pub revents: c_short,
    }

    pub struct sockaddr {
        pub sa_len: u8,
        pub sa_family: sa_family_t,
        pub sa_data: [c_char; 14],
    }

    pub struct sockaddr_storage {
        pub ss_len: u8,
        pub ss_family: sa_family_t,
        __ss_pad1: Padding<[c_char; 6]>,
        __ss_align: i64,
        __ss_pad2: Padding<[c_char; 112]>,
    }

    pub struct sockaddr_in {
        pub sin_len: u8,
        pub sin_family: sa_family_t,
        pub sin_port: crate::in_port_t,
        pub sin_addr: crate::in_addr,
        pub sin_zero: [c_char; 8],
    }

    // libnx declares no `sin6_len`: the family byte comes first and padding follows it.
    pub struct sockaddr_in6 {
        pub sin6_family: sa_family_t,
        pub sin6_port: crate::in_port_t,
        pub sin6_flowinfo: u32,
        pub sin6_addr: crate::in6_addr,
        pub sin6_scope_id: u32,
    }

    // libnx has no AF_UNIX sockets; std still names the type.
    pub struct sockaddr_un {
        pub sun_len: c_uchar,
        pub sun_family: sa_family_t,
        pub sun_path: [c_char; 104usize],
    }

    pub struct msghdr {
        pub msg_name: *mut c_void,
        pub msg_namelen: crate::socklen_t,
        pub msg_iov: *mut crate::iovec,
        pub msg_iovlen: c_int,
        pub msg_control: *mut c_void,
        pub msg_controllen: crate::socklen_t,
        pub msg_flags: c_int,
    }

    pub struct cmsghdr {
        pub cmsg_len: crate::socklen_t,
        pub cmsg_level: c_int,
        pub cmsg_type: c_int,
    }

    // Not in libnx: provided by punktfunk-rs/shims/horizon_shims.{h,c}.
    pub struct ifaddrs {
        pub ifa_next: *mut ifaddrs,
        pub ifa_name: *mut c_char,
        pub ifa_flags: c_uint,
        pub ifa_addr: *mut crate::sockaddr,
        pub ifa_netmask: *mut crate::sockaddr,
        pub ifa_dstaddr: *mut crate::sockaddr,
        pub ifa_data: *mut c_void,
    }

    pub struct sched_param {
        pub sched_priority: c_int,
    }

    pub struct stat {
        pub st_dev: dev_t,
        pub st_ino: ino_t,
        pub st_mode: crate::mode_t,
        pub st_nlink: crate::nlink_t,
        pub st_uid: crate::uid_t,
        pub st_gid: crate::gid_t,
        pub st_rdev: dev_t,
        pub st_size: off_t,
        pub st_atim: crate::timespec,
        pub st_mtim: crate::timespec,
        pub st_ctim: crate::timespec,
        pub st_blksize: blksize_t,
        pub st_blocks: blkcnt_t,
        pub st_spare4: [c_long; 2usize],
    }

    pub struct rusage {
        pub ru_utime: crate::timeval,
        pub ru_stime: crate::timeval,
    }

    pub struct fd_set {
        pub fds_bits: [c_ulong; crate::FD_SETSIZE / 64],
    }

    #[repr(align(8))]
    pub struct pthread_attr_t {
        __size: [u8; 24],
    }

    pub struct pthread_mutex_t {
        __type: c_int,
        __lock: [u32; 2],
    }

    pub struct pthread_mutexattr_t {
        __type: c_int,
    }

    pub struct pthread_cond_t {
        __clock_id: crate::clockid_t,
        __cond: u32,
    }

    pub struct pthread_condattr_t {
        __clock_id: crate::clockid_t,
    }

    pub struct pthread_rwlock_t {
        __lock: u32,
        __cond_r: u32,
        __cond_w: u32,
        __cnt: u32,
    }

    pub struct pthread_rwlockattr_t {
        __size: [u8; 0],
    }
}

pub const PTHREAD_MUTEX_INITIALIZER: pthread_mutex_t = pthread_mutex_t {
    __type: crate::PTHREAD_MUTEX_NORMAL,
    __lock: [0; 2],
};
pub const PTHREAD_COND_INITIALIZER: pthread_cond_t = pthread_cond_t {
    __clock_id: crate::CLOCK_REALTIME,
    __cond: 0,
};
pub const PTHREAD_RWLOCK_INITIALIZER: pthread_rwlock_t = pthread_rwlock_t {
    __lock: 0,
    __cond_r: 0,
    __cond_w: 0,
    __cnt: 0,
};

pub const __SIZEOF_PTHREAD_ATTR_T: usize = 24;
pub const __SIZEOF_PTHREAD_MUTEX_T: usize = 12;
pub const __SIZEOF_PTHREAD_MUTEXATTR_T: usize = 4;
pub const __SIZEOF_PTHREAD_COND_T: usize = 16;
pub const __SIZEOF_PTHREAD_CONDATTR_T: usize = 8;
pub const __SIZEOF_PTHREAD_RWLOCK_T: usize = 16;
pub const __SIZEOF_PTHREAD_RWLOCKATTR_T: usize = 0;
pub const __SIZEOF_PTHREAD_BARRIER_T: usize = 20;

pub const SIGEV_NONE: c_int = 1;
pub const SIGEV_SIGNAL: c_int = 2;
pub const SIGEV_THREAD: c_int = 3;
pub const SA_NOCLDSTOP: c_int = 1;
pub const MINSIGSTKSZ: c_int = 2048;
pub const SIGSTKSZ: c_int = 8192;
pub const SS_ONSTACK: c_int = 1;
pub const SS_DISABLE: c_int = 2;
pub const SIG_SETMASK: c_int = 0;
pub const SIG_BLOCK: c_int = 1;
pub const SIG_UNBLOCK: c_int = 2;
pub const SIGHUP: c_int = 1;
pub const SIGINT: c_int = 2;
pub const SIGQUIT: c_int = 3;
pub const SIGILL: c_int = 4;
pub const SIGTRAP: c_int = 5;
pub const SIGABRT: c_int = 6;
pub const SIGEMT: c_int = 7;
pub const SIGFPE: c_int = 8;
pub const SIGKILL: c_int = 9;
pub const SIGBUS: c_int = 10;
pub const SIGSEGV: c_int = 11;
pub const SIGSYS: c_int = 12;
pub const SIGPIPE: c_int = 13;
pub const SIGALRM: c_int = 14;
pub const SIGTERM: c_int = 15;
pub const SIGURG: c_int = 16;
pub const SIGSTOP: c_int = 17;
pub const SIGTSTP: c_int = 18;
pub const SIGCONT: c_int = 19;
pub const SIGCHLD: c_int = 20;
pub const SIGCLD: c_int = 20;
pub const SIGTTIN: c_int = 21;
pub const SIGTTOU: c_int = 22;
pub const SIGIO: c_int = 23;
pub const SIGPOLL: c_int = 23;
pub const SIGXCPU: c_int = 24;
pub const SIGXFSZ: c_int = 25;
pub const SIGVTALRM: c_int = 26;
pub const SIGPROF: c_int = 27;
pub const SIGWINCH: c_int = 28;
pub const SIGLOST: c_int = 29;
pub const SIGUSR1: c_int = 30;
pub const SIGUSR2: c_int = 31;
pub const NSIG: c_int = 32;
pub const CLOCK_ENABLED: c_uint = 1;
pub const CLOCK_DISABLED: c_uint = 0;
pub const CLOCK_ALLOWED: c_uint = 1;
pub const CLOCK_DISALLOWED: c_uint = 0;
pub const TIMER_ABSTIME: c_uint = 4;

pub const SOL_SOCKET: c_int = 0xffff;
pub const MSG_OOB: c_int = 0x1;
pub const MSG_PEEK: c_int = 0x2;
pub const MSG_DONTROUTE: c_int = 0x4;
pub const MSG_EOR: c_int = 0x8;
pub const MSG_TRUNC: c_int = 0x10;
pub const MSG_CTRUNC: c_int = 0x20;
pub const MSG_WAITALL: c_int = 0x40;
pub const MSG_DONTWAIT: c_int = 0x80;
pub const MSG_EOF: c_int = 0x100;
pub const MSG_NOSIGNAL: c_int = 0x20000;
pub const MSG_CMSG_CLOEXEC: c_int = 0x40000;
pub const MSG_WAITFORONE: c_int = 0x80000;
pub const MSG_MORE: c_int = 0;

pub const SOCK_RAW: c_int = 3;
pub const SOCK_SEQPACKET: c_int = 5;
pub const SOCK_NONBLOCK: c_int = 0x20000000;
pub const SOMAXCONN: c_int = 128;
pub const SCM_RIGHTS: c_int = 0x01;
pub const SCM_TIMESTAMP: c_int = 0x02;

pub const PTHREAD_STACK_MIN: size_t = 4096;
pub const WNOHANG: c_int = 1;

pub const POLLIN: c_short = 0x0001;
pub const POLLPRI: c_short = 0x0002;
pub const POLLOUT: c_short = 0x0004;
pub const POLLRDNORM: c_short = 0x0040;
pub const POLLWRNORM: c_short = POLLOUT;
pub const POLLRDBAND: c_short = 0x0080;
pub const POLLWRBAND: c_short = 0x0100;
pub const POLLERR: c_short = 0x0008;
pub const POLLHUP: c_short = 0x0010;
pub const POLLNVAL: c_short = 0x0020;

pub const EAI_AGAIN: c_int = 2;
pub const EAI_BADFLAGS: c_int = 3;
pub const EAI_FAIL: c_int = 4;
pub const EAI_SERVICE: c_int = 9;
pub const EAI_SYSTEM: c_int = 11;
pub const EAI_BADHINTS: c_int = 12;
pub const EAI_PROTOCOL: c_int = 13;
pub const EAI_OVERFLOW: c_int = 14;
pub const EAI_MAX: c_int = 15;

pub const AF_UNIX: c_int = 1;
pub const AF_INET6: c_int = 28;

pub const FIONBIO: c_ulong = 0x8004667e;

pub const RTLD_DEFAULT: *mut c_void = ptr::null_mut();

pub const SCHED_FIFO: c_int = 1;
pub const SCHED_RR: c_int = 2;

pub const GRND_NONBLOCK: c_uint = 0x1;
pub const GRND_RANDOM: c_uint = 0x2;

const fn _ALIGN(p: usize) -> usize {
    (p + size_of::<c_long>() - 1) & !(size_of::<c_long>() - 1)
}

f! {
    pub unsafe fn CMSG_FIRSTHDR(mhdr: *const msghdr) -> *mut cmsghdr {
        if (*mhdr).msg_controllen as usize >= size_of::<cmsghdr>() {
            (*mhdr).msg_control.cast::<cmsghdr>()
        } else {
            core::ptr::null_mut()
        }
    }

    pub unsafe fn CMSG_NXTHDR(mhdr: *const msghdr, cmsg: *const cmsghdr) -> *mut cmsghdr {
        let next = cmsg as usize + _ALIGN((*cmsg).cmsg_len as usize);
        let max = (*mhdr).msg_control as usize + (*mhdr).msg_controllen as usize;
        if next + _ALIGN(size_of::<cmsghdr>()) > max {
            core::ptr::null_mut()
        } else {
            next as *mut cmsghdr
        }
    }

    pub unsafe fn CMSG_DATA(cmsg: *const cmsghdr) -> *mut c_uchar {
        (cmsg as *mut c_uchar).add(_ALIGN(size_of::<cmsghdr>()))
    }

    pub const unsafe fn CMSG_SPACE(length: c_uint) -> c_uint {
        (_ALIGN(size_of::<cmsghdr>()) + _ALIGN(length as usize)) as c_uint
    }

    pub const unsafe fn CMSG_LEN(length: c_uint) -> c_uint {
        (_ALIGN(size_of::<cmsghdr>()) + length as usize) as c_uint
    }
}

safe_f! {
    pub const safe fn WIFSTOPPED(_status: c_int) -> bool {
        false
    }

    pub const safe fn WSTOPSIG(_status: c_int) -> c_int {
        0
    }

    pub const safe fn WIFCONTINUED(_status: c_int) -> bool {
        true
    }

    pub const safe fn WIFSIGNALED(_status: c_int) -> bool {
        false
    }

    pub const safe fn WTERMSIG(_status: c_int) -> c_int {
        0
    }

    pub const safe fn WIFEXITED(_status: c_int) -> bool {
        true
    }

    pub const safe fn WEXITSTATUS(_status: c_int) -> c_int {
        0
    }

    pub const safe fn WCOREDUMP(_status: c_int) -> bool {
        false
    }
}

extern "C" {
    pub fn pthread_create(
        native: *mut crate::pthread_t,
        attr: *const crate::pthread_attr_t,
        f: extern "C" fn(_: *mut c_void) -> *mut c_void,
        value: *mut c_void,
    ) -> c_int;

    pub fn pthread_attr_getschedparam(
        attr: *const crate::pthread_attr_t,
        param: *mut sched_param,
    ) -> c_int;

    pub fn pthread_attr_setschedparam(
        attr: *mut crate::pthread_attr_t,
        param: *const sched_param,
    ) -> c_int;

    pub fn pthread_getschedparam(
        native: crate::pthread_t,
        policy: *mut c_int,
        param: *mut crate::sched_param,
    ) -> c_int;

    pub fn pthread_setschedparam(
        native: crate::pthread_t,
        policy: c_int,
        param: *const crate::sched_param,
    ) -> c_int;

    pub fn pthread_condattr_getclock(
        attr: *const crate::pthread_condattr_t,
        clock_id: *mut crate::clockid_t,
    ) -> c_int;

    pub fn pthread_condattr_setclock(
        attr: *mut crate::pthread_condattr_t,
        clock_id: crate::clockid_t,
    ) -> c_int;

    pub fn getrandom(buf: *mut c_void, buflen: size_t, flags: c_uint) -> ssize_t;

    pub fn gethostid() -> c_long;

    #[link_name = "pf_nx_sendmsg"]
    pub fn sendmsg(fd: c_int, msg: *const crate::msghdr, flags: c_int) -> ssize_t;
    pub fn recvmsg(fd: c_int, msg: *mut crate::msghdr, flags: c_int) -> ssize_t;

    // horizon_shims.c
    pub fn getifaddrs(ifap: *mut *mut ifaddrs) -> c_int;
    pub fn freeifaddrs(ifa: *mut ifaddrs);
}

pub use crate::unix::newlib::generic::dirent;
