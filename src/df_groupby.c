/*
 * df_groupby.c - group rows by a key column and aggregate each group.
 *
 * How it works:
 *   1. Sort the row numbers by the key column (df_sort_indices). Now all
 *      rows with the same key are next to each other, and the rows with a
 *      missing key are at the very end (we ignore those).
 *   2. Walk through the sorted rows and remember where each new key starts.
 *      Group g is then sorted[group_start[g]] .. sorted[group_start[g+1]-1].
 *   3. The key column of the result is the first row of every group.
 *   4. For each aggregation, copy each group's cells into a small Series
 *      (series_take) and run the statistic on it (series_aggregate).
 */
#include "df_groupby.h"
#include "df_sort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

static int valid_func(df_agg_func func)
{
    if (func == DF_AGG_COUNT || func == DF_AGG_SUM || func == DF_AGG_MEAN ||
        func == DF_AGG_MEDIAN || func == DF_AGG_MIN || func == DF_AGG_MAX) {
        return 1;
    }
    return 0;
}

/* Builds one result column (e.g. "salary_mean") with one value per group
   and adds it to `result`. */
static df_status add_aggregate_column(DataFrame *result, const Series *col,
                                      df_agg_func func, const size_t *sorted,
                                      const size_t *group_start,
                                      size_t ngroups)
{
    /* Make the name "<column>_<function>". The +2 is for '_' and '\0'. */
    const char *col_name = series_name(col);
    const char *func_name = df_agg_name(func);
    size_t name_len = strlen(col_name) + strlen(func_name) + 2;
    char *name = malloc(name_len);
    if (name == NULL) {
        return DF_ERR_NOMEM;
    }
    snprintf(name, name_len, "%s_%s", col_name, func_name);

    /* Counts are whole numbers, everything else is a double. */
    df_dtype type = DTYPE_FLOAT;
    if (func == DF_AGG_COUNT) {
        type = DTYPE_INT;
    }

    Series *values = NULL;
    df_status st = series_create(name, type, ngroups, &values);
    free(name);     /* series_create made its own copy of the name */
    if (st != DF_OK) {
        return st;
    }

    for (size_t g = 0; g < ngroups; g++) {
        size_t start = group_start[g];
        size_t size = group_start[g + 1] - start;

        /* Copy just this group's cells into a temporary Series. */
        Series *group = NULL;
        st = series_take(col, &sorted[start], size, &group);
        if (st != DF_OK) {
            series_free(values);
            return st;
        }

        if (func == DF_AGG_COUNT) {
            size_t count = 0;
            st = series_count(group, &count);
            if (st == DF_OK) {
                st = series_append_int(values, (int64_t)count);
            }
        } else {
            double result_value = 0.0;
            st = series_aggregate(group, func, &result_value);
            if (st == DF_OK) {
                st = series_append_float(values, result_value);
            } else if (st == DF_ERR_MISSING_VALUE) {
                /* the group had no values (all missing): result is missing */
                st = series_append_missing(values);
            }
        }
        series_free(group);

        if (st != DF_OK) {
            series_free(values);
            return st;
        }
    }

    st = df_add_column(result, values);
    if (st != DF_OK) {
        series_free(values);    /* not taken by the frame, so we free it */
    }
    return st;
}

/* ---- public function ---------------------------------------------------- */

df_status df_groupby(const DataFrame *df, const char *key_column,
                     const df_aggregation *aggs, size_t naggs,
                     DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || key_column == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (naggs > 0 && aggs == NULL) {
        return DF_ERR_NULL_ARG;
    }

    const Series *key = NULL;
    df_status st = df_get_column(df, key_column, &key);
    if (st != DF_OK) {
        return st;
    }

    /* Check every aggregation before doing any work. */
    for (size_t a = 0; a < naggs; a++) {
        if (aggs[a].column == NULL) {
            return DF_ERR_NULL_ARG;
        }
        if (!valid_func(aggs[a].func)) {
            return DF_ERR_INVALID_ARG;
        }
        const Series *col = NULL;
        st = df_get_column(df, aggs[a].column, &col);
        if (st != DF_OK) {
            return st;
        }
        /* Only count makes sense for text. */
        if (aggs[a].func != DF_AGG_COUNT && series_dtype(col) == DTYPE_STRING) {
            return DF_ERR_TYPE_MISMATCH;
        }
    }

    /* Step 1: sort the row numbers by key. */
    const char *sort_columns[1];
    sort_columns[0] = key_column;
    size_t *sorted = NULL;
    st = df_sort_indices(df, sort_columns, NULL, 1, &sorted);
    if (st != DF_OK) {
        return st;
    }

    /* Missing keys were sorted to the end, so only the first `nvalid`
       sorted rows have a key. */
    size_t nvalid = df_nrows(df) - series_count_missing(key);

    /* Step 2: find where each group starts. One extra slot at the end
       holds nvalid, so group g always ends at group_start[g + 1]. */
    size_t *group_start = malloc((nvalid + 1) * sizeof(size_t));
    if (group_start == NULL) {
        free(sorted);
        return DF_ERR_NOMEM;
    }
    size_t ngroups = 0;
    for (size_t i = 0; i < nvalid; i++) {
        if (i == 0 ||
            series_compare_cells(key, sorted[i - 1], key, sorted[i]) != 0) {
            group_start[ngroups] = i;   /* a new key starts here */
            ngroups++;
        }
    }
    group_start[ngroups] = nvalid;

    /* Step 3: the key column = the first row of every group. */
    size_t *first_rows = NULL;
    if (ngroups > 0) {
        first_rows = malloc(ngroups * sizeof(size_t));
        if (first_rows == NULL) {
            free(group_start);
            free(sorted);
            return DF_ERR_NOMEM;
        }
        for (size_t g = 0; g < ngroups; g++) {
            first_rows[g] = sorted[group_start[g]];
        }
    }

    DataFrame *result = NULL;
    st = df_create(&result);
    if (st == DF_OK) {
        Series *key_values = NULL;
        st = series_take(key, first_rows, ngroups, &key_values);
        if (st == DF_OK) {
            st = df_add_column(result, key_values);
            if (st != DF_OK) {
                series_free(key_values);
            }
        }
    }

    /* Step 4: one column per aggregation. */
    for (size_t a = 0; a < naggs && st == DF_OK; a++) {
        const Series *col = NULL;
        df_get_column(df, aggs[a].column, &col);  /* checked above */
        st = add_aggregate_column(result, col, aggs[a].func, sorted,
                                  group_start, ngroups);
    }

    free(first_rows);
    free(group_start);
    free(sorted);

    if (st != DF_OK) {
        df_free(result);    /* safe even if result is NULL */
        return st;
    }
    *out = result;
    return DF_OK;
}
