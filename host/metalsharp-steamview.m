/*
 * metalsharp-steamview: Linux Steam's windows on the Mac.
 *
 * Steam runs in the Linux virtual machine and draws on an X server there (Xvnc). This program is the
 * Mac side of that screen. It speaks RFB 3.8 (the VNC protocol) to the guest's screen, which
 * metalsharp-steam forwards to a port on this Mac only, and signs in by itself with the password
 * metalsharp-steam made, so there is nothing to type and no separate viewer.
 *
 * With the three --ssh options it also runs the guest's window agent (mtsp-windows) over ssh. The agent
 * lays Steam's windows out on one large guest screen so that they never overlap and reports where each
 * one is; every Steam window then gets a Mac window of its own, exactly its size, that shows that part of
 * the screen. Where a window sits on the guest's screen is never seen: the Mac windows go wherever the
 * user puts them. Menus, dropdowns and tooltips are borderless windows attached to the window they came
 * from. Steam closes to its tray, so the program keeps running with no windows; a click on the Dock icon
 * asks Steam for its main window again.
 *
 * Without those options (or if the agent never answers) there is one window showing the whole screen,
 * and resizing it resizes the guest's screen.
 *
 *     metalsharp-steamview --port 5901 --password-file ~/.metalsharp/vm/vnc-password.txt
 *                          [--ssh-port 2222 --ssh-key FILE --known-hosts FILE]   (one Mac window per Steam window)
 *                          [--steam-command PATH]   (metalsharp-steam: gives the Library menu)
 *                          [--title Steam] [--snapshot FILE.png]   (write the first picture and exit)
 */
#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonCryptor.h>
#import <QuartzCore/QuartzCore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_DIMENSION   16384
#define MAX_CUT_TEXT    (4 * 1024 * 1024)
#define RETRY_SECONDS   2
#define SEND_SECONDS    3            /* a guest that takes nothing for this long is treated as gone */
#define SNAPSHOT_SECONDS 30          /* how long --snapshot waits for a picture */
#define MAX_WINDOWS     64           /* what the agent may have open at once */
#define MAX_TITLE       256
#define MAX_AGENT_LINE  65536
#define AGENT_GRACE     8            /* seconds to wait for the agent before showing the whole screen instead */
#define NEW_WINDOW_TIME 2.0          /* a window this young may take the keyboard when the guest focuses it */
#define MAX_CURSOR_POINTS 32         /* no guest cursor is drawn larger than this */
#define RESIZE_ANSWER   0.6          /* seconds the guest has to report a new size after it was asked for one */
#define CASCADE_STEP    26           /* how far a new window steps down and right from the one before */
#define COMMAND_TAIL    16384        /* how much of metalsharp-steam's output is kept: the end of it */
#define COMMAND_LINES   12           /* how many of its last lines an alert shows */

enum { ENC_RAW = 0, ENC_COPYRECT = 1, ENC_CURSOR = -239, ENC_DESKTOP_SIZE = -223, ENC_EXT_DESKTOP_SIZE = -308 };

static int opt_port = 5901, opt_ssh_port;
static NSString *opt_password_file, *opt_title = @"Steam", *opt_snapshot, *opt_ssh_key, *opt_known_hosts;
static NSString *opt_steam_command;  /* metalsharp-steam, for the Library menu */
static BOOL opt_agent;               /* all three ssh options were given */

static NSColor *steam_colour( void )
{
    return [NSColor colorWithSRGBRed:0.09 green:0.114 blue:0.145 alpha:1];  /* Steam's own */
}

/* New windows open on the primary display, the one with the menu bar. That is the first of the screens:
 * mainScreen is the one with the keyboard's window and -center picks by where the window already is. Its
 * frame starts at (0,0); the other displays' frames are relative to it and can be negative. */
static NSRect primary_visible( void )
{
    NSScreen *primary = NSScreen.screens.firstObject ?: NSScreen.mainScreen;
    return primary ? primary.visibleFrame : NSMakeRect( 0, 0, 1280, 800 );
}

/* in the middle from side to side and a little above it from top to bottom, as -center has it; one that is
 * too large keeps its title bar on the display */
static void centre_on_primary( NSWindow *window )
{
    NSRect visible = primary_visible(), frame = window.frame;

    frame.origin.x = round( NSMidX( visible ) - NSWidth( frame ) / 2 );
    frame.origin.y = round( NSMinY( visible ) + (NSHeight( visible ) - NSHeight( frame )) * 2 / 3 );
    if (NSMinX( frame ) < NSMinX( visible )) frame.origin.x = NSMinX( visible );
    if (NSMaxY( frame ) > NSMaxY( visible )) frame.origin.y = NSMaxY( visible ) - NSHeight( frame );
    [window setFrameOrigin:frame.origin];
}

/* ---- the connection to the guest's screen ---- */

@protocol ScreenDelegate
- (void)screenChanged:(NSRect)dirty;         /* new pixels inside this rectangle (main thread, coalesced) */
- (void)screenResized:(NSSize)size first:(BOOL)first;
- (void)screenCursor:(NSCursor *)cursor;
- (void)screenText:(NSString *)text;         /* the guest's clipboard */
- (void)screenStatus:(NSString *)status;     /* nil when connected */
- (void)screenCanResize;                     /* the server has said it takes size requests (once a connection) */
@end

@interface Screen : NSObject
@property (weak) id<ScreenDelegate> delegate;
- (void)start;
- (CGImageRef)copyImage CF_RETURNS_RETAINED;
- (CGImageRef)copyImageRect:(NSRect)rect CF_RETURNS_RETAINED;
- (NSSize)size;
- (void)pointerX:(int)x y:(int)y buttons:(int)mask;
- (void)key:(uint32_t)keysym down:(BOOL)down;
- (void)cutText:(NSString *)text;
- (void)requestSize:(NSSize)size;
@end

@implementation Screen
{
    pthread_mutex_t fbLock, sendLock;
    uint8_t *fb;                 /* BGRX, fbw * fbh * 4 */
    int fbw, fbh;
    int dirty[4];                /* left, top, right, bottom of what changed since the delegate was last told */
    int fd;                      /* -1 when not connected; guarded by sendLock for writers */
    BOOL canResize;              /* the server offered ExtendedDesktopSize */
    uint32_t screenId;
    BOOL pending;                /* a screenChanged is already queued */
}

- (instancetype)init
{
    if ((self = [super init]))
    {
        pthread_mutex_init( &fbLock, NULL );
        pthread_mutex_init( &sendLock, NULL );
        fd = -1;
    }
    return self;
}

- (void)start
{
    [NSThread detachNewThreadSelector:@selector(run) toTarget:self withObject:nil];
}

static BOOL read_all( int fd, void *buf, size_t len )
{
    uint8_t *p = buf;
    while (len)
    {
        ssize_t n = recv( fd, p, len, 0 );
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR) continue;
            return NO;
        }
        p += n;
        len -= n;
    }
    return YES;
}

static BOOL skip_bytes( int fd, size_t len )
{
    uint8_t buf[4096];
    while (len)
    {
        size_t n = MIN( len, sizeof(buf) );
        if (!read_all( fd, buf, n )) return NO;
        len -= n;
    }
    return YES;
}

- (BOOL)send:(const void *)data length:(size_t)len
{
    const uint8_t *p = data;
    BOOL ok = YES;

    pthread_mutex_lock( &sendLock );
    if (fd < 0) ok = NO;
    while (ok && len)
    {
        ssize_t n = send( fd, p, len, 0 );
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR) continue;
            /* This includes the send timeout. Part of a message may have gone, so the connection cannot be
             * used again: shutting it down wakes the reader, which closes it and connects anew. */
            shutdown( fd, SHUT_RDWR );
            ok = NO;
        }
        else { p += n; len -= n; }
    }
    pthread_mutex_unlock( &sendLock );
    return ok;
}

static uint16_t be16( const uint8_t *p ) { return (p[0] << 8) | p[1]; }
static uint32_t be32( const uint8_t *p ) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void put16( uint8_t *p, uint16_t v ) { p[0] = v >> 8; p[1] = v; }
static void put32( uint8_t *p, uint32_t v ) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

- (void)status:(NSString *)text
{
    dispatch_async( dispatch_get_main_queue(), ^{ [self.delegate screenStatus:text]; } );
}

- (void)run
{
    for (;;)
    {
        @autoreleasepool
        {
            NSString *why = [self session];
            pthread_mutex_lock( &sendLock );
            if (fd >= 0) close( fd );
            fd = -1;
            pthread_mutex_unlock( &sendLock );
            [self status:why ?: @"Steam's screen went away"];
        }
        sleep( RETRY_SECONDS );
    }
}

/* VNC authentication: the 16-byte challenge encrypted with DES, the key being the password with the bits
 * of each byte reversed */
static NSData *read_password( void )
{
    NSString *text = [NSString stringWithContentsOfFile:opt_password_file encoding:NSUTF8StringEncoding error:NULL];
    return [[text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
               dataUsingEncoding:NSISOLatin1StringEncoding allowLossyConversion:YES];
}

static BOOL vnc_response( NSData *pw, const uint8_t challenge[16], uint8_t response[16] )
{
    uint8_t key[8] = {0};
    size_t done = 0;

    if (!pw.length) return NO;
    for (NSUInteger i = 0; i < 8 && i < pw.length; i++)
    {
        uint8_t c = ((const uint8_t *)pw.bytes)[i], r = 0;
        for (int b = 0; b < 8; b++) if (c & (1 << b)) r |= 0x80 >> b;
        key[i] = r;
    }
    return CCCrypt( kCCEncrypt, kCCAlgorithmDES, kCCOptionECBMode, key, 8, NULL, challenge, 16, response, 16, &done ) == kCCSuccess
           && done == 16;
}

/* with fbLock held */
- (void)addDirtyX:(int)x y:(int)y width:(int)w height:(int)h
{
    if (dirty[2] <= dirty[0] || dirty[3] <= dirty[1])
    {
        dirty[0] = x; dirty[1] = y; dirty[2] = x + w; dirty[3] = y + h;
        return;
    }
    dirty[0] = MIN( dirty[0], x ); dirty[1] = MIN( dirty[1], y );
    dirty[2] = MAX( dirty[2], x + w ); dirty[3] = MAX( dirty[3], y + h );
}

- (void)resizeTo:(int)w height:(int)h first:(BOOL)first
{
    pthread_mutex_lock( &fbLock );
    free( fb );
    fb = calloc( (size_t)w * h, 4 );
    fbw = fb ? w : 0;
    fbh = fb ? h : 0;
    dirty[0] = dirty[1] = 0; dirty[2] = fbw; dirty[3] = fbh;
    pthread_mutex_unlock( &fbLock );
    dispatch_async( dispatch_get_main_queue(), ^{ [self.delegate screenResized:NSMakeSize( w, h ) first:first]; } );
}

- (BOOL)requestUpdate:(BOOL)incremental
{
    uint8_t msg[10] = { 3, incremental };
    int w, h;

    pthread_mutex_lock( &fbLock );
    w = fbw; h = fbh;
    pthread_mutex_unlock( &fbLock );
    put16( msg + 6, w );
    put16( msg + 8, h );
    return [self send:msg length:sizeof(msg)];
}

/* One connection, from the handshake until it drops. Returns what to show while not connected. */
- (NSString *)session
{
    struct sockaddr_in addr = { .sin_len = sizeof(addr), .sin_family = AF_INET, .sin_port = htons( opt_port ),
                                .sin_addr.s_addr = htonl( INADDR_LOOPBACK ) };
    static const int32_t encodings[] = { ENC_COPYRECT, ENC_RAW, ENC_CURSOR, ENC_DESKTOP_SIZE, ENC_EXT_DESKTOP_SIZE };
    uint8_t buf[256], count, type = 0;  /* holds the longest list of security types there can be */
    int s, on = 1, idle = 10, interval = 5, probes = 3, w, h;
    struct timeval timeout = { .tv_sec = SEND_SECONDS };
    uint32_t len;
    /* Read before connecting: when the file is on a disk macOS asks the user about first, the read waits
     * for their answer, and the guest's screen gives up on a sign-in left half done for that long. */
    NSData *password = read_password();

    if ((s = socket( AF_INET, SOCK_STREAM, 0 )) < 0) return @"Cannot open a socket";
    setsockopt( s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on) );
    setsockopt( s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on) );
    /* input is sent from the main thread: a guest that stops reading must not hang the program */
    setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout) );
    /* a screen that does not change sends nothing, so only a probe tells a quiet peer from a dead one */
    setsockopt( s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on) );
    setsockopt( s, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle) );
    setsockopt( s, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval) );
    setsockopt( s, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes) );
    if (connect( s, (struct sockaddr *)&addr, sizeof(addr) ))
    {
        close( s );
        return @"Steam is not running yet\nStart it with: metalsharp-steam start";
    }
    pthread_mutex_lock( &sendLock );
    fd = s;
    pthread_mutex_unlock( &sendLock );
    canResize = NO;

    /* version and security */
    if (!read_all( s, buf, 12 ) || memcmp( buf, "RFB 003.", 8 )) return @"Steam's screen is not answering yet";
    if (![self send:"RFB 003.008\n" length:12] || !read_all( s, &count, 1 )) return nil;
    if (!count || !read_all( s, buf, count )) return @"Steam's screen refused the connection";
    for (int i = 0; i < count; i++) if (buf[i] == 1 || (buf[i] == 2 && type != 1)) type = buf[i];
    if (!type) return @"Steam's screen wants a sign-in this program does not know";
    if (![self send:&type length:1]) return nil;
    if (type == 2)
    {
        uint8_t challenge[16], response[16];
        if (!read_all( s, challenge, 16 )) return nil;
        if (!vnc_response( password, challenge, response )) return @"The screen password file is missing\nRun: metalsharp-steam start";
        if (![self send:response length:16]) return nil;
    }
    if (!read_all( s, buf, 4 )) return nil;
    if (be32( buf )) return @"Steam's screen did not accept the password";

    /* ClientInit (shared), ServerInit */
    buf[0] = 1;
    if (![self send:buf length:1] || !read_all( s, buf, 24 )) return nil;
    w = be16( buf ); h = be16( buf + 2 ); len = be32( buf + 20 );
    if (!w || !h || w > MAX_DIMENSION || h > MAX_DIMENSION || len > 4096 || !skip_bytes( s, len )) return nil;
    [self resizeTo:w height:h first:YES];

    /* 32 bits a pixel, little-endian, blue in the low byte: BGRX in memory */
    memset( buf, 0, 20 );
    buf[4] = 32; buf[5] = 24; buf[6] = 0; buf[7] = 1;
    put16( buf + 8, 255 ); put16( buf + 10, 255 ); put16( buf + 12, 255 );
    buf[14] = 16; buf[15] = 8; buf[16] = 0;
    if (![self send:buf length:20]) return nil;
    buf[0] = 2; buf[1] = 0;
    put16( buf + 2, sizeof(encodings) / sizeof(encodings[0]) );
    for (size_t i = 0; i < sizeof(encodings) / sizeof(encodings[0]); i++) put32( buf + 4 + 4 * i, encodings[i] );
    if (![self send:buf length:4 + sizeof(encodings)]) return nil;
    if (![self requestUpdate:NO]) return nil;
    [self status:nil];

    /* a pool for each message: the thread's own is not drained until the connection drops */
    for (;;) @autoreleasepool
    {
        uint8_t kind;
        if (!read_all( s, &kind, 1 )) return nil;
        switch (kind)
        {
        case 0:  /* FramebufferUpdate */
        {
            int rects;
            if (!read_all( s, buf, 3 )) return nil;
            rects = be16( buf + 1 );
            while (rects--) if (![self readRect:s]) return nil;
            if (![self requestUpdate:YES]) return nil;
            pthread_mutex_lock( &fbLock );
            BOOL queue = !pending;
            pending = YES;
            pthread_mutex_unlock( &fbLock );
            if (queue) dispatch_async( dispatch_get_main_queue(), ^{
                NSRect changed;
                pthread_mutex_lock( &self->fbLock );
                self->pending = NO;
                changed = NSMakeRect( self->dirty[0], self->dirty[1], MAX( 0, self->dirty[2] - self->dirty[0] ),
                                      MAX( 0, self->dirty[3] - self->dirty[1] ) );
                memset( self->dirty, 0, sizeof(self->dirty) );
                pthread_mutex_unlock( &self->fbLock );
                if (!NSIsEmptyRect( changed )) [self.delegate screenChanged:changed];
            } );
            break;
        }
        case 1:  /* SetColourMapEntries: not used with true colour */
            if (!read_all( s, buf, 5 ) || !skip_bytes( s, be16( buf + 3 ) * 6 )) return nil;
            break;
        case 2:  /* Bell */
            dispatch_async( dispatch_get_main_queue(), ^{ NSBeep(); } );
            break;
        case 3:  /* ServerCutText */
        {
            NSMutableData *text;
            if (!read_all( s, buf, 7 )) return nil;
            len = be32( buf + 3 );
            if (len > MAX_CUT_TEXT) { if (!skip_bytes( s, len )) return nil; break; }
            text = [NSMutableData dataWithLength:len];
            if (!read_all( s, text.mutableBytes, len )) return nil;
            NSString *str = [[NSString alloc] initWithData:text encoding:NSISOLatin1StringEncoding];
            if (str) dispatch_async( dispatch_get_main_queue(), ^{ [self.delegate screenText:str]; } );
            break;
        }
        default:
            return nil;  /* nothing else was asked for */
        }
    }
}

/* with fbLock held: is this rectangle inside a framebuffer that exists */
- (BOOL)holdsX:(int)x y:(int)y width:(int)w height:(int)h
{
    return fb && x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= fbw && y + h <= fbh;
}

- (BOOL)readRect:(int)s
{
    uint8_t head[12];
    int x, y, w, h;
    int32_t enc;

    if (!read_all( s, head, 12 )) return NO;
    x = be16( head ); y = be16( head + 2 ); w = be16( head + 4 ); h = be16( head + 6 );
    enc = (int32_t)be32( head + 8 );
    switch (enc)
    {
    case ENC_RAW:
    {
        size_t bytes = (size_t)w * h * 4;
        uint8_t *pixels;
        BOOL fits;
        if (!bytes) return YES;
        /* a rectangle that is not on the screen is read and dropped, never given memory of its size */
        pthread_mutex_lock( &fbLock );
        fits = [self holdsX:x y:y width:w height:h];
        pthread_mutex_unlock( &fbLock );
        if (!fits) return skip_bytes( s, bytes );
        if (!(pixels = malloc( bytes ))) return NO;
        if (!read_all( s, pixels, bytes )) { free( pixels ); return NO; }
        pthread_mutex_lock( &fbLock );
        if ([self holdsX:x y:y width:w height:h])
        {
            for (int row = 0; row < h; row++)
                memcpy( fb + ((size_t)(y + row) * fbw + x) * 4, pixels + (size_t)row * w * 4, (size_t)w * 4 );
            [self addDirtyX:x y:y width:w height:h];
        }
        pthread_mutex_unlock( &fbLock );
        free( pixels );
        return YES;
    }
    case ENC_COPYRECT:
    {
        int sx, sy;
        if (!read_all( s, head, 4 )) return NO;
        sx = be16( head ); sy = be16( head + 2 );
        pthread_mutex_lock( &fbLock );
        if ([self holdsX:x y:y width:w height:h] && [self holdsX:sx y:sy width:w height:h])
        {
            /* the areas may overlap: copy rows in the order that does not overwrite what is still to be read */
            for (int i = 0; i < h; i++)
            {
                int row = sy < y ? h - 1 - i : i;
                memmove( fb + ((size_t)(y + row) * fbw + x) * 4, fb + ((size_t)(sy + row) * fbw + sx) * 4, (size_t)w * 4 );
            }
            [self addDirtyX:x y:y width:w height:h];
        }
        pthread_mutex_unlock( &fbLock );
        return YES;
    }
    case ENC_CURSOR:
    {
        size_t bytes = (size_t)w * h * 4, mask_row = (w + 7) / 8;
        NSMutableData *data;
        NSBitmapImageRep *rep;
        NSImage *image;
        NSCursor *cursor;
        const uint8_t *src, *mask;
        uint8_t *dst;
        CGFloat scale = 1;

        if (w > 512 || h > 512) return NO;
        data = [NSMutableData dataWithLength:bytes + mask_row * h];
        if (!read_all( s, data.mutableBytes, data.length )) return NO;
        src = data.bytes; mask = src + bytes;
        if (!w || !h) w = h = 1, src = mask = (const uint8_t *)"\0\0\0\0", x = y = 0;  /* no cursor: a clear one */
        rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8
                   samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:w * 4 bitsPerPixel:32];
        dst = rep.bitmapData;
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++)
            {
                const uint8_t *p = src + ((size_t)row * w + col) * 4;
                uint8_t *q = dst + ((size_t)row * w + col) * 4;
                BOOL on = mask[row * mask_row + col / 8] & (0x80 >> (col % 8));
                q[0] = on ? p[2] : 0; q[1] = on ? p[1] : 0; q[2] = on ? p[0] : 0; q[3] = on ? 255 : 0;
            }
        /* Steam's browser asks for a large cursor (48 pixels). Its pixels are kept and shown at half the
         * size in points, which is a Mac cursor's size and sharp on a Retina display; anything still larger
         * than a Mac cursor is brought down to one. The hot spot is in the image's points. */
        if (w > MAX_CURSOR_POINTS || h > MAX_CURSOR_POINTS) scale = 0.5;
        if (MAX( w, h ) * scale > MAX_CURSOR_POINTS) scale = (CGFloat)MAX_CURSOR_POINTS / MAX( w, h );
        image = [[NSImage alloc] initWithSize:NSMakeSize( w * scale, h * scale )];
        [image addRepresentation:rep];
        cursor = [[NSCursor alloc] initWithImage:image
                                         hotSpot:NSMakePoint( MIN( x, w - 1 ) * scale, MIN( y, h - 1 ) * scale )];
        dispatch_async( dispatch_get_main_queue(), ^{ [self.delegate screenCursor:cursor]; } );
        return YES;
    }
    case ENC_EXT_DESKTOP_SIZE:
    {
        uint8_t info[4], screen[16];
        if (!read_all( s, info, 4 )) return NO;
        screenId = 0;  /* not the one from an earlier layout if this one has no screens */
        for (int i = 0; i < info[0]; i++)
        {
            if (!read_all( s, screen, 16 )) return NO;
            if (!i) screenId = be32( screen );
        }
        if (!canResize)
        {
            /* the size asked for on connecting was dropped, as the server had not offered this yet */
            canResize = YES;
            dispatch_async( dispatch_get_main_queue(), ^{ [self.delegate screenCanResize]; } );
        }
        if (x == 1 && y) return YES;  /* our own request, refused: nothing changed */
    }   /* fall through: the rectangle's size is the new screen size */
    case ENC_DESKTOP_SIZE:
    {
        BOOL same;
        if (!w || !h || w > MAX_DIMENSION || h > MAX_DIMENSION) return NO;
        pthread_mutex_lock( &fbLock );
        same = fb && w == fbw && h == fbh;
        pthread_mutex_unlock( &fbLock );
        if (same) return YES;
        [self resizeTo:w height:h first:NO];
        return [self requestUpdate:NO];
    }
    default:
        return NO;
    }
}

- (NSSize)size
{
    NSSize size;
    pthread_mutex_lock( &fbLock );
    size = NSMakeSize( fbw, fbh );
    pthread_mutex_unlock( &fbLock );
    return size;
}

static void free_pixels( void *info, const void *data, size_t size )
{
    (void)info; (void)size;
    free( (void *)data );
}

/* A picture of one rectangle of the screen, cut to what the screen has. Only those rows are copied: with
 * a window for each Steam window the screen is far larger than any one of them. */
- (CGImageRef)copyImageRect:(NSRect)rect
{
    int x = (int)rect.origin.x, y = (int)rect.origin.y, w = (int)rect.size.width, h = (int)rect.size.height;
    uint8_t *copy = NULL;
    CGImageRef image = NULL;

    pthread_mutex_lock( &fbLock );
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    w = MIN( w, fbw - x );
    h = MIN( h, fbh - y );
    if (fb && w > 0 && h > 0 && (copy = malloc( (size_t)w * h * 4 )))
        for (int row = 0; row < h; row++)
            memcpy( copy + (size_t)row * w * 4, fb + ((size_t)(y + row) * fbw + x) * 4, (size_t)w * 4 );
    pthread_mutex_unlock( &fbLock );
    if (copy)
    {
        CGDataProviderRef provider = CGDataProviderCreateWithData( NULL, copy, (size_t)w * h * 4, free_pixels );
        CGColorSpaceRef space = CGColorSpaceCreateWithName( kCGColorSpaceSRGB );
        if (provider)
            image = CGImageCreate( w, h, 8, 32, (size_t)w * 4, space, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                   provider, NULL, false, kCGRenderingIntentDefault );
        else free( copy );
        CGColorSpaceRelease( space );
        CGDataProviderRelease( provider );
    }
    return image;
}

- (CGImageRef)copyImage
{
    return [self copyImageRect:NSMakeRect( 0, 0, MAX_DIMENSION, MAX_DIMENSION )];
}

- (void)pointerX:(int)x y:(int)y buttons:(int)mask
{
    uint8_t msg[6] = { 5, mask };
    NSSize size = self.size;

    put16( msg + 2, MAX( 0, MIN( x, (int)size.width - 1 ) ) );
    put16( msg + 4, MAX( 0, MIN( y, (int)size.height - 1 ) ) );
    [self send:msg length:sizeof(msg)];
}

- (void)key:(uint32_t)keysym down:(BOOL)down
{
    uint8_t msg[8] = { 4, down };
    put32( msg + 4, keysym );
    [self send:msg length:sizeof(msg)];
}

- (void)cutText:(NSString *)text
{
    NSData *latin = [text dataUsingEncoding:NSISOLatin1StringEncoding allowLossyConversion:YES];
    NSMutableData *msg;
    uint8_t head[8] = { 6 };

    if (!latin.length || latin.length > MAX_CUT_TEXT) return;
    put32( head + 4, (uint32_t)latin.length );
    msg = [NSMutableData dataWithBytes:head length:8];
    [msg appendData:latin];
    [self send:msg.bytes length:msg.length];
}

- (void)requestSize:(NSSize)size
{
    uint8_t msg[24] = { 251 };
    int w = MAX( 640, MIN( (int)size.width, MAX_DIMENSION ) ), h = MAX( 400, MIN( (int)size.height, MAX_DIMENSION ) );

    if (!canResize) return;
    put16( msg + 2, w ); put16( msg + 4, h );
    msg[6] = 1;  /* one screen, covering it all */
    put32( msg + 8, screenId );
    put16( msg + 16, w ); put16( msg + 18, h );
    [self send:msg length:sizeof(msg)];
}
@end

/* ---- the view ---- */

/* One view shows one rectangle of the guest's screen, a point for each guest pixel, and sends the mouse
 * and keyboard to it. The whole-screen window uses the same view with 'whole' set: then the picture is
 * the entire screen, scaled to the view. */
@interface GuestView : NSView
- (instancetype)initWithFrame:(NSRect)frame screen:(Screen *)screen popup:(BOOL)popup;
@property long long ident;                   /* the guest window shown, 0 for the whole screen */
@property (nonatomic) NSRect crop;           /* its rectangle on the guest's screen */
@property BOOL whole;
@property (nonatomic) BOOL live;             /* showing the guest and sending input to it (not a status window) */
@property BOOL resizesGuest, sized;          /* the guest's screen follows this view's size, once it has taken the guest's */
@property (nonatomic, strong) NSCursor *cursor;
- (void)refresh;
- (void)setStatus:(NSString *)text;
- (void)sendSize;
- (void)syncModifiers;
- (void)releaseAll;
- (void)forget;
- (void)detach;
@end

@implementation GuestView
{
    Screen *screen;
    CALayer *picture;            /* what the guest drew, never stretched except in the whole-screen view */
    NSSize pictureSize;
    NSTextField *status;
    int buttons, lastX, lastY;
    NSMutableDictionary<NSNumber *, NSNumber *> *held;  /* key code -> the keysym sent for it */
    int mods;                    /* which of mod_syms are down in the guest */
    CGFloat scrollX, scrollY;
    BOOL inside;                 /* the mouse is over this view */
}

- (instancetype)initWithFrame:(NSRect)frame screen:(Screen *)s popup:(BOOL)popup
{
    if ((self = [super initWithFrame:frame]))
    {
        screen = s;
        self.wantsLayer = YES;
        self.layer.backgroundColor = steam_colour().CGColor;
        /* The picture is a layer of its own, placed by hand at the top left at its own size. A view's layer
         * would stretch it (or, with a gravity, scale it by the display's factor) while the window is being
         * resized and the guest has not caught up. */
        picture = [CALayer layer];
        picture.magnificationFilter = kCAFilterNearest;
        picture.contentsGravity = kCAGravityResize;
        picture.zPosition = -1;
        picture.actions = @{ @"contents": NSNull.null, @"bounds": NSNull.null, @"position": NSNull.null };
        [self.layer addSublayer:picture];
        held = [NSMutableDictionary dictionary];
        /* a popup's window is never the key one, and the pointer must still reach the menu it shows */
        [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                  options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingInVisibleRect |
                                          NSTrackingCursorUpdate | (popup ? NSTrackingActiveAlways : NSTrackingActiveInKeyWindow)
                                    owner:self userInfo:nil]];
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

/* -- the picture -- */

- (void)place
{
    NSRect rect = self.whole ? self.bounds : NSMakeRect( 0, 0, pictureSize.width, pictureSize.height );
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    picture.frame = [self convertRectToLayer:rect];  /* whichever way up the layer's own coordinates are */
    [CATransaction commit];
}

- (void)refresh
{
    CGImageRef image;

    if (!self.live) return;
    if (!(image = self.whole ? [screen copyImage] : [screen copyImageRect:self.crop])) return;
    pictureSize = NSMakeSize( CGImageGetWidth( image ), CGImageGetHeight( image ) );
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    picture.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease( image );
    [self place];
}

- (void)setCrop:(NSRect)crop
{
    if (NSEqualRects( crop, _crop )) return;
    _crop = crop;
    [self refresh];
}

- (void)setLive:(BOOL)live
{
    _live = live;
    if (!live)
    {
        picture.contents = nil;
        [self forget];
    }
}

- (void)setStatus:(NSString *)text
{
    if (!status && text)
    {
        status = [NSTextField wrappingLabelWithString:text];
        status.alignment = NSTextAlignmentCenter;
        status.textColor = [NSColor colorWithWhite:0.8 alpha:1];
        status.font = [NSFont systemFontOfSize:15];
        status.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:status];
        [NSLayoutConstraint activateConstraints:@[ [status.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
                                                   [status.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
                                                   [status.widthAnchor constraintLessThanOrEqualToAnchor:self.widthAnchor constant:-40] ]];
    }
    status.hidden = !text;
    if (text)
    {
        status.stringValue = text;
        picture.contents = nil;
    }
}

/* -- the cursor: the guest's, wherever one of its views is under the mouse -- */

- (void)showCursor
{
    if (self.live && _cursor) [_cursor set];
    else [NSCursor.arrowCursor set];
}

- (void)setCursor:(NSCursor *)cursor
{
    _cursor = cursor;
    if (inside) [self showCursor];
}

- (void)cursorUpdate:(NSEvent *)event { (void)event; [self showCursor]; }
- (void)mouseEntered:(NSEvent *)event { (void)event; inside = YES; [self showCursor]; }
- (void)mouseExited:(NSEvent *)event { (void)event; inside = NO; }

/* -- size: only the whole-screen window without an agent asks the guest's screen to follow it -- */

- (void)sendSize
{
    if (self.resizesGuest && self.sized && !self.inLiveResize) [screen requestSize:self.bounds.size];
}

- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    [self sendSize];
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self place];
    [self sendSize];
}

/* -- mouse -- */

- (void)pointer:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];

    if (!self.live) return;
    if (self.whole)
    {
        NSSize guest = screen.size, mine = self.bounds.size;
        if (!guest.width || !mine.width || !mine.height) return;
        lastX = p.x * guest.width / mine.width;
        lastY = p.y * guest.height / mine.height;
    }
    else
    {
        /* a point is a guest pixel, and the view's top left is the window's top left on the guest's screen */
        lastX = (int)floor( _crop.origin.x + p.x );
        lastY = (int)floor( _crop.origin.y + p.y );
    }
    [screen pointerX:lastX y:lastY buttons:buttons];
}

- (void)button:(int)bit down:(BOOL)down event:(NSEvent *)event
{
    if (!self.live) return;
    buttons = down ? buttons | bit : buttons & ~bit;
    [self pointer:event];
}

- (void)mouseMoved:(NSEvent *)event
{
    /* the system puts the arrow back when a window that is not the key one is entered */
    if (self.live && _cursor && NSCursor.currentCursor != _cursor) [_cursor set];
    [self pointer:event];
}
- (void)mouseDragged:(NSEvent *)event { [self pointer:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self pointer:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self pointer:event]; }
- (void)mouseDown:(NSEvent *)event { [self.window makeFirstResponder:self]; [self button:1 down:YES event:event]; }
- (void)mouseUp:(NSEvent *)event { [self button:1 down:NO event:event]; }
- (void)rightMouseDown:(NSEvent *)event { [self button:4 down:YES event:event]; }
- (void)rightMouseUp:(NSEvent *)event { [self button:4 down:NO event:event]; }
- (void)otherMouseDown:(NSEvent *)event { if (event.buttonNumber == 2) [self button:2 down:YES event:event]; }
- (void)otherMouseUp:(NSEvent *)event { if (event.buttonNumber == 2) [self button:2 down:NO event:event]; }

/* the wheel is buttons 4 to 7 on an X server: one press and release for each step */
- (void)scrollWheel:(NSEvent *)event
{
    CGFloat step = event.hasPreciseScrollingDeltas ? 16 : 1;
    int clicks;

    scrollX += event.scrollingDeltaX;
    scrollY += event.scrollingDeltaY;
    if ((clicks = (int)(scrollY / step)))
    {
        int bit = clicks > 0 ? 8 : 16;
        scrollY -= clicks * step;
        for (int i = 0; i < MIN( abs( clicks ), 12 ); i++)
        {
            [self button:bit down:YES event:event];
            [self button:bit down:NO event:event];
        }
    }
    if ((clicks = (int)(scrollX / step)))
    {
        int bit = clicks > 0 ? 32 : 64;
        scrollX -= clicks * step;
        for (int i = 0; i < MIN( abs( clicks ), 12 ); i++)
        {
            [self button:bit down:YES event:event];
            [self button:bit down:NO event:event];
        }
    }
}

/* -- keyboard -- */

static uint32_t special_keysym( unsigned short code )
{
    switch (code)
    {
    case 36: return 0xff0d;  /* Return */
    case 76: return 0xff8d;  /* keypad Enter */
    case 48: return 0xff09;  /* Tab */
    case 51: return 0xff08;  /* Delete -> BackSpace */
    case 117: return 0xffff; /* forward Delete */
    case 53: return 0xff1b;  /* Escape */
    case 123: return 0xff51; case 126: return 0xff52; case 124: return 0xff53; case 125: return 0xff54;  /* arrows */
    case 115: return 0xff50; case 119: return 0xff57; case 116: return 0xff55; case 121: return 0xff56;  /* Home End PgUp PgDn */
    case 122: return 0xffbe; case 120: return 0xffbf; case 99: return 0xffc0; case 118: return 0xffc1;   /* F1-F4 */
    case 96: return 0xffc2; case 97: return 0xffc3; case 98: return 0xffc4; case 100: return 0xffc5;     /* F5-F8 */
    case 101: return 0xffc6; case 109: return 0xffc7; case 103: return 0xffc8; case 111: return 0xffc9;  /* F9-F12 */
    }
    return 0;
}

static uint32_t keysym_for( NSEvent *event )
{
    uint32_t sym = special_keysym( event.keyCode );
    NSString *chars;
    UTF32Char c;

    if (sym) return sym;
    /* the character the key makes with Shift and Caps Lock only: Command and Option are sent as modifiers */
    chars = [event charactersByApplyingModifiers:event.modifierFlags & (NSEventModifierFlagShift | NSEventModifierFlagCapsLock)];
    if (!chars.length) return 0;
    if (![chars getBytes:&c maxLength:4 usedLength:NULL encoding:NSUTF32LittleEndianStringEncoding options:0
                   range:[chars rangeOfComposedCharacterSequenceAtIndex:0] remainingRange:NULL]) return 0;
    if (c < 0x20 || c == 0x7f) return 0;
    return c < 0x100 ? c : 0x01000000 | c;  /* Latin-1 is its own keysym; the rest are Unicode keysyms */
}

- (void)keyDown:(NSEvent *)event
{
    uint32_t sym = self.live ? keysym_for( event ) : 0;
    if (!sym) return;
    held[@(event.keyCode)] = @(sym);
    [screen key:sym down:YES];
}

- (void)keyUp:(NSEvent *)event
{
    NSNumber *sym = held[@(event.keyCode)];
    if (!sym) return;
    [held removeObjectForKey:@(event.keyCode)];
    [screen key:sym.unsignedIntValue down:NO];
}

/* Command is Control in the guest, so the Mac's shortcuts (copy, paste, select all, find) work in Steam.
 * Command-key presses do not arrive as keyDown: the ones the menu does not take are sent from here. The
 * menu is asked here because a window is offered a key equivalent before the menu bar is. */
- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    uint32_t sym;
    if (event.type != NSEventTypeKeyDown || !self.live || self.window.firstResponder != self) return NO;
    if (!(event.modifierFlags & NSEventModifierFlagCommand)) return NO;
    if ([NSApp.mainMenu performKeyEquivalent:event]) return YES;
    if (!(sym = keysym_for( event ))) return NO;
    [screen key:sym down:YES];
    [screen key:sym down:NO];
    return YES;
}

/* Shift_L, Control_L and Alt_L. Command and Control are both Control_L in the guest, so they are followed
 * as one key: it is down while either is, and letting go of one does not release it under the other. */
static const uint32_t mod_syms[] = { 0xffe1, 0xffe3, 0xffe9 };

- (void)setMods:(int)now
{
    for (int i = 0; i < 3; i++)
        if ((now ^ mods) & (1 << i)) [screen key:mod_syms[i] down:(now >> i) & 1];
    mods = now;
}

- (void)modifiers:(NSEventModifierFlags)flags
{
    if (!self.live) return;
    [self setMods:(flags & NSEventModifierFlagShift ? 1 : 0) |
                  (flags & (NSEventModifierFlagControl | NSEventModifierFlagCommand) ? 2 : 0) |
                  (flags & NSEventModifierFlagOption ? 4 : 0)];
}

- (void)flagsChanged:(NSEvent *)event { [self modifiers:event.modifierFlags]; }

/* a window that becomes the key one with Shift already down (a Shift-click on it) has seen no flagsChanged */
- (void)syncModifiers { [self modifiers:NSEvent.modifierFlags]; }

/* nothing stays held in the guest when the window is left */
- (void)releaseAll
{
    for (NSNumber *sym in held.allValues) [screen key:sym.unsignedIntValue down:NO];
    [held removeAllObjects];
    for (int i = 0; i < 3; i++) [screen key:mod_syms[i] down:NO];
    mods = 0;
}

/* the connection went: whatever was held is not held in the screen that comes back */
- (void)forget
{
    [held removeAllObjects];
    buttons = mods = 0;
}

/* The window is going away. A menu closes on the press, so the release never comes to its view: let go
 * of the button in the guest here. */
- (void)detach
{
    if (buttons && self.live)
    {
        buttons = 0;
        [screen pointerX:lastX y:lastY buttons:0];
    }
    _live = NO;
}
@end

/* ---- a guest window and its Mac window ---- */

/* menus, dropdowns and tooltips: they take the mouse and never the keyboard, which stays with the window
 * they belong to */
@interface PopupWindow : NSPanel
@end

@implementation PopupWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface Guest : NSObject
@property long long ident, parent;
@property long long host;                    /* a popup: the window its Mac window is attached to */
@property NSRect rect;                       /* on the guest's screen */
@property BOOL popup, dialog;
@property BOOL hasMin, hasMax;
@property NSSize min, max;
@property (strong) NSWindow *window;
@property (strong) GuestView *view;
@property CFAbsoluteTime created;
@property NSUInteger resizeGen;              /* counts the size changes sent, so a late timer knows it is stale */
@property BOOL resizePending;                /* the user changed the size and the guest has not been told yet */
@property BOOL resizeSent;                   /* it has been told, and has not answered with a different size */
@property NSSize sizeBefore;                 /* what it had when it was told */
@end

@implementation Guest
@end

/* What the agent says is not trusted: a number is a number in range or the message is dropped. */
static BOOL json_int( id value, double low, double high, long long *out )
{
    double d;
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID( (__bridge CFTypeRef)value ) == CFBooleanGetTypeID()) return NO;
    d = [value doubleValue];
    if (!(d >= low && d <= high)) return NO;  /* not a number fails both */
    *out = (long long)d;
    return YES;
}

static BOOL json_size( id value, NSSize *out )
{
    long long w, h;
    NSArray *pair = value;
    if (![value isKindOfClass:NSArray.class] || pair.count != 2) return NO;
    if (!json_int( pair[0], 0, 1e9, &w ) || !json_int( pair[1], 0, 1e9, &h )) return NO;
    *out = NSMakeSize( MAX( 1, MIN( w, MAX_DIMENSION ) ), MAX( 1, MIN( h, MAX_DIMENSION ) ) );
    return YES;
}

/* ---- the application ---- */

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, ScreenDelegate>
@end

@implementation AppDelegate
{
    Screen *screen;
    NSWindow *base;              /* the whole-screen window; with an agent, the small status window */
    GuestView *baseView;
    NSCursor *cursor;
    NSInteger pasteboardCount;
    BOOL connected;              /* to the guest's screen */
    BOOL fallback;               /* the agent was asked for and has not answered: base shows the whole screen */
    NSUInteger graceGen;

    NSTask *agent;
    NSFileHandle *agentIn, *agentOut;
    NSMutableData *agentBuffer;
    BOOL agentSkip;              /* inside a line that was too long */
    BOOL agentRunning, agentConnected;  /* the ssh process lives; it has said hello */
    BOOL agentLive;              /* an agent has said hello at some time: there is a window for each guest window */
    BOOL seenWindow;
    NSUInteger agentGen;         /* which ssh process a queued block belongs to */
    NSMutableDictionary<NSNumber *, Guest *> *guests;
    NSMutableSet<NSNumber *> *stale;    /* windows from before the agent restarted that it has not confirmed */
    long long activeId;          /* the guest's focused window */
    NSPoint cascade;
    BOOL applying;               /* this program is setting a window's size, not the user */

    BOOL commandRunning;         /* metalsharp-steam is at work for the Library menu: one at a time */
    NSPanel *commandNote;        /* what it is doing, while it does */
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    NSMenu *bar = [NSMenu new], *app = [NSMenu new], *win = [NSMenu new];
    NSMenuItem *item;

    [app addItemWithTitle:[@"Hide " stringByAppendingString:opt_title] action:@selector(hide:) keyEquivalent:@"h"];
    item = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    item.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    /* this leaves the Mac program only: Steam keeps running in the virtual machine */
    [app addItemWithTitle:@"Quit Steam Window" action:@selector(terminate:) keyEquivalent:@"q"];
    [win addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [win addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    if (!opt_agent)
        [win addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask =
            NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [[bar addItemWithTitle:@"" action:NULL keyEquivalent:@""] setSubmenu:app];
    if (opt_steam_command)
    {
        /* Steam's own "Add Drive" opens a Linux file chooser that sees the virtual machine and not this
         * Mac: a folder here has to be shared into the machine first, which metalsharp-steam does */
        NSMenu *lib = [[NSMenu alloc] initWithTitle:@"Library"];
        [lib addItemWithTitle:@"Add Library Folder…" action:@selector(addLibraryFolder:) keyEquivalent:@""].target = self;
        [lib addItemWithTitle:@"Show Library Folders" action:@selector(showLibraryFolders:) keyEquivalent:@""].target = self;
        [[bar addItemWithTitle:@"Library" action:NULL keyEquivalent:@""] setSubmenu:lib];
    }
    item = [bar addItemWithTitle:@"Window" action:NULL keyEquivalent:@""];
    win.title = @"Window";
    item.submenu = win;
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = win;

    guests = [NSMutableDictionary dictionary];
    stale = [NSMutableSet set];
    agentBuffer = [NSMutableData data];
    screen = [Screen new];
    screen.delegate = self;

    NSSize size = opt_agent ? NSMakeSize( 420, 140 ) : NSMakeSize( 1280, 800 );
    base = [[NSWindow alloc] initWithContentRect:NSMakeRect( 0, 0, size.width, size.height )
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          (opt_agent ? 0 : NSWindowStyleMaskResizable)
                  backing:NSBackingStoreBuffered defer:NO];
    base.releasedWhenClosed = NO;
    base.title = opt_title;
    base.delegate = self;
    if (!opt_agent)
    {
        base.contentMinSize = NSMakeSize( 800, 500 );
        base.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    }
    base.titlebarAppearsTransparent = YES;
    base.backgroundColor = steam_colour();
    base.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    baseView = [[GuestView alloc] initWithFrame:NSMakeRect( 0, 0, size.width, size.height ) screen:screen popup:NO];
    baseView.whole = YES;
    baseView.live = !opt_agent;
    baseView.resizesGuest = !opt_agent;
    [baseView setStatus:@"Connecting to Steam…"];
    base.contentView = baseView;
    centre_on_primary( base );
    [base makeFirstResponder:baseView];
    [base makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (opt_snapshot)
        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, SNAPSHOT_SECONDS * NSEC_PER_SEC ), dispatch_get_main_queue(), ^{
            fprintf( stderr, "metalsharp-steamview: no picture within %d seconds\n", SNAPSHOT_SECONDS );
            exit( 1 );
        } );
    [screen start];
    if (opt_agent) [self startAgent];
}

/* Steam closes to its tray: with an agent the program stays, and the Dock icon brings Steam's window back */
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return !opt_agent; }

- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible
{
    if (!opt_agent) return YES;
    if (agentLive)
    {
        for (Guest *g in guests.allValues) if (!g.popup) return YES;  /* the system brings one forward */
        if (agentConnected)
        {
            [self sendOp:@{ @"op": @"open" }];
            return NO;
        }
    }
    [base makeKeyAndOrderFront:nil];
    return NO;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    agentGen++;
    if (agent.running) [agent terminate];
}

/* -- the Library menu: metalsharp-steam does the work, this shows what came of it -- */

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(addLibraryFolder:) || item.action == @selector(showLibraryFolders:)) return !commandRunning;
    return YES;
}

- (void)showNote:(NSString *)text
{
    NSProgressIndicator *spin = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect( 20, 32, 16, 16 )];
    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect( 0, 0, 380, 80 ) styleMask:NSWindowStyleMaskTitled
                                                  backing:NSBackingStoreBuffered defer:NO];

    panel.releasedWhenClosed = NO;
    panel.hidesOnDeactivate = NO;        /* it says what is happening for as long as it happens */
    panel.title = opt_title;
    panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    spin.style = NSProgressIndicatorStyleSpinning;
    spin.controlSize = NSControlSizeSmall;
    [spin startAnimation:nil];
    label.frame = NSMakeRect( 48, 16, 312, 48 );
    [panel.contentView addSubview:spin];
    [panel.contentView addSubview:label];
    centre_on_primary( panel );
    [panel orderFront:nil];
    commandNote = panel;
}

/* the end of what the command wrote, which is where it says what went wrong */
static NSString *last_lines( NSData *output )
{
    NSString *text = [[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding] ?:
                     [[NSString alloc] initWithData:output encoding:NSISOLatin1StringEncoding] ?: @"";
    NSArray<NSString *> *lines;

    text = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    lines = [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
    if (lines.count > COMMAND_LINES) lines = [lines subarrayWithRange:NSMakeRange( lines.count - COMMAND_LINES, COMMAND_LINES )];
    return [lines componentsJoinedByString:@"\n"];
}

/* Run metalsharp-steam with these arguments, each one an argument as it is: nothing goes through a shell.
 * The program is not waited for on the main thread; `done` is called there when it has ended, with its
 * status (-1 if it could not be started) and what it wrote. */
- (void)steamCommand:(NSArray<NSString *> *)arguments note:(NSString *)note done:(void (^)( int status, NSString *output ))done
{
    NSTask *task = [NSTask new];
    NSPipe *output = [NSPipe pipe];
    NSMutableData *written = [NSMutableData data];  /* touched on the main thread only */
    NSFileHandle *reading = output.fileHandleForReading;
    NSError *error = nil;
    void (^finish)( int, NSString * ) = ^( int status, NSString *text ) {
        [self->commandNote close];
        self->commandNote = nil;
        self->commandRunning = NO;
        done( status, text );
    };

    if (commandRunning) return;
    commandRunning = YES;
    task.executableURL = [NSURL fileURLWithPath:opt_steam_command];
    task.arguments = arguments;
    task.standardInput = NSFileHandle.fileHandleWithNullDevice;
    task.standardOutput = output;
    task.standardError = output;
    /* Both handlers run off the main thread and only pass what they have to it. The first holds the pipe
     * it reads, on purpose: that keeps the pipe open until its end, after the task itself is forgotten. */
    reading.readabilityHandler = ^( NSFileHandle *handle ) {
        NSData *data = handle.availableData;
        if (!data.length) { output.fileHandleForReading.readabilityHandler = nil; return; }
        dispatch_async( dispatch_get_main_queue(), ^{
            [written appendData:data];
            if (written.length > COMMAND_TAIL)
                [written replaceBytesInRange:NSMakeRange( 0, written.length - COMMAND_TAIL ) withBytes:NULL length:0];
        } );
    };
    task.terminationHandler = ^( NSTask *ended ) {
        int status = ended.terminationReason == NSTaskTerminationReasonExit ? ended.terminationStatus : -1;
        /* The end of the output is not waited for: anything the command leaves running (the virtual
         * machine) may hold the pipe open for as long as it runs. A moment is enough for what the command
         * itself wrote last. The pipe goes on being read to its end, so that nothing that still writes to
         * it finds it closed. */
        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, NSEC_PER_SEC / 4 ), dispatch_get_main_queue(), ^{
            finish( status, last_lines( written ) );
        } );
    };
    if (![task launchAndReturnError:&error])
    {
        reading.readabilityHandler = nil;
        task.terminationHandler = nil;
        finish( -1, error.localizedDescription ?: @"" );
        return;
    }
    if (note) [self showNote:note];
}

- (void)commandAlert:(NSString *)message text:(NSString *)text style:(NSAlertStyle)style
{
    NSAlert *alert = [NSAlert new];
    alert.alertStyle = style;
    alert.messageText = message;
    alert.informativeText = text;
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
}

- (void)addLibraryFolder:(id)sender
{
    (void)sender;
    if (commandRunning) return;
    /* The folder is chosen in Wine's folder browser, which metalsharp-steam opens itself; it then adds the
     * folder and restarts the virtual machine with it shared in, and Steam's windows come back by
     * themselves. The environment is the one this program was given: the command needs it. */
    [self steamCommand:@[ @"library", @"choose" ]
                  note:@"Choose a folder in the Wine window that opens…"
                  done:^( int status, NSString *output ) {
        if (!status) return;
        if (status == 1 && !output.length) return;  /* nothing chosen, nothing to say */
        [self commandAlert:@"The library folder could not be added."
                      text:output.length ? output : [NSString stringWithFormat:@"%@ did not say why.", opt_steam_command.lastPathComponent]
                     style:NSAlertStyleWarning];
    }];
}

- (void)showLibraryFolders:(id)sender
{
    (void)sender;
    [self steamCommand:@[ @"library", @"list" ] note:nil done:^( int status, NSString *output ) {
        if (status)
            [self commandAlert:@"The library folders could not be listed."
                          text:output.length ? output : [NSString stringWithFormat:@"%@ did not say why.", opt_steam_command.lastPathComponent]
                         style:NSAlertStyleWarning];
        else
            [self commandAlert:@"Steam library folders" text:output.length ? output : @"There are none."
                         style:NSAlertStyleInformational];
    }];
}

/* -- what the screen reports -- */

- (void)snapshot
{
    /* wait for a picture that is not all one colour, then write it and leave */
    CGImageRef image = [screen copyImage];
    NSBitmapImageRep *rep;
    NSColor *first;
    BOOL blank = YES, ok;

    if (!image) return;
    rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    CGImageRelease( image );
    first = [rep colorAtX:0 y:0];
    for (NSInteger y = 0; blank && y < rep.pixelsHigh; y += 16)
        for (NSInteger x = 0; blank && x < rep.pixelsWide; x += 16)
            if (![[rep colorAtX:x y:y] isEqual:first]) blank = NO;
    if (blank) return;
    ok = [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:opt_snapshot atomically:YES];
    if (!ok) fprintf( stderr, "metalsharp-steamview: cannot write %s\n", opt_snapshot.fileSystemRepresentation );
    exit( ok ? 0 : 1 );
}

- (void)screenChanged:(NSRect)dirty
{
    if (opt_snapshot) [self snapshot];
    [baseView refresh];
    /* only the windows the change touches get a new picture */
    for (Guest *g in guests.allValues)
        if (NSIntersectsRect( dirty, g.rect )) [g.view refresh];
}

- (void)screenResized:(NSSize)size first:(BOOL)first
{
    /* without an agent the window takes the guest's size once; after that the guest follows the window */
    if (opt_agent || !first || baseView.sized) return;
    baseView.sized = YES;
    [base setContentSize:size];
    centre_on_primary( base );
}

- (void)screenCanResize
{
    [baseView sendSize];
}

- (void)screenCursor:(NSCursor *)c
{
    cursor = c;
    baseView.cursor = c;
    for (Guest *g in guests.allValues) g.view.cursor = c;
}

- (void)screenText:(NSString *)text
{
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:text forType:NSPasteboardTypeString];
    pasteboardCount = pb.changeCount;
}

/* what the Mac's clipboard holds goes to the guest whenever one of its windows becomes the one in use */
- (void)offerClipboard
{
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    NSString *text;
    if (!connected) return;  /* it would be lost, and counted as offered: it goes when the screen is there */
    if (pb.changeCount == pasteboardCount) return;
    pasteboardCount = pb.changeCount;
    if ((text = [pb stringForType:NSPasteboardTypeString])) [screen cutText:text];
}

- (void)screenStatus:(NSString *)text
{
    connected = !text;
    if (text)
    {
        [baseView forget];
        for (Guest *g in guests.allValues) [g.view forget];
        [self screenCursor:nil];  /* the guest's cursor goes with its picture */
    }
    else if (NSApp.keyWindow) [self offerClipboard];  /* what was copied before the screen was there */
    if (!opt_agent)
    {
        [baseView setStatus:text];
        if (!text) [baseView sendSize];
        return;
    }
    if (text)
    {
        /* neither the screen nor the agent: the virtual machine is gone and so are its windows */
        if (!agentConnected) [self removeAllGuests];
        [baseView setStatus:text];
        if (!base.visible) [base orderFront:nil];
        return;
    }
    if (agentLive) [self updateBase];
    else if (fallback)
    {
        [baseView setStatus:nil];
        [baseView refresh];
    }
    else
    {
        NSUInteger gen = ++graceGen;
        [baseView setStatus:@"Starting Steam…"];
        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, AGENT_GRACE * NSEC_PER_SEC ), dispatch_get_main_queue(), ^{
            if (gen == self->graceGen && self->connected && !self->agentLive && !self->fallback) [self enterFallback];
        } );
    }
}

/* -- the base window with an agent: a small note of what is happening, or the whole screen if the agent
 *    never comes -- */

- (void)updateBase
{
    /* there until Steam's first window; after that only while the screen is lost */
    if (!connected || !seenWindow)
    {
        if (connected) [baseView setStatus:@"Waiting for Steam's window…\nClick the Dock icon if Steam is already running"];
        if (!base.visible) [base orderFront:nil];
    }
    else [base orderOut:nil];
}

- (void)enterFallback
{
    NSSize size = screen.size;
    NSRect visible = primary_visible();
    CGFloat scale;

    if (!size.width || !size.height) return;
    /* the agent's screen is larger than any display: the window is the screen's shape, scaled to fit.
     * The guest's screen is left as it is, for the agent may yet come. */
    scale = MIN( 1, MIN( visible.size.width * 0.9 / size.width, visible.size.height * 0.85 / size.height ) );
    fallback = YES;
    base.styleMask |= NSWindowStyleMaskResizable;
    base.contentAspectRatio = size;
    [base setContentSize:NSMakeSize( floor( size.width * scale ), floor( size.height * scale ) )];
    centre_on_primary( base );
    [base makeFirstResponder:baseView];
    baseView.live = YES;
    baseView.cursor = cursor;
    [baseView setStatus:nil];
    [baseView refresh];
}

- (void)leaveFallback
{
    fallback = NO;
    baseView.live = NO;
    base.styleMask &= ~NSWindowStyleMaskResizable;
    base.contentResizeIncrements = NSMakeSize( 1, 1 );  /* ends the aspect ratio */
    [base setContentSize:NSMakeSize( 420, 140 )];
    centre_on_primary( base );
    [base makeFirstResponder:baseView];
}

/* -- the agent: ssh to the guest, a line of JSON for each thing that happens to a window -- */

- (void)startAgent
{
    NSTask *task = [NSTask new];
    NSPipe *input = [NSPipe pipe], *output = [NSPipe pipe];
    NSUInteger gen = ++agentGen;
    __weak AppDelegate *weak = self;

    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/ssh"];
    task.arguments = @[ @"-F", @"/dev/null", @"-i", opt_ssh_key, @"-p", @(opt_ssh_port).stringValue,
                        @"-o", @"IdentitiesOnly=yes", @"-o", @"BatchMode=yes", @"-o", @"StrictHostKeyChecking=accept-new",
                        @"-o", [@"UserKnownHostsFile=" stringByAppendingString:opt_known_hosts],
                        @"-o", @"ServerAliveInterval=15", @"-o", @"LogLevel=ERROR",
                        @"steam@127.0.0.1", @"/usr/local/bin/mtsp-windows" ];
    task.standardInput = input;
    task.standardOutput = output;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    /* both handlers run off the main thread and only pass what they have to it; neither holds the task */
    output.fileHandleForReading.readabilityHandler = ^( NSFileHandle *handle ) {
        NSData *data = handle.availableData;
        if (!data.length) { handle.readabilityHandler = nil; return; }
        dispatch_async( dispatch_get_main_queue(), ^{ [weak agentData:data gen:gen]; } );
    };
    task.terminationHandler = ^( NSTask *ended ) {
        (void)ended;
        dispatch_async( dispatch_get_main_queue(), ^{ [weak agentExited:gen]; } );
    };
    agentBuffer.length = 0;
    agentSkip = NO;
    if (![task launchAndReturnError:NULL])
    {
        output.fileHandleForReading.readabilityHandler = nil;
        task.terminationHandler = nil;
        [self restartAgentLater:gen];
        return;
    }
    agent = task;
    agentIn = input.fileHandleForWriting;
    agentOut = output.fileHandleForReading;
    agentRunning = YES;
}

- (void)restartAgentLater:(NSUInteger)gen
{
    dispatch_after( dispatch_time( DISPATCH_TIME_NOW, RETRY_SECONDS * NSEC_PER_SEC ), dispatch_get_main_queue(), ^{
        if (gen == self->agentGen) [self startAgent];
    } );
}

- (void)agentExited:(NSUInteger)gen
{
    if (gen != agentGen) return;
    agentOut.readabilityHandler = nil;
    agent = nil;
    agentIn = agentOut = nil;
    agentRunning = agentConnected = NO;
    /* The windows stay where the user put them: an agent that comes back reports them again. Without the
     * screen as well there is nothing behind them. */
    if (!connected) [self removeAllGuests];
    [self restartAgentLater:gen];
}

- (void)agentData:(NSData *)data gen:(NSUInteger)gen
{
    const uint8_t *p = data.bytes;
    NSUInteger n = data.length, start = 0;

    if (gen != agentGen || !agentRunning) return;
    for (NSUInteger i = 0; i < n; i++)
    {
        if (p[i] != '\n') continue;
        if (!agentSkip)
        {
            [agentBuffer appendBytes:p + start length:i - start];
            if (agentBuffer.length && agentBuffer.length <= MAX_AGENT_LINE) [self agentLine:[agentBuffer copy]];
        }
        agentBuffer.length = 0;
        agentSkip = NO;
        start = i + 1;
        if (gen != agentGen || !agentRunning) return;
    }
    if (agentSkip) return;
    [agentBuffer appendBytes:p + start length:n - start];
    if (agentBuffer.length > MAX_AGENT_LINE)
    {
        agentBuffer.length = 0;
        agentSkip = YES;
    }
}

- (void)sendOp:(NSDictionary *)op
{
    NSMutableData *line;
    if (!agentConnected) return;
    line = [[NSJSONSerialization dataWithJSONObject:op options:0 error:NULL] mutableCopy];
    [line appendBytes:"\n" length:1];
    [agentIn writeData:line error:NULL];  /* a dead ssh is seen when it exits, not here */
}

/* lines that are not understood are skipped: the two programs need not be the same version */
- (void)agentLine:(NSData *)line
{
    NSDictionary *msg = [NSJSONSerialization JSONObjectWithData:line options:0 error:NULL];
    NSString *ev;
    long long ident;

    if (![msg isKindOfClass:NSDictionary.class]) return;
    ev = msg[@"ev"];
    if (![ev isKindOfClass:NSString.class]) return;
    if ([ev isEqual:@"hello"]) { [self agentHello]; return; }
    if (!agentConnected) return;  /* nothing counts before the hello */
    if ([ev isEqual:@"window"]) [self agentWindow:msg];
    else if ([ev isEqual:@"gone"])
    {
        if (json_int( msg[@"id"], 1, UINT32_MAX, &ident )) [self removeGuest:ident];
    }
    else if ([ev isEqual:@"active"])
    {
        Guest *g;
        if (!json_int( msg[@"id"], 0, UINT32_MAX, &ident )) return;
        activeId = ident;
        /* The guest moving its focus does not move the Mac's: a window the user did not pick must not take
         * the keyboard. One that has just opened in answer to the user is the exception. */
        g = guests[@(ident)];
        if (g && !g.popup && NSApp.active && g.window != NSApp.keyWindow &&
            CFAbsoluteTimeGetCurrent() - g.created < NEW_WINDOW_TIME)
            [g.window makeKeyAndOrderFront:nil];
    }
}

- (void)agentHello
{
    NSUInteger gen = agentGen;

    agentConnected = YES;
    if (!agentLive)
    {
        agentLive = YES;
        if (fallback) [self leaveFallback];
    }
    /* the agent now reports every window it has; the ones it does not name within a moment have closed
     * while it was away */
    [stale removeAllObjects];
    [stale addObjectsFromArray:guests.allKeys];
    if (stale.count)
        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC ), dispatch_get_main_queue(), ^{
            if (gen != self->agentGen || !self->agentConnected) return;
            for (NSNumber *key in self->stale.allObjects) [self removeGuest:key.longLongValue];
            [self->stale removeAllObjects];
        } );
    [self updateBase];
}

- (void)agentWindow:(NSDictionary *)msg
{
    long long ident, x, y, w, h, parent = 0;
    NSString *kind = msg[@"kind"], *title = msg[@"title"];
    NSSize limit = screen.size, min = NSZeroSize, max = NSZeroSize;
    BOOL popup, dialog, hasMin, hasMax, fresh = NO;
    NSRect rect;
    Guest *g;

    if (!json_int( msg[@"id"], 1, UINT32_MAX, &ident )) return;
    if (!json_int( msg[@"x"], -MAX_DIMENSION, MAX_DIMENSION, &x ) || !json_int( msg[@"y"], -MAX_DIMENSION, MAX_DIMENSION, &y ) ||
        !json_int( msg[@"w"], 1, MAX_DIMENSION, &w ) || !json_int( msg[@"h"], 1, MAX_DIMENSION, &h )) return;
    if (![kind isKindOfClass:NSString.class]) return;
    popup = [kind isEqual:@"popup"];
    dialog = [kind isEqual:@"dialog"];
    if (!popup && !dialog && ![kind isEqual:@"normal"]) return;
    json_int( msg[@"parent"], 0, UINT32_MAX, &parent );
    if (![title isKindOfClass:NSString.class]) title = @"";
    if (title.length > MAX_TITLE)
        title = [title substringWithRange:[title rangeOfComposedCharacterSequencesForRange:NSMakeRange( 0, MAX_TITLE )]];
    hasMin = !popup && json_size( msg[@"min"], &min );
    hasMax = !popup && json_size( msg[@"max"], &max );
    if (hasMin && hasMax) max = NSMakeSize( MAX( max.width, min.width ), MAX( max.height, min.height ) );

    /* the rectangle is the part of it that is on the screen */
    if (!limit.width || !limit.height) limit = NSMakeSize( MAX_DIMENSION, MAX_DIMENSION );
    rect = NSIntersectionRect( NSMakeRect( x, y, w, h ), NSMakeRect( 0, 0, limit.width, limit.height ) );
    if (NSIsEmptyRect( rect )) return;

    [stale removeObject:@(ident)];
    g = guests[@(ident)];
    if (g && g.popup != popup)
    {
        [self removeGuest:ident];
        g = nil;
    }
    if (!g)
    {
        if (guests.count >= MAX_WINDOWS) return;
        g = [Guest new];
        g.ident = ident;
        g.popup = popup;
        g.created = CFAbsoluteTimeGetCurrent();
        fresh = YES;
    }
    g.rect = rect;
    g.parent = parent;
    g.dialog = dialog;
    g.hasMin = hasMin; g.min = min;
    g.hasMax = hasMax; g.max = max;

    if (popup)
    {
        if (fresh) [self makePopup:g];
        g.view.crop = rect;
        if (![self placePopup:g])
        {
            /* no window to hang it on */
            if (!fresh) [self removeGuest:ident];
            return;
        }
        if (fresh)
        {
            guests[@(ident)] = g;
            [g.view refresh];
        }
        return;
    }

    if (fresh) [self makeWindow:g];
    g.window.title = title.length ? title : opt_title;
    g.view.crop = rect;
    [self applyLimits:g];
    if (fresh)
    {
        NSWindow *key = NSApp.keyWindow;
        guests[@(ident)] = g;
        [self positionNew:g];
        [g.view refresh];
        /* a new window takes the keyboard only when the guest has given it its focus, or nothing of
         * Steam's has it; otherwise it just comes to the front */
        if (NSApp.active && (activeId == ident || !key || key == base)) [g.window makeKeyAndOrderFront:nil];
        else [g.window orderFront:nil];
        if (!seenWindow)
        {
            seenWindow = YES;
            [self updateBase];
        }
    }
    else
    {
        /* While the user is changing the size the picture follows the guest and the window follows the
         * user. A report that still has the old size after the guest was told a new one is about something
         * else (its title): the answer is yet to come, and the window does not jump back to wait for it. */
        if (g.resizeSent && !NSEqualSizes( rect.size, g.sizeBefore )) g.resizeSent = NO;
        if (!g.window.inLiveResize && !g.resizePending && !g.resizeSent) [self fit:g];
        [self placePopupsOf:g];
    }
}

/* -- Mac windows for guest windows -- */

- (Guest *)guestOfWindow:(NSWindow *)window
{
    GuestView *view = window.contentView;
    Guest *g;
    if (!window || window == base || ![view isKindOfClass:GuestView.class]) return nil;
    g = guests[@(view.ident)];
    return g.window == window ? g : nil;
}

- (GuestView *)makeView:(Guest *)g popup:(BOOL)popup
{
    GuestView *view = [[GuestView alloc] initWithFrame:NSMakeRect( 0, 0, g.rect.size.width, g.rect.size.height )
                                                screen:screen popup:popup];
    view.ident = g.ident;
    view.live = YES;
    view.cursor = cursor;
    return view;
}

- (void)makeWindow:(Guest *)g
{
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect( 0, 0, g.rect.size.width, g.rect.size.height )
                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                      NSWindowStyleMaskResizable
                              backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    window.delegate = self;
    /* the guest window lives in a slot of the guest's screen and cannot be a screen of its own */
    window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
    window.titlebarAppearsTransparent = YES;
    window.backgroundColor = steam_colour();
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    g.view = [self makeView:g popup:NO];
    window.contentView = g.view;
    [window makeFirstResponder:g.view];
    g.window = window;
}

- (void)makePopup:(Guest *)g
{
    PopupWindow *window = [[PopupWindow alloc] initWithContentRect:NSMakeRect( 0, 0, g.rect.size.width, g.rect.size.height )
                                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                    backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    window.hasShadow = YES;
    window.hidesOnDeactivate = NO;       /* it goes when the guest says so */
    window.floatingPanel = NO;
    window.excludedFromWindowsMenu = YES;
    window.acceptsMouseMovedEvents = YES;
    window.backgroundColor = steam_colour();
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    g.view = [self makeView:g popup:YES];
    window.contentView = g.view;
    g.window = window;
}

- (void)applyLimits:(Guest *)g
{
    BOOL fixed = g.hasMin && g.hasMax && NSEqualSizes( g.min, g.max );
    NSWindowStyleMask style = g.window.styleMask;
    NSWindowStyleMask want = fixed ? style & ~NSWindowStyleMaskResizable : style | NSWindowStyleMaskResizable;

    if (want != style)
    {
        g.window.styleMask = want;
        [g.window makeFirstResponder:g.view];
    }
    g.window.contentMinSize = g.hasMin ? g.min : NSMakeSize( 1, 1 );
    g.window.contentMaxSize = g.hasMax ? g.max : NSMakeSize( CGFLOAT_MAX, CGFLOAT_MAX );
}

/* a window that can be seen, for another to be put over */
- (BOOL)onScreen:(Guest *)g
{
    return g && !g.popup && g.window.visible && !g.window.miniaturized && g.window.screen != nil;
}

/* The first window in the middle of the primary display and the next ones stepping down from it, whichever
 * display the keyboard is on. A window that belongs to another goes over that one, and a dialog that names
 * none over the window in use, wherever those are. */
- (void)positionNew:(Guest *)g
{
    Guest *over = g.parent != g.ident ? guests[@(g.parent)] : nil;
    NSRect visible = primary_visible(), frame = g.window.frame;

    if (![self onScreen:over]) over = g.dialog ? [self guestOfWindow:NSApp.keyWindow] : nil;
    if (over != g && [self onScreen:over])
    {
        NSRect under = over.window.frame;
        frame.origin = NSMakePoint( round( NSMidX( under ) - NSWidth( frame ) / 2 ), round( NSMidY( under ) - NSHeight( frame ) / 2 ) );
        [g.window setFrame:[g.window constrainFrameRect:frame toScreen:over.window.screen] display:NO];
        return;
    }
    /* The steps are counted here and not by -cascadeTopLeftFromPoint:, which keeps a window on the display
     * it takes it to be on. `cascade` is the top left of the last window placed, in the primary display's
     * coordinates; one that would leave that display starts from the middle again. */
    if (!NSEqualPoints( cascade, NSZeroPoint ))
    {
        frame.origin = NSMakePoint( cascade.x + CASCADE_STEP, cascade.y - CASCADE_STEP - NSHeight( frame ) );
        if (NSContainsRect( visible, frame ))
        {
            [g.window setFrameOrigin:frame.origin];
            cascade = NSMakePoint( NSMinX( frame ), NSMaxY( frame ) );
            return;
        }
    }
    centre_on_primary( g.window );
    frame = g.window.frame;
    cascade = NSMakePoint( NSMinX( frame ), NSMaxY( frame ) );
}

/* make the Mac window the guest window's size, its top left staying where it is */
- (void)fit:(Guest *)g
{
    NSRect old = g.window.frame;
    NSRect frame = [g.window frameRectForContentRect:NSMakeRect( 0, 0, g.rect.size.width, g.rect.size.height )];

    if (NSEqualSizes( frame.size, old.size )) return;
    frame.origin = NSMakePoint( NSMinX( old ), NSMaxY( old ) - NSHeight( frame ) );
    applying = YES;
    [g.window setFrame:frame display:YES];
    applying = NO;
}

/* A popup sits where it is on the guest's screen relative to its window. Screen coordinates on the Mac
 * grow upwards and the guest's grow downwards, so its top is that far below the top of the window's content. */
- (BOOL)placePopup:(Guest *)g
{
    Guest *host = g.parent != g.ident ? guests[@(g.parent)] : nil;
    NSRect content, frame;

    /* one whose parent is not known stays on the window it is on, or goes on the one in use */
    if (!host || host == g) host = guests[@(g.host)];
    if (!host || host == g) host = [self guestOfWindow:NSApp.keyWindow];
    if (!host || host == g) host = guests[@(activeId)];
    if (!host || host == g || !host.window) return NO;
    content = [host.window contentRectForFrameRect:host.window.frame];
    frame = NSMakeRect( NSMinX( content ) + g.rect.origin.x - host.rect.origin.x,
                        NSMaxY( content ) - (g.rect.origin.y - host.rect.origin.y) - g.rect.size.height,
                        g.rect.size.width, g.rect.size.height );
    [g.window setFrame:frame display:YES];
    if (g.window.parentWindow != host.window)
    {
        [g.window.parentWindow removeChildWindow:g.window];
        [host.window addChildWindow:g.window ordered:NSWindowAbove];
    }
    g.host = host.ident;
    return YES;
}

/* a child window keeps its distance from the bottom of its parent; these are measured from the top */
- (void)placePopupsOf:(Guest *)g
{
    for (Guest *p in guests.allValues)
        if (p.popup && p.host == g.ident) [self placePopup:p];
}

- (void)removeGuest:(long long)ident
{
    Guest *g = guests[@(ident)];

    if (!g) return;
    [guests removeObjectForKey:@(ident)];
    [stale removeObject:@(ident)];
    /* its menus go with it; the list is a copy, and one already taken out is simply not found */
    for (Guest *p in guests.allValues)
        if (p.popup && p.host == ident) [self removeGuest:p.ident];
    [g.view detach];
    [g.window.parentWindow removeChildWindow:g.window];
    g.window.delegate = nil;
    [g.window close];
    for (Guest *other in guests.allValues) if (!other.popup) return;
    cascade = NSZeroPoint;  /* the next first window starts in the middle again */
}

- (void)removeAllGuests
{
    for (NSNumber *key in guests.allKeys) [self removeGuest:key.longLongValue];
}

/* -- the user and the windows -- */

/* Tell the guest the size the user gave the window. The guest answers with the size it took, and the
 * window follows that. A window that refuses the size (Steam Settings has one size and does not always say
 * so in its limits) brings no answer at all, for the agent reports changes only: after a moment the window
 * goes back to the size the guest last reported, which is the one the picture has. */
- (void)sendResize:(Guest *)g
{
    NSSize size = [g.window contentRectForFrameRect:g.window.frame].size;
    NSUInteger gen = ++g.resizeGen;

    g.resizePending = g.resizeSent = NO;
    if (!agentConnected || NSEqualSizes( size, g.rect.size ))
    {
        [self fit:g];
        return;
    }
    g.resizeSent = YES;
    g.sizeBefore = g.rect.size;
    [self sendOp:@{ @"op": @"resize", @"id": @(g.ident), @"w": @((long long)size.width), @"h": @((long long)size.height) }];
    dispatch_after( dispatch_time( DISPATCH_TIME_NOW, (int64_t)(RESIZE_ANSWER * NSEC_PER_SEC) ), dispatch_get_main_queue(), ^{
        /* a later change of size has its own wait; a window that has closed is no longer in the list, and
         * one being resized again is told once more when the user lets go */
        if (g.resizeGen != gen || self->guests[@(g.ident)] != g || g.window.inLiveResize) return;
        g.resizeSent = NO;
        [self fit:g];
        [self placePopupsOf:g];
    } );
}

- (void)windowDidEndLiveResize:(NSNotification *)note
{
    Guest *g = [self guestOfWindow:note.object];
    if (g && !g.popup) [self sendResize:g];
}

- (void)windowDidResize:(NSNotification *)note
{
    Guest *g = [self guestOfWindow:note.object];
    NSUInteger gen;

    if (!g || g.popup) return;
    [self placePopupsOf:g];
    if (applying || g.window.inLiveResize) return;
    /* a zoom: the size changes many times on the way, and the guest is told the last one */
    gen = ++g.resizeGen;
    g.resizePending = YES;
    dispatch_after( dispatch_time( DISPATCH_TIME_NOW, NSEC_PER_SEC / 5 ), dispatch_get_main_queue(), ^{
        if (g.resizeGen == gen && self->guests[@(g.ident)] == g) [self sendResize:g];
    } );
}

/* the guest closes its window, and the Mac window follows when it has */
- (BOOL)windowShouldClose:(NSWindow *)window
{
    Guest *g = [self guestOfWindow:window];
    long long ident = g.ident;

    if (!g) return YES;
    if (agentConnected) [self sendOp:@{ @"op": @"close", @"id": @(ident) }];
    else dispatch_async( dispatch_get_main_queue(), ^{ [self removeGuest:ident]; } );
    return NO;
}

- (void)windowDidBecomeKey:(NSNotification *)note
{
    NSWindow *window = note.object;
    GuestView *view = window.contentView;
    Guest *g = [self guestOfWindow:window];

    [self offerClipboard];
    if ([view isKindOfClass:GuestView.class]) [view syncModifiers];
    /* the keyboard goes to whatever has the guest's focus: bring that along with the Mac's */
    if (g && !g.popup && activeId != g.ident)
    {
        activeId = g.ident;
        [self sendOp:@{ @"op": @"activate", @"id": @(g.ident) }];
    }
}

- (void)windowDidResignKey:(NSNotification *)note
{
    GuestView *view = ((NSWindow *)note.object).contentView;
    if ([view isKindOfClass:GuestView.class]) [view releaseAll];
}
@end

int main( int argc, char **argv )
{
    @autoreleasepool
    {
        AppDelegate *delegate = [AppDelegate new];

        for (int i = 1; i < argc; i++)
        {
            NSString *arg = @(argv[i]), *value = i + 1 < argc ? @(argv[i + 1]) : nil;
            if ([arg isEqual:@"--port"] && value) opt_port = value.intValue, i++;
            else if ([arg isEqual:@"--password-file"] && value) opt_password_file = value.stringByExpandingTildeInPath, i++;
            else if ([arg isEqual:@"--ssh-port"] && value) opt_ssh_port = value.intValue, i++;
            else if ([arg isEqual:@"--ssh-key"] && value) opt_ssh_key = value.stringByExpandingTildeInPath, i++;
            else if ([arg isEqual:@"--known-hosts"] && value) opt_known_hosts = value.stringByExpandingTildeInPath, i++;
            else if ([arg isEqual:@"--title"] && value) opt_title = value, i++;
            else if ([arg isEqual:@"--snapshot"] && value) opt_snapshot = value, i++;
            else if ([arg isEqual:@"--steam-command"] && value) opt_steam_command = value.stringByExpandingTildeInPath, i++;
            else if ([arg hasPrefix:@"-psn"] || [arg hasPrefix:@"-NS"] || [arg hasPrefix:@"-Apple"]) i += [arg hasPrefix:@"-psn"] ? 0 : 1;
            else
            {
                fprintf( stderr, "usage: metalsharp-steamview --port N --password-file FILE\n"
                                 "           [--ssh-port N --ssh-key FILE --known-hosts FILE] [--steam-command PATH]\n"
                                 "           [--title NAME] [--snapshot FILE.png]\n" );
                return 2;
            }
        }
        if (opt_port <= 0 || opt_port > 65535) return 2;
        if (!opt_steam_command.length) opt_steam_command = nil;
        if (opt_ssh_port || opt_ssh_key || opt_known_hosts)
        {
            /* the three go together: a window for each of Steam's needs all of them */
            if (opt_ssh_port <= 0 || opt_ssh_port > 65535 || !opt_ssh_key.length || !opt_known_hosts.length)
            {
                fprintf( stderr, "metalsharp-steamview: --ssh-port, --ssh-key and --known-hosts are given together\n" );
                return 2;
            }
            opt_agent = YES;
        }
        if (!opt_password_file) opt_password_file = [@"~/.metalsharp/vm/vnc-password.txt" stringByExpandingTildeInPath];
        signal( SIGPIPE, SIG_IGN );  /* writing to an ssh that has just died is an error, not the end */
        [NSApplication sharedApplication];
        NSApp.delegate = delegate;
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        [NSApp run];
    }
    return 0;
}
