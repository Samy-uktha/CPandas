/*
 * df_groupby.c - group rows by one or more key columns and aggregate each
 * group.
 *
 * How it works:
 *   1. Put every row into its group with a hash table (df_keyindex.c):
 *      rows with equal keys get the same group number, in O(n). Rows with
 *      a missing key cell get no group and are left out.
 *   2. The key columns of the result are the first row of every group.
 *      Only these (few) rows are sorted by key, so the groups come out in
 *      key order: O(g log g) for g groups instead of sorting all n rows.
 *   3. For each aggregation, copy each group's cells into a small Series
 *      (series_take) and run the statistic on it (series_aggregate).
 *
 * The first version sorted all the rows by key and cut the sorted list
 * into runs (O(n log n)); the result is exactly the same.
 */
#include "df_groupby.h"
#include "df_keyindex.h"
#include "df_rows.h"
#include "df_sort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

static int valid_func(df_agg_func func)
{
    if (func == DF_AGG_COUNT || func == DF_AGG_SUM || func == DF_AGG_MEAN ||
        func == DF_AGG_MEDIAN || func == DF_AGG_MIN || func == DF_AGG_MAX ||
        func == DF_AGG_STD || func == DF_AGG_VAR || func == DF_AGG_NUNIQUE) {
        return 1;
    }
    return 0;
}

/* count and nunique work on any column; the rest need numbers. */
static int works_on_type(df_agg_func func, df_dtype type)
{
    if (func == DF_AGG_COUNT || func == DF_AGG_NUNIQUE) {
        return 1;
    }
    return type == DTYPE_INT || type == DTYPE_FLOAT || type == DTYPE_BOOL;
}

/* Builds one result column (e.g. "salary_mean") with one value per group,
   for the groups in the order given by `order`, and adds it to `result`. */
static df_status add_aggregate_column(DataFrame *result, const Series *col,
                                      df_agg_func func, const KeyIndex *index,
                                      const size_t *order)
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
    if (func == DF_AGG_COUNT || func == DF_AGG_NUNIQUE) {
        type = DTYPE_INT;
    }

    size_t ngroups = index->ngroups;
    Series *values = NULL;
    df_status st = series_create(name, type, ngroups, &values);
    free(name);     /* series_create made its own copy of the name */
    if (st != DF_OK) {
        return st;
    }

    for (size_t k = 0; k < ngroups; k++) {
        size_t g = order[k];
        size_t start = index->start[g];
        size_t size = index->start[g + 1] - start;

        /* Copy just this group's cells into a temporary Series. */
        Series *group = NULL;
        st = series_take(col, &index->members[start], size, &group);
        if (st != DF_OK) {
            series_free(values);
            return st;
        }

        double result_value = 0.0;
        st = series_aggregate(group, func, &result_value);
        if (st == DF_OK && type == DTYPE_INT) {
            st = series_append_int(values, (int64_t)result_value);
        } else if (st == DF_OK) {
            st = series_append_float(values, result_value);
        } else if (st == DF_ERR_MISSING_VALUE) {
            /* e.g. the mean of a group whose values are all missing */
            st = series_append_missing(values);
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

/* Checks the key names and the aggregations before any work is done, and
   looks up the key columns into keys[]. */
static df_status check_arguments(const DataFrame *df,
                                 const char *const *key_columns, size_t nkeys,
                                 const df_aggregation *aggs, size_t naggs,
                                 const Series **keys)
{
    for (size_t k = 0; k < nkeys; k++) {
        if (key_columns[k] == NULL) {
            return DF_ERR_NULL_ARG;
        }
        df_status st = df_get_column(df, key_columns[k], &keys[k]);
        if (st != DF_OK) {
            return st;
        }
        /* the same key twice would give two result columns of that name */
        for (size_t j = 0; j < k; j++) {
            if (strcmp(key_columns[j], key_columns[k]) == 0) {
                return DF_ERR_DUPLICATE_NAME;
            }
        }
    }

    for (size_t a = 0; a < naggs; a++) {
        if (aggs[a].column == NULL) {
            return DF_ERR_NULL_ARG;
        }
        if (!valid_func(aggs[a].func)) {
            return DF_ERR_INVALID_ARG;
        }
        const Series *col = NULL;
        df_status st = df_get_column(df, aggs[a].column, &col);
        if (st != DF_OK) {
            return st;
        }
        if (!works_on_type(aggs[a].func, series_dtype(col))) {
            return DF_ERR_TYPE_MISMATCH;
        }
    }
    return DF_OK;
}

/* ---- public functions --------------------------------------------------- */

df_status df_groupby_multi(const DataFrame *df,
                           const char *const *key_columns, size_t nkeys,
                           const df_aggregation *aggs, size_t naggs,
                           DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || key_columns == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (naggs > 0 && aggs == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (nkeys == 0) {
        return DF_ERR_INVALID_ARG;
    }

    const Series **keys = malloc(nkeys * sizeof(const Series *));
    if (keys == NULL) {
        return DF_ERR_NOMEM;
    }
    df_status st = check_arguments(df, key_columns, nkeys, aggs, naggs, keys);
    if (st != DF_OK) {
        free(keys);
        return st;
    }

    /* Step 1: put the rows into groups. */
    KeyIndex index;
    st = key_index_build(&index, keys, nkeys, df_nrows(df));
    if (st != DF_OK) {
        free(keys);
        return st;
    }

    /* Step 2: one row per group with just the key columns, then the order
       that sorts those rows by key. */
    DataFrame *group_keys = NULL;
    DataFrame *result = NULL;
    size_t *order = NULL;
    st = df_create(&group_keys);
    for (size_t k = 0; k < nkeys && st == DF_OK; k++) {
        Series *firsts = NULL;
        st = series_take(keys[k], index.first_row, index.ngroups, &firsts);
        if (st == DF_OK) {
            st = df_add_column(group_keys, firsts);
            if (st != DF_OK) {
                series_free(firsts);
            }
        }
    }
    if (st == DF_OK) {
        st = df_sort_indices(group_keys, key_columns, NULL, nkeys, &order);
    }
    if (st == DF_OK) {
        /* order is NULL when there are no groups; take_rows accepts that */
        st = df_take_rows(group_keys, order, index.ngroups, &result);
    }

    /* Step 3: one column per aggregation. */
    for (size_t a = 0; a < naggs && st == DF_OK; a++) {
        const Series *col = NULL;
        df_get_column(df, aggs[a].column, &col);  /* checked above */
        st = add_aggregate_column(result, col, aggs[a].func, &index, order);
    }

    free(order);
    df_free(group_keys);
    key_index_free(&index);
    free(keys);

    if (st != DF_OK) {
        df_free(result);    /* safe even if result is NULL */
        return st;
    }
    *out = result;
    return DF_OK;
}

df_status df_groupby(const DataFrame *df, const char *key_column,
                     const df_aggregation *aggs, size_t naggs,
                     DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (key_column == NULL) {
        return DF_ERR_NULL_ARG;
    }
    const char *key_columns[1];
    key_columns[0] = key_column;
    return df_groupby_multi(df, key_columns, 1, aggs, naggs, out);
}
