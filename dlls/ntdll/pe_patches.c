/*
 * Verified in-memory compatibility patches for native PE modules.
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

#include <stdarg.h>
#include <string.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/debug.h"
#include "ntdll_misc.h"
#include "pe_patches.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

#if defined(__i386__) || defined(__x86_64__)

struct wineforge_patch_fragment
{
    DWORD rva;
    const BYTE *before;
    const BYTE *after;
    SIZE_T size;
};

struct wineforge_patch_set
{
    const char *identifier;
    const WCHAR *module_name;
    const WCHAR *path_suffix;
    WORD machine;
    SIZE_T image_size;
    DWORD timestamp;
    BOOL builtin;
    BOOL (*should_apply)(void);
    const struct wineforge_patch_fragment *fragments;
    unsigned int fragment_count;
    void (*custom_apply)(void);
    /* WineForge-Internal: ntdll/verified-pe-patch-deferred-module-v1.
     * Retry a custom handler when this dependency loads, but only after the
     * original rule matched. State is process-local under the loader lock. */
    const WCHAR *retry_module_name;
    BOOL matched;
};

#define WF_PATCH_SIZE(before, after) \
    ((sizeof(before) - 1) + 0 * sizeof(char[sizeof(before) == sizeof(after) ? 1 : -1]))

#define WF_PATCH_FRAGMENT(rva, before, after) \
    { (rva), (const BYTE *)(before), (const BYTE *)(after), WF_PATCH_SIZE(before, after) }

static BOOL path_has_suffix( const WCHAR *path, const WCHAR *suffix )
{
    SIZE_T path_len, suffix_len;

    if (!suffix) return TRUE;
    if (!path) return FALSE;
    path_len = wcslen( path );
    suffix_len = wcslen( suffix );
    return path_len >= suffix_len && !wcsicmp( path + path_len - suffix_len, suffix );
}

/* Ubisoft Connect CEF 135 command-line compatibility (i386). */
/* WineForge-Internal: launcher-compat/ubisoft-cef135-command-line-gate-v1. */
#ifdef __i386__
static BOOL process_image_path_has_suffix( const WCHAR *suffix )
{
    const UNICODE_STRING *path = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
    SIZE_T path_len = path->Length / sizeof(WCHAR);
    SIZE_T suffix_len = wcslen( suffix );

    return path->Buffer && path_len >= suffix_len &&
           !wcsnicmp( path->Buffer + path_len - suffix_len, suffix, suffix_len );
}

static BOOL ubisoft_connect_process(void)
{
    return process_image_path_has_suffix(
            L"\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe" );
}

static const struct wineforge_patch_fragment ubisoft_cef_fragments[] =
{
    WF_PATCH_FRAGMENT(
        0x11e92d,
        "\x8b\x47\x34\x89\x46\x34\x8b\x47\x38\x89\x46\x38",
        "\x8b\x47\x34\x89\x46\x34\x31\xc0\x90\x89\x46\x38"
    ),
};
#endif

/* Steam Overlay D3DMetal DXGI thunk compatibility (x86_64). */

static struct wineforge_patch_set patch_sets[] =
{
#ifdef __i386__
    {
        "launcher-compat/ubisoft-cef135-command-line-gate-v1",
        L"libcef.dll",
        NULL,
        IMAGE_FILE_MACHINE_I386,
        0,
        0,
        FALSE,
        ubisoft_connect_process,
        ubisoft_cef_fragments,
        ARRAY_SIZE(ubisoft_cef_fragments),
        NULL,
        NULL,
        FALSE,
    },
#endif
    {0},
};

/* WineForge-Internal: ntdll/verified-pe-patch-manager-v1. */
static void apply_patch_set( struct wineforge_patch_set *set, const WCHAR *module_path,
                             void *module_base, SIZE_T image_size, BOOL is_builtin )
{
    IMAGE_NT_HEADERS *nt = RtlImageNtHeader( module_base );
    BOOL all_before = TRUE, all_after = TRUE;
    SIZE_T first = ~(SIZE_T)0, last = 0, protect_size;
    void *protect_addr;
    ULONG old_prot;
    unsigned int i;

    if (!nt || nt->FileHeader.Machine != set->machine) return;
    if (is_builtin != set->builtin) return;
    if (!path_has_suffix( module_path, set->path_suffix )) return;
    if (set->image_size && image_size != set->image_size) return;
    if (set->timestamp && nt->FileHeader.TimeDateStamp != set->timestamp) return;
    if (set->should_apply && !set->should_apply()) return;
    if (set->custom_apply)
    {
        /* WineForge-Internal: ntdll/verified-pe-patch-deferred-module-v1. */
        set->matched = TRUE;
        TRACE( "matched %s for %s\n", set->identifier, debugstr_w(module_path) );
        set->custom_apply();
        return;
    }

    for (i = 0; i < set->fragment_count; ++i)
    {
        const struct wineforge_patch_fragment *fragment = &set->fragments[i];
        const BYTE *target;

        if (fragment->rva > image_size || fragment->size > image_size - fragment->rva)
        {
            WARN( "%s has an out-of-range fragment at RVA %#lx\n",
                  set->identifier, fragment->rva );
            return;
        }

        target = (const BYTE *)module_base + fragment->rva;
        if (memcmp( target, fragment->before, fragment->size )) all_before = FALSE;
        if (memcmp( target, fragment->after, fragment->size )) all_after = FALSE;
        if (fragment->rva < first) first = fragment->rva;
        if (fragment->rva + fragment->size > last) last = fragment->rva + fragment->size;
    }

    if (all_after) return;
    if (!all_before)
    {
        WARN( "%s original-byte signature does not match\n", set->identifier );
        return;
    }

    protect_addr = (BYTE *)module_base + first;
    protect_size = last - first;
    if (NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                                PAGE_EXECUTE_READWRITE, &old_prot ))
    {
        WARN( "failed to make %s writable\n", set->identifier );
        return;
    }

    for (i = 0; i < set->fragment_count; ++i)
    {
        const struct wineforge_patch_fragment *fragment = &set->fragments[i];
        memcpy( (BYTE *)module_base + fragment->rva, fragment->after, fragment->size );
    }

    NtFlushInstructionCache( NtCurrentProcess(), (BYTE *)module_base + first, last - first );
    NtProtectVirtualMemory( NtCurrentProcess(), &protect_addr, &protect_size,
                            old_prot, &old_prot );
    TRACE( "applied %s\n", set->identifier );
}

void wineforge_apply_pe_patches( const WCHAR *module_name, const WCHAR *module_path,
                                 void *module_base, SIZE_T image_size, BOOL is_builtin )
{
    unsigned int i;

    for (i = 0; patch_sets[i].identifier; ++i)
    {
        struct wineforge_patch_set *set = &patch_sets[i];

        if (!wcsicmp( module_name, set->module_name ))
            apply_patch_set( set, module_path, module_base, image_size, is_builtin );
        /* WineForge-Internal: ntdll/verified-pe-patch-deferred-module-v1.
         * The custom handler still validates the dependency's image and bytes. */
        else if (set->matched && set->custom_apply && set->retry_module_name &&
                 !wcsicmp( module_name, set->retry_module_name ))
        {
            TRACE( "retrying %s after loading %s\n", set->identifier, debugstr_w(module_path) );
            set->custom_apply();
        }
    }
}

#else

/* Native ARM and ARM64 have no verified WineForge PE patches yet.
 * Translated x86 processes use the i386/x86_64 implementation above. */
void wineforge_apply_pe_patches( const WCHAR *module_name, const WCHAR *module_path,
                                 void *module_base, SIZE_T image_size, BOOL is_builtin )
{
    (void)module_name;
    (void)module_path;
    (void)module_base;
    (void)image_size;
    (void)is_builtin;
}

#endif
