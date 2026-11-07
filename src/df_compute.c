/*
 * df_compute.c - new columns computed from other columns.
 *
 * Every function builds the whole new Series first and only then adds it
 * to the frame, so a failure half-way (overflow, out of memory) never
 * leaves the frame with a half-made column.
 */
#include "df_compute.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* One operand: a column, or a fixed number (scalar). */
typedef struct {
    const Series *column;   /* NULL for a scalar */
    double        value;    /* the scalar        */
    int           whole;    /* 1 if every value is a whole number */
} Operand;

/* ---- helpers ------------------------------------------------------------ */

static int valid_op(df_arith_op op)
{
    return op == DF_ARITH_ADD || op == DF_ARITH_SUB || op == DF_ARITH_MUL ||
           op == DF_ARITH_DIV;
}

/* Looks up an operand column and checks that it holds numbers. */
static df_status column_operand(const DataFrame *df, const char *name,
                                Operand *operand)
{
    const Series *col = NULL;
    df_status st = df_get_column(df, name, &col);
    if (st != DF_OK) {
        return st;
    }
    df_dtype type = series_dtype(col);
    if (type != DTYPE_INT && type != DTYPE_FLOAT && type != DTYPE_BOOL) {
        return DF_ERR_TYPE_MISMATCH;
    }
    operand->column = col;
    operand->value = 0.0;
    operand->whole = (type == DTYPE_INT || type == DTYPE_BOOL);
    return DF_OK;
}

/* 1 if v has no fraction and fits in int64_t. */
static int is_whole_number(double v)
{
    return v == floor(v) && v >= -9223372036854775808.0 &&
           v < 9223372036854775808.0;
}

/* Reads the operand of `row` as a whole number (only used when whole).
   Returns 0 if the cell is missing. */
static int read_whole(const Operand *operand, size_t row, int64_t *out)
{
    if (operand->column == NULL) {
        *out = (int64_t)operand->value;
        return 1;
    }
    if (series_is_missing(operand->column, row)) {
        return 0;
    }
    if (series_dtype(operand->column) == DTYPE_BOOL) {
        int b = 0;
        series_get_bool(operand->column, row, &b);
        *out = b;
        return 1;
    }
    series_get_int(operand->column, row, out);
    return 1;
}

/* Reads the operand of `row` as a double. Returns 0 if it is missing. */
static int read_number(const Operand *operand, size_t row, double *out)
{
    if (operand->column != NULL &&
        series_dtype(operand->column) == DTYPE_FLOAT) {
        if (series_is_missing(operand->column, row)) {
            return 0;
        }
        series_get_float(operand->column, row, out);
        return 1;
    }
    if (operand->column == NULL) {
        *out = operand->value;
        return 1;
    }
    int64_t v = 0;
    if (!read_whole(operand, row, &v)) {
        return 0;
    }
    *out = (double)v;
    return 1;
}

/* a <op> b for whole numbers, checking that the result fits in int64_t.
   (The checks come before the operation: an overflow itself would already
   be undefined behaviour in C.) */
static df_status whole_arith(int64_t a, df_arith_op op, int64_t b,
                             int64_t *out)
{
    if (op == DF_ARITH_ADD) {
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
            return DF_ERR_OVERFLOW;
        }
        *out = a + b;
        return DF_OK;
    }
    if (op == DF_ARITH_SUB) {
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
            return DF_ERR_OVERFLOW;
        }
        *out = a - b;
        return DF_OK;
    }
    /* DF_ARITH_MUL */
    if (a != 0 && b != 0) {
        if (a > 0 && b > 0 && a > INT64_MAX / b) return DF_ERR_OVERFLOW;
        if (a > 0 && b < 0 && b < INT64_MIN / a) return DF_ERR_OVERFLOW;
        if (a < 0 && b > 0 && a < INT64_MIN / b) return DF_ERR_OVERFLOW;
        if (a < 0 && b < 0 && b < INT64_MAX / a) return DF_ERR_OVERFLOW;
    }
    *out = a * b;
    return DF_OK;
}

/* Builds the column `left <op> right` (one of them may be a scalar). */
static df_status build_arith(const DataFrame *df, const char *new_name,
                             const Operand *left, df_arith_op op,
                             const Operand *right, Series **out)
{
    int whole = left->whole && right->whole && op != DF_ARITH_DIV;
    size_t nrows = df_nrows(df);
    Series *s = NULL;
    df_status st = series_create(new_name, whole ? DTYPE_INT : DTYPE_FLOAT,
                                 nrows, &s);

    for (size_t r = 0; r < nrows && st == DF_OK; r++) {
        if (whole) {
            int64_t a = 0, b = 0, result = 0;
            if (!read_whole(left, r, &a) || !read_whole(right, r, &b)) {
                st = series_append_missing(s);
                continue;
            }
            st = whole_arith(a, op, b, &result);
            if (st == DF_OK) {
                st = series_append_int(s, result);
            }
            continue;
        }

        double a = 0.0, b = 0.0;
        if (!read_number(left, r, &a) || !read_number(right, r, &b) ||
            (op == DF_ARITH_DIV && b == 0.0)) {
            st = series_append_missing(s);      /* missing, or x / 0 */
            continue;
        }
        double result = 0.0;
        switch (op) {
        case DF_ARITH_ADD: result = a + b; break;
        case DF_ARITH_SUB: result = a - b; break;
        case DF_ARITH_MUL: result = a * b; break;
        case DF_ARITH_DIV: result = a / b; break;
        }
        st = series_append_float(s, result);
    }

    if (st != DF_OK) {
        series_free(s);     /* safe with NULL */
        return st;
    }
    *out = s;
    return DF_OK;
}

/* Checks the new name, then hands the finished column to the frame. */
static df_status check_new_name(const DataFrame *df, const char *new_name)
{
    if (new_name[0] == '\0') {
        return DF_ERR_INVALID_ARG;
    }
    size_t idx = 0;
    if (df_find_column(df, new_name, &idx) == DF_OK) {
        return DF_ERR_DUPLICATE_NAME;
    }
    return DF_OK;
}

static df_status attach(DataFrame *df, df_status st, Series *s)
{
    if (st != DF_OK) {
        return st;
    }
    st = df_add_column(df, s);
    if (st != DF_OK) {
        series_free(s);     /* not taken by the frame */
    }
    return st;
}

/* ---- public functions --------------------------------------------------- */

df_status df_add_arith_column(DataFrame *df, const char *new_name,
                              const char *left, df_arith_op op,
                              const char *right)
{
    if (df == NULL || new_name == NULL || left == NULL || right == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!valid_op(op)) {
        return DF_ERR_INVALID_ARG;
    }
    df_status st = check_new_name(df, new_name);
    Operand a = {NULL, 0.0, 0};
    Operand b = {NULL, 0.0, 0};
    if (st == DF_OK) {
        st = column_operand(df, left, &a);
    }
    if (st == DF_OK) {
        st = column_operand(df, right, &b);
    }
    Series *s = NULL;
    if (st == DF_OK) {
        st = build_arith(df, new_name, &a, op, &b, &s);
    }
    return attach(df, st, s);
}

df_status df_add_scalar_column(DataFrame *df, const char *new_name,
                               const char *column, df_arith_op op,
                               double value)
{
    if (df == NULL || new_name == NULL || column == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!valid_op(op)) {
        return DF_ERR_INVALID_ARG;
    }
    df_status st = check_new_name(df, new_name);
    Operand a = {NULL, 0.0, 0};
    Operand b = {NULL, value, is_whole_number(value)};
    if (st == DF_OK) {
        st = column_operand(df, column, &a);
    }
    Series *s = NULL;
    if (st == DF_OK) {
        st = build_arith(df, new_name, &a, op, &b, &s);
    }
    return attach(df, st, s);
}

df_status df_add_computed_column(DataFrame *df, const char *new_name,
                                 df_row_compute compute, void *user_data)
{
    if (df == NULL || new_name == NULL || compute == NULL) {
        return DF_ERR_NULL_ARG;
    }
    df_status st = check_new_name(df, new_name);
    if (st != DF_OK) {
        return st;
    }

    size_t nrows = df_nrows(df);
    Series *s = NULL;
    st = series_create(new_name, DTYPE_FLOAT, nrows, &s);
    for (size_t r = 0; r < nrows && st == DF_OK; r++) {
        double value = 0.0;
        if (compute(df, r, user_data, &value)) {
            st = series_append_float(s, value);
        } else {
            st = series_append_missing(s);
        }
    }
    if (st != DF_OK) {
        series_free(s);
        return st;
    }
    return attach(df, st, s);
}
