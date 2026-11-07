/* Tests for df_filter.c. Build and run with `make test`. */
#include <stdlib.h>

#include "df_filter.h"
#include "test_helpers.h"

/*   name    age   score  city
 *   Asha    25    88.5   Pune
 *   Ben     32    72.0   Delhi
 *   Chen    NA    90.0   Pune
 *   Dev     41    NA     Mumbai
 *   Esha    32    65.5   Pune
 *   NA      19    72.0   NA
 */
static DataFrame *make_people(void)
{
    const char *names[] = {"Asha", "Ben", "Chen", "Dev", "Esha", NULL};
    int64_t ages[] = {25, 32, MISSING_NUM, 41, 32, 19};
    double scores[] = {88.5, 72.0, 90.0, MISSING_NUM, 65.5, 72.0};
    const char *cities[] = {"Pune", "Delhi", "Pune", "Mumbai", "Pune", NULL};

    DataFrame *df = new_frame();
    add_column(df, make_string_column("name", names, 6));
    add_column(df, make_int_column("age", ages, 6));
    add_column(df, make_float_column("score", scores, 6));
    add_column(df, make_string_column("city", cities, 6));
    return df;
}

static void test_filter_int_column(void)
{
    DataFrame *df = make_people();
    DataFrame *out = NULL;

    CHECK(df_filter_number(df, "age", DF_OP_GT, 30, &out) == DF_OK);
    CHECK(df_nrows(out) == 3 && df_ncols(out) == 4);
    CHECK(same_string(cell_string(out, "name", 0), "Ben"));
    CHECK(same_string(cell_string(out, "name", 1), "Dev"));
    CHECK(same_string(cell_string(out, "name", 2), "Esha"));
    df_free(out);

    CHECK(df_filter_number(df, "age", DF_OP_EQ, 32, &out) == DF_OK);
    CHECK(df_nrows(out) == 2);
    df_free(out);

    /* != does not match the missing age */
    CHECK(df_filter_number(df, "age", DF_OP_NE, 32, &out) == DF_OK);
    CHECK(df_nrows(out) == 3);
    df_free(out);

    CHECK(df_filter_number(df, "age", DF_OP_LT, 25, &out) == DF_OK);
    CHECK(df_nrows(out) == 1 && cell_int(out, "age", 0) == 19);
    df_free(out);
    CHECK(df_filter_number(df, "age", DF_OP_LE, 25, &out) == DF_OK);
    CHECK(df_nrows(out) == 2);
    df_free(out);
    CHECK(df_filter_number(df, "age", DF_OP_GE, 41, &out) == DF_OK);
    CHECK(df_nrows(out) == 1);
    df_free(out);

    CHECK(df_nrows(df) == 6);   /* input unchanged */
    df_free(df);
}

static void test_filter_float_column(void)
{
    DataFrame *df = make_people();
    DataFrame *out = NULL;

    CHECK(df_filter_number(df, "score", DF_OP_GE, 72.0, &out) == DF_OK);
    CHECK(df_nrows(out) == 4);  /* 88.5, 72, 90, 72 (NA skipped) */
    df_free(out);

    CHECK(df_filter_number(df, "score", DF_OP_LT, 70.0, &out) == DF_OK);
    CHECK(df_nrows(out) == 1);
    CHECK(same_string(cell_string(out, "name", 0), "Esha"));
    df_free(out);

    /* no row matches: same columns, 0 rows */
    CHECK(df_filter_number(df, "score", DF_OP_GT, 1000.0, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 4);
    df_free(out);
    df_free(df);
}

static void test_filter_string_column(void)
{
    DataFrame *df = make_people();
    DataFrame *out = NULL;

    CHECK(df_filter_string(df, "city", DF_OP_EQ, "Pune", &out) == DF_OK);
    CHECK(df_nrows(out) == 3);
    CHECK(same_string(cell_string(out, "name", 2), "Esha"));
    df_free(out);

    /* != skips the missing city */
    CHECK(df_filter_string(df, "city", DF_OP_NE, "Pune", &out) == DF_OK);
    CHECK(df_nrows(out) == 2);
    df_free(out);

    /* alphabetical: names before "Chen" are Asha and Ben */
    CHECK(df_filter_string(df, "name", DF_OP_LT, "Chen", &out) == DF_OK);
    CHECK(df_nrows(out) == 2);
    df_free(out);

    /* comparison is case-sensitive */
    CHECK(df_filter_string(df, "city", DF_OP_EQ, "pune", &out) == DF_OK);
    CHECK(df_nrows(out) == 0);
    df_free(out);
    df_free(df);
}

static void test_filter_missing(void)
{
    DataFrame *df = make_people();
    DataFrame *out = NULL;

    CHECK(df_filter_missing(df, "age", 1, &out) == DF_OK);
    CHECK(df_nrows(out) == 1);
    CHECK(same_string(cell_string(out, "name", 0), "Chen"));
    df_free(out);

    CHECK(df_filter_missing(df, "age", 0, &out) == DF_OK);
    CHECK(df_nrows(out) == 5);
    df_free(out);

    CHECK(df_filter_missing(df, "city", 1, &out) == DF_OK);
    CHECK(df_nrows(out) == 1 && cell_int(out, "age", 0) == 19);
    df_free(out);
    df_free(df);
}

/* Custom condition: age < limit OR name == "Dev". */
static int young_or_dev(const DataFrame *df, size_t row, void *user_data)
{
    int64_t limit = *(int64_t *)user_data;
    const Series *age = NULL;
    const Series *name = NULL;
    df_get_column(df, "age", &age);
    df_get_column(df, "name", &name);

    int64_t a = 0;
    if (series_get_int(age, row, &a) == DF_OK && a < limit) {
        return 1;
    }
    const char *n = NULL;
    if (series_get_string(name, row, &n) == DF_OK && strcmp(n, "Dev") == 0) {
        return 1;
    }
    return 0;
}

static int keep_nothing(const DataFrame *df, size_t row, void *user_data)
{
    (void)df;
    (void)row;
    (void)user_data;
    return 0;
}

static void test_filter_custom(void)
{
    DataFrame *df = make_people();
    DataFrame *out = NULL;
    int64_t limit = 30;

    CHECK(df_filter_custom(df, young_or_dev, &limit, &out) == DF_OK);
    CHECK(df_nrows(out) == 3);  /* Asha (25), Dev, the unnamed 19 */
    CHECK(same_string(cell_string(out, "name", 0), "Asha"));
    CHECK(same_string(cell_string(out, "name", 1), "Dev"));
    CHECK(cell_missing(out, "name", 2) == 1);
    df_free(out);

    CHECK(df_filter_custom(df, keep_nothing, NULL, &out) == DF_OK);
    CHECK(df_nrows(out) == 0);
    df_free(out);
    df_free(df);
}

static void test_and_by_filtering_twice(void)
{
    DataFrame *df = make_people();
    DataFrame *step1 = NULL;
    DataFrame *step2 = NULL;

    /* city == "Pune" AND age > 30  ->  only Esha */
    CHECK(df_filter_string(df, "city", DF_OP_EQ, "Pune", &step1) == DF_OK);
    CHECK(df_filter_number(step1, "age", DF_OP_GT, 30, &step2) == DF_OK);
    CHECK(df_nrows(step2) == 1);
    CHECK(same_string(cell_string(step2, "name", 0), "Esha"));
    df_free(step1);
    df_free(step2);
    df_free(df);
}

static void test_errors(void)
{
    DataFrame *df = make_people();
    DataFrame *out = (DataFrame *)1;

    CHECK(df_filter_number(df, "salary", DF_OP_GT, 1, &out) == DF_ERR_NOT_FOUND);
    CHECK(out == NULL);
    CHECK(df_filter_number(df, "name", DF_OP_GT, 1, &out) == DF_ERR_TYPE_MISMATCH);
    CHECK(df_filter_string(df, "age", DF_OP_EQ, "x", &out) == DF_ERR_TYPE_MISMATCH);
    CHECK(df_filter_number(df, "age", (df_compare_op)99, 1, &out) == DF_ERR_INVALID_ARG);
    CHECK(df_filter_string(df, "city", (df_compare_op)-1, "x", &out) == DF_ERR_INVALID_ARG);
    CHECK(df_filter_missing(df, "nope", 1, &out) == DF_ERR_NOT_FOUND);

    CHECK(df_filter_number(NULL, "age", DF_OP_GT, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_filter_number(df, NULL, DF_OP_GT, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_filter_number(df, "age", DF_OP_GT, 1, NULL) == DF_ERR_NULL_ARG);
    CHECK(df_filter_string(df, "city", DF_OP_EQ, NULL, &out) == DF_ERR_NULL_ARG);
    CHECK(df_filter_custom(df, NULL, NULL, &out) == DF_ERR_NULL_ARG);
    CHECK(df_filter_missing(NULL, "age", 1, &out) == DF_ERR_NULL_ARG);
    CHECK(out == NULL);

    CHECK(df_nrows(df) == 6 && df_ncols(df) == 4);  /* unchanged */
    df_free(df);
}

static void test_empty_frame(void)
{
    /* columns but 0 rows */
    DataFrame *df = new_frame();
    add_column(df, make_int_column("age", NULL, 0));
    add_column(df, make_string_column("city", NULL, 0));

    DataFrame *out = NULL;
    CHECK(df_filter_number(df, "age", DF_OP_GT, 1, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 2);
    df_free(out);
    CHECK(df_filter_string(df, "city", DF_OP_EQ, "Pune", &out) == DF_OK);
    CHECK(df_nrows(out) == 0);
    df_free(out);
    df_free(df);
}

static void test_large(void)
{
    enum { N = 100000 };
    int64_t *ids = malloc(N * sizeof(int64_t));
    CHECK(ids != NULL);
    if (ids == NULL) return;
    for (int i = 0; i < N; i++) {
        ids[i] = i;
    }
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, N));
    free(ids);

    DataFrame *out = NULL;
    CHECK(df_filter_number(df, "id", DF_OP_GE, 90000, &out) == DF_OK);
    CHECK(df_nrows(out) == 10000);
    CHECK(cell_int(out, "id", 0) == 90000);
    df_free(out);
    df_free(df);
}

int main(void)
{
    test_filter_int_column();
    test_filter_float_column();
    test_filter_string_column();
    test_filter_missing();
    test_filter_custom();
    test_and_by_filtering_twice();
    test_errors();
    test_empty_frame();
    test_large();
    return finish_tests();
}
