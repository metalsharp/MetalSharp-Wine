/* mtsp-statfs.so: Steam's own folder reports the disk its games are on.
 *
 * Steam always lists the folder it is installed in (~/.local/share/Steam) as its first library and shows
 * that folder's disk: the virtual machine's small one. The games of that library are not there, though:
 * steamapps inside it is a link to a folder on the Mac. Loaded into Steam with LD_PRELOAD, this makes a
 * question about the free space of Steam's own folder answer for the folder its steamapps link leads
 * to, so that Steam shows (and checks before a download) the Mac disk the games really go to.
 *
 * Built in the guest by metalsharp-steam: cc -O2 -shared -fPIC -o mtsp-statfs.so mtsp-statfs.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>

static char games[PATH_MAX];      /* <Steam's folder>/steamapps/. */
static dev_t root_dev;
static ino_t root_ino;

__attribute__((constructor)) static void init( void )
{
    const char *home = getenv( "HOME" );
    char root[PATH_MAX];
    struct stat st;

    if (!home || snprintf( root, sizeof(root), "%s/.local/share/Steam", home ) >= (int)sizeof(root)) return;
    if (stat( root, &st )) return;
    if (snprintf( games, sizeof(games), "%s/steamapps/.", root ) >= (int)sizeof(games)) { games[0] = 0; return; }
    root_dev = st.st_dev;
    root_ino = st.st_ino;
}

/* the path to ask about: Steam's games folder when PATH is Steam's own folder (by any name), else PATH */
static const char *target( const char *path )
{
    struct stat st;

    if (!games[0] || !path || stat( path, &st ) || st.st_dev != root_dev || st.st_ino != root_ino) return path;
    return stat( games, &st ) ? path : games;
}

int statfs( const char *path, struct statfs *buf )
{
    static int (*next)( const char *, struct statfs * );
    if (!next) next = dlsym( RTLD_NEXT, "statfs" );
    return next( target( path ), buf );
}

int statvfs( const char *path, struct statvfs *buf )
{
    static int (*next)( const char *, struct statvfs * );
    if (!next) next = dlsym( RTLD_NEXT, "statvfs" );
    return next( target( path ), buf );
}

/* on 64-bit Linux the 64 variants are the same functions with the same structures */
int statfs64( const char *path, struct statfs64 *buf ) { return statfs( path, (struct statfs *)buf ); }
int statvfs64( const char *path, struct statvfs64 *buf ) { return statvfs( path, (struct statvfs *)buf ); }
