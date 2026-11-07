/*
 * df_join.c - merge two DataFrames on one or more key columns.
 *
 * How it works (a "hash join"):
 *   1. Build a hash table of one frame's keys (df_keyindex.c): every
 *      different key with the list of its rows. That is the right frame,
 *      or the left frame for a right join.
 *   2. Go through the rows of the other frame and look each key up in the
 *      table: O(1) on average instead of comparing with every row. Write
 *      down each matching pair (left row, right row) in a PairList. For
 *      left/right/outer joins, a row with no partner is paired with
 *      SERIES_TAKE_MISSING, which series_take() turns into a missing cell.
 *   3. Build every result column with series_take() using the left (or
 *      right) row numbers of the pairs.
 * Total O(n + m + number of result rows), where the first version (two
 * nested loops comparing every pair) was O(n * m). The pairs come out in
 * exactly the same order as before.
 */
#include "df_join.h"
#include "df_keyindex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A growing list of (left row, right row) pairs. */
typedef struct {
    size_t *left;
    size_t *right;
    size_t  count;
    size_t  cap;
} PairList;

/* ---- helpers ------------------------------------------------------------ */

/* Adds one pair to the list, making the arrays bigger when they are full. */
static df_status add_pair(PairList *pairs, size_t left_row, size_t right_row)
{
    if (pairs->count == pairs->cap) {
        size_t new_cap = 16;
        if (pairs->cap > 0) {
            new_cap = pairs->cap * 2;
        }

        /* Use temporary pointers so we don't lose the old arrays if
           realloc fails. */
        size_t *new_left = realloc(pairs->left, new_cap * sizeof(size_t));
        if (new_left == NULL) {
            return DF_ERR_NOMEM;
        }
        pairs->left = new_left;

        size_t *new_right = realloc(pairs->right, new_cap * sizeof(size_t));
        if (new_right == NULL) {
            return DF_ERR_NOMEM;   /* cap unchanged, list still valid */
        }
        pairs->right = new_right;

        pairs->cap = new_cap;
    }

    pairs->left[pairs->count] = left_row;
    pairs->right[pairs->count] = right_row;
    pairs->count++;
    return DF_OK;
}

/* RIGHT join: index the left keys, then go through the right rows. */
static df_status find_pairs_right(const Series *const *left_keys,
                                  size_t nleft,
                                  const Series *const *right_keys,
                                  size_t nright, size_t nkeys,
                                  PairList *pairs)
{
    KeyIndex left_index;
    df_status st = key_index_build(&left_index, left_keys, nkeys, nleft);
    if (st != DF_OK) {
        return st;
    }

    for (size_t j = 0; j < nright && st == DF_OK; j++) {
        size_t g = key_index_find(&left_index, right_keys, j);
        if (g == KEY_NO_GROUP) {
            st = add_pair(pairs, SERIES_TAKE_MISSING, j);
            continue;
        }
        /* every left row with this key, in left row order */
        for (size_t k = left_index.start[g];
             k < left_index.start[g + 1] && st == DF_OK; k++) {
            st = add_pair(pairs, left_index.members[k], j);
        }
    }

    key_index_free(&left_index);
    return st;
}

/* INNER, LEFT and OUTER joins: index the right keys, then go through the
   left rows. */
static df_status find_pairs_left(const Series *const *left_keys,
                                 size_t nleft,
                                 const Series *const *right_keys,
                                 size_t nright, size_t nkeys,
                                 df_join_type how, PairList *pairs)
{
    KeyIndex right_index;
    df_status st = key_index_build(&right_index, right_keys, nkeys, nright);
    if (st != DF_OK) {
        return st;
    }

    /* For OUTER we also remember which right groups got a partner. The +1
       keeps the size above 0, so the pointer is never NULL. */
    unsigned char *group_used = NULL;
    if (how == DF_JOIN_OUTER) {
        group_used = calloc(right_index.ngroups + 1, 1);
        if (group_used == NULL) {
            key_index_free(&right_index);
            return DF_ERR_NOMEM;
        }
    }

    for (size_t i = 0; i < nleft && st == DF_OK; i++) {
        size_t g = key_index_find(&right_index, left_keys, i);
        if (g == KEY_NO_GROUP) {
            if (how != DF_JOIN_INNER) {
                st = add_pair(pairs, i, SERIES_TAKE_MISSING);
            }
            continue;
        }
        /* every right row with this key, in right row order */
        for (size_t k = right_index.start[g];
             k < right_index.start[g + 1] && st == DF_OK; k++) {
            st = add_pair(pairs, i, right_index.members[k]);
        }
        if (group_used != NULL) {
            group_used[g] = 1;
        }
    }

    /* OUTER: add the right rows that never matched, in right row order.
       That is a row with a missing key, or whose group was never used. */
    if (how == DF_JOIN_OUTER) {
        for (size_t j = 0; j < nright && st == DF_OK; j++) {
            size_t g = key_index_find(&right_index, right_keys, j);
            if (g == KEY_NO_GROUP || group_used[g] == 0) {
                st = add_pair(pairs, SERIES_TAKE_MISSING, j);
            }
        }
    }

    free(group_used);
    key_index_free(&right_index);
    return st;
}

/* Copies cell src[si] into dst[di]. Both Series have the same dtype. */
static df_status copy_cell(Series *dst, size_t di, const Series *src, size_t si)
{
    if (series_is_missing(src, si)) {
        return series_set_missing(dst, di);
    }

    switch (series_dtype(src)) {
    case DTYPE_INT: {
        int64_t v = 0;
        series_get_int(src, si, &v);
        return series_set_int(dst, di, v);
    }
    case DTYPE_FLOAT: {
        double v = 0.0;
        series_get_float(src, si, &v);
        return series_set_float(dst, di, v);
    }
    case DTYPE_BOOL: {
        int v = 0;
        series_get_bool(src, si, &v);
        return series_set_bool(dst, di, v);
    }
    case DTYPE_DATE: {
        int64_t v = 0;
        series_get_date(src, si, &v);
        return series_set_date(dst, di, v);
    }
    case DTYPE_STRING: {
        const char *v = NULL;
        series_get_string(src, si, &v);
        return series_set_string(dst, di, v);
    }
    }
    return DF_ERR_INVALID_ARG;
}

/* Renames s to "<its name><suffix>", e.g. "name" -> "name_left". */
static df_status add_suffix(Series *s, const char *suffix)
{
    const char *old_name = series_name(s);
    size_t len = strlen(old_name) + strlen(suffix) + 1;
    char *new_name = malloc(len);
    if (new_name == NULL) {
        return DF_ERR_NOMEM;
    }
    snprintf(new_name, len, "%s%s", old_name, suffix);
    df_status st = series_rename(s, new_name);   /* rename copies the name */
    free(new_name);
    return st;
}

/* Returns 1 if the frame has a column with this name. */
static int has_column(const DataFrame *df, const char *name)
{
    size_t idx = 0;
    if (df_find_column(df, name, &idx) == DF_OK) {
        return 1;
    }
    return 0;
}

/* The position of `name` in on[0..non-1], or non if it is not a key. */
static size_t key_position(const char *name, const char *const *on,
                           size_t non)
{
    for (size_t k = 0; k < non; k++) {
        if (strcmp(name, on[k]) == 0) {
            return k;
        }
    }
    return non;
}

/* Adds column `col`, using the row numbers in rows[0..n-1], to `result`.
   suffix is added to the name if it is not NULL. */
static df_status add_joined_column(DataFrame *result, const Series *col,
                                   const size_t *rows, size_t n,
                                   const char *suffix)
{
    Series *part = NULL;
    df_status st = series_take(col, rows, n, &part);
    if (st != DF_OK) {
        return st;
    }
    if (suffix != NULL) {
        st = add_suffix(part, suffix);
        if (st != DF_OK) {
            series_free(part);
            return st;
        }
    }
    st = df_add_column(result, part);
    if (st != DF_OK) {
        series_free(part);
    }
    return st;
}

/* Adds key column `col` of the left frame. Rows that exist only in the
   right frame have no left key, so their key is copied from right_key. */
static df_status add_key_column(DataFrame *result, const Series *col,
                                const Series *right_key, df_join_type how,
                                const PairList *pairs)
{
    Series *part = NULL;
    df_status st = series_take(col, pairs->left, pairs->count, &part);
    if (st != DF_OK) {
        return st;
    }
    if (how == DF_JOIN_RIGHT || how == DF_JOIN_OUTER) {
        for (size_t k = 0; k < pairs->count && st == DF_OK; k++) {
            if (pairs->left[k] == SERIES_TAKE_MISSING) {
                st = copy_cell(part, k, right_key, pairs->right[k]);
            }
        }
    }
    if (st == DF_OK) {
        st = df_add_column(result, part);
    }
    if (st != DF_OK) {
        series_free(part);
    }
    return st;
}

/* Builds the result frame from the list of pairs. */
static df_status build_result(const DataFrame *left, const DataFrame *right,
                              const char *const *on, size_t non,
                              const Series *const *right_keys,
                              df_join_type how, const PairList *pairs,
                              DataFrame *result)
{
    df_status st = DF_OK;

    /* 1. All the left columns. */
    size_t nleft_cols = df_ncols(left);
    for (size_t c = 0; c < nleft_cols && st == DF_OK; c++) {
        const Series *col = NULL;
        df_get_column_at(left, c, &col);
        const char *name = series_name(col);
        size_t k = key_position(name, on, non);

        if (k < non) {
            st = add_key_column(result, col, right_keys[k], how, pairs);
        } else if (has_column(right, name)) {
            /* Same name on both sides: call this one "<name>_left". */
            st = add_joined_column(result, col, pairs->left, pairs->count,
                                   "_left");
        } else {
            st = add_joined_column(result, col, pairs->left, pairs->count,
                                   NULL);
        }
    }

    /* 2. The right columns, except the keys (they are already there). */
    size_t nright_cols = df_ncols(right);
    for (size_t c = 0; c < nright_cols && st == DF_OK; c++) {
        const Series *col = NULL;
        df_get_column_at(right, c, &col);
        const char *name = series_name(col);

        if (key_position(name, on, non) < non) {
            continue;
        }
        if (has_column(left, name)) {
            st = add_joined_column(result, col, pairs->right, pairs->count,
                                   "_right");
        } else {
            st = add_joined_column(result, col, pairs->right, pairs->count,
                                   NULL);
        }
    }
    return st;
}

/* Looks up the key columns in both frames and checks their types. */
static df_status find_keys(const DataFrame *left, const DataFrame *right,
                           const char *const *on, size_t non,
                           const Series **left_keys,
                           const Series **right_keys)
{
    for (size_t k = 0; k < non; k++) {
        if (on[k] == NULL) {
            return DF_ERR_NULL_ARG;
        }
        for (size_t j = 0; j < k; j++) {
            if (strcmp(on[j], on[k]) == 0) {
                return DF_ERR_DUPLICATE_NAME;
            }
        }
        df_status st = df_get_column(left, on[k], &left_keys[k]);
        if (st != DF_OK) {
            return st;
        }
        st = df_get_column(right, on[k], &right_keys[k]);
        if (st != DF_OK) {
            return st;
        }
        if (series_dtype(left_keys[k]) != series_dtype(right_keys[k])) {
            return DF_ERR_TYPE_MISMATCH;
        }
    }
    return DF_OK;
}

/* ---- public functions --------------------------------------------------- */

df_status df_merge_on(const DataFrame *left, const DataFrame *right,
                      const char *const *on, size_t non, df_join_type how,
                      DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (left == NULL || right == NULL || on == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (non == 0) {
        return DF_ERR_INVALID_ARG;
    }
    if (how != DF_JOIN_INNER && how != DF_JOIN_LEFT &&
        how != DF_JOIN_RIGHT && how != DF_JOIN_OUTER) {
        return DF_ERR_INVALID_ARG;
    }

    /* Both frames need every key column, with the same types. */
    const Series **left_keys = malloc(non * sizeof(const Series *));
    const Series **right_keys = malloc(non * sizeof(const Series *));
    if (left_keys == NULL || right_keys == NULL) {
        free(left_keys);
        free(right_keys);
        return DF_ERR_NOMEM;
    }
    df_status st = find_keys(left, right, on, non, left_keys, right_keys);

    /* Step 1 and 2: find the matching rows. */
    PairList pairs;
    pairs.left = NULL;
    pairs.right = NULL;
    pairs.count = 0;
    pairs.cap = 0;
    if (st == DF_OK && how == DF_JOIN_RIGHT) {
        st = find_pairs_right(left_keys, df_nrows(left), right_keys,
                              df_nrows(right), non, &pairs);
    } else if (st == DF_OK) {
        st = find_pairs_left(left_keys, df_nrows(left), right_keys,
                             df_nrows(right), non, how, &pairs);
    }

    /* Step 3: build the result frame. */
    DataFrame *result = NULL;
    if (st == DF_OK) {
        st = df_create(&result);
    }
    if (st == DF_OK) {
        st = build_result(left, right, on, non, right_keys, how, &pairs,
                          result);
    }

    free(pairs.left);
    free(pairs.right);
    free(left_keys);
    free(right_keys);

    if (st != DF_OK) {
        df_free(result);    /* safe even if result is NULL */
        return st;
    }
    *out = result;
    return DF_OK;
}

df_status df_merge(const DataFrame *left, const DataFrame *right,
                   const char *on, df_join_type how, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (on == NULL) {
        return DF_ERR_NULL_ARG;
    }
    const char *keys[1];
    keys[0] = on;
    return df_merge_on(left, right, keys, 1, how, out);
}
