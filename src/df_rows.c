/*
 * df_rows.c - row selection (head, tail, slice, take rows).
 *
 * The main function is df_take_rows(). It makes a new frame by calling
 * series_take() on every column with the same list of row numbers.
 * head / tail / slice just build that list of row numbers and call it.
 */
#include "df_rows.h"

#include <stdlib.h>

df_status df_take_rows(const DataFrame *df, const size_t *rows, size_t n,
                       DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (n > 0 && rows == NULL) {
        return DF_ERR_NULL_ARG;
    }

    /* Check every row number first, so we don't build half a frame. */
    size_t nrows = df_nrows(df);
    for (size_t k = 0; k < n; k++) {
        if (rows[k] >= nrows) {
            return DF_ERR_INDEX_OUT_OF_RANGE;
        }
    }

    DataFrame *result = NULL;
    df_status st = df_create(&result);
    if (st != DF_OK) {
        return st;
    }

    /* Copy the chosen rows of each column, one column at a time. */
    size_t ncols = df_ncols(df);
    for (size_t c = 0; c < ncols; c++) {
        const Series *col = NULL;
        st = df_get_column_at(df, c, &col);
        if (st != DF_OK) {
            df_free(result);
            return st;
        }

        Series *part = NULL;
        st = series_take(col, rows, n, &part);
        if (st != DF_OK) {
            df_free(result);
            return st;
        }

        st = df_add_column(result, part);
        if (st != DF_OK) {
            /* df_add_column only takes the Series when it succeeds,
               so here we still have to free it ourselves. */
            series_free(part);
            df_free(result);
            return st;
        }
    }

    *out = result;
    return DF_OK;
}

df_status df_slice(const DataFrame *df, size_t start, size_t end,
                   DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }

    /* Fix up the range the same way Python does for list[start:end]. */
    size_t nrows = df_nrows(df);
    if (end > nrows) {
        end = nrows;
    }
    if (start > end) {
        start = end;
    }
    size_t count = end - start;

    /* Empty range: no list needed, df_take_rows accepts NULL with n = 0. */
    if (count == 0) {
        return df_take_rows(df, NULL, 0, out);
    }

    /* Build the list start, start+1, ..., end-1. */
    size_t *rows = malloc(count * sizeof(size_t));
    if (rows == NULL) {
        return DF_ERR_NOMEM;
    }
    for (size_t k = 0; k < count; k++) {
        rows[k] = start + k;
    }

    df_status st = df_take_rows(df, rows, count, out);
    free(rows);
    return st;
}

df_status df_head(const DataFrame *df, size_t n, DataFrame **out)
{
    /* The first n rows are rows 0 .. n-1. df_slice() handles n > nrows. */
    return df_slice(df, 0, n, out);
}

df_status df_tail(const DataFrame *df, size_t n, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }

    size_t nrows = df_nrows(df);
    if (n > nrows) {
        n = nrows;
    }
    /* The last n rows are rows nrows-n .. nrows-1. */
    return df_slice(df, nrows - n, nrows, out);
}
