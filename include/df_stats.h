/*
 * df_stats.h - basic statistics on one column (Series).
 *
 * To get a column of a DataFrame, use df_get_column():
 *     const Series *col = NULL;
 *     double avg;
 *     if (df_get_column(df, "score", &col) == DF_OK &&
 *         series_mean(col, &avg) == DF_OK) { ... }
 *
 * RULES
 *   - Missing cells are skipped (like pandas' skipna=True).
 *   - series_count() works on every column type. All the others need a
 *     number column (DTYPE_INT or DTYPE_FLOAT) and fail with
 *     DF_ERR_TYPE_MISMATCH on a string column.
 *   - Results are returned as double (INT cells are converted).
 *   - Sum of zero values is 0. Mean, median, min and max of zero values
 *     (empty column, or every cell missing) cannot be computed: they fail
 *     with DF_ERR_MISSING_VALUE and *out is not changed.
 *   - Median: the middle value after sorting; for an even number of values,
 *     the average of the two middle values.
 *
 * MEMORY OWNERSHIP
 *   - Nothing is returned that needs freeing. The Series is only read.
 *   - series_median() uses a temporary array internally and frees it
 *     before returning (it fails with DF_ERR_NOMEM if it cannot get one).
 *
 * Owner: Person B
 */
#ifndef DF_STATS_H
#define DF_STATS_H

#include <stddef.h>
#include "df_error.h"
#include "series.h"

/* The aggregation functions, used by series_aggregate() and by group-by. */
typedef enum {
    DF_AGG_COUNT,
    DF_AGG_SUM,
    DF_AGG_MEAN,
    DF_AGG_MEDIAN,
    DF_AGG_MIN,
    DF_AGG_MAX
} df_agg_func;

df_status series_count(const Series *s, size_t *out);  /* non-missing cells */
df_status series_sum(const Series *s, double *out);
df_status series_mean(const Series *s, double *out);
df_status series_median(const Series *s, double *out);
df_status series_min(const Series *s, double *out);
df_status series_max(const Series *s, double *out);

/* Runs the function chosen by `func` (the count is returned as a double).
 * Fails with DF_ERR_INVALID_ARG for an unknown func. */
df_status series_aggregate(const Series *s, df_agg_func func, double *out);

/* Short lowercase name of the function: "count", "sum", "mean", "median",
 * "min", "max", or "unknown". Static string, do not free. */
const char *df_agg_name(df_agg_func func);

#endif /* DF_STATS_H */
