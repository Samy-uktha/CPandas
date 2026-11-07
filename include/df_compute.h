/*
 * df_compute.h - add a new column computed from other columns.
 *
 * Examples (like pandas df["bonus"] = df["salary"] * 0.1):
 *     df_add_scalar_column(df, "bonus", "salary", DF_ARITH_MUL, 0.1);
 *     df_add_arith_column(df, "total", "salary", DF_ARITH_ADD, "bonus");
 *
 * For anything else write a function that computes one row and use
 * df_add_computed_column().
 *
 * Unlike the query functions, these CHANGE the frame: they add the new
 * column at the end, like df_add_column(). If they fail, the frame is left
 * exactly as it was.
 *
 * RULES
 *   - Operand columns must be DTYPE_INT, DTYPE_FLOAT or DTYPE_BOOL (False
 *     is 0, True is 1); DF_ERR_TYPE_MISMATCH otherwise.
 *   - If any operand of a row is missing, the result cell is missing.
 *   - Division by zero gives a missing cell (pandas would give inf / NaN,
 *     which CSV files cannot hold).
 *   - Result type: INT when both operands are whole numbers (INT or BOOL
 *     columns, or a scalar with no fraction such as 2.0) and the operation
 *     is +, - or *. Everything else, including every division, is FLOAT.
 *   - An INT result that does not fit in int64_t fails the whole call with
 *     DF_ERR_OVERFLOW.
 *
 * ERRORS
 *   DF_ERR_NOT_FOUND (unknown operand column), DF_ERR_DUPLICATE_NAME
 *   (new_name is already a column), DF_ERR_INVALID_ARG (empty new_name or
 *   unknown op), DF_ERR_TYPE_MISMATCH, DF_ERR_OVERFLOW.
 */
#ifndef DF_COMPUTE_H
#define DF_COMPUTE_H

#include <stddef.h>
#include "df_error.h"
#include "dataframe.h"

typedef enum {
    DF_ARITH_ADD,   /* +  */
    DF_ARITH_SUB,   /* -  */
    DF_ARITH_MUL,   /* *  */
    DF_ARITH_DIV    /* /  */
} df_arith_op;

/* new column = left <op> right, row by row (two columns). */
df_status df_add_arith_column(DataFrame *df, const char *new_name,
                              const char *left, df_arith_op op,
                              const char *right);

/* new column = column <op> value, row by row (a column and a number). */
df_status df_add_scalar_column(DataFrame *df, const char *new_name,
                               const char *column, df_arith_op op,
                               double value);

/* Your own formula. It is called once for every row (row = 0, 1, 2, ...)
 * and must either write the value to *out and return 1, or return 0 to
 * make that cell missing. Read the cells with df_get_column() and
 * series_get_*(). `user_data` is passed through unchanged. The function
 * must not change or free the frame. The new column is DTYPE_FLOAT. */
typedef int (*df_row_compute)(const DataFrame *df, size_t row,
                              void *user_data, double *out);

df_status df_add_computed_column(DataFrame *df, const char *new_name,
                                 df_row_compute compute, void *user_data);

#endif /* DF_COMPUTE_H */
