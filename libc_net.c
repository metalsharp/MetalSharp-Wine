/* Linux module host: glibc's sockets, poll, select and epoll, on Winsock.
 *
 * A Linux socket is a Winsock socket in the host's descriptor table. Linux and Winsock agree on socket address
 * layouts (a 16-bit family first) except for AF_INET6's number (10, Winsock 23); on socket types; and on IPPROTO_TCP's
 * options. They differ on SOL_SOCKET (1, Winsock 0xffff) and its option numbers, the receive and send timeouts (a
 * struct timeval, Winsock milliseconds), error numbers, poll's bits and struct addrinfo's field order and sizes. epoll
 * is a set of registrations polled together, its struct epoll_event packed on x86_64. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "elfload.h"
#include "libc.h"
#include "process.h"

typedef unsigned long long lx_size_t;
typedef long long lx_long;
typedef unsigned int lx_socklen_t;

static lx_long fail( int err )
{
    *linuxhost_errno() = err;
    return -1;
}

static int errno_from_wsa( int err )
{
    switch (err)
    {
    case WSAEWOULDBLOCK: return 11;       /* EAGAIN */
    case WSAEINPROGRESS: return 115;      /* EINPROGRESS */
    case WSAEALREADY: return 114;
    case WSAENOTSOCK: return 88;
    case WSAEDESTADDRREQ: return 89;
    case WSAEMSGSIZE: return 90;
    case WSAEPROTOTYPE: return 91;
    case WSAENOPROTOOPT: return 92;
    case WSAEPROTONOSUPPORT: return 93;
    case WSAEOPNOTSUPP: return 95;
    case WSAEAFNOSUPPORT: return 97;
    case WSAEADDRINUSE: return 98;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;
    case WSAECONNABORTED: return 103;
    case WSAECONNRESET: return 104;
    case WSAENOBUFS: return 105;
    case WSAEISCONN: return 106;
    case WSAENOTCONN: return 107;
    case WSAESHUTDOWN: return 108;
    case WSAETIMEDOUT: return 110;
    case WSAECONNREFUSED: return 111;
    case WSAEHOSTUNREACH: return 113;
    case WSAEINVAL: return 22;
    case WSAEBADF: return 9;
    case WSAEACCES: return 13;
    case WSAEFAULT: return 14;
    case WSAEINTR: return 4;
    case WSAEMFILE: return 24;
    default: return 5;
    }
}

static lx_long wsa_fail(void) { return fail( errno_from_wsa( WSAGetLastError() ) ); }

static void init_winsock(void)
{
    static LONG done;
    WSADATA data;

    if (!InterlockedExchange( &done, 1 )) WSAStartup( MAKEWORD( 2, 2 ), &data );
}

static SOCKET get_socket( int fd )
{
    HANDLE handle;

    if (linuxhost_fd_kind( fd, &handle ) != LINUXHOST_FD_SOCKET) return INVALID_SOCKET;
    return (SOCKET)(UINT_PTR)handle;
}

#define GET_SOCKET( s, fd ) SOCKET s = get_socket( fd ); if (s == INVALID_SOCKET) return fail( 88 )  /* ENOTSOCK */

/* ---- addresses ---- */

static int family_from_linux( int af ) { return af == 10 ? AF_INET6 : af; }
static int family_to_linux( int af ) { return af == AF_INET6 ? 10 : af; }

/* Linux's socket address in Winsock's: only AF_INET6's family number differs */
static int addr_from_linux( const void *in, lx_socklen_t len, struct sockaddr_storage *out )
{
    if (!in || len < 2 || len > sizeof(*out)) return 0;
    memcpy( out, in, len );
    out->ss_family = family_from_linux( *(const unsigned short *)in );
    return len;
}

static void addr_to_linux( const struct sockaddr_storage *in, int in_len, void *out, lx_socklen_t *out_len )
{
    struct sockaddr_storage tmp;

    if (!out || !out_len) return;
    tmp = *in;
    tmp.ss_family = family_to_linux( in->ss_family );
    memcpy( out, &tmp, min( (int)*out_len, in_len ) );
    *out_len = in_len;
}

/* ---- sockets ---- */

static SYSV int lx_socket( int domain, int type, int protocol )
{
    SOCKET s;
    int fd;

    init_winsock();
    if ((s = socket( family_from_linux( domain ), type & 0xf, protocol )) == INVALID_SOCKET) return wsa_fail();
    if (type & 0x800)  /* SOCK_NONBLOCK */
    {
        u_long one = 1;
        ioctlsocket( s, FIONBIO, &one );
    }
    if ((fd = linuxhost_new_fd( LINUXHOST_FD_SOCKET, (HANDLE)(UINT_PTR)s, "socket" )) < 0)
    {
        closesocket( s );
        return fail( -fd );
    }
    linuxhost_trace( "socket %d %d %d: %d\n", domain, type, protocol, fd );
    return fd;
}

void linuxhost_socket_close( UINT_PTR s ) { closesocket( (SOCKET)s ); }

long long linuxhost_socket_io( UINT_PTR s, void *buf, size_t size, int write )
{
    int ret = write ? send( (SOCKET)s, buf, (int)min( size, 0x7fffffff ), 0 )
                    : recv( (SOCKET)s, buf, (int)min( size, 0x7fffffff ), 0 );

    if (ret == SOCKET_ERROR) return -errno_from_wsa( WSAGetLastError() );
    return ret;
}

static SYSV int lx_bind( int fd, const void *addr, lx_socklen_t len )
{
    struct sockaddr_storage ss;
    GET_SOCKET( s, fd );

    if (!addr_from_linux( addr, len, &ss )) return fail( 22 );
    return bind( s, (struct sockaddr *)&ss, len ) ? wsa_fail() : 0;
}

static SYSV int lx_connect( int fd, const void *addr, lx_socklen_t len )
{
    struct sockaddr_storage ss;
    GET_SOCKET( s, fd );

    if (!addr_from_linux( addr, len, &ss )) return fail( 22 );
    if (connect( s, (struct sockaddr *)&ss, len ))
    {
        int err = WSAGetLastError();

        return fail( err == WSAEWOULDBLOCK ? 115 : errno_from_wsa( err ) );  /* a non-blocking connect: EINPROGRESS */
    }
    return 0;
}

static SYSV int lx_listen( int fd, int backlog ) { GET_SOCKET( s, fd ); return listen( s, backlog ) ? wsa_fail() : 0; }
static SYSV int lx_shutdown( int fd, int how ) { GET_SOCKET( s, fd ); return shutdown( s, how ) ? wsa_fail() : 0; }

static SYSV int lx_accept( int fd, void *addr, lx_socklen_t *len )
{
    struct sockaddr_storage ss;
    int ss_len = sizeof(ss), newfd;
    SOCKET n;
    GET_SOCKET( s, fd );

    if ((n = accept( s, (struct sockaddr *)&ss, &ss_len )) == INVALID_SOCKET) return wsa_fail();
    if ((newfd = linuxhost_new_fd( LINUXHOST_FD_SOCKET, (HANDLE)(UINT_PTR)n, "socket" )) < 0)
    {
        closesocket( n );
        return fail( -newfd );
    }
    addr_to_linux( &ss, ss_len, addr, len );
    return newfd;
}

static SYSV int lx_getsockname( int fd, void *addr, lx_socklen_t *len )
{
    struct sockaddr_storage ss;
    int ss_len = sizeof(ss);
    GET_SOCKET( s, fd );

    if (getsockname( s, (struct sockaddr *)&ss, &ss_len )) return wsa_fail();
    addr_to_linux( &ss, ss_len, addr, len );
    return 0;
}

static int msg_flags( int flags )
{
    return flags & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);  /* MSG_NOSIGNAL and MSG_DONTWAIT have no Winsock flag */
}

static SYSV lx_long lx_send( int fd, const void *buf, lx_size_t len, int flags )
{
    int ret;
    GET_SOCKET( s, fd );

    if ((ret = send( s, buf, (int)min( len, 0x7fffffff ), msg_flags( flags ) )) == SOCKET_ERROR) return wsa_fail();
    return ret;
}

static SYSV lx_long lx_recv( int fd, void *buf, lx_size_t len, int flags )
{
    int ret;
    GET_SOCKET( s, fd );

    if ((ret = recv( s, buf, (int)min( len, 0x7fffffff ), msg_flags( flags ) )) == SOCKET_ERROR) return wsa_fail();
    return ret;
}

static SYSV lx_long lx_recvfrom( int fd, void *buf, lx_size_t len, int flags, void *addr, lx_socklen_t *addr_len )
{
    struct sockaddr_storage ss;
    int ss_len = sizeof(ss), ret;
    GET_SOCKET( s, fd );

    if ((ret = recvfrom( s, buf, (int)min( len, 0x7fffffff ), msg_flags( flags ), (struct sockaddr *)&ss,
                         &ss_len )) == SOCKET_ERROR)
        return wsa_fail();
    if (addr && addr_len) addr_to_linux( &ss, ss_len, addr, addr_len );
    return ret;
}

/* ---- options ---- */

static int sockopt_from_linux( int level, int name, int *wlevel, int *wname )
{
    static const int names[] = { 0, SO_DEBUG, SO_REUSEADDR, SO_TYPE, SO_ERROR, SO_DONTROUTE, SO_BROADCAST, SO_SNDBUF,
                                 SO_RCVBUF, SO_KEEPALIVE, SO_OOBINLINE, 0, 0, SO_LINGER, 0, 0, 0, 0, SO_RCVLOWAT,
                                 SO_SNDLOWAT, SO_RCVTIMEO, SO_SNDTIMEO };

    *wlevel = level;
    *wname = name;
    if (level != 1) return level == IPPROTO_TCP || level == IPPROTO_IP || level == IPPROTO_IPV6;
    *wlevel = SOL_SOCKET;
    if (name <= 0 || name >= (int)ARRAY_SIZE(names) || !names[name]) return 0;
    *wname = names[name];
    return 1;
}

struct lx_timeval { long long sec, usec; };

static SYSV int lx_setsockopt( int fd, int level, int name, const void *value, lx_socklen_t len )
{
    int wlevel, wname;
    GET_SOCKET( s, fd );

    if (!sockopt_from_linux( level, name, &wlevel, &wname ))
    {
        linuxhost_trace( "setsockopt %d %d: ignored\n", level, name );
        return 0;  /* an option Winsock lacks (SO_PRIORITY, IP_RECVERR...): a hint, which Linux may ignore too */
    }
    if (wlevel == SOL_SOCKET && (wname == SO_RCVTIMEO || wname == SO_SNDTIMEO) && len >= sizeof(struct lx_timeval))
    {
        const struct lx_timeval *tv = value;
        DWORD ms = (DWORD)(tv->sec * 1000 + tv->usec / 1000);

        return setsockopt( s, wlevel, wname, (const char *)&ms, sizeof(ms) ) ? wsa_fail() : 0;
    }
    return setsockopt( s, wlevel, wname, value, len ) ? wsa_fail() : 0;
}

static SYSV int lx_getsockopt( int fd, int level, int name, void *value, lx_socklen_t *len )
{
    int wlevel, wname, wlen = *len;
    GET_SOCKET( s, fd );

    if (!sockopt_from_linux( level, name, &wlevel, &wname )) return fail( 92 );  /* ENOPROTOOPT */
    if (wlevel == SOL_SOCKET && (wname == SO_RCVTIMEO || wname == SO_SNDTIMEO) && *len >= sizeof(struct lx_timeval))
    {
        DWORD ms = 0;
        int ms_len = sizeof(ms);
        struct lx_timeval *tv = value;

        if (getsockopt( s, wlevel, wname, (char *)&ms, &ms_len )) return wsa_fail();
        tv->sec = ms / 1000;
        tv->usec = (ms % 1000) * 1000;
        *len = sizeof(*tv);
        return 0;
    }
    if (getsockopt( s, wlevel, wname, value, &wlen )) return wsa_fail();
    *len = wlen;
    if (wlevel == SOL_SOCKET && wname == SO_ERROR && wlen >= 4 && *(int *)value)
        *(int *)value = errno_from_wsa( *(int *)value );
    return 0;
}

/* ---- names ---- */

/* glibc's struct addrinfo: ai_addrlen is 32 bits and ai_addr comes before ai_canonname */
struct lx_addrinfo
{
    int flags, family, socktype, protocol;
    lx_socklen_t addrlen;
    void *addr;
    char *canonname;
    struct lx_addrinfo *next;
};

static SYSV void lx_freeaddrinfo( struct lx_addrinfo *ai )
{
    while (ai)
    {
        struct lx_addrinfo *next = ai->next;

        free( ai->canonname );
        free( ai );
        ai = next;
    }
}

static int eai_to_linux( int err )
{
    switch (err)
    {
    case 0: return 0;
    case WSAHOST_NOT_FOUND: case WSANO_DATA: return -2;  /* EAI_NONAME */
    case WSATRY_AGAIN: return -3;                        /* EAI_AGAIN */
    case WSAEAFNOSUPPORT: return -6;                     /* EAI_FAMILY */
    case WSAESOCKTNOSUPPORT: return -7;                  /* EAI_SOCKTYPE */
    case WSATYPE_NOT_FOUND: return -8;                   /* EAI_SERVICE */
    case WSA_NOT_ENOUGH_MEMORY: return -10;              /* EAI_MEMORY */
    default: return -4;                                  /* EAI_FAIL */
    }
}

static SYSV int lx_getaddrinfo( const char *node, const char *service, const struct lx_addrinfo *lhints,
                                struct lx_addrinfo **result )
{
    struct addrinfo hints = { 0 }, *res, *ai;
    struct lx_addrinfo *head = NULL, **tail = &head;
    int ret;

    init_winsock();
    if (lhints)
    {
        hints.ai_flags = lhints->flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
        hints.ai_family = family_from_linux( lhints->family );
        hints.ai_socktype = lhints->socktype;
        hints.ai_protocol = lhints->protocol;
    }
    if ((ret = getaddrinfo( node, service, lhints ? &hints : NULL, &res ))) return eai_to_linux( ret );
    for (ai = res; ai; ai = ai->ai_next)
    {
        struct lx_addrinfo *out = calloc( 1, sizeof(*out) + ai->ai_addrlen );
        lx_socklen_t len = ai->ai_addrlen;

        if (!out) break;
        out->flags = ai->ai_flags;
        out->family = family_to_linux( ai->ai_family );
        out->socktype = ai->ai_socktype;
        out->protocol = ai->ai_protocol;
        out->addr = out + 1;
        addr_to_linux( (struct sockaddr_storage *)ai->ai_addr, ai->ai_addrlen, out->addr, &len );
        out->addrlen = len;
        if (ai->ai_canonname) out->canonname = _strdup( ai->ai_canonname );
        *tail = out;
        tail = &out->next;
    }
    freeaddrinfo( res );
    *result = head;
    linuxhost_trace( "getaddrinfo %s %s: %s\n", node ? node : "-", service ? service : "-", head ? "ok" : "none" );
    return head ? 0 : -2;
}

/* ---- poll, select, epoll ---- */

struct lx_pollfd { int fd; short events, revents; };

/* Linux's poll bits from and to WSAPoll's */
static short poll_from_linux( short e ) { return ((e & 1) ? POLLRDNORM : 0) | ((e & 2) ? POLLPRI : 0) | ((e & 4) ? POLLWRNORM : 0); }
static short poll_to_linux( short e )
{
    return ((e & (POLLRDNORM | POLLRDBAND)) ? 1 : 0) | ((e & POLLPRI) ? 2 : 0) | ((e & POLLWRNORM) ? 4 : 0) |
           ((e & POLLERR) ? 8 : 0) | ((e & POLLHUP) ? 0x10 : 0) | ((e & POLLNVAL) ? 0x20 : 0);
}

/* Sockets are polled with WSAPoll; anything else is a file or an in-memory file, always ready */
static int poll_fds( struct lx_pollfd *fds, unsigned long count, int timeout )
{
    WSAPOLLFD wfds[256];
    unsigned long i, nsock = 0, map[256];
    int ready = 0, ret;

    for (i = 0; i < count; i++)
    {
        SOCKET s = get_socket( fds[i].fd );

        fds[i].revents = 0;
        if (s != INVALID_SOCKET && nsock < ARRAY_SIZE(wfds))
        {
            wfds[nsock].fd = s;
            wfds[nsock].events = poll_from_linux( fds[i].events );
            wfds[nsock].revents = 0;
            map[nsock++] = i;
        }
        else if (fds[i].fd >= 0)
        {
            fds[i].revents = linuxhost_fd_kind( fds[i].fd, NULL ) ? (fds[i].events & (1 | 4)) : 0x20;  /* POLLNVAL */
            if (fds[i].revents) ready++;
        }
    }
    if (!nsock) return ready;
    if ((ret = WSAPoll( wfds, nsock, ready ? 0 : timeout )) == SOCKET_ERROR) return -1;
    for (i = 0; i < nsock; i++)
    {
        fds[map[i]].revents = poll_to_linux( wfds[i].revents );
        if (fds[map[i]].revents) ready++;
    }
    return ready;
}

static SYSV int lx_poll( struct lx_pollfd *fds, unsigned long count, int timeout )
{
    int ret;

    init_winsock();
    if ((ret = poll_fds( fds, count, timeout )) < 0) return wsa_fail();
    return ret;
}

/* glibc's fd_set: 1024 bits */
struct lx_fd_set { unsigned long long bits[16]; };

static SYSV int lx_select( int nfds, struct lx_fd_set *r, struct lx_fd_set *w, struct lx_fd_set *e,
                           struct lx_timeval *tv )
{
    struct lx_pollfd fds[256];
    unsigned long count = 0;
    int fd, ret;

    for (fd = 0; fd < nfds && count < ARRAY_SIZE(fds); fd++)
    {
        short events = 0;

        if (r && (r->bits[fd / 64] >> (fd % 64) & 1)) events |= 1;
        if (w && (w->bits[fd / 64] >> (fd % 64) & 1)) events |= 4;
        if (e && (e->bits[fd / 64] >> (fd % 64) & 1)) events |= 2;
        if (!events) continue;
        fds[count].fd = fd;
        fds[count].events = events;
        count++;
    }
    init_winsock();
    if ((ret = poll_fds( fds, count, tv ? (int)(tv->sec * 1000 + tv->usec / 1000) : -1 )) < 0) return wsa_fail();
    if (r) memset( r, 0, sizeof(*r) );
    if (w) memset( w, 0, sizeof(*w) );
    if (e) memset( e, 0, sizeof(*e) );
    ret = 0;
    for (unsigned long i = 0; i < count; i++)
    {
        fd = fds[i].fd;
        if (r && (fds[i].revents & (1 | 0x10 | 8))) { r->bits[fd / 64] |= 1ull << (fd % 64); ret++; }
        if (w && (fds[i].revents & 4)) { w->bits[fd / 64] |= 1ull << (fd % 64); ret++; }
        if (e && (fds[i].revents & 2)) { e->bits[fd / 64] |= 1ull << (fd % 64); ret++; }
    }
    return ret;
}

/* glibc's struct epoll_event is packed on x86_64 */
#pragma pack(push, 4)
struct lx_epoll_event { unsigned int events; unsigned long long data; };
#pragma pack(pop)

struct epoll_set { int count; struct { int fd; struct lx_epoll_event ev; } items[128]; };

void linuxhost_epoll_free( void *set ) { free( set ); }

static SYSV int lx_epoll_create( int size )
{
    int fd = linuxhost_new_fd( LINUXHOST_FD_EPOLL, NULL, "epoll" );
    void **priv;

    if (fd < 0) return fail( -fd );
    priv = linuxhost_fd_private( fd );
    *priv = calloc( 1, sizeof(struct epoll_set) );
    return fd;
}

static SYSV int lx_epoll_ctl( int epfd, int op, int fd, struct lx_epoll_event *event )
{
    struct epoll_set *set;
    void **priv;
    int i;

    if (linuxhost_fd_kind( epfd, NULL ) != LINUXHOST_FD_EPOLL || !(priv = linuxhost_fd_private( epfd ))) return fail( 9 );
    set = *priv;
    for (i = 0; i < set->count; i++) if (set->items[i].fd == fd) break;
    switch (op)
    {
    case 1:  /* EPOLL_CTL_ADD */
        if (i < set->count) return fail( 17 );  /* EEXIST */
        if (set->count == (int)ARRAY_SIZE(set->items)) return fail( 28 );
        set->items[set->count].fd = fd;
        set->items[set->count].ev = *event;
        set->count++;
        return 0;
    case 2:  /* EPOLL_CTL_DEL */
        if (i == set->count) return fail( 2 );
        set->items[i] = set->items[--set->count];
        return 0;
    case 3:  /* EPOLL_CTL_MOD */
        if (i == set->count) return fail( 2 );
        set->items[i].ev = *event;
        return 0;
    }
    return fail( 22 );
}

static SYSV int lx_epoll_wait( int epfd, struct lx_epoll_event *events, int max, int timeout )
{
    struct lx_pollfd fds[128];
    struct epoll_set *set;
    void **priv;
    int i, n = 0, ret;

    if (linuxhost_fd_kind( epfd, NULL ) != LINUXHOST_FD_EPOLL || !(priv = linuxhost_fd_private( epfd ))) return fail( 9 );
    set = *priv;
    for (i = 0; i < set->count; i++)
    {
        fds[i].fd = set->items[i].fd;
        fds[i].events = set->items[i].ev.events & 7;
    }
    init_winsock();
    if ((ret = poll_fds( fds, set->count, timeout )) < 0) return wsa_fail();
    for (i = 0; i < set->count && n < max; i++)
    {
        if (!fds[i].revents) continue;
        events[n].events = fds[i].revents;
        events[n].data = set->items[i].ev.data;
        n++;
    }
    return n;
}

/* ioctl on sockets: FIONREAD and FIONBIO; a terminal request gets ENOTTY */
static SYSV int lx_ioctl( int fd, unsigned long long request, void *arg )
{
    SOCKET s = get_socket( fd );
    u_long value;

    if (s != INVALID_SOCKET && request == 0x541b)  /* FIONREAD */
    {
        if (ioctlsocket( s, FIONREAD, &value )) return wsa_fail();
        *(int *)arg = value;
        return 0;
    }
    if (s != INVALID_SOCKET && request == 0x5421)  /* FIONBIO */
    {
        value = *(int *)arg;
        return ioctlsocket( s, FIONBIO, &value ) ? wsa_fail() : 0;
    }
    return fail( 25 );  /* ENOTTY */
}

/* fcntl's O_NONBLOCK on a socket (libc_io.c handles the rest) */
int linuxhost_socket_set_nonblock( int fd, int on )
{
    SOCKET s = get_socket( fd );
    u_long value = on;

    if (s == INVALID_SOCKET) return 0;
    ioctlsocket( s, FIONBIO, &value );
    return 1;
}

const struct libc_function linuxhost_net_functions[] =
{
    { "socket", lx_socket }, { "bind", lx_bind }, { "connect", lx_connect }, { "listen", lx_listen },
    { "shutdown", lx_shutdown }, { "accept", lx_accept }, { "getsockname", lx_getsockname }, { "send", lx_send },
    { "recv", lx_recv }, { "recvfrom", lx_recvfrom }, { "setsockopt", lx_setsockopt }, { "getsockopt", lx_getsockopt },
    { "getaddrinfo", lx_getaddrinfo }, { "freeaddrinfo", lx_freeaddrinfo }, { "poll", lx_poll },
    { "select", lx_select }, { "epoll_create", lx_epoll_create }, { "epoll_ctl", lx_epoll_ctl },
    { "epoll_wait", lx_epoll_wait }, { "ioctl", lx_ioctl },
    { NULL, NULL }
};
