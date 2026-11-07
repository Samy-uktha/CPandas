/* Tests for df_concat.c. Build and run with `make test`. */
#include <stdlib.h>

#include "df_concat.h"
#include "test_helpers.h"

/*  a:  id  name  score          b:  name  id  city
 *      1   A     1.5                C     3   Pune
 *      2   B     NA                 D     4   NA
 */
static DataFrame *make_a(void)
{
    int64_t ids[] = {1, 2};
    const char *names[] = {"A", "B"};
    double scores[] = {1.5, MISSING_NUM};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, 2));
    add_column(df, make_string_column("name", names, 2));
    add_column(df, make_float_column("score", scores, 2));
    return df;
}

static DataFrame *make_b(void)
{
    const char *names[] = {"C", "D"};
    int64_t ids[] = {3, 4};
    const char *cities[] = {"Pune", NULL};
    DataFrame *df = new_frame();
    add_column(df, make_string_column("name", names, 2));
    add_column(df, make_int_column("id", ids, 2));
    add_column(df, make_string_column("city", cities, 2));
    return df;
}

static void test_by_name_and_new_columns(void)
{
    DataFrame *a = make_a();
    DataFrame *b = make_b();
    const DataFrame *parts[] = {a, b};
    DataFrame *out = NULL;
    CHECK(df_concat(parts, 2, &out) == DF_OK);
    CHECK(df_nrows(out) == 4);
    CHECK(df_ncols(out) == 4);          /* id name score city */

    const char *name = NULL;
    df_column_name(out, 0, &name);
    CHECK(same_string(name, "id"));
    df_column_name(out, 3, &name);
    CHECK(same_string(name, "city"));

    CHECK(cell_int(out, "id", 2) == 3);             /* matched by name */
    CHECK(same_string(cell_string(out, "name", 3), "D"));
    CHECK(cell_missing(out, "score", 1));           /* missing in a */
    CHECK(cell_missing(out, "score", 2));           /* b has no score */
    CHECK(cell_missing(out, "city", 0));            /* a has no city */
    CHECK(same_string(cell_string(out, "city", 2), "Pune"));

    const Series *id = NULL;
    df_get_column(out, "id", &id);
    CHECK(series_dtype(id) == DTYPE_INT);           /* stays INT */
    df_free(out);

    /* the same frame twice, and a frame with no columns in between */
    DataFrame *empty = new_frame();
    const DataFrame *again[] = {a, empty, a};
    CHECK(df_concat(again, 3, &out) == DF_OK);
    CHECK(df_nrows(out) == 4 && df_ncols(out) == 3);
    CHECK(cell_int(out, "id", 3) == 2);
    df_free(out);

    /* only empty frames: an empty result */
    const DataFrame *nothing[] = {empty};
    CHECK(df_concat(nothing, 1, &out) == DF_OK);
    CHECK(df_ncols(out) == 0 && df_nrows(out) == 0);
    df_free(out);

    df_free(empty);
    df_free(a);
    df_free(b);
}

static void test_types(void)
{
    /* INT in one frame and FLOAT in the other becomes FLOAT */
    int64_t ints[] = {1, MISSING_NUM};
    double floats[] = {2.5};
    DataFrame *x = new_frame();
    add_column(x, make_int_column("v", ints, 2));
    DataFrame *y = new_frame();
    add_column(y, make_float_column("v", floats, 1));
    const DataFrame *parts[] = {x, y};
    DataFrame *out = NULL;
    CHECK(df_concat(parts, 2, &out) == DF_OK);
    const Series *v = NULL;
    df_get_column(out, "v", &v);
    CHECK(series_dtype(v) == DTYPE_FLOAT);
    CHECK(cell_float(out, "v", 0) == 1.0);
    CHECK(cell_missing(out, "v", 1));
    CHECK(cell_float(out, "v", 2) == 2.5);
    df_free(out);

    /* INT and STRING do not go together */
    const char *words[] = {"a"};
    DataFrame *z = new_frame();
    add_column(z, make_string_column("v", words, 1));
    const DataFrame *bad[] = {x, z};
    out = (DataFrame *)1;
    CHECK(df_concat(bad, 2, &out) == DF_ERR_TYPE_MISMATCH);
    CHECK(out == NULL);

    df_free(x);
    df_free(y);
    df_free(z);
}

static void test_errors(void)
{
    DataFrame *a = make_a();
    const DataFrame *parts[] = {a, NULL};
    DataFrame *out = NULL;
    CHECK(df_concat(parts, 0, &out) == DF_ERR_INVALID_ARG);
    CHECK(df_concat(parts, 2, &out) == DF_ERR_NULL_ARG);
    CHECK(df_concat(NULL, 1, &out) == DF_ERR_NULL_ARG);
    CHECK(df_concat(parts, 1, NULL) == DF_ERR_NULL_ARG);
    df_free(a);
}

static void test_large(void)
{
    enum { N = 20000 };
    int64_t *ids = malloc(N * sizeof(int64_t));
    CHECK(ids != NULL);
    if (ids == NULL) return;
    for (int i = 0; i < N; i++) {
        ids[i] = i;
    }
    DataFrame *a = new_frame();
    add_column(a, make_int_column("id", ids, N));
    const DataFrame *parts[] = {a, a, a};
    DataFrame *out = NULL;
    CHECK(df_concat(parts, 3, &out) == DF_OK);
    CHECK(df_nrows(out) == 3 * N);
    CHECK(cell_int(out, "id", 2 * N + 7) == 7);
    df_free(out);
    df_free(a);
    free(ids);
}

int main(void)
{
    test_by_name_and_new_columns();
    test_types();
    test_errors();
    test_large();
    return finish_tests();
}
