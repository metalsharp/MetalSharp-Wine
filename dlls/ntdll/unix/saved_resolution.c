/*
 * Saved game resolutions after the main display changes
 *
 * Copyright (C) 2026 MetalSharp
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* MetalSharp: a game keeps the resolution it saved, so one first played on the Mac's own screen
 * starts at that screen's size after an external display became the main one. When a game starts,
 * this compares the main display with the one it last started on (<game folder>/.metalsharp/
 * main-display) and, if that changed, sets the game's saved fullscreen and borderless resolution
 * to the main display's before the game reads it. Without a record yet, a saved resolution is
 * replaced if it fits only the built-in screen while a larger external display is the main one.
 * Windowed sizes are left alone. Known settings: Unity (registry), Unreal Engine (GameUserSettings.ini),
 * Remedy's Northlight (Control's renderer.ini), REDengine 3 (The Witcher 3's user.settings) and ELDEN RING
 * (GraphicsConfig.xml). With MetalFX, the main display's size is divided by the upscale factor, and a saved size
 * of the whole display, or one larger than the divided one, is replaced even without a record. */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"

/* MetalSharp: Mullion's MetalFX spatial scaler shows each frame at the upscale factor times the size the game
 * renders at. So that a game renders at the size that upscales to the display, a game with MetalFX on is shown a
 * display of the main display's size divided by the factor (win32u), and its saved resolution is fitted to that.
 * The factor is the one Mullion uses for this program: d3d12.metalSpatialUpscale[Factor], or d3d11's where the
 * d3d12 options are unset, from $MULLION_CONFIG_FILE (its own [program.exe] section included) and $MULLION_CONFIG,
 * or MULLION_METALFX_SPATIAL_SWAPCHAIN=1 with the default factor of 2. WineMetalGL, whatever the backend, upscales
 * OpenGL windows by the same factor (ntdll_get_metalfx_scale). Only games MetalSharp launched (their folder
 * has .metalsharp/launch.env) are shown it; programs on another graphics backend, programs not told about Mullion's
 * configuration, Steam and Wine's own programs see the display's size. */
static double metalfx_scale = 1.0;

/* the scale programs see, 1 without MetalFX */
double ntdll_get_metalfx_scale(void)
{
    return metalfx_scale;
}

static unsigned int gcd( unsigned int a, unsigned int b )
{
    while (b)
    {
        unsigned int r = a % b;
        a = b;
        b = r;
    }
    return a;
}

/* the size a display is shown at: the nearest to its size divided by the factor that keeps its exact aspect ratio
 * (games list only modes of a display's aspect) and that win32u's DPI ratios can take (the display's size over it,
 * times a DPI up to 480, under 65536 once reduced); FALSE without MetalFX */
BOOL ntdll_metalfx_size( unsigned int width, unsigned int height, unsigned int *scaled_width,
                         unsigned int *scaled_height )
{
    unsigned int unit_width, unit_height, base, steps, i, n, w, h;

    if (metalfx_scale <= 1.0 || !width || !height) return FALSE;
    unit_width = width / gcd( width, height );
    unit_height = height / gcd( width, height );
    steps = width / unit_width;
    base = width / metalfx_scale / unit_width + 0.5;
    for (i = 0; i < 2 * 64; i++)
    {
        n = i & 1 ? base + (i + 1) / 2 : base - i / 2;
        if (!n || n >= steps) continue;
        w = n * unit_width;
        h = n * unit_height;
        if (width / gcd( width, w ) * 480 < 65536 && height / gcd( height, h ) * 480 < 65536)
        {
            *scaled_width = w;
            *scaled_height = h;
            return TRUE;
        }
    }
    return FALSE;
}

struct metalfx_options
{
    int enable[2];      /* d3d11, d3d12: -1 unset */
    double factor[2];   /* 0 unset */
};

static void parse_metalfx_line( struct metalfx_options *options, BOOL *active, const char *exe_name,
                                const char *line, const char *end )
{
    static const char *const prefixes[2] = {"d3d11.", "d3d12."};
    char key[64], value[64];
    size_t len = 0, vlen = 0;
    BOOL quoted = FALSE;
    int i;

    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (line < end && *line == '[')
    {
        const char *close = end;
        while (close > line + 1 && close[-1] != ']') close--;
        if (close > line + 1) close--;
        *active = (size_t)(close - line - 1) == strlen( exe_name ) && !memcmp( line + 1, exe_name, close - line - 1 );
        return;
    }
    while (line < end && len < sizeof(key) - 1 && (isalnum( (unsigned char)*line ) || *line == '.' || *line == '_'))
        key[len++] = *line++;
    key[len] = 0;
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (!*active || !len || line >= end || *line++ != '=') return;
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    for (; line < end && vlen < sizeof(value) - 1; line++)
    {
        if (!quoted && (*line == ' ' || *line == '\t' || *line == '\r')) break;
        if (*line == '"') quoted = !quoted;
        else value[vlen++] = *line;
    }
    value[vlen] = 0;

    for (i = 0; i < 2; i++)
    {
        size_t plen = strlen( prefixes[i] );
        if (strncmp( key, prefixes[i], plen )) continue;
        if (!strcmp( key + plen, "metalSpatialUpscale" ))
        {
            if (!strcasecmp( value, "true" )) options->enable[i] = 1;
            else if (!strcasecmp( value, "false" )) options->enable[i] = 0;
        }
        else if (!strcmp( key + plen, "metalSpatialUpscaleFactor" ))
        {
            char *stop;
            double factor = strtod( value, &stop );
            if (stop != value && !*stop && factor > 0) options->factor[i] = factor;
        }
    }
}

static void parse_metalfx_text( struct metalfx_options *options, BOOL *active, const char *exe_name,
                                const char *text, char separator )
{
    const char *end;

    for (; *text; text = *end ? end + 1 : end)
    {
        if (!(end = strchr( text, separator ))) end = text + strlen( text );
        parse_metalfx_line( options, active, exe_name, text, end );
    }
}

static char *read_file( const char *path, size_t *size )
{
    struct stat st;
    char *data = NULL;
    int fd;

    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return NULL;
    if (!fstat( fd, &st ) && S_ISREG( st.st_mode ) && st.st_size > 0 && st.st_size < (1 << 20) &&
        (data = malloc( st.st_size + 1 )))
    {
        if (read( fd, data, st.st_size ) != st.st_size)
        {
            free( data );
            data = NULL;
        }
        else
        {
            data[st.st_size] = 0;
            *size = st.st_size;
        }
    }
    close( fd );
    return data;
}

static BOOL write_file( const char *path, const char *data, size_t size )
{
    char tmp[PATH_MAX];
    int fd;
    BOOL ret;

    if (snprintf( tmp, sizeof(tmp), "%s.metalsharp-tmp", path ) >= sizeof(tmp)) return FALSE;
    if ((fd = open( tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 )) == -1) return FALSE;
    ret = write( fd, data, size ) == size;
    close( fd );
    if (ret) ret = !rename( tmp, path );
    if (!ret) unlink( tmp );
    return ret;
}

/* a game MetalSharp started, whose folder has its launch.env: Steam and the programs it starts inherit
 * MetalSharp's configuration too, and Wine's own programs (explorer, services) the environment of the program
 * that started them */
static BOOL is_launched_game( const char *exe )
{
    const char *exe_name = strrchr( exe, '/' );
    char env_path[PATH_MAX];
    struct stat st;

    if (!exe_name || exe_name - exe + sizeof("/.metalsharp/launch.env") > sizeof(env_path)) return FALSE;
    snprintf( env_path, sizeof(env_path), "%.*s/.metalsharp/launch.env", (int)(exe_name - exe), exe );
    if (stat( env_path, &st ) || !S_ISREG( st.st_mode )) return FALSE;
    return !strcasestr( exe, "/windows/system32/" ) && !strcasestr( exe, "/windows/syswow64/" );
}

/* MetalSharp: the GPU a game is shown in Windows' device list (SetupAPI, WMI, the display adapter's registry
 * keys), from its [program.exe] section of winemac.conf ($WINEMAC_CONFIG_FILE, or winemac.conf next to
 * $MULLION_CONFIG_FILE, else the one MetalSharp Wine comes with, share/metalsharp/assets/wine/etc/winemac.conf):
 *     gpu.vendorId = 10de                          PCI vendor and device, in hex
 *     gpu.deviceId = 2704
 *     gpu.description = "NVIDIA GeForce RTX 4080"
 *     gpu.driverVersion = 617.14                   NVIDIA's numbering, or a Windows driver version (35.0.16.1714)
 * Windows has one device list for every program, so win32u registers the GPU of the game that starts (the Mac's
 * own for a game without a section, or an NVIDIA one for a DLSS game, init_dlss_game) when it differs from the one
 * registered; other programs leave it. */
static int gpu_override = -1;  /* -1 not a game, 0 a game shown the Mac's GPU, 1 a game shown gpu_* */
static unsigned int gpu_vendor, gpu_device;
static char gpu_description[128], gpu_driver_version[32];

static void parse_gpu_line( BOOL *active, const char *exe_name, const char *line, const char *end )
{
    char key[64], value[128];
    size_t len = 0, vlen = 0;
    BOOL quoted = FALSE;

    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (line >= end || *line == '#') return;
    if (*line == '[')
    {
        const char *close = memchr( line, ']', end - line );
        *active = close && (size_t)(close - line - 1) == strlen( exe_name ) &&
                  !strncasecmp( line + 1, exe_name, close - line - 1 );
        return;
    }
    while (line < end && len < sizeof(key) - 1 && (isalnum( (unsigned char)*line ) || *line == '.' || *line == '_'))
        key[len++] = *line++;
    key[len] = 0;
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (!*active || !len || line >= end || *line++ != '=') return;
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    for (; line < end && vlen < sizeof(value) - 1; line++)
    {
        if (!quoted && (*line == ' ' || *line == '\t' || *line == '\r' || *line == '#')) break;
        if (*line == '"') quoted = !quoted;
        else value[vlen++] = *line;
    }
    value[vlen] = 0;

    if (!strcasecmp( key, "gpu.vendorId" )) gpu_vendor = strtoul( value, NULL, 16 );
    else if (!strcasecmp( key, "gpu.deviceId" )) gpu_device = strtoul( value, NULL, 16 );
    else if (!strcasecmp( key, "gpu.description" ))
        snprintf( gpu_description, sizeof(gpu_description), "%s", value );
    else if (!strcasecmp( key, "gpu.driverVersion" ))
    {
        unsigned int major, minor, a, b, c, d;
        /* NVIDIA's 617.14 is Windows' x.x.16.1714: the last five digits, with 10 added before the last four */
        if (sscanf( value, "%u.%u.%u.%u", &a, &b, &c, &d ) == 4)
            snprintf( gpu_driver_version, sizeof(gpu_driver_version), "%u.%u.%u.%u", a, b, c, d );
        else if (sscanf( value, "%u.%u", &major, &minor ) == 2 && major < 1000 && minor < 100)
            snprintf( gpu_driver_version, sizeof(gpu_driver_version), "35.0.%u.%04u",
                      10 + (major * 100 + minor) / 10000, (major * 100 + minor) % 10000 );
    }
}

/* MetalSharp's Direct3D lanes that run DLSS and MetalFX: Mullion's (dxmt), VKD3D-Proton's with DXVK (vkd3d), whose
 * configuration files are vkd3d.conf and dxvk.conf, and DXVK-MacOS's (dxvk), whose file is dxvk.conf */
static BOOL is_mullion_backend( const char *backend )
{
    return backend && (!strcasecmp( backend, "dxmt" ) || !strcasecmp( backend, "mullion" ));
}

/* VKD3D-Proton's lane or DXVK's, both on MoltenVK */
static BOOL is_vulkan_lane_backend( const char *backend )
{
    return backend && (!strcasecmp( backend, "vkd3d" ) || !strcasecmp( backend, "dxvk" ));
}

/* MetalSharp: a game that comes with NVIDIA's DLSS (nvngx_dlss.dll, nvngx_dlssd.dll) or Streamline
 * (sl.interposer.dll) anywhere in its folder (its steamapps/common/<game> folder, the one above an Unreal Engine
 * project's, else its program's) is shown an NVIDIA GPU on the Mullion and VKD3D-Proton backends, so DLSS runs on MetalFX: in Windows'
 * device list here, and by Mullion or DXVK's DXGI through MULLION_CONFIG or DXVK_CONFIG and MULLION_ENABLE_NVEXT
 * (env.c), unless its winemac.conf section names
 * another GPU. A Streamline game is hidden Wine's exports too: The Witcher 3 starts Streamline only without
 * wine_get_version. The search is kept in <program's folder>/.metalsharp/dlss while it is newer than the program. */
enum { DLSS_NGX = 1, DLSS_STREAMLINE = 2 };
static int dlss_game;

static void scan_for_dlss( const char *dir, int depth, int *found, unsigned int *budget )
{
    char path[PATH_MAX];
    struct dirent *entry;
    struct stat st;
    DIR *d;

    if (!(d = opendir( dir ))) return;
    while ((entry = readdir( d )) && *budget && !(*found & DLSS_STREAMLINE))
    {
        --*budget;
        if (entry->d_name[0] == '.') continue;
        if (!strcasecmp( entry->d_name, "nvngx_dlss.dll" ) || !strcasecmp( entry->d_name, "nvngx_dlssd.dll" ))
            *found |= DLSS_NGX;
        else if (!strcasecmp( entry->d_name, "sl.interposer.dll" ))
            *found |= DLSS_NGX | DLSS_STREAMLINE;
        else if (depth && snprintf( path, sizeof(path), "%s/%s", dir, entry->d_name ) < sizeof(path) &&
                 (entry->d_type == DT_DIR || (entry->d_type == DT_UNKNOWN && !stat( path, &st ) && S_ISDIR(st.st_mode))))
            scan_for_dlss( path, depth - 1, found, budget );
    }
    closedir( d );
}

static void init_dlss_game( const char *exe )
{
    const char *exe_name = strrchr( exe, '/' ), *backend = getenv( "GRAPHICS_BACKEND" ), *common, *end;
    char root[PATH_MAX], cache[PATH_MAX], value[4];
    struct stat exe_st, cache_st;
    unsigned int budget = 200000;
    int fd, found = 0, i;
    ssize_t len;

    dlss_game = 0;
    if (!is_launched_game( exe )) return;
    if (!is_mullion_backend( backend ) && !is_vulkan_lane_backend( backend )) return;
    if (snprintf( cache, sizeof(cache), "%.*s/.metalsharp/dlss", (int)(exe_name - exe), exe ) >= sizeof(cache))
        return;
    if (!stat( exe, &exe_st ) && !stat( cache, &cache_st ) && cache_st.st_mtime >= exe_st.st_mtime &&
        (fd = open( cache, O_RDONLY | O_CLOEXEC )) != -1)
    {
        len = read( fd, value, sizeof(value) - 1 );
        close( fd );
        if (len > 0 && value[0] >= '0' && value[0] <= '3')
        {
            dlss_game = value[0] - '0';
            return;
        }
    }

    if ((common = strcasestr( exe, "/steamapps/common/" )) && (end = strchr( common + 18, '/' )) &&
        end - exe < sizeof(root))
    {
        memcpy( root, exe, end - exe );
        root[end - exe] = 0;
    }
    else
    {
        /* Unreal Engine's <game>/<Project>/Binaries/Win64/<program>.exe has DLSS in <Project>/Plugins or Engine/,
         * so from <game>; elsewhere the program's own folder (not its neighbours) */
        snprintf( root, sizeof(root), "%.*s", (int)(exe_name - exe), exe );
        if ((end = strcasestr( root, "/Binaries/Win64" )) && !end[15])
        {
            for (i = 0; i < 3; i++)
            {
                char *slash = strrchr( root, '/' );
                if (slash && slash != root) *slash = 0;
            }
        }
    }
    scan_for_dlss( root, 8, &found, &budget );
    dlss_game = found;
    value[0] = '0' + found;
    value[1] = '\n';
    write_file( cache, value, 2 );
    if (found) MESSAGE( "MetalSharp: %s comes with %s\n", exe_name + 1, found & DLSS_STREAMLINE ? "Streamline" : "DLSS" );
}

/* the DLSS a game MetalSharp started on the Mullion or VKD3D-Proton backend comes with: 1 NGX, 3 Streamline, 0 none */
int metalsharp_dlss_game(void)
{
    return dlss_game;
}

static void init_gpu_override( const char *exe )
{
    const char *exe_name = strrchr( exe, '/' ) ? strrchr( exe, '/' ) + 1 : exe;
    const char *file = getenv( "WINEMAC_CONFIG_FILE" ), *mullion = getenv( "MULLION_CONFIG_FILE" ), *slash;
    char path[PATH_MAX], *text, *line, *end;
    BOOL active = FALSE, section = FALSE;
    size_t size;

    gpu_override = -1;
    if (!is_launched_game( exe )) return;
    gpu_override = 0;
    if (!file && mullion && (slash = strrchr( mullion, '/' )) &&
        snprintf( path, sizeof(path), "%.*s/winemac.conf", (int)(slash - mullion), mullion ) < sizeof(path) &&
        !access( path, R_OK ))
        file = path;
    /* else MetalSharp's own, which comes with MetalSharp Wine (share/metalsharp next to Wine's data) */
    if (!file && data_dir && (slash = strrchr( data_dir, '/' )) &&
        snprintf( path, sizeof(path), "%.*s/metalsharp/assets/wine/etc/winemac.conf", (int)(slash - data_dir),
                  data_dir ) < sizeof(path))
        file = path;
    if (file && (text = read_file( file, &size )))
    {
        for (line = text; *line; line = *end ? end + 1 : end)
        {
            if (!(end = strchr( line, '\n' ))) end = line + strlen( line );
            parse_gpu_line( &active, exe_name, line, end );
            section |= active;
        }
        free( text );
    }
    if ((!gpu_vendor || !gpu_device) && !section && dlss_game)
    {
        gpu_vendor = 0x10de;  /* an RTX 4080, and a driver newer than any DLSS game asks for */
        gpu_device = 0x2704;
        snprintf( gpu_description, sizeof(gpu_description), "NVIDIA GeForce RTX 4080" );
        snprintf( gpu_driver_version, sizeof(gpu_driver_version), "35.0.16.1714" );
    }
    if (!gpu_vendor || !gpu_device) return;
    gpu_override = 1;
    MESSAGE( "MetalSharp: %s is shown the GPU %04x:%04x %s, driver %s\n", exe_name, gpu_vendor, gpu_device,
             gpu_description, gpu_driver_version[0] ? gpu_driver_version : "(Wine's)" );
}

/* -1 for a program that is not a game MetalSharp started, 0 for one shown the Mac's GPU, 1 for one shown the GPU
 * of its winemac.conf section (empty strings for the description and driver version it does not set) */
int ntdll_get_gpu_override( unsigned int *vendor, unsigned int *device, const char **description,
                            const char **driver_version )
{
    if (gpu_override == 1)
    {
        *vendor = gpu_vendor;
        *device = gpu_device;
        *description = gpu_description;
        *driver_version = gpu_driver_version;
    }
    return gpu_override;
}

static void init_metalfx_scale( const char *exe )
{
    struct metalfx_options options = {{-1, -1}, {0, 0}};
    const char *backend = getenv( "GRAPHICS_BACKEND" ), *swapchain = NULL, *files[2] = {NULL}, *lines[2] = {NULL};
    const char *exe_name = strrchr( exe, '/' ) ? strrchr( exe, '/' ) + 1 : exe;
    BOOL active, enable;
    double factor;
    char *text;
    size_t size;
    int i;

    metalfx_scale = 1.0;
    if (!is_launched_game( exe )) return;
    /* VKD3D-Proton's lane: vkd3d.conf for Direct3D 12, dxvk.conf for 11 and earlier; DXVK's: dxvk.conf. MetalSharp
     * gives them the same MetalFX lines as mullion.conf */
    if (is_vulkan_lane_backend( backend ))
    {
        files[0] = getenv( "VKD3D_CONFIG_FILE" );
        files[1] = getenv( "DXVK_CONFIG_FILE" );
        lines[0] = getenv( "DXVK_CONFIG" );
    }
    /* the OpenGL lane (Wine's own Direct3D too) is WineMetalGL's, which upscales by this factor; MetalSharp
     * gives it mullion.conf */
    else if (!backend || !*backend || is_mullion_backend( backend ) || !strcasecmp( backend, "wine" ))
    {
        files[0] = getenv( "MULLION_CONFIG_FILE" );
        lines[0] = getenv( "MULLION_CONFIG" );
        swapchain = getenv( "MULLION_METALFX_SPATIAL_SWAPCHAIN" );
    }
    else return;
    if (!files[0] && !files[1] && !lines[0] && !swapchain) return;
    for (i = 0; i < 2; i++)
    {
        active = TRUE;
        if (files[i] && (text = read_file( files[i], &size )))
        {
            parse_metalfx_text( &options, &active, exe_name, text, '\n' );
            free( text );
        }
    }
    active = TRUE;  /* the environment applies to this program, whichever section the file ended in */
    if (lines[0]) parse_metalfx_text( &options, &active, exe_name, lines[0], ';' );

    enable = (swapchain && !strcmp( swapchain, "1" )) ||
             (options.enable[1] != -1 ? options.enable[1] : options.enable[0] == 1);
    factor = options.factor[1] ? options.factor[1] : options.factor[0] ? options.factor[0] : 2.0;
    if (!enable || factor <= 1.0) return;
    metalfx_scale = factor;
    MESSAGE( "MetalSharp: MetalFX upscales %s by %g; it is shown the main display divided by that\n", exe_name, factor );
}

#ifdef __APPLE__

WINE_DEFAULT_DEBUG_CHANNEL(environ);

struct main_display
{
    unsigned int width, height;      /* current mode, in the points Wine reports; divided by the MetalFX factor */
    unsigned int full_width, full_height;  /* the current mode, when divided by a MetalFX factor */
    char id[64];                     /* vendor-model-serial */
    BOOL builtin;
    unsigned int builtin_width, builtin_height;  /* the built-in screen, when it is online */
};

static BOOL get_main_display( struct main_display *display )
{
    static void *cg;
    static uint32_t (*pCGMainDisplayID)(void);
    static void *(*pCGDisplayCopyDisplayMode)(uint32_t);
    static size_t (*pCGDisplayModeGetWidth)(void *), (*pCGDisplayModeGetHeight)(void *);
    static void (*pCGDisplayModeRelease)(void *);
    static uint32_t (*pCGDisplayVendorNumber)(uint32_t), (*pCGDisplayModelNumber)(uint32_t);
    static uint32_t (*pCGDisplaySerialNumber)(uint32_t), (*pCGDisplayIsBuiltin)(uint32_t);
    static int (*pCGGetOnlineDisplayList)(uint32_t, uint32_t *, uint32_t *);
    uint32_t ids[16], count = 0, main_id, i;
    void *mode;

    if (!cg)
    {
        if (!(cg = dlopen( "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW ))) return FALSE;
#define LOAD(f) if (!(p##f = dlsym( cg, #f ))) return FALSE
        LOAD(CGMainDisplayID); LOAD(CGDisplayCopyDisplayMode); LOAD(CGDisplayModeGetWidth);
        LOAD(CGDisplayModeGetHeight); LOAD(CGDisplayModeRelease); LOAD(CGDisplayVendorNumber);
        LOAD(CGDisplayModelNumber); LOAD(CGDisplaySerialNumber); LOAD(CGDisplayIsBuiltin);
        LOAD(CGGetOnlineDisplayList);
#undef LOAD
    }
    memset( display, 0, sizeof(*display) );
    main_id = pCGMainDisplayID();
    if (!(mode = pCGDisplayCopyDisplayMode( main_id ))) return FALSE;
    display->width = pCGDisplayModeGetWidth( mode );
    display->height = pCGDisplayModeGetHeight( mode );
    pCGDisplayModeRelease( mode );
    snprintf( display->id, sizeof(display->id), "%x-%x-%x", pCGDisplayVendorNumber( main_id ),
              pCGDisplayModelNumber( main_id ), pCGDisplaySerialNumber( main_id ) );
    display->builtin = !!pCGDisplayIsBuiltin( main_id );
    if (!pCGGetOnlineDisplayList( ARRAY_SIZE(ids), ids, &count ))
    {
        for (i = 0; i < count; i++)
        {
            if (!pCGDisplayIsBuiltin( ids[i] ) || !(mode = pCGDisplayCopyDisplayMode( ids[i] ))) continue;
            display->builtin_width = pCGDisplayModeGetWidth( mode );
            display->builtin_height = pCGDisplayModeGetHeight( mode );
            pCGDisplayModeRelease( mode );
            break;
        }
    }
    return display->width && display->height;
}

/* whether a saved resolution is replaced by the main display's */
static BOOL needs_reset( const struct main_display *display, const char *record, unsigned int width, unsigned int height )
{
    char id[64];
    unsigned int old_width, old_height;

    if (!width || !height || (width == display->width && height == display->height)) return FALSE;
    if (record && sscanf( record, "%63s %ux%u", id, &old_width, &old_height ) == 3)
        return strcmp( id, display->id ) || old_width != display->width || old_height != display->height;
    /* no record, with MetalFX: the saved size is the whole display's, the size a game starts at, or larger than
     * the display the game is shown, which lists no such mode */
    if (display->full_width && ((width == display->full_width && height == display->full_height) ||
                                width > display->width || height > display->height))
        return TRUE;
    /* no record: the saved size fits only the built-in screen, and a larger external one is the main display */
    return !display->builtin && display->builtin_width && width <= display->builtin_width &&
           height <= display->builtin_height && (width < display->width || height < display->height) &&
           (width > display->builtin_width / 2 || height > display->builtin_height / 2);
}

/* replaces the number after the first "key" (followed by the end character) */
static char *replace_number( char *text, size_t *size, const char *key, char end, unsigned int value )
{
    char *p = strstr( text, key ), *q, number[16], *ret;
    size_t len, nlen;

    if (!p) return text;
    p += strlen( key );
    for (q = p; *q >= '0' && *q <= '9'; q++);
    if (q == p || (end && *q != end)) return text;
    nlen = snprintf( number, sizeof(number), "%u", value );
    len = *size - (q - p) + nlen;
    if (!(ret = malloc( len + 1 ))) return text;
    memcpy( ret, text, p - text );
    memcpy( ret + (p - text), number, nlen );
    memcpy( ret + (p - text) + nlen, q, *size - (q - text) + 1 );
    free( text );
    *size = len;
    return ret;
}

static BOOL get_number( const char *text, const char *key, unsigned int *value )
{
    const char *p = strstr( text, key );
    return p && sscanf( p + strlen( key ), "%u", value ) == 1;
}

/* ELDEN RING: %APPDATA%\EldenRing\GraphicsConfig.xml, UTF-16 with ASCII content */
static BOOL fit_elden_ring( const char *appdata, const struct main_display *display, const char *record )
{
    static const char *const keys[][2] =
    {
        {"<Resolution-FullScreenWidth>", "<Resolution-FullScreenHeight>"},
        {"<Resolution-BorderlessScreenWidth>", "<Resolution-BorderlessScreenHeight>"},
    };
    char path[PATH_MAX], *wide, *text;
    unsigned int width = 0, height = 0, i;
    size_t size, len;
    BOOL bom, ret = FALSE;

    if (!appdata || snprintf( path, sizeof(path), "%s/EldenRing/GraphicsConfig.xml", appdata ) >= sizeof(path))
        return FALSE;
    if (!(wide = read_file( path, &size ))) return FALSE;
    bom = size >= 2 && (unsigned char)wide[0] == 0xff && (unsigned char)wide[1] == 0xfe;
    if (!(text = malloc( size / 2 + 1 )))
    {
        free( wide );
        return FALSE;
    }
    for (i = bom ? 2 : 0, len = 0; i + 1 < size; i += 2)
    {
        if (wide[i + 1] || !wide[i]) break;  /* not ASCII: leave the file alone */
        text[len++] = wide[i];
    }
    text[len] = 0;
    if (i + 1 < size) goto done;

    if (strstr( text, "<ScreenMode>WINDOW<" )) goto done;
    i = strstr( text, "<ScreenMode>BORDERLESS<" ) ? 1 : 0;
    if (!get_number( text, keys[i][0], &width ) || !get_number( text, keys[i][1], &height ) ||
        !needs_reset( display, record, width, height ))
        goto done;
    for (i = 0; i < ARRAY_SIZE(keys); i++)
    {
        text = replace_number( text, &len, keys[i][0], '<', display->width );
        text = replace_number( text, &len, keys[i][1], '<', display->height );
    }
    free( wide );
    if (!(wide = malloc( len * 2 + 2 ))) goto done;
    size = 0;
    if (bom)
    {
        wide[size++] = 0xff;
        wide[size++] = 0xfe;
    }
    for (i = 0; i < len; i++)
    {
        wide[size++] = text[i];
        wide[size++] = 0;
    }
    if ((ret = write_file( path, wide, size )))
        MESSAGE( "MetalSharp: ELDEN RING's saved resolution %ux%u is now the main display's %ux%u\n",
                 width, height, display->width, display->height );
done:
    free( wide );
    free( text );
    return ret;
}

/* Unreal Engine: %LOCALAPPDATA%\<Project>\Saved\Config\Windows[NoEditor]\GameUserSettings.ini,
 * for <Project>\Binaries\Win64\<Name>.exe */
static BOOL fit_unreal( const char *exe, const char *localappdata, const struct main_display *display,
                        const char *record )
{
    static const char *const subdirs[] = {"Windows", "WindowsNoEditor"};
    char project[256], path[PATH_MAX], *text;
    const char *end, *start;
    unsigned int width, height, mode, i;
    size_t size;
    BOOL ret = FALSE;

    if (!localappdata || !(end = strstr( exe, "/Binaries/Win64/" ))) return FALSE;
    for (start = end; start > exe && start[-1] != '/'; start--);
    if (end - start <= 0 || end - start >= sizeof(project)) return FALSE;
    memcpy( project, start, end - start );
    project[end - start] = 0;

    for (i = 0; i < ARRAY_SIZE(subdirs); i++)
    {
        if (snprintf( path, sizeof(path), "%s/%s/Saved/Config/%s/GameUserSettings.ini", localappdata, project,
                      subdirs[i] ) >= sizeof(path))
            continue;
        if (!(text = read_file( path, &size ))) continue;
        /* FullscreenMode 0 fullscreen, 1 windowed fullscreen, 2 windowed */
        if ((!get_number( text, "\nFullscreenMode=", &mode ) || mode < 2) &&
            get_number( text, "\nResolutionSizeX=", &width ) && get_number( text, "\nResolutionSizeY=", &height ) &&
            needs_reset( display, record, width, height ))
        {
            text = replace_number( text, &size, "\nResolutionSizeX=", 0, display->width );
            text = replace_number( text, &size, "\nResolutionSizeY=", 0, display->height );
            text = replace_number( text, &size, "\nLastUserConfirmedResolutionSizeX=", 0, display->width );
            text = replace_number( text, &size, "\nLastUserConfirmedResolutionSizeY=", 0, display->height );
            if (write_file( path, text, size ))
            {
                MESSAGE( "MetalSharp: %s's saved resolution %ux%u is now the main display's %ux%u\n", project,
                         width, height, display->width, display->height );
                ret = TRUE;
            }
        }
        free( text );
    }
    return ret;
}

static HANDLE open_subkey( HANDLE root, const char *name )
{
    WCHAR nameW[512];
    UNICODE_STRING str;
    OBJECT_ATTRIBUTES attr;
    HANDLE key;
    DWORD len = strlen( name );

    if (len >= ARRAY_SIZE(nameW)) return 0;
    len = ntdll_umbstowcs( name, len + 1, nameW, ARRAY_SIZE(nameW) );
    if (!len) return 0;
    init_unicode_string( &str, nameW );
    InitializeObjectAttributes( &attr, &str, OBJ_CASE_INSENSITIVE, root, NULL );
    return NtOpenKey( &key, KEY_ALL_ACCESS, &attr ) ? 0 : key;
}

/* Unity: HKCU\Software\<company>\<product> from <exe>_Data\app.info (company, then product) */
static BOOL fit_unity( const char *exe, const struct main_display *display, const char *record )
{
    static const WCHAR widthW[] = {'S','c','r','e','e','n','m','a','n','a','g','e','r',' ','R','e','s','o','l','u','t','i','o','n',' ','W','i','d','t','h','_','h'};
    static const WCHAR heightW[] = {'S','c','r','e','e','n','m','a','n','a','g','e','r',' ','R','e','s','o','l','u','t','i','o','n',' ','H','e','i','g','h','t','_','h'};
    static const WCHAR modeW[] = {'S','c','r','e','e','n','m','a','n','a','g','e','r',' ','F','u','l','l','s','c','r','e','e','n',' ','m','o','d','e','_','h'};
    char path[PATH_MAX], *info, *product, *p;
    const char *dot;
    char buffer[sizeof(KEY_VALUE_FULL_INFORMATION) + 1024];
    KEY_VALUE_FULL_INFORMATION *value = (KEY_VALUE_FULL_INFORMATION *)buffer;
    UNICODE_STRING width_name = {0}, height_name = {0};
    WCHAR width_buf[128], height_buf[128];
    DWORD width = 0, height = 0, mode = 1, w, h;
    HANDLE software, company, key;
    ULONG i, len;
    size_t size;
    BOOL ret = FALSE;

    if (!(dot = strrchr( exe, '.' )) || strcasecmp( dot, ".exe" ) ||
        snprintf( path, sizeof(path), "%.*s_Data/app.info", (int)(dot - exe), exe ) >= sizeof(path))
        return FALSE;
    if (!(info = read_file( path, &size ))) return FALSE;
    if (!(product = strchr( info, '\n' )) || product == info) goto done;
    *product++ = 0;
    if ((p = strchr( info, '\r' ))) *p = 0;
    if ((p = strpbrk( product, "\r\n" ))) *p = 0;
    if (!product[0]) goto done;

    if (open_hkcu_key( "Software", &software )) goto done;
    company = open_subkey( software, info );
    NtClose( software );
    if (!company) goto done;
    key = open_subkey( company, product );
    NtClose( company );
    if (!key) goto done;

    for (i = 0; !NtEnumerateValueKey( key, i, KeyValueFullInformation, value, sizeof(buffer), &len ); i++)
    {
        const WCHAR *name = value->Name;
        DWORD data;

        if (value->DataLength != sizeof(DWORD) || value->NameLength >= sizeof(width_buf)) continue;
        memcpy( &data, (char *)value + value->DataOffset, sizeof(data) );
        if (value->NameLength > sizeof(widthW) && !memcmp( name, widthW, sizeof(widthW) ))
        {
            memcpy( width_buf, name, value->NameLength );
            width_name.Buffer = width_buf;
            width_name.Length = width_name.MaximumLength = value->NameLength;
            width = data;
        }
        else if (value->NameLength > sizeof(heightW) && !memcmp( name, heightW, sizeof(heightW) ))
        {
            memcpy( height_buf, name, value->NameLength );
            height_name.Buffer = height_buf;
            height_name.Length = height_name.MaximumLength = value->NameLength;
            height = data;
        }
        else if (value->NameLength > sizeof(modeW) && !memcmp( name, modeW, sizeof(modeW) ))
            mode = data;
    }
    /* Fullscreen mode 0 exclusive, 1 fullscreen window, 2 maximized window, 3 windowed */
    if (width_name.Buffer && height_name.Buffer && mode < 2 && needs_reset( display, record, width, height ))
    {
        w = display->width;
        h = display->height;
        if (!NtSetValueKey( key, &width_name, 0, REG_DWORD, &w, sizeof(w) ) &&
            !NtSetValueKey( key, &height_name, 0, REG_DWORD, &h, sizeof(h) ))
        {
            MESSAGE( "MetalSharp: %s's saved resolution %ux%u is now the main display's %ux%u\n", product,
                     (unsigned int)width, (unsigned int)height, display->width, display->height );
            ret = TRUE;
        }
    }
    NtClose( key );
done:
    free( info );
    return ret;
}

/* CD Projekt RED's REDengine 3 (The Witcher 3): Documents\The Witcher 3\dx12user.settings (user.settings for
 * DirectX 11), [Viewport] Resolution="<width>x<height>", FullScreenMode 0 windowed, 1 borderless, 2 fullscreen */
static BOOL fit_redengine( const char *exe, const char *profile, const struct main_display *display,
                           const char *record )
{
    static const char *const files[] = {"dx12user.settings", "user.settings"};
    const char *name = strrchr( exe, '/' ) ? strrchr( exe, '/' ) + 1 : exe;
    char path[PATH_MAX], number[32], *text, *key, *end, *out;
    unsigned int width, height, mode, i;
    size_t size, nlen;
    BOOL ret = FALSE;

    if (!profile || strcasecmp( name, "witcher3.exe" )) return FALSE;
    for (i = 0; i < ARRAY_SIZE(files); i++)
    {
        if (snprintf( path, sizeof(path), "%s/Documents/The Witcher 3/%s", profile, files[i] ) >= sizeof(path))
            continue;
        if (!(text = read_file( path, &size ))) continue;
        if ((key = strstr( text, "\nResolution=\"" )) && sscanf( key + 13, "%ux%u", &width, &height ) == 2 &&
            (!get_number( text, "\nFullScreenMode=", &mode ) || mode) &&
            needs_reset( display, record, width, height ) && (end = strchr( key + 13, '"' )) &&
            (out = malloc( size + 32 )))
        {
            nlen = snprintf( number, sizeof(number), "%ux%u", display->width, display->height );
            memcpy( out, text, key + 13 - text );
            memcpy( out + (key + 13 - text), number, nlen );
            memcpy( out + (key + 13 - text) + nlen, end, size - (end - text) + 1 );
            if (write_file( path, out, size - (end - (key + 13)) + nlen ))
            {
                MESSAGE( "MetalSharp: the saved resolution %ux%u in %s is now %ux%u\n", width, height, path,
                         display->width, display->height );
                ret = TRUE;
            }
            free( out );
        }
        free( text );
    }
    return ret;
}

/* Remedy's Northlight engine (Control): <game folder>/renderer.ini, a JSON rend::UserRendererSettings. The output
 * resolution is the fullscreen one (windowed sizes are m_iLastWindowSize); the render resolution follows it where
 * it was the output's or would be larger than the new one. */
static BOOL fit_remedy( const char *dir, const struct main_display *display, const char *record )
{
    char path[PATH_MAX], *text;
    unsigned int width, height, render_width = 0, render_height = 0;
    size_t size;
    BOOL ret = FALSE;

    if (snprintf( path, sizeof(path), "%s/renderer.ini", dir ) >= sizeof(path)) return FALSE;
    if (!(text = read_file( path, &size ))) return FALSE;
    if (strstr( text, "\"rend::UserRendererSettings\"" ) &&
        get_number( text, "\"m_iOutputResolutionX\": ", &width ) &&
        get_number( text, "\"m_iOutputResolutionY\": ", &height ) && needs_reset( display, record, width, height ))
    {
        get_number( text, "\"m_iRenderResolutionX\": ", &render_width );
        get_number( text, "\"m_iRenderResolutionY\": ", &render_height );
        text = replace_number( text, &size, "\"m_iOutputResolutionX\": ", ',', display->width );
        text = replace_number( text, &size, "\"m_iOutputResolutionY\": ", ',', display->height );
        if ((render_width == width && render_height == height) || render_width > display->width ||
            render_height > display->height)
        {
            text = replace_number( text, &size, "\"m_iRenderResolutionX\": ", ',', display->width );
            text = replace_number( text, &size, "\"m_iRenderResolutionY\": ", ',', display->height );
        }
        if ((ret = write_file( path, text, size )))
            MESSAGE( "MetalSharp: the saved resolution %ux%u in %s is now %ux%u\n", width, height, path,
                     display->width, display->height );
    }
    free( text );
    return ret;
}

static BOOL is_remedy_folder( const char *dir )
{
    char path[PATH_MAX];
    struct stat st;

    return snprintf( path, sizeof(path), "%s/renderer.ini", dir ) < sizeof(path) && !stat( path, &st );
}

static BOOL is_elden_ring_folder( const char *dir )
{
    char path[PATH_MAX];
    struct stat st;

    return snprintf( path, sizeof(path), "%s/eldenring.exe", dir ) < sizeof(path) && !stat( path, &st );
}

static BOOL is_game_folder( const char *dir, const char *exe )
{
    const char *dot = strrchr( exe, '.' );
    char path[PATH_MAX];
    struct stat st;

    if (strstr( exe, "/Binaries/Win64/" )) return TRUE;
    if (strrchr( exe, '/' ) && !strcasecmp( strrchr( exe, '/' ) + 1, "witcher3.exe" )) return TRUE;
    if (dot && snprintf( path, sizeof(path), "%.*s_Data/app.info", (int)(dot - exe), exe ) < sizeof(path) &&
        !stat( path, &st ))
        return TRUE;
    return is_elden_ring_folder( dir ) || is_remedy_folder( dir );
}

void fit_saved_resolution( const char *exe, const char *appdata, const char *localappdata, const char *profile )
{
    struct main_display display;
    char dir[PATH_MAX], record_path[PATH_MAX], *record, line[128];
    const char *slash = strrchr( exe, '/' );
    size_t size;
    int len;

    if (!slash || slash - exe >= sizeof(dir)) return;
    memcpy( dir, exe, slash - exe );
    dir[slash - exe] = 0;
    init_metalfx_scale( exe );
    init_dlss_game( exe );
    init_gpu_override( exe );
    if (!is_game_folder( dir, exe ) || !get_main_display( &display )) return;
    if (metalfx_scale > 1.0)
    {
        display.full_width = display.width;
        display.full_height = display.height;
        if (!ntdll_metalfx_size( display.full_width, display.full_height, &display.width, &display.height ))
        {
            display.width = display.full_width;
            display.height = display.full_height;
        }
    }
    if (snprintf( record_path, sizeof(record_path), "%s/.metalsharp/main-display", dir ) >= sizeof(record_path))
        return;
    record = read_file( record_path, &size );

    if (!fit_unity( exe, &display, record ) && !fit_unreal( exe, localappdata, &display, record ) &&
        !fit_remedy( dir, &display, record ) && !fit_redengine( exe, profile, &display, record ) &&
        is_elden_ring_folder( dir ))
        fit_elden_ring( appdata, &display, record );

    len = snprintf( line, sizeof(line), "%s %ux%u\n", display.id, display.width, display.height );
    if (!record || strcmp( record, line ))
    {
        char metalsharp[PATH_MAX];

        snprintf( metalsharp, sizeof(metalsharp), "%s/.metalsharp", dir );
        mkdir( metalsharp, 0755 );
        if (!write_file( record_path, line, len )) TRACE( "can't write %s\n", record_path );
    }
    free( record );
}

#else

void fit_saved_resolution( const char *exe, const char *appdata, const char *localappdata, const char *profile )
{
    init_metalfx_scale( exe );
    init_dlss_game( exe );
    init_gpu_override( exe );
}

#endif
