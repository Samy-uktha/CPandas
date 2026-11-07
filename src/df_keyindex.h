/*
 * df_keyindex.h - a hash table that groups the rows of a frame by the
 * values in some key columns. Used inside the library by group-by, join
 * and nunique; not part of the public API, so it lives in src/.
 *
 * After key_index_build(), every row whose keys are all present belongs
 * to exactly one group, and rows with equal keys share a group:
 *
 *     ngroups              how many different keys there are
 *     first_row[g]         the first row (in row order) with key g
 *     members[start[g] .. start[g+1]-1]
 *                          all rows with key g, in row order
 *
 * Groups are numbered in order of their first row. Rows with a missing
 * key cell are in no group. Building is O(n) on average; key_index_find()
 * looks up the group of a row of ANOTHER frame (with key columns of the
 * same types) in O(1) on average. That is what makes the hash join fast.
 */
#ifndef DF_KEYINDEX_H
#define DF_KEYINDEX_H

#include <stddef.h>
#include <stdint.h>
#include "series.h"

#define KEY_NO_GROUP ((size_t)-1)

typedef struct {
    const Series *const *keys;   /* the key columns (borrowed)           */
    size_t   nkeys;
    size_t   ngroups;
    size_t  *first_row;          /* [ngroups]                            */
    size_t  *start;              /* [ngroups + 1]                        */
    size_t  *members;            /* [start[ngroups]]                     */
    uint64_t *group_hash;        /* [ngroups] hash of each group's key   */
    size_t  *slots;              /* hash table: group number + 1, 0 = empty */
    size_t   nslots;             /* a power of two                       */
} KeyIndex;

/* Groups rows 0 .. nrows-1 by keys[0..nkeys-1] (each of length nrows).
 * The key columns must stay alive and unchanged while the index is used.
 * On failure (DF_ERR_NOMEM) nothing needs freeing. */
df_status key_index_build(KeyIndex *index, const Series *const *keys,
                          size_t nkeys, size_t nrows);

/* The group whose key equals row `row` of other_keys (same number and
 * types of columns as the index), or KEY_NO_GROUP if there is none or a
 * key cell of that row is missing. */
size_t key_index_find(const KeyIndex *index, const Series *const *other_keys,
                      size_t row);

void key_index_free(KeyIndex *index);

/* 1 if any key cell of `row` is missing. */
int key_row_has_missing(const Series *const *keys, size_t nkeys, size_t row);

#endif /* DF_KEYINDEX_H */
