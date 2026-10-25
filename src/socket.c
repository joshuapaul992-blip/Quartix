/*
 * Quartix - BSD sockets for the Android engine, on Winsock2.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * Roblox's networking imports the BSD socket API from bionic; Windows serves
 * the same calls through Winsock2 (ws2_32, already linked, WSAPoll since
 * Vista). The calls look identical but the constants and structs do not, so
 * this layer translates in both directions:
 *
 *   - address family: Linux AF_INET6 is 10, Windows' is 23 (AF_INET is 2 on
 *     both). Every sockaddr crossing the boundary has its family fixed up.
 *   - socket type flags: Linux ORs SOCK_NONBLOCK/SOCK_CLOEXEC into the type;
 *     those are stripped and NONBLOCK re-applied with ioctlsocket(FIONBIO).
 *   - setsockopt/getsockopt levels and option names differ wholesale
 *     (SOL_SOCKET is 1 vs 0xffff, SO_REUSEADDR 2 vs 4, ...); the common ones
 *     are mapped, and SO_RCVTIMEO/SO_SNDTIMEO convert between a Linux timeval
 *     and the Windows millisecond DWORD.
 *   - poll(): WSAPOLLFD has a 64-bit SOCKET, so the Linux pollfd[] is repacked
 *     and the event bits translated.
 *   - select(): the Linux fd_set is a 1024-bit mask; the Windows one is a
 *     SOCKET array, so the masks are converted in and the results back out.
 *   - getaddrinfo(): the Linux addrinfo swaps ai_canonname/ai_addr relative to
 *     Windows and carries Linux family values, so a Linux-layout chain is built
 *     from the Windows result and freed by our freeaddrinfo.
 *   - errno: WSAGetLastError's WSAE* codes map to Linux errno in the engine's
 *     __errno slot.
 *
 * Sockets are not CRT descriptors on Windows, so a small registry records which
 * int the engine holds is a socket; src/stdio.c consults it so close/read/write
 * on a socket fd route to closesocket/recv/send. Every engine-callable function
 * is sysv_abi. Scope: the core socket calls; scatter/gather (sendmsg/recvmsg,
 * the mmsg batch forms), socketpair and the name-resolution extras are a
 * follow-up slice.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h> /* if_nametoindex / if_indextoname (Vista+) */
#include "quartix.h"
#include <stdint.h>
#include <string.h>

#define SYSV __attribute__((sysv_abi))

/* ---- Linux constants ------------------------------------------------------ */
#define LX_AF_UNSPEC 0
#define LX_AF_INET 2
#define LX_AF_INET6 10
#define LX_SOCK_NONBLOCK 0x800
#define LX_SOCK_CLOEXEC 0x80000
#define LX_SOL_SOCKET 1
#define LX_IPPROTO_IP 0
#define LX_IPPROTO_TCP 6
#define LX_IPPROTO_IPV6 41

/* Linux errno values the engine checks against */
#define LX_EINTR 4
#define LX_EBADF 9
#define LX_EAGAIN 11
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_EINVAL 22
#define LX_EMFILE 24
#define LX_EMSGSIZE 90
#define LX_EPROTONOSUPPORT 93
#define LX_EOPNOTSUPP 95
#define LX_EAFNOSUPPORT 97
#define LX_EADDRINUSE 98
#define LX_EADDRNOTAVAIL 99
#define LX_ENETUNREACH 101
#define LX_ECONNABORTED 103
#define LX_ECONNRESET 104
#define LX_ENOBUFS 105
#define LX_EISCONN 106
#define LX_ENOTCONN 107
#define LX_ETIMEDOUT 110
#define LX_ECONNREFUSED 111
#define LX_EHOSTUNREACH 113
#define LX_EALREADY 114
#define LX_EINPROGRESS 115

/* ---- startup + errno ------------------------------------------------------ */
static LONG g_wsa_started;
static void wsa_ensure(void)
{
    if (InterlockedCompareExchange(&g_wsa_started, 1, 0) == 0) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
}
static int wsa_to_errno(int e)
{
    switch (e) {
    case WSAEINTR: return LX_EINTR;
    case WSAEBADF: return LX_EBADF;
    case WSAEWOULDBLOCK: return LX_EAGAIN;
    case WSAEINPROGRESS: return LX_EINPROGRESS;
    case WSAEALREADY: return LX_EALREADY;
    case WSAEACCES: return LX_EACCES;
    case WSAEFAULT: return LX_EFAULT;
    case WSAEINVAL: return LX_EINVAL;
    case WSAEMFILE: return LX_EMFILE;
    case WSAEMSGSIZE: return LX_EMSGSIZE;
    case WSAEPROTONOSUPPORT: return LX_EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return LX_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return LX_EAFNOSUPPORT;
    case WSAEADDRINUSE: return LX_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return LX_EADDRNOTAVAIL;
    case WSAENETUNREACH: return LX_ENETUNREACH;
    case WSAECONNABORTED: return LX_ECONNABORTED;
    case WSAECONNRESET: return LX_ECONNRESET;
    case WSAENOBUFS: return LX_ENOBUFS;
    case WSAEISCONN: return LX_EISCONN;
    case WSAENOTCONN: return LX_ENOTCONN;
    case WSAETIMEDOUT: return LX_ETIMEDOUT;
    case WSAECONNREFUSED: return LX_ECONNREFUSED;
    case WSAEHOSTUNREACH: return LX_EHOSTUNREACH;
    case WSAENOTSOCK: return LX_EBADF;
    default: return LX_EINVAL;
    }
}
static void set_err(void) { *qx_shim_errno() = wsa_to_errno(WSAGetLastError()); }

/* ---- socket fd registry (so close/read/write route sockets) --------------- */
#define QX_MAX_SOCKS 1024
static int g_socks[QX_MAX_SOCKS];
static int g_sock_n;
static CRITICAL_SECTION g_sock_lock;
static LONG g_sock_lock_init;
static void sock_lock(void)
{
    if (InterlockedCompareExchange(&g_sock_lock_init, 1, 0) == 0)
        InitializeCriticalSection(&g_sock_lock);
    /* a second caller before init completes would race; init runs under the
     * first socket() call, before any fd exists to close, so this is safe */
    EnterCriticalSection(&g_sock_lock);
}
static void sock_register(int fd)
{
    sock_lock();
    if (g_sock_n < QX_MAX_SOCKS)
        g_socks[g_sock_n++] = fd;
    LeaveCriticalSection(&g_sock_lock);
}
static void sock_unregister(int fd)
{
    int i;
    sock_lock();
    for (i = 0; i < g_sock_n; i++)
        if (g_socks[i] == fd) {
            g_socks[i] = g_socks[--g_sock_n];
            break;
        }
    LeaveCriticalSection(&g_sock_lock);
}
int qx_socket_is_sock(int fd)
{
    int i, found = 0;
    if (!g_sock_lock_init)
        return 0;
    sock_lock();
    for (i = 0; i < g_sock_n; i++)
        if (g_socks[i] == fd) { found = 1; break; }
    LeaveCriticalSection(&g_sock_lock);
    return found;
}
int qx_socket_close(int fd)
{
    int r = closesocket((SOCKET)fd);
    if (r != 0) set_err();
    else sock_unregister(fd);
    return r;
}
long long qx_socket_read(int fd, void *buf, size_t n)
{
    int cap = n > 0x7fffffff ? 0x7fffffff : (int)n;
    int r = recv((SOCKET)fd, (char *)buf, cap, 0);
    if (r < 0) set_err();
    return r;
}
long long qx_socket_write(int fd, const void *buf, size_t n)
{
    int cap = n > 0x7fffffff ? 0x7fffffff : (int)n;
    int r = send((SOCKET)fd, (const char *)buf, cap, 0);
    if (r < 0) set_err();
    return r;
}

/* ---- family + sockaddr translation ---------------------------------------- */
static int af_to_win(int af) { return af == LX_AF_INET6 ? AF_INET6 : af; }
static int af_to_lx(int af) { return af == AF_INET6 ? LX_AF_INET6 : af; }

/* sockaddr_in/in6 share their byte layout across Linux and Windows except for
 * the family number in the first 16-bit field, so an in-place family fixup on a
 * copy is all that is needed. */
static int sa_to_win(const void *src, int len, struct sockaddr_storage *dst)
{
    uint16_t fam;
    if (len <= 0 || (size_t)len > sizeof(*dst))
        return -1;
    memcpy(dst, src, (size_t)len);
    memcpy(&fam, src, 2);
    fam = (uint16_t)af_to_win(fam);
    memcpy(dst, &fam, 2);
    return len;
}
static void sa_to_lx(void *dst, const void *win, int len)
{
    uint16_t fam;
    if (len <= 0)
        return;
    memcpy(dst, win, (size_t)len);
    memcpy(&fam, win, 2);
    fam = (uint16_t)af_to_lx(fam);
    memcpy(dst, &fam, 2);
}

/* ---- socket / bind / connect / listen / accept ---------------------------- */
static SYSV int qx_socket(int domain, int type, int protocol)
{
    SOCKET s;
    int nonblock = (type & LX_SOCK_NONBLOCK) ? 1 : 0;
    int t = type & ~(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC);
    wsa_ensure();
    s = socket(af_to_win(domain), t, protocol);
    if (s == INVALID_SOCKET) { set_err(); return -1; }
    if (nonblock) {
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
    }
    sock_register((int)s);
    return (int)s;
}
static SYSV int qx_bind(int fd, const void *addr, int len)
{
    struct sockaddr_storage ss;
    int wl = sa_to_win(addr, len, &ss);
    int r;
    if (wl < 0) { *qx_shim_errno() = LX_EINVAL; return -1; }
    r = bind((SOCKET)fd, (struct sockaddr *)&ss, wl);
    if (r != 0) set_err();
    return r;
}
static SYSV int qx_connect(int fd, const void *addr, int len)
{
    struct sockaddr_storage ss;
    int wl = sa_to_win(addr, len, &ss);
    int r;
    if (wl < 0) { *qx_shim_errno() = LX_EINVAL; return -1; }
    r = connect((SOCKET)fd, (struct sockaddr *)&ss, wl);
    if (r != 0) set_err();
    return r;
}
static SYSV int qx_listen(int fd, int backlog)
{
    int r = listen((SOCKET)fd, backlog);
    if (r != 0) set_err();
    return r;
}
static SYSV int qx_accept(int fd, void *addr, int *len)
{
    struct sockaddr_storage ss;
    int sl = sizeof(ss);
    SOCKET s = accept((SOCKET)fd, (struct sockaddr *)&ss, len ? &sl : NULL);
    if (s == INVALID_SOCKET) { set_err(); return -1; }
    if (addr && len && *len > 0) {
        int out = sl < *len ? sl : *len;
        sa_to_lx(addr, &ss, out);
        *len = sl;
    }
    sock_register((int)s);
    return (int)s;
}
static SYSV int qx_accept4(int fd, void *addr, int *len, int flags)
{
    int s = qx_accept(fd, addr, len);
    if (s >= 0 && (flags & LX_SOCK_NONBLOCK)) {
        u_long on = 1;
        ioctlsocket((SOCKET)s, FIONBIO, &on);
    }
    return s;
}
static SYSV int qx_shutdown(int fd, int how)
{
    int r = shutdown((SOCKET)fd, how);
    if (r != 0) set_err();
    return r;
}
static SYSV int qx_getsockname(int fd, void *addr, int *len)
{
    struct sockaddr_storage ss;
    int sl = sizeof(ss);
    int r = getsockname((SOCKET)fd, (struct sockaddr *)&ss, &sl);
    if (r != 0) { set_err(); return r; }
    if (addr && len) { int out = sl < *len ? sl : *len; sa_to_lx(addr, &ss, out); *len = sl; }
    return 0;
}
static SYSV int qx_getpeername(int fd, void *addr, int *len)
{
    struct sockaddr_storage ss;
    int sl = sizeof(ss);
    int r = getpeername((SOCKET)fd, (struct sockaddr *)&ss, &sl);
    if (r != 0) { set_err(); return r; }
    if (addr && len) { int out = sl < *len ? sl : *len; sa_to_lx(addr, &ss, out); *len = sl; }
    return 0;
}

/* ---- send / recv datagram ------------------------------------------------- */
static SYSV long long qx_sendto(int fd, const void *buf, size_t n, int flags,
                                const void *addr, int alen)
{
    int cap = n > 0x7fffffff ? 0x7fffffff : (int)n;
    int r;
    if (addr && alen > 0) {
        struct sockaddr_storage ss;
        int wl = sa_to_win(addr, alen, &ss);
        if (wl < 0) { *qx_shim_errno() = LX_EINVAL; return -1; }
        r = sendto((SOCKET)fd, (const char *)buf, cap, flags, (struct sockaddr *)&ss, wl);
    } else {
        r = send((SOCKET)fd, (const char *)buf, cap, flags);
    }
    if (r < 0) set_err();
    return r;
}
static SYSV long long qx_recvfrom(int fd, void *buf, size_t n, int flags,
                                  void *addr, int *alen)
{
    int cap = n > 0x7fffffff ? 0x7fffffff : (int)n;
    struct sockaddr_storage ss;
    int sl = sizeof(ss);
    int r = recvfrom((SOCKET)fd, (char *)buf, cap, flags,
                     addr ? (struct sockaddr *)&ss : NULL, addr ? &sl : NULL);
    if (r < 0) { set_err(); return r; }
    if (addr && alen && *alen > 0) { int out = sl < *alen ? sl : *alen; sa_to_lx(addr, &ss, out); *alen = sl; }
    return r;
}

/* ---- sockopt -------------------------------------------------------------- */
struct opt_map { int lx, win; };
static const struct opt_map g_so_opts[] = {
    {1, SO_DEBUG}, {2, SO_REUSEADDR}, {3, SO_TYPE}, {4, SO_ERROR}, {5, SO_DONTROUTE},
    {6, SO_BROADCAST}, {7, SO_SNDBUF}, {8, SO_RCVBUF}, {9, SO_KEEPALIVE},
    {10, SO_OOBINLINE}, {13, SO_LINGER}, {15, SO_REUSEADDR /*REUSEPORT~*/},
    {18, SO_RCVLOWAT}, {19, SO_SNDLOWAT}, {20, SO_RCVTIMEO}, {21, SO_SNDTIMEO},
    {30, SO_ACCEPTCONN},
};
static int map_so(int lx)
{
    size_t i;
    for (i = 0; i < sizeof(g_so_opts) / sizeof(g_so_opts[0]); i++)
        if (g_so_opts[i].lx == lx)
            return g_so_opts[i].win;
    return lx;
}
static int map_level(int lx) { return lx == LX_SOL_SOCKET ? SOL_SOCKET : lx; }
/* Linux timeval is {int64 sec; int64 usec}; Windows SO_*TIMEO is a DWORD ms */
struct lx_timeval64 { int64_t sec, usec; };
static SYSV int qx_setsockopt(int fd, int level, int opt, const void *val, int len)
{
    int r;
    if (level == LX_SOL_SOCKET && (opt == 20 || opt == 21) && val && (size_t)len >= sizeof(struct lx_timeval64)) {
        const struct lx_timeval64 *tv = (const struct lx_timeval64 *)val;
        DWORD ms = (DWORD)(tv->sec * 1000 + tv->usec / 1000);
        r = setsockopt((SOCKET)fd, SOL_SOCKET, map_so(opt), (const char *)&ms, sizeof(ms));
    } else {
        r = setsockopt((SOCKET)fd, map_level(level), map_so(opt), (const char *)val, len);
    }
    if (r != 0) set_err();
    return r;
}
static SYSV int qx_getsockopt(int fd, int level, int opt, void *val, int *len)
{
    int r;
    if (level == LX_SOL_SOCKET && (opt == 20 || opt == 21) && val && len && (size_t)*len >= sizeof(struct lx_timeval64)) {
        DWORD ms = 0; int ml = sizeof(ms);
        r = getsockopt((SOCKET)fd, SOL_SOCKET, map_so(opt), (char *)&ms, &ml);
        if (r == 0) {
            struct lx_timeval64 *tv = (struct lx_timeval64 *)val;
            tv->sec = ms / 1000; tv->usec = (int64_t)(ms % 1000) * 1000;
            *len = sizeof(*tv);
        }
    } else {
        r = getsockopt((SOCKET)fd, map_level(level), map_so(opt), (char *)val, len);
    }
    if (r != 0) set_err();
    return r;
}

/* ---- inet_ntop / inet_pton ------------------------------------------------ */
static SYSV const char *qx_inet_ntop(int af, const void *src, char *dst, unsigned size)
{
    return inet_ntop(af_to_win(af), (void *)src, dst, size);
}
static SYSV int qx_inet_pton(int af, const char *src, void *dst)
{
    return inet_pton(af_to_win(af), src, dst);
}

/* ---- poll (via WSAPoll) --------------------------------------------------- */
#define LX_POLLIN 0x001
#define LX_POLLPRI 0x002
#define LX_POLLOUT 0x004
#define LX_POLLERR 0x008
#define LX_POLLHUP 0x010
#define LX_POLLNVAL 0x020
struct lx_pollfd { int fd; short events; short revents; };
static short pe_to_win(short e)
{
    short w = 0;
    if (e & LX_POLLIN) w |= POLLRDNORM;
    if (e & LX_POLLPRI) w |= POLLRDBAND;
    if (e & LX_POLLOUT) w |= POLLWRNORM;
    return w;
}
static short pe_to_lx(short w)
{
    short e = 0;
    if (w & POLLRDNORM) e |= LX_POLLIN;
    if (w & POLLRDBAND) e |= LX_POLLPRI;
    if (w & POLLWRNORM) e |= LX_POLLOUT;
    if (w & POLLERR) e |= LX_POLLERR;
    if (w & POLLHUP) e |= LX_POLLHUP;
    if (w & POLLNVAL) e |= LX_POLLNVAL;
    return e;
}
static SYSV int qx_poll(struct lx_pollfd *fds, unsigned nfds, int timeout)
{
    WSAPOLLFD *wf;
    int r;
    unsigned i;
    if (nfds == 0)
        return 0;
    wf = (WSAPOLLFD *)calloc(nfds, sizeof(*wf));
    if (!wf) { *qx_shim_errno() = LX_ENOBUFS; return -1; }
    for (i = 0; i < nfds; i++) {
        wf[i].fd = (SOCKET)fds[i].fd;
        wf[i].events = pe_to_win(fds[i].events);
        wf[i].revents = 0;
    }
    r = WSAPoll(wf, nfds, timeout);
    if (r == SOCKET_ERROR) { set_err(); free(wf); return -1; }
    for (i = 0; i < nfds; i++)
        fds[i].revents = pe_to_lx(wf[i].revents);
    free(wf);
    return r;
}

/* ---- select (Linux fd_set bitmask <-> Winsock fd_set) --------------------- */
#define LX_FD_SETSIZE 1024
static int bitmask_to_win(const uint64_t *m, fd_set *out, int nfds)
{
    int fd;
    FD_ZERO(out);
    if (!m)
        return 0;
    for (fd = 0; fd < nfds && fd < LX_FD_SETSIZE; fd++)
        if (m[fd / 64] & ((uint64_t)1 << (fd % 64)))
            FD_SET((SOCKET)fd, out);
    return 0;
}
static void win_to_bitmask(const fd_set *in, uint64_t *m, int nfds)
{
    int fd;
    if (!m)
        return;
    memset(m, 0, ((LX_FD_SETSIZE + 63) / 64) * sizeof(uint64_t));
    for (fd = 0; fd < nfds && fd < LX_FD_SETSIZE; fd++)
        if (FD_ISSET((SOCKET)fd, (fd_set *)in))
            m[fd / 64] |= (uint64_t)1 << (fd % 64);
}
static SYSV int qx_select(int nfds, uint64_t *rd, uint64_t *wr, uint64_t *ex, struct lx_timeval64 *tv)
{
    fd_set rs, ws, es;
    struct timeval wtv, *ptv = NULL;
    int r;
    bitmask_to_win(rd, &rs, nfds);
    bitmask_to_win(wr, &ws, nfds);
    bitmask_to_win(ex, &es, nfds);
    if (tv) {
        wtv.tv_sec = (long)tv->sec;
        wtv.tv_usec = (long)tv->usec;
        ptv = &wtv;
    }
    r = select(nfds, rd ? &rs : NULL, wr ? &ws : NULL, ex ? &es : NULL, ptv);
    if (r == SOCKET_ERROR) { set_err(); return -1; }
    if (rd) win_to_bitmask(&rs, rd, nfds);
    if (wr) win_to_bitmask(&ws, wr, nfds);
    if (ex) win_to_bitmask(&es, ex, nfds);
    return r;
}

/* ---- getaddrinfo (build a Linux-layout chain) ----------------------------- */
struct lx_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    uint32_t ai_addrlen;
    void *ai_addr;        /* Linux order: addr before canonname */
    char *ai_canonname;
    struct lx_addrinfo *ai_next;
};
void qx_freeaddrinfo_impl(struct lx_addrinfo *ai); /* defined below; used in the error path */
static SYSV int qx_getaddrinfo(const char *node, const char *service,
                               const struct lx_addrinfo *hints, struct lx_addrinfo **res)
{
    struct addrinfo whints, *wres = NULL, *w;
    struct lx_addrinfo *head = NULL, **tail = &head;
    int r;
    wsa_ensure();
    if (hints) {
        memset(&whints, 0, sizeof(whints));
        whints.ai_flags = hints->ai_flags;
        whints.ai_family = af_to_win(hints->ai_family);
        whints.ai_socktype = hints->ai_socktype;
        whints.ai_protocol = hints->ai_protocol;
    }
    r = getaddrinfo(node, service, hints ? &whints : NULL, &wres);
    if (r != 0)
        return r; /* EAI_* codes are the same numbers on both */
    for (w = wres; w; w = w->ai_next) {
        struct lx_addrinfo *n = (struct lx_addrinfo *)calloc(1, sizeof(*n));
        if (!n) { freeaddrinfo(wres); qx_freeaddrinfo_impl(head); if (res) *res = NULL; return EAI_MEMORY; }
        n->ai_flags = w->ai_flags;
        n->ai_family = af_to_lx(w->ai_family);
        n->ai_socktype = w->ai_socktype;
        n->ai_protocol = w->ai_protocol;
        n->ai_addrlen = (uint32_t)w->ai_addrlen;
        if (w->ai_addr && w->ai_addrlen) {
            n->ai_addr = malloc(w->ai_addrlen);
            if (n->ai_addr) sa_to_lx(n->ai_addr, w->ai_addr, (int)w->ai_addrlen);
        }
        if (w->ai_canonname) {
            size_t l = strlen(w->ai_canonname) + 1;
            n->ai_canonname = (char *)malloc(l);
            if (n->ai_canonname) memcpy(n->ai_canonname, w->ai_canonname, l);
        }
        *tail = n;
        tail = &n->ai_next;
    }
    freeaddrinfo(wres);
    if (res) *res = head;
    return 0;
}
/* exposed so getaddrinfo's error path can free a partial chain */
void qx_freeaddrinfo_impl(struct lx_addrinfo *ai)
{
    while (ai) {
        struct lx_addrinfo *n = ai->ai_next;
        free(ai->ai_addr);
        free(ai->ai_canonname);
        free(ai);
        ai = n;
    }
}
static SYSV void qx_freeaddrinfo(struct lx_addrinfo *ai) { qx_freeaddrinfo_impl(ai); }
static SYSV const char *qx_gai_strerror(int code)
{
    (void)code;
    return "getaddrinfo error"; /* bionic returns a static string; callers only print it */
}

/* ---- scatter/gather: sendmsg / recvmsg (+ the mmsg batch forms) ----------- */
struct lx_iovec { void *iov_base; size_t iov_len; };
struct lx_msghdr {
    void *msg_name;
    uint32_t msg_namelen;
    uint32_t _pad;
    struct lx_iovec *msg_iov;
    uint64_t msg_iovlen;
    void *msg_control;
    uint64_t msg_controllen;
    int msg_flags;
};
struct lx_mmsghdr { struct lx_msghdr msg_hdr; unsigned msg_len; };

/* Build a WSABUF array from the Linux iovec array; returns a malloc'd array the
 * caller frees (NULL on allocation failure or zero iovs). */
static WSABUF *iov_to_wsabuf(const struct lx_iovec *iov, uint64_t n)
{
    WSABUF *b;
    uint64_t i;
    if (n == 0)
        return NULL;
    b = (WSABUF *)malloc((size_t)n * sizeof(*b));
    if (!b)
        return NULL;
    for (i = 0; i < n; i++) {
        b[i].buf = (char *)iov[i].iov_base;
        b[i].len = iov[i].iov_len > 0xffffffffu ? 0xffffffffu : (ULONG)iov[i].iov_len;
    }
    return b;
}
static SYSV long long qx_sendmsg(int fd, const struct lx_msghdr *m, int flags)
{
    WSABUF *b;
    DWORD sent = 0;
    int r;
    if (!m) { *qx_shim_errno() = LX_EFAULT; return -1; }
    b = iov_to_wsabuf(m->msg_iov, m->msg_iovlen);
    if (m->msg_iovlen && !b) { *qx_shim_errno() = LX_ENOBUFS; return -1; }
    if (m->msg_name && m->msg_namelen) {
        struct sockaddr_storage ss;
        int wl = sa_to_win(m->msg_name, (int)m->msg_namelen, &ss);
        r = WSASendTo((SOCKET)fd, b, (DWORD)m->msg_iovlen, &sent, (DWORD)flags,
                      (struct sockaddr *)&ss, wl, NULL, NULL);
    } else {
        r = WSASend((SOCKET)fd, b, (DWORD)m->msg_iovlen, &sent, (DWORD)flags, NULL, NULL);
    }
    free(b);
    if (r != 0) { set_err(); return -1; }
    return (long long)sent;
}
static SYSV long long qx_recvmsg(int fd, struct lx_msghdr *m, int flags)
{
    WSABUF *b;
    DWORD got = 0, wflags = (DWORD)flags;
    int r;
    struct sockaddr_storage ss;
    int sl = sizeof(ss);
    if (!m) { *qx_shim_errno() = LX_EFAULT; return -1; }
    b = iov_to_wsabuf(m->msg_iov, m->msg_iovlen);
    if (m->msg_iovlen && !b) { *qx_shim_errno() = LX_ENOBUFS; return -1; }
    r = WSARecvFrom((SOCKET)fd, b, (DWORD)m->msg_iovlen, &got, &wflags,
                    m->msg_name ? (struct sockaddr *)&ss : NULL,
                    m->msg_name ? &sl : NULL, NULL, NULL);
    free(b);
    if (r != 0) { set_err(); return -1; }
    if (m->msg_name && m->msg_namelen) {
        int out = sl < (int)m->msg_namelen ? sl : (int)m->msg_namelen;
        sa_to_lx(m->msg_name, &ss, out);
        m->msg_namelen = sl;
    }
    m->msg_flags = 0;
    return (long long)got;
}
static SYSV int qx_sendmmsg(int fd, struct lx_mmsghdr *m, unsigned vlen, int flags)
{
    unsigned i;
    for (i = 0; i < vlen; i++) {
        long long s = qx_sendmsg(fd, &m[i].msg_hdr, flags);
        if (s < 0)
            return i ? (int)i : -1; /* partial success returns the count sent */
        m[i].msg_len = (unsigned)s;
    }
    return (int)vlen;
}
static SYSV int qx_recvmmsg(int fd, struct lx_mmsghdr *m, unsigned vlen, int flags, void *timeout)
{
    unsigned i;
    (void)timeout;
    for (i = 0; i < vlen; i++) {
        long long s = qx_recvmsg(fd, &m[i].msg_hdr, flags);
        if (s < 0)
            return i ? (int)i : -1;
        m[i].msg_len = (unsigned)s;
    }
    return (int)vlen;
}

/* ---- socketpair (loopback-emulated; Winsock has no AF_UNIX pre-Win10) ------ */
static SYSV int qx_socketpair(int domain, int type, int protocol, int sv[2])
{
    int t = type & ~(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC);
    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET, lis = INVALID_SOCKET;
    struct sockaddr_in addr;
    int alen = sizeof(addr);
    (void)domain; (void)protocol;
    wsa_ensure();
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (t == SOCK_DGRAM) {
        /* two UDP sockets bound to loopback, each connected to the other */
        struct sockaddr_in aa, bb;
        int al2 = sizeof(aa), bl2 = sizeof(bb);
        a = socket(AF_INET, SOCK_DGRAM, 0);
        b = socket(AF_INET, SOCK_DGRAM, 0);
        if (a == INVALID_SOCKET || b == INVALID_SOCKET) goto fail;
        if (bind(a, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            bind(b, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;
        if (getsockname(a, (struct sockaddr *)&aa, &al2) != 0 ||
            getsockname(b, (struct sockaddr *)&bb, &bl2) != 0) goto fail;
        if (connect(a, (struct sockaddr *)&bb, bl2) != 0 ||
            connect(b, (struct sockaddr *)&aa, al2) != 0) goto fail;
    } else {
        /* a connected TCP pair over the loopback */
        lis = socket(AF_INET, SOCK_STREAM, 0);
        if (lis == INVALID_SOCKET) goto fail;
        if (bind(lis, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(lis, 1) != 0 ||
            getsockname(lis, (struct sockaddr *)&addr, &alen) != 0) goto fail;
        a = socket(AF_INET, SOCK_STREAM, 0);
        if (a == INVALID_SOCKET) goto fail;
        if (connect(a, (struct sockaddr *)&addr, alen) != 0) goto fail;
        b = accept(lis, NULL, NULL);
        if (b == INVALID_SOCKET) goto fail;
        closesocket(lis);
        lis = INVALID_SOCKET;
    }
    sv[0] = (int)a;
    sv[1] = (int)b;
    sock_register((int)a);
    sock_register((int)b);
    return 0;
fail:
    set_err();
    if (a != INVALID_SOCKET) closesocket(a);
    if (b != INVALID_SOCKET) closesocket(b);
    if (lis != INVALID_SOCKET) closesocket(lis);
    return -1;
}

/* ---- getnameinfo / gethostbyname / if_* ----------------------------------- */
/* Linux NI_* (NUMERICHOST=1, NUMERICSERV=2, NOFQDN=4, NAMEREQD=8, DGRAM=16)
 * map to the differently-numbered Windows NI_* bits. */
static int ni_flags_to_win(int f)
{
    int w = 0;
    if (f & 0x01) w |= NI_NUMERICHOST;
    if (f & 0x02) w |= NI_NUMERICSERV;
    if (f & 0x04) w |= NI_NOFQDN;
    if (f & 0x08) w |= NI_NAMEREQD;
    if (f & 0x10) w |= NI_DGRAM;
    return w;
}
static SYSV int qx_getnameinfo(const void *sa, int salen, char *host, unsigned hostlen,
                               char *serv, unsigned servlen, int flags)
{
    struct sockaddr_storage ss;
    int wl = sa_to_win(sa, salen, &ss);
    if (wl < 0)
        return EAI_FAMILY;
    return getnameinfo((struct sockaddr *)&ss, wl, host, hostlen, serv, servlen, ni_flags_to_win(flags));
}
/* gethostbyname: the hostent layout matches Linux for AF_INET (the common
 * case), so forward to Winsock. Winsock keeps the result in thread storage. */
static SYSV void *qx_gethostbyname(const char *name)
{
    void *h;
    wsa_ensure();
    h = gethostbyname(name);
    if (!h)
        set_err();
    return h;
}
static SYSV unsigned qx_if_nametoindex(const char *name) { return if_nametoindex(name); }
static SYSV char *qx_if_indextoname(unsigned idx, char *name) { return if_indextoname(idx, name); }

/* ---- resolver -------------------------------------------------------------- */
struct sock_sym { const char *name; void *fn; };
static const struct sock_sym g_sock_table[] = {
    {"socket", (void *)qx_socket}, {"bind", (void *)qx_bind}, {"connect", (void *)qx_connect},
    {"listen", (void *)qx_listen}, {"accept", (void *)qx_accept}, {"accept4", (void *)qx_accept4},
    {"shutdown", (void *)qx_shutdown}, {"getsockname", (void *)qx_getsockname},
    {"getpeername", (void *)qx_getpeername}, {"sendto", (void *)qx_sendto},
    {"recvfrom", (void *)qx_recvfrom}, {"setsockopt", (void *)qx_setsockopt},
    {"getsockopt", (void *)qx_getsockopt}, {"inet_ntop", (void *)qx_inet_ntop},
    {"inet_pton", (void *)qx_inet_pton}, {"poll", (void *)qx_poll}, {"select", (void *)qx_select},
    {"getaddrinfo", (void *)qx_getaddrinfo}, {"freeaddrinfo", (void *)qx_freeaddrinfo},
    {"gai_strerror", (void *)qx_gai_strerror},
    {"sendmsg", (void *)qx_sendmsg}, {"recvmsg", (void *)qx_recvmsg},
    {"sendmmsg", (void *)qx_sendmmsg}, {"recvmmsg", (void *)qx_recvmmsg},
    {"socketpair", (void *)qx_socketpair}, {"getnameinfo", (void *)qx_getnameinfo},
    {"gethostbyname", (void *)qx_gethostbyname},
    {"if_nametoindex", (void *)qx_if_nametoindex}, {"if_indextoname", (void *)qx_if_indextoname},
};

void *qx_socket_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_sock_table) / sizeof(g_sock_table[0]); i++)
        if (strcmp(name, g_sock_table[i].name) == 0)
            return g_sock_table[i].fn;
    return NULL;
}

/* ---- self-test ------------------------------------------------------------ */
SYSV int qx_socket_selftest(int *total)
{
    int fails = 0, n = 0;

    /* inet_pton/inet_ntop round-trip an IPv4 literal */
    n++;
    {
        unsigned char a[4];
        char out[32];
        if (qx_inet_pton(LX_AF_INET, "127.0.0.1", a) != 1 ||
            a[0] != 127 || a[1] != 0 || a[2] != 0 || a[3] != 1 ||
            !qx_inet_ntop(LX_AF_INET, a, out, sizeof(out)) || strcmp(out, "127.0.0.1") != 0)
            fails++;
    }
    /* getaddrinfo resolves localhost to a Linux-family IPv4 loopback node */
    n++;
    {
        struct lx_addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = LX_AF_INET;
        hints.ai_socktype = 1; /* SOCK_STREAM */
        if (qx_getaddrinfo("127.0.0.1", "0", &hints, &res) != 0 || !res ||
            res->ai_family != LX_AF_INET || res->ai_addr == NULL)
            fails++;
        qx_freeaddrinfo(res);
    }
    /* a TCP loopback round-trip: listen, connect, accept, sendto, recvfrom */
    n++;
    {
        int ls = qx_socket(LX_AF_INET, 1, 0);
        int cs = -1, as = -1;
        /* sockaddr_in: family(2) port(2) addr(4) zero(8); port/addr network order */
        unsigned char sa[16];
        int slen = 16;
        memset(sa, 0, sizeof(sa));
        sa[0] = LX_AF_INET; sa[1] = 0;          /* family = 2, little-endian */
        sa[2] = 0; sa[3] = 0;                    /* port 0 -> kernel picks */
        sa[4] = 127; sa[5] = 0; sa[6] = 0; sa[7] = 1;
        if (ls < 0 || !qx_socket_is_sock(ls)) {
            fails++;
        } else if (qx_bind(ls, sa, slen) != 0 || qx_listen(ls, 1) != 0) {
            fails++;
        } else {
            unsigned char bound[16];
            int blen = sizeof(bound);
            if (qx_getsockname(ls, bound, &blen) != 0) {
                fails++;
            } else {
                /* connect to the bound port */
                unsigned char ca[16];
                char rbuf[8];
                memcpy(ca, sa, 16);
                ca[2] = bound[2]; ca[3] = bound[3]; /* copy the chosen port */
                cs = qx_socket(LX_AF_INET, 1, 0);
                if (cs < 0 || qx_connect(cs, ca, 16) != 0) {
                    fails++;
                } else {
                    as = qx_accept(ls, NULL, NULL);
                    if (as < 0 ||
                        qx_sendto(as, "hey", 3, 0, NULL, 0) != 3) {
                        fails++;
                    } else {
                        long long got = qx_recvfrom(cs, rbuf, sizeof(rbuf), 0, NULL, NULL);
                        if (got != 3 || memcmp(rbuf, "hey", 3) != 0)
                            fails++;
                    }
                }
            }
        }
        if (as >= 0) qx_socket_close(as);
        if (cs >= 0) qx_socket_close(cs);
        if (ls >= 0) qx_socket_close(ls);
        /* the listen socket is no longer registered after close */
        if (ls >= 0 && qx_socket_is_sock(ls))
            fails++;
    }
    /* socketpair + scatter/gather: a two-iovec sendmsg reassembles on recvmsg */
    n++;
    {
        int sp[2] = {-1, -1};
        if (qx_socketpair(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0, sp) != 0) {
            fails++;
        } else {
            char b1[] = "foo", b2[] = "bar", rb[8];
            struct lx_iovec iov[2], riov;
            struct lx_msghdr mh, rmh;
            long long s, r;
            iov[0].iov_base = b1; iov[0].iov_len = 3;
            iov[1].iov_base = b2; iov[1].iov_len = 3;
            memset(&mh, 0, sizeof(mh));
            mh.msg_iov = iov; mh.msg_iovlen = 2;
            s = qx_sendmsg(sp[0], &mh, 0);
            riov.iov_base = rb; riov.iov_len = sizeof(rb);
            memset(&rmh, 0, sizeof(rmh));
            rmh.msg_iov = &riov; rmh.msg_iovlen = 1;
            r = qx_recvmsg(sp[1], &rmh, 0);
            if (s != 6 || r != 6 || memcmp(rb, "foobar", 6) != 0)
                fails++;
            qx_socket_close(sp[0]);
            qx_socket_close(sp[1]);
        }
    }

    if (total)
        *total = n;
    return fails;
}
