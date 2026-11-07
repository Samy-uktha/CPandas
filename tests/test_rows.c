/* Tests for df_rows.c (head, tail, slice, take rows).
 * Build and run with `make test`. */
#include <stdlib.h>

#include "df_rows.h"
#include "test_helpers.h"

/* id: 0..9, name: "n0".."n9", score: id * 1.5, with score missing in row 3 */
static DataFrame *make_ten_rows(void)
{
    int64_t ids[10];
    double scores[10];
    const char *names[10] = {"n0", "n1", "n2", "n3", "n4",
                             "n5", "n6", "n7", "n8", "n9"};
    for (int i = 0; i < 10; i++) {
        ids[i] = i;
        scores[i] = i * 1.5;
    }
    scores[3] = MISSING_NUM;

    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, 10));
    add_column(df, make_string_column("name", names, 10));
    add_column(df, make_float_column("score", scores, 10));
    return df;
}

static void test_head(void)
{
    DataFrame *df = make_ten_rows();
    DataFrame *out = NULL;

    CHECK(df_head(df, 3, &out) == DF_OK);
    CHECK(df_nrows(out) == 3 && df_ncols(out) == 3);
    CHECK(cell_int(out, "id", 0) == 0 && cell_int(out, "id", 2) == 2);
    CHECK(same_string(cell_string(out, "name", 1), "n1"));
    df_free(out);

    /* boundary: 0 rows, exactly all rows, more than all rows */
    CHECK(df_head(df, 0, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 3);
    df_free(out);

    CHECK(df_head(df, 10, &out) == DF_OK && df_nrows(out) == 10);
    df_free(out);

    CHECK(df_head(df, 1000, &out) == DF_OK && df_nrows(out) == 10);
    CHECK(cell_int(out, "id", 9) == 9);
    df_free(out);

    CHECK(df_nrows(df) == 10);          /* the input is not changed */
    df_free(df);
}

static void test_tail(void)
{
    DataFrame *df = make_ten_rows();
    DataFrame *out = NULL;

    CHECK(df_tail(df, 3, &out) == DF_OK);
    CHECK(df_nrows(out) == 3);
    CHECK(cell_int(out, "id", 0) == 7 && cell_int(out, "id", 2) == 9);
    df_free(out);

    CHECK(df_tail(df, 0, &out) == DF_OK && df_nrows(out) == 0);
    df_free(out);

    CHECK(df_tail(df, 50, &out) == DF_OK && df_nrows(out) == 10);
    CHECK(cell_int(out, "id", 0) == 0);
    df_free(out);
    df_free(df);
}

static void test_slice(void)
{
    DataFrame *df = make_ten_rows();
    DataFrame *out = NULL;

    CHECK(df_slice(df, 2, 5, &out) == DF_OK);
    CHECK(df_nrows(out) == 3);
    CHECK(cell_int(out, "id", 0) == 2 && cell_int(out, "id", 2) == 4);
    CHECK(cell_missing(out, "score", 1) == 1);  /* row 3 had a missing score */
    CHECK(close_to(cell_float(out, "score", 2), 6.0));
    df_free(out);

    /* end past the last row is cut back */
    CHECK(df_slice(df, 8, 100, &out) == DF_OK && df_nrows(out) == 2);
    CHECK(cell_int(out, "id", 1) == 9);
    df_free(out);

    /* start >= end, and start past the end, give 0 rows (not an error) */
    CHECK(df_slice(df, 5, 2, &out) == DF_OK && df_nrows(out) == 0);
    CHECK(df_ncols(out) == 3);
    df_free(out);
    CHECK(df_slice(df, 4, 4, &out) == DF_OK && df_nrows(out) == 0);
    df_free(out);
    CHECK(df_slice(df, 100, 200, &out) == DF_OK && df_nrows(out) == 0);
    df_free(out);
    df_free(df);
}

static void test_take_rows(void)
{
    DataFrame *df = make_ten_rows();
    DataFrame *out = NULL;

    size_t rows[] = {9, 0, 0, 3};
    CHECK(df_take_rows(df, rows, 4, &out) == DF_OK);
    CHECK(df_nrows(out) == 4);
    CHECK(cell_int(out, "id", 0) == 9);
    CHECK(cell_int(out, "id", 1) == 0 && cell_int(out, "id", 2) == 0);
    CHECK(cell_missing(out, "score", 3) == 1);
    df_free(out);

    /* an index that is too big fails and gives NULL */
    size_t bad[] = {1, 10};
    out = NULL;
    CHECK(df_take_rows(df, bad, 2, &out) == DF_ERR_INDEX_OUT_OF_RANGE);
    CHECK(out == NULL);
    size_t sentinel[] = {SERIES_TAKE_MISSING};
    CHECK(df_take_rows(df, sentinel, 1, &out) == DF_ERR_INDEX_OUT_OF_RANGE);

    /* n = 0 with NULL is fine, n > 0 with NULL is not */
    CHECK(df_take_rows(df, NULL, 0, &out) == DF_OK && df_nrows(out) == 0);
    df_free(out);
    CHECK(df_take_rows(df, NULL, 2, &out) == DF_ERR_NULL_ARG);
    df_free(df);
}

static void test_result_is_a_deep_copy(void)
{
    DataFrame *df = make_ten_rows();
    DataFrame *out = NULL;
    CHECK(df_head(df, 2, &out) == DF_OK);

    /* free the input first: the result must still be readable (ASan would
       report a use-after-free if memory was shared) */
    df_free(df);
    CHECK(same_string(cell_string(out, "name", 1), "n1"));
    df_free(out);
}

static void test_null_and_empty(void)
{
    DataFrame *out = (DataFrame *)1;    /* must be set to NULL on failure */
    CHECK(df_head(NULL, 3, &out) == DF_ERR_NULL_ARG && out == NULL);
    CHECK(df_tail(NULL, 3, &out) == DF_ERR_NULL_ARG);
    CHECK(df_slice(NULL, 0, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_take_rows(NULL, NULL, 0, &out) == DF_ERR_NULL_ARG);

    DataFrame *empty = new_frame();     /* 0 columns, 0 rows */
    CHECK(df_head(empty, 3, NULL) == DF_ERR_NULL_ARG);
    CHECK(df_head(empty, 3, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 0);
    df_free(out);
    CHECK(df_tail(empty, 3, &out) == DF_OK && df_nrows(out) == 0);
    df_free(out);
    df_free(empty);
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
    CHECK(df_tail(df, 5, &out) == DF_OK && df_nrows(out) == 5);
    CHECK(cell_int(out, "id", 0) == N - 5);
    df_free(out);

    CHECK(df_slice(df, 50000, 60000, &out) == DF_OK);
    CHECK(df_nrows(out) == 10000 && cell_int(out, "id", 9999) == 59999);
    df_free(out);
    df_free(df);
}

int main(void)
{
    test_head();
    test_tail();
    test_slice();
    test_take_rows();
    test_result_is_a_deep_copy();
    test_null_and_empty();
    test_large();
    return finish_tests();
}
