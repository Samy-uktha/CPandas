/*
 * test_helpers.h - small helpers shared by the Person B test files
 * (test_rows.c, test_filter.c, test_sort.c, test_stats.c, test_groupby.c,
 * test_join.c).
 *
 * The helper functions are "static inline" so that a test file that does
 * not use one of them does not get an "unused function" warning.
 *
 * To make test tables quickly, missing cells are written as:
 *   MISSING_NUM (-999) in int / double arrays, and NULL in string arrays.
 */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include <string.h>

#include "series.h"
#include "dataframe.h"

static int failures = 0;
static int checks = 0;

/* Same CHECK macro as tests/test_series.c */
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

#define MISSING_NUM -999

/* ---- building columns --------------------------------------------------- */

static inline Series *make_int_column(const char *name, const int64_t *values,
                                      size_t n)
{
    Series *s = NULL;
    CHECK(series_create(name, DTYPE_INT, n, &s) == DF_OK);
    for (size_t i = 0; i < n; i++) {
        if (values[i] == MISSING_NUM) {
            series_append_missing(s);
        } else {
            series_append_int(s, values[i]);
        }
    }
    return s;
}

static inline Series *make_float_column(const char *name, const double *values,
                                        size_t n)
{
    Series *s = NULL;
    CHECK(series_create(name, DTYPE_FLOAT, n, &s) == DF_OK);
    for (size_t i = 0; i < n; i++) {
        if (values[i] == MISSING_NUM) {
            series_append_missing(s);
        } else {
            series_append_float(s, values[i]);
        }
    }
    return s;
}

static inline Series *make_string_column(const char *name,
                                         const char *const *values, size_t n)
{
    Series *s = NULL;
    CHECK(series_create(name, DTYPE_STRING, n, &s) == DF_OK);
    for (size_t i = 0; i < n; i++) {
        if (values[i] == NULL) {
            series_append_missing(s);
        } else {
            series_append_string(s, values[i]);
        }
    }
    return s;
}

/* Adds a column to a frame (the frame takes ownership of it). */
static inline void add_column(DataFrame *df, Series *s)
{
    df_status st = df_add_column(df, s);
    CHECK(st == DF_OK);
    if (st != DF_OK) {
        series_free(s);
    }
}

static inline DataFrame *new_frame(void)
{
    DataFrame *df = NULL;
    CHECK(df_create(&df) == DF_OK);
    return df;
}

/* ---- reading cells ------------------------------------------------------ */

/* Returns MISSING_NUM if the cell is missing or cannot be read. */
static inline int64_t cell_int(const DataFrame *df, const char *col, size_t row)
{
    const Series *s = NULL;
    int64_t v = MISSING_NUM;
    if (df_get_column(df, col, &s) == DF_OK) {
        series_get_int(s, row, &v);
    }
    return v;
}

static inline double cell_float(const DataFrame *df, const char *col, size_t row)
{
    const Series *s = NULL;
    double v = MISSING_NUM;
    if (df_get_column(df, col, &s) == DF_OK) {
        series_get_float(s, row, &v);
    }
    return v;
}

/* Returns NULL if the cell is missing or cannot be read. */
static inline const char *cell_string(const DataFrame *df, const char *col,
                                      size_t row)
{
    const Series *s = NULL;
    const char *v = NULL;
    if (df_get_column(df, col, &s) == DF_OK) {
        series_get_string(s, row, &v);
    }
    return v;
}

/* 1 if the cell exists and is missing. */
static inline int cell_missing(const DataFrame *df, const char *col, size_t row)
{
    const Series *s = NULL;
    if (df_get_column(df, col, &s) != DF_OK) {
        return 0;
    }
    return series_is_missing(s, row);
}

/* strcmp that also accepts NULL (two NULLs are equal). */
static inline int same_string(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

/* 1 if both numbers are equal up to a tiny rounding error. */
static inline int close_to(double a, double b)
{
    double diff = a - b;
    if (diff < 0) {
        diff = -diff;
    }
    return diff < 1e-9;
}

/* Prints the summary line and gives the exit code for main(). */
static inline int finish_tests(void)
{
    printf("%d checks, %d failures\n", checks, failures);
    if (failures > 0) {
        return 1;
    }
    return 0;
}

#endif /* TEST_HELPERS_H */
