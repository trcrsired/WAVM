// WASI syscall API definitions
// Derived from wasi-sysroot:
// https://github.com/CraneStation/wasi-sysroot/blob/320054e84f8f2440def3b1c8700cedb8fd697bf8/libc-bottom-half/headers/public/wasi/core.h
// The wasi-sysroot code is licensed under the terms of the CC0 1.0 Universal license:
// https://github.com/CraneStation/wasi-sysroot/blob/320054e84f8f2440def3b1c8700cedb8fd697bf8/libc-bottom-half/headers/LICENSE
// That license has been mirrored in WASITypes.LICENSE for posterity.
// Thank you to the wasi-sysroot developers for their contributions to the public domain.

#pragma once

#include <cstddef>
#include <cstdint>

typedef int32_t __wasi_intptr_t;
typedef uint32_t __wasi_uintptr_t;
typedef uint32_t __wasi_size_t;
typedef uint32_t __wasi_void_ptr_t;

static_assert(alignof(int8_t) == 1, "non-wasi data layout");
static_assert(alignof(uint8_t) == 1, "non-wasi data layout");
static_assert(alignof(int16_t) == 2, "non-wasi data layout");
static_assert(alignof(uint16_t) == 2, "non-wasi data layout");
static_assert(alignof(int32_t) == 4, "non-wasi data layout");
static_assert(alignof(uint32_t) == 4, "non-wasi data layout");
static_assert(alignof(int64_t) == 8, "non-wasi data layout");
static_assert(alignof(uint64_t) == 8, "non-wasi data layout");

typedef uint8_t __wasi_advice_t;
#define __WASI_ADVICE_NORMAL (UINT8_C(0))
#define __WASI_ADVICE_SEQUENTIAL (UINT8_C(1))
#define __WASI_ADVICE_RANDOM (UINT8_C(2))
#define __WASI_ADVICE_WILLNEED (UINT8_C(3))
#define __WASI_ADVICE_DONTNEED (UINT8_C(4))
#define __WASI_ADVICE_NOREUSE (UINT8_C(5))

typedef uint32_t __wasi_clockid_t;
#define __WASI_CLOCK_REALTIME (UINT32_C(0))
#define __WASI_CLOCK_MONOTONIC (UINT32_C(1))
#define __WASI_CLOCK_PROCESS_CPUTIME_ID (UINT32_C(2))
#define __WASI_CLOCK_THREAD_CPUTIME_ID (UINT32_C(3))

typedef uint64_t __wasi_device_t;

typedef uint64_t __wasi_dircookie_t;
#define __WASI_DIRCOOKIE_START (UINT64_C(0))

typedef uint32_t __wasi_dirnamlen_t;

typedef uint16_t __wasi_errno_t;
#define __WASI_ESUCCESS (UINT16_C(0))
#define __WASI_E2BIG (UINT16_C(1))
#define __WASI_EACCES (UINT16_C(2))
#define __WASI_EADDRINUSE (UINT16_C(3))
#define __WASI_EADDRNOTAVAIL (UINT16_C(4))
#define __WASI_EAFNOSUPPORT (UINT16_C(5))
#define __WASI_EAGAIN (UINT16_C(6))
#define __WASI_EALREADY (UINT16_C(7))
#define __WASI_EBADF (UINT16_C(8))
#define __WASI_EBADMSG (UINT16_C(9))
#define __WASI_EBUSY (UINT16_C(10))
#define __WASI_ECANCELED (UINT16_C(11))
#define __WASI_ECHILD (UINT16_C(12))
#define __WASI_ECONNABORTED (UINT16_C(13))
#define __WASI_ECONNREFUSED (UINT16_C(14))
#define __WASI_ECONNRESET (UINT16_C(15))
#define __WASI_EDEADLK (UINT16_C(16))
#define __WASI_EDESTADDRREQ (UINT16_C(17))
#define __WASI_EDOM (UINT16_C(18))
#define __WASI_EDQUOT (UINT16_C(19))
#define __WASI_EEXIST (UINT16_C(20))
#define __WASI_EFAULT (UINT16_C(21))
#define __WASI_EFBIG (UINT16_C(22))
#define __WASI_EHOSTUNREACH (UINT16_C(23))
#define __WASI_EIDRM (UINT16_C(24))
#define __WASI_EILSEQ (UINT16_C(25))
#define __WASI_EINPROGRESS (UINT16_C(26))
#define __WASI_EINTR (UINT16_C(27))
#define __WASI_EINVAL (UINT16_C(28))
#define __WASI_EIO (UINT16_C(29))
#define __WASI_EISCONN (UINT16_C(30))
#define __WASI_EISDIR (UINT16_C(31))
#define __WASI_ELOOP (UINT16_C(32))
#define __WASI_EMFILE (UINT16_C(33))
#define __WASI_EMLINK (UINT16_C(34))
#define __WASI_EMSGSIZE (UINT16_C(35))
#define __WASI_EMULTIHOP (UINT16_C(36))
#define __WASI_ENAMETOOLONG (UINT16_C(37))
#define __WASI_ENETDOWN (UINT16_C(38))
#define __WASI_ENETRESET (UINT16_C(39))
#define __WASI_ENETUNREACH (UINT16_C(40))
#define __WASI_ENFILE (UINT16_C(41))
#define __WASI_ENOBUFS (UINT16_C(42))
#define __WASI_ENODEV (UINT16_C(43))
#define __WASI_ENOENT (UINT16_C(44))
#define __WASI_ENOEXEC (UINT16_C(45))
#define __WASI_ENOLCK (UINT16_C(46))
#define __WASI_ENOLINK (UINT16_C(47))
#define __WASI_ENOMEM (UINT16_C(48))
#define __WASI_ENOMSG (UINT16_C(49))
#define __WASI_ENOPROTOOPT (UINT16_C(50))
#define __WASI_ENOSPC (UINT16_C(51))
#define __WASI_ENOSYS (UINT16_C(52))
#define __WASI_ENOTCONN (UINT16_C(53))
#define __WASI_ENOTDIR (UINT16_C(54))
#define __WASI_ENOTEMPTY (UINT16_C(55))
#define __WASI_ENOTRECOVERABLE (UINT16_C(56))
#define __WASI_ENOTSOCK (UINT16_C(57))
#define __WASI_ENOTSUP (UINT16_C(58))
#define __WASI_ENOTTY (UINT16_C(59))
#define __WASI_ENXIO (UINT16_C(60))
#define __WASI_EOVERFLOW (UINT16_C(61))
#define __WASI_EOWNERDEAD (UINT16_C(62))
#define __WASI_EPERM (UINT16_C(63))
#define __WASI_EPIPE (UINT16_C(64))
#define __WASI_EPROTO (UINT16_C(65))
#define __WASI_EPROTONOSUPPORT (UINT16_C(66))
#define __WASI_EPROTOTYPE (UINT16_C(67))
#define __WASI_ERANGE (UINT16_C(68))
#define __WASI_EROFS (UINT16_C(69))
#define __WASI_ESPIPE (UINT16_C(70))
#define __WASI_ESRCH (UINT16_C(71))
#define __WASI_ESTALE (UINT16_C(72))
#define __WASI_ETIMEDOUT (UINT16_C(73))
#define __WASI_ETXTBSY (UINT16_C(74))
#define __WASI_EXDEV (UINT16_C(75))
#define __WASI_ENOTCAPABLE (UINT16_C(76))

typedef uint16_t __wasi_eventrwflags_t;
#define __WASI_EVENT_FD_READWRITE_HANGUP (UINT16_C(0x0001))

typedef uint8_t __wasi_eventtype_t;
#define __WASI_EVENTTYPE_CLOCK (UINT8_C(0))
#define __WASI_EVENTTYPE_FD_READ (UINT8_C(1))
#define __WASI_EVENTTYPE_FD_WRITE (UINT8_C(2))

typedef uint32_t __wasi_exitcode_t;

typedef uint32_t __wasi_fd_t;

typedef uint16_t __wasi_fdflags_t;
#define __WASI_FDFLAG_APPEND (UINT16_C(0x0001))
#define __WASI_FDFLAG_DSYNC (UINT16_C(0x0002))
#define __WASI_FDFLAG_NONBLOCK (UINT16_C(0x0004))
#define __WASI_FDFLAG_RSYNC (UINT16_C(0x0008))
#define __WASI_FDFLAG_SYNC (UINT16_C(0x0010))

typedef int64_t __wasi_filedelta_t;

typedef uint64_t __wasi_filesize_t;

typedef uint8_t __wasi_filetype_t;
#define __WASI_FILETYPE_UNKNOWN (UINT8_C(0))
#define __WASI_FILETYPE_BLOCK_DEVICE (UINT8_C(1))
#define __WASI_FILETYPE_CHARACTER_DEVICE (UINT8_C(2))
#define __WASI_FILETYPE_DIRECTORY (UINT8_C(3))
#define __WASI_FILETYPE_REGULAR_FILE (UINT8_C(4))
#define __WASI_FILETYPE_SOCKET_DGRAM (UINT8_C(5))
#define __WASI_FILETYPE_SOCKET_STREAM (UINT8_C(6))
#define __WASI_FILETYPE_SYMBOLIC_LINK (UINT8_C(7))

typedef uint16_t __wasi_fstflags_t;
#define __WASI_FILESTAT_SET_ATIM (UINT16_C(0x0001))
#define __WASI_FILESTAT_SET_ATIM_NOW (UINT16_C(0x0002))
#define __WASI_FILESTAT_SET_MTIM (UINT16_C(0x0004))
#define __WASI_FILESTAT_SET_MTIM_NOW (UINT16_C(0x0008))

typedef uint64_t __wasi_inode_t;

typedef uint64_t __wasi_linkcount_t;

typedef uint32_t __wasi_lookupflags_t;
#define __WASI_LOOKUP_SYMLINK_FOLLOW (UINT32_C(0x00000001))

typedef uint16_t __wasi_oflags_t;
#define __WASI_O_CREAT (UINT16_C(0x0001))
#define __WASI_O_DIRECTORY (UINT16_C(0x0002))
#define __WASI_O_EXCL (UINT16_C(0x0004))
#define __WASI_O_TRUNC (UINT16_C(0x0008))

typedef uint16_t __wasi_riflags_t;
#define __WASI_SOCK_RECV_PEEK (UINT16_C(0x0001))
#define __WASI_SOCK_RECV_WAITALL (UINT16_C(0x0002))

typedef uint64_t __wasi_rights_t;
#define __WASI_RIGHT_FD_DATASYNC (UINT64_C(0x0000000000000001))
#define __WASI_RIGHT_FD_READ (UINT64_C(0x0000000000000002))
#define __WASI_RIGHT_FD_SEEK (UINT64_C(0x0000000000000004))
#define __WASI_RIGHT_FD_FDSTAT_SET_FLAGS (UINT64_C(0x0000000000000008))
#define __WASI_RIGHT_FD_SYNC (UINT64_C(0x0000000000000010))
#define __WASI_RIGHT_FD_TELL (UINT64_C(0x0000000000000020))
#define __WASI_RIGHT_FD_WRITE (UINT64_C(0x0000000000000040))
#define __WASI_RIGHT_FD_ADVISE (UINT64_C(0x0000000000000080))
#define __WASI_RIGHT_FD_ALLOCATE (UINT64_C(0x0000000000000100))
#define __WASI_RIGHT_PATH_CREATE_DIRECTORY (UINT64_C(0x0000000000000200))
#define __WASI_RIGHT_PATH_CREATE_FILE (UINT64_C(0x0000000000000400))
#define __WASI_RIGHT_PATH_LINK_SOURCE (UINT64_C(0x0000000000000800))
#define __WASI_RIGHT_PATH_LINK_TARGET (UINT64_C(0x0000000000001000))
#define __WASI_RIGHT_PATH_OPEN (UINT64_C(0x0000000000002000))
#define __WASI_RIGHT_FD_READDIR (UINT64_C(0x0000000000004000))
#define __WASI_RIGHT_PATH_READLINK (UINT64_C(0x0000000000008000))
#define __WASI_RIGHT_PATH_RENAME_SOURCE (UINT64_C(0x0000000000010000))
#define __WASI_RIGHT_PATH_RENAME_TARGET (UINT64_C(0x0000000000020000))
#define __WASI_RIGHT_PATH_FILESTAT_GET (UINT64_C(0x0000000000040000))
#define __WASI_RIGHT_PATH_FILESTAT_SET_SIZE (UINT64_C(0x0000000000080000))
#define __WASI_RIGHT_PATH_FILESTAT_SET_TIMES (UINT64_C(0x0000000000100000))
#define __WASI_RIGHT_FD_FILESTAT_GET (UINT64_C(0x0000000000200000))
#define __WASI_RIGHT_FD_FILESTAT_SET_SIZE (UINT64_C(0x0000000000400000))
#define __WASI_RIGHT_FD_FILESTAT_SET_TIMES (UINT64_C(0x0000000000800000))
#define __WASI_RIGHT_PATH_SYMLINK (UINT64_C(0x0000000001000000))
#define __WASI_RIGHT_PATH_REMOVE_DIRECTORY (UINT64_C(0x0000000002000000))
#define __WASI_RIGHT_PATH_UNLINK_FILE (UINT64_C(0x0000000004000000))
#define __WASI_RIGHT_POLL_FD_READWRITE (UINT64_C(0x0000000008000000))
#define __WASI_RIGHT_SOCK_SHUTDOWN (UINT64_C(0x0000000010000000))
#define __WASI_RIGHT_SOCK_ACCEPT (UINT64_C(0x0000000020000000))

typedef uint16_t __wasi_roflags_t;
#define __WASI_SOCK_RECV_DATA_TRUNCATED (UINT16_C(0x0001))

typedef uint8_t __wasi_sdflags_t;
#define __WASI_SHUT_RD (UINT8_C(0x01))
#define __WASI_SHUT_WR (UINT8_C(0x02))

// WAVM's non-standard WASI preview1 socket extension, following the WASIX ABI.
//
// In addition to the standard sock_accept/sock_recv/sock_send/sock_shutdown syscalls, WAVM
// implements the WASIX socket syscalls (as in wasix-libc's <wasi/api_wasix.h>) when network
// access has been granted to the process:
//
//   sock_status(fd, statusOut:ptr) -> errno
//   sock_addr_local(fd, addrOut:ptr) -> errno
//   sock_addr_peer(fd, addrOut:ptr) -> errno
//   sock_open(af:u8, socktype:u8, proto:u16, fdOut:ptr) -> errno
//   sock_pair(af:u8, socktype:u8, proto:u16, fd0Out:ptr, fd1Out:ptr) -> errno
//   sock_bind(fd, addr:ptr) -> errno
//   sock_listen(fd, backlog:u32) -> errno
//   sock_accept_v2(fd, flags:u16, fdOut:ptr, addrOut:ptr) -> errno
//   sock_connect(fd, addr:ptr) -> errno
//   sock_recv_from(fd, riData:ptr, riDataLen:u32, riFlags:u16,
//                  roDatalen:ptr, roFlags:ptr, addrOut:ptr) -> errno
//   sock_send_to(fd, siData:ptr, siDataLen:u32, siFlags:u16, addr:ptr, soDatalen:ptr) -> errno
//   sock_send_file(outFd, inFd, offset:u64, count:u64, sentOut:ptr) -> errno
//   sock_set_opt_flag(fd, opt:u8, flag:u8) -> errno
//   sock_get_opt_flag(fd, opt:u8, flagOut:ptr) -> errno
//   sock_set_opt_time(fd, opt:u8, timeout:ptr) -> errno
//   sock_get_opt_time(fd, opt:u8, timeoutOut:ptr) -> errno
//   sock_set_opt_size(fd, opt:u8, size:u64) -> errno
//   sock_get_opt_size(fd, opt:u8, sizeOut:ptr) -> errno
//   sock_join_multicast_v4(fd, multiaddr:ptr, iface:ptr) -> errno
//   sock_leave_multicast_v4(fd, multiaddr:ptr, iface:ptr) -> errno
//   sock_join_multicast_v6(fd, multiaddr:ptr, iface:u32) -> errno
//   sock_leave_multicast_v6(fd, multiaddr:ptr, iface:u32) -> errno
//   resolve(host:cstr, port:u16, addrs:ptr, naddrs:u32, naddrsOut:ptr) -> errno
//
// addr is a WASIX __wasi_addr_port_t: a 110-byte tagged union {u8 tag; union {...} u} with
// the union at offset 2. tag selects the variant: UNSPEC={u16 port, u8 n0},
// INET4={u16 port, u8 ip[4]}, INET6={u16 port, u8 ip[16], u16 flowHi, u16 flowLo, u16
// scopeHi, u16 scopeLo}, UNIX={u8 path[108]}. Ports are in host byte order (little-endian
// u16 in wasm memory), IP bytes in network order.
// resolve's addrs are __wasi_addr_ip_t: 18-byte records {u8 tag; union{u8 unspec;
// u8 ip4[4]; u8 ip6[16]} u} with the union at offset 2.

typedef uint8_t __wasi_address_family_t;
#define __WASI_ADDRESS_FAMILY_UNSPEC (UINT8_C(0))
#define __WASI_ADDRESS_FAMILY_INET4 (UINT8_C(1))
#define __WASI_ADDRESS_FAMILY_INET6 (UINT8_C(2))
#define __WASI_ADDRESS_FAMILY_UNIX (UINT8_C(3))

typedef uint8_t __wasi_sock_type_t;
#define __WASI_SOCK_TYPE_SOCKET_UNUSED (UINT8_C(0))
#define __WASI_SOCK_TYPE_SOCKET_STREAM (UINT8_C(1))
#define __WASI_SOCK_TYPE_SOCKET_DGRAM (UINT8_C(2))
#define __WASI_SOCK_TYPE_SOCKET_RAW (UINT8_C(3))
#define __WASI_SOCK_TYPE_SOCKET_SEQPACKET (UINT8_C(4))

typedef uint16_t __wasi_sock_proto_t;
#define __WASI_SOCK_PROTO_IP (UINT16_C(0))
#define __WASI_SOCK_PROTO_TCP (UINT16_C(6))
#define __WASI_SOCK_PROTO_UDP (UINT16_C(17))
#define __WASI_SOCK_PROTO_IPV6 (UINT16_C(41))
#define __WASI_SOCK_PROTO_RAW (UINT16_C(255))

typedef uint8_t __wasi_sock_status_t;
#define __WASI_SOCK_STATUS_OPENING (UINT8_C(0))
#define __WASI_SOCK_STATUS_OPENED (UINT8_C(1))
#define __WASI_SOCK_STATUS_CLOSED (UINT8_C(2))
#define __WASI_SOCK_STATUS_FAILED (UINT8_C(3))

typedef uint8_t __wasi_sock_option_t;
#define __WASI_SOCK_OPTION_NOOP (UINT8_C(0))
#define __WASI_SOCK_OPTION_REUSE_PORT (UINT8_C(1))
#define __WASI_SOCK_OPTION_REUSE_ADDR (UINT8_C(2))
#define __WASI_SOCK_OPTION_NO_DELAY (UINT8_C(3))
#define __WASI_SOCK_OPTION_DONT_ROUTE (UINT8_C(4))
#define __WASI_SOCK_OPTION_ONLY_V6 (UINT8_C(5))
#define __WASI_SOCK_OPTION_BROADCAST (UINT8_C(6))
#define __WASI_SOCK_OPTION_MULTICAST_LOOP_V4 (UINT8_C(7))
#define __WASI_SOCK_OPTION_MULTICAST_LOOP_V6 (UINT8_C(8))
#define __WASI_SOCK_OPTION_PROMISCUOUS (UINT8_C(9))
#define __WASI_SOCK_OPTION_LISTENING (UINT8_C(10))
#define __WASI_SOCK_OPTION_LAST_ERROR (UINT8_C(11))
#define __WASI_SOCK_OPTION_KEEP_ALIVE (UINT8_C(12))
#define __WASI_SOCK_OPTION_LINGER (UINT8_C(13))
#define __WASI_SOCK_OPTION_OOB_INLINE (UINT8_C(14))
#define __WASI_SOCK_OPTION_RECV_BUF_SIZE (UINT8_C(15))
#define __WASI_SOCK_OPTION_SEND_BUF_SIZE (UINT8_C(16))
#define __WASI_SOCK_OPTION_RECV_LOWAT (UINT8_C(17))
#define __WASI_SOCK_OPTION_SEND_LOWAT (UINT8_C(18))
#define __WASI_SOCK_OPTION_RECV_TIMEOUT (UINT8_C(19))
#define __WASI_SOCK_OPTION_SEND_TIMEOUT (UINT8_C(20))
#define __WASI_SOCK_OPTION_CONNECT_TIMEOUT (UINT8_C(21))
#define __WASI_SOCK_OPTION_ACCEPT_TIMEOUT (UINT8_C(22))
#define __WASI_SOCK_OPTION_TTL (UINT8_C(23))
#define __WASI_SOCK_OPTION_MULTICAST_TTL_V4 (UINT8_C(24))
#define __WASI_SOCK_OPTION_TYPE (UINT8_C(25))
#define __WASI_SOCK_OPTION_PROTO (UINT8_C(26))

// __wasi_option_timestamp_t: {u8 tag; u8 pad[7]; u64 some} — used by sock_*_opt_time.
#define __WASI_OPTION_NONE (UINT8_C(0))
#define __WASI_OPTION_SOME (UINT8_C(1))

// WASIX extends the preview1 riflags/siflags with nonblocking flags.
#define __WASI_RIFLAGS_RECV_DONT_WAIT ((__wasi_riflags_t)UINT16_C(0x0008))
#define __WASI_SIFLAGS_SEND_DONT_WAIT ((__wasi_siflags_t)UINT16_C(0x0001))

typedef uint16_t __wasi_siflags_t;

typedef uint8_t __wasi_signal_t;
/* UINT8_C(0) is reserved; POSIX has special semantics for kill(pid, 0). */
#define __WASI_SIGHUP (UINT8_C(1))
#define __WASI_SIGINT (UINT8_C(2))
#define __WASI_SIGQUIT (UINT8_C(3))
#define __WASI_SIGILL (UINT8_C(4))
#define __WASI_SIGTRAP (UINT8_C(5))
#define __WASI_SIGABRT (UINT8_C(6))
#define __WASI_SIGBUS (UINT8_C(7))
#define __WASI_SIGFPE (UINT8_C(8))
#define __WASI_SIGKILL (UINT8_C(9))
#define __WASI_SIGUSR1 (UINT8_C(10))
#define __WASI_SIGSEGV (UINT8_C(11))
#define __WASI_SIGUSR2 (UINT8_C(12))
#define __WASI_SIGPIPE (UINT8_C(13))
#define __WASI_SIGALRM (UINT8_C(14))
#define __WASI_SIGTERM (UINT8_C(15))
#define __WASI_SIGCHLD (UINT8_C(16))
#define __WASI_SIGCONT (UINT8_C(17))
#define __WASI_SIGSTOP (UINT8_C(18))
#define __WASI_SIGTSTP (UINT8_C(19))
#define __WASI_SIGTTIN (UINT8_C(20))
#define __WASI_SIGTTOU (UINT8_C(21))
#define __WASI_SIGURG (UINT8_C(22))
#define __WASI_SIGXCPU (UINT8_C(23))
#define __WASI_SIGXFSZ (UINT8_C(24))
#define __WASI_SIGVTALRM (UINT8_C(25))
#define __WASI_SIGPROF (UINT8_C(26))
#define __WASI_SIGWINCH (UINT8_C(27))
#define __WASI_SIGPOLL (UINT8_C(28))
#define __WASI_SIGPWR (UINT8_C(29))
#define __WASI_SIGSYS (UINT8_C(30))

typedef uint16_t __wasi_subclockflags_t;
#define __WASI_SUBSCRIPTION_CLOCK_ABSTIME (UINT16_C(0x0001))

typedef uint64_t __wasi_timestamp_t;

typedef uint64_t __wasi_userdata_t;

typedef uint8_t __wasi_whence_t;
#define __WASI_WHENCE_SET (UINT8_C(0))
#define __WASI_WHENCE_CUR (UINT8_C(1))
#define __WASI_WHENCE_END (UINT8_C(2))

typedef uint8_t __wasi_preopentype_t;
#define __WASI_PREOPENTYPE_DIR (UINT8_C(0))

typedef struct __wasi_dirent_t
{
	__wasi_dircookie_t d_next;
	__wasi_inode_t d_ino;
	__wasi_dirnamlen_t d_namlen;
	__wasi_filetype_t d_type;
} __wasi_dirent_t;
static_assert(offsetof(__wasi_dirent_t, d_next) == 0, "non-wasi data layout");
static_assert(offsetof(__wasi_dirent_t, d_ino) == 8, "non-wasi data layout");
static_assert(offsetof(__wasi_dirent_t, d_namlen) == 16, "non-wasi data layout");
static_assert(offsetof(__wasi_dirent_t, d_type) == 20, "non-wasi data layout");
static_assert(sizeof(__wasi_dirent_t) == 24, "non-wasi data layout");
static_assert(alignof(__wasi_dirent_t) == 8, "non-wasi data layout");

typedef struct __wasi_event_t
{
	__wasi_userdata_t userdata;
	__wasi_errno_t error;
	__wasi_eventtype_t type;
	union __wasi_event_u
	{
		struct __wasi_event_u_fd_readwrite_t
		{
			__wasi_filesize_t nbytes;
			__wasi_eventrwflags_t flags;
		} fd_readwrite;
	} u;
} __wasi_event_t;
static_assert(offsetof(__wasi_event_t, userdata) == 0, "non-wasi data layout");
static_assert(offsetof(__wasi_event_t, error) == 8, "non-wasi data layout");
static_assert(offsetof(__wasi_event_t, type) == 10, "non-wasi data layout");
static_assert(offsetof(__wasi_event_t, u.fd_readwrite.nbytes) == 16, "non-wasi data layout");
static_assert(offsetof(__wasi_event_t, u.fd_readwrite.flags) == 24, "non-wasi data layout");
static_assert(sizeof(__wasi_event_t) == 32, "non-wasi data layout");
static_assert(alignof(__wasi_event_t) == 8, "non-wasi data layout");

typedef struct __wasi_prestat_t
{
	__wasi_preopentype_t pr_type;
	union __wasi_prestat_u
	{
		struct __wasi_prestat_u_dir_t
		{
			__wasi_size_t pr_name_len;
		} dir;
	} u;
} __wasi_prestat_t;
static_assert(offsetof(__wasi_prestat_t, pr_type) == 0, "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || offsetof(__wasi_prestat_t, u.dir.pr_name_len) == 4,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || offsetof(__wasi_prestat_t, u.dir.pr_name_len) == 8,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || sizeof(__wasi_prestat_t) == 8,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || sizeof(__wasi_prestat_t) == 16,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || alignof(__wasi_prestat_t) == 4,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || alignof(__wasi_prestat_t) == 8,
			  "non-wasi data layout");

typedef struct __wasi_fdstat_t
{
	__wasi_filetype_t fs_filetype;
	__wasi_fdflags_t fs_flags;
	__wasi_rights_t fs_rights_base;
	__wasi_rights_t fs_rights_inheriting;
} __wasi_fdstat_t;
static_assert(offsetof(__wasi_fdstat_t, fs_filetype) == 0, "non-wasi data layout");
static_assert(offsetof(__wasi_fdstat_t, fs_flags) == 2, "non-wasi data layout");
static_assert(offsetof(__wasi_fdstat_t, fs_rights_base) == 8, "non-wasi data layout");
static_assert(offsetof(__wasi_fdstat_t, fs_rights_inheriting) == 16, "non-wasi data layout");
static_assert(sizeof(__wasi_fdstat_t) == 24, "non-wasi data layout");
static_assert(alignof(__wasi_fdstat_t) == 8, "non-wasi data layout");

typedef struct __wasi_filestat_t
{
	__wasi_device_t st_dev;
	__wasi_inode_t st_ino;
	__wasi_filetype_t st_filetype;
	__wasi_linkcount_t st_nlink;
	__wasi_filesize_t st_size;
	__wasi_timestamp_t st_atim;
	__wasi_timestamp_t st_mtim;
	__wasi_timestamp_t st_ctim;
} __wasi_filestat_t;
static_assert(offsetof(__wasi_filestat_t, st_dev) == 0, "non-wasi data layout");
static_assert(offsetof(__wasi_filestat_t, st_ino) == 8, "non-wasi data layout");
static_assert(offsetof(__wasi_filestat_t, st_filetype) == 16, "non-wasi data layout");
static_assert(offsetof(__wasi_filestat_t, st_nlink) == 24, "non-wasi data layout");
static_assert(offsetof(__wasi_filestat_t, st_size) == 32, "non-wasi data layout");
static_assert(offsetof(__wasi_filestat_t, st_atim) == 40, "non-wasi data layout");
static_assert(offsetof(__wasi_filestat_t, st_mtim) == 48, "non-wasi data layout");
static_assert(offsetof(__wasi_filestat_t, st_ctim) == 56, "non-wasi data layout");
static_assert(sizeof(__wasi_filestat_t) == 64, "non-wasi data layout");
static_assert(alignof(__wasi_filestat_t) == 8, "non-wasi data layout");

typedef struct __wasi_ciovec_t
{
	__wasi_void_ptr_t buf;
	__wasi_size_t buf_len;
} __wasi_ciovec_t;
static_assert(offsetof(__wasi_ciovec_t, buf) == 0, "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || offsetof(__wasi_ciovec_t, buf_len) == 4,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || offsetof(__wasi_ciovec_t, buf_len) == 8,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || sizeof(__wasi_ciovec_t) == 8,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || sizeof(__wasi_ciovec_t) == 16,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || alignof(__wasi_ciovec_t) == 4,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || alignof(__wasi_ciovec_t) == 8,
			  "non-wasi data layout");

typedef struct __wasi_iovec_t
{
	__wasi_void_ptr_t buf;
	__wasi_size_t buf_len;
} __wasi_iovec_t;
static_assert(offsetof(__wasi_iovec_t, buf) == 0, "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || offsetof(__wasi_iovec_t, buf_len) == 4,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || offsetof(__wasi_iovec_t, buf_len) == 8,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || sizeof(__wasi_iovec_t) == 8,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || sizeof(__wasi_iovec_t) == 16,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 4 || alignof(__wasi_iovec_t) == 4,
			  "non-wasi data layout");
static_assert(sizeof(__wasi_void_ptr_t) != 8 || alignof(__wasi_iovec_t) == 8,
			  "non-wasi data layout");

typedef struct __wasi_subscription_t
{
	__wasi_userdata_t userdata;
	__wasi_eventtype_t type;
	union __wasi_subscription_u
	{
		struct __wasi_subscription_u_clock_t
		{
			__wasi_clockid_t clock_id;
			__wasi_timestamp_t timeout;
			__wasi_timestamp_t precision;
			__wasi_subclockflags_t flags;
		} clock;
		struct __wasi_subscription_u_fd_readwrite_t
		{
			__wasi_fd_t fd;
		} fd_readwrite;
	} u;
} __wasi_subscription_t;
static_assert(offsetof(__wasi_subscription_t, userdata) == 0, "non-wasi data layout");
static_assert(offsetof(__wasi_subscription_t, type) == 8, "non-wasi data layout");
static_assert(offsetof(__wasi_subscription_t, u.clock.clock_id) == 16, "non-wasi data layout");
static_assert(offsetof(__wasi_subscription_t, u.clock.timeout) == 24, "non-wasi data layout");
static_assert(offsetof(__wasi_subscription_t, u.clock.precision) == 32, "non-wasi data layout");
static_assert(offsetof(__wasi_subscription_t, u.clock.flags) == 40, "non-wasi data layout");
static_assert(offsetof(__wasi_subscription_t, u.fd_readwrite.fd) == 16, "non-wasi data layout");
static_assert(sizeof(__wasi_subscription_t) == 48, "non-wasi data layout");
static_assert(alignof(__wasi_subscription_t) == 8, "non-wasi data layout");

#define __WASI_IOV_MAX 1024
