/*
 * WineForge launcher compatibility policy
 *
 * Copyright (C) 2026 Radim Vesely for WineForge
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winreg.h"
#include "wine/debug.h"

#include "launchers.h"

WINE_DEFAULT_DEBUG_CHANNEL(process);

struct launcher_process_policy
{
    const WCHAR *exe_name;
    const WCHAR *path;
    const WCHAR *path_alt;
    const WCHAR *excluded_path;
    const WCHAR *excluded_path_alt;
    const WCHAR *blocked_args[3];
    const WCHAR *append_args;
};

/* MetalSharp: what every Chromium browser (CEF, Chrome, Edge/WebView2) gets: no sandbox, the GPU in process and off,
 * and SwiftShader for WebGL only, so its UI composites in software. With the GPU on, or all of ANGLE on SwiftShader,
 * Chromium draws its UI through ANGLE (Direct3D 11, or SwiftShader's GL) into its windows, and under FEX those windows
 * stop updating: the program looks frozen while its threads sit idle (Steam's web helper, Ubisoft Connect). */
#define CHROMIUM_SOFTWARE_ARGS \
    L" --no-sandbox --in-process-gpu --disable-gpu --disable-d3d11 --enable-unsafe-swiftshader --use-gl=angle" \
    L" --use-angle=swiftshader-webgl"
/* A command line that already has them. */
#define CHROMIUM_SOFTWARE_MARKER L"--use-angle=swiftshader-webgl"

static const struct launcher_process_policy launcher_process_policies[] =
{
    /* WineForge-Internal: launcher-compat/steam-cef-swiftshader-v1. Every web helper process, as MetalSharp's x86_64
     * runtime starts them. */
    {
        L"steamwebhelper.exe",
        L"\\Steam\\bin\\cef\\",
        L"/Steam/bin/cef/",
        NULL,
        NULL,
        {L"--type=crashpad-handler", L"--disable-gpu", NULL},
        CHROMIUM_SOFTWARE_ARGS,
    },
    /* WineForge-Internal: launcher-compat/rockstar-cef-in-process-gpu-v1. */
    {
        L"SocialClubHelper.exe",
        L"\\Rockstar Games\\Social Club\\",
        L"/Rockstar Games/Social Club/",
        NULL,
        NULL,
        {L"--type=", CHROMIUM_SOFTWARE_MARKER, NULL},
        CHROMIUM_SOFTWARE_ARGS,
    },
    /* WineForge-Internal: launcher-compat/ea-app-in-process-gpu-v1. */
    {
        L"EADesktop.exe",
        L"\\Program Files\\Electronic Arts\\EA Desktop\\",
        L"/Program Files/Electronic Arts/EA Desktop/",
        L"\\compatibility32\\",
        L"/compatibility32/",
        {L"--type=", CHROMIUM_SOFTWARE_MARKER, NULL},
        CHROMIUM_SOFTWARE_ARGS,
    },
    /* WineForge-Internal: launcher-compat/battlenet-in-process-gpu-v1. */
    {
        L"Battle.net.exe",
        L"\\Program Files (x86)\\Battle.net\\",
        L"/Program Files (x86)/Battle.net/",
        NULL,
        NULL,
        {L"--type=", CHROMIUM_SOFTWARE_MARKER, NULL},
        CHROMIUM_SOFTWARE_ARGS,
    },
    /* WineForge-Internal: launcher-compat/ubisoft-connect-in-process-gpu-v1. Its browser is upc.exe itself; its
     * renderers (UplayWebCore.exe --type=...) are left as they are. */
    {
        L"upc.exe",
        L"\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\",
        L"/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/",
        NULL,
        NULL,
        {L"--type=", CHROMIUM_SOFTWARE_MARKER, NULL},
        CHROMIUM_SOFTWARE_ARGS,
    },
};

static BOOL string_contains_ci( const WCHAR *string, const WCHAR *needle )
{
    SIZE_T needle_len;

    if (!string || !needle) return FALSE;
    needle_len = lstrlenW( needle );
    if (!needle_len) return TRUE;
    for (; *string; string++)
        if (!wcsnicmp( string, needle, needle_len )) return TRUE;
    return FALSE;
}

/* MetalSharp: whether the command line has the argument `arg`. One ending in '=' is a prefix
 * ("--type=" matches any type); another must be the whole argument, so "--disable-gpu" is not
 * "--disable-gpu-early-init", which Steam's web helper has since November 2026. */
static BOOL cmd_line_has_arg( const WCHAR *cmd_line, const WCHAR *arg )
{
    SIZE_T len = lstrlenW( arg );
    const WCHAR *p;

    if (!cmd_line || !len) return FALSE;
    if (arg[len - 1] == '=') return string_contains_ci( cmd_line, arg );
    for (p = cmd_line; *p; p++)
    {
        if (wcsnicmp( p, arg, len )) continue;
        if (p > cmd_line && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '"') continue;
        if (!p[len] || p[len] == ' ' || p[len] == '\t' || p[len] == '"' || p[len] == '=') return TRUE;
    }
    return FALSE;
}

static BOOL process_matches( const WCHAR *app_name, const WCHAR *cmd_line,
                             const struct launcher_process_policy *policy )
{
    return ((string_contains_ci( app_name, policy->exe_name ) &&
             (string_contains_ci( app_name, policy->path ) ||
              string_contains_ci( app_name, policy->path_alt ))) ||
            (string_contains_ci( cmd_line, policy->exe_name ) &&
             (string_contains_ci( cmd_line, policy->path ) ||
              string_contains_ci( cmd_line, policy->path_alt ))));
}

/* MetalSharp: a Chromium browser (CEF, Chrome, Edge/WebView2) has one of these next to its exe */
static BOOL is_chromium_browser( const WCHAR *app_name )
{
    static const WCHAR *const markers[] = { L"libcef.dll", L"chrome_elf.dll", L"msedge_elf.dll" };
    const WCHAR *name = app_name, *p;
    WCHAR path[MAX_PATH];
    unsigned int i, dir_len;

    if (!app_name) return FALSE;
    for (p = app_name; *p; p++) if (*p == '\\' || *p == '/') name = p + 1;
    dir_len = name - app_name;
    for (i = 0; i < ARRAY_SIZE(markers); i++)
    {
        if (dir_len + lstrlenW( markers[i] ) >= MAX_PATH) continue;
        memcpy( path, app_name, dir_len * sizeof(WCHAR) );
        lstrcpyW( path + dir_len, markers[i] );
        if (GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

const WCHAR *get_launcher_process_args( const WCHAR *app_name, const WCHAR *cmd_line )
{
    unsigned int i, j;

    for (i = 0; i < ARRAY_SIZE(launcher_process_policies); i++)
    {
        const struct launcher_process_policy *policy = &launcher_process_policies[i];

        if (!process_matches( app_name, cmd_line, policy )) continue;
        if (string_contains_ci( app_name, policy->excluded_path ) ||
            string_contains_ci( app_name, policy->excluded_path_alt ) ||
            string_contains_ci( cmd_line, policy->excluded_path ) ||
            string_contains_ci( cmd_line, policy->excluded_path_alt ))
            return NULL;
        for (j = 0; j < ARRAY_SIZE(policy->blocked_args) && policy->blocked_args[j]; j++)
            if (cmd_line_has_arg( cmd_line, policy->blocked_args[j] )) return NULL;
        return policy->append_args;
    }

    /* MetalSharp: any other Chromium browser process (its children take their settings from it). */
    if (!string_contains_ci( cmd_line, L"--type=" ) && !cmd_line_has_arg( cmd_line, CHROMIUM_SOFTWARE_MARKER ) &&
        is_chromium_browser( app_name ))
        return CHROMIUM_SOFTWARE_ARGS;
    return NULL;
}

/* MetalSharp: drawing in software, Edge WebView2 presents through DirectComposition, which Wine does not have
 * (DCompositionCreateDevice is E_NOTIMPL), and fails a CHECK there unless ForceSWDCompWhenDCompFallbackRequired is
 * off. Chromium takes only its last --disable-features, so the feature joins that one's list, or comes in a new one.
 * Returns a new command line (from the process heap), or NULL when it has the feature already. */
WCHAR *chromium_disable_dcomp_fallback( const WCHAR *cmd_line )
{
    static const WCHAR feature[] = L"ForceSWDCompWhenDCompFallbackRequired";
    static const WCHAR option[] = L"--disable-features=";
    SIZE_T option_len = ARRAY_SIZE(option) - 1, feature_len = ARRAY_SIZE(feature) - 1;
    SIZE_T len = lstrlenW( cmd_line ), at, n;
    const WCHAR *p, *last = NULL, *end;
    WCHAR *ret;

    if (string_contains_ci( cmd_line, feature )) return NULL;
    for (p = cmd_line; *p; p++)
        if (!wcsnicmp( p, option, option_len ) && (p == cmd_line || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '"'))
            last = p;
    if (!(ret = RtlAllocateHeap( GetProcessHeap(), 0, (len + 2 + option_len + feature_len + 1) * sizeof(WCHAR) )))
        return NULL;
    if (!last)
    {
        lstrcpyW( ret, cmd_line );
        lstrcatW( ret, L" " );
        lstrcatW( ret, option );
        lstrcatW( ret, feature );
        return ret;
    }
    for (end = last + option_len; *end && *end != ' ' && *end != '\t' && *end != '"'; end++) ;
    at = end - cmd_line;
    memcpy( ret, cmd_line, at * sizeof(WCHAR) );
    n = at;
    if (end > last + option_len) ret[n++] = ',';
    memcpy( ret + n, feature, feature_len * sizeof(WCHAR) );
    lstrcpyW( ret + n + feature_len, end );
    return ret;
}

/* MetalSharp: Edge WebView2's loader needs the WebView2 Runtime, which a Wine prefix doesn't have: a program then
 * fails, NoHesi Launcher (.NET) with "Couldn't find a compatible Webview2 Runtime installation". When a program
 * loads WebView2Loader.dll in a prefix without the runtime, Microsoft's Evergreen bootstrapper, which MetalSharp
 * Wine carries in its data directory (webview2/MicrosoftEdgeWebview2Setup.exe), installs it first: it downloads the
 * runtime from Microsoft, about a minute. A named mutex keeps processes from installing it at once. */
static BOOL webview2_runtime_installed( void )
{
    static const WCHAR key[] = L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    static const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    WCHAR version[64];
    DWORD size, type;
    unsigned int i;
    BOOL ret = FALSE;
    HKEY hkey;

    for (i = 0; i < ARRAY_SIZE(roots) && !ret; i++)
    {
        if (RegOpenKeyExW( roots[i], key, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &hkey )) continue;
        size = sizeof(version) - sizeof(WCHAR);
        memset( version, 0, sizeof(version) );
        if (!RegQueryValueExW( hkey, L"pv", NULL, &type, (BYTE *)version, &size ) && type == REG_SZ &&
            version[0] && wcscmp( version, L"0.0.0.0" ))
            ret = TRUE;
        RegCloseKey( hkey );
    }
    return ret;
}

/* runs the bootstrapper, unless another process is or the runtime is there by now */
static void install_webview2_runtime( const WCHAR *reason )
{
    static const WCHAR setup_name[] = L"\\webview2\\MicrosoftEdgeWebview2Setup.exe";
    WCHAR data[MAX_PATH], setup[MAX_PATH], cmdline[MAX_PATH + 32];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    const WCHAR *dir;
    HANDLE mutex;
    DWORD len;

    len = GetEnvironmentVariableW( L"WINEDATADIR", data, ARRAY_SIZE(data) );
    if (!len || len >= ARRAY_SIZE(data)) return;
    dir = wcsncmp( data, L"\\??\\", 4 ) ? data : data + 4;
    if (lstrlenW( dir ) + ARRAY_SIZE(setup_name) > ARRAY_SIZE(setup)) return;
    lstrcpyW( setup, dir );
    lstrcatW( setup, setup_name );
    if (GetFileAttributesW( setup ) == INVALID_FILE_ATTRIBUTES)
    {
        WARN( "no WebView2 Runtime, and no %s to install it\n", debugstr_w(setup) );
        return;
    }

    if ((mutex = CreateMutexW( NULL, FALSE, L"MetalSharpWebView2RuntimeSetup" )))
        WaitForSingleObject( mutex, 15 * 60 * 1000 );
    if (!webview2_runtime_installed())
    {
        lstrcpyW( cmdline, L"\"" );
        lstrcatW( cmdline, setup );
        lstrcatW( cmdline, L"\" /silent /install" );
        WARN( "%s needs the WebView2 Runtime: installing it\n", debugstr_w(reason) );
        if (CreateProcessW( setup, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
        {
            WaitForSingleObject( pi.hProcess, 10 * 60 * 1000 );
            CloseHandle( pi.hThread );
            CloseHandle( pi.hProcess );
        }
        else WARN( "could not start %s: %lu\n", debugstr_w(setup), GetLastError() );
    }
    if (mutex)
    {
        ReleaseMutex( mutex );
        CloseHandle( mutex );
    }
}

/* a program loading the WebView2 loader, which waits for the runtime to install */
void ensure_webview2_runtime( const WCHAR *name )
{
    static const WCHAR loader[] = L"webview2loader";
    static LONG checked;
    const WCHAR *base = name, *p;

    if (!name) return;
    for (p = name; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    if (wcsnicmp( base, loader, ARRAY_SIZE(loader) - 1 )) return;
    p = base + ARRAY_SIZE(loader) - 1;
    if (*p && wcsicmp( p, L".dll" )) return;
    if (InterlockedCompareExchange( &checked, 1, 0 )) return;
    if (!webview2_runtime_installed()) install_webview2_runtime( name );
}
