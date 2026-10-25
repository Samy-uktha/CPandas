/*
 * df_join.h - join (merge) two DataFrames on a key column.
 *
 * Example (like pandas pd.merge(left, right, on="id", how="left")):
 *     df_merge(left, right, "id", DF_JOIN_LEFT, &out);
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
 *   - All left columns in their order (the key column included), then all
 *     right columns except the key.
 *   - If a non-key column name is used in both frames, it gets "_left" on
 *     the left one and "_right" on the right one (e.g. "name_left",
 *     "name_right").
 *   - For right/outer joins, the key cell of a row that exists only in the
 *     right frame is filled in from the right frame.
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
 *   - Both frames must have a column called `on`, and the two key columns
 *     must have the same type (DF_ERR_TYPE_MISMATCH otherwise).
 *   - A missing key never matches anything (like SQL NULL).
 *   - Algorithm: nested loops (every left row is compared with every right
 *     row), O(n * m). Simple and fine for small and medium tables.
 *
 * ERRORS
 *   DF_ERR_NOT_FOUND (no `on` column in one of the frames),
 *   DF_ERR_TYPE_MISMATCH, DF_ERR_INVALID_ARG (unknown join type),
 *   DF_ERR_DUPLICATE_NAME (a renamed column clashes with an existing name,
 *   e.g. the left frame has both "name" and "name_left").
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

#include "df_error.h"
#include "dataframe.h"

typedef enum {
    DF_JOIN_INNER,
    DF_JOIN_LEFT,
    DF_JOIN_RIGHT,
    DF_JOIN_OUTER
} df_join_type;

df_status df_merge(const DataFrame *left, const DataFrame *right,
                   const char *on, df_join_type how, DataFrame **out);

#endif /* DF_JOIN_H */
