/*
 * Arm64EC support
 *
 * Copyright 2023 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU Lesser General Public License,
 * the authors give you unlimited permission to link the compiled version
 * of this file with other programs, and to distribute those programs
 * without any restriction coming from the use of this file.  (The GNU
 * Lesser General Public License restrictions do apply in other respects;
 * for example, they cover modification of the file, and distribution when
 * not linked into another program.)
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

#ifdef __arm64ec__

#include "windef.h"
#include "winnt.h"
#include "wine/asm.h"

__ASM_GLOBAL_FUNC( __icall_helper_arm64ec,
                   "stp fp, lr, [sp, #-0x10]!\n\t"
                   ".seh_save_fplr_x 16\n\t"
                   "mov fp, sp\n\t"
                   ".seh_set_fp\n\t"
                   ".seh_endprologue\n\t"
                   "adrp x16, __os_arm64x_dispatch_icall\n\t"
                   "ldr x16, [x16, #:lo12:__os_arm64x_dispatch_icall]\n\t"
                   "blr x16\n\t"
                   ".seh_startepilogue\n\t"
                   "ldp fp, lr, [sp], #0x10\n\t"
                   ".seh_save_fplr_x 0x10\n\t"
                   ".seh_endepilogue\n\t"
		   "br x11" )

/* Exit thunks for the C library calls the compiler makes itself. A struct copy, or memcpy, memmove and memset that
 * the compiler expands as builtins, become calls with no exit thunk in the object, so the linker gives the import's
 * thunk none (RVA 0) unless some object of the module calls the function explicitly. The import thunk is used only
 * when the call goes to x64 code: an x64 ucrtbase.dll an application ships and loaded first (EXOCAD's .NET
 * WinForms: ImageList_Duplicate's memcpy executed comctl32_v6's base, MetalSharp-Wine issue #57). These explicit
 * calls give each one its exit thunk; winebuild's CHPE metadata refers to this function, so every ARM64EC module
 * links it. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
/* not dllimport: a direct call is what gives the named #memcpy$exit_thunk the import's thunk takes */
void *__cdecl memcpy( void *dst, const void *src, size_t size );
void *__cdecl memmove( void *dst, const void *src, size_t size );
void *__cdecl memset( void *dst, int c, size_t size );
#pragma clang diagnostic pop

__attribute__((used, no_builtin("memcpy", "memmove", "memset")))
void __wine_arm64ec_libcall_exit_thunks( void *dst, const void *src, size_t size )
{
    memcpy( dst, src, size );
    memmove( dst, src, size );
    memset( dst, 0, size );
}

#endif /* __arm64ec__ */
