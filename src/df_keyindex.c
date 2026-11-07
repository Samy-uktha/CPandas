/*
 * df_keyindex.c - group rows by their key values with a hash table.
 *
 * How it works:
 *   1. For every row, hash its key cells into one 64-bit number.
 *   2. Look that number up in an open-addressing table whose slots hold
 *      group numbers. A slot is a match if the group has the same hash
 *      AND its first row really has equal keys (two different keys can
 *      have the same hash, so the keys are always compared too).
 *   3. No match: the row starts a new group in the first empty slot.
 *   4. At the end, a counting pass lists the rows of each group together
 *      (members), keeping row order inside every group.
 * The table has at least twice as many slots as rows, so it never fills
 * up and lookups stay short.
 */
#include "df_keyindex.h"
#include "df_hash.h"

#include <stdlib.h>
#include <string.h>

/* ---- hashing and comparing key cells ------------------------------------ */

/* Hash of one present cell. Equal cells must give equal hashes. */
static uint64_t cell_hash(const Series *s, size_t row)
{
    switch (series_dtype(s)) {
    case DTYPE_INT: {
        int64_t v = 0;
        series_get_int(s, row, &v);
        return df_hash_u64((uint64_t)v);
    }
    case DTYPE_DATE: {
        int64_t v = 0;
        series_get_date(s, row, &v);
        return df_hash_u64((uint64_t)v);
    }
    case DTYPE_FLOAT: {
        double v = 0.0;
        series_get_float(s, row, &v);
        if (v == 0.0) {
            v = 0.0;            /* -0.0 == 0.0, so they must hash the same */
        }
        uint64_t bits = 0;
        if (v == v) {           /* every NaN gets bits 0, see cells_equal */
            memcpy(&bits, &v, sizeof bits);
        }
        return df_hash_u64(bits);
    }
    case DTYPE_BOOL: {
        int v = 0;
        series_get_bool(s, row, &v);
        return df_hash_u64((uint64_t)v);
    }
    case DTYPE_STRING: {
        const char *v = NULL;
        series_get_string(s, row, &v);
        return df_hash_text(v);
    }
    }
    return 0;
}

/* 1 if two present cells (same dtype) hold the same value. */
static int cells_equal(const Series *a, size_t i, const Series *b, size_t j)
{
    switch (series_dtype(a)) {
    case DTYPE_INT: {
        int64_t x = 0, y = 0;
        series_get_int(a, i, &x);
        series_get_int(b, j, &y);
        return x == y;
    }
    case DTYPE_DATE: {
        int64_t x = 0, y = 0;
        series_get_date(a, i, &x);
        series_get_date(b, j, &y);
        return x == y;
    }
    case DTYPE_FLOAT: {
        double x = 0.0, y = 0.0;
        series_get_float(a, i, &x);
        series_get_float(b, j, &y);
        return x == y || (x != x && y != y);    /* NaN groups with NaN */
    }
    case DTYPE_BOOL: {
        int x = 0, y = 0;
        series_get_bool(a, i, &x);
        series_get_bool(b, j, &y);
        return x == y;
    }
    case DTYPE_STRING: {
        const char *x = NULL;
        const char *y = NULL;
        series_get_string(a, i, &x);
        series_get_string(b, j, &y);
        /* equal texts in one Series share their storage, so x == y is
           the quick answer most of the time */
        return x == y || strcmp(x, y) == 0;
    }
    }
    return 0;
}

int key_row_has_missing(const Series *const *keys, size_t nkeys, size_t row)
{
    for (size_t k = 0; k < nkeys; k++) {
        if (series_is_missing(keys[k], row)) {
            return 1;
        }
    }
    return 0;
}

static uint64_t row_hash(const Series *const *keys, size_t nkeys, size_t row)
{
    uint64_t h = 0x2545F4914F6CDD1DULL;
    for (size_t k = 0; k < nkeys; k++) {
        h = df_hash_combine(h, cell_hash(keys[k], row));
    }
    return h;
}

static int rows_equal(const Series *const *a, size_t i,
                      const Series *const *b, size_t j, size_t nkeys)
{
    for (size_t k = 0; k < nkeys; k++) {
        if (!cells_equal(a[k], i, b[k], j)) {
            return 0;
        }
    }
    return 1;
}

/* The slot holding the group of (keys, row), or the empty slot where that
   group would go. */
static size_t find_slot(const KeyIndex *index, const Series *const *keys,
                        size_t row, uint64_t hash)
{
    size_t mask = index->nslots - 1;
    size_t k = (size_t)hash & mask;
    while (index->slots[k] != 0) {
        size_t g = index->slots[k] - 1;
        if (index->group_hash[g] == hash &&
            rows_equal(index->keys, index->first_row[g], keys, row,
                       index->nkeys)) {
            break;
        }
        k = (k + 1) & mask;
    }
    return k;
}

/* ---- public (inside the library) functions ------------------------------ */

df_status key_index_build(KeyIndex *index, const Series *const *keys,
                          size_t nkeys, size_t nrows)
{
    memset(index, 0, sizeof *index);
    index->keys = keys;
    index->nkeys = nkeys;

    /* At least 2 slots per row, and a power of two so that "& mask" can
       replace the slower "% nslots". */
    size_t nslots = 16;
    while (nslots < 2 * nrows) {
        if (nslots > SIZE_MAX / 2 / sizeof(size_t)) {
            return DF_ERR_NOMEM;
        }
        nslots *= 2;
    }
    index->nslots = nslots;

    /* nrows + 1 so the sizes are never 0 (malloc(0) may give NULL). */
    index->slots = calloc(nslots, sizeof(size_t));
    index->first_row = malloc((nrows + 1) * sizeof(size_t));
    index->group_hash = malloc((nrows + 1) * sizeof(uint64_t));
    size_t *row_group = malloc((nrows + 1) * sizeof(size_t));
    if (index->slots == NULL || index->first_row == NULL ||
        index->group_hash == NULL || row_group == NULL) {
        free(row_group);
        key_index_free(index);
        return DF_ERR_NOMEM;
    }

    /* Steps 1-3: give every row its group number. */
    size_t nvalid = 0;
    for (size_t r = 0; r < nrows; r++) {
        if (key_row_has_missing(keys, nkeys, r)) {
            row_group[r] = KEY_NO_GROUP;
            continue;
        }
        uint64_t hash = row_hash(keys, nkeys, r);
        size_t slot = find_slot(index, keys, r, hash);
        if (index->slots[slot] == 0) {
            size_t g = index->ngroups;      /* a key we have not seen */
            index->first_row[g] = r;
            index->group_hash[g] = hash;
            index->slots[slot] = g + 1;
            index->ngroups++;
        }
        row_group[r] = index->slots[slot] - 1;
        nvalid++;
    }

    /* Step 4: count the rows of each group, turn the counts into start
       positions, then drop every row into its group's part of members. */
    size_t ngroups = index->ngroups;
    index->start = calloc(ngroups + 1, sizeof(size_t));
    index->members = malloc((nvalid + 1) * sizeof(size_t));
    size_t *next = malloc((ngroups + 1) * sizeof(size_t));
    if (index->start == NULL || index->members == NULL || next == NULL) {
        free(next);
        free(row_group);
        key_index_free(index);
        return DF_ERR_NOMEM;
    }
    for (size_t r = 0; r < nrows; r++) {
        if (row_group[r] != KEY_NO_GROUP) {
            index->start[row_group[r] + 1]++;
        }
    }
    for (size_t g = 0; g < ngroups; g++) {
        index->start[g + 1] += index->start[g];
        next[g] = index->start[g];
    }
    for (size_t r = 0; r < nrows; r++) {
        size_t g = row_group[r];
        if (g != KEY_NO_GROUP) {
            index->members[next[g]] = r;
            next[g]++;
        }
    }

    free(next);
    free(row_group);
    return DF_OK;
}

size_t key_index_find(const KeyIndex *index, const Series *const *other_keys,
                      size_t row)
{
    if (key_row_has_missing(other_keys, index->nkeys, row)) {
        return KEY_NO_GROUP;    /* a missing key never matches */
    }
    uint64_t hash = row_hash(other_keys, index->nkeys, row);
    size_t slot = find_slot(index, other_keys, row, hash);
    if (index->slots[slot] == 0) {
        return KEY_NO_GROUP;
    }
    return index->slots[slot] - 1;
}

void key_index_free(KeyIndex *index)
{
    free(index->first_row);
    free(index->start);
    free(index->members);
    free(index->group_hash);
    free(index->slots);
    memset(index, 0, sizeof *index);
}
