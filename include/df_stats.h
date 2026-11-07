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
 *   - series_count() and series_nunique() work on every column type. All
 *     the others need a number column (DTYPE_INT, DTYPE_FLOAT, or
 *     DTYPE_BOOL where False = 0 and True = 1, so the mean of a bool column
 *     is the share of True) and fail with DF_ERR_TYPE_MISMATCH on a string
 *     or date column.
 *   - Results are returned as double (INT cells are converted).
 *   - Sum of zero values is 0. Mean, median, min and max of zero values
 *     (empty column, or every cell missing) cannot be computed: they fail
 *     with DF_ERR_MISSING_VALUE and *out is not changed.
 *   - Median: the middle value after sorting; for an even number of values,
 *     the average of the two middle values.
 *   - Variance and standard deviation are the SAMPLE versions (divide by
 *     count - 1), like pandas' default ddof=1. With fewer than 2 values they
 *     fail with DF_ERR_MISSING_VALUE (pandas gives NaN).
 *   - Quantile q (0 <= q <= 1) interpolates linearly between the two
 *     nearest sorted values, like pandas: q = 0.5 is the median, 0.25 the
 *     first quartile. q outside [0, 1] fails with DF_ERR_INVALID_ARG.
 *
 * MEMORY OWNERSHIP
 *   - Nothing is returned that needs freeing. The Series is only read.
 *   - series_median(), series_quantile() and series_nunique() use temporary
 *     memory internally and free it before returning (they fail with
 *     DF_ERR_NOMEM if they cannot get it).
 *   - df_describe(): the caller owns *out and frees it with df_free().
 *
 * Owner: Person B
 */
#ifndef DF_STATS_H
#define DF_STATS_H

#include <stddef.h>
#include "df_error.h"
#include "series.h"
#include "dataframe.h"

/* The aggregation functions, used by series_aggregate() and by group-by. */
typedef enum {
    DF_AGG_COUNT,
    DF_AGG_SUM,
    DF_AGG_MEAN,
    DF_AGG_MEDIAN,
    DF_AGG_MIN,
    DF_AGG_MAX,
    DF_AGG_STD,       /* sample standard deviation              */
    DF_AGG_VAR,       /* sample variance                        */
    DF_AGG_NUNIQUE    /* number of different non-missing values */
} df_agg_func;

df_status series_count(const Series *s, size_t *out);  /* non-missing cells */
df_status series_sum(const Series *s, double *out);
df_status series_mean(const Series *s, double *out);
df_status series_median(const Series *s, double *out);
df_status series_min(const Series *s, double *out);
df_status series_max(const Series *s, double *out);
df_status series_var(const Series *s, double *out);
df_status series_std(const Series *s, double *out);
df_status series_quantile(const Series *s, double q, double *out);

/* How many different values the column has (missing cells not counted).
 * Any column type. Uses a hash table, so it is O(n). */
df_status series_nunique(const Series *s, size_t *out);

/* Runs the function chosen by `func` (the count is returned as a double).
 * Fails with DF_ERR_INVALID_ARG for an unknown func. */
df_status series_aggregate(const Series *s, df_agg_func func, double *out);

/* Short lowercase name of the function: "count", "sum", "mean", "median",
 * "min", "max", "std", "var", "nunique", or "unknown". Static string, do not
 * free. */
const char *df_agg_name(df_agg_func func);

/* Summary of every number column, like pandas' df.describe().
 * The result has a first column "stat" with the 8 row labels
 *     count, mean, std, min, 25%, 50%, 75%, max
 * and then one DTYPE_FLOAT column per DTYPE_INT / DTYPE_FLOAT column of df
 * (same name, same order). Cells that cannot be computed (for example the
 * std of a single value) are missing. Other column types are left out; a
 * frame without number columns gives just the "stat" column.
 * Errors: DF_ERR_DUPLICATE_NAME if a number column is itself called
 * "stat". */
df_status df_describe(const DataFrame *df, DataFrame **out);

#endif /* DF_STATS_H */
