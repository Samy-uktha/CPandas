/*
 * df_join.c - merge two DataFrames on a key column.
 *
 * How it works:
 *   1. Compare every left row with every right row (two nested loops) and
 *      write down each matching pair (left row, right row) in a PairList.
 *      For left/right/outer joins, a row with no partner is paired with
 *      SERIES_TAKE_MISSING, which series_take() turns into a missing cell.
 *   2. Build every result column with series_take() using the left (or
 *      right) row numbers of the pairs.
 */
#include "df_join.h"
#include "df_sort.h"

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

/* Two key cells match if neither is missing and they are equal. */
static int keys_match(const Series *left_key, size_t i,
                      const Series *right_key, size_t j)
{
    if (series_is_missing(left_key, i) || series_is_missing(right_key, j)) {
        return 0;
    }
    if (series_compare_cells(left_key, i, right_key, j) == 0) {
        return 1;
    }
    return 0;
}

/* Fills pairs with the matching rows for the chosen join type. */
static df_status find_pairs(const Series *left_key, const Series *right_key,
                            df_join_type how, PairList *pairs)
{
    size_t nleft = series_len(left_key);
    size_t nright = series_len(right_key);
    df_status st = DF_OK;

    if (how == DF_JOIN_RIGHT) {
        /* Go through the right rows, and look for partners on the left. */
        for (size_t j = 0; j < nright; j++) {
            int found = 0;
            for (size_t i = 0; i < nleft; i++) {
                if (keys_match(left_key, i, right_key, j)) {
                    st = add_pair(pairs, i, j);
                    if (st != DF_OK) return st;
                    found = 1;
                }
            }
            if (!found) {
                st = add_pair(pairs, SERIES_TAKE_MISSING, j);
                if (st != DF_OK) return st;
            }
        }
        return DF_OK;
    }

    /* INNER, LEFT and OUTER: go through the left rows. For OUTER we also
       remember which right rows got a partner (right_used[j] = 1). */
    unsigned char *right_used = NULL;
    if (how == DF_JOIN_OUTER) {
        /* calloc sets everything to 0. The +1 keeps the size above 0, so
           right_used is never NULL for an outer join. */
        right_used = calloc(nright + 1, 1);
        if (right_used == NULL) {
            return DF_ERR_NOMEM;
        }
    }

    for (size_t i = 0; i < nleft; i++) {
        int found = 0;
        for (size_t j = 0; j < nright; j++) {
            if (keys_match(left_key, i, right_key, j)) {
                st = add_pair(pairs, i, j);
                if (st != DF_OK) {
                    free(right_used);
                    return st;
                }
                found = 1;
                if (right_used != NULL) {
                    right_used[j] = 1;
                }
            }
        }
        if (!found && how != DF_JOIN_INNER) {
            st = add_pair(pairs, i, SERIES_TAKE_MISSING);
            if (st != DF_OK) {
                free(right_used);
                return st;
            }
        }
    }

    /* OUTER: add the right rows that never matched, at the end. */
    if (how == DF_JOIN_OUTER) {
        for (size_t j = 0; j < nright; j++) {
            if (right_used[j] == 0) {
                st = add_pair(pairs, SERIES_TAKE_MISSING, j);
                if (st != DF_OK) {
                    free(right_used);
                    return st;
                }
            }
        }
    }

    free(right_used);
    return DF_OK;
}

/* Copies cell src[si] into dst[di]. Both Series have the same dtype. */
static df_status copy_cell(Series *dst, size_t di, const Series *src, size_t si)
{
    if (series_is_missing(src, si)) {
        return series_set_missing(dst, di);
    }

    if (series_dtype(src) == DTYPE_INT) {
        int64_t v = 0;
        series_get_int(src, si, &v);
        return series_set_int(dst, di, v);
    }
    if (series_dtype(src) == DTYPE_FLOAT) {
        double v = 0.0;
        series_get_float(src, si, &v);
        return series_set_float(dst, di, v);
    }
    const char *v = NULL;
    series_get_string(src, si, &v);
    return series_set_string(dst, di, v);
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

/* Builds the result frame from the list of pairs. */
static df_status build_result(const DataFrame *left, const DataFrame *right,
                              const char *on, df_join_type how,
                              const PairList *pairs, DataFrame *result)
{
    const Series *right_key = NULL;
    df_get_column(right, on, &right_key);   /* checked by df_merge */
    df_status st = DF_OK;

    /* 1. All the left columns. */
    size_t nleft_cols = df_ncols(left);
    for (size_t c = 0; c < nleft_cols; c++) {
        const Series *col = NULL;
        df_get_column_at(left, c, &col);
        const char *name = series_name(col);

        if (strcmp(name, on) == 0) {
            /* The key column. */
            Series *part = NULL;
            st = series_take(col, pairs->left, pairs->count, &part);
            if (st != DF_OK) {
                return st;
            }
            /* Rows that exist only in the right frame have no left key,
               so copy the key from the right frame instead. */
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
                return st;
            }
        } else if (has_column(right, name)) {
            /* Same name on both sides: call this one "<name>_left". */
            st = add_joined_column(result, col, pairs->left, pairs->count,
                                   "_left");
        } else {
            st = add_joined_column(result, col, pairs->left, pairs->count,
                                   NULL);
        }
        if (st != DF_OK) {
            return st;
        }
    }

    /* 2. The right columns, except the key (it is already there). */
    size_t nright_cols = df_ncols(right);
    for (size_t c = 0; c < nright_cols; c++) {
        const Series *col = NULL;
        df_get_column_at(right, c, &col);
        const char *name = series_name(col);

        if (strcmp(name, on) == 0) {
            continue;
        }
        if (has_column(left, name)) {
            st = add_joined_column(result, col, pairs->right, pairs->count,
                                   "_right");
        } else {
            st = add_joined_column(result, col, pairs->right, pairs->count,
                                   NULL);
        }
        if (st != DF_OK) {
            return st;
        }
    }
    return DF_OK;
}

/* ---- public function ---------------------------------------------------- */

df_status df_merge(const DataFrame *left, const DataFrame *right,
                   const char *on, df_join_type how, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (left == NULL || right == NULL || on == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (how != DF_JOIN_INNER && how != DF_JOIN_LEFT &&
        how != DF_JOIN_RIGHT && how != DF_JOIN_OUTER) {
        return DF_ERR_INVALID_ARG;
    }

    /* Both frames need the key column, with the same type. */
    const Series *left_key = NULL;
    const Series *right_key = NULL;
    df_status st = df_get_column(left, on, &left_key);
    if (st != DF_OK) {
        return st;
    }
    st = df_get_column(right, on, &right_key);
    if (st != DF_OK) {
        return st;
    }
    if (series_dtype(left_key) != series_dtype(right_key)) {
        return DF_ERR_TYPE_MISMATCH;
    }

    /* Step 1: find the matching rows. */
    PairList pairs;
    pairs.left = NULL;
    pairs.right = NULL;
    pairs.count = 0;
    pairs.cap = 0;
    st = find_pairs(left_key, right_key, how, &pairs);

    /* Step 2: build the result frame. */
    DataFrame *result = NULL;
    if (st == DF_OK) {
        st = df_create(&result);
    }
    if (st == DF_OK) {
        st = build_result(left, right, on, how, &pairs, result);
    }

    free(pairs.left);
    free(pairs.right);

    if (st != DF_OK) {
        df_free(result);    /* safe even if result is NULL */
        return st;
    }
    *out = result;
    return DF_OK;
}
