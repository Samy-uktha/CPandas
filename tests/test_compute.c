/* Tests for df_compute.c. Build and run with `make test`. */
#include <stdint.h>
#include <stdlib.h>

#include "df_compute.h"
#include "test_helpers.h"

/*  salary  bonus  rate   remote
 *  100     10     0.5    True
 *  200     NA     0.25   False
 *  NA      5      2.0    True
 *  300     0      NA     NA
 */
static DataFrame *make_frame(void)
{
    int64_t salary[] = {100, 200, MISSING_NUM, 300};
    int64_t bonus[] = {10, MISSING_NUM, 5, 0};
    double rate[] = {0.5, 0.25, 2.0, MISSING_NUM};
    const char *names[] = {"A", "B", "C", "D"};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("salary", salary, 4));
    add_column(df, make_int_column("bonus", bonus, 4));
    add_column(df, make_float_column("rate", rate, 4));
    add_column(df, make_string_column("name", names, 4));

    Series *remote = NULL;
    series_create("remote", DTYPE_BOOL, 4, &remote);
    series_append_bool(remote, 1);
    series_append_bool(remote, 0);
    series_append_bool(remote, 1);
    series_append_missing(remote);
    add_column(df, remote);
    return df;
}

static df_dtype type_of(const DataFrame *df, const char *name)
{
    const Series *s = NULL;
    df_get_column(df, name, &s);
    return series_dtype(s);
}

static void test_two_columns(void)
{
    DataFrame *df = make_frame();

    CHECK(df_add_arith_column(df, "total", "salary", DF_ARITH_ADD,
                              "bonus") == DF_OK);
    CHECK(type_of(df, "total") == DTYPE_INT);
    CHECK(cell_int(df, "total", 0) == 110);
    CHECK(cell_missing(df, "total", 1));        /* bonus missing */
    CHECK(cell_missing(df, "total", 2));        /* salary missing */
    CHECK(cell_int(df, "total", 3) == 300);

    CHECK(df_add_arith_column(df, "scaled", "salary", DF_ARITH_MUL,
                              "rate") == DF_OK);
    CHECK(type_of(df, "scaled") == DTYPE_FLOAT);
    CHECK(cell_float(df, "scaled", 0) == 50.0);
    CHECK(cell_float(df, "scaled", 1) == 50.0);

    /* division: always FLOAT, x / 0 is missing */
    CHECK(df_add_arith_column(df, "ratio", "salary", DF_ARITH_DIV,
                              "bonus") == DF_OK);
    CHECK(type_of(df, "ratio") == DTYPE_FLOAT);
    CHECK(cell_float(df, "ratio", 0) == 10.0);
    CHECK(cell_missing(df, "ratio", 3));        /* 300 / 0 */

    /* bools count as 0 and 1 */
    CHECK(df_add_arith_column(df, "extra", "bonus", DF_ARITH_MUL,
                              "remote") == DF_OK);
    CHECK(type_of(df, "extra") == DTYPE_INT);
    CHECK(cell_int(df, "extra", 0) == 10);
    CHECK(cell_int(df, "extra", 2) == 5);

    CHECK(df_ncols(df) == 9);
    df_free(df);
}

static void test_scalar(void)
{
    DataFrame *df = make_frame();
    CHECK(df_add_scalar_column(df, "tax", "salary", DF_ARITH_MUL, 0.1) ==
          DF_OK);
    CHECK(type_of(df, "tax") == DTYPE_FLOAT);
    CHECK(close_to(cell_float(df, "tax", 1), 20.0));
    CHECK(cell_missing(df, "tax", 2));

    CHECK(df_add_scalar_column(df, "double", "salary", DF_ARITH_MUL, 2.0) ==
          DF_OK);
    CHECK(type_of(df, "double") == DTYPE_INT);  /* 2.0 is a whole number */
    CHECK(cell_int(df, "double", 3) == 600);

    CHECK(df_add_scalar_column(df, "minus", "salary", DF_ARITH_SUB, 1) ==
          DF_OK);
    CHECK(cell_int(df, "minus", 0) == 99);

    CHECK(df_add_scalar_column(df, "half", "salary", DF_ARITH_DIV, 2) ==
          DF_OK);
    CHECK(type_of(df, "half") == DTYPE_FLOAT);
    CHECK(cell_float(df, "half", 0) == 50.0);

    CHECK(df_add_scalar_column(df, "bad", "salary", DF_ARITH_DIV, 0) ==
          DF_OK);
    CHECK(cell_missing(df, "bad", 0));
    df_free(df);
}

static void test_overflow_and_errors(void)
{
    int64_t big[] = {INT64_MAX - 1, 5};
    int64_t neg[] = {INT64_MIN + 1, -5};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("big", big, 2));
    add_column(df, make_int_column("neg", neg, 2));
    const char *names[] = {"a", "b"};
    add_column(df, make_string_column("name", names, 2));

    CHECK(df_add_scalar_column(df, "x", "big", DF_ARITH_ADD, 1) == DF_OK);
    CHECK(cell_int(df, "x", 0) == INT64_MAX);
    CHECK(df_add_scalar_column(df, "y", "big", DF_ARITH_ADD, 2) ==
          DF_ERR_OVERFLOW);
    CHECK(df_add_scalar_column(df, "y", "neg", DF_ARITH_SUB, 2) ==
          DF_ERR_OVERFLOW);
    CHECK(df_add_arith_column(df, "y", "big", DF_ARITH_MUL, "big") ==
          DF_ERR_OVERFLOW);
    CHECK(df_add_arith_column(df, "y", "neg", DF_ARITH_MUL, "big") ==
          DF_ERR_OVERFLOW);
    CHECK(df_add_arith_column(df, "y", "neg", DF_ARITH_MUL, "neg") ==
          DF_ERR_OVERFLOW);
    CHECK(df_ncols(df) == 4);       /* the failed calls added nothing */
    /* a float result cannot overflow like that */
    CHECK(df_add_arith_column(df, "y", "big", DF_ARITH_DIV, "neg") == DF_OK);

    CHECK(df_add_scalar_column(df, "x", "big", DF_ARITH_ADD, 1) ==
          DF_ERR_DUPLICATE_NAME);
    CHECK(df_add_scalar_column(df, "", "big", DF_ARITH_ADD, 1) ==
          DF_ERR_INVALID_ARG);
    CHECK(df_add_scalar_column(df, "z", "nope", DF_ARITH_ADD, 1) ==
          DF_ERR_NOT_FOUND);
    CHECK(df_add_scalar_column(df, "z", "name", DF_ARITH_ADD, 1) ==
          DF_ERR_TYPE_MISMATCH);
    CHECK(df_add_arith_column(df, "z", "big", (df_arith_op)9, "neg") ==
          DF_ERR_INVALID_ARG);
    CHECK(df_add_arith_column(NULL, "z", "big", DF_ARITH_ADD, "neg") ==
          DF_ERR_NULL_ARG);
    CHECK(df_ncols(df) == 5);
    df_free(df);
}

/* salary per day of the year, or missing when salary is missing */
static int per_day(const DataFrame *df, size_t row, void *user_data,
                   double *out)
{
    const double *days = user_data;
    const Series *salary = NULL;
    int64_t v = 0;
    if (df_get_column(df, "salary", &salary) != DF_OK ||
        series_get_int(salary, row, &v) != DF_OK) {
        return 0;
    }
    *out = (double)v / *days;
    return 1;
}

static void test_custom(void)
{
    DataFrame *df = make_frame();
    double days = 4.0;
    CHECK(df_add_computed_column(df, "daily", per_day, &days) == DF_OK);
    CHECK(type_of(df, "daily") == DTYPE_FLOAT);
    CHECK(cell_float(df, "daily", 1) == 50.0);
    CHECK(cell_missing(df, "daily", 2));
    CHECK(df_add_computed_column(df, "daily", per_day, &days) ==
          DF_ERR_DUPLICATE_NAME);
    CHECK(df_add_computed_column(df, "other", NULL, NULL) == DF_ERR_NULL_ARG);
    df_free(df);

    /* on a frame with no columns: a column with 0 rows */
    df = new_frame();
    CHECK(df_add_computed_column(df, "c", per_day, &days) == DF_OK);
    CHECK(df_ncols(df) == 1 && df_nrows(df) == 0);
    df_free(df);
}

int main(void)
{
    test_two_columns();
    test_scalar();
    test_overflow_and_errors();
    test_custom();
    return finish_tests();
}
