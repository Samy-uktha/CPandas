/*
 * df_groupby.h - group rows by the values of one or more columns and
 * aggregate.
 *
 * Example (like pandas df.groupby("dept").agg(...)):
 *     df_aggregation aggs[] = {
 *         { "salary", DF_AGG_MEAN },
 *         { "name",   DF_AGG_COUNT }
 *     };
 *     df_groupby(df, "dept", aggs, 2, &out);
 *
 * gives a frame with the columns  dept | salary_mean | name_count
 * and one row per different value of "dept".
 *
 * Several key columns (like df.groupby(["dept", "city"])):
 *     const char *keys[] = { "dept", "city" };
 *     df_groupby_multi(df, keys, 2, aggs, 2, &out);
 * gives one row per different (dept, city) pair.
 *
 * RULES
 *   - The first result columns are the key columns (same names and types,
 *     in the order given).
 *   - Then one column per aggregation, named "<column>_<function>", for
 *     example "salary_mean". (See df_agg_name() in df_stats.h.)
 *   - count and nunique columns are DTYPE_INT, all other results are
 *     DTYPE_FLOAT.
 *   - The groups (rows of the result) are sorted by key, ascending (by the
 *     first key, then the second, ...).
 *   - Rows with a missing cell in any key column are left out (like
 *     pandas' dropna=True).
 *   - Inside each group, missing values are skipped, exactly as in
 *     df_stats.h. If a group has no values to compute mean / median / min /
 *     max from (or fewer than 2 for std / var), that result cell is
 *     missing.
 *   - naggs may be 0 (aggs may then be NULL): the result is just the list
 *     of different keys.
 *   - Algorithm: a hash table puts every row into its group in O(n), then
 *     only the g group keys are sorted, O(g log g). See df_keyindex.c.
 *
 * ERRORS
 *   DF_ERR_NOT_FOUND (unknown key or aggregation column),
 *   DF_ERR_TYPE_MISMATCH (sum/mean/median/min/max/std/var of a string or
 *   date column),
 *   DF_ERR_INVALID_ARG (unknown aggregation function, or nkeys == 0),
 *   DF_ERR_DUPLICATE_NAME (two result columns would get the same name, e.g.
 *   the same aggregation asked twice, or the same key column twice).
 *
 * MEMORY OWNERSHIP
 *   - The caller owns *out and frees it with df_free().
 *   - The aggs and key_columns arrays and their strings are only read
 *     during the call; the caller keeps ownership of them.
 *   - The input frame is never changed.
 *   - On failure *out is set to NULL and nothing is leaked.
 *
 * Owner: Person B
 */
#ifndef DF_GROUPBY_H
#define DF_GROUPBY_H

#include <stddef.h>
#include "df_error.h"
#include "dataframe.h"
#include "df_stats.h"

typedef struct {
    const char  *column;  /* the column to aggregate */
    df_agg_func  func;    /* what to compute for it  */
} df_aggregation;

/* Groups by one key column. */
df_status df_groupby(const DataFrame *df, const char *key_column,
                     const df_aggregation *aggs, size_t naggs,
                     DataFrame **out);

/* Groups by key_columns[0 .. nkeys-1] together. */
df_status df_groupby_multi(const DataFrame *df,
                           const char *const *key_columns, size_t nkeys,
                           const df_aggregation *aggs, size_t naggs,
                           DataFrame **out);

#endif /* DF_GROUPBY_H */
