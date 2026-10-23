/*
 * WFDXCompat launcher runtime loader helpers
 *
 * WineForge-Internal: wfdxcompat-runtime-loader-v1, here backed by the Mullion (DXMT) runtime
 * instead of D3DMetal, which does not exist on arm64 hosts.
 *
 * Copyright (C) 2026 Radim Vesely for WineForge
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "windef.h"
#include "winnt.h"
#include "unix_private.h"

#ifdef __APPLE__

/* The launcher companion is built as ARM64EC: the Rockstar Games Launcher and SocialClubHelper
 * are x86_64 programs, which run as ARM64EC here and search builtins as ARM64. */
static const char wfdx_launcher_companion[] = "aarch64-windows/wfdx-launchers-v1.dll";

static const char *get_wfdxcompat_runtime_dir(void)
{
    return getenv( "WFDXCOMPAT_RUNTIME_DIR" );
}

static char *wfdxcompat_launcher_path(void)
{
    const char *runtime_dir = get_wfdxcompat_runtime_dir();
    char *path;

    if (!runtime_dir || !runtime_dir[0]) return NULL;
    if (asprintf( &path, "%s/%s", runtime_dir, wfdx_launcher_companion ) == -1) return NULL;
    return path;
}

/* The companion and the Mullion runtime it hands D3D11 device creation to. */
BOOL wfdxcompat_launcher_runtime_available(void)
{
    char *companion = wfdxcompat_launcher_path();
    BOOL available = companion && !access( companion, R_OK ) && dxmt_runtime_available();

    free( companion );
    return available;
}

BOOL is_wfdxcompat_launcher_module_name( const char *path )
{
    const char *name = strrchr( path, '/' );

    name = name ? name + 1 : path;
    return !strcasecmp( name, "wfdx-launchers-v1.dll" );
}

/* Only a launcher process the policy selected sees the companion. */
char *resolve_wfdxcompat_launcher_pe_path( USHORT machine )
{
    char *companion;

    if (!wfdxcompat_launcher_runtime_enabled() ||
        (machine != IMAGE_FILE_MACHINE_AMD64 && machine != IMAGE_FILE_MACHINE_ARM64))
        return NULL;
    if (!(companion = wfdxcompat_launcher_path())) return NULL;
    if (!access( companion, R_OK )) return companion;
    free( companion );
    return NULL;
}

#endif
