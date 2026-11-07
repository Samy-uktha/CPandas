/*
 * df_groupby.h - group rows by the value of one column and aggregate.
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
 * RULES
 *   - The first result column is the key column (same name and type).
 *   - Then one column per aggregation, named "<column>_<function>", for
 *     example "salary_mean". (See df_agg_name() in df_stats.h.)
 *   - Count columns are DTYPE_INT, all other results are DTYPE_FLOAT.
 *   - The groups (rows of the result) are sorted by key, ascending.
 *   - Rows whose key is missing are left out (like pandas' dropna=True).
 *   - Inside each group, missing values are skipped, exactly as in
 *     df_stats.h. If a group has no values to compute mean / median / min /
 *     max from, that result cell is missing.
 *   - naggs may be 0 (aggs may then be NULL): the result is just the list
 *     of different keys.
 *   - Algorithm: sort the row numbers by key (merge sort), then rows with
 *     the same key are next to each other, so each group is one run of
 *     the sorted array. O(n log n).
 *
 * ERRORS
 *   DF_ERR_NOT_FOUND (unknown key or aggregation column),
 *   DF_ERR_TYPE_MISMATCH (sum/mean/median/min/max of a string column),
 *   DF_ERR_INVALID_ARG (unknown aggregation function),
 *   DF_ERR_DUPLICATE_NAME (two result columns would get the same name, e.g.
 *   the same aggregation asked twice).
 *
 * MEMORY OWNERSHIP
 *   - The caller owns *out and frees it with df_free().
 *   - The aggs array and its strings are only read during the call; the
 *     caller keeps ownership of them.
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

df_status df_groupby(const DataFrame *df, const char *key_column,
                     const df_aggregation *aggs, size_t naggs,
                     DataFrame **out);

#endif /* DF_GROUPBY_H */
