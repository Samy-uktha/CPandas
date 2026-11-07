/*
 * df_hash.h - small hashing helpers used inside the library (series.c,
 * df_keyindex.c). Not part of the public API, so it lives in src/.
 *
 * A hash turns a value into a 64-bit number so that equal values always
 * get equal numbers and different values (almost always) different ones.
 * Hash tables use it to jump straight to the right slot instead of
 * comparing with every element.
 */
#ifndef DF_HASH_H
#define DF_HASH_H

#include <stdint.h>

/* FNV-1a: a simple, well-known string hash. */
static inline uint64_t df_hash_text(const char *text)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Mixes the bits of a 64-bit number (the "splitmix64" finaliser), so that
   nearby numbers such as 1, 2, 3 end up far apart in the table. */
static inline uint64_t df_hash_u64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Combines the hash of the next key column into the hash so far. */
static inline uint64_t df_hash_combine(uint64_t seed, uint64_t h)
{
    return seed ^ (h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

#endif /* DF_HASH_H */
