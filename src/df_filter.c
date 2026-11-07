/*
 * df_filter.c - keep only the rows that match a condition.
 *
 * Every filter works the same way:
 *   1. make an array big enough for every row number,
 *   2. go through the rows and write down the ones that match,
 *   3. call df_take_rows() with that list to build the result.
 */
#include "df_filter.h"
#include "df_rows.h"

#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

static int valid_op(df_compare_op op)
{
    if (op == DF_OP_EQ || op == DF_OP_NE || op == DF_OP_LT ||
        op == DF_OP_LE || op == DF_OP_GT || op == DF_OP_GE) {
        return 1;
    }
    return 0;
}

/* `diff` is negative if cell < value, 0 if equal, positive if cell > value.
   Returns 1 if that result satisfies the operator. */
static int op_matches(df_compare_op op, int diff)
{
    switch (op) {
    case DF_OP_EQ: return diff == 0;
    case DF_OP_NE: return diff != 0;
    case DF_OP_LT: return diff < 0;
    case DF_OP_LE: return diff <= 0;
    case DF_OP_GT: return diff > 0;
    case DF_OP_GE: return diff >= 0;
    }
    return 0;
}

/* Compares two doubles and gives -1, 0 or 1. */
static int compare_doubles(double a, double b)
{
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

/* Allocates the array that will hold the matching row numbers: room for
   every row, plus one so the size is never 0 (malloc(0) may give NULL). */
static df_status make_row_list(const DataFrame *df, size_t **rows)
{
    *rows = malloc((df_nrows(df) + 1) * sizeof(size_t));
    if (*rows == NULL) {
        return DF_ERR_NOMEM;
    }
    return DF_OK;
}

/* Builds the result from the list of kept rows and frees the list. */
static df_status finish_filter(const DataFrame *df, size_t *rows,
                               size_t count, DataFrame **out)
{
    df_status st = df_take_rows(df, rows, count, out);
    free(rows);
    return st;
}

/* ---- public functions --------------------------------------------------- */

df_status df_filter_number(const DataFrame *df, const char *column,
                           df_compare_op op, double value, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || column == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!valid_op(op)) {
        return DF_ERR_INVALID_ARG;
    }

    const Series *col = NULL;
    df_status st = df_get_column(df, column, &col);
    if (st != DF_OK) {
        return st;
    }
    df_dtype type = series_dtype(col);
    if (type != DTYPE_INT && type != DTYPE_FLOAT) {
        return DF_ERR_TYPE_MISMATCH;
    }

    size_t *rows = NULL;
    st = make_row_list(df, &rows);
    if (st != DF_OK) {
        return st;
    }

    size_t count = 0;
    size_t nrows = df_nrows(df);
    for (size_t r = 0; r < nrows; r++) {
        if (series_is_missing(col, r)) {
            continue;   /* missing never matches */
        }

        /* Read the cell as a double, whatever the number type is. */
        double cell = 0.0;
        if (type == DTYPE_INT) {
            int64_t iv = 0;
            series_get_int(col, r, &iv);
            cell = (double)iv;
        } else {
            series_get_float(col, r, &cell);
        }

        if (op_matches(op, compare_doubles(cell, value))) {
            rows[count] = r;
            count++;
        }
    }

    return finish_filter(df, rows, count, out);
}

df_status df_filter_string(const DataFrame *df, const char *column,
                           df_compare_op op, const char *value,
                           DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || column == NULL || value == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!valid_op(op)) {
        return DF_ERR_INVALID_ARG;
    }

    const Series *col = NULL;
    df_status st = df_get_column(df, column, &col);
    if (st != DF_OK) {
        return st;
    }
    if (series_dtype(col) != DTYPE_STRING) {
        return DF_ERR_TYPE_MISMATCH;
    }

    size_t *rows = NULL;
    st = make_row_list(df, &rows);
    if (st != DF_OK) {
        return st;
    }

    size_t count = 0;
    size_t nrows = df_nrows(df);
    for (size_t r = 0; r < nrows; r++) {
        if (series_is_missing(col, r)) {
            continue;   /* missing never matches */
        }

        const char *cell = NULL;
        series_get_string(col, r, &cell);

        /* strcmp can return any negative/positive number, op_matches only
           looks at the sign so that is fine. */
        if (op_matches(op, strcmp(cell, value))) {
            rows[count] = r;
            count++;
        }
    }

    return finish_filter(df, rows, count, out);
}

df_status df_filter_missing(const DataFrame *df, const char *column,
                            int keep_missing, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || column == NULL) {
        return DF_ERR_NULL_ARG;
    }

    const Series *col = NULL;
    df_status st = df_get_column(df, column, &col);
    if (st != DF_OK) {
        return st;
    }

    size_t *rows = NULL;
    st = make_row_list(df, &rows);
    if (st != DF_OK) {
        return st;
    }

    size_t count = 0;
    size_t nrows = df_nrows(df);
    for (size_t r = 0; r < nrows; r++) {
        int missing = series_is_missing(col, r);
        /* keep the row if "is it missing?" matches what the caller wants */
        if ((keep_missing && missing) || (!keep_missing && !missing)) {
            rows[count] = r;
            count++;
        }
    }

    return finish_filter(df, rows, count, out);
}

df_status df_filter_custom(const DataFrame *df, df_row_condition keep,
                           void *user_data, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || keep == NULL) {
        return DF_ERR_NULL_ARG;
    }

    size_t *rows = NULL;
    df_status st = make_row_list(df, &rows);
    if (st != DF_OK) {
        return st;
    }

    size_t count = 0;
    size_t nrows = df_nrows(df);
    for (size_t r = 0; r < nrows; r++) {
        if (keep(df, r, user_data)) {
            rows[count] = r;
            count++;
        }
    }

    return finish_filter(df, rows, count, out);
}
