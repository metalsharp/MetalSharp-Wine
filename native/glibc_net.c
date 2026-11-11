/* Linux module host, native: glibc's sockets, name resolution and epoll.
 *
 * What differs from macOS: AF_INET6 (10, not 30), the socket address header (Linux has a 16-bit family, macOS a
 * length byte and a family byte), the order of struct addrinfo's fields, struct msghdr and cmsghdr's integer
 * sizes, socket option and message flag numbers, poll's write bits, getaddrinfo's error codes and flags. Every
 * socket gets SO_NOSIGPIPE, for Linux's MSG_NOSIGNAL. epoll is implemented on kqueue. */
#define __APPLE_USE_RFC_3542
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "glibc_internal.h"

int glibc_af_from_linux( int af )
{
    switch (af) { case 0: return AF_UNSPEC; case 1: return AF_UNIX; case 2: return AF_INET; case 10: return AF_INET6; }
    return -1;
}

int glibc_af_to_linux( int af )
{
    switch (af) { case AF_UNSPEC: return 0; case AF_UNIX: return 1; case AF_INET: return 2; case AF_INET6: return 10; }
    return af;
}

/* ---------------------------------------------------------------------------------------------------------
 * Socket addresses
 */

/* a Linux socket address to macOS's; returns its length, or 0 */
static socklen_t addr_from_linux( const void *in, socklen_t len, struct sockaddr_storage *out )
{
    const uint8_t *p = in;
    int af;

    if (!in || len < 2) return 0;
    af = p[0] | (p[1] << 8);
    memset( out, 0, sizeof(*out) );
    switch (af)
    {
    case 2:
        if (len < sizeof(struct sockaddr_in)) return 0;
        memcpy( out, in, sizeof(struct sockaddr_in) );
        out->ss_len = sizeof(struct sockaddr_in);
        out->ss_family = AF_INET;
        return sizeof(struct sockaddr_in);
    case 10:
        if (len < sizeof(struct sockaddr_in6)) return 0;
        memcpy( out, in, sizeof(struct sockaddr_in6) );
        out->ss_len = sizeof(struct sockaddr_in6);
        out->ss_family = AF_INET6;
        return sizeof(struct sockaddr_in6);
    case 1:
    {
        struct sockaddr_un *un = (struct sockaddr_un *)out;
        size_t path_len = len > 2 ? len - 2 : 0;

        if (path_len >= sizeof(un->sun_path)) path_len = sizeof(un->sun_path) - 1;
        memcpy( un->sun_path, p + 2, path_len );
        un->sun_len = sizeof(*un);
        un->sun_family = AF_UNIX;
        return sizeof(*un);
    }
    case 0:
        out->ss_len = 2;
        out->ss_family = AF_UNSPEC;
        return 2;
    }
    return 0;
}

/* a macOS socket address to Linux's, as much as fits; *len gets the full Linux length */
static void addr_to_linux( const struct sockaddr *in, void *out, socklen_t *len )
{
    uint8_t buf[128];
    socklen_t size = 0;

    if (!out || !len) return;
    memset( buf, 0, sizeof(buf) );
    switch (in->sa_family)
    {
    case AF_INET: size = sizeof(struct sockaddr_in); memcpy( buf, in, size ); break;
    case AF_INET6: size = sizeof(struct sockaddr_in6); memcpy( buf, in, size ); break;
    case AF_UNIX:
        size = 110;
        strncpy( (char *)buf + 2, ((const struct sockaddr_un *)in)->sun_path, 107 );
        break;
    default: size = 2; break;
    }
    buf[0] = (uint8_t)glibc_af_to_linux( in->sa_family );
    buf[1] = 0;
    memcpy( out, buf, *len < size ? *len : size );
    *len = size;
}

/* ---------------------------------------------------------------------------------------------------------
 * Sockets
 */

static int lnx_socket( int domain, int type, int protocol )
{
    int af = glibc_af_from_linux( domain ), fd, one = 1;

    if (af < 0) return linux_fail( 97 );  /* EAFNOSUPPORT (netlink, packet sockets) */
    if ((fd = socket( af, type & 0xf, protocol )) == -1) return linux_ret( -1 );
    setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one) );
    if (type & 0x80000) fcntl( fd, F_SETFD, FD_CLOEXEC );
    if (type & 0x800) fcntl( fd, F_SETFL, fcntl( fd, F_GETFL ) | O_NONBLOCK );
    return fd;
}

static int lnx_socketpair( int domain, int type, int protocol, int fds[2] )
{
    int one = 1;

    if (socketpair( glibc_af_from_linux( domain ), type & 0xf, protocol, fds )) return linux_ret( -1 );
    for (int i = 0; i < 2; i++)
    {
        setsockopt( fds[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one) );
        if (type & 0x80000) fcntl( fds[i], F_SETFD, FD_CLOEXEC );
        if (type & 0x800) fcntl( fds[i], F_SETFL, fcntl( fds[i], F_GETFL ) | O_NONBLOCK );
    }
    return 0;
}

static int lnx_bind( int fd, const void *addr, socklen_t len )
{
    struct sockaddr_storage ss;
    socklen_t size = addr_from_linux( addr, len, &ss );

    if (!size) return linux_fail( 22 );
    return linux_ret( bind( fd, (struct sockaddr *)&ss, size ) );
}

static int lnx_connect( int fd, const void *addr, socklen_t len )
{
    struct sockaddr_storage ss;
    socklen_t size = addr_from_linux( addr, len, &ss );

    if (!size) return linux_fail( 22 );
    return linux_ret( connect( fd, (struct sockaddr *)&ss, size ) );
}

static int lnx_listen( int fd, int backlog ) { return linux_ret( listen( fd, backlog ) ); }
static int lnx_shutdown( int fd, int how ) { return linux_ret( shutdown( fd, how ) ); }

static int lnx_accept( int fd, void *addr, socklen_t *len )
{
    struct sockaddr_storage ss;
    socklen_t size = sizeof(ss);
    int ret, one = 1;

    if ((ret = accept( fd, (struct sockaddr *)&ss, &size )) == -1) return linux_ret( -1 );
    setsockopt( ret, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one) );
    addr_to_linux( (struct sockaddr *)&ss, addr, len );
    return ret;
}

static int lnx_getsockname( int fd, void *addr, socklen_t *len )
{
    struct sockaddr_storage ss;
    socklen_t size = sizeof(ss);

    if (getsockname( fd, (struct sockaddr *)&ss, &size )) return linux_ret( -1 );
    addr_to_linux( (struct sockaddr *)&ss, addr, len );
    return 0;
}

static int lnx_getpeername( int fd, void *addr, socklen_t *len )
{
    struct sockaddr_storage ss;
    socklen_t size = sizeof(ss);

    if (getpeername( fd, (struct sockaddr *)&ss, &size )) return linux_ret( -1 );
    addr_to_linux( (struct sockaddr *)&ss, addr, len );
    return 0;
}

static int msg_flags_from_linux( int flags )
{
    int ret = flags & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);

    if (flags & 0x40) ret |= MSG_DONTWAIT;
    if (flags & 0x80) ret |= MSG_EOR;
    if (flags & 0x100) ret |= MSG_WAITALL;
    /* MSG_NOSIGNAL (0x4000): every socket has SO_NOSIGPIPE */
    return ret;
}

static int msg_flags_to_linux( int flags )
{
    int ret = flags & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);

    if (flags & MSG_TRUNC) ret |= 0x20;
    if (flags & MSG_CTRUNC) ret |= 0x8;
    if (flags & MSG_EOR) ret |= 0x80;
    return ret;
}

static long lnx_send( int fd, const void *buf, size_t len, int flags )
{
    return linux_ret( send( fd, buf, len, msg_flags_from_linux( flags ) ) );
}

static long lnx_recv( int fd, void *buf, size_t len, int flags )
{
    return linux_ret( recv( fd, buf, len, msg_flags_from_linux( flags ) ) );
}

static long lnx_sendto( int fd, const void *buf, size_t len, int flags, const void *addr, socklen_t addr_len )
{
    struct sockaddr_storage ss;
    socklen_t size = 0;

    if (addr && !(size = addr_from_linux( addr, addr_len, &ss ))) return linux_fail( 22 );
    return linux_ret( sendto( fd, buf, len, msg_flags_from_linux( flags ), addr ? (struct sockaddr *)&ss : NULL, size ) );
}

static long lnx_recvfrom( int fd, void *buf, size_t len, int flags, void *addr, socklen_t *addr_len )
{
    struct sockaddr_storage ss;
    socklen_t size = sizeof(ss);
    long ret;

    ss.ss_family = AF_UNSPEC;
    if ((ret = recvfrom( fd, buf, len, msg_flags_from_linux( flags ), (struct sockaddr *)&ss, &size )) == -1)
        return linux_ret( -1 );
    if (addr && addr_len) addr_to_linux( (struct sockaddr *)&ss, addr, addr_len );
    return ret;
}

/* glibc's struct msghdr and cmsghdr on aarch64 */
struct lnx_msghdr { void *name; socklen_t namelen; struct iovec *iov; size_t iovlen; void *control; size_t controllen; int flags; };
struct lnx_cmsghdr { size_t len; int level, type; };
struct lnx_mmsghdr { struct lnx_msghdr hdr; unsigned int len; };

#define LNX_CMSG_ALIGN(n) (((n) + 7) & ~(size_t)7)

/* control messages: only the ones a library is likely to ask for are carried over */
static int cmsg_type_to_linux( int level, int type )
{
    if (level == IPPROTO_IP && type == IP_PKTINFO) return 8;
    if (level == IPPROTO_IPV6 && type == IPV6_PKTINFO) return 50;
    if (level == SOL_SOCKET && type == SCM_RIGHTS) return 1;
    if (level == SOL_SOCKET && type == SCM_TIMESTAMP) return 29;
    return -1;
}

static int cmsg_level_to_linux( int level ) { return level == SOL_SOCKET ? 1 : level; }

static void control_to_linux( struct msghdr *msg, struct lnx_msghdr *out )
{
    struct cmsghdr *c;
    size_t used = 0;

    for (c = CMSG_FIRSTHDR( msg ); c; c = CMSG_NXTHDR( msg, c ))
    {
        size_t data_len = c->cmsg_len - CMSG_LEN( 0 ), space = LNX_CMSG_ALIGN( sizeof(struct lnx_cmsghdr) + data_len );
        int type = cmsg_type_to_linux( c->cmsg_level, c->cmsg_type );
        struct lnx_cmsghdr *lc;

        if (type < 0) continue;
        if (used + space > out->controllen)
        {
            out->flags |= 0x8;  /* MSG_CTRUNC */
            break;
        }
        lc = (struct lnx_cmsghdr *)((char *)out->control + used);
        lc->len = sizeof(*lc) + data_len;
        lc->level = cmsg_level_to_linux( c->cmsg_level );
        lc->type = type;
        memcpy( lc + 1, CMSG_DATA( c ), data_len );
        used += space;
    }
    out->controllen = used;
}

static long lnx_recvmsg( int fd, struct lnx_msghdr *lmsg, int flags )
{
    struct sockaddr_storage ss;
    struct msghdr msg = { 0 };
    char control[512];
    long ret;

    msg.msg_name = lmsg->name ? &ss : NULL;
    msg.msg_namelen = lmsg->name ? sizeof(ss) : 0;
    msg.msg_iov = lmsg->iov;
    msg.msg_iovlen = (int)lmsg->iovlen;
    msg.msg_control = lmsg->controllen ? control : NULL;
    msg.msg_controllen = lmsg->controllen ? sizeof(control) : 0;
    if ((ret = recvmsg( fd, &msg, msg_flags_from_linux( flags ) )) == -1) return linux_ret( -1 );
    if (lmsg->name) addr_to_linux( (struct sockaddr *)&ss, lmsg->name, &lmsg->namelen );
    lmsg->flags = msg_flags_to_linux( msg.msg_flags );
    if (lmsg->controllen) control_to_linux( &msg, lmsg );
    return ret;
}

static long lnx_sendmsg( int fd, const struct lnx_msghdr *lmsg, int flags )
{
    struct sockaddr_storage ss;
    struct msghdr msg = { 0 };
    socklen_t size = 0;

    if (lmsg->name && !(size = addr_from_linux( lmsg->name, lmsg->namelen, &ss ))) return linux_fail( 22 );
    if (lmsg->controllen) return linux_fail( 95 );  /* EOPNOTSUPP: control messages on send */
    msg.msg_name = lmsg->name ? &ss : NULL;
    msg.msg_namelen = size;
    msg.msg_iov = lmsg->iov;
    msg.msg_iovlen = (int)lmsg->iovlen;
    return linux_ret( sendmsg( fd, &msg, msg_flags_from_linux( flags ) ) );
}

/* recvmmsg: one recvmsg after another, the later ones not waiting */
static int lnx_recvmmsg( int fd, struct lnx_mmsghdr *msgs, unsigned int count, int flags, void *timeout )
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        long ret = lnx_recvmsg( fd, &msgs[i].hdr, i ? flags | 0x40 : flags );

        if (ret < 0) return i ? (int)i : -1;
        msgs[i].len = (unsigned int)ret;
        if (flags & 0x10000) flags |= 0x40;  /* MSG_WAITFORONE */
    }
    return (int)i;
}

/* ---------------------------------------------------------------------------------------------------------
 * Socket options
 */

static int sockopt_from_linux( int level, int name, int *mac_level, int *mac_name )
{
    *mac_level = level;
    if (level == 1)  /* SOL_SOCKET */
    {
        static const int names[] = { 0, SO_DEBUG, SO_REUSEADDR, SO_TYPE, SO_ERROR, SO_DONTROUTE, SO_BROADCAST,
                                     SO_SNDBUF, SO_RCVBUF, SO_KEEPALIVE, SO_OOBINLINE, 0, 0, SO_LINGER, 0,
                                     SO_REUSEPORT, 0, 0, SO_RCVLOWAT, SO_SNDLOWAT, SO_RCVTIMEO, SO_SNDTIMEO };

        *mac_level = SOL_SOCKET;
        if (name == 29) { *mac_name = SO_TIMESTAMP; return 0; }
        if (name > 0 && name < (int)ARRAY_SIZE(names) && names[name]) { *mac_name = names[name]; return 0; }
        return -1;
    }
    if (level == IPPROTO_IP)
    {
        switch (name)
        {
        case 1: *mac_name = IP_TOS; return 0;
        case 2: *mac_name = IP_TTL; return 0;
        case 3: *mac_name = IP_HDRINCL; return 0;
        case 8: *mac_name = IP_RECVPKTINFO; return 0;
        case 32: *mac_name = IP_MULTICAST_IF; return 0;
        case 33: *mac_name = IP_MULTICAST_TTL; return 0;
        case 34: *mac_name = IP_MULTICAST_LOOP; return 0;
        case 35: *mac_name = IP_ADD_MEMBERSHIP; return 0;
        case 36: *mac_name = IP_DROP_MEMBERSHIP; return 0;
        }
        return -1;
    }
    if (level == IPPROTO_IPV6)
    {
        switch (name)
        {
        case 16: *mac_name = IPV6_UNICAST_HOPS; return 0;
        case 17: *mac_name = IPV6_MULTICAST_IF; return 0;
        case 18: *mac_name = IPV6_MULTICAST_HOPS; return 0;
        case 19: *mac_name = IPV6_MULTICAST_LOOP; return 0;
        case 20: *mac_name = IPV6_JOIN_GROUP; return 0;
        case 21: *mac_name = IPV6_LEAVE_GROUP; return 0;
        case 26: *mac_name = IPV6_V6ONLY; return 0;
        case 49: *mac_name = IPV6_RECVPKTINFO; return 0;
        case 67: *mac_name = IPV6_TCLASS; return 0;
        }
        return -1;
    }
    if (level == IPPROTO_TCP)
    {
        switch (name)
        {
        case 1: *mac_name = TCP_NODELAY; return 0;
        case 2: *mac_name = TCP_MAXSEG; return 0;
        case 4: *mac_name = TCP_KEEPALIVE; return 0;
        case 5: *mac_name = TCP_KEEPINTVL; return 0;
        case 6: *mac_name = TCP_KEEPCNT; return 0;
        }
        return -1;
    }
    return -1;
}

/* options Linux has and macOS does not, which a library sets as hints: accepted and ignored */
static int ignored_option( int level, int name )
{
    return (level == 1 && (name == 12 /* SO_PRIORITY */ || name == 15 /* SO_REUSEPORT on old kernels */)) ||
           (level == IPPROTO_IP && (name == 10 /* IP_MTU_DISCOVER */ || name == 11 /* IP_RECVERR */)) ||
           (level == IPPROTO_IPV6 && (name == 23 /* IPV6_MTU_DISCOVER */ || name == 25 /* IPV6_RECVERR */)) ||
           (level == IPPROTO_TCP && (name == 9 /* TCP_DEFER_ACCEPT */ || name == 12 /* TCP_QUICKACK */ ||
                                     name == 23 /* TCP_FASTOPEN */ || name == 18 /* TCP_USER_TIMEOUT */));
}

static int lnx_setsockopt( int fd, int level, int name, const void *value, socklen_t len )
{
    int mac_level, mac_name;

    if (sockopt_from_linux( level, name, &mac_level, &mac_name ))
        return ignored_option( level, name ) ? 0 : linux_fail( 92 );  /* ENOPROTOOPT */
    return linux_ret( setsockopt( fd, mac_level, mac_name, value, len ) );
}

static int lnx_getsockopt( int fd, int level, int name, void *value, socklen_t *len )
{
    int mac_level, mac_name, ret;

    if (sockopt_from_linux( level, name, &mac_level, &mac_name )) return linux_fail( 92 );
    if ((ret = getsockopt( fd, mac_level, mac_name, value, len ))) return linux_ret( ret );
    if (level == 1 && name == 4 && *len >= sizeof(int)) *(int *)value = glibc_errno_to_linux( *(int *)value );
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------
 * poll, select, epoll
 */

static short poll_from_linux( short events )
{
    short ret = events & (POLLIN | POLLPRI | POLLOUT | POLLRDNORM | POLLRDBAND);

    if (events & 0x100) ret |= POLLWRNORM;
    if (events & 0x200) ret |= POLLWRBAND;
    return ret;
}

static short poll_to_linux( short events )
{
    short ret = events & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL | POLLRDNORM | POLLRDBAND);

    if (events & POLLWRBAND) ret |= 0x200;
    if ((events & POLLWRNORM) && POLLWRNORM != POLLOUT) ret |= 0x100;
    return ret;
}

static int lnx_poll( struct pollfd *fds, nfds_t count, int timeout )
{
    int ret;

    for (nfds_t i = 0; i < count; i++)
    {
        if (fds[i].events & 0x2000) fds[i].events |= POLLIN;  /* POLLRDHUP: reported as POLLHUP or a read */
        fds[i].events = poll_from_linux( fds[i].events );
    }
    ret = poll( fds, count, timeout );
    for (nfds_t i = 0; i < count; i++)
    {
        fds[i].revents = poll_to_linux( fds[i].revents );
        fds[i].events = poll_to_linux( fds[i].events );
    }
    return linux_ret( ret );
}

static int lnx_select( int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout )
{
    return linux_ret( select( nfds, r, w, e, timeout ) );
}

/* glibc's struct epoll_event on aarch64: not packed */
struct lnx_epoll_event { uint32_t events; uint64_t data; };

enum { EPOLLIN = 0x1, EPOLLPRI = 0x2, EPOLLOUT = 0x4, EPOLLERR = 0x8, EPOLLHUP = 0x10, EPOLLRDHUP = 0x2000,
       EPOLLONESHOT = 1u << 30, EPOLLET = 1u << 31 };

static int lnx_epoll_create( int size ) { return linux_ret( kqueue() ); }

static int lnx_epoll_ctl( int epfd, int op, int fd, struct lnx_epoll_event *event )
{
    struct kevent changes[2];
    int n = 0;

    /* every change starts from nothing: both filters off, which fails harmlessly for one never added */
    EV_SET( &changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL );
    EV_SET( &changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL );
    for (int i = 0; i < 2; i++) kevent( epfd, &changes[i], 1, NULL, 0, NULL );
    if (op == 2) return 0;  /* EPOLL_CTL_DEL */
    if (op != 1 && op != 3) return linux_fail( 22 );
    if (!event) return linux_fail( 14 );
    {
        unsigned short flags = EV_ADD | ((event->events & EPOLLET) ? EV_CLEAR : 0) |
                               ((event->events & EPOLLONESHOT) ? EV_ONESHOT : 0);

        if (event->events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP))
            EV_SET( &changes[n++], fd, EVFILT_READ, flags, 0, 0, (void *)event->data );
        if (event->events & EPOLLOUT)
            EV_SET( &changes[n++], fd, EVFILT_WRITE, flags, 0, 0, (void *)event->data );
    }
    if (n && kevent( epfd, changes, n, NULL, 0, NULL ) == -1) return linux_ret( -1 );
    return 0;
}

static int lnx_epoll_wait( int epfd, struct lnx_epoll_event *events, int max, int timeout )
{
    struct kevent results[64];
    struct timespec ts, *tsp = NULL;
    int count, n = 0;

    if (max <= 0) return linux_fail( 22 );
    if (max > 64) max = 64;
    if (timeout >= 0)
    {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
        tsp = &ts;
    }
    if ((count = kevent( epfd, NULL, 0, results, max, tsp )) == -1) return linux_ret( -1 );
    for (int i = 0; i < count; i++)
    {
        uint32_t ev = results[i].filter == EVFILT_READ ? EPOLLIN : EPOLLOUT;

        if (results[i].flags & EV_EOF) ev |= EPOLLHUP | (results[i].filter == EVFILT_READ ? EPOLLRDHUP : 0);
        if (results[i].flags & EV_ERROR) ev = EPOLLERR;
        events[n].events = ev;
        events[n].data = (uint64_t)results[i].udata;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------------------------------------
 * Names and addresses
 */

/* glibc's struct addrinfo: ai_addr comes before ai_canonname */
struct lnx_addrinfo
{
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    void *ai_addr;
    char *ai_canonname;
    struct lnx_addrinfo *ai_next;
};

static int eai_to_linux( int err )
{
    switch (err)
    {
    case 0: return 0;
    case EAI_BADFLAGS: return -1;
    case EAI_NONAME: return -2;
    case EAI_AGAIN: return -3;
    case EAI_FAIL: return -4;
    case EAI_FAMILY: return -6;
    case EAI_SOCKTYPE: return -7;
    case EAI_SERVICE: return -8;
    case EAI_MEMORY: return -10;
    case EAI_OVERFLOW: return -12;
    case EAI_SYSTEM: errno = glibc_errno_to_linux( errno ); return -11;
    default: return -4;
    }
}

static int eai_from_linux( int err )
{
    switch (err)
    {
    case -1: return EAI_BADFLAGS; case -2: return EAI_NONAME; case -3: return EAI_AGAIN; case -4: return EAI_FAIL;
    case -6: return EAI_FAMILY; case -7: return EAI_SOCKTYPE; case -8: return EAI_SERVICE;
    case -10: return EAI_MEMORY; case -11: return EAI_SYSTEM; case -12: return EAI_OVERFLOW;
    default: return EAI_FAIL;
    }
}

static int ai_flags_from_linux( int flags )
{
    int ret = flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);

    if (flags & 0x8) ret |= AI_V4MAPPED;
    if (flags & 0x10) ret |= AI_ALL;
    if (flags & 0x20) ret |= AI_ADDRCONFIG;
    if (flags & 0x400) ret |= AI_NUMERICSERV;
    return ret;
}

static void lnx_freeaddrinfo( struct lnx_addrinfo *ai )
{
    while (ai)
    {
        struct lnx_addrinfo *next = ai->ai_next;

        free( ai->ai_canonname );
        free( ai );  /* the address is in the same block */
        ai = next;
    }
}

static int lnx_getaddrinfo( const char *node, const char *service, const struct lnx_addrinfo *lhints,
                            struct lnx_addrinfo **result )
{
    struct addrinfo hints = { 0 }, *res, *ai;
    struct lnx_addrinfo *head = NULL, **tail = &head;
    int ret;

    if (lhints)
    {
        hints.ai_flags = ai_flags_from_linux( lhints->ai_flags );
        if ((hints.ai_family = glibc_af_from_linux( lhints->ai_family )) < 0) return -6;  /* EAI_FAMILY */
        hints.ai_socktype = lhints->ai_socktype;
        hints.ai_protocol = lhints->ai_protocol;
    }
    if ((ret = getaddrinfo( node, service, lhints ? &hints : NULL, &res ))) return eai_to_linux( ret );
    for (ai = res; ai; ai = ai->ai_next)
    {
        struct lnx_addrinfo *out;
        socklen_t len = 128;

        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        if (!(out = calloc( 1, sizeof(*out) + 128 )))
        {
            freeaddrinfo( res );
            lnx_freeaddrinfo( head );
            return -10;
        }
        out->ai_flags = lhints ? lhints->ai_flags : 0;
        out->ai_family = glibc_af_to_linux( ai->ai_family );
        out->ai_socktype = ai->ai_socktype;
        out->ai_protocol = ai->ai_protocol;
        out->ai_addr = out + 1;
        addr_to_linux( ai->ai_addr, out->ai_addr, &len );
        out->ai_addrlen = len;
        if (ai->ai_canonname) out->ai_canonname = strdup( ai->ai_canonname );
        *tail = out;
        tail = &out->ai_next;
    }
    freeaddrinfo( res );
    *result = head;
    return head ? 0 : -2;
}

static const char *lnx_gai_strerror( int err ) { return gai_strerror( eai_from_linux( err ) ); }

static int lnx_getnameinfo( const void *addr, socklen_t len, char *host, socklen_t host_len, char *serv,
                            socklen_t serv_len, int flags )
{
    struct sockaddr_storage ss;
    socklen_t size = addr_from_linux( addr, len, &ss );
    int mac_flags = 0;

    if (!size) return -6;
    if (flags & 1) mac_flags |= NI_NUMERICHOST;
    if (flags & 2) mac_flags |= NI_NUMERICSERV;
    if (flags & 4) mac_flags |= NI_NOFQDN;
    if (flags & 8) mac_flags |= NI_NAMEREQD;
    if (flags & 16) mac_flags |= NI_DGRAM;
    return eai_to_linux( getnameinfo( (struct sockaddr *)&ss, size, host, host_len, serv, serv_len, mac_flags ) );
}

static const char *lnx_inet_ntop( int af, const void *src, char *dst, socklen_t size )
{
    const char *ret = inet_ntop( glibc_af_from_linux( af ), src, dst, size );

    if (!ret) errno = glibc_errno_to_linux( errno );
    return ret;
}

static int lnx_inet_pton( int af, const char *src, void *dst )
{
    int ret = inet_pton( glibc_af_from_linux( af ), src, dst );

    if (ret < 0) errno = 97;
    return ret;
}

/* glibc's struct ifaddrs has macOS's field order; the addresses are converted, in one block per entry */
struct lnx_ifaddrs
{
    struct lnx_ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    void *ifa_addr, *ifa_netmask, *ifa_ifu, *ifa_data;
};

static void lnx_freeifaddrs( struct lnx_ifaddrs *ifa )
{
    while (ifa)
    {
        struct lnx_ifaddrs *next = ifa->ifa_next;

        free( ifa );
        ifa = next;
    }
}

static int lnx_getifaddrs( struct lnx_ifaddrs **result )
{
    struct ifaddrs *list, *ifa;
    struct lnx_ifaddrs *head = NULL, **tail = &head;

    if (getifaddrs( &list )) return linux_ret( -1 );
    for (ifa = list; ifa; ifa = ifa->ifa_next)
    {
        struct lnx_ifaddrs *out;
        size_t name_len = strlen( ifa->ifa_name ) + 1;
        char *p;
        socklen_t len;

        if (ifa->ifa_addr && ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
            continue;  /* link-level entries: Linux lists AF_PACKET ones, which nothing here can open */
        if (!(out = calloc( 1, sizeof(*out) + 3 * 128 + name_len )))
        {
            freeifaddrs( list );
            lnx_freeifaddrs( head );
            return linux_fail( 12 );
        }
        p = (char *)(out + 1);
        out->ifa_flags = ifa->ifa_flags;
        if (ifa->ifa_addr) { len = 128; addr_to_linux( ifa->ifa_addr, p, &len ); out->ifa_addr = p; }
        if (ifa->ifa_netmask) { len = 128; addr_to_linux( ifa->ifa_netmask, p + 128, &len ); out->ifa_netmask = p + 128; }
        if (ifa->ifa_dstaddr) { len = 128; addr_to_linux( ifa->ifa_dstaddr, p + 256, &len ); out->ifa_ifu = p + 256; }
        out->ifa_name = p + 384;
        memcpy( out->ifa_name, ifa->ifa_name, name_len );
        *tail = out;
        tail = &out->ifa_next;
    }
    freeifaddrs( list );
    *result = head;
    return 0;
}

const struct glibc_export glibc_net_exports[] =
{
    WRAP( socket ), WRAP( socketpair ), WRAP( bind ), WRAP( connect ), WRAP( listen ), WRAP( shutdown ),
    WRAP( accept ), WRAP( getsockname ), WRAP( getpeername ), WRAP( send ), WRAP( recv ), WRAP( sendto ),
    WRAP( recvfrom ), WRAP( recvmsg ), WRAP( sendmsg ), WRAP( recvmmsg ), WRAP( setsockopt ), WRAP( getsockopt ),
    WRAP( poll ), WRAP( select ), WRAP( epoll_create ), WRAP( epoll_ctl ), WRAP( epoll_wait ),
    WRAP( getaddrinfo ), WRAP( freeaddrinfo ), WRAP( gai_strerror ), WRAP( getnameinfo ), WRAP( inet_ntop ),
    WRAP( inet_pton ), WRAP( getifaddrs ), WRAP( freeifaddrs ), PASS( gethostbyname ), PASS( if_nametoindex ),
    END
};
