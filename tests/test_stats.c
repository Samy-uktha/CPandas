/* Tests for df_stats.c. Build and run with `make test`. */
#include <stdlib.h>

#include "df_stats.h"
#include "test_helpers.h"

static void test_int_series(void)
{
    int64_t values[] = {4, 1, MISSING_NUM, 3, 2};
    Series *s = make_int_column("v", values, 5);
    size_t count = 0;
    double d = 0.0;

    CHECK(series_count(s, &count) == DF_OK && count == 4);
    CHECK(series_sum(s, &d) == DF_OK && close_to(d, 10.0));
    CHECK(series_mean(s, &d) == DF_OK && close_to(d, 2.5));
    CHECK(series_median(s, &d) == DF_OK && close_to(d, 2.5));  /* (2+3)/2 */
    CHECK(series_min(s, &d) == DF_OK && close_to(d, 1.0));
    CHECK(series_max(s, &d) == DF_OK && close_to(d, 4.0));

    CHECK(series_len(s) == 5 && series_count_missing(s) == 1);  /* unchanged */
    series_free(s);
}

static void test_float_series(void)
{
    double values[] = {2.5, -1.0, 10.0, MISSING_NUM, 0.5, 3.0};
    Series *s = make_float_column("f", values, 6);
    double d = 0.0;

    CHECK(series_sum(s, &d) == DF_OK && close_to(d, 15.0));
    CHECK(series_mean(s, &d) == DF_OK && close_to(d, 3.0));
    CHECK(series_median(s, &d) == DF_OK && close_to(d, 2.5));  /* odd count */
    CHECK(series_min(s, &d) == DF_OK && close_to(d, -1.0));
    CHECK(series_max(s, &d) == DF_OK && close_to(d, 10.0));
    series_free(s);
}

static void test_single_and_negative(void)
{
    int64_t one[] = {-7};
    Series *s = make_int_column("one", one, 1);
    double d = 0.0;
    CHECK(series_mean(s, &d) == DF_OK && close_to(d, -7.0));
    CHECK(series_median(s, &d) == DF_OK && close_to(d, -7.0));
    CHECK(series_min(s, &d) == DF_OK && close_to(d, -7.0));
    CHECK(series_max(s, &d) == DF_OK && close_to(d, -7.0));
    series_free(s);

    int64_t neg[] = {-5, -2, -9};
    s = make_int_column("neg", neg, 3);
    CHECK(series_max(s, &d) == DF_OK && close_to(d, -2.0));
    CHECK(series_min(s, &d) == DF_OK && close_to(d, -9.0));
    series_free(s);
}

static void test_empty_and_all_missing(void)
{
    Series *empty = make_int_column("e", NULL, 0);
    int64_t gone[] = {MISSING_NUM, MISSING_NUM};
    Series *all_missing = make_int_column("m", gone, 2);
    Series *cases[2];
    cases[0] = empty;
    cases[1] = all_missing;

    for (int k = 0; k < 2; k++) {
        Series *s = cases[k];
        size_t count = 99;
        double d = 123.0;
        CHECK(series_count(s, &count) == DF_OK && count == 0);
        CHECK(series_sum(s, &d) == DF_OK && close_to(d, 0.0));
        d = 123.0;
        CHECK(series_mean(s, &d) == DF_ERR_MISSING_VALUE);
        CHECK(series_median(s, &d) == DF_ERR_MISSING_VALUE);
        CHECK(series_min(s, &d) == DF_ERR_MISSING_VALUE);
        CHECK(series_max(s, &d) == DF_ERR_MISSING_VALUE);
        CHECK(close_to(d, 123.0));  /* *out not changed on failure */
    }
    series_free(empty);
    series_free(all_missing);
}

static void test_string_series(void)
{
    const char *words[] = {"a", NULL, "c"};
    Series *s = make_string_column("w", words, 3);
    size_t count = 0;
    double d = 0.0;
    CHECK(series_count(s, &count) == DF_OK && count == 2);
    CHECK(series_sum(s, &d) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_mean(s, &d) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_median(s, &d) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_min(s, &d) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_max(s, &d) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_aggregate(s, DF_AGG_COUNT, &d) == DF_OK && close_to(d, 2.0));
    CHECK(series_aggregate(s, DF_AGG_MEAN, &d) == DF_ERR_TYPE_MISMATCH);
    series_free(s);
}

static void test_aggregate_and_names(void)
{
    int64_t values[] = {1, 2, 3, 10};
    Series *s = make_int_column("v", values, 4);
    double d = 0.0;
    CHECK(series_aggregate(s, DF_AGG_COUNT, &d) == DF_OK && close_to(d, 4.0));
    CHECK(series_aggregate(s, DF_AGG_SUM, &d) == DF_OK && close_to(d, 16.0));
    CHECK(series_aggregate(s, DF_AGG_MEAN, &d) == DF_OK && close_to(d, 4.0));
    CHECK(series_aggregate(s, DF_AGG_MEDIAN, &d) == DF_OK && close_to(d, 2.5));
    CHECK(series_aggregate(s, DF_AGG_MIN, &d) == DF_OK && close_to(d, 1.0));
    CHECK(series_aggregate(s, DF_AGG_MAX, &d) == DF_OK && close_to(d, 10.0));
    CHECK(series_aggregate(s, (df_agg_func)42, &d) == DF_ERR_INVALID_ARG);
    series_free(s);

    CHECK(same_string(df_agg_name(DF_AGG_COUNT), "count"));
    CHECK(same_string(df_agg_name(DF_AGG_MEDIAN), "median"));
    CHECK(same_string(df_agg_name((df_agg_func)42), "unknown"));
}

static void test_null_args(void)
{
    int64_t values[] = {1};
    Series *s = make_int_column("v", values, 1);
    size_t count = 0;
    double d = 0.0;
    CHECK(series_count(NULL, &count) == DF_ERR_NULL_ARG);
    CHECK(series_count(s, NULL) == DF_ERR_NULL_ARG);
    CHECK(series_sum(NULL, &d) == DF_ERR_NULL_ARG);
    CHECK(series_mean(s, NULL) == DF_ERR_NULL_ARG);
    CHECK(series_median(NULL, &d) == DF_ERR_NULL_ARG);
    CHECK(series_min(NULL, &d) == DF_ERR_NULL_ARG);
    CHECK(series_max(s, NULL) == DF_ERR_NULL_ARG);
    CHECK(series_aggregate(NULL, DF_AGG_SUM, &d) == DF_ERR_NULL_ARG);
    series_free(s);
}

static void test_on_dataframe_column(void)
{
    double scores[] = {80.0, MISSING_NUM, 60.0};
    DataFrame *df = new_frame();
    add_column(df, make_float_column("score", scores, 3));

    const Series *col = NULL;
    double avg = 0.0;
    CHECK(df_get_column(df, "score", &col) == DF_OK);
    CHECK(series_mean(col, &avg) == DF_OK && close_to(avg, 70.0));
    df_free(df);
}

static void test_large(void)
{
    /* the numbers 0..N-1 in a shuffled order */
    enum { N = 100001 };
    int64_t *values = malloc(N * sizeof(int64_t));
    CHECK(values != NULL);
    if (values == NULL) return;
    for (int i = 0; i < N; i++) {
        values[i] = ((int64_t)i * 7919) % N;  /* 7919 is prime, so no repeats */
    }
    Series *s = make_int_column("big", values, N);
    free(values);

    double d = 0.0;
    CHECK(series_sum(s, &d) == DF_OK && close_to(d, 5000050000.0));
    CHECK(series_mean(s, &d) == DF_OK && close_to(d, 50000.0));
    CHECK(series_median(s, &d) == DF_OK && close_to(d, 50000.0));
    CHECK(series_min(s, &d) == DF_OK && close_to(d, 0.0));
    CHECK(series_max(s, &d) == DF_OK && close_to(d, 100000.0));
    series_free(s);
}

/* ---- variance, std, quantiles, nunique, bools, describe ----------------- */

static void test_spread_and_quantiles(void)
{
    /* 2 4 4 4 5 5 7 9 (+ a missing cell): mean 5, sample var 32/7 */
    int64_t values[] = {2, 4, 4, MISSING_NUM, 4, 5, 5, 7, 9};
    Series *s = make_int_column("v", values, 9);
    double out = 0.0;

    CHECK(series_var(s, &out) == DF_OK && close_to(out, 32.0 / 7.0));
    CHECK(series_std(s, &out) == DF_OK && close_to(out * out, 32.0 / 7.0));
    CHECK(series_quantile(s, 0.0, &out) == DF_OK && close_to(out, 2.0));
    CHECK(series_quantile(s, 1.0, &out) == DF_OK && close_to(out, 9.0));
    CHECK(series_quantile(s, 0.5, &out) == DF_OK && close_to(out, 4.5));
    /* position 0.25 * 7 = 1.75: between 4 and 4 */
    CHECK(series_quantile(s, 0.25, &out) == DF_OK && close_to(out, 4.0));
    /* position 0.75 * 7 = 5.25: 5 + 0.25 * (7 - 5) */
    CHECK(series_quantile(s, 0.75, &out) == DF_OK && close_to(out, 5.5));
    /* position 0.9 * 7 = 6.3: 7 + 0.3 * (9 - 7) */
    CHECK(series_quantile(s, 0.9, &out) == DF_OK && close_to(out, 7.6));
    CHECK(series_quantile(s, 1.5, &out) == DF_ERR_INVALID_ARG);
    CHECK(series_quantile(s, -0.1, &out) == DF_ERR_INVALID_ARG);

    size_t n = 0;
    CHECK(series_nunique(s, &n) == DF_OK && n == 5);    /* 2 4 5 7 9 */
    CHECK(series_aggregate(s, DF_AGG_NUNIQUE, &out) == DF_OK && out == 5.0);
    CHECK(series_aggregate(s, DF_AGG_STD, &out) == DF_OK);
    CHECK(series_aggregate(s, DF_AGG_VAR, &out) == DF_OK &&
          close_to(out, 32.0 / 7.0));
    CHECK(same_string(df_agg_name(DF_AGG_STD), "std"));
    CHECK(same_string(df_agg_name(DF_AGG_VAR), "var"));
    CHECK(same_string(df_agg_name(DF_AGG_NUNIQUE), "nunique"));
    series_free(s);

    /* fewer than two values: no variance */
    int64_t one[] = {MISSING_NUM, 3};
    s = make_int_column("one", one, 2);
    out = 123.0;
    CHECK(series_var(s, &out) == DF_ERR_MISSING_VALUE && out == 123.0);
    CHECK(series_std(s, &out) == DF_ERR_MISSING_VALUE);
    CHECK(series_quantile(s, 0.3, &out) == DF_OK && out == 3.0);
    series_free(s);

    /* nunique of text, with missing cells */
    const char *words[] = {"b", "a", NULL, "b", "c", NULL, "a"};
    s = make_string_column("w", words, 7);
    CHECK(series_nunique(s, &n) == DF_OK && n == 3);
    CHECK(series_std(s, &out) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_quantile(s, 0.5, &out) == DF_ERR_TYPE_MISMATCH);
    series_free(s);

    /* nunique of floats: 0.0 and -0.0 are the same value */
    double fl[] = {0.0, -0.0, 1.5, 1.5, MISSING_NUM};
    s = make_float_column("f", fl, 5);
    CHECK(series_nunique(s, &n) == DF_OK && n == 2);
    series_free(s);

    CHECK(series_nunique(NULL, &n) == DF_ERR_NULL_ARG);
    CHECK(series_var(NULL, &out) == DF_ERR_NULL_ARG);
}

static void test_bool_numbers(void)
{
    Series *s = NULL;
    series_create("ok", DTYPE_BOOL, 0, &s);
    series_append_bool(s, 1);
    series_append_bool(s, 0);
    series_append_bool(s, 1);
    series_append_missing(s);
    series_append_bool(s, 1);
    double out = 0.0;
    CHECK(series_sum(s, &out) == DF_OK && out == 3.0);
    CHECK(series_mean(s, &out) == DF_OK && close_to(out, 0.75));
    CHECK(series_min(s, &out) == DF_OK && out == 0.0);
    size_t n = 0;
    CHECK(series_nunique(s, &n) == DF_OK && n == 2);
    series_free(s);

    /* dates are not numbers here */
    series_create("d", DTYPE_DATE, 0, &s);
    series_append_date(s, 10);
    series_append_date(s, 10);
    CHECK(series_mean(s, &out) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_nunique(s, &n) == DF_OK && n == 1);
    series_free(s);
}

static void test_describe(void)
{
    int64_t ages[] = {25, 32, MISSING_NUM, 41};
    double scores[] = {4.5, MISSING_NUM, MISSING_NUM, MISSING_NUM};
    const char *names[] = {"A", "B", "C", "D"};
    DataFrame *df = new_frame();
    add_column(df, make_string_column("name", names, 4));
    add_column(df, make_int_column("age", ages, 4));
    add_column(df, make_float_column("score", scores, 4));

    DataFrame *out = NULL;
    CHECK(df_describe(df, &out) == DF_OK);
    CHECK(df_ncols(out) == 3);          /* stat, age, score; no "name" */
    CHECK(df_nrows(out) == 8);
    const char *labels[] = {"count", "mean", "std", "min",
                            "25%", "50%", "75%", "max"};
    for (size_t r = 0; r < 8; r++) {
        CHECK(same_string(cell_string(out, "stat", r), labels[r]));
    }
    CHECK(cell_float(out, "age", 0) == 3.0);
    CHECK(close_to(cell_float(out, "age", 1), 98.0 / 3.0));
    CHECK(cell_float(out, "age", 3) == 25.0);
    CHECK(close_to(cell_float(out, "age", 4), 28.5));   /* 25 + .5 * 7 */
    CHECK(cell_float(out, "age", 5) == 32.0);
    CHECK(close_to(cell_float(out, "age", 6), 36.5));
    CHECK(cell_float(out, "age", 7) == 41.0);
    /* one value: std is missing, the rest are that value */
    CHECK(cell_float(out, "score", 0) == 1.0);
    CHECK(cell_missing(out, "score", 2));
    CHECK(cell_float(out, "score", 5) == 4.5);
    df_free(out);

    /* no number columns: just the labels */
    DataFrame *text_only = new_frame();
    add_column(text_only, make_string_column("name", names, 4));
    CHECK(df_describe(text_only, &out) == DF_OK);
    CHECK(df_ncols(out) == 1 && df_nrows(out) == 8);
    df_free(out);
    df_free(text_only);

    /* a number column called "stat" clashes with the label column */
    CHECK(df_rename_column(df, "age", "stat") == DF_OK);
    out = (DataFrame *)1;
    CHECK(df_describe(df, &out) == DF_ERR_DUPLICATE_NAME);
    CHECK(out == NULL);
    CHECK(df_describe(NULL, &out) == DF_ERR_NULL_ARG);
    df_free(df);
}

int main(void)
{
    test_int_series();
    test_float_series();
    test_single_and_negative();
    test_empty_and_all_missing();
    test_string_series();
    test_aggregate_and_names();
    test_null_args();
    test_on_dataframe_column();
    test_large();
    test_spread_and_quantiles();
    test_bool_numbers();
    test_describe();
    return finish_tests();
}
