/*
 * df_stats.c - count, sum, mean, median, min and max of a Series.
 *
 * All of them skip missing cells. INT and FLOAT cells are both read as a
 * double by the helper get_number(), so each statistic is written once.
 */
#include "df_stats.h"

#include <stdlib.h>

/* ---- helpers ------------------------------------------------------------ */

static int is_number_series(const Series *s)
{
    df_dtype type = series_dtype(s);
    if (type == DTYPE_INT || type == DTYPE_FLOAT) {
        return 1;
    }
    return 0;
}

/* Reads cell i of a number Series as a double.
   The caller must check first that the cell is not missing. */
static double get_number(const Series *s, size_t i)
{
    if (series_dtype(s) == DTYPE_INT) {
        int64_t v = 0;
        series_get_int(s, i, &v);
        return (double)v;
    }
    double v = 0.0;
    series_get_float(s, i, &v);
    return v;
}

/* Compare function for qsort() on an array of doubles (small to large). */
static int compare_double_values(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* ---- public functions --------------------------------------------------- */

df_status series_count(const Series *s, size_t *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    /* count = all cells minus the missing ones */
    *out = series_len(s) - series_count_missing(s);
    return DF_OK;
}

df_status series_sum(const Series *s, double *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!is_number_series(s)) {
        return DF_ERR_TYPE_MISMATCH;
    }

    double total = 0.0;
    size_t n = series_len(s);
    for (size_t i = 0; i < n; i++) {
        if (!series_is_missing(s, i)) {
            total = total + get_number(s, i);
        }
    }
    *out = total;
    return DF_OK;
}

df_status series_mean(const Series *s, double *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!is_number_series(s)) {
        return DF_ERR_TYPE_MISMATCH;
    }

    size_t count = 0;
    series_count(s, &count);
    if (count == 0) {
        return DF_ERR_MISSING_VALUE;    /* mean of nothing is not defined */
    }

    double total = 0.0;
    series_sum(s, &total);
    *out = total / (double)count;
    return DF_OK;
}

df_status series_median(const Series *s, double *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!is_number_series(s)) {
        return DF_ERR_TYPE_MISMATCH;
    }

    size_t count = 0;
    series_count(s, &count);
    if (count == 0) {
        return DF_ERR_MISSING_VALUE;
    }

    /* Copy the non-missing values into a temporary array and sort it. */
    double *values = malloc(count * sizeof(double));
    if (values == NULL) {
        return DF_ERR_NOMEM;
    }
    size_t k = 0;
    size_t n = series_len(s);
    for (size_t i = 0; i < n; i++) {
        if (!series_is_missing(s, i)) {
            values[k] = get_number(s, i);
            k++;
        }
    }
    qsort(values, count, sizeof(double), compare_double_values);

    if (count % 2 == 1) {
        /* odd count: the middle element, e.g. index 2 of 5 */
        *out = values[count / 2];
    } else {
        /* even count: average of the two middle elements, e.g. 1 and 2 of 4 */
        *out = (values[count / 2 - 1] + values[count / 2]) / 2.0;
    }

    free(values);
    return DF_OK;
}

/* min and max are almost the same code, so both use this helper.
   want_max = 0 finds the smallest value, want_max = 1 the largest. */
static df_status find_min_or_max(const Series *s, int want_max, double *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!is_number_series(s)) {
        return DF_ERR_TYPE_MISMATCH;
    }

    int found = 0;      /* have we seen a non-missing value yet? */
    double best = 0.0;
    size_t n = series_len(s);
    for (size_t i = 0; i < n; i++) {
        if (series_is_missing(s, i)) {
            continue;
        }
        double v = get_number(s, i);
        if (!found) {
            best = v;   /* the first value is the best so far */
            found = 1;
        } else if (want_max && v > best) {
            best = v;
        } else if (!want_max && v < best) {
            best = v;
        }
    }

    if (!found) {
        return DF_ERR_MISSING_VALUE;    /* no values at all */
    }
    *out = best;
    return DF_OK;
}

df_status series_min(const Series *s, double *out)
{
    return find_min_or_max(s, 0, out);
}

df_status series_max(const Series *s, double *out)
{
    return find_min_or_max(s, 1, out);
}

df_status series_aggregate(const Series *s, df_agg_func func, double *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }

    switch (func) {
    case DF_AGG_COUNT: {
        size_t count = 0;
        df_status st = series_count(s, &count);
        if (st == DF_OK) {
            *out = (double)count;
        }
        return st;
    }
    case DF_AGG_SUM:    return series_sum(s, out);
    case DF_AGG_MEAN:   return series_mean(s, out);
    case DF_AGG_MEDIAN: return series_median(s, out);
    case DF_AGG_MIN:    return series_min(s, out);
    case DF_AGG_MAX:    return series_max(s, out);
    }
    return DF_ERR_INVALID_ARG;
}

const char *df_agg_name(df_agg_func func)
{
    switch (func) {
    case DF_AGG_COUNT:  return "count";
    case DF_AGG_SUM:    return "sum";
    case DF_AGG_MEAN:   return "mean";
    case DF_AGG_MEDIAN: return "median";
    case DF_AGG_MIN:    return "min";
    case DF_AGG_MAX:    return "max";
    }
    return "unknown";
}
