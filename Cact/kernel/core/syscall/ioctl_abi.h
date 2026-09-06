#ifndef IOCTL_ABI_H
#define IOCTL_ABI_H

// ioctl_abi.h — canonical kernel <-> userspace ABI for the VFS-node service
// model.
//
// The OS exposes almost all functionality through VFS nodes.  A process only
// ever traps for 15 core syscalls (open/close/read/write/ioctl/poll,
// fork/exec/exit/waitpid, brk/mmap/munmap/mprotect, sigreturn); everything
// else is reached by open()ing a node and then issuing ioctl/read/write on
// it.  This header defines every ioctl command and protocol structure used by
// that relay.  CactLib mirrors this file; keep both in sync.
//
// Numbering:
//   CACT_FDCTL_*   0x3000  ioctl on ANY open fd        -> fd-level ops
//   CACT_DIRCTL_*  0x3100  ioctl on a DIRECTORY fd     -> basename ops
//   CACT_PROCCTL_* 0x3200  ioctl on /proc/self|pid/ctl -> process control
//   CACT_SOCKCTL_* 0x3300  ioctl on a socket fd        -> network control
//   CACT_NETCTL_*  0x3400  ioctl on /dev/net           -> socket/ping/dns
//   CACT_SYSCTL_*  0x3500  ioctl on /dev/sys           -> mount/reboot/modules
//   CACT_PIPECTL_* 0x3600  ioctl on /dev/pipe          -> pipe creation
//   CACT_CRYPTCTL_* 0x3700 ioctl on /dev/crypto        -> hash/hmac/hkdf/aead/kx/random
// Device-specific ioctls (FB/TIOC, ...) keep their legacy numbers and are
// routed straight to the node's own ops; they must stay outside 0x3000-0x3FFF.
//
// ioctl conventions:
//   - return value >= 0 is meaningful (new fd, offset, mask, ...)
//   - return value < 0 is -errno
//   - every `arg` is a pointer to a user-space struct from this header, unless
//     documented otherwise.

#include <stdint.h>

// ===========================================================================
// Final core syscall numbering (15).  Applied when syscalls.h is rewritten in
// the final migration step (SYS_* names already collide with the legacy table
// and with mmap.h macros until then):
//   SYS_OPEN=0 SYS_CLOSE=1 SYS_READ=2 SYS_WRITE=3 SYS_IOCTL=4 SYS_POLL=5
//   SYS_FORK=6 SYS_EXEC=7 SYS_EXIT=8 SYS_WAITPID=9 SYS_BRK=10 SYS_MMAP=11
//   SYS_MUNMAP=12 SYS_MPROTECT=13 SYS_SIGRETURN=14  (SYS_COUNT=15)
// ===========================================================================

// ===========================================================================
// Shared stat structure (4 words, matches the kernel VFS stat buffer).
// ===========================================================================
typedef struct cact_stat {
    uint32_t inode;
    uint32_t mode;   // POSIX-ish mode (type bits | rwxrwxrwx)
    uint32_t size;
    uint32_t type;   // VFS node type (file/dir/chardev/...)
} cact_stat_t;

// ===========================================================================
// FD-level commands (ioctl on any open fd).  RANGE 0x3000.
// ===========================================================================
#define CACT_FDCTL_DUP        0x3001  // arg=NULL;          returns new fd
#define CACT_FDCTL_DUP2       0x3002  // arg=cact_fd_arg_t* (newfd)
#define CACT_FDCTL_FCNTL      0x3003  // arg=cact_fcntl_arg_t*
#define CACT_FDCTL_LSEEK      0x3004  // arg=cact_lseek_arg_t*; returns new offset
#define CACT_FDCTL_FSTAT      0x3005  // arg=cact_stat_t*    (out)
#define CACT_FDCTL_FTRUNCATE  0x3006  // arg=uint32_t* length
#define CACT_FDCTL_GETDENTS   0x3007  // arg=cact_getdents_arg_t*; returns bytes
#define CACT_FDCTL_FSYNC      0x3008  // arg=NULL (no-op)

typedef struct cact_fd_arg { uint32_t newfd; } cact_fd_arg_t;

// fcntl commands (subset of POSIX)
#define CACT_F_DUPFD  0
#define CACT_F_GETFD  1
#define CACT_F_SETFD  2
#define CACT_F_GETFL  3
#define CACT_F_SETFL  4

typedef struct cact_fcntl_arg { uint32_t cmd; uint32_t arg; } cact_fcntl_arg_t;

typedef struct cact_lseek_arg { int32_t offset; uint32_t whence; } cact_lseek_arg_t;
// whence: 0=SET 1=CUR 2=END

typedef struct cact_getdents_arg { void *buf; uint32_t count; } cact_getdents_arg_t;

// ===========================================================================
// Directory commands (ioctl on an open DIRECTORY fd).  RANGE 0x3100.
// All names are single path components resolved relative to the directory.
// ===========================================================================
#define CACT_DIRCTL_OPENAT    0x3101  // arg=cact_openat_arg_t*;   returns fd
#define CACT_DIRCTL_STAT      0x3102  // arg=cact_statat_arg_t*
#define CACT_DIRCTL_MKDIR     0x3103  // arg=char* name
#define CACT_DIRCTL_RMDIR     0x3104  // arg=char* name
#define CACT_DIRCTL_UNLINK    0x3105  // arg=char* name
#define CACT_DIRCTL_LINK      0x3106  // arg=cact_link_arg_t*   (hard link)
#define CACT_DIRCTL_SYMLINK   0x3107  // arg=cact_symlink_arg_t*
#define CACT_DIRCTL_READLINK  0x3108  // arg=cact_readlink_arg_t*
#define CACT_DIRCTL_RENAME    0x3109  // arg=cact_rename_arg_t*
#define CACT_DIRCTL_ACCESS    0x310A  // arg=cact_access_arg_t*
#define CACT_DIRCTL_CHMOD     0x310B  // arg=cact_chmod_arg_t*
#define CACT_DIRCTL_CHOWN     0x310C  // arg=cact_chown_arg_t*
#define CACT_DIRCTL_TRUNCATE  0x310D  // arg=cact_truncate_arg_t*
#define CACT_DIRCTL_MKNOD     0x310E  // arg=cact_mknod_arg_t*
#define CACT_DIRCTL_CREATE    0x310F  // arg=cact_openat_arg_t* (O_CREAT only)

// open flags (Linux/i386-compatible; must mirror the kernel OPEN_* values)
#define CACT_O_RDONLY 0
#define CACT_O_WRONLY 1
#define CACT_O_RDWR   2
#define CACT_O_CREAT  0x0040
#define CACT_O_TRUNC  0x0200
#define CACT_O_NONBLOCK 0x0800

typedef struct cact_openat_arg { char *name; uint32_t flags; } cact_openat_arg_t;
typedef struct cact_statat_arg { char *name; cact_stat_t *buf; } cact_statat_arg_t;
typedef struct cact_link_arg    { char *target; char *newname; } cact_link_arg_t;
typedef struct cact_symlink_arg { char *target; char *linkname; } cact_symlink_arg_t;
typedef struct cact_readlink_arg{ char *name; char *buf; uint32_t len; } cact_readlink_arg_t;
typedef struct cact_rename_arg  { char *oldname; char *newname; } cact_rename_arg_t;
typedef struct cact_access_arg  { char *name; uint32_t mode; } cact_access_arg_t;  // mode: 4=r 2=w 1=x
typedef struct cact_chmod_arg   { char *name; uint32_t mode; } cact_chmod_arg_t;
typedef struct cact_chown_arg   { char *name; uint32_t uid; uint32_t gid; } cact_chown_arg_t;
typedef struct cact_truncate_arg{ char *name; uint32_t length; } cact_truncate_arg_t;
typedef struct cact_mknod_arg   { char *name; uint32_t mode; uint32_t dev; } cact_mknod_arg_t;

// ===========================================================================
// /proc/self/info — binary read-only (44 bytes).
// ===========================================================================
typedef struct cact_proc_info {
    uint32_t pid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t sid;
    uint32_t uid;      // real uid
    uint32_t gid;      // real gid
    uint32_t euid;     // effective uid
    uint32_t egid;     // effective gid
    uint32_t umask;
    uint32_t state;    // task_state enum value
    uint32_t flags;    // bit 0 = kernel task
} cact_proc_info_t;

// /proc/self/cwd — read-only; content is the absolute cwd (no trailing NUL).

// /proc/time — binary read-only (8 bytes), monotonic since boot.
typedef struct cact_time { uint32_t sec; uint32_t usec; } cact_time_t;

// /proc/uname — binary read-only, layout matches struct utsname (Linux i386).
typedef struct cact_uname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
} cact_uname_t;

// ===========================================================================
// Process control (ioctl on /proc/self/ctl or /proc/<pid>/ctl). RANGE 0x3200.
// ===========================================================================
#define CACT_PROCCTL_SETSID      0x3200  // self; arg=NULL; returns new sid
#define CACT_PROCCTL_SETPGID     0x3201  // self; arg=cact_pgid_arg_t* (self or target)
#define CACT_PROCCTL_SETUID      0x3202  // self; arg=uint32_t*
#define CACT_PROCCTL_SETGID      0x3203  // self; arg=uint32_t*
#define CACT_PROCCTL_UMASK       0x3204  // self; arg=uint32_t* new mask; returns old
#define CACT_PROCCTL_CHDIR       0x3205  // self; arg=char* path
#define CACT_PROCCTL_CHROOT      0x3206  // self, root only; arg=char* path
#define CACT_PROCCTL_SIGNAL      0x3207  // self; arg=cact_signal_arg_t*  (kill(pid,sig))
#define CACT_PROCCTL_SIGACTION   0x3208  // self; arg=cact_sigaction_arg_t*
#define CACT_PROCCTL_SIGPROCMASK 0x3209  // self; arg=cact_sigprocmask_arg_t*
#define CACT_PROCCTL_SIGPENDING  0x320A  // self; arg=uint32_t* (out)
#define CACT_PROCCTL_SIGSUSPEND  0x320B  // self; arg=uint32_t* mask (blocks)
#define CACT_PROCCTL_ALARM       0x320C  // self; arg=uint32_t* secs; returns old
#define CACT_PROCCTL_SETITIMER   0x320D  // self; arg=cact_itimerval_arg_t*
#define CACT_PROCCTL_SHMGET      0x320E  // self; arg=cact_shmget_arg_t* -> shmid
#define CACT_PROCCTL_SHMAT       0x320F  // self; arg=cact_shmat_arg_t* -> addr
#define CACT_PROCCTL_SHMDT       0x3210  // self; arg=uint32_t* addr
#define CACT_PROCCTL_SHMCTL      0x3211  // self; arg=cact_shmctl_arg_t*

typedef struct cact_pgid_arg { uint32_t pid; uint32_t pgid; } cact_pgid_arg_t;
typedef struct cact_signal_arg { uint32_t pid; uint32_t signum; } cact_signal_arg_t;
typedef struct cact_sigaction_arg { uint32_t signum; uint32_t handler; } cact_sigaction_arg_t;
typedef struct cact_sigprocmask_arg { uint32_t how; uint32_t set; uint32_t oldset; } cact_sigprocmask_arg_t;
typedef struct cact_itimerval_arg { uint32_t it_value_ms; uint32_t it_interval_ms; uint32_t old_value_ms; uint32_t old_interval_ms; } cact_itimerval_arg_t;
typedef struct cact_shmget_arg { uint32_t key; uint32_t size; uint32_t flags; } cact_shmget_arg_t;
typedef struct cact_shmat_arg  { uint32_t shmid; uint32_t addr; uint32_t flags; } cact_shmat_arg_t;  // addr in/out
typedef struct cact_shmctl_arg { uint32_t shmid; uint32_t cmd; void *buf; } cact_shmctl_arg_t;

// ===========================================================================
// Socket control (ioctl on a socket fd). RANGE 0x3300.
// Data path is plain read()/write() on the socket fd.
// ===========================================================================
#define CACT_SOCKCTL_BIND        0x3301  // arg=cact_sockaddr_arg_t*
#define CACT_SOCKCTL_CONNECT     0x3302  // arg=cact_sockaddr_arg_t*
#define CACT_SOCKCTL_LISTEN      0x3303  // arg=NULL (backlog fixed)
#define CACT_SOCKCTL_ACCEPT      0x3304  // arg=cact_accept_arg_t*; returns new fd
#define CACT_SOCKCTL_SHUTDOWN    0x3305  // arg=uint32_t* how
#define CACT_SOCKCTL_SETSOCKOPT  0x3306  // arg=cact_sockopt_arg_t*
#define CACT_SOCKCTL_GETSOCKOPT  0x3307  // arg=cact_sockopt_arg_t*
#define CACT_SOCKCTL_SENDTO      0x3308  // arg=cact_sendto_arg_t*
#define CACT_SOCKCTL_RECVFROM    0x3309  // arg=cact_recvfrom_arg_t*
#define CACT_SOCKCTL_UNIX_BIND   0x330A  // AF_UNIX bind:   arg=cact_unix_addr_t*
#define CACT_SOCKCTL_UNIX_CONNECT 0x330B // AF_UNIX connect:arg=cact_unix_addr_t*
// AF_UNIX reuses CACT_SOCKCTL_LISTEN / ACCEPT / SHUTDOWN; the data path is
// plain read()/write(), as for AF_INET sockets.

typedef struct cact_sockaddr_in {
    uint32_t addr;   // IPv4 big-endian
    uint32_t port;   // network order
} cact_sockaddr_in_t;

// AF_UNIX pathname (sockaddr_un layout: family is implicit — an AF_UNIX fd
// already knows its family).  Paths are NUL-terminated; up to 107 bytes.
typedef struct cact_unix_addr {
    char path[108];
} cact_unix_addr_t;

typedef struct cact_sockaddr_arg {
    uint32_t fd;                 // for ioctls on /dev/net, the target fd
    cact_sockaddr_in_t addr;
} cact_sockaddr_arg_t;

typedef struct cact_accept_arg {
    cact_sockaddr_in_t peer;     // out
    uint32_t addrlen;            // in/out
} cact_accept_arg_t;

// socket option levels/names (kernel socket.h values; relay passes them through)
//   level: SOL_SOCKET=1, IPPROTO_TCP=6
//   SOL_SOCKET names: SO_REUSEADDR=2, SO_KEEPALIVE=9, SO_ERROR=4
//   IPPROTO_TCP names: TCP_NODELAY=1
typedef struct cact_sockopt_arg {
    uint32_t level;              // SOL_SOCKET=1 / IPPROTO_TCP=6
    uint32_t optname;            // kernel socket.h option number
    uint32_t val;                // in for setsockopt
    uint32_t val_out;            // out for getsockopt
} cact_sockopt_arg_t;

typedef struct cact_sendto_arg {
    cact_sockaddr_in_t dst;      // only for UDP
    void *buf;
    uint32_t len;
} cact_sendto_arg_t;

typedef struct cact_recvfrom_arg {
    cact_sockaddr_in_t src;      // out (only for UDP)
    void *buf;
    uint32_t len;
} cact_recvfrom_arg_t;

// ===========================================================================
// /dev/net control. RANGE 0x3400.
// ===========================================================================
#define CACT_NETCTL_SOCKET       0x3401  // arg=cact_socket_arg_t*; returns fd
#define CACT_NETCTL_PING         0x3402  // arg=cact_ping_arg_t*
#define CACT_NETCTL_DNS_RESOLVE  0x3403  // arg=cact_dns_arg_t*
#define CACT_NETCTL_NETCFG       0x3404  // arg=cact_netcfg_arg_t* (root)
#define CACT_NETCTL_SOCKETPAIR   0x3405  // arg=cact_socketpair_arg_t*; fds[2] out

typedef struct cact_socket_arg { uint32_t domain; uint32_t type; uint32_t proto; } cact_socket_arg_t;
typedef struct cact_socketpair_arg { uint32_t type; uint32_t fds[2]; } cact_socketpair_arg_t;
typedef struct cact_ping_arg { uint32_t dst_ip; uint32_t id; uint32_t seq; } cact_ping_arg_t;
typedef struct cact_dns_arg { char *name; uint32_t *out_ip; } cact_dns_arg_t;
typedef struct cact_netcfg_arg {
    uint32_t ip_host;
    uint32_t netmask_host;
    uint32_t gateway_host;
    uint32_t dns_host;
    uint32_t dhcp_server_host;
    uint32_t lease_s;
    uint32_t t1_s;
    uint32_t t2_s;
} cact_netcfg_arg_t;

// ===========================================================================
// /dev/sys control (privileged, root only). RANGE 0x3500.
// ===========================================================================
#define CACT_SYSCTL_MOUNT        0x3501  // arg=cact_mount_arg_t*
#define CACT_SYSCTL_UMOUNT       0x3502  // arg=char* target
#define CACT_SYSCTL_REBOOT       0x3503  // arg=uint32_t* cmd
#define CACT_SYSCTL_MODULE_LOAD  0x3504  // arg=cact_module_arg_t*
#define CACT_SYSCTL_MODULE_UNLOAD 0x3505 // arg=char* name

typedef struct cact_mount_arg { char *src; char *target; char *fstype; } cact_mount_arg_t;
typedef struct cact_module_arg { char *path; uint32_t vendor_id; uint32_t device_id; } cact_module_arg_t;

#define CACT_REBOOT_RESTART  0x01234567u
#define CACT_REBOOT_HALT     0xCDEF0123u
#define CACT_REBOOT_POWEROFF 0x4321FEDCu

// ===========================================================================
// /dev/pipe control. RANGE 0x3600.
// ===========================================================================
#define CACT_PIPECTL_CREATE 0x3601  // arg=uint32_t fds[2] (out)

// ===========================================================================
// /dev/crypto control. RANGE 0x3700.
//
// Exposes the in-kernel crypto primitives (the same algorithms the rustls
// cact_crypto provider offers) to userspace.  Every arg is a struct from this
// header; input buffers are read-only user pointers, output arrays are
// embedded in the structs, output buffers are separate user pointers with an
// explicit capacity.  All `alg` fields use the CACT_CRYPT_* constants below.
// ===========================================================================
#define CACT_CRYPTCTL_RANDOM      0x3701  // arg=cact_crypt_random_arg_t*
#define CACT_CRYPTCTL_HASH        0x3702  // arg=cact_crypt_hash_arg_t*
#define CACT_CRYPTCTL_HMAC        0x3703  // arg=cact_crypt_hmac_arg_t*  (sign)
#define CACT_CRYPTCTL_HMAC_VERIFY 0x3704  // arg=cact_crypt_hmac_arg_t*  (tag in)
#define CACT_CRYPTCTL_HKDF        0x3705  // arg=cact_crypt_hkdf_arg_t*
#define CACT_CRYPTCTL_AEAD        0x3706  // arg=cact_crypt_aead_arg_t*
#define CACT_CRYPTCTL_KX_KEYGEN   0x3707  // arg=cact_crypt_kx_keygen_arg_t*
#define CACT_CRYPTCTL_KX_DERIVE   0x3708  // arg=cact_crypt_kx_derive_arg_t*

// algorithm selectors
#define CACT_CRYPT_SHA256     0   // hash / hmac / hkdf: SHA-256 family
#define CACT_CRYPT_SHA384     1   // hash / hmac / hkdf: SHA-384 family
#define CACT_CRYPT_AES128_GCM 0   // aead
#define CACT_CRYPT_AES256_GCM 1   // aead
#define CACT_CRYPT_KX_X25519  0   // key exchange
#define CACT_CRYPT_KX_P256    1   // key exchange (secp256r1)
#define CACT_CRYPT_OP_SEAL    0   // aead: encrypt
#define CACT_CRYPT_OP_OPEN    1   // aead: decrypt

// fixed sizes (bytes)
#define CACT_CRYPT_SHA256_LEN  32
#define CACT_CRYPT_SHA384_LEN  48
#define CACT_CRYPT_MAX_TAG     64
#define CACT_CRYPT_NONCE_LEN   12
#define CACT_CRYPT_GCM_TAG_LEN 16
#define CACT_CRYPT_PUB_MAX     65   // X25519: 32, P-256: 65 (uncompressed SEC1)
#define CACT_CRYPT_SECRET_LEN  32

typedef struct cact_crypt_random_arg {
    uint8_t *buf;         // out: random bytes
    uint32_t len;
} cact_crypt_random_arg_t;

typedef struct cact_crypt_hash_arg {
    uint32_t alg;         // CACT_CRYPT_SHA256 / CACT_CRYPT_SHA384
    const uint8_t *data;  // in
    uint32_t data_len;
    uint8_t digest[64];   // out: 32 or 48 bytes used
} cact_crypt_hash_arg_t;

typedef struct cact_crypt_hmac_arg {
    uint32_t alg;         // CACT_CRYPT_SHA256 / CACT_CRYPT_SHA384
    const uint8_t *key;   // in
    uint32_t key_len;
    const uint8_t *data;  // in
    uint32_t data_len;
    uint8_t tag[64];      // out (sign) / in (verify): 32 or 48 bytes used
} cact_crypt_hmac_arg_t;

typedef struct cact_crypt_hkdf_arg {
    uint32_t alg;              // CACT_CRYPT_SHA256 / CACT_CRYPT_SHA384
    const uint8_t *salt;       // in (may be NULL when salt_len == 0)
    uint32_t salt_len;
    const uint8_t *ikm;        // in
    uint32_t ikm_len;
    const uint8_t *info;       // in (may be NULL when info_len == 0)
    uint32_t info_len;
    uint8_t *out;              // out
    uint32_t out_len;          // out length requested / written
} cact_crypt_hkdf_arg_t;

typedef struct cact_crypt_aead_arg {
    uint32_t alg;         // CACT_CRYPT_AES128_GCM / CACT_CRYPT_AES256_GCM
    uint32_t op;          // CACT_CRYPT_OP_SEAL / CACT_CRYPT_OP_OPEN
    const uint8_t *key;   // in: 16 (AES-128) or 32 (AES-256) bytes
    uint32_t key_len;
    uint8_t nonce[12];    // in
    const uint8_t *aad;   // in (may be NULL when aad_len == 0)
    uint32_t aad_len;
    const uint8_t *in;    // in: plaintext (SEAL) or ciphertext||tag (OPEN)
    uint32_t in_len;
    uint8_t *out;         // out: ciphertext||tag (SEAL) or plaintext (OPEN)
    uint32_t out_cap;     // capacity of out
    uint32_t out_len;     // out: bytes written
} cact_crypt_aead_arg_t;

typedef struct cact_crypt_kx_keygen_arg {
    uint32_t alg;         // CACT_CRYPT_KX_X25519 / CACT_CRYPT_KX_P256
    uint8_t pub[65];      // out: X25519 32 bytes / P-256 65 bytes
    uint8_t priv[32];     // out
} cact_crypt_kx_keygen_arg_t;

typedef struct cact_crypt_kx_derive_arg {
    uint32_t alg;         // CACT_CRYPT_KX_X25519 / CACT_CRYPT_KX_P256
    uint8_t priv[32];     // in
    uint8_t peer_pub[65]; // in: X25519 32 bytes / P-256 65 bytes (uncompressed)
    uint8_t shared[32];   // out
} cact_crypt_kx_derive_arg_t;

#endif
