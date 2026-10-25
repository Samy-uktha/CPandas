/* Tests for df_sort.c. Build and run with `make test`. */
#include <stdlib.h>

#include "df_sort.h"
#include "test_helpers.h"

/*   id  dept    salary  rating
 *   0   Sales   300     4.5
 *   1   IT      500     NA
 *   2   Sales   NA      3.0
 *   3   HR      300     4.5
 *   4   IT      200     1.0
 *   5   Sales   300     2.0
 */
static DataFrame *make_staff(void)
{
    int64_t ids[] = {0, 1, 2, 3, 4, 5};
    const char *depts[] = {"Sales", "IT", "Sales", "HR", "IT", "Sales"};
    int64_t salaries[] = {300, 500, MISSING_NUM, 300, 200, 300};
    double ratings[] = {4.5, MISSING_NUM, 3.0, 4.5, 1.0, 2.0};

    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, 6));
    add_column(df, make_string_column("dept", depts, 6));
    add_column(df, make_int_column("salary", salaries, 6));
    add_column(df, make_float_column("rating", ratings, 6));
    return df;
}

/* Checks that column "id" of df is exactly expected[0..n-1]. */
static int ids_are(const DataFrame *df, const int64_t *expected, size_t n)
{
    if (df_nrows(df) != n) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (cell_int(df, "id", i) != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static void test_sort_one_int_column(void)
{
    DataFrame *df = make_staff();
    DataFrame *out = NULL;

    /* ascending; equal salaries keep input order (stable); NA last */
    CHECK(df_sort_by_column(df, "salary", 1, &out) == DF_OK);
    int64_t asc[] = {4, 0, 3, 5, 1, 2};
    CHECK(ids_are(out, asc, 6));
    CHECK(cell_missing(out, "salary", 5) == 1);
    df_free(out);

    /* descending: NA is still last, ties still in input order */
    CHECK(df_sort_by_column(df, "salary", 0, &out) == DF_OK);
    int64_t desc[] = {1, 0, 3, 5, 4, 2};
    CHECK(ids_are(out, desc, 6));
    df_free(out);

    /* every column moved together with the sort key */
    CHECK(df_sort_by_column(df, "salary", 1, &out) == DF_OK);
    CHECK(same_string(cell_string(out, "dept", 0), "IT"));
    CHECK(close_to(cell_float(out, "rating", 0), 1.0));
    df_free(out);

    CHECK(cell_int(df, "id", 0) == 0);  /* input unchanged */
    df_free(df);
}

static void test_sort_float_and_string(void)
{
    DataFrame *df = make_staff();
    DataFrame *out = NULL;

    CHECK(df_sort_by_column(df, "rating", 1, &out) == DF_OK);
    int64_t by_rating[] = {4, 5, 2, 0, 3, 1};
    CHECK(ids_are(out, by_rating, 6));
    df_free(out);

    CHECK(df_sort_by_column(df, "dept", 1, &out) == DF_OK);
    int64_t by_dept[] = {3, 1, 4, 0, 2, 5};   /* HR, IT, IT, Sales x3 */
    CHECK(ids_are(out, by_dept, 6));
    df_free(out);
    df_free(df);
}

static void test_sort_many_columns(void)
{
    DataFrame *df = make_staff();
    DataFrame *out = NULL;

    /* dept ascending, then salary descending */
    const char *cols[] = {"dept", "salary"};
    int asc[] = {1, 0};
    CHECK(df_sort_by(df, cols, asc, 2, &out) == DF_OK);
    int64_t expected[] = {3, 1, 4, 0, 5, 2};
    CHECK(ids_are(out, expected, 6));
    df_free(out);

    /* salary, then rating, both ascending (rating breaks the 300 ties) */
    const char *cols2[] = {"salary", "rating"};
    int asc2[] = {1, 1};
    CHECK(df_sort_by(df, cols2, asc2, 2, &out) == DF_OK);
    int64_t expected2[] = {4, 5, 0, 3, 1, 2};
    CHECK(ids_are(out, expected2, 6));
    df_free(out);

    /* ascending = NULL means all ascending */
    CHECK(df_sort_by(df, cols, NULL, 2, &out) == DF_OK);
    int64_t expected3[] = {3, 4, 1, 0, 5, 2};
    CHECK(ids_are(out, expected3, 6));
    df_free(out);
    df_free(df);
}

static void test_sort_indices_and_compare(void)
{
    DataFrame *df = make_staff();
    size_t *rows = NULL;
    const char *cols[] = {"salary"};
    CHECK(df_sort_indices(df, cols, NULL, 1, &rows) == DF_OK);
    CHECK(rows != NULL);
    if (rows != NULL) {
        CHECK(rows[0] == 4 && rows[1] == 0 && rows[5] == 2);
    }
    free(rows);

    const Series *salary = NULL;
    const Series *dept = NULL;
    df_get_column(df, "salary", &salary);
    df_get_column(df, "dept", &dept);
    CHECK(series_compare_cells(salary, 4, salary, 0) < 0);   /* 200 < 300 */
    CHECK(series_compare_cells(salary, 0, salary, 3) == 0);  /* 300 == 300 */
    CHECK(series_compare_cells(salary, 2, salary, 1) > 0);   /* NA after */
    CHECK(series_compare_cells(salary, 2, salary, 2) == 0);  /* NA == NA */
    CHECK(series_compare_cells(dept, 3, dept, 1) < 0);       /* HR < IT */
    CHECK(series_compare_cells(dept, 0, salary, 0) == 0);    /* types differ */
    CHECK(series_compare_cells(salary, 99, salary, 0) == 0); /* out of range */
    CHECK(series_compare_cells(NULL, 0, salary, 0) == 0);
    df_free(df);
}

static void test_errors(void)
{
    DataFrame *df = make_staff();
    DataFrame *out = (DataFrame *)1;
    const char *cols[] = {"salary", "bonus"};
    const char *null_name[] = {NULL};
    size_t *rows = (size_t *)1;

    CHECK(df_sort_by(df, cols, NULL, 2, &out) == DF_ERR_NOT_FOUND);
    CHECK(out == NULL);
    CHECK(df_sort_by(df, cols, NULL, 0, &out) == DF_ERR_INVALID_ARG);
    CHECK(df_sort_by(df, NULL, NULL, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_sort_by(df, null_name, NULL, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_sort_by(NULL, cols, NULL, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_sort_by(df, cols, NULL, 1, NULL) == DF_ERR_NULL_ARG);
    CHECK(df_sort_by_column(df, NULL, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_sort_by_column(df, "nope", 1, &out) == DF_ERR_NOT_FOUND);
    CHECK(df_sort_indices(df, cols, NULL, 2, &rows) == DF_ERR_NOT_FOUND);
    CHECK(rows == NULL);
    CHECK(df_sort_indices(df, cols, NULL, 1, NULL) == DF_ERR_NULL_ARG);
    CHECK(out == NULL);
    df_free(df);
}

static void test_empty_and_one_row(void)
{
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", NULL, 0));
    DataFrame *out = NULL;
    CHECK(df_sort_by_column(df, "id", 1, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 1);
    df_free(out);

    size_t *rows = (size_t *)1;
    const char *cols[] = {"id"};
    CHECK(df_sort_indices(df, cols, NULL, 1, &rows) == DF_OK);
    CHECK(rows == NULL);    /* 0 rows: no array */
    df_free(df);

    int64_t one[] = {7};
    df = new_frame();
    add_column(df, make_int_column("id", one, 1));
    CHECK(df_sort_by_column(df, "id", 0, &out) == DF_OK);
    CHECK(df_nrows(out) == 1 && cell_int(out, "id", 0) == 7);
    df_free(out);
    df_free(df);
}

static void test_all_missing_column(void)
{
    int64_t ids[] = {0, 1, 2};
    int64_t vals[] = {MISSING_NUM, MISSING_NUM, MISSING_NUM};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, 3));
    add_column(df, make_int_column("v", vals, 3));
    DataFrame *out = NULL;
    CHECK(df_sort_by_column(df, "v", 0, &out) == DF_OK);
    CHECK(ids_are(out, ids, 3));    /* all equal: order unchanged */
    df_free(out);
    df_free(df);
}

static void test_large(void)
{
    enum { N = 100000 };
    int64_t *ids = malloc(N * sizeof(int64_t));
    int64_t *keys = malloc(N * sizeof(int64_t));
    CHECK(ids != NULL && keys != NULL);
    if (ids == NULL || keys == NULL) {
        free(ids);
        free(keys);
        return;
    }
    /* keys look random and have lots of repeats (0..999) */
    for (int i = 0; i < N; i++) {
        ids[i] = i;
        keys[i] = ((int64_t)i * 7919) % 1000;
    }
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, N));
    add_column(df, make_int_column("key", keys, N));
    free(ids);
    free(keys);

    DataFrame *out = NULL;
    CHECK(df_sort_by_column(df, "key", 1, &out) == DF_OK);
    CHECK(df_nrows(out) == N);

    /* keys must not go down; equal keys must have increasing ids (stable) */
    int sorted_ok = 1;
    int stable_ok = 1;
    int64_t id_sum = 0;
    for (size_t i = 0; i < N; i++) {
        id_sum += cell_int(out, "id", i);
        if (i > 0) {
            int64_t k0 = cell_int(out, "key", i - 1);
            int64_t k1 = cell_int(out, "key", i);
            if (k1 < k0) {
                sorted_ok = 0;
            }
            if (k1 == k0 && cell_int(out, "id", i) < cell_int(out, "id", i - 1)) {
                stable_ok = 0;
            }
        }
    }
    CHECK(sorted_ok);
    CHECK(stable_ok);
    CHECK(id_sum == (int64_t)N * (N - 1) / 2);  /* no row lost or repeated */
    df_free(out);
    df_free(df);
}

int main(void)
{
    test_sort_one_int_column();
    test_sort_float_and_string();
    test_sort_many_columns();
    test_sort_indices_and_compare();
    test_errors();
    test_empty_and_one_row();
    test_all_missing_column();
    test_large();
    return finish_tests();
}
