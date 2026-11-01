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
 * Windowed sizes are left alone. Known settings: Unity (registry), Unreal Engine
 * (GameUserSettings.ini) and ELDEN RING (GraphicsConfig.xml). */

#if 0
#pragma makedep unix
#endif

#include "config.h"

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

#ifdef __APPLE__

WINE_DEFAULT_DEBUG_CHANNEL(environ);

struct main_display
{
    unsigned int width, height;      /* current mode, in the points Wine reports */
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
    /* no record: the saved size fits only the built-in screen, and a larger external one is the main display */
    return !display->builtin && display->builtin_width && width <= display->builtin_width &&
           height <= display->builtin_height && (width < display->width || height < display->height) &&
           (width > display->builtin_width / 2 || height > display->builtin_height / 2);
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
    if (dot && snprintf( path, sizeof(path), "%.*s_Data/app.info", (int)(dot - exe), exe ) < sizeof(path) &&
        !stat( path, &st ))
        return TRUE;
    return is_elden_ring_folder( dir );
}

void fit_saved_resolution( const char *exe, const char *appdata, const char *localappdata )
{
    struct main_display display;
    char dir[PATH_MAX], record_path[PATH_MAX], *record, line[128];
    const char *slash = strrchr( exe, '/' );
    size_t size;
    int len;

    if (!slash || slash - exe >= sizeof(dir)) return;
    memcpy( dir, exe, slash - exe );
    dir[slash - exe] = 0;
    if (!is_game_folder( dir, exe ) || !get_main_display( &display )) return;
    if (snprintf( record_path, sizeof(record_path), "%s/.metalsharp/main-display", dir ) >= sizeof(record_path))
        return;
    record = read_file( record_path, &size );

    if (!fit_unity( exe, &display, record ) && !fit_unreal( exe, localappdata, &display, record ) &&
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

void fit_saved_resolution( const char *exe, const char *appdata, const char *localappdata )
{
}

#endif
