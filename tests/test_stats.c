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
    return finish_tests();
}
