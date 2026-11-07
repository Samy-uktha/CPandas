/* Tests for df_join.c. Build and run with `make test`. */
#include <stdlib.h>

#include "df_join.h"
#include "test_helpers.h"

/*  left:  id  name          right:  id  city
 *         1   A                     2   X
 *         2   B                     4   Y
 *         3   C                     4   Z
 *         4   D                     5   W
 *         NA  E                     NA  V
 */
static DataFrame *make_left(void)
{
    int64_t ids[] = {1, 2, 3, 4, MISSING_NUM};
    const char *names[] = {"A", "B", "C", "D", "E"};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, 5));
    add_column(df, make_string_column("name", names, 5));
    return df;
}

static DataFrame *make_right(void)
{
    int64_t ids[] = {2, 4, 4, 5, MISSING_NUM};
    const char *cities[] = {"X", "Y", "Z", "W", "V"};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, 5));
    add_column(df, make_string_column("city", cities, 5));
    return df;
}

/* Checks one result row: id (MISSING_NUM = missing), name, city
   (NULL = missing). */
static int row_is(const DataFrame *df, size_t row, int64_t id,
                  const char *name, const char *city)
{
    if (id == MISSING_NUM) {
        if (!cell_missing(df, "id", row)) return 0;
    } else if (cell_int(df, "id", row) != id) {
        return 0;
    }
    if (!same_string(cell_string(df, "name", row), name)) return 0;
    if (!same_string(cell_string(df, "city", row), city)) return 0;
    return 1;
}

static void test_inner(void)
{
    DataFrame *left = make_left();
    DataFrame *right = make_right();
    DataFrame *out = NULL;

    CHECK(df_merge(left, right, "id", DF_JOIN_INNER, &out) == DF_OK);
    CHECK(df_nrows(out) == 3 && df_ncols(out) == 3);
    CHECK(row_is(out, 0, 2, "B", "X"));
    CHECK(row_is(out, 1, 4, "D", "Y"));  /* one left row, two right rows */
    CHECK(row_is(out, 2, 4, "D", "Z"));

    const char *name = NULL;
    df_column_name(out, 0, &name);
    CHECK(same_string(name, "id"));
    df_column_name(out, 2, &name);
    CHECK(same_string(name, "city"));
    df_free(out);

    /* inputs unchanged */
    CHECK(df_nrows(left) == 5 && df_ncols(left) == 2);
    CHECK(df_nrows(right) == 5 && df_ncols(right) == 2);
    df_free(left);
    df_free(right);
}

static void test_left(void)
{
    DataFrame *left = make_left();
    DataFrame *right = make_right();
    DataFrame *out = NULL;

    CHECK(df_merge(left, right, "id", DF_JOIN_LEFT, &out) == DF_OK);
    CHECK(df_nrows(out) == 6);
    CHECK(row_is(out, 0, 1, "A", NULL));
    CHECK(row_is(out, 1, 2, "B", "X"));
    CHECK(row_is(out, 2, 3, "C", NULL));
    CHECK(row_is(out, 3, 4, "D", "Y"));
    CHECK(row_is(out, 4, 4, "D", "Z"));
    CHECK(row_is(out, 5, MISSING_NUM, "E", NULL));  /* NA key matches nothing */
    df_free(out);
    df_free(left);
    df_free(right);
}

static void test_right(void)
{
    DataFrame *left = make_left();
    DataFrame *right = make_right();
    DataFrame *out = NULL;

    CHECK(df_merge(left, right, "id", DF_JOIN_RIGHT, &out) == DF_OK);
    CHECK(df_nrows(out) == 5);
    CHECK(row_is(out, 0, 2, "B", "X"));
    CHECK(row_is(out, 1, 4, "D", "Y"));
    CHECK(row_is(out, 2, 4, "D", "Z"));
    CHECK(row_is(out, 3, 5, NULL, "W"));    /* key 5 comes from the right */
    CHECK(row_is(out, 4, MISSING_NUM, NULL, "V"));
    df_free(out);
    df_free(left);
    df_free(right);
}

static void test_outer(void)
{
    DataFrame *left = make_left();
    DataFrame *right = make_right();
    DataFrame *out = NULL;

    CHECK(df_merge(left, right, "id", DF_JOIN_OUTER, &out) == DF_OK);
    CHECK(df_nrows(out) == 8);
    CHECK(row_is(out, 0, 1, "A", NULL));
    CHECK(row_is(out, 4, 4, "D", "Z"));
    CHECK(row_is(out, 5, MISSING_NUM, "E", NULL));
    CHECK(row_is(out, 6, 5, NULL, "W"));    /* unmatched right rows at the end */
    CHECK(row_is(out, 7, MISSING_NUM, NULL, "V"));
    df_free(out);
    df_free(left);
    df_free(right);
}

static void test_string_key_and_same_names(void)
{
    const char *lk[] = {"ann", "bob"};
    double ls[] = {1.0, 2.0};
    const char *rk[] = {"bob", "ann"};
    double rs[] = {20.0, 10.0};
    int64_t extra[] = {7, 8};

    DataFrame *left = new_frame();
    add_column(left, make_string_column("who", lk, 2));
    add_column(left, make_float_column("score", ls, 2));
    DataFrame *right = new_frame();
    add_column(right, make_string_column("who", rk, 2));
    add_column(right, make_float_column("score", rs, 2));
    add_column(right, make_int_column("extra", extra, 2));

    DataFrame *out = NULL;
    CHECK(df_merge(left, right, "who", DF_JOIN_INNER, &out) == DF_OK);
    CHECK(df_nrows(out) == 2 && df_ncols(out) == 4);
    const char *name = NULL;
    df_column_name(out, 1, &name);
    CHECK(same_string(name, "score_left"));
    df_column_name(out, 2, &name);
    CHECK(same_string(name, "score_right"));
    df_column_name(out, 3, &name);
    CHECK(same_string(name, "extra"));
    CHECK(same_string(cell_string(out, "who", 0), "ann"));
    CHECK(close_to(cell_float(out, "score_left", 0), 1.0));
    CHECK(close_to(cell_float(out, "score_right", 0), 10.0));
    CHECK(cell_int(out, "extra", 0) == 8);
    df_free(out);

    /* the original frames keep their names */
    const Series *col = NULL;
    CHECK(df_get_column(left, "score", &col) == DF_OK);
    df_free(left);
    df_free(right);
}

static void test_self_join(void)
{
    DataFrame *df = make_left();
    DataFrame *out = NULL;
    CHECK(df_merge(df, df, "id", DF_JOIN_INNER, &out) == DF_OK);
    CHECK(df_nrows(out) == 4);      /* the NA key matches nothing */
    CHECK(df_ncols(out) == 3);      /* id, name_left, name_right */
    CHECK(same_string(cell_string(out, "name_left", 3), "D"));
    CHECK(same_string(cell_string(out, "name_right", 3), "D"));
    df_free(out);
    df_free(df);
}

static void test_errors(void)
{
    DataFrame *left = make_left();
    DataFrame *right = make_right();
    DataFrame *out = (DataFrame *)1;

    CHECK(df_merge(left, right, "city", DF_JOIN_INNER, &out) == DF_ERR_NOT_FOUND);
    CHECK(out == NULL);
    CHECK(df_merge(left, right, "name", DF_JOIN_INNER, &out) == DF_ERR_NOT_FOUND);
    CHECK(df_merge(left, right, "id", (df_join_type)9, &out) == DF_ERR_INVALID_ARG);
    CHECK(df_merge(NULL, right, "id", DF_JOIN_INNER, &out) == DF_ERR_NULL_ARG);
    CHECK(df_merge(left, NULL, "id", DF_JOIN_INNER, &out) == DF_ERR_NULL_ARG);
    CHECK(df_merge(left, right, NULL, DF_JOIN_INNER, &out) == DF_ERR_NULL_ARG);
    CHECK(df_merge(left, right, "id", DF_JOIN_INNER, NULL) == DF_ERR_NULL_ARG);

    /* key columns of different types */
    const char *text_ids[] = {"1", "2"};
    DataFrame *text = new_frame();
    add_column(text, make_string_column("id", text_ids, 2));
    CHECK(df_merge(left, text, "id", DF_JOIN_INNER, &out) == DF_ERR_TYPE_MISMATCH);
    df_free(text);

    /* renaming "v" to "v_left" clashes with an existing "v_left" column.
       This fails half way through building the result: nothing may leak. */
    int64_t ids[] = {1};
    int64_t vals[] = {5};
    DataFrame *a = new_frame();
    add_column(a, make_int_column("id", ids, 1));
    add_column(a, make_int_column("v", vals, 1));
    add_column(a, make_int_column("v_left", vals, 1));
    DataFrame *b = new_frame();
    add_column(b, make_int_column("id", ids, 1));
    add_column(b, make_int_column("v", vals, 1));
    CHECK(df_merge(a, b, "id", DF_JOIN_INNER, &out) == DF_ERR_DUPLICATE_NAME);
    CHECK(out == NULL);
    df_free(a);
    df_free(b);

    df_free(left);
    df_free(right);
}

static void test_empty_side(void)
{
    DataFrame *left = make_left();
    DataFrame *right = new_frame();
    add_column(right, make_int_column("id", NULL, 0));
    add_column(right, make_string_column("city", NULL, 0));
    DataFrame *out = NULL;

    CHECK(df_merge(left, right, "id", DF_JOIN_INNER, &out) == DF_OK);
    CHECK(df_nrows(out) == 0 && df_ncols(out) == 3);
    df_free(out);

    CHECK(df_merge(left, right, "id", DF_JOIN_LEFT, &out) == DF_OK);
    CHECK(df_nrows(out) == 5);
    CHECK(row_is(out, 0, 1, "A", NULL));
    df_free(out);

    CHECK(df_merge(left, right, "id", DF_JOIN_RIGHT, &out) == DF_OK);
    CHECK(df_nrows(out) == 0);
    df_free(out);

    CHECK(df_merge(right, left, "id", DF_JOIN_OUTER, &out) == DF_OK);
    CHECK(df_nrows(out) == 5);
    CHECK(cell_int(out, "id", 0) == 1);     /* key filled from the right */
    CHECK(same_string(cell_string(out, "name", 0), "A"));
    df_free(out);

    df_free(left);
    df_free(right);
}

static void test_large(void)
{
    /* left ids 0..N-1, right ids N-1..0 with val = 2 * id */
    enum { N = 2000 };
    int64_t *lids = malloc(N * sizeof(int64_t));
    int64_t *rids = malloc(N * sizeof(int64_t));
    int64_t *rvals = malloc(N * sizeof(int64_t));
    CHECK(lids != NULL && rids != NULL && rvals != NULL);
    if (lids == NULL || rids == NULL || rvals == NULL) {
        free(lids);
        free(rids);
        free(rvals);
        return;
    }
    for (int i = 0; i < N; i++) {
        lids[i] = i;
        rids[i] = N - 1 - i;
        rvals[i] = 2 * (N - 1 - i);
    }
    DataFrame *left = new_frame();
    add_column(left, make_int_column("id", lids, N));
    DataFrame *right = new_frame();
    add_column(right, make_int_column("id", rids, N));
    add_column(right, make_int_column("val", rvals, N));
    free(lids);
    free(rids);
    free(rvals);

    DataFrame *out = NULL;
    CHECK(df_merge(left, right, "id", DF_JOIN_INNER, &out) == DF_OK);
    CHECK(df_nrows(out) == N);
    int all_ok = 1;
    for (int i = 0; i < N; i++) {
        if (cell_int(out, "id", i) != i || cell_int(out, "val", i) != 2 * i) {
            all_ok = 0;
        }
    }
    CHECK(all_ok);
    df_free(out);
    df_free(left);
    df_free(right);
}

int main(void)
{
    test_inner();
    test_left();
    test_right();
    test_outer();
    test_string_key_and_same_names();
    test_self_join();
    test_errors();
    test_empty_side();
    test_large();
    return finish_tests();
}
