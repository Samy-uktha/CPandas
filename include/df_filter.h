/*
 * df_filter.h - keep only the rows of a DataFrame that match a condition.
 *
 * Examples:
 *     df_filter_number(df, "age", DF_OP_GT, 30, &out);       age > 30
 *     df_filter_string(df, "city", DF_OP_EQ, "Pune", &out);  city == "Pune"
 *     df_filter_missing(df, "score", 1, &out);               score is missing
 *
 * To combine conditions with AND, filter the result again. For anything
 * else (OR, conditions on two columns, ...) write your own function and use
 * df_filter_custom().
 *
 * MISSING VALUES
 *   A missing cell never matches a comparison, not even DF_OP_NE.
 *   (Use df_filter_missing() to select rows by missing / not missing.)
 *
 * MEMORY OWNERSHIP
 *   - The result is a new DataFrame with the same columns as the input and
 *     only the matching rows. The caller owns *out and frees it with
 *     df_free(). It is a deep copy of the input.
 *   - The input frame is never changed.
 *   - On failure *out is set to NULL and nothing is leaked.
 *
 * Owner: Person B
 */
#ifndef DF_FILTER_H
#define DF_FILTER_H

#include <stddef.h>
#include "df_error.h"
#include "dataframe.h"

typedef enum {
    DF_OP_EQ,   /* ==  */
    DF_OP_NE,   /* !=  */
    DF_OP_LT,   /* <   */
    DF_OP_LE,   /* <=  */
    DF_OP_GT,   /* >   */
    DF_OP_GE    /* >=  */
} df_compare_op;

/* Compares a number column (DTYPE_INT or DTYPE_FLOAT) with `value`.
 * INT cells are converted to double before comparing (exact for integers
 * up to 2^53).
 * Errors: DF_ERR_NOT_FOUND (no such column), DF_ERR_TYPE_MISMATCH (string
 * column), DF_ERR_INVALID_ARG (bad op). */
df_status df_filter_number(const DataFrame *df, const char *column,
                           df_compare_op op, double value, DataFrame **out);

/* Compares a DTYPE_STRING column with `value` using strcmp(), so the order
 * is case-sensitive alphabetical order ("Apple" < "apple" < "banana").
 * `value` is only read, never stored.
 * Errors: DF_ERR_NOT_FOUND, DF_ERR_TYPE_MISMATCH (number column),
 * DF_ERR_INVALID_ARG (bad op). */
df_status df_filter_string(const DataFrame *df, const char *column,
                           df_compare_op op, const char *value,
                           DataFrame **out);

/* keep_missing != 0: keep rows where `column` is missing.
 * keep_missing == 0: keep rows where `column` is NOT missing.
 * Works on any column type. Errors: DF_ERR_NOT_FOUND. */
df_status df_filter_missing(const DataFrame *df, const char *column,
                            int keep_missing, DataFrame **out);

/* Your own condition. It is called once for every row (row = 0, 1, 2, ...)
 * and must return non-zero to keep that row, 0 to drop it. Use
 * df_get_column() and series_get_*() inside it to read the cells.
 * `user_data` is passed through unchanged (it may be NULL). The function
 * must not change or free the frame. */
typedef int (*df_row_condition)(const DataFrame *df, size_t row,
                                void *user_data);

df_status df_filter_custom(const DataFrame *df, df_row_condition keep,
                           void *user_data, DataFrame **out);

#endif /* DF_FILTER_H */
