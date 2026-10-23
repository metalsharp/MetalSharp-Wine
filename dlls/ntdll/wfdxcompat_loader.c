/*
 * WFDXCompat launcher integration: process-local D3D11 interposition.
 *
 * WineForge-Internal: wfdxcompat/launcher-process-interposition-v1. WineForge ran the companion
 * on D3DMetal; here it wraps the Mullion (DXMT) runtime the Rockstar launcher processes use.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wfdxcompat_loader.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

/* The companion's wfdx_launcher_init flags (wfdxcompat/launchers_abi.h). */
#define WFDX_LAUNCHER_FLAG_INTERPOSE     0x00000001
#define WFDX_LAUNCHER_FLAG_CONTEXT_STATE 0x00000002
#define WFDX_LAUNCHER_FLAG_GDI_SURFACE   0x00000004
#define WFDX_LAUNCHER_FLAG_NATIVE_D3D10  0x00000008

/* Mullion implements D3D10 devices, device context state and GDI-compatible surfaces itself,
 * which WineForge's companion emulated for D3DMetal: by default the companion only interposes
 * device creation and leaves D3D10 to Mullion. WFDXCOMPAT_LAUNCHER_FLAGS (hex) overrides. */
#define WFDX_LAUNCHER_DEFAULT_FLAGS (WFDX_LAUNCHER_FLAG_INTERPOSE | WFDX_LAUNCHER_FLAG_NATIVE_D3D10)

/* State is private to this process; loader callbacks hold the loader lock. */
static HMODULE wfdx_launcher_d3d11;
static HMODULE wfdx_launcher_dxgi;
static HMODULE wfdx_launcher_companion;
static void *wfdx_launcher_create_device;
static void *wfdx_launcher_create_device_and_swapchain;
static void *wfdx_launcher_dxgi_d3d10_create_device;
static BOOL wfdx_launcher_interposition_active;

static BOOL wfdx_launcher_path_has_suffix( const UNICODE_STRING *path, const WCHAR *suffix )
{
    unsigned int path_len = path->Length / sizeof(WCHAR);
    unsigned int suffix_len = wcslen( suffix );
    unsigned int i;

    if (path_len < suffix_len) return FALSE;
    for (i = 0; i < suffix_len; ++i)
    {
        WCHAR a = path->Buffer[path_len - suffix_len + i];
        WCHAR b = suffix[i];

        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (towupper( a ) != towupper( b )) return FALSE;
    }
    return TRUE;
}

/* The unix side resolves the companion only in a process its launcher policy selected; this
 * keeps every other process from trying to load it. */
static BOOL wfdx_launcher_process_enabled(void)
{
    const UNICODE_STRING *path = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;

    return wfdx_launcher_path_has_suffix( path, L"\\Rockstar Games\\Launcher\\Launcher.exe" ) ||
           wfdx_launcher_path_has_suffix( path, L"\\Rockstar Games\\Social Club\\SocialClubHelper.exe" );
}

/* WineForge-Internal: wfdxcompat-runtime-loader-v1. The companion lives outside the prefix, in
 * WFDXCOMPAT_RUNTIME_DIR (by default the bundle's lib/wfdxcompat), so a launcher process opens it
 * from there; no other process sees it. */
NTSTATUS wfdx_launcher_get_alias( const WCHAR *name, UNICODE_STRING *nt_name )
{
    static const WCHAR unix_prefixW[] = L"\\??\\unix";
    static const WCHAR suffixW[] = L"/aarch64-windows/wfdx-launchers-v1.dll";
    UNICODE_STRING env_name, runtime;
    NTSTATUS status;
    SIZE_T length;

    if (wcsicmp( name, L"wfdx-launchers-v1.dll" ) || !wfdx_launcher_process_enabled())
        return STATUS_DLL_NOT_FOUND;

    RtlInitUnicodeString( &env_name, L"WFDXCOMPAT_RUNTIME_DIR" );
    runtime.Length = 0;
    runtime.MaximumLength = 0;
    runtime.Buffer = NULL;
    status = RtlQueryEnvironmentVariable_U( NULL, &env_name, &runtime );
    if (status != STATUS_BUFFER_TOO_SMALL) return STATUS_DLL_NOT_FOUND;
    runtime.MaximumLength = runtime.Length + sizeof(WCHAR);
    if (!(runtime.Buffer = RtlAllocateHeap( GetProcessHeap(), 0, runtime.MaximumLength )))
        return STATUS_NO_MEMORY;
    if (RtlQueryEnvironmentVariable_U( NULL, &env_name, &runtime ) || !runtime.Length)
    {
        RtlFreeHeap( GetProcessHeap(), 0, runtime.Buffer );
        return STATUS_DLL_NOT_FOUND;
    }
    runtime.Buffer[runtime.Length / sizeof(WCHAR)] = 0;

    length = wcslen( unix_prefixW ) + runtime.Length / sizeof(WCHAR) + wcslen( suffixW );
    if (!(nt_name->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR) )))
    {
        RtlFreeHeap( GetProcessHeap(), 0, runtime.Buffer );
        return STATUS_NO_MEMORY;
    }
    wcscpy( nt_name->Buffer, unix_prefixW );
    wcscat( nt_name->Buffer, runtime.Buffer );
    wcscat( nt_name->Buffer, suffixW );
    nt_name->Length = length * sizeof(WCHAR);
    nt_name->MaximumLength = (length + 1) * sizeof(WCHAR);
    RtlFreeHeap( GetProcessHeap(), 0, runtime.Buffer );
    return STATUS_SUCCESS;
}

static UINT wfdx_launcher_flags(void)
{
    WCHAR buffer[16];
    UNICODE_STRING name, value;
    UINT flags = 0;
    WCHAR *p;

    RtlInitUnicodeString( &name, L"WFDXCOMPAT_LAUNCHER_FLAGS" );
    value.Buffer = buffer;
    value.Length = 0;
    value.MaximumLength = sizeof(buffer) - sizeof(WCHAR);
    if (RtlQueryEnvironmentVariable_U( NULL, &name, &value )) return WFDX_LAUNCHER_DEFAULT_FLAGS;
    buffer[value.Length / sizeof(WCHAR)] = 0;
    p = buffer;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    for (; *p; p++)
    {
        if (*p >= '0' && *p <= '9') flags = flags * 16 + (*p - '0');
        else if (*p >= 'a' && *p <= 'f') flags = flags * 16 + (*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F') flags = flags * 16 + (*p - 'A' + 10);
        else return WFDX_LAUNCHER_DEFAULT_FLAGS;
    }
    return flags | WFDX_LAUNCHER_FLAG_INTERPOSE;
}

static BOOL wfdx_launcher_export_name( const ANSI_STRING *name, const char *expected )
{
    unsigned int len = strlen( expected );
    return name && name->Length == len && !memcmp( name->Buffer, expected, len );
}

void wfdx_launcher_activate( HMODULE d3d11 )
{
    struct wfdx_launcher_init
    {
        UINT size;
        UINT abi_version;
        UINT flags;
        UINT reserved;
        void *create_device;
        void *create_device_and_swapchain;
    } init_data;
    static const WCHAR companion_nameW[] = L"wfdx-launchers-v1.dll";
    static const WCHAR dxgi_nameW[] = L"dxgi.dll";
    static const char create_nameA[] = "D3D11CreateDevice";
    static const char create_sc_nameA[] = "D3D11CreateDeviceAndSwapChain";
    static const char init_nameA[] = "wfdx_launcher_init";
    static const char wrapper_nameA[] = "wfdx_launcher_D3D11CreateDevice";
    static const char wrapper_sc_nameA[] = "wfdx_launcher_D3D11CreateDeviceAndSwapChain";
    static const char wrapper_d3d10_nameA[] = "wfdx_launcher_DXGID3D10CreateDevice";
    UNICODE_STRING companion_name;
    UNICODE_STRING dxgi_name;
    ANSI_STRING name;
    HMODULE companion = NULL;
    void *original_create = NULL, *original_create_sc = NULL;
    void *wrapper_create = NULL, *wrapper_create_sc = NULL;
    void *wrapper_d3d10 = NULL, *init_proc = NULL;
    HMODULE dxgi = NULL;
    BOOL (WINAPI *init)(const struct wfdx_launcher_init *);
    NTSTATUS status;

    if (wfdx_launcher_interposition_active || wfdx_launcher_companion ||
        !wfdx_launcher_process_enabled())
        return;

    RtlInitAnsiString( &name, create_nameA );
    if (LdrGetProcedureAddress( d3d11, &name, 0, &original_create ))
        return;
    RtlInitAnsiString( &name, create_sc_nameA );
    if (LdrGetProcedureAddress( d3d11, &name, 0, &original_create_sc ))
        return;

    RtlInitUnicodeString( &companion_name, companion_nameW );
    if ((status = LdrLoadDll( NULL, NULL, &companion_name, &companion )))
    {
        TRACE( "no launcher companion for this process (%#lx)\n", (long)status );
        return;
    }

    RtlInitAnsiString( &name, init_nameA );
    status = LdrGetProcedureAddress( companion, &name, 0, &init_proc );
    if (!status)
    {
        RtlInitAnsiString( &name, wrapper_nameA );
        status = LdrGetProcedureAddress( companion, &name, 0, &wrapper_create );
    }
    if (!status)
    {
        RtlInitAnsiString( &name, wrapper_sc_nameA );
        status = LdrGetProcedureAddress( companion, &name, 0, &wrapper_create_sc );
    }
    if (!status)
    {
        RtlInitAnsiString( &name, wrapper_d3d10_nameA );
        status = LdrGetProcedureAddress( companion, &name, 0, &wrapper_d3d10 );
    }
    if (!status)
    {
        RtlInitUnicodeString( &dxgi_name, dxgi_nameW );
        status = LdrGetDllHandle( NULL, 0, &dxgi_name, &dxgi );
    }
    if (status)
    {
        WARN( "launcher companion %p is missing an entry point (%#lx)\n", companion, (long)status );
        LdrUnloadDll( companion );
        return;
    }

    init = init_proc;
    init_data.size = sizeof(init_data);
    init_data.abi_version = 1;
    init_data.flags = wfdx_launcher_flags();
    init_data.reserved = 0;
    init_data.create_device = original_create;
    init_data.create_device_and_swapchain = original_create_sc;
    if (!init( &init_data ))
    {
        WARN( "launcher companion %p refused flags %#x\n", companion, init_data.flags );
        LdrUnloadDll( companion );
        return;
    }

    wfdx_launcher_d3d11 = d3d11;
    wfdx_launcher_dxgi = dxgi;
    wfdx_launcher_companion = companion;
    wfdx_launcher_create_device = wrapper_create;
    wfdx_launcher_create_device_and_swapchain = wrapper_create_sc;
    wfdx_launcher_dxgi_d3d10_create_device = wrapper_d3d10;
    wfdx_launcher_interposition_active = TRUE;
    TRACE( "activated launcher companion %p (flags %#x) for d3d11 %p\n",
           companion, init_data.flags, d3d11 );
}

/* Imports resolved after activation: dxgi's DXGID3D10CreateDevice, which Mullion's dxgi does
 * not export, comes from the companion. */
void *wfdx_launcher_get_import( HMODULE module, const char *name )
{
    if (wfdx_launcher_interposition_active && module == wfdx_launcher_dxgi &&
        !strcmp( name, "DXGID3D10CreateDevice" ))
        return wfdx_launcher_dxgi_d3d10_create_device;
    return NULL;
}

void *wfdx_launcher_get_proc( HMODULE module, const ANSI_STRING *name )
{
    if (!wfdx_launcher_interposition_active) return NULL;
    if (module == wfdx_launcher_d3d11)
    {
        if (wfdx_launcher_export_name( name, "D3D11CreateDevice" ))
            return wfdx_launcher_create_device;
        if (wfdx_launcher_export_name( name, "D3D11CreateDeviceAndSwapChain" ))
            return wfdx_launcher_create_device_and_swapchain;
    }
    else if (module == wfdx_launcher_dxgi &&
             wfdx_launcher_export_name( name, "DXGID3D10CreateDevice" ))
        return wfdx_launcher_dxgi_d3d10_create_device;
    return NULL;
}
