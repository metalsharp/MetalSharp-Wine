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

#include "launchers.h"

struct launcher_process_policy
{
    const WCHAR *exe_name;
    const WCHAR *path;
    const WCHAR *path_alt;
    const WCHAR *excluded_path;
    const WCHAR *excluded_path_alt;
    const WCHAR *blocked_args[4];
    const WCHAR *append_args;
    const WCHAR *stripped_args[2];
};

static const struct launcher_process_policy launcher_process_policies[] =
{
    /* WineForge-Internal: launcher-compat/steam-cef-swiftshader-v1. */
    {
        L"steamwebhelper.exe",
        L"\\Steam\\bin\\cef\\",
        L"/Steam/bin/cef/",
        NULL,
        NULL,
        {L"--type=crashpad-handler", L"--type=renderer", L"--disable-gpu", NULL},
        L" --no-sandbox"
        L" --in-process-gpu"
        L" --disable-gpu"
        L" --disable-d3d11"
        L" --enable-unsafe-swiftshader"
        L" --use-gl=angle"
        L" --use-angle=swiftshader",
        /* Steam 1794424036 defers GPU init; with the in-process GPU its renderers then never get a
         * channel and Steam kills the main window as unresponsive */
        {L"--disable-gpu-early-init", NULL},
    },
    /* WineForge-Internal: launcher-compat/rockstar-cef-in-process-gpu-v1. */
    {
        L"SocialClubHelper.exe",
        L"\\Rockstar Games\\Social Club\\",
        L"/Rockstar Games/Social Club/",
        NULL,
        NULL,
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
    },
    /* WineForge-Internal: launcher-compat/ea-app-in-process-gpu-v1. */
    {
        L"EADesktop.exe",
        L"\\Program Files\\Electronic Arts\\EA Desktop\\",
        L"/Program Files/Electronic Arts/EA Desktop/",
        L"\\compatibility32\\",
        L"/compatibility32/",
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
    },
    /* WineForge-Internal: launcher-compat/battlenet-in-process-gpu-v1. */
    {
        L"Battle.net.exe",
        L"\\Program Files (x86)\\Battle.net\\",
        L"/Program Files (x86)/Battle.net/",
        NULL,
        NULL,
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
    },
    /* WineForge-Internal: launcher-compat/ubisoft-connect-in-process-gpu-v1. */
    {
        L"upc.exe",
        L"\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\",
        L"/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/",
        NULL,
        NULL,
        {L"--type=", L"--in-process-gpu", NULL},
        L" --in-process-gpu",
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

/* A whole argument: Steam's webhelper passes --disable-gpu-early-init, which must not count as
 * --disable-gpu. An entry ending in '=' (--type=) matches any value. */
static const WCHAR *find_cmd_line_arg( const WCHAR *cmd_line, const WCHAR *arg )
{
    SIZE_T len;
    const WCHAR *p;

    if (!cmd_line || !arg || !(len = lstrlenW( arg ))) return NULL;
    for (p = cmd_line; *p; p++)
    {
        if (p > cmd_line && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '"') continue;
        if (wcsnicmp( p, arg, len )) continue;
        if (arg[len - 1] == '=' || !p[len] || p[len] == ' ' || p[len] == '\t' || p[len] == '"' || p[len] == '=')
            return p;
    }
    return NULL;
}

static BOOL cmd_line_has_arg( const WCHAR *cmd_line, const WCHAR *arg )
{
    return find_cmd_line_arg( cmd_line, arg ) != NULL;
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

    /* MetalSharp: any other Chromium browser process. Its GPU process can't draw into the
     * browser's windows on macOS (they stay blank), so keep the GPU thread in process. */
    if (!cmd_line_has_arg( cmd_line, L"--type=" ) && !cmd_line_has_arg( cmd_line, L"--in-process-gpu" ) &&
        is_chromium_browser( app_name ))
        return L" --in-process-gpu";
    return NULL;
}

/* removes the matching policy's stripped arguments from a command line it was applied to */
void strip_launcher_process_args( const WCHAR *app_name, WCHAR *cmd_line )
{
    unsigned int i, j;

    for (i = 0; i < ARRAY_SIZE(launcher_process_policies); i++)
    {
        const struct launcher_process_policy *policy = &launcher_process_policies[i];

        if (!process_matches( app_name, cmd_line, policy )) continue;
        for (j = 0; j < ARRAY_SIZE(policy->stripped_args) && policy->stripped_args[j]; j++)
        {
            SIZE_T len = lstrlenW( policy->stripped_args[j] );
            WCHAR *p;

            while ((p = (WCHAR *)find_cmd_line_arg( cmd_line, policy->stripped_args[j] )))
                memmove( p, p + len, (lstrlenW( p + len ) + 1) * sizeof(WCHAR) );
        }
        return;
    }
}
