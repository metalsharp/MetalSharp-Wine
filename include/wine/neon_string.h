/*
 * NEON versions of the C runtime string and memory scans, for ntdll and msvcrt on ARM64 and ARM64EC.
 *
 * Copyright 2026 MetalSharp
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

#ifndef __WINE_WINE_NEON_STRING_H
#define __WINE_WINE_NEON_STRING_H

#if defined(__aarch64__) || defined(__arm64ec__)

#include <stddef.h>
#include <stdint.h>
#include <arm_neon.h>

#define WINE_NEON_STRING 1

/* Scans that stop at a terminator read whole aligned 16-byte blocks, so a load never crosses into the next
 * page; the bytes before the start are masked off. Bounded functions only read inside their bounds. */

/* One nibble per byte of a 16-byte compare result, 0 if no lane is set. */
static inline uint64_t neon_mask8( uint8x16_t cmp )
{
    return vget_lane_u64( vreinterpret_u64_u8( vshrn_n_u16( vreinterpretq_u16_u8( cmp ), 4 )), 0 );
}

/* One byte per 16-bit lane of an 8-lane compare result. */
static inline uint64_t neon_mask16( uint16x8_t cmp )
{
    return vget_lane_u64( vreinterpret_u64_u8( vmovn_u16( cmp )), 0 );
}

static inline size_t neon_strlen( const char *str )
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)str & ~(uintptr_t)15);
    uint64_t mask = neon_mask8( vceqzq_u8( vld1q_u8( p ))) & (~0ull << (((uintptr_t)str & 15) * 4));

    while (!mask)
    {
        p += 16;
        mask = neon_mask8( vceqzq_u8( vld1q_u8( p )));
    }
    return (const char *)p + __builtin_ctzll( mask ) / 4 - str;
}

static inline size_t neon_wcslen( const uint16_t *str )
{
    const uint16_t *s = str, *p;
    uint64_t mask;

    /* 16-byte blocks only line up with characters on an even address. The empty asm keeps the compiler from
     * turning this loop back into a call to wcslen, which is this function in ntdll. */
    if ((uintptr_t)s & 1)
    {
        while (*s)
        {
            s++;
            __asm__( "" : "+r" (s) );
        }
        return s - str;
    }
    p = (const uint16_t *)((uintptr_t)s & ~(uintptr_t)15);
    mask = neon_mask16( vceqzq_u16( vld1q_u16( p ))) & (~0ull << (((uintptr_t)s & 15) * 4));
    while (!mask)
    {
        p += 8;
        mask = neon_mask16( vceqzq_u16( vld1q_u16( p )));
    }
    return p + __builtin_ctzll( mask ) / 8 - str;
}

static inline void *neon_memchr( const void *ptr, int c, size_t n )
{
    const uint8_t *p = ptr, *end = p + n;
    uint8x16_t ch = vdupq_n_u8( c );
    uint64_t mask;

    for (; end - p >= 16; p += 16)
        if ((mask = neon_mask8( vceqq_u8( vld1q_u8( p ), ch ))))
            return (void *)(p + __builtin_ctzll( mask ) / 4);
    for (; p < end; p++) if (*p == (uint8_t)c) return (void *)p;
    return NULL;
}

static inline char *neon_strchr( const char *str, int c )
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)str & ~(uintptr_t)15);
    uint8x16_t ch = vdupq_n_u8( c ), v = vld1q_u8( p );
    uint64_t start = ~0ull << (((uintptr_t)str & 15) * 4);
    uint64_t found = neon_mask8( vceqq_u8( v, ch )) & start, zero = neon_mask8( vceqzq_u8( v )) & start;

    while (!found && !zero)
    {
        p += 16;
        v = vld1q_u8( p );
        found = neon_mask8( vceqq_u8( v, ch ));
        zero = neon_mask8( vceqzq_u8( v ));
    }
    /* A match at or before the terminator counts; strchr(s, 0) finds the terminator. */
    if (found && (!zero || __builtin_ctzll( found ) <= __builtin_ctzll( zero )))
        return (char *)(p + __builtin_ctzll( found ) / 4);
    return NULL;
}

static inline int neon_memcmp( const void *ptr1, const void *ptr2, size_t n )
{
    const uint8_t *p1 = ptr1, *p2 = ptr2;
    uint64_t mask;

    for (; n >= 16; n -= 16, p1 += 16, p2 += 16)
    {
        if ((mask = neon_mask8( vmvnq_u8( vceqq_u8( vld1q_u8( p1 ), vld1q_u8( p2 ))))))
        {
            size_t i = __builtin_ctzll( mask ) / 4;
            return p1[i] < p2[i] ? -1 : 1;
        }
    }
    for (; n; n--, p1++, p2++)
        if (*p1 != *p2) return *p1 < *p2 ? -1 : 1;
    return 0;
}

/* Both strings are read 16 bytes at a time while neither block crosses a page, then a byte at a time. */
static inline int neon_strcmp( const char *str1, const char *str2 )
{
    const uint8_t *p1 = (const uint8_t *)str1, *p2 = (const uint8_t *)str2;

    for (;;)
    {
        if (((uintptr_t)p1 & 4095) <= 4096 - 16 && ((uintptr_t)p2 & 4095) <= 4096 - 16)
        {
            uint8x16_t a = vld1q_u8( p1 ), b = vld1q_u8( p2 );
            uint64_t mask = neon_mask8( vorrq_u8( vmvnq_u8( vceqq_u8( a, b )), vceqzq_u8( a )));
            if (mask)
            {
                size_t i = __builtin_ctzll( mask ) / 4;
                return p1[i] == p2[i] ? 0 : p1[i] < p2[i] ? -1 : 1;
            }
            p1 += 16;
            p2 += 16;
        }
        else
        {
            if (*p1 != *p2) return *p1 < *p2 ? -1 : 1;
            if (!*p1) return 0;
            p1++;
            p2++;
        }
    }
}

/* memmove with unaligned 32-byte blocks. Each step loads before it stores, and the head or tail block is loaded
 * before the loop runs, so overlapping copies in either direction get the original bytes. */
static inline void *neon_memmove( void *dst, const void *src, size_t n )
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (n <= 32)
    {
        if (n >= 16)
        {
            uint8x16_t a = vld1q_u8( s ), b = vld1q_u8( s + n - 16 );
            vst1q_u8( d, a );
            vst1q_u8( d + n - 16, b );
        }
        else if (n >= 8)
        {
            uint8x8_t a = vld1_u8( s ), b = vld1_u8( s + n - 8 );
            vst1_u8( d, a );
            vst1_u8( d + n - 8, b );
        }
        else if (n >= 4)
        {
            uint32_t a = *(const uint32_t *)s, b = *(const uint32_t *)(s + n - 4);
            *(uint32_t *)d = a;
            *(uint32_t *)(d + n - 4) = b;
        }
        else if (n)
        {
            uint8_t a = s[0], b = s[n / 2], c = s[n - 1];
            d[0] = a;
            d[n / 2] = b;
            d[n - 1] = c;
        }
        return dst;
    }

    if ((uintptr_t)d - (uintptr_t)s >= n)
    {
        uint8_t *dend = d + n;
        uint8x16_t t0 = vld1q_u8( s + n - 32 ), t1 = vld1q_u8( s + n - 16 );

        /* Aligned stores for long copies; only when the buffers are far enough apart that the unaligned head
         * store can't overwrite source bytes still to be read. */
        if (n >= 256 && (uintptr_t)s - (uintptr_t)d >= 64)
        {
            size_t skip = 16 - ((uintptr_t)d & 15);
            vst1q_u8( d, vld1q_u8( s ));
            d += skip;
            s += skip;
            n -= skip;
        }
        for (; n > 32; n -= 32, s += 32, d += 32)
        {
            uint8x16_t a = vld1q_u8( s ), b = vld1q_u8( s + 16 );
            vst1q_u8( d, a );
            vst1q_u8( d + 16, b );
        }
        vst1q_u8( dend - 32, t0 );
        vst1q_u8( dend - 16, t1 );
    }
    else
    {
        uint8x16_t h0 = vld1q_u8( s ), h1 = vld1q_u8( s + 16 );

        if (n >= 256 && (uintptr_t)d - (uintptr_t)s >= 64)
        {
            size_t skip = ((uintptr_t)(d + n) & 15);
            vst1q_u8( d + n - 16, vld1q_u8( s + n - 16 ));
            n -= skip;
        }
        for (; n > 32; n -= 32)
        {
            uint8x16_t a = vld1q_u8( s + n - 32 ), b = vld1q_u8( s + n - 16 );
            vst1q_u8( d + n - 32, a );
            vst1q_u8( d + n - 16, b );
        }
        vst1q_u8( d, h0 );
        vst1q_u8( d + 16, h1 );
    }
    return dst;
}

#endif /* __aarch64__ || __arm64ec__ */

#endif /* __WINE_WINE_NEON_STRING_H */
