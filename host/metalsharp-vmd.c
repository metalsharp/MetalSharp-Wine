/*
 * metalsharp-vmd: the host side of the Linux Steam microVM desktop bridge.
 *
 * The guest (Linux Steam in a krunkit microVM) asks the Mac to do desktop things it cannot do itself:
 * open a folder (Steam's "Browse local files") in Wine's file explorer, or open a web link in the Mac
 * browser. Requests arrive on a UNIX socket that krunkit connects a virtio-vsock port to, one JSON
 * object per line, one request per connection:
 *
 *     {"op": "ping"}
 *     {"op": "open-path", "path": "/Users/me/SteamLibrary/steamapps/common/Game"}
 *     {"op": "open-url",  "url": "https://store.steampowered.com/"}
 *     {"op": "choose-library"}
 *     {"op": "detach"}
 *
 * and the reply is one JSON line: {"ok": true} or {"ok": false, "error": "..."}.
 *
 * Play in Linux Steam is the one request whose connection stays open. Steam's compatibility tool in the
 * guest asks for a Windows game of a shared library to be run on the Mac, by MetalSharp Wine:
 *
 *     {"op": "launch", "backend": "mullion", "exe": "/Users/me/SteamLibrary/steamapps/common/Game/game.exe",
 *      "args": ["-windowed"], "cwd": "/Users/me/SteamLibrary/steamapps/common/Game", "appid": "1245620",
 *      "gameid": "1245620", "compat_data": "/Users/me/SteamLibrary/steamapps/compatdata/1245620"}
 *
 * The first reply is {"ok": true, "state": "started"} (or a refusal, as above). The connection is then
 * held for the life of the game; the guest may send {"op": "stop"} on it (Steam's Stop), and when the
 * game has ended the last reply is {"ok": true, "exit": N}. A connection that closes without a stop line
 * does not end the game: a restarted VM or a crashed guest must not cost unsaved progress. The game is
 * kept and reaped when it ends, and a later launch for the same game takes it over instead of starting a
 * second copy. See "Play" below for what is checked and what the game is started with.
 *
 * The backend of the request is the one the game runs with. A game folder's .metalsharp/launch.env (the
 * MetalSharp application writes one for the games it has set up) names a backend as well, the one that
 * application last used; the backend asked for here wins over it, and the file's other variables still
 * apply. Only the user's own .metalsharp/launch.local.env in that folder wins over both.
 *
 * The guest is not trusted with the Mac: a path is only opened when it resolves (symlinks followed) to
 * an existing location inside one of the shared roots, a URL only when it is plain http or https, and
 * nothing from a request is ever passed through a shell. The limit of that, accepted for "launch": the
 * guest decides which .exe inside a shared library runs on the Mac, and it can write to that library and
 * to the game's prefix, so it controls what the program is and the Windows files it starts with. Which
 * Wine runs it, its environment, where its prefix may be and where its output goes stay with the Mac.
 *
 * Configuration is ~/.metalsharp/vm/bridge.json, read again for every request:
 *
 *     {"shared_roots": ["/Users/me/SteamLibrary"],
 *      "file_manager": "wine",                        (or "finder")
 *      "wine_prefix": "/Users/me/.metalsharp/vm/utility-pfx",
 *      "wine": "/path/to/wine",                       (default: the wine next to this program)
 *      "steam_command": "/path/to/metalsharp-steam",  (for "choose-library")
 *      "launch": "on",                                (anything else: "launch" is refused)
 *      "launch_log": "/Users/me/.metalsharp/vm/logs/game.log",   (the games' output; default: none kept)
 *      "wine_debug": "-all",                          (WINEDEBUG for games; this is the default)
 *      "lang": "en_GB.UTF-8",                         (LANG for games; default: not set)
 *      "game_prefix": "/Users/me/.metalsharp/prefix-steam",   (one prefix for every game; default: none)
 *      "game_user": "me"}                             (the Windows user in that prefix; default steamuser)
 *
 * Without "game_prefix" each game runs in the prefix Steam keeps for it, <compat_data>/pfx. With it,
 * every game runs in that one prefix (MetalSharp's Steam prefix, which its other programs share), as
 * the user "game_user" names: the prefix's profile is the Mac user's there. Nothing is created then, and
 * a game is no more than what its own process group holds: Stop ends that group, and the game is over
 * once the program started for it has exited and its group is empty. The prefix's wineserver, which
 * the other programs of the prefix are using, is neither asked to end its programs nor waited for.
 * (Wine starts a program in a group of its own only when it gets a console or no window of its own, or
 * is detached: Wine's own services, and what a game starts that way, are not counted as the game.)
 *
 * MetalSharp's Play: the MetalSharp application on the Mac starts a game through Linux Steam with the
 * backend chosen in MetalSharp. Right before it asks Steam to run the game it writes the backend's name
 * (mullion, dxvk, vkd3d or wine, one line) to launch-intent/<appid> in the folder this program's socket
 * is in, a folder of the Mac that the guest has no way to. A launch for that app id in the next two
 * minutes runs on that backend instead of the one the guest asks for, and the file is removed. A file
 * that is a link, is not this user's, is in a folder others can write to, or is older than that is
 * not used; a Play started in Steam itself keeps the backend of Steam's tool.
 *
 * "detach" is what "metalsharp-steam stop" sends before it shuts the virtual machine down: every game's
 * connection is let go of (the games are kept), so that Steam's programs being ended in the guest are not
 * taken for Steam's Stop. The reply says how many games are running, {"ok": true, "games": N}, and
 * metalsharp-steam leaves this program running while there are any, so that the next Play of such a game
 * finds it. The guest can send "detach" too; it gains nothing by it that closing its own connections
 * would not do.
 *
 * When this program is ended (SIGTERM) the games it started are left running: each is in a session of
 * its own and does not depend on this program. The guest's connections close, so Steam there shows them
 * as stopped. A new metalsharp-vmd does not know of them.
 *
 *     metalsharp-vmd [--socket PATH] [--config PATH]
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_REQUEST      65536  /* bytes; a request is one JSON line ("launch" carries a game's arguments) */
#define MAX_CONFIG       65536
#define MAX_URL          2048
#define MAX_ROOTS        16
#define MAX_THREADS      24     /* a slow or flooding guest cannot exhaust host threads/fds; running games hold MAX_LAUNCHES */
#define MAX_LIVE_OPENS   8      /* at most this many explorer/open children alive at once */
#define MIN_INTERVAL_MS  500    /* between things opened on the Mac, so a guest cannot flood the desktop */
#define REQUEST_DEADLINE_MS 5000 /* a whole request must arrive and be answered within this long */
#define MAX_DEPTH        32
#define MAX_LAUNCHES     4      /* games running at once, counting those whose connection is gone */
#define MAX_LAUNCH_ARGS  64     /* arguments of one game, and their total size with terminators */
#define MAX_ARGS_BYTES   16384
#define MIN_LAUNCH_INTERVAL_MS 1000  /* between games started, apart from the interval for things opened */
#define LAUNCH_TICK_MS   200    /* how often a running game's thread looks at its connection and its processes */
#define STOP_WAIT_MS     20000  /* a stopped game is given up on (and its processes killed) after this long */
#define GUEST_LOG_LINES  20     /* log lines the guest can cause at will, in each GUEST_LOG_WINDOW_MS */
#define GUEST_LOG_WINDOW_MS 10000
#define GROUP_STOP_WAIT_MS 5000 /* in a shared prefix, a stopped game's group is killed if still there after this */
#define INTENT_MAX_AGE   120    /* seconds a launch intent of the MetalSharp application stands for */

static char socket_path[PATH_MAX], config_path[PATH_MAX], default_wine[PATH_MAX], default_prefix[PATH_MAX];
static char home_dir[PATH_MAX], temp_dir[PATH_MAX];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int active_threads;
static int64_t last_open_ms = -MIN_INTERVAL_MS;
static pid_t live[MAX_LIVE_OPENS];
static int64_t last_launch_ms = -MIN_LAUNCH_INTERVAL_MS;

static int64_t now_ms( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logline( const char *fmt, va_list ap )
{
    char stamp[16], line[PATH_MAX + 256];
    time_t t = time( NULL );
    struct tm tm;

    localtime_r( &t, &tm );
    strftime( stamp, sizeof(stamp), "%H:%M:%S", &tm );
    vsnprintf( line, sizeof(line), fmt, ap );
    /* a name in a share is the guest's to choose: it cannot start a line of its own or send escapes */
    for (unsigned char *c = (unsigned char *)line; *c; c++) if (*c < 0x20 || *c == 0x7f) *c = '?';
    fprintf( stderr, "%s %s\n", stamp, line );
    fflush( stderr );
}

static void logmsg( const char *fmt, ... )
{
    va_list ap;

    va_start( ap, fmt );
    logline( fmt, ap );
    va_end( ap );
}

/* For what a guest can cause as often as it likes without anything being started (a refusal, a
 * connection taken up or dropped): the log is a file on the Mac, and a guest in a loop must not fill the
 * disk with it. A few lines are written in each stretch of time; the rest are only counted, and the
 * count is written once. */
static void logguest( const char *fmt, ... )
{
    static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
    static int64_t window = -GUEST_LOG_WINDOW_MS;
    static unsigned int lines, dropped;
    unsigned int missed = 0;
    int64_t now = now_ms();
    bool write;
    va_list ap;

    pthread_mutex_lock( &log_lock );
    if (now - window >= GUEST_LOG_WINDOW_MS)
    {
        missed = dropped;
        window = now;
        lines = dropped = 0;
    }
    if (!(write = lines < GUEST_LOG_LINES)) dropped++;
    else lines++;
    pthread_mutex_unlock( &log_lock );
    if (missed) logmsg( "(%u more lines about the guest's requests were not written)", missed );
    if (!write) return;
    va_start( ap, fmt );
    logline( fmt, ap );
    va_end( ap );
}

/* ---- a small JSON reader: one object whose members are strings or arrays of strings ---- */

struct json { const unsigned char *p, *end; };

/* Called for each member (in_array false) and each element of an array member. val is the text of a
 * string; it is NULL, and clean false, for a value of any other kind, which is otherwise skipped. */
typedef void (*field_fn)( void *ctx, const char *key, const char *val, bool clean, bool in_array );

static void skip_ws( struct json *j )
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static bool take( struct json *j, char c )
{
    skip_ws( j );
    if (j->p < j->end && *j->p == c) { j->p++; return true; }
    return false;
}

static int hex4( struct json *j )
{
    int i, v = 0;

    if (j->end - j->p < 4) return -1;
    for (i = 0; i < 4; i++)
    {
        unsigned char c = *j->p++;
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* Decode the string at j->p (which is at its opening quote) into out, NUL-terminated; with no out the
 * string is only checked and passed over. *clean is cleared when the decoded text has a control
 * character. False for a malformed string or one that does not fit. */
static bool read_string( struct json *j, char *out, size_t cap, bool *clean )
{
    size_t n = 0;

    *clean = true;
    if (j->p >= j->end || *j->p != '"') return false;
    j->p++;
    while (j->p < j->end)
    {
        unsigned int cp = *j->p++;
        if (cp == '"')
        {
            if (out) out[n] = 0;
            return true;
        }
        if (cp < 0x20) return false;
        if (cp == '\\')
        {
            if (j->p >= j->end) return false;
            switch (*j->p++)
            {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u':
            {
                int hi = hex4( j ), lo;
                if (hi < 0) return false;
                if (hi >= 0xdc00 && hi <= 0xdfff) return false;
                if (hi >= 0xd800 && hi <= 0xdbff)
                {
                    if (j->end - j->p < 2 || j->p[0] != '\\' || j->p[1] != 'u') return false;
                    j->p += 2;
                    lo = hex4( j );
                    if (lo < 0xdc00 || lo > 0xdfff) return false;
                    cp = 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
                }
                else cp = hi;
                break;
            }
            default: return false;
            }
            if (cp < 0x20 || cp == 0x7f) *clean = false;
            if (!out) continue;
            if (n + 5 > cap) return false;
            if (cp < 0x80) out[n++] = cp;
            else if (cp < 0x800)
            {
                out[n++] = 0xc0 | (cp >> 6);
                out[n++] = 0x80 | (cp & 0x3f);
            }
            else if (cp < 0x10000)
            {
                out[n++] = 0xe0 | (cp >> 12);
                out[n++] = 0x80 | ((cp >> 6) & 0x3f);
                out[n++] = 0x80 | (cp & 0x3f);
            }
            else
            {
                out[n++] = 0xf0 | (cp >> 18);
                out[n++] = 0x80 | ((cp >> 12) & 0x3f);
                out[n++] = 0x80 | ((cp >> 6) & 0x3f);
                out[n++] = 0x80 | (cp & 0x3f);
            }
            continue;
        }
        if (cp == 0x7f) *clean = false;
        if (!out) continue;
        if (n + 2 > cap) return false;
        out[n++] = cp;  /* a raw byte; the whole input was checked to be UTF-8 beforehand */
    }
    return false;
}

/* Pass over one value of any kind. Nothing is kept of it, so the nesting a guest chooses costs only a
 * small stack frame per level, up to MAX_DEPTH. */
static bool skip_value( struct json *j, int depth )
{
    bool clean;

    skip_ws( j );
    if (j->p >= j->end || depth > MAX_DEPTH) return false;
    if (*j->p == '"') return read_string( j, NULL, 0, &clean );
    if (*j->p == '{' || *j->p == '[')
    {
        char close = *j->p++ == '{' ? '}' : ']';
        bool object = close == '}';

        if (take( j, close )) return true;
        for (;;)
        {
            if (object)
            {
                skip_ws( j );
                if (!read_string( j, NULL, 0, &clean ) || !take( j, ':' )) return false;
            }
            if (!skip_value( j, depth + 1 )) return false;
            if (take( j, close )) return true;
            if (!take( j, ',' )) return false;
        }
    }
    /* a number, true, false or null: its exact spelling does not matter here */
    {
        const unsigned char *start = j->p;
        while (j->p < j->end && *j->p && (strchr( "+-.0123456789", *j->p ) || (*j->p >= 'a' && *j->p <= 'z') || *j->p == 'E'))
            j->p++;
        return j->p > start;
    }
}

static bool read_members( struct json *j, char *key, char *val, size_t cap, field_fn fn, void *ctx )
{
    bool key_clean, clean;

    if (!take( j, '{' )) return false;
    if (!take( j, '}' ))
    {
        for (;;)
        {
            skip_ws( j );
            if (!read_string( j, key, cap, &key_clean ) || !take( j, ':' )) return false;
            if (!key_clean) key[0] = 0;  /* matches no member name */
            skip_ws( j );
            if (j->p < j->end && *j->p == '"')
            {
                if (!read_string( j, val, cap, &clean )) return false;
                fn( ctx, key, val, clean, false );
            }
            else if (j->p < j->end && *j->p == '[')
            {
                j->p++;
                if (!take( j, ']' ))
                {
                    for (;;)
                    {
                        skip_ws( j );
                        if (j->p < j->end && *j->p == '"')
                        {
                            if (!read_string( j, val, cap, &clean )) return false;
                            fn( ctx, key, val, clean, true );
                        }
                        else
                        {
                            if (!skip_value( j, 1 )) return false;
                            fn( ctx, key, NULL, false, true );
                        }
                        if (take( j, ']' )) break;
                        if (!take( j, ',' )) return false;
                    }
                }
            }
            else
            {
                if (!skip_value( j, 1 )) return false;
                fn( ctx, key, NULL, false, false );
            }
            if (take( j, '}' )) break;
            if (!take( j, ',' )) return false;
        }
    }
    skip_ws( j );
    return j->p == j->end;
}

/* Parse one JSON object and nothing after it. False when it is not well-formed. A decoded string is
 * never longer than its source, so two buffers the size of the text hold any name and value; they are
 * on the heap because a request can be far larger than a handler's stack should have to carry. */
static bool read_object( const void *data, size_t len, field_fn fn, void *ctx )
{
    struct json j = { data, (const unsigned char *)data + len };
    size_t cap = len + 8;
    char *key = malloc( cap ), *val = malloc( cap );
    bool ok = key && val && read_members( &j, key, val, cap, fn, ctx );

    free( key );
    free( val );
    return ok;
}

static bool utf8_valid( const unsigned char *s, size_t len )
{
    size_t i = 0;

    while (i < len)
    {
        unsigned char c = s[i];
        unsigned int cp, n, k;

        if (c < 0x80) { i++; continue; }
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 0x07; }
        else return false;
        if (len - i <= n) return false;
        for (k = 1; k <= n; k++)
        {
            if ((s[i + k] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3f);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        i += n + 1;
    }
    return true;
}

/* ---- configuration ---- */

struct config
{
    char roots[MAX_ROOTS][PATH_MAX];
    int nroots;
    bool finder;                /* file_manager is not "wine" */
    char wine_prefix[PATH_MAX];
    char wine[PATH_MAX];
    char steam_command[PATH_MAX];   /* metalsharp-steam, for "choose-library" */
    bool launch;                /* games may be started for the guest */
    char launch_log[PATH_MAX];  /* where the games' output goes, or empty */
    char wine_debug[256];       /* WINEDEBUG for games */
    char lang[64];              /* LANG for games, or empty */
    char game_prefix[PATH_MAX]; /* the one prefix every game runs in, or empty: each its own */
    char game_user[33];         /* USER for games in game_prefix */
};

/* a Windows user name: letters, digits and . _ - */
static bool user_name( const char *s )
{
    if (!*s || strlen( s ) > 32) return false;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || strchr( "._-", *s )))
            return false;
    return true;
}

/* a locale name such as en_GB.UTF-8, and nothing else */
static bool locale_name( const char *s )
{
    if (!*s) return false;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || strchr( "_.@-", *s )))
            return false;
    return true;
}

static void config_field( void *ctx, const char *key, const char *val, bool clean, bool in_array )
{
    struct config *cfg = ctx;

    if (!clean) return;
    if (in_array)
    {
        if (!strcmp( key, "shared_roots" ) && val[0] == '/' && cfg->nroots < MAX_ROOTS && strlen( val ) < PATH_MAX)
            strcpy( cfg->roots[cfg->nroots++], val );
        return;
    }
    if (!strcmp( key, "file_manager" )) cfg->finder = strcmp( val, "wine" ) != 0;
    else if (!strcmp( key, "wine_prefix" ) && val[0] == '/') strlcpy( cfg->wine_prefix, val, PATH_MAX );
    else if (!strcmp( key, "wine" ) && val[0] == '/') strlcpy( cfg->wine, val, PATH_MAX );
    else if (!strcmp( key, "steam_command" ) && val[0] == '/' && strlen( val ) < PATH_MAX) strcpy( cfg->steam_command, val );
    else if (!strcmp( key, "launch" )) cfg->launch = !strcmp( val, "on" );
    else if (!strcmp( key, "launch_log" ) && val[0] == '/' && strlen( val ) < PATH_MAX) strcpy( cfg->launch_log, val );
    else if (!strcmp( key, "wine_debug" ) && val[0] && strlen( val ) < sizeof(cfg->wine_debug)) strcpy( cfg->wine_debug, val );
    else if (!strcmp( key, "lang" ) && locale_name( val ) && strlen( val ) < sizeof(cfg->lang)) strcpy( cfg->lang, val );
    else if (!strcmp( key, "game_prefix" ) && val[0] == '/' && strlen( val ) < PATH_MAX) strcpy( cfg->game_prefix, val );
    else if (!strcmp( key, "game_user" ) && user_name( val )) strcpy( cfg->game_user, val );
}

static void load_config( struct config *cfg )
{
    static char buf[MAX_CONFIG];
    static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
    ssize_t n = 0, got;
    int fd, i;

    memset( cfg, 0, sizeof(*cfg) );
    pthread_mutex_lock( &buf_lock );
    if ((fd = open( config_path, O_RDONLY | O_CLOEXEC )) >= 0)
    {
        while (n < MAX_CONFIG && (got = read( fd, buf + n, MAX_CONFIG - n )) > 0) n += got;
        close( fd );
        if (n >= MAX_CONFIG || !utf8_valid( (unsigned char *)buf, n ) || !read_object( buf, n, config_field, cfg ))
        {
            logmsg( "ignoring %s: not a JSON object this program understands", config_path );
            memset( cfg, 0, sizeof(*cfg) );
        }
    }
    else if (errno != ENOENT) logmsg( "ignoring %s: %s", config_path, strerror( errno ) );
    pthread_mutex_unlock( &buf_lock );
    /* a root is kept as its real path; one that does not exist (a volume not mounted) shares nothing */
    for (i = n = 0; i < cfg->nroots; i++)
    {
        char real[PATH_MAX];
        if (realpath( cfg->roots[i], real )) strcpy( cfg->roots[n++], real );
    }
    cfg->nroots = n;
    if (!cfg->wine_prefix[0]) strlcpy( cfg->wine_prefix, default_prefix, PATH_MAX );
    if (!cfg->wine[0]) strlcpy( cfg->wine, default_wine, PATH_MAX );
    if (!cfg->wine_debug[0]) strcpy( cfg->wine_debug, "-all" );
    if (!cfg->game_user[0]) strcpy( cfg->game_user, "steamuser" );
}

/* ---- the things a guest may ask for ---- */

static bool inside( const char *path, const char *root )
{
    size_t len = strlen( root );

    while (len > 1 && root[len - 1] == '/') len--;
    if (strncmp( path, root, len )) return false;
    return !path[len] || path[len] == '/' || (len == 1 && root[0] == '/');
}

static bool shared( const struct config *cfg, const char *path )
{
    int i;

    for (i = 0; i < cfg->nroots; i++) if (inside( path, cfg->roots[i] )) return true;
    return false;
}

static bool is_dir( const char *path )
{
    struct stat st;
    return !stat( path, &st ) && S_ISDIR( st.st_mode );
}

/* Windows explorer re-parses its argument as a command line and runs ShellExecuteW on a non-directory
 * (wine/programs/explorer/explorer.c), so a folder name carrying a quote, comma or backslash, or a
 * component Wine's path code trims to a different name, must never reach it. A name made only of these
 * characters round-trips to the same Z: path and cannot be reinterpreted; anything else is opened with
 * Finder's reveal instead, which takes the path as one literal argument and never launches it. */
static bool safe_char( unsigned char c )
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    return c && strchr( " ._()'&+!@#$%^=~[]{}-", c );
}

/* CON, NUL, COM1 and the like, with or without an extension, name devices in Windows paths */
static bool dos_device( const char *name, size_t len )
{
    static const char *const devices[] = { "con", "prn", "aux", "nul", "conin$", "conout$" };
    size_t i, base = 0;

    while (base < len && name[base] != '.' && name[base] != ' ') base++;
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
        if (base == strlen( devices[i] ) && !strncasecmp( name, devices[i], base )) return true;
    return base == 4 && (!strncasecmp( name, "com", 3 ) || !strncasecmp( name, "lpt", 3 )) && name[3] >= '0' && name[3] <= '9';
}

/* The Z: path of a folder explorer.exe cannot reinterpret: every component is made of safe characters and
 * none ends in a dot or space (Wine's path code trims those, so x.exe. would resolve to x.exe). */
static bool wine_safe( const char *folder, char *win, size_t cap )
{
    const char *p = folder;
    size_t n = 2;

    if (*p != '/' || !p[1]) return false;
    win[0] = 'Z'; win[1] = ':';
    while (*p)
    {
        const char *start = ++p;  /* past the slash */
        if (n + 1 >= cap) return false;
        win[n++] = '\\';
        while (*p && *p != '/')
        {
            if (!safe_char( *p ) || n + 1 >= cap) return false;
            win[n++] = *p++;
        }
        if (p == start || p[-1] == '.' || p[-1] == ' ' || dos_device( start, p - start )) return false;
    }
    win[n] = 0;
    return n < 260;  /* explorer's buffers are MAX_PATH */
}

static bool throttle( void )
{
    int64_t now = now_ms();
    bool ok;

    pthread_mutex_lock( &lock );
    if ((ok = now - last_open_ms >= MIN_INTERVAL_MS)) last_open_ms = now;
    pthread_mutex_unlock( &lock );
    return ok;
}

/* Start a detached child with nothing of ours but the environment. A new child is only allowed while few
 * earlier ones are still alive, so a guest cannot pile up windows and processes. */
static const char *spawn_child( char *const argv[], char *const envp[] )
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    sigset_t none, all;
    const char *error = NULL;
    int i, slot = -1, err;
    pid_t pid;

    pthread_mutex_lock( &lock );
    for (i = 0; i < MAX_LIVE_OPENS; i++)
    {
        if (live[i] && waitpid( live[i], NULL, WNOHANG ) != 0) live[i] = 0;  /* exited, or not ours any more */
        if (!live[i]) slot = i;
    }
    if (slot < 0) error = "too many open windows";
    else
    {
        sigemptyset( &none );
        sigfillset( &all );
        posix_spawn_file_actions_init( &fa );
        posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
        posix_spawn_file_actions_addopen( &fa, 1, "/dev/null", O_WRONLY, 0 );
        posix_spawn_file_actions_adddup2( &fa, 1, 2 );
        posix_spawnattr_init( &attr );
        posix_spawnattr_setsigmask( &attr, &none );
        posix_spawnattr_setsigdefault( &attr, &all );
        posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT |
                                         POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF );
        if ((err = posix_spawn( &pid, argv[0], &fa, &attr, argv, envp )))
        {
            logmsg( "cannot start %s: %s", argv[0], strerror( err ) );
            error = "could not be opened";
        }
        else live[slot] = pid;
        posix_spawnattr_destroy( &attr );
        posix_spawn_file_actions_destroy( &fa );
    }
    pthread_mutex_unlock( &lock );
    return error;
}

/* this process's environment with the variables Wine's explorer is started with replaced */
static char **wine_environment( const char *prefix_var )
{
    size_t n = 0, i;
    char **env;

    while (environ[n]) n++;
    if (!(env = calloc( n + 4, sizeof(*env) ))) return NULL;
    for (i = 0, n = 0; environ[i]; i++)
        if (strncmp( environ[i], "WINEPREFIX=", 11 ) && strncmp( environ[i], "WINEDEBUG=", 10 ) &&
            strncmp( environ[i], "METALSHARP_EXPLORER_FOLDER_ONLY=", 32 )) env[n++] = environ[i];
    env[n++] = (char *)prefix_var;
    env[n++] = "WINEDEBUG=-all";
    /* explorer.exe launches an argument that is not a folder; this tells MetalSharp Wine's explorer not to,
     * so a folder the guest swaps for a file after the check below is not run. */
    env[n++] = "METALSHARP_EXPLORER_FOLDER_ONLY=1";
    return env;
}

struct request
{
    char op[32];
    char path[PATH_MAX * 4];
    char url[MAX_URL + 1];
    bool has_path, has_url;  /* present, clean and not too long */
    /* "launch" */
    char backend[16], appid[16], gameid[24];
    char exe[PATH_MAX], cwd[PATH_MAX], compat_data[PATH_MAX];
    bool launch_bad;         /* one of those was given and is empty, unclean or too long */
    char *args[MAX_LAUNCH_ARGS + 1];
    char args_buf[MAX_ARGS_BYTES];
    size_t nargs, args_used;
    bool args_bad;           /* not an array of clean strings, or too many, or too much */
};

static void launch_field( struct request *rq, char *dst, size_t cap, const char *val, bool clean )
{
    if (!clean || !val[0] || strlcpy( dst, val, cap ) >= cap)
    {
        dst[0] = 0;
        rq->launch_bad = true;
    }
}

static void request_field( void *ctx, const char *key, const char *val, bool clean, bool in_array )
{
    struct request *rq = ctx;

    if (!strcmp( key, "args" ))
    {
        size_t len;

        if (!in_array || !val || !clean || rq->nargs >= MAX_LAUNCH_ARGS ||
            (len = strlen( val ) + 1) > sizeof(rq->args_buf) - rq->args_used)
        {
            rq->args_bad = true;
            return;
        }
        rq->args[rq->nargs++] = memcpy( rq->args_buf + rq->args_used, val, len );
        rq->args_used += len;
        return;
    }
    if (in_array || !val) return;
    if (!strcmp( key, "op" )) strlcpy( rq->op, clean ? val : "", sizeof(rq->op) );
    else if (!strcmp( key, "backend" )) launch_field( rq, rq->backend, sizeof(rq->backend), val, clean );
    else if (!strcmp( key, "appid" )) launch_field( rq, rq->appid, sizeof(rq->appid), val, clean );
    else if (!strcmp( key, "gameid" )) launch_field( rq, rq->gameid, sizeof(rq->gameid), val, clean );
    else if (!strcmp( key, "exe" )) launch_field( rq, rq->exe, sizeof(rq->exe), val, clean );
    else if (!strcmp( key, "cwd" )) launch_field( rq, rq->cwd, sizeof(rq->cwd), val, clean );
    else if (!strcmp( key, "compat_data" )) launch_field( rq, rq->compat_data, sizeof(rq->compat_data), val, clean );
    else if (!strcmp( key, "path" )) rq->has_path = clean && val[0] && strlcpy( rq->path, val, sizeof(rq->path) ) < sizeof(rq->path);
    else if (!strcmp( key, "url" )) rq->has_url = clean && val[0] && strlcpy( rq->url, val, sizeof(rq->url) ) < sizeof(rq->url);
}

static const char *open_path( const struct config *cfg, const struct request *rq )
{
    char real[PATH_MAX], folder[PATH_MAX], check[PATH_MAX], win[PATH_MAX];
    char prefix_var[PATH_MAX + 16], *slash;
    const char *error;
    bool use_wine;

    if (!rq->has_path || rq->path[0] != '/') return "not an absolute path";
    if (strlen( rq->path ) >= PATH_MAX) return "not inside a shared folder";
    if (!realpath( rq->path, real ))
    {
        /* It does not exist on the Mac. It is "not there" only when the deepest part of it that does exist
         * is inside a share; anywhere else the reply is the same whether or not the path exists, so the
         * guest learns nothing about the rest of the Mac. */
        strlcpy( folder, rq->path, sizeof(folder) );
        while ((slash = strrchr( folder, '/' )) && slash != folder)
        {
            *slash = 0;
            if (realpath( folder, real )) return shared( cfg, real ) ? "no such file or folder" : "not inside a shared folder";
        }
        return "not inside a shared folder";
    }
    if (!shared( cfg, real )) return "not inside a shared folder";
    strlcpy( folder, real, sizeof(folder) );
    if (!is_dir( real ))
    {
        if ((slash = strrchr( folder, '/' )) && slash != folder) *slash = 0;
        else return "not inside a shared folder";
        if (!shared( cfg, folder )) return "not inside a shared folder";
    }
    use_wine = !cfg->finder && wine_safe( folder, win, sizeof(win) ) &&
               !access( cfg->wine, X_OK ) && is_dir( cfg->wine_prefix );
    if (!throttle()) return "too many requests";
    /* The guest can write to the share, so resolve again right before launching: a swap of the checked
     * folder for a file or an out-of-share symlink since the check is caught here, and a swap after this
     * is refused by explorer itself (METALSHARP_EXPLORER_FOLDER_ONLY). Finder reveal (-R) is used for the fallback and whenever a name is not provably safe for
     * explorer; it selects the item and never launches it, so a folder that is really an .app or an .exe
     * is not run. */
    if (!realpath( folder, check ) || strcmp( check, folder ) || !is_dir( folder )) return "folder changed underneath";
    if (use_wine)
    {
        char *argv[] = { (char *)cfg->wine, "explorer.exe", win, NULL };
        char **env;

        snprintf( prefix_var, sizeof(prefix_var), "WINEPREFIX=%s", cfg->wine_prefix );
        if (!(env = wine_environment( prefix_var ))) return "could not be opened";
        if (!(error = spawn_child( argv, env ))) logmsg( "open-path %s -> wine explorer", real );
        free( env );
    }
    else
    {
        char *argv[] = { "/usr/bin/open", "-R", "--", real, NULL };
        if (!(error = spawn_child( argv, environ ))) logmsg( "open-path %s -> finder reveal", real );
    }
    return error;
}

static const char *open_url( const struct request *rq )
{
    const char *url = rq->url, *host, *error;
    const unsigned char *c;
    size_t scheme;

    if (!rq->has_url) return "not a usable URL";
    if (!strncasecmp( url, "http://", 7 )) scheme = 4;
    else if (!strncasecmp( url, "https://", 8 )) scheme = 5;
    else return "only http and https links are opened";
    host = url + scheme + 3;
    if (!((*host >= 'a' && *host <= 'z') || (*host >= 'A' && *host <= 'Z') || (*host >= '0' && *host <= '9') || *host == '['))
        return "only http and https links are opened";
    for (c = (const unsigned char *)url; *c; c++)
        if (*c <= 0x20 || *c >= 0x7f || strchr( "\"<>\\^`{|}", *c )) return "not a usable URL";
    if (!throttle()) return "too many requests";
    {
        char *argv[] = { "/usr/bin/open", "--", (char *)url, NULL };  /* -- so a URL is never read as a flag */
        if (!(error = spawn_child( argv, environ )))
            logmsg( "open-url %.*s://%.*s/...", (int)scheme, url, (int)strcspn( host, "/?#" ), host );
    }
    return error;
}

/* Steam's "Add Drive" in the guest: the person at the Mac chooses a folder in Wine's folder browser and
 * it becomes a Steam library, all of it done by "metalsharp-steam library choose" (the command named in
 * the configuration, never anything the guest sends). The guest only gets to ask for the browser to be
 * shown; it does not learn what was chosen, and one browser is open at a time (the command sees to that). */
static const char *choose_library( const struct config *cfg )
{
    char *argv[] = { (char *)cfg->steam_command, "library", "choose", NULL };
    const char *error;

    if (!cfg->steam_command[0]) return "not available";
    if (!throttle()) return "too many requests";
    if (!(error = spawn_child( argv, environ ))) logmsg( "choose-library -> %s library choose", cfg->steam_command );
    return error;
}

/* One newline-terminated request, with the whole exchange bounded by the deadline so a slow or silent
 * guest cannot hold a thread. */
static size_t read_request( int fd, char *data, int64_t deadline )
{
    size_t len = 0;

    while (len < MAX_REQUEST && !memchr( data, '\n', len ))
    {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int64_t remaining = deadline - now_ms();
        ssize_t got;

        if (remaining <= 0 || poll( &pfd, 1, (int)remaining ) <= 0) break;
        if ((got = recv( fd, data + len, MAX_REQUEST - len, 0 )) <= 0) break;
        len += got;
    }
    return len;
}

static void send_reply( int fd, const char *reply, int64_t deadline )
{
    size_t len = strlen( reply ), sent = 0;

    while (sent < len)
    {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        int64_t remaining = deadline - now_ms();
        ssize_t n;

        if (remaining < 100) remaining = 100;
        if (poll( &pfd, 1, (int)remaining ) <= 0) break;
        if ((n = send( fd, reply + sent, len - sent, 0 )) <= 0) break;
        sent += n;
    }
}

/* ---- Play: a Windows game of a shared library, started for Linux Steam ----
 *
 * What the guest chooses is checked, after symlinks are resolved: the program is an .exe file inside a
 * shared folder, the working folder is inside one, and the compatibility data folder is
 * <shared library>/steamapps/compatdata/<number>. The prefix is its "pfx" folder, which must not be a
 * link; this program creates that one empty folder when it is missing and leaves everything inside it to
 * Wine, because the guest can write there and could aim anything this program copied or edited at
 * another place on the Mac. For the same reason the two folders are opened first and checked afterwards,
 * by the path the opened folder really has: the guest can swap a folder for a link between a check of a
 * name and the use of it, but not change what is already open. "pfx" is made and looked at inside the
 * opened folder, and the working folder is entered through its descriptor. Wine itself is given the
 * prefix by name, so the name is looked at once more right before Wine, or a wineserver for the prefix,
 * is started; what a swap in the instant after that could reach is no more than the guest has anyway by
 * choosing the program. The game's environment is put together here and nothing of this program's
 * own is passed on, so neither the guest nor whoever started this program reaches the loader (DYLD_*),
 * the prefix or the Wine that runs.
 *
 * A game is over when the process started here has exited and its prefix's wineserver is gone
 * ("wineserver -w"), so a launcher that starts the real game and exits is followed to the game's end.
 * In the configuration's shared game_prefix it is over when that process has exited and nothing is
 * left in its process group (see the configuration above).
 * One thread, the one that took the request, stays with the game until then; MAX_LAUNCHES of the
 * MAX_THREADS can be held this way. Children are only ever waited for by their own process id, by the
 * thread that started them, so this table and spawn_child's never reap each other's. */

struct launch
{
    bool used;
    int fd;                     /* the guest's connection; -1 once it is gone (the game is kept) */
    pid_t pid;                  /* the wine process, also its process group; 0 until it is started */
    bool stop;                  /* the guest asked for the game to be ended */
    bool detach;                /* the connection is to be let go of ("detach") */
    bool shared;                /* in the configuration's game_prefix, which other programs share */
    char prefix[PATH_MAX];
    char wineserver[PATH_MAX];
    char appid[16];
    char **env;
    char line[256];             /* what has arrived on the connection since the last whole line */
    size_t linelen;
};

/* The table is the lock's. A game's thread reads its own pid, prefix, wineserver and env freely (only it
 * writes them once the game is started); fd, stop, detach and linelen are shared with the threads of
 * later requests and only touched with the lock held. The bytes of line belong to whoever holds a
 * connection for the game: its thread while fd is one, a joining launch while fd is -1. */

static struct launch launches[MAX_LAUNCHES];

static const struct { const char *name, *value, *marker; } backends[] =
{
    /* the names of GRAPHICS_BACKEND (wine/dlls/ntdll/unix/graphics_policy.c), and "mullion" for Wine's
     * own default, which is chosen by leaving the variable out. The third is what
     * METALSHARP_LAUNCH_BACKEND says (see launch_environment): one of the four names Wine knows there.
     * DXMT is Wine's other name for Mullion, and asked for D3DMetal, which Apple silicon does not have,
     * Wine uses its own Direct3D. */
    { "mullion", NULL, "mullion" }, { "dxmt", "dxmt", "mullion" }, { "dxvk", "dxvk", "dxvk" },
    { "vkd3d", "vkd3d", "vkd3d" }, { "d3dmetal", "d3dmetal", "wine" }, { "wine", "wine", "wine" },
    { "wined3d", "wine", "wine" },
};

static bool digits( const char *s, size_t max )
{
    size_t n = strspn( s, "0123456789" );
    return n && n <= max && !s[n];
}

static void free_environment( char **env )
{
    char **e;

    for (e = env; e && *e; e++) free( *e );
    free( env );
}

static bool env_add( char **env, int *n, const char *fmt, ... )
{
    va_list ap;
    int len;

    va_start( ap, fmt );
    len = vasprintf( &env[*n], fmt, ap );
    va_end( ap );
    if (len < 0)
    {
        env[*n] = NULL;
        return false;
    }
    (*n)++;
    return true;
}

/* Everything a game is started with. Wine finds its own libraries, modules and wineserver from where
 * its program is, so it needs nothing more than this. The Windows user is "steamuser" as under Proton,
 * not the Mac account: Wine names the profile after USER, and the prefix is in a folder the guest reads.
 * In the configuration's game_prefix it is game_user, the profile that prefix has.
 *
 * The backend is named twice. GRAPHICS_BACKEND is what Wine chooses by (left out for Mullion, its
 * default). But Wine applies the game folder's .metalsharp/launch.env first, the environment the
 * MetalSharp application last started the game with, and that file names a backend too: a game set up
 * for DXVK there ran on DXVK whichever tool was chosen in Steam. METALSHARP_LAUNCH_BACKEND, always
 * there, tells Wine which backend this launch asked for, and Wine puts it back over launch.env's
 * (wine/dlls/ntdll/unix/env.c, add_launch_environment). The rest of launch.env still applies, and the
 * user's own launch.local.env still comes last: a backend it names is the one the game's program and
 * the programs it starts run with. */
static char **launch_environment( const struct config *cfg, const struct request *rq, const char *backend,
                                  const char *marker, const char *prefix, const char *compat_data, const char *user )
{
    char **env = calloc( 20, sizeof(*env) );
    int n = 0;

    if (!env) return NULL;
    if (env_add( env, &n, "HOME=%s", home_dir ) &&
        env_add( env, &n, "TMPDIR=%s", temp_dir ) &&
        env_add( env, &n, "PATH=/usr/bin:/bin:/usr/sbin:/sbin" ) &&
        env_add( env, &n, "USER=%s", user ) &&
        env_add( env, &n, "LOGNAME=%s", user ) &&
        (!cfg->lang[0] || env_add( env, &n, "LANG=%s", cfg->lang )) &&
        env_add( env, &n, "WINEPREFIX=%s", prefix ) &&
        env_add( env, &n, "WINEDEBUG=%s", cfg->wine_debug ) &&
        (!backend || env_add( env, &n, "GRAPHICS_BACKEND=%s", backend )) &&
        env_add( env, &n, "METALSHARP_LAUNCH_BACKEND=%s", marker ) &&
        env_add( env, &n, "SteamAppId=%s", rq->appid ) &&
        env_add( env, &n, "SteamGameId=%s", rq->gameid[0] ? rq->gameid : rq->appid ) &&
        env_add( env, &n, "STEAM_COMPAT_APP_ID=%s", rq->appid ) &&
        env_add( env, &n, "STEAM_COMPAT_DATA_PATH=%s", compat_data )) return env;
    free_environment( env );
    return NULL;
}

/* Start wine or wineserver in a session of its own, with nothing of ours: the given environment, the
 * given (open) folder, and its output on out (or nowhere). The process id, or 0. */
static pid_t spawn_wine( char *const argv[], char *const envp[], int cwd, int out )
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    sigset_t none, all;
    pid_t pid = 0;
    int err;

    sigemptyset( &none );
    sigfillset( &all );
    posix_spawn_file_actions_init( &fa );
    posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
    if (out >= 0) posix_spawn_file_actions_adddup2( &fa, out, 1 );
    else posix_spawn_file_actions_addopen( &fa, 1, "/dev/null", O_WRONLY, 0 );
    posix_spawn_file_actions_adddup2( &fa, 1, 2 );
    if (cwd >= 0) posix_spawn_file_actions_addfchdir_np( &fa, cwd );
    posix_spawnattr_init( &attr );
    posix_spawnattr_setsigmask( &attr, &none );
    posix_spawnattr_setsigdefault( &attr, &all );
    posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT |
                                     POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF );
    if ((err = posix_spawn( &pid, argv[0], &fa, &attr, argv, envp )))
    {
        logmsg( "cannot start %s: %s", argv[0], strerror( err ) );
        pid = 0;
    }
    posix_spawnattr_destroy( &attr );
    posix_spawn_file_actions_destroy( &fa );
    return pid;
}

static void stop_field( void *ctx, const char *key, const char *val, bool clean, bool in_array )
{
    if (!in_array && val && clean && !strcmp( key, "op" )) *(bool *)ctx = !strcmp( val, "stop" );
}

/* The whole lines that have arrived on the game's connection: a stop line is noted, anything else is
 * passed over. Called with the lock held. */
static void launch_lines( struct launch *l )
{
    char *nl;

    while ((nl = memchr( l->line, '\n', l->linelen )))
    {
        size_t len = nl - l->line;
        bool stop = false;

        if (utf8_valid( (unsigned char *)l->line, len ) && read_object( l->line, len, stop_field, &stop ) && stop)
            l->stop = true;
        l->linelen -= len + 1;
        memmove( l->line, nl + 1, l->linelen );
    }
}

/* Open a folder and say where it really is. The path is that of the folder that was opened, whatever
 * is done to the names leading to it meanwhile. -1 when it is no folder. */
static int open_folder( const char *path, char *real )
{
    int fd = open( path, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC );

    if (fd >= 0 && fcntl( fd, F_GETPATH, real ) == -1)
    {
        close( fd );
        fd = -1;
    }
    return fd;
}

/* Is the prefix still the folder it was when the game was started? The guest can put a link in its
 * place, and a wineserver command for the prefix would then go to whatever the link names. */
static bool prefix_in_place( const char *prefix )
{
    char real[PATH_MAX];

    return realpath( prefix, real ) && !strcmp( real, prefix );
}

/* Wait one tick for the guest to say something on the game's connection. A stop line is noted; a
 * connection that closes, or sends something that is no line of ours, is let go and the game kept. */
static void launch_serve( struct launch *l )
{
    struct pollfd pfd = { -1, POLLIN, 0 };
    bool detach, stop;
    ssize_t got;

    pthread_mutex_lock( &lock );
    pfd.fd = l->fd;  /* a later launch for the same game may have handed its connection over */
    detach = l->detach;
    /* what came in with the request itself: Stop pressed before the game had started */
    if (pfd.fd >= 0 && !detach) launch_lines( l );
    pthread_mutex_unlock( &lock );
    if (pfd.fd < 0)
    {
        usleep( LAUNCH_TICK_MS * 1000 );
        return;
    }
    if (!detach)
    {
        if (poll( &pfd, 1, LAUNCH_TICK_MS ) <= 0) return;
        got = recv( pfd.fd, l->line + l->linelen, sizeof(l->line) - l->linelen, 0 );
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) return;
        if (got > 0)
        {
            bool room;

            pthread_mutex_lock( &lock );
            l->linelen += got;
            /* once "detach" was asked for, nothing this connection says ends the game any more */
            if (!l->detach) launch_lines( l );
            room = l->linelen < sizeof(l->line);
            pthread_mutex_unlock( &lock );
            if (room) return;
        }
    }
    pthread_mutex_lock( &lock );
    l->fd = -1;
    l->linelen = 0;
    l->detach = false;
    stop = l->stop;
    pthread_mutex_unlock( &lock );
    close( pfd.fd );
    if (detach) logguest( "launch app %s: let go of the guest's connection; the game is kept", l->appid );
    else if (!stop) logguest( "launch app %s: the guest's connection is gone; the game is kept", l->appid );
}

/* "detach": have every game let go of its connection, and say how many games there are. From the moment
 * this returns no stop line on those connections is acted on; each is closed within a tick. */
static int detach_games( void )
{
    int i, games = 0;

    pthread_mutex_lock( &lock );
    for (i = 0; i < MAX_LAUNCHES; i++)
    {
        if (!launches[i].used) continue;
        games++;
        if (launches[i].fd >= 0) launches[i].detach = true;
    }
    pthread_mutex_unlock( &lock );
    return games;
}

/* Steam's Stop: have the prefix's wineserver end its programs, as "wineserver -k" does (it gives the
 * server ten seconds and then kills it), then kill what is left of the game's own process group. The
 * group is named by the wine process, which has not been reaped yet, so the number cannot have passed
 * to anything else. A prefix that is no longer the folder the game was started in gets no wineserver
 * command: only the group is killed. */
/* Is a process of the group still running? Its leader, the wine process, is left unreaped until the
 * game is over (so the number keeps naming the group) and is not counted once it has exited. When the
 * system cannot say, the group is taken to be running. */
static bool group_running( pid_t pgid )
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PGRP, pgid };
    struct kinfo_proc *procs = NULL, *more;
    size_t size = 0, i;
    bool running = false;
    int tries;

    for (tries = 0; tries < 4; tries++)
    {
        if (sysctl( mib, 4, NULL, &size, NULL, 0 )) break;
        size += 8 * sizeof(*procs);  /* room for a few that start meanwhile */
        if (!(more = realloc( procs, size ))) break;
        procs = more;
        if (!sysctl( mib, 4, procs, &size, NULL, 0 ))
        {
            for (i = 0; i < size / sizeof(*procs); i++)
                if (procs[i].kp_proc.p_stat != SZOMB) running = true;
            free( procs );
            return running;
        }
        if (errno != ENOMEM) break;
    }
    free( procs );
    return !(killpg( pgid, 0 ) && errno == ESRCH);
}

/* Steam's Stop for a game in the shared prefix: its process group is asked to end, and killed when it
 * has not after a few seconds. The prefix's wineserver and the other programs using it are left alone. */
static void launch_stop_group( struct launch *l )
{
    int64_t until = now_ms() + GROUP_STOP_WAIT_MS;
    siginfo_t info;

    logmsg( "launch app %s: stop (its own processes; the prefix is shared)", l->appid );
    killpg( l->pid, SIGTERM );
    while (now_ms() < until)
    {
        memset( &info, 0, sizeof(info) );
        if ((waitid( P_PID, l->pid, &info, WEXITED | WNOHANG | WNOWAIT ) || info.si_pid) && !group_running( l->pid ))
            return;
        usleep( 50000 );
    }
    killpg( l->pid, SIGKILL );
}

static void launch_stop( struct launch *l )
{
    char *argv[] = { l->wineserver, "-k", NULL };
    int64_t until = now_ms() + STOP_WAIT_MS;
    pid_t pid;

    if (l->shared)
    {
        launch_stop_group( l );
        return;
    }
    logmsg( "launch app %s: stop", l->appid );
    if (!prefix_in_place( l->prefix )) logmsg( "launch app %s: its prefix folder was moved; only its own processes are ended", l->appid );
    else if ((pid = spawn_wine( argv, l->env, -1, -1 )))
    {
        while (waitpid( pid, NULL, WNOHANG ) == 0)
        {
            if (now_ms() < until)
            {
                usleep( 50000 );
                continue;
            }
            kill( pid, SIGKILL );
            waitpid( pid, NULL, 0 );
            break;
        }
    }
    killpg( l->pid, SIGKILL );
}

/* Stay with a started game until it is over, then tell the guest how it ended. */
static void launch_run( struct launch *l )
{
    char *wait_argv[] = { l->wineserver, "-w", NULL };
    char reply[64], **env;
    bool exited = false, stopped = false, stop;
    int64_t stop_time = 0;
    pid_t waiter = 0;
    int fd, status = 0, code;

    for (;;)
    {
        launch_serve( l );
        pthread_mutex_lock( &lock );
        stop = l->stop;
        pthread_mutex_unlock( &lock );
        if (stop && !stopped)
        {
            launch_stop( l );
            stopped = true;
            stop_time = now_ms();
        }
        if (!exited)
        {
            /* (looked at, not reaped: its process id keeps naming the game's process group until the end) */
            siginfo_t info;

            memset( &info, 0, sizeof(info) );
            exited = waitid( P_PID, l->pid, &info, WEXITED | WNOHANG | WNOWAIT ) || info.si_pid;
        }
        if (l->shared)
        {
            /* the prefix's wineserver serves other programs too: the game is its own process group */
            if (exited && !group_running( l->pid )) break;
        }
        else if (exited && !waiter &&
            (!prefix_in_place( l->prefix ) || !(waiter = spawn_wine( wait_argv, l->env, -1, -1 )))) break;
        if (waiter && waitpid( waiter, NULL, WNOHANG ) != 0)
        {
            waiter = 0;
            break;
        }
        if (stopped && now_ms() - stop_time > STOP_WAIT_MS) break;  /* a stopped game does not hold on */
    }
    if (waiter)
    {
        kill( waiter, SIGKILL );
        waitpid( waiter, NULL, 0 );
    }
    if (stopped) killpg( l->pid, SIGKILL );
    while (waitpid( l->pid, &status, 0 ) < 0 && errno == EINTR) ;
    if (WIFEXITED( status )) code = WEXITSTATUS( status );
    else if (WIFSIGNALED( status )) code = 128 + WTERMSIG( status );  /* as a shell reports it */
    else code = 255;
    logmsg( "launch app %s: ended, exit %d", l->appid, code );

    pthread_mutex_lock( &lock );
    fd = l->fd;
    env = l->env;
    memset( l, 0, sizeof(*l) );
    pthread_mutex_unlock( &lock );
    free_environment( env );
    if (fd >= 0)
    {
        snprintf( reply, sizeof(reply), "{\"ok\": true, \"exit\": %d}\n", code );
        send_reply( fd, reply, now_ms() + REQUEST_DEADLINE_MS );
        close( fd );
    }
}

/* MetalSharp's Play (see the top of this file): the backend the MetalSharp application asked for this
 * app id, as an index into backends[], or -1 when there is no intent that stands. The file is opened
 * without following a link and judged by what was opened; a file of this user's is removed whether it
 * is used or too old or unreadable, so it applies to one launch at most. */
static int take_intent( const char *appid )
{
    static const char *const names[] = { "mullion", "dxvk", "vkd3d", "wine" };
    char dir[PATH_MAX], text[32], *slash;
    struct stat st;
    int dfd, fd, found = -1;
    ssize_t len;
    time_t now;
    size_t i;

    strlcpy( dir, socket_path, sizeof(dir) );
    if (!(slash = strrchr( dir, '/' ))) return -1;
    if (strlcpy( slash + 1, "launch-intent", sizeof(dir) - (slash + 1 - dir) ) >= sizeof(dir) - (slash + 1 - dir)) return -1;
    if ((dfd = open( dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC )) < 0) return -1;
    /* the folder is this user's and nobody else can put a file in it */
    if (fstat( dfd, &st ) || st.st_uid != getuid() || (st.st_mode & 022))
    {
        logmsg( "launch app %s: ignoring %s: not a private folder of this user's", appid, dir );
        close( dfd );
        return -1;
    }
    if ((fd = openat( dfd, appid, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC )) < 0)
    {
        if (errno != ENOENT) logmsg( "launch app %s: ignoring MetalSharp's launch intent: %s", appid, strerror( errno ) );
        close( dfd );
        return -1;
    }
    if (fstat( fd, &st ) || !S_ISREG( st.st_mode ) || st.st_uid != getuid())
    {
        logmsg( "launch app %s: ignoring MetalSharp's launch intent: not a file of this user's", appid );
        close( fd );
        close( dfd );
        return -1;
    }
    now = time( NULL );
    len = read( fd, text, sizeof(text) - 1 );
    close( fd );
    if (st.st_mtime < now - INTENT_MAX_AGE || st.st_mtime > now + 5)
        logmsg( "launch app %s: ignoring MetalSharp's launch intent: it is too old", appid );
    else if (len <= 0) logmsg( "launch app %s: ignoring MetalSharp's launch intent: it is empty", appid );
    else
    {
        text[len] = 0;
        while (len && strchr( "\n\r \t", text[len - 1] )) text[--len] = 0;
        for (i = 0; i < sizeof(names) / sizeof(names[0]) && found < 0; i++)
        {
            size_t b;
            if (strcmp( text, names[i] )) continue;
            for (b = 0; b < sizeof(backends) / sizeof(backends[0]); b++)
                if (!strcmp( backends[b].name, names[i] )) found = b;
        }
        if (found < 0) logmsg( "launch app %s: ignoring MetalSharp's launch intent: not a backend", appid );
    }
    unlinkat( dfd, appid, 0 );
    close( dfd );
    return found;
}

/* Check a "launch" request and start the game. An error text when it is refused. Otherwise the first
 * reply has been sent and the connection is no longer the caller's: *started is the game to stay with,
 * or NULL when the game was already running without a connection and has been given this one. more is
 * what arrived behind the request (a stop line can be right behind it); it goes with the connection. */
static const char *launch( const struct config *cfg, const struct request *rq, int fd, int64_t deadline,
                           const char *more, size_t more_len, struct launch **started )
{
    static const char started_reply[] = "{\"ok\": true, \"state\": \"started\"}\n";
    static const char bad_exe[] = "the program is not an .exe file inside a shared folder";
    static const char bad_cwd[] = "the working folder is not inside a shared folder";
    static const char bad_data[] = "the compatibility data folder is not usable";
    char exe[PATH_MAX], cwd[PATH_MAX], compat[PATH_MAX], check[PATH_MAX], prefix[PATH_MAX], wineserver[PATH_MAX];
    char *argv[MAX_LAUNCH_ARGS + 3], *slash, **env = NULL;
    const char *backend = NULL, *marker = NULL, *error = NULL, *p;
    struct launch *l = NULL, *same = NULL;
    bool shared_prefix = cfg->game_prefix[0] != 0;
    int out = -1, cwd_fd = -1, data_fd = -1, intent;
    struct stat st;
    int64_t now;
    size_t i, len, i_backend;
    pid_t pid = 0;

    *started = NULL;
    if (!cfg->launch) return "launching games is turned off";
    if (rq->launch_bad) return "not a usable launch request";
    if (rq->args_bad) return "too many or unusable arguments";
    for (i = 0; i < sizeof(backends) / sizeof(backends[0]); i++)
        if (!strcmp( rq->backend[0] ? rq->backend : "mullion", backends[i].name )) break;
    if (i == sizeof(backends) / sizeof(backends[0])) return "unknown graphics backend";
    backend = backends[i].value;
    marker = backends[i].marker;
    i_backend = i;
    if (!digits( rq->appid, 10 ) || (rq->gameid[0] && !digits( rq->gameid, 20 ))) return "not a usable app id";
    if (more_len > sizeof(l->line)) more_len = 0;  /* no line of ours is that long */

    /* Each of these replies is the same whether the path exists or not, so the guest learns nothing
     * about the Mac outside the shares. */
    if (rq->exe[0] != '/' || !realpath( rq->exe, exe ) || !shared( cfg, exe ) ||
        stat( exe, &st ) || !S_ISREG( st.st_mode ) || (len = strlen( exe )) < 5 || strcasecmp( exe + len - 4, ".exe" ))
        return bad_exe;
    if (rq->cwd[0])
    {
        if (rq->cwd[0] != '/') return bad_cwd;
        strcpy( check, rq->cwd );
    }
    else
    {
        strcpy( check, exe );
        if (!(slash = strrchr( check, '/' )) || slash == check) return bad_cwd;
        *slash = 0;
    }
    if ((cwd_fd = open_folder( check, cwd )) < 0 || !shared( cfg, cwd )) error = bad_cwd;
    else if (rq->compat_data[0] != '/' || (data_fd = open_folder( rq->compat_data, compat )) < 0 || !shared( cfg, compat ))
        error = bad_data;
    else if (!(slash = strrchr( compat, '/' )) || !digits( slash + 1, 20 )) error = bad_data;
    else
    {
        p = "/steamapps/compatdata";
        if ((size_t)(slash - compat) < strlen( p ) || strncmp( slash - strlen( p ), p, strlen( p ) )) error = bad_data;
        /* the shared prefix is the Mac's choice; it is not created here */
        else if (shared_prefix)
        {
            if (!realpath( cfg->game_prefix, prefix ) || !is_dir( prefix )) error = "the game prefix is not usable";
        }
        else if (snprintf( prefix, sizeof(prefix), "%s/pfx", compat ) >= (int)sizeof(prefix)) error = bad_data;
    }
    if (error) goto done;

    /* wineserver is the one beside wine, which is also where wine looks for it first */
    strlcpy( wineserver, cfg->wine, sizeof(wineserver) );
    if (!(slash = strrchr( wineserver, '/' ))) error = "Wine is not available";
    else
    {
        *slash = 0;
        if (strlcat( wineserver, "/wineserver", sizeof(wineserver) ) >= sizeof(wineserver) ||
            access( cfg->wine, X_OK ) || access( wineserver, X_OK )) error = "Wine is not available";
    }
    if (error) goto done;

    now = now_ms();
    pthread_mutex_lock( &lock );
    for (i = 0; i < MAX_LAUNCHES; i++)
    {
        if (!launches[i].used)
        {
            if (!l) l = &launches[i];
        }
        /* a game is its prefix, or in the shared prefix its app id there */
        else if (!strcmp( launches[i].prefix, prefix ) &&
                 (!launches[i].shared || !shared_prefix || !strcmp( launches[i].appid, rq->appid ))) same = &launches[i];
    }
    /* A game that is running already is refused while a guest is attached to it (or it is being
     * stopped). Without one, the VM or Steam was restarted under it, and this Play becomes its
     * connection: Steam shows it running again and can stop it. Joining is held to the same interval as
     * starting, so a guest cannot take up and drop a game in a loop. (The few bytes of the reply go into
     * an empty socket buffer.) */
    if (same && (same->fd >= 0 || same->stop || !same->pid)) error = "this game is already running";
    else if (!same && !l) error = "too many games running";
    else if (now - last_launch_ms < MIN_LAUNCH_INTERVAL_MS) error = "too many requests";
    else if (same)
    {
        if (send( fd, started_reply, sizeof(started_reply) - 1, 0 ) != sizeof(started_reply) - 1)
            error = "could not be started";
        else
        {
            last_launch_ms = now;
            memcpy( same->line, more, more_len );
            same->linelen = more_len;
            same->detach = false;
            same->fd = fd;
        }
    }
    else
    {
        last_launch_ms = now;
        memset( l, 0, sizeof(*l) );
        l->used = true;  /* the place and the prefix are taken while the game is started */
        l->fd = fd;
        l->shared = shared_prefix;
        strcpy( l->prefix, prefix );
        strcpy( l->wineserver, wineserver );
        strlcpy( l->appid, rq->appid, sizeof(l->appid) );
        memcpy( l->line, more, more_len );
        l->linelen = more_len;
    }
    pthread_mutex_unlock( &lock );
    if (error) goto done;
    if (same)
    {
        take_intent( rq->appid );  /* (the game runs on what it was started with; the intent is spent) */
        logguest( "launch app %s: joined the game already running", rq->appid );
        goto done;
    }
    /* MetalSharp's Play: the backend chosen there wins over the one Steam's tool asks for */
    if ((intent = take_intent( rq->appid )) >= 0)
    {
        logmsg( "launch app %s: MetalSharp asked for %s (Steam's tool: %s)", rq->appid, backends[intent].name,
                backends[i_backend].name );
        backend = backends[intent].value;
        marker = backends[intent].marker;
    }

    /* Only now, with a place taken and the interval started, is anything made on disk: a refused
     * request leaves nothing behind, and a guest cannot try again at once. */
    argv[0] = (char *)cfg->wine;
    argv[1] = exe;
    for (i = 0; i < rq->nargs; i++) argv[i + 2] = rq->args[i];
    argv[i + 2] = NULL;
    if (!shared_prefix && ((mkdirat( data_fd, "pfx", 0755 ) && errno != EEXIST) ||
                           fstatat( data_fd, "pfx", &st, AT_SYMLINK_NOFOLLOW ) || !S_ISDIR( st.st_mode ))) error = bad_data;
    else if (!(env = launch_environment( cfg, rq, backend, marker, prefix, compat,
                                         shared_prefix ? cfg->game_user : "steamuser" ))) error = "could not be started";
    /* the guest can write to the share, so look once more right before starting (see open_path) */
    else if (!realpath( rq->exe, check ) || strcmp( check, exe ) || !prefix_in_place( prefix ))
        error = "the game's files changed underneath";
    else
    {
        if (cfg->launch_log[0] &&
            (out = open( cfg->launch_log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600 )) < 0)
            logmsg( "cannot write %s: %s", cfg->launch_log, strerror( errno ) );
        if (!(pid = spawn_wine( argv, env, cwd_fd, out ))) error = "could not be started";
        if (out >= 0) close( out );
    }
    if (error)
    {
        free_environment( env );
        pthread_mutex_lock( &lock );
        memset( l, 0, sizeof(*l) );
        pthread_mutex_unlock( &lock );
        goto done;
    }
    l->env = env;
    pthread_mutex_lock( &lock );
    l->pid = pid;
    pthread_mutex_unlock( &lock );
    logmsg( "launch app %s (%s%s): %s", rq->appid, backend ? backend : "mullion", shared_prefix ? ", shared prefix" : "", exe );
    send_reply( fd, started_reply, deadline );
    *started = l;
done:
    if (cwd_fd >= 0) close( cwd_fd );
    if (data_fd >= 0) close( data_fd );
    return error;
}

static void *handle( void *arg )
{
    int fd = (int)(intptr_t)arg;
    int64_t deadline = now_ms() + REQUEST_DEADLINE_MS;
    char *data = malloc( MAX_REQUEST ), reply[128];
    const char *error = "malformed request", *nl;
    struct launch *game = NULL;
    struct request *rq;
    struct config *cfg;
    bool answered = false;
    size_t len = 0, more = 0;
    int games = -1;

    if (data) len = read_request( fd, data, deadline );
    if (len && (nl = memchr( data, '\n', len )))
    {
        more = len - (nl + 1 - data);  /* a stop line can be right behind a "launch" */
        len = nl - data;
    }
    rq = calloc( 1, sizeof(*rq) );
    cfg = calloc( 1, sizeof(*cfg) );
    if (!data || !rq || !cfg) error = "internal error";
    else if (utf8_valid( (unsigned char *)data, len ) && read_object( data, len, request_field, rq ))
    {
        if (!strcmp( rq->op, "ping" )) error = NULL;
        else if (!strcmp( rq->op, "open-path" ))
        {
            load_config( cfg );
            error = open_path( cfg, rq );
        }
        else if (!strcmp( rq->op, "open-url" )) error = open_url( rq );
        else if (!strcmp( rq->op, "choose-library" ))
        {
            load_config( cfg );
            error = choose_library( cfg );
        }
        else if (!strcmp( rq->op, "launch" ))
        {
            load_config( cfg );
            answered = !(error = launch( cfg, rq, fd, deadline, more ? data + len + 1 : data, more, &game ));
        }
        else if (!strcmp( rq->op, "detach" ))
        {
            games = detach_games();
            error = NULL;
        }
        else error = "unknown request";
    }
    free( data );
    free( rq );
    free( cfg );
    if (!answered)
    {
        if (error)
        {
            logguest( "refused: %s", error );
            snprintf( reply, sizeof(reply), "{\"ok\": false, \"error\": \"%s\"}\n", error );  /* fixed texts only */
        }
        else if (games >= 0) snprintf( reply, sizeof(reply), "{\"ok\": true, \"games\": %d}\n", games );
        else strcpy( reply, "{\"ok\": true}\n" );
        send_reply( fd, reply, deadline );
        close( fd );
    }
    /* the deadline was for the request and its first reply; a game is stayed with for as long as it runs */
    if (game) launch_run( game );
    pthread_mutex_lock( &lock );
    active_threads--;
    pthread_mutex_unlock( &lock );
    return NULL;
}

/* ---- start-up ---- */

static void die( const char *fmt, ... )
{
    va_list ap;

    fprintf( stderr, "metalsharp-vmd: " );
    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
    fprintf( stderr, "\n" );
    exit( 1 );
}

static void set_paths( void )
{
    char exe[PATH_MAX], real[PATH_MAX], *slash;
    uint32_t size = sizeof(exe);
    const char *home = getenv( "HOME" ), *tmp;
    struct passwd *pw;

    if ((!home || home[0] != '/') && (pw = getpwuid( getuid() ))) home = pw->pw_dir;
    if (!home || home[0] != '/') die( "no home directory" );
    if (strlcpy( home_dir, home, PATH_MAX ) >= PATH_MAX) die( "home directory path too long" );
    /* games get the user's own temporary folder, as any program started from the desktop does */
    if (!(tmp = getenv( "TMPDIR" )) || tmp[0] != '/' || strlcpy( temp_dir, tmp, PATH_MAX ) >= PATH_MAX)
    {
        size_t len = confstr( _CS_DARWIN_USER_TEMP_DIR, temp_dir, PATH_MAX );
        if (!len || len > PATH_MAX) strcpy( temp_dir, "/tmp" );
    }
    if (!socket_path[0] && snprintf( socket_path, PATH_MAX, "%s/.metalsharp/run/launch.sock", home ) >= PATH_MAX)
        die( "home directory path too long" );
    if (!config_path[0]) snprintf( config_path, PATH_MAX, "%s/.metalsharp/vm/bridge.json", home );
    snprintf( default_prefix, PATH_MAX, "%s/.metalsharp/vm/utility-pfx", home );
    /* the wine this program was shipped with is the one next to it */
    if (!_NSGetExecutablePath( exe, &size ) && realpath( exe, real ) && (slash = strrchr( real, '/' )))
    {
        *slash = 0;
        snprintf( default_wine, PATH_MAX, "%s/wine", real );
    }
}

/* create the directory a path is in, private to the user, with its parents */
static void make_parent( const char *path )
{
    char dir[PATH_MAX], *p;

    strlcpy( dir, path, sizeof(dir) );
    if (!(p = strrchr( dir, '/' )) || p == dir) return;
    *p = 0;
    for (p = dir + 1; ; p++)
    {
        if (*p && *p != '/') continue;
        char c = *p;
        *p = 0;
        if (mkdir( dir, 0700 ) && errno != EEXIST) die( "cannot create %s: %s", dir, strerror( errno ) );
        if (!c) break;
        *p = c;
    }
}

int main( int argc, char **argv )
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    char lock_path[PATH_MAX + 8];
    struct config *cfg;
    pthread_attr_t attr;
    mode_t old;
    int i, srv, lock_fd, on = 1;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp( argv[i], "--socket" ) && i + 1 < argc) strlcpy( socket_path, argv[++i], PATH_MAX );
        else if (!strcmp( argv[i], "--config" ) && i + 1 < argc) strlcpy( config_path, argv[++i], PATH_MAX );
        else
        {
            fprintf( stderr, "usage: metalsharp-vmd [--socket PATH] [--config PATH]\n" );
            return 2;
        }
    }
    set_paths();
    if (strlen( socket_path ) >= sizeof(addr.sun_path)) die( "socket path too long: %s", socket_path );
    signal( SIGPIPE, SIG_IGN );
    /* Whoever starts this program may keep its own files private (metalsharp-steam does); what is started
     * from here, games above all, creates files the way the user's programs normally do. Everything this
     * program creates itself names its permissions. */
    umask( 022 );
    make_parent( socket_path );
    if (chdir( "/" )) die( "chdir: %s", strerror( errno ) );

    /* one daemon per socket: the lock is held for as long as this process lives */
    snprintf( lock_path, sizeof(lock_path), "%s.lock", socket_path );
    if ((lock_fd = open( lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600 )) < 0) die( "cannot open %s: %s", lock_path, strerror( errno ) );
    if (flock( lock_fd, LOCK_EX | LOCK_NB ))
    {
        fprintf( stderr, "metalsharp-vmd: already running for %s\n", socket_path );
        return 0;
    }

    unlink( socket_path );
    strlcpy( addr.sun_path, socket_path, sizeof(addr.sun_path) );
    if ((srv = socket( AF_UNIX, SOCK_STREAM, 0 )) < 0) die( "socket: %s", strerror( errno ) );
    fcntl( srv, F_SETFD, FD_CLOEXEC );
    old = umask( 0177 );  /* the socket is ours alone */
    if (bind( srv, (struct sockaddr *)&addr, sizeof(addr) )) die( "cannot bind %s: %s", socket_path, strerror( errno ) );
    umask( old );
    if (listen( srv, 16 )) die( "listen: %s", strerror( errno ) );

    if (!(cfg = calloc( 1, sizeof(*cfg) ))) die( "out of memory" );
    load_config( cfg );
    logmsg( "listening on %s; %d shared folder%s; file manager: %s; game launches: %s", socket_path, cfg->nroots,
            cfg->nroots == 1 ? "" : "s", cfg->finder ? "finder" : "wine", cfg->launch ? "on" : "off" );
    for (i = 0; i < cfg->nroots; i++) logmsg( "  shared: %s", cfg->roots[i] );
    free( cfg );

    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    pthread_attr_setstacksize( &attr, 1024 * 1024 );  /* a handler keeps a few paths on its stack; requests are on the heap */
    for (;;)
    {
        pthread_t thread;
        bool room;
        int fd = accept( srv, NULL, NULL );

        if (fd < 0)
        {
            if (errno != EINTR)
            {
                logmsg( "accept error: %s", strerror( errno ) );
                usleep( 100000 );
            }
            continue;
        }
        fcntl( fd, F_SETFD, FD_CLOEXEC );
        setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on) );
        pthread_mutex_lock( &lock );
        if ((room = active_threads < MAX_THREADS)) active_threads++;
        pthread_mutex_unlock( &lock );
        /* at capacity: drop the connection rather than pile up threads */
        if (room && !pthread_create( &thread, &attr, handle, (void *)(intptr_t)fd )) continue;
        if (room)
        {
            pthread_mutex_lock( &lock );
            active_threads--;
            pthread_mutex_unlock( &lock );
        }
        close( fd );
    }
}
