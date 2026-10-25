/* Tests for df_groupby.c. Build and run with `make test`. */
#include <stdlib.h>

#include "df_groupby.h"
#include "test_helpers.h"

/*   dept    name   salary
 *   Sales   Asha   100
 *   IT      Ben    200
 *   Sales   Chen   150
 *   HR      Dev    120
 *   IT      Esha   NA
 *   NA      Farid  999      <- missing key: left out of every group
 *   IT      Gita   300
 *   Ops     Hari   NA       <- a group with no salary at all
 */
static DataFrame *make_staff(void)
{
    const char *depts[] = {"Sales", "IT", "Sales", "HR", "IT", NULL, "IT", "Ops"};
    const char *names[] = {"Asha", "Ben", "Chen", "Dev", "Esha", "Farid",
                           "Gita", "Hari"};
    int64_t salaries[] = {100, 200, 150, 120, MISSING_NUM, 999, 300,
                          MISSING_NUM};

    DataFrame *df = new_frame();
    add_column(df, make_string_column("dept", depts, 8));
    add_column(df, make_string_column("name", names, 8));
    add_column(df, make_int_column("salary", salaries, 8));
    return df;
}

static void test_basic_groupby(void)
{
    DataFrame *df = make_staff();
    DataFrame *out = NULL;
    df_aggregation aggs[] = {
        {"salary", DF_AGG_SUM},
        {"salary", DF_AGG_MEAN},
        {"salary", DF_AGG_COUNT},
        {"name", DF_AGG_COUNT},
        {"salary", DF_AGG_MAX}
    };

    CHECK(df_groupby(df, "dept", aggs, 5, &out) == DF_OK);
    CHECK(df_nrows(out) == 4);      /* HR, IT, Ops, Sales */
    CHECK(df_ncols(out) == 6);      /* key + 5 aggregations */

    /* column names and types */
    const char *name = NULL;
    df_column_name(out, 0, &name);
    CHECK(same_string(name, "dept"));
    df_column_name(out, 1, &name);
    CHECK(same_string(name, "salary_sum"));
    df_column_name(out, 2, &name);
    CHECK(same_string(name, "salary_mean"));
    df_column_name(out, 4, &name);
    CHECK(same_string(name, "name_count"));
    const Series *col = NULL;
    df_get_column(out, "salary_count", &col);
    CHECK(series_dtype(col) == DTYPE_INT);
    df_get_column(out, "salary_mean", &col);
    CHECK(series_dtype(col) == DTYPE_FLOAT);

    /* groups are sorted by key */
    CHECK(same_string(cell_string(out, "dept", 0), "HR"));
    CHECK(same_string(cell_string(out, "dept", 1), "IT"));
    CHECK(same_string(cell_string(out, "dept", 2), "Ops"));
    CHECK(same_string(cell_string(out, "dept", 3), "Sales"));

    /* HR: 120 */
    CHECK(close_to(cell_float(out, "salary_sum", 0), 120.0));
    CHECK(cell_int(out, "salary_count", 0) == 1);

    /* IT: 200, NA, 300 */
    CHECK(close_to(cell_float(out, "salary_sum", 1), 500.0));
    CHECK(close_to(cell_float(out, "salary_mean", 1), 250.0));
    CHECK(cell_int(out, "salary_count", 1) == 2);
    CHECK(cell_int(out, "name_count", 1) == 3);
    CHECK(close_to(cell_float(out, "salary_max", 1), 300.0));

    /* Ops: only NA -> sum 0, count 0, mean and max are missing */
    CHECK(close_to(cell_float(out, "salary_sum", 2), 0.0));
    CHECK(cell_int(out, "salary_count", 2) == 0);
    CHECK(cell_missing(out, "salary_mean", 2) == 1);
    CHECK(cell_missing(out, "salary_max", 2) == 1);
    CHECK(cell_int(out, "name_count", 2) == 1);

    /* Sales: 100, 150 */
    CHECK(close_to(cell_float(out, "salary_mean", 3), 125.0));

    /* Farid's 999 (missing dept) is in no group */
    df_get_column(out, "salary_max", &col);
    double big = 0.0;
    CHECK(series_max(col, &big) == DF_OK && close_to(big, 300.0));

    df_free(out);
    CHECK(df_nrows(df) == 8);   /* input unchanged */
    df_free(df);
}

static void test_int_key_and_median(void)
{
    int64_t teams[] = {2, 1, 2, 1, 2};
    double points[] = {5.0, 1.0, 9.0, 3.0, 6.0};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("team", teams, 5));
    add_column(df, make_float_column("points", points, 5));

    df_aggregation aggs[] = {
        {"points", DF_AGG_MEDIAN},
        {"points", DF_AGG_MIN}
    };
    DataFrame *out = NULL;
    CHECK(df_groupby(df, "team", aggs, 2, &out) == DF_OK);
    CHECK(df_nrows(out) == 2);
    CHECK(cell_int(out, "team", 0) == 1 && cell_int(out, "team", 1) == 2);
    CHECK(close_to(cell_float(out, "points_median", 0), 2.0));  /* 1, 3 */
    CHECK(close_to(cell_float(out, "points_median", 1), 6.0));  /* 5, 6, 9 */
    CHECK(close_to(cell_float(out, "points_min", 1), 5.0));
    df_free(out);
    df_free(df);
}

static void test_no_aggregations(void)
{
    DataFrame *df = make_staff();
    DataFrame *out = NULL;
    /* with no aggregations, the result is the list of different keys */
    CHECK(df_groupby(df, "dept", NULL, 0, &out) == DF_OK);
    CHECK(df_nrows(out) == 4 && df_ncols(out) == 1);
    df_free(out);
    df_free(df);
}

static void test_errors(void)
{
    DataFrame *df = make_staff();
    DataFrame *out = (DataFrame *)1;
    df_aggregation mean_of_text[] = {{"name", DF_AGG_MEAN}};
    df_aggregation unknown_col[] = {{"bonus", DF_AGG_SUM}};
    df_aggregation bad_func[] = {{"salary", (df_agg_func)77}};
    df_aggregation null_col[] = {{NULL, DF_AGG_SUM}};
    df_aggregation twice[] = {{"salary", DF_AGG_SUM}, {"salary", DF_AGG_SUM}};

    CHECK(df_groupby(df, "nope", NULL, 0, &out) == DF_ERR_NOT_FOUND);
    CHECK(out == NULL);
    CHECK(df_groupby(df, "dept", mean_of_text, 1, &out) == DF_ERR_TYPE_MISMATCH);
    CHECK(df_groupby(df, "dept", unknown_col, 1, &out) == DF_ERR_NOT_FOUND);
    CHECK(df_groupby(df, "dept", bad_func, 1, &out) == DF_ERR_INVALID_ARG);
    CHECK(df_groupby(df, "dept", null_col, 1, &out) == DF_ERR_NULL_ARG);
    /* fails half way (2nd column has the same name): nothing may leak */
    CHECK(df_groupby(df, "dept", twice, 2, &out) == DF_ERR_DUPLICATE_NAME);
    CHECK(out == NULL);

    CHECK(df_groupby(NULL, "dept", NULL, 0, &out) == DF_ERR_NULL_ARG);
    CHECK(df_groupby(df, NULL, NULL, 0, &out) == DF_ERR_NULL_ARG);
    CHECK(df_groupby(df, "dept", NULL, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_groupby(df, "dept", NULL, 0, NULL) == DF_ERR_NULL_ARG);
    df_free(df);
}

static void test_empty_and_all_missing_keys(void)
{
    /* 0 rows: 0 groups, but the columns are still there */
    DataFrame *df = new_frame();
    add_column(df, make_string_column("k", NULL, 0));
    add_column(df, make_int_column("v", NULL, 0));
    df_aggregation aggs[] = {{"v", DF_AGG_SUM}};
    DataFrame *out = NULL;
    CHECK(df_groupby(df, "k", aggs, 1, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 2);
    df_free(out);
    df_free(df);

    /* every key missing: 0 groups */
    const char *keys[] = {NULL, NULL};
    int64_t vals[] = {1, 2};
    df = new_frame();
    add_column(df, make_string_column("k", keys, 2));
    add_column(df, make_int_column("v", vals, 2));
    CHECK(df_groupby(df, "k", aggs, 1, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 2);
    df_free(out);
    df_free(df);
}

static void test_large(void)
{
    /* key = i % 10, value = i. Group k has the values k, k+10, k+20, ... */
    enum { N = 100000 };
    int64_t *keys = malloc(N * sizeof(int64_t));
    int64_t *vals = malloc(N * sizeof(int64_t));
    CHECK(keys != NULL && vals != NULL);
    if (keys == NULL || vals == NULL) {
        free(keys);
        free(vals);
        return;
    }
    for (int i = 0; i < N; i++) {
        keys[i] = i % 10;
        vals[i] = i;
    }
    DataFrame *df = new_frame();
    add_column(df, make_int_column("key", keys, N));
    add_column(df, make_int_column("val", vals, N));
    free(keys);
    free(vals);

    df_aggregation aggs[] = {{"val", DF_AGG_COUNT}, {"val", DF_AGG_SUM}};
    DataFrame *out = NULL;
    CHECK(df_groupby(df, "key", aggs, 2, &out) == DF_OK);
    CHECK(df_nrows(out) == 10);
    int all_ok = 1;
    for (int k = 0; k < 10; k++) {
        /* sum of (10*j + k) for j = 0..9999 */
        double expected = 10.0 * (9999.0 * 10000.0 / 2.0) + 10000.0 * k;
        if (cell_int(out, "key", k) != k ||
            cell_int(out, "val_count", k) != 10000 ||
            !close_to(cell_float(out, "val_sum", k), expected)) {
            all_ok = 0;
        }
    }
    CHECK(all_ok);
    df_free(out);
    df_free(df);
}

int main(void)
{
    test_basic_groupby();
    test_int_key_and_median();
    test_no_aggregations();
    test_errors();
    test_empty_and_all_missing_keys();
    test_large();
    return finish_tests();
}
