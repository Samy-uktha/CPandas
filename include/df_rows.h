/*
 * df_rows.h - row selection: pick rows of a DataFrame by position.
 *
 * Every function here returns a NEW DataFrame that has the same columns
 * (same names, same types, same order) as the input, but only some rows.
 *
 * MEMORY OWNERSHIP
 *   - The new frame is allocated by the function. The caller owns *out and
 *     must free it with df_free().
 *   - The new frame is a deep copy: it shares no memory with the input, so
 *     the input can be changed or freed afterwards.
 *   - The input frame is never changed.
 *   - On failure *out is set to NULL and nothing is leaked.
 *
 * Owner: Person B
 */
#ifndef DF_ROWS_H
#define DF_ROWS_H

#include <stddef.h>
#include "df_error.h"
#include "dataframe.h"

/* First n rows. If n is bigger than the number of rows, all rows are
 * returned. n = 0 gives a frame with the same columns but 0 rows. */
df_status df_head(const DataFrame *df, size_t n, DataFrame **out);

/* Last n rows, in their original order. Same rules as df_head(). */
df_status df_tail(const DataFrame *df, size_t n, DataFrame **out);

/* Rows start, start+1, ..., end-1 (end is NOT included, like Python's
 * df.iloc[start:end]). If end is past the last row it is moved back to the
 * last row. If start >= end the result has 0 rows. Never fails because of
 * the range itself. */
df_status df_slice(const DataFrame *df, size_t start, size_t end,
                   DataFrame **out);

/* The rows listed in rows[0..n-1], in that order. A row number may appear
 * more than once. Every row number must be < df_nrows(df), otherwise the
 * call fails with DF_ERR_INDEX_OUT_OF_RANGE. rows may be NULL when n is 0.
 * Filter, sort and group-by are all built on top of this function. */
df_status df_take_rows(const DataFrame *df, const size_t *rows, size_t n,
                       DataFrame **out);

#endif /* DF_ROWS_H */
