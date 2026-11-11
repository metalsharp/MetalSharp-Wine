/* A Linux x86_64 shared library for testing the module host. Built with no C library:
 * everything it uses is declared here and imported, as a real library imports from glibc. */
typedef unsigned long size_t;

extern size_t strlen( const char *s );
extern void *malloc( size_t size );
extern void free( void *ptr );
extern char *strcpy( char *dst, const char *src );
extern int puts( const char *s );
extern int pthread_mutex_lock( void *mutex );
extern int pthread_mutex_unlock( void *mutex );

static int counter = 40;
int *counter_ptr = &counter;            /* a relative relocation */
__thread int tls_value = 7;             /* thread-local storage through __tls_get_addr */
static char mutex[40];                  /* a zero-initialised pthread_mutex_t */

static void __attribute__((constructor)) init( void )
{
    counter += 1;                       /* constructors run: 41 */
}

/* strlen(s) + 41 + the thread-local value, which goes up by one per call */
long host_test( const char *s )
{
    char *copy = malloc( strlen( s ) + 1 );
    long result;

    pthread_mutex_lock( mutex );
    strcpy( copy, s );
    puts( "hello from a Linux x86_64 library" );
    result = strlen( copy ) + *counter_ptr + tls_value++;
    pthread_mutex_unlock( mutex );
    free( copy );
    return result;
}

/* six integer arguments and two floating point ones, to check the calling convention */
double host_args( long a, long b, long c, long d, long e, long f, double x, double y )
{
    return (double)(a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f) + x * y;
}

/* calls back into the host with the System V convention */
long host_callback( long (*fn)( long, long ), long a, long b )
{
    return fn( a, b ) + 1;
}

/* direct Linux system calls: getpid, and a number no kernel has */
long host_raw_getpid( void )
{
    long ret;

    __asm__ volatile( "syscall" : "=a"(ret) : "a"(39L) : "rcx", "r11", "memory" );
    return ret;
}

long host_raw_unknown( void )
{
    long ret;

    __asm__ volatile( "syscall" : "=a"(ret) : "a"(9999L) : "rcx", "r11", "memory" );
    return ret;
}
