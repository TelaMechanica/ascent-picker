/* ascent_types.h — fixed-width types and freestanding primitives.
 *
 * Every module under src/ is compiled TWICE, unchanged:
 *   - for AArch64 with -nostdlib -ffreestanding (the shipped VRX binaries)
 *   - for the host with normal libc + ASan/UBSan (the tests under tests/)
 *
 * That is deliberate: it is what makes the host tests exercise production
 * code rather than a mock. Consequently NOTHING under src/ may include a
 * libc header. Declare what you need; implement the rest here.
 *
 * Both supported targets (x86-64 and aarch64) are LP64, so the widths below
 * are exact on both. A static assert enforces it at compile time.
 */
#ifndef ASCENT_TYPES_H
#define ASCENT_TYPES_H

typedef unsigned char      au8;
typedef signed   char      as8;
typedef unsigned short     au16;
typedef signed   short     as16;
typedef unsigned int       au32;
typedef signed   int       as32;
typedef unsigned long      au64;
typedef signed   long      as64;
typedef unsigned long      ausize;
typedef signed   long      assize;

#define ASCENT_STATIC_ASSERT(c,name) typedef char ascent_sa_##name[(c)?1:-1]
ASCENT_STATIC_ASSERT(sizeof(au8)==1,  u8);
ASCENT_STATIC_ASSERT(sizeof(au16)==2, u16);
ASCENT_STATIC_ASSERT(sizeof(au32)==4, u32);
ASCENT_STATIC_ASSERT(sizeof(au64)==8, u64);
ASCENT_STATIC_ASSERT(sizeof(void*)==8, ptr);

#ifndef ASCENT_NULL
#define ASCENT_NULL ((void*)0)
#endif

/* ---- freestanding primitives (no libc) ---------------------------------- */

static inline void a_memcpy(void *d, const void *s, ausize n)
{
    au8 *D = (au8*)d; const au8 *S = (const au8*)s;
    while (n--) *D++ = *S++;
}

static inline void a_memset(void *d, int c, ausize n)
{
    au8 *D = (au8*)d;
    while (n--) *D++ = (au8)c;
}

static inline int a_memcmp(const void *a, const void *b, ausize n)
{
    const au8 *A = (const au8*)a, *B = (const au8*)b;
    for (; n; n--, A++, B++) if (*A != *B) return (int)*A - (int)*B;
    return 0;
}

static inline ausize a_strnlen(const char *s, ausize cap)
{
    ausize n = 0;
    if (!s) return 0;
    while (n < cap && s[n]) n++;
    return n;
}

/* Bounded copy that ALWAYS NUL-terminates. Returns the number of source
 * characters that did not fit (0 == no truncation). Callers that care about
 * identity must check the return value: silent truncation is how two distinct
 * identities became one object in the pre-fix code. */
static inline ausize a_strlcpy(char *d, const char *s, ausize cap)
{
    ausize i = 0, dropped = 0;
    if (!cap) return s ? a_strnlen(s, (ausize)-1) : 0;
    if (s) { for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
             while (s[i + dropped]) dropped++; }
    d[i] = 0;
    return dropped;
}

static inline int a_streq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* FNV-1a over an explicit length. Used only for object-table bucketing,
 * never as a security primitive. Full keys are always compared on match. */
static inline au64 a_fnv1a(const void *p, ausize n)
{
    const au8 *b = (const au8*)p;
    au64 h = 1469598103934665603ULL;
    while (n--) { h ^= *b++; h *= 1099511628211ULL; }
    return h;
}

/* ---- big/little endian readers ------------------------------------------ */
static inline au16 a_le16(const au8*p){return (au16)((au16)p[0]|((au16)p[1]<<8));}
static inline au32 a_le32(const au8*p){return (au32)p[0]|((au32)p[1]<<8)|((au32)p[2]<<16)|((au32)p[3]<<24);}
static inline as32 a_sle32(const au8*p){return (as32)a_le32(p);}
static inline au16 a_be16(const au8*p){return (au16)(((au16)p[0]<<8)|(au16)p[1]);}
static inline au32 a_be32(const au8*p){return ((au32)p[0]<<24)|((au32)p[1]<<16)|((au32)p[2]<<8)|(au32)p[3];}
static inline as32 a_sbe32(const au8*p){return (as32)a_be32(p);}
static inline float a_lef32(const au8*p){union{au32 u;float f;}v; v.u=a_le32(p); return v.f;}

#endif /* ASCENT_TYPES_H */
