/*
 * df_stats.c - count, sum, mean, median, min, max, variance, standard
 * deviation, quantiles and number of different values of a Series, and
 * df_describe() for a whole frame.
 *
 * All of them skip missing cells. INT, FLOAT and BOOL cells are all read
 * as a double by the helper get_number(), so each statistic is written
 * once.
 */
#include "df_stats.h"
#include "df_keyindex.h"

#include <math.h>
#include <stdlib.h>

/* ---- helpers ------------------------------------------------------------ */

static int is_number_series(const Series *s)
{
    df_dtype type = series_dtype(s);
    if (type == DTYPE_INT || type == DTYPE_FLOAT || type == DTYPE_BOOL) {
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
    if (series_dtype(s) == DTYPE_BOOL) {
        int b = 0;
        series_get_bool(s, i, &b);
        return (double)b;
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

df_status series_var(const Series *s, double *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!is_number_series(s)) {
        return DF_ERR_TYPE_MISMATCH;
    }

    size_t count = 0;
    series_count(s, &count);
    if (count < 2) {
        return DF_ERR_MISSING_VALUE;    /* needs at least two values */
    }

    /* Two passes: first the mean, then the squared distances from it.
       (This is more accurate than adding up x*x in one pass.) */
    double mean = 0.0;
    series_mean(s, &mean);
    double squares = 0.0;
    size_t n = series_len(s);
    for (size_t i = 0; i < n; i++) {
        if (!series_is_missing(s, i)) {
            double d = get_number(s, i) - mean;
            squares = squares + d * d;
        }
    }
    *out = squares / (double)(count - 1);
    return DF_OK;
}

df_status series_std(const Series *s, double *out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    double var = 0.0;
    df_status st = series_var(s, &var);
    if (st == DF_OK) {
        *out = sqrt(var);
    }
    return st;
}

/* Copies the non-missing values of a number Series into a new array
   (*out_values, freed by the caller) and sorts it. count must be > 0. */
static df_status sorted_values(const Series *s, size_t count,
                               double **out_values)
{
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
    *out_values = values;
    return DF_OK;
}

/* The quantile q of `count` sorted values. It sits at position
   q * (count - 1), usually between two values: take the right amount of
   each. */
static double quantile_of_sorted(const double *values, size_t count, double q)
{
    double position = q * (double)(count - 1);
    size_t below = (size_t)position;
    double fraction = position - (double)below;
    if (below + 1 < count) {
        return values[below] + (values[below + 1] - values[below]) * fraction;
    }
    return values[below];
}

df_status series_quantile(const Series *s, double q, double *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (!is_number_series(s)) {
        return DF_ERR_TYPE_MISMATCH;
    }
    if (!(q >= 0.0 && q <= 1.0)) {      /* also catches q = NaN */
        return DF_ERR_INVALID_ARG;
    }

    size_t count = 0;
    series_count(s, &count);
    if (count == 0) {
        return DF_ERR_MISSING_VALUE;
    }

    double *values = NULL;
    df_status st = sorted_values(s, count, &values);
    if (st != DF_OK) {
        return st;
    }
    *out = quantile_of_sorted(values, count, q);
    free(values);
    return DF_OK;
}

df_status series_nunique(const Series *s, size_t *out)
{
    if (s == NULL || out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    /* Group the cells by their own value: the number of groups is the
       number of different values (missing cells are in no group). */
    const Series *keys[1];
    keys[0] = s;
    KeyIndex index;
    df_status st = key_index_build(&index, keys, 1, series_len(s));
    if (st != DF_OK) {
        return st;
    }
    *out = index.ngroups;
    key_index_free(&index);
    return DF_OK;
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
    case DF_AGG_STD:    return series_std(s, out);
    case DF_AGG_VAR:    return series_var(s, out);
    case DF_AGG_NUNIQUE: {
        size_t count = 0;
        df_status st = series_nunique(s, &count);
        if (st == DF_OK) {
            *out = (double)count;
        }
        return st;
    }
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
    case DF_AGG_STD:    return "std";
    case DF_AGG_VAR:    return "var";
    case DF_AGG_NUNIQUE: return "nunique";
    }
    return "unknown";
}

/* ---- describe ----------------------------------------------------------- */

/* Builds the 8 statistics of one number column. The values are sorted
   only once: min, the three quartiles and max all come from that sorted
   array. With no values, everything after the count is missing; with one
   value, only the std is missing. */
static df_status describe_column(const Series *col, Series **out)
{
    Series *result = NULL;
    df_status st = series_create(series_name(col), DTYPE_FLOAT, 8, &result);
    if (st != DF_OK) {
        return st;
    }

    size_t count = 0;
    series_count(col, &count);
    double stats[8];
    int present[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    stats[0] = (double)count;

    double *values = NULL;
    if (count > 0) {
        st = sorted_values(col, count, &values);
    }
    if (st == DF_OK && count > 0) {
        present[1] = (series_mean(col, &stats[1]) == DF_OK);
        present[2] = (series_std(col, &stats[2]) == DF_OK);
        stats[3] = values[0];
        stats[4] = quantile_of_sorted(values, count, 0.25);
        stats[5] = quantile_of_sorted(values, count, 0.50);
        stats[6] = quantile_of_sorted(values, count, 0.75);
        stats[7] = values[count - 1];
        for (int k = 3; k < 8; k++) {
            present[k] = 1;
        }
    }
    free(values);

    for (int k = 0; k < 8 && st == DF_OK; k++) {
        if (present[k]) {
            st = series_append_float(result, stats[k]);
        } else {
            st = series_append_missing(result);
        }
    }

    if (st != DF_OK) {
        series_free(result);
        return st;
    }
    *out = result;
    return DF_OK;
}

df_status df_describe(const DataFrame *df, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }

    const char *labels[8] = {"count", "mean", "std", "min",
                             "25%", "50%", "75%", "max"};

    DataFrame *result = NULL;
    df_status st = df_create(&result);
    if (st != DF_OK) {
        return st;
    }

    /* First column: the names of the statistics. */
    Series *stat = NULL;
    st = series_create("stat", DTYPE_STRING, 8, &stat);
    for (int k = 0; k < 8 && st == DF_OK; k++) {
        st = series_append_string(stat, labels[k]);
    }
    if (st == DF_OK) {
        st = df_add_column(result, stat);
    }
    if (st != DF_OK) {
        series_free(stat);      /* safe with NULL; not taken by the frame */
        df_free(result);
        return st;
    }

    /* Then one column per number column. */
    size_t ncols = df_ncols(df);
    for (size_t c = 0; c < ncols && st == DF_OK; c++) {
        const Series *col = NULL;
        df_get_column_at(df, c, &col);
        df_dtype type = series_dtype(col);
        if (type != DTYPE_INT && type != DTYPE_FLOAT) {
            continue;
        }
        Series *summary = NULL;
        st = describe_column(col, &summary);
        if (st == DF_OK) {
            st = df_add_column(result, summary);
            if (st != DF_OK) {
                series_free(summary);
            }
        }
    }

    if (st != DF_OK) {
        df_free(result);
        return st;
    }
    *out = result;
    return DF_OK;
}
