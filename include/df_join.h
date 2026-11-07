/*
 * df_join.h - join (merge) two DataFrames on one or more key columns.
 *
 * Example (like pandas pd.merge(left, right, on="id", how="left")):
 *     df_merge(left, right, "id", DF_JOIN_LEFT, &out);
 *
 * Several key columns (like pd.merge(..., on=["dept", "year"])): rows
 * match when ALL their key cells are equal.
 *     const char *keys[] = { "dept", "year" };
 *     df_merge_on(left, right, keys, 2, DF_JOIN_INNER, &out);
 *
 * JOIN TYPES
 *   DF_JOIN_INNER  only rows whose key is in both frames.
 *   DF_JOIN_LEFT   every left row; right columns are missing where there is
 *                  no match.
 *   DF_JOIN_RIGHT  every right row; left columns are missing where there is
 *                  no match.
 *   DF_JOIN_OUTER  every row of both frames.
 *   If a key appears several times, every matching pair of rows is output
 *   (e.g. 2 matching left rows x 3 matching right rows = 6 result rows).
 *
 * RESULT COLUMNS
 *   - All left columns in their order (the key columns included), then all
 *     right columns except the keys.
 *   - If a non-key column name is used in both frames, it gets "_left" on
 *     the left one and "_right" on the right one (e.g. "name_left",
 *     "name_right").
 *   - For right/outer joins, the key cells of a row that exists only in the
 *     right frame are filled in from the right frame.
 *
 * RESULT ROW ORDER
 *   - INNER / LEFT: left row order; for each left row, its matches in right
 *     row order.
 *   - RIGHT: right row order; for each right row, its matches in left row
 *     order.
 *   - OUTER: same as LEFT, then the right rows that matched nothing, in
 *     right row order.
 *
 * RULES
 *   - Both frames must have every key column, and each key column must have
 *     the same type in both frames (DF_ERR_TYPE_MISMATCH otherwise).
 *   - A row with a missing key cell never matches anything (like SQL NULL).
 *   - Algorithm: hash join. One frame's keys go into a hash table, then each
 *     row of the other frame finds its partners with one lookup:
 *     O(n + m + result rows) instead of comparing every pair, O(n * m).
 *
 * ERRORS
 *   DF_ERR_NOT_FOUND (a key column is missing in one of the frames),
 *   DF_ERR_TYPE_MISMATCH, DF_ERR_INVALID_ARG (unknown join type, or
 *   non == 0), DF_ERR_DUPLICATE_NAME (the same key given twice, or
 *   a renamed column clashes with an existing name, e.g. the left frame has
 *   both "name" and "name_left").
 *
 * MEMORY OWNERSHIP
 *   - The caller owns *out and frees it with df_free(). It is a deep copy:
 *     nothing is shared with either input frame.
 *   - Both input frames are never changed. left and right may be the same
 *     frame (a self-join).
 *   - On failure *out is set to NULL and nothing is leaked.
 *
 * Owner: Person B
 */
#ifndef DF_JOIN_H
#define DF_JOIN_H

#include <stddef.h>
#include "df_error.h"
#include "dataframe.h"

typedef enum {
    DF_JOIN_INNER,
    DF_JOIN_LEFT,
    DF_JOIN_RIGHT,
    DF_JOIN_OUTER
} df_join_type;

/* Joins on one key column. */
df_status df_merge(const DataFrame *left, const DataFrame *right,
                   const char *on, df_join_type how, DataFrame **out);

/* Joins on the key columns on[0 .. non-1] together. */
df_status df_merge_on(const DataFrame *left, const DataFrame *right,
                      const char *const *on, size_t non, df_join_type how,
                      DataFrame **out);

#endif /* DF_JOIN_H */
