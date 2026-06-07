/* Freestanding compiler-support routines for the HDPart OS build.
 * The Bartman toolchain ships no libc/libgcc, but GCC emits calls to
 * memcpy/memset for struct copies, array initializers, and SDK varargs
 * macros (e.g. OpenWindowTags).  GCC 15 on m68000 also emits __mulsi3
 * for 32-bit multiplication (the 68000 only has 16x16->32 mulu).
 * Provide minimal implementations. */
#include <stddef.h>

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

/* 32x32->32 multiply for m68000 (no hardware 32-bit mul).
 * Uses the 16x16->32 unsigned multiply instruction via inline asm.
 * This function must not itself trigger a __mulsi3 call; the attribute
 * ensures GCC does not replace the inner operations with library calls. */
__attribute__((used))
__attribute__((optimize("no-tree-loop-distribute-patterns")))
long __mulsi3(long a, long b)
{
    /* Decompose: a = ah*65536 + al,  b = bh*65536 + bl
       a*b (low 32) = al*bl + (al*bh + ah*bl)*65536  (high halves drop out) */
    unsigned short al = (unsigned short)a;
    unsigned short ah = (unsigned short)((unsigned long)a >> 16);
    unsigned short bl = (unsigned short)b;
    unsigned short bh = (unsigned short)((unsigned long)b >> 16);
    unsigned long result;
    /* Use GCC's 16x16 unsigned multiply — maps to mulu on 68000. */
    unsigned long albl, albh, ahbl;
    __asm("mulu.w %1,%0" : "=d"(albl) : "mid"(bl), "0"((unsigned long)al));
    __asm("mulu.w %1,%0" : "=d"(albh) : "mid"(bh), "0"((unsigned long)al));
    __asm("mulu.w %1,%0" : "=d"(ahbl) : "mid"(bl), "0"((unsigned long)ah));
    result = albl + ((albh + ahbl) << 16);
    return (long)result;
}

/* 32-bit software division/remainder. The 68000 has no 32/32-bit divide and
 * this toolchain ships no libgcc, so GCC's calls to these helpers must be
 * satisfied here. Shift-subtract long division. */
unsigned int __udivsi3(unsigned int num, unsigned int den)
{
    unsigned int quot = 0;
    unsigned int bit  = 1;
    if (den == 0) return 0xFFFFFFFFu;            /* avoid hang on divide-by-0 */
    while (den <= num && (den & 0x80000000u) == 0) { den <<= 1; bit <<= 1; }
    while (bit) {
        if (num >= den) { num -= den; quot |= bit; }
        den >>= 1; bit >>= 1;
    }
    return quot;
}

unsigned int __umodsi3(unsigned int num, unsigned int den)
{
    unsigned int bit = 1;
    if (den == 0) return num;
    while (den <= num && (den & 0x80000000u) == 0) { den <<= 1; bit <<= 1; }
    while (bit) {
        if (num >= den) num -= den;
        den >>= 1; bit >>= 1;
    }
    return num;
}

int __divsi3(int a, int b)
{
    int neg = 0;
    unsigned int q;
    if (a < 0) { a = -a; neg ^= 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    q = __udivsi3((unsigned int)a, (unsigned int)b);
    return neg ? -(int)q : (int)q;
}

int __modsi3(int a, int b)
{
    int neg = (a < 0);
    unsigned int r = __umodsi3((unsigned int)(a < 0 ? -a : a),
                               (unsigned int)(b < 0 ? -b : b));
    return neg ? -(int)r : (int)r;
}
