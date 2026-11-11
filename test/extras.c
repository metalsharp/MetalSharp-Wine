/* A Linux x86_64 shared library testing the module host's unwinder and the rest of its glibc. Built with no C
 * library, as hello.c is. Its exceptions are C ones (cleanup attributes), with its own personality routine: the
 * unwinder's job is the same as for C++, which brings its personality routine with it. */
typedef unsigned long size_t;
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;

/* ---- the unwinder ---- */

struct exception
{
    uint64_t exception_class;
    void (*cleanup)( int, struct exception * );
    uint64_t private_1, private_2;
} __attribute__((aligned(16)));

extern int _Unwind_RaiseException( struct exception *exc );
extern void _Unwind_Resume( struct exception *exc );
extern int _Unwind_Backtrace( int (*fn)( void *context, void *arg ), void *arg );
extern uint64_t _Unwind_GetIP( void *context );
extern uint64_t _Unwind_GetRegionStart( void *context );
extern const unsigned char *_Unwind_GetLanguageSpecificData( void *context );
extern void _Unwind_SetIP( void *context, uint64_t ip );
extern void _Unwind_SetGR( void *context, int reg, uint64_t value );

static uint64_t uleb( const unsigned char **p )
{
    uint64_t v = 0;
    int shift = 0;
    unsigned char b;

    do { b = *(*p)++; v |= (uint64_t)(b & 0x7f) << shift; shift += 7; } while (b & 0x80);
    return v;
}

long unwind_test( void );

/* The handler is unwind_test's frame; a landing pad anywhere is a cleanup. The LSDA is clang's: no landing pad base,
 * no type table, uleb128 call sites. */
__attribute__((visibility("hidden")))
int __gcc_personality_v0( int version, int actions, uint64_t exception_class, struct exception *exc, void *context )
{
    const unsigned char *p = _Unwind_GetLanguageSpecificData( context );
    uint64_t start = _Unwind_GetRegionStart( context ), ip = _Unwind_GetIP( context ) - 1 - start;
    uint64_t table_len, landing = 0;
    const unsigned char *end;

    if (actions & 1) return start == (uint64_t)unwind_test ? 6 : 8;  /* search: HANDLER_FOUND or CONTINUE_UNWIND */
    if (!p) return 8;
    if (*p++ != 0xff) return 8;
    if (*p++ != 0xff) uleb( &p );
    if (*p++ != 0x01) return 8;
    table_len = uleb( &p );
    for (end = p + table_len; p < end; )
    {
        uint64_t cs_start = uleb( &p ), cs_len = uleb( &p ), cs_lp = uleb( &p );

        uleb( &p );
        if (ip >= cs_start && ip < cs_start + cs_len) { landing = cs_lp; break; }
    }
    if (!landing) return 8;
    _Unwind_SetGR( context, 0, (uint64_t)exc );
    _Unwind_SetGR( context, 1, 0 );
    _Unwind_SetIP( context, start + landing );
    return 7;  /* INSTALL_CONTEXT */
}

static void *jump_buffer[5];
static int cleaned, caught, raise_returned;

static void middle_cleanup( int *value ) { cleaned += *value; }
static void catch_cleanup( int *value ) { caught = *value; __builtin_longjmp( jump_buffer, 1 ); }

static struct exception exc = { 0x4c4e58484f535400 };

__attribute__((noinline)) static void thrower( void )
{
    _Unwind_RaiseException( &exc );
    raise_returned = 1;
}

__attribute__((noinline)) static void middle( int v )
{
    int x __attribute__((cleanup(middle_cleanup))) = v;

    thrower();
    __asm__ volatile( "" ::: "memory" );
}

/* the exception passes middle (its cleanup adds 7) to here (whose cleanup records 42 and jumps out); the values held
 * in callee-saved registers across it survive: 7042 + 3 */
long unwind_test( void )
{
    volatile long kept = 3;

    if (__builtin_setjmp( jump_buffer )) return raise_returned ? -2 : cleaned * 1000 + caught + kept;
    {
        int y __attribute__((cleanup(catch_cleanup))) = 42;

        middle( 7 );
    }
    return -1;
}

/* ---- a backtrace ---- */

struct trace { uint64_t starts[8]; int count; };

static int trace_frame( void *context, void *arg )
{
    struct trace *t = arg;

    if (t->count < 8) t->starts[t->count] = _Unwind_GetRegionStart( context );
    t->count++;
    return 0;
}

long backtrace_test( void );

__attribute__((noinline)) static long deep( struct trace *t )
{
    long ret = _Unwind_Backtrace( trace_frame, t );

    __asm__ volatile( "" ::: "memory" );
    return ret;
}

/* deep, then this function, then the host: 5 (END_OF_STACK) * 100 + frames seen, with deep's and ours in order */
long backtrace_test( void )
{
    struct trace t = { { 0 }, 0 };
    long ret = deep( &t );

    if (t.count < 2 || t.starts[0] != (uint64_t)deep || t.starts[1] != (uint64_t)backtrace_test) return -1;
    return ret * 100 + t.count;
}

/* ---- the rest of glibc ---- */

struct sockaddr_in { uint16_t family, port; uint32_t addr; char zero[8]; };
struct pollfd { int fd; short events, revents; };
struct epoll_event { uint32_t events; uint64_t data; } __attribute__((packed));
struct glob { size_t pathc; char **pathv; size_t offs; int flags; void *funcs[5]; };
struct locale { void *locales[13]; const unsigned short *ctype_b; const int *tolower, *toupper; const char *names[13]; };

extern int socket( int, int, int );
extern int bind( int, const void *, uint32_t );
extern int listen( int, int );
extern int getsockname( int, void *, uint32_t * );
extern int connect( int, const void *, uint32_t );
extern int accept( int, void *, uint32_t * );
extern long send( int, const void *, size_t, int );
extern long recv( int, void *, size_t, int );
extern int poll( struct pollfd *, unsigned long, int );
extern int epoll_create( int );
extern int epoll_ctl( int, int, int, struct epoll_event * );
extern int epoll_wait( int, struct epoll_event *, int, int );
extern int close( int );
extern int open( const char *, int, ... );
extern long read( int, void *, size_t );
extern int glob( const char *, int, void *, struct glob * );
extern void globfree( struct glob * );
extern void srand( unsigned int );
extern int rand( void );
extern size_t mbrtowc( uint32_t *, const char *, size_t, void * );
extern size_t wcrtomb( char *, uint32_t, void * );
extern struct locale *__newlocale( int, const char *, void * );
extern void *mmap( void *, size_t, int, int, int, long );
extern int munmap( void *, size_t );
extern int pthread_create( unsigned long *, const void *, void *(*)( void * ), void * );
extern int pthread_join( unsigned long, void ** );
extern int memcmp( const void *, const void *, size_t );

static void *thread_main( void *arg ) { return (char *)arg + 1; }

/* a bit per check that passed */
long libc_test( void )
{
    long ok = 0;
    char buf[64], wide[4];
    uint32_t wc = 0, len = sizeof(struct sockaddr_in);
    struct sockaddr_in addr = { 2, 0, 0x0100007f };
    int s, c, a, e, fd, i, any = 0;
    struct pollfd pfd;
    struct epoll_event ev = { 1, 99 }, out;
    struct glob g;
    struct locale *loc;
    unsigned long thread;
    void *result, *mem;

    srand( 1 );
    if (rand() == 1804289383 && rand() == 846930886) ok |= 1 << 0;
    if (mbrtowc( &wc, "\xc3\xa9", 2, 0 ) == 2 && wc == 0xe9) ok |= 1 << 1;
    if (wcrtomb( wide, 0x20ac, 0 ) == 3 && !memcmp( wide, "\xe2\x82\xac", 3 )) ok |= 1 << 2;

    s = socket( 2, 1, 0 );
    if (s >= 0 && !bind( s, &addr, sizeof(addr) ) && !listen( s, 1 ) && !getsockname( s, &addr, &len ) && addr.port)
    {
        c = socket( 2, 1, 0 );
        if (!connect( c, &addr, sizeof(addr) ) && (a = accept( s, 0, 0 )) >= 0)
        {
            if (send( c, "ping", 4, 0 ) == 4 && recv( a, buf, sizeof(buf), 0 ) == 4 && !memcmp( buf, "ping", 4 ))
                ok |= 1 << 3;
            send( c, "x", 1, 0 );
            pfd.fd = a;
            pfd.events = 1;
            if (poll( &pfd, 1, 1000 ) == 1 && (pfd.revents & 1)) ok |= 1 << 4;
            e = epoll_create( 1 );
            if (!epoll_ctl( e, 1, a, &ev ) && epoll_wait( e, &out, 1, 1000 ) == 1 && out.data == 99) ok |= 1 << 5;
            close( e );
            close( a );
        }
        close( c );
    }
    close( s );

    if (!glob( "/proc/self/fd/*", 0, 0, &g ) && g.pathc >= 3) ok |= 1 << 6;
    globfree( &g );
    if ((fd = open( "/dev/urandom", 0 )) >= 0 && read( fd, buf, 32 ) == 32)
    {
        for (i = 0; i < 32; i++) any |= buf[i];
        if (any) ok |= 1 << 7;
    }
    close( fd );
    if ((fd = open( "/proc/cpuinfo", 0 )) >= 0 && read( fd, buf, 9 ) == 9 && !memcmp( buf, "processor", 9 ))
        ok |= 1 << 8;
    close( fd );
    if (!pthread_create( &thread, 0, thread_main, (void *)41 ) && !pthread_join( thread, &result ) && result == (void *)42)
        ok |= 1 << 9;
    if ((mem = mmap( 0, 65536, 3, 0x22, -1, 0 )) != (void *)-1)
    {
        ((char *)mem)[65535] = 1;
        if (!munmap( mem, 65536 )) ok |= 1 << 10;
    }
    loc = __newlocale( 0x1fbf, "C", 0 );
    if (loc && (loc->ctype_b['a'] & 0x400) && loc->toupper['a'] == 'A' && !(loc->ctype_b['1'] & 0x400)) ok |= 1 << 11;
    return ok;
}
