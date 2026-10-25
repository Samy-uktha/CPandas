/* Tests for dataframe.c. Build and run with `make test` (from the project
 * folder: the df_print test writes a temporary file into build/). */
#include <stdlib.h>

#include "dataframe.h"
#include "test_helpers.h"

/*   id  name  score
 *   1   a     1.5
 *   2   b     NA
 *   3   NA    3.5
 *   4   d     4.5
 */
static DataFrame *make_small(void)
{
    int64_t ids[] = {1, 2, 3, 4};
    const char *names[] = {"a", "b", NULL, "d"};
    double scores[] = {1.5, MISSING_NUM, 3.5, 4.5};
    DataFrame *df = new_frame();
    add_column(df, make_int_column("id", ids, 4));
    add_column(df, make_string_column("name", names, 4));
    add_column(df, make_float_column("score", scores, 4));
    return df;
}

static void test_create_and_free(void)
{
    DataFrame *df = NULL;
    CHECK(df_create(NULL) == DF_ERR_NULL_ARG);
    CHECK(df_create(&df) == DF_OK && df != NULL);
    CHECK(df_nrows(df) == 0 && df_ncols(df) == 0);
    df_free(df);
    df_free(NULL);                      /* must not crash */
    CHECK(df_nrows(NULL) == 0 && df_ncols(NULL) == 0);
}

static void test_add_column(void)
{
    DataFrame *df = new_frame();
    int64_t three[] = {1, 2, 3};
    int64_t two[] = {1, 2};

    CHECK(df_add_column(df, make_int_column("a", three, 3)) == DF_OK);
    CHECK(df_nrows(df) == 3 && df_ncols(df) == 1);  /* first column sets rows */

    /* wrong length: refused, and we still own the Series */
    Series *short_col = make_int_column("b", two, 2);
    CHECK(df_add_column(df, short_col) == DF_ERR_LENGTH_MISMATCH);
    CHECK(df_ncols(df) == 1);
    series_free(short_col);

    /* same name: refused */
    Series *same_name = make_int_column("a", three, 3);
    CHECK(df_add_column(df, same_name) == DF_ERR_DUPLICATE_NAME);
    series_free(same_name);

    CHECK(df_add_column(NULL, NULL) == DF_ERR_NULL_ARG);
    CHECK(df_add_column(df, NULL) == DF_ERR_NULL_ARG);
    CHECK(df_ncols(df) == 1 && df_nrows(df) == 3);  /* unchanged */
    df_free(df);

    /* zero-length columns in an empty frame are fine */
    df = new_frame();
    CHECK(df_add_column(df, make_int_column("x", NULL, 0)) == DF_OK);
    CHECK(df_add_column(df, make_string_column("y", NULL, 0)) == DF_OK);
    CHECK(df_ncols(df) == 2 && df_nrows(df) == 0);
    df_free(df);
}

static void test_lookups(void)
{
    DataFrame *df = make_small();
    size_t idx = 99;
    const Series *col = NULL;
    const char *name = NULL;

    CHECK(df_find_column(df, "name", &idx) == DF_OK && idx == 1);
    CHECK(df_find_column(df, "nope", &idx) == DF_ERR_NOT_FOUND);
    CHECK(df_find_column(df, NULL, &idx) == DF_ERR_NULL_ARG);

    CHECK(df_get_column(df, "score", &col) == DF_OK);
    CHECK(series_dtype(col) == DTYPE_FLOAT && series_len(col) == 4);
    CHECK(df_get_column(df, "nope", &col) == DF_ERR_NOT_FOUND && col == NULL);

    CHECK(df_get_column_at(df, 0, &col) == DF_OK);
    CHECK(same_string(series_name(col), "id"));
    CHECK(df_get_column_at(df, 3, &col) == DF_ERR_INDEX_OUT_OF_RANGE);
    CHECK(col == NULL);

    CHECK(df_column_name(df, 2, &name) == DF_OK && same_string(name, "score"));
    CHECK(df_column_name(df, 9, &name) == DF_ERR_INDEX_OUT_OF_RANGE);
    CHECK(name == NULL);
    CHECK(df_column_name(NULL, 0, &name) == DF_ERR_NULL_ARG);
    df_free(df);
}

static void test_remove_column(void)
{
    DataFrame *df = make_small();
    const char *name = NULL;

    CHECK(df_remove_column(df, "name") == DF_OK);
    CHECK(df_ncols(df) == 2 && df_nrows(df) == 4);
    df_column_name(df, 1, &name);
    CHECK(same_string(name, "score"));          /* later columns moved left */
    CHECK(close_to(cell_float(df, "score", 3), 4.5));

    CHECK(df_remove_column(df, "name") == DF_ERR_NOT_FOUND);
    CHECK(df_remove_column_at(df, 5) == DF_ERR_INDEX_OUT_OF_RANGE);
    CHECK(df_remove_column(NULL, "id") == DF_ERR_NULL_ARG);
    CHECK(df_remove_column(df, NULL) == DF_ERR_NULL_ARG);

    /* removing every column also resets the row count */
    CHECK(df_remove_column_at(df, 0) == DF_OK);
    CHECK(df_remove_column_at(df, 0) == DF_OK);
    CHECK(df_ncols(df) == 0 && df_nrows(df) == 0);

    /* now any length may be added again */
    int64_t seven[] = {1, 2, 3, 4, 5, 6, 7};
    CHECK(df_add_column(df, make_int_column("n", seven, 7)) == DF_OK);
    CHECK(df_nrows(df) == 7);
    df_free(df);
}

static void test_rename_column(void)
{
    DataFrame *df = make_small();
    const char *name = NULL;

    CHECK(df_rename_column(df, "score", "points") == DF_OK);
    df_column_name(df, 2, &name);
    CHECK(same_string(name, "points"));
    CHECK(close_to(cell_float(df, "points", 0), 1.5));

    CHECK(df_rename_column(df, "id", "id") == DF_OK);   /* own name is fine */
    CHECK(df_rename_column(df, "id", "name") == DF_ERR_DUPLICATE_NAME);
    CHECK(df_rename_column(df, "id", "") == DF_ERR_INVALID_ARG);
    CHECK(df_rename_column(df, "nope", "x") == DF_ERR_NOT_FOUND);
    CHECK(df_rename_column(df, "id", NULL) == DF_ERR_NULL_ARG);
    CHECK(df_rename_column(NULL, "id", "x") == DF_ERR_NULL_ARG);

    df_column_name(df, 0, &name);
    CHECK(same_string(name, "id"));             /* failures changed nothing */
    df_free(df);
}

static void test_clone(void)
{
    DataFrame *df = make_small();
    DataFrame *copy = NULL;
    CHECK(df_clone(df, &copy) == DF_OK);
    CHECK(df_nrows(copy) == 4 && df_ncols(copy) == 3);

    /* changing or freeing the original must not affect the copy */
    df_remove_column(df, "name");
    CHECK(df_ncols(copy) == 3);
    df_free(df);
    CHECK(same_string(cell_string(copy, "name", 1), "b"));
    CHECK(cell_missing(copy, "name", 2) == 1);
    CHECK(cell_missing(copy, "score", 1) == 1);
    df_free(copy);

    DataFrame *empty = new_frame();
    CHECK(df_clone(empty, &copy) == DF_OK && df_ncols(copy) == 0);
    df_free(copy);

    copy = (DataFrame *)1;
    CHECK(df_clone(NULL, &copy) == DF_ERR_NULL_ARG && copy == NULL);
    CHECK(df_clone(empty, NULL) == DF_ERR_NULL_ARG);
    df_free(empty);
}

static void test_select_columns(void)
{
    DataFrame *df = make_small();
    DataFrame *out = NULL;
    const char *name = NULL;

    const char *pick[] = {"score", "id"};
    CHECK(df_select_columns(df, pick, 2, &out) == DF_OK);
    CHECK(df_ncols(out) == 2 && df_nrows(out) == 4);
    df_column_name(out, 0, &name);
    CHECK(same_string(name, "score"));          /* in the order asked */
    df_free(df);                                /* out is a deep copy */
    CHECK(cell_int(out, "id", 3) == 4);
    df_free(out);

    df = make_small();
    const char *unknown[] = {"id", "nope"};
    const char *twice[] = {"id", "id"};
    const char *with_null[] = {"id", NULL};
    out = (DataFrame *)1;
    CHECK(df_select_columns(df, unknown, 2, &out) == DF_ERR_NOT_FOUND);
    CHECK(out == NULL);
    CHECK(df_select_columns(df, twice, 2, &out) == DF_ERR_DUPLICATE_NAME);
    CHECK(df_select_columns(df, with_null, 2, &out) == DF_ERR_NULL_ARG);
    CHECK(df_select_columns(df, NULL, 1, &out) == DF_ERR_NULL_ARG);

    /* selecting nothing gives an empty frame */
    CHECK(df_select_columns(df, NULL, 0, &out) == DF_OK);
    CHECK(df_ncols(out) == 0 && df_nrows(out) == 0);
    df_free(out);
    df_free(df);
}

static void test_drop_missing_rows(void)
{
    DataFrame *df = make_small();
    DataFrame *out = NULL;

    CHECK(df_drop_missing_rows(df, &out) == DF_OK);
    CHECK(df_nrows(out) == 2 && df_ncols(out) == 3);  /* rows 1 and 4 */
    CHECK(cell_int(out, "id", 0) == 1 && cell_int(out, "id", 1) == 4);
    df_free(out);
    CHECK(df_nrows(df) == 4);                   /* input unchanged */
    df_free(df);

    /* every row has a missing cell */
    int64_t gone[] = {MISSING_NUM, MISSING_NUM};
    df = new_frame();
    add_column(df, make_int_column("v", gone, 2));
    CHECK(df_drop_missing_rows(df, &out) == DF_OK && df_nrows(out) == 0);
    CHECK(df_ncols(out) == 1);
    df_free(out);
    df_free(df);

    /* empty frame */
    df = new_frame();
    CHECK(df_drop_missing_rows(df, &out) == DF_OK && df_ncols(out) == 0);
    df_free(out);
    CHECK(df_drop_missing_rows(NULL, &out) == DF_ERR_NULL_ARG);
    CHECK(df_drop_missing_rows(df, NULL) == DF_ERR_NULL_ARG);
    df_free(df);
}

static void test_make_sample(void)
{
    DataFrame *df = NULL;
    CHECK(df_make_sample(&df) == DF_OK);
    CHECK(df_ncols(df) == 3 && df_nrows(df) == 6);

    const Series *col = NULL;
    CHECK(df_get_column(df, "id", &col) == DF_OK && series_dtype(col) == DTYPE_INT);
    CHECK(df_get_column(df, "name", &col) == DF_OK && series_dtype(col) == DTYPE_STRING);
    CHECK(df_get_column(df, "score", &col) == DF_OK && series_dtype(col) == DTYPE_FLOAT);
    CHECK(series_count_missing(col) >= 1);      /* at least one missing cell */
    df_free(df);
    CHECK(df_make_sample(NULL) == DF_ERR_NULL_ARG);
}

/* Prints df into a temporary file and reads the text back into buf. */
static void print_to_text(const DataFrame *df, size_t max_rows, char *buf,
                          size_t size)
{
    const char *path = "build/test_dataframe_print.txt";
    buf[0] = '\0';
    FILE *f = fopen(path, "w+");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    CHECK(df_print(df, f, max_rows) == DF_OK);
    rewind(f);
    size_t got = fread(buf, 1, size - 1, f);
    buf[got] = '\0';
    fclose(f);
    remove(path);
}

static void test_print(void)
{
    DataFrame *df = make_small();
    char text[2000];

    print_to_text(df, 0, text, sizeof text);
    CHECK(strstr(text, "id") != NULL && strstr(text, "score") != NULL);
    CHECK(strstr(text, "NA") != NULL);          /* missing cells */
    CHECK(strstr(text, "4.5") != NULL);
    CHECK(strstr(text, "[4 rows x 3 columns]") != NULL);
    CHECK(strstr(text, "more rows") == NULL);

    print_to_text(df, 1, text, sizeof text);
    CHECK(strstr(text, "... (3 more rows)") != NULL);
    CHECK(strstr(text, "4.5") == NULL);         /* row 4 not shown */
    df_free(df);

    /* long text is cut short with "..." */
    const char *longer[] = {"abcdefghijklmnopqrstuvwxyz0123456789"};
    df = new_frame();
    add_column(df, make_string_column("t", longer, 1));
    print_to_text(df, 0, text, sizeof text);
    CHECK(strstr(text, "abcdefghijklmnopqrstu...") != NULL);
    CHECK(strstr(text, "0123456789") == NULL);
    df_free(df);

    df = new_frame();
    print_to_text(df, 0, text, sizeof text);
    CHECK(strstr(text, "[0 rows x 0 columns]") != NULL);
    CHECK(df_print(NULL, stdout, 0) == DF_ERR_NULL_ARG);
    CHECK(df_print(df, NULL, 0) == DF_ERR_NULL_ARG);
    df_free(df);
}

static void test_large(void)
{
    /* many columns: the column array has to grow many times */
    DataFrame *df = new_frame();
    int64_t one[] = {5};
    for (int c = 0; c < 1000; c++) {
        char name[32];
        snprintf(name, sizeof name, "c%d", c);
        add_column(df, make_int_column(name, one, 1));
    }
    CHECK(df_ncols(df) == 1000);
    CHECK(cell_int(df, "c999", 0) == 5);
    df_free(df);

    /* many rows */
    enum { N = 100000 };
    int64_t *vals = malloc(N * sizeof(int64_t));
    CHECK(vals != NULL);
    if (vals == NULL) return;
    for (int i = 0; i < N; i++) {
        vals[i] = (i % 10 == 0) ? MISSING_NUM : i;
    }
    df = new_frame();
    add_column(df, make_int_column("v", vals, N));
    free(vals);

    DataFrame *copy = NULL;
    DataFrame *clean = NULL;
    CHECK(df_clone(df, &copy) == DF_OK && df_nrows(copy) == N);
    CHECK(df_drop_missing_rows(df, &clean) == DF_OK);
    CHECK(df_nrows(clean) == N - N / 10);
    df_free(copy);
    df_free(clean);
    df_free(df);
}

int main(void)
{
    test_create_and_free();
    test_add_column();
    test_lookups();
    test_remove_column();
    test_rename_column();
    test_clone();
    test_select_columns();
    test_drop_missing_rows();
    test_make_sample();
    test_print();
    test_large();
    return finish_tests();
}
