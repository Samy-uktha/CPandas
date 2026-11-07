/* Tests for csv.c. Build and run with `make test` (from the project folder:
 * the file tests write temporary files into build/). */
#include <stdlib.h>

#include "csv.h"
#include "test_helpers.h"

#define TMP_FILE "build/test_csv_tmp.csv"

/* Reads text with the default options. */
static DataFrame *read_text(const char *text)
{
    DataFrame *df = NULL;
    CHECK(df_read_csv_string(text, NULL, &df) == DF_OK);
    return df;
}

static df_dtype type_of(const DataFrame *df, const char *col)
{
    const Series *s = NULL;
    df_get_column(df, col, &s);
    return series_dtype(s);
}

static const char *name_of(const DataFrame *df, size_t idx)
{
    const char *name = NULL;
    df_column_name(df, idx, &name);
    return name;
}

/* 1 if both frames have the same columns, types, missing cells and values. */
static int frames_equal(const DataFrame *a, const DataFrame *b)
{
    if (df_ncols(a) != df_ncols(b) || df_nrows(a) != df_nrows(b)) {
        return 0;
    }
    for (size_t c = 0; c < df_ncols(a); c++) {
        const Series *x = NULL;
        const Series *y = NULL;
        df_get_column_at(a, c, &x);
        df_get_column_at(b, c, &y);
        if (!same_string(series_name(x), series_name(y)) ||
            series_dtype(x) != series_dtype(y)) {
            return 0;
        }
        for (size_t r = 0; r < series_len(x); r++) {
            if (series_is_missing(x, r) != series_is_missing(y, r)) {
                return 0;
            }
            if (series_is_missing(x, r)) {
                continue;
            }
            if (series_dtype(x) == DTYPE_INT) {
                int64_t u = 0, v = 0;
                series_get_int(x, r, &u);
                series_get_int(y, r, &v);
                if (u != v) return 0;
            } else if (series_dtype(x) == DTYPE_FLOAT) {
                double u = 0, v = 0;
                series_get_float(x, r, &u);
                series_get_float(y, r, &v);
                if (u != v) return 0;   /* must round-trip exactly */
            } else {
                const char *u = NULL, *v = NULL;
                series_get_string(x, r, &u);
                series_get_string(y, r, &v);
                if (!same_string(u, v)) return 0;
            }
        }
    }
    return 1;
}

static void test_basic_read(void)
{
    DataFrame *df = read_text("id,name,score\n1,Asha,88.5\n2,Ben,\n3,,72\n");
    CHECK(df_ncols(df) == 3 && df_nrows(df) == 3);
    CHECK(same_string(name_of(df, 0), "id"));
    CHECK(same_string(name_of(df, 2), "score"));
    CHECK(type_of(df, "id") == DTYPE_INT);
    CHECK(type_of(df, "name") == DTYPE_STRING);
    CHECK(type_of(df, "score") == DTYPE_FLOAT);     /* 88.5 and 72 */
    CHECK(cell_int(df, "id", 2) == 3);
    CHECK(same_string(cell_string(df, "name", 0), "Asha"));
    CHECK(cell_missing(df, "name", 2) == 1);
    CHECK(cell_missing(df, "score", 1) == 1);
    CHECK(close_to(cell_float(df, "score", 2), 72.0));
    df_free(df);
}

static void test_type_inference(void)
{
    DataFrame *df = read_text(
        "a,b,c,d,e,f,g,h\n"
        "1,1,007,1e3,nan,0x1F,,-5\n"
        "2,2.5,-3,.5,1,2,,+6\n"
        "99999999999999999999,3,4,2.,x,3,,7\n");
    CHECK(type_of(df, "a") == DTYPE_FLOAT);   /* too big for int64 */
    CHECK(type_of(df, "b") == DTYPE_FLOAT);   /* ints and a decimal */
    CHECK(type_of(df, "c") == DTYPE_INT);
    CHECK(cell_int(df, "c", 0) == 7);
    CHECK(type_of(df, "d") == DTYPE_FLOAT);
    CHECK(close_to(cell_float(df, "d", 0), 1000.0));
    CHECK(type_of(df, "e") == DTYPE_STRING);  /* "nan" is text */
    CHECK(type_of(df, "f") == DTYPE_STRING);  /* hex is text */
    CHECK(type_of(df, "g") == DTYPE_STRING);  /* all missing */
    CHECK(type_of(df, "h") == DTYPE_INT);
    CHECK(cell_int(df, "h", 1) == 6);
    df_free(df);

    /* largest and smallest int64 still fit */
    df = read_text("v\n9223372036854775807\n-9223372036854775808\n");
    CHECK(type_of(df, "v") == DTYPE_INT);
    CHECK(cell_int(df, "v", 0) == INT64_MAX);
    CHECK(cell_int(df, "v", 1) == INT64_MIN);
    df_free(df);

    /* a number too big even for double becomes text */
    df = read_text("v\n1e999\n");
    CHECK(type_of(df, "v") == DTYPE_STRING);
    df_free(df);
}

static void test_spaces_are_trimmed(void)
{
    DataFrame *df = read_text("id , name\n 1 ,  Asha  \n2,\" x \"\n");
    CHECK(same_string(name_of(df, 0), "id"));
    CHECK(same_string(name_of(df, 1), "name"));
    CHECK(type_of(df, "id") == DTYPE_INT);
    CHECK(cell_int(df, "id", 0) == 1);
    CHECK(same_string(cell_string(df, "name", 0), "Asha"));
    CHECK(same_string(cell_string(df, "name", 1), " x "));  /* quoted: kept */
    df_free(df);
}

static void test_quotes(void)
{
    DataFrame *df = read_text(
        "name,quote\n"
        "\"Smith, John\",\"He said \"\"hi\"\"\"\n"
        "\"two\nlines\",\"\"\n");
    CHECK(df_nrows(df) == 2);
    CHECK(same_string(cell_string(df, "name", 0), "Smith, John"));
    CHECK(same_string(cell_string(df, "quote", 0), "He said \"hi\""));
    CHECK(same_string(cell_string(df, "name", 1), "two\nlines"));
    CHECK(cell_missing(df, "quote", 1) == 1);   /* "" is missing */
    df_free(df);

    /* a quote in the middle of an unquoted field is kept as it is */
    df = read_text("a\n5\"6\n");
    CHECK(same_string(cell_string(df, "a", 0), "5\"6"));
    df_free(df);
}

static void test_na_values(void)
{
    const char *words[] = {"NA", "null"};
    csv_options opts = csv_default_options();
    opts.na_values = words;
    opts.n_na_values = 2;

    DataFrame *df = NULL;
    CHECK(df_read_csv_string("NA,b\n1,null\nNA,x\n3,\n", &opts, &df) == DF_OK);
    CHECK(same_string(name_of(df, 0), "NA"));   /* header is not affected */
    CHECK(type_of(df, "NA") == DTYPE_INT);      /* "NA" cell is missing */
    CHECK(cell_missing(df, "NA", 1) == 1);
    CHECK(cell_missing(df, "b", 0) == 1);
    CHECK(cell_missing(df, "b", 2) == 1);
    CHECK(same_string(cell_string(df, "b", 1), "x"));
    df_free(df);

    /* without the option, "NA" is ordinary text */
    df = read_text("v\nNA\n");
    CHECK(same_string(cell_string(df, "v", 0), "NA"));
    df_free(df);
}

static void test_line_endings_and_blank_lines(void)
{
    DataFrame *df = read_text("a,b\r\n1,2\r\n\r\n   \r\n3,4");  /* no final \n */
    CHECK(df_nrows(df) == 2 && cell_int(df, "b", 1) == 4);
    df_free(df);

    df = read_text("a,b\r1,2\r3,4\r");                       /* old Mac */
    CHECK(df_nrows(df) == 2 && cell_int(df, "a", 1) == 3);
    df_free(df);

    df = read_text("\xEF\xBB\xBFid\n5\n");                    /* Excel BOM */
    CHECK(same_string(name_of(df, 0), "id") && cell_int(df, "id", 0) == 5);
    df_free(df);
}

static void test_options(void)
{
    csv_options opts = csv_default_options();
    CHECK(opts.delimiter == ',' && opts.has_header == 1);
    CHECK(opts.na_values == NULL && opts.n_na_values == 0);

    /* no header: columns are col0, col1 and the first line is data */
    opts.has_header = 0;
    DataFrame *df = NULL;
    CHECK(df_read_csv_string("1,x\n2,y\n", &opts, &df) == DF_OK);
    CHECK(df_nrows(df) == 2);
    CHECK(same_string(name_of(df, 0), "col0"));
    CHECK(same_string(name_of(df, 1), "col1"));
    df_free(df);

    /* other delimiters */
    opts = csv_default_options();
    opts.delimiter = ';';
    CHECK(df_read_csv_string("a;b\n1,5;x\n", &opts, &df) == DF_OK);
    CHECK(same_string(cell_string(df, "a", 0), "1,5"));
    df_free(df);

    /* with tabs as the delimiter, a tab is never trimmed away */
    opts.delimiter = '\t';
    CHECK(df_read_csv_string("a\tb\n 1 \t\n2\tx\n", &opts, &df) == DF_OK);
    CHECK(df_ncols(df) == 2 && df_nrows(df) == 2);
    CHECK(type_of(df, "a") == DTYPE_INT && cell_int(df, "a", 0) == 1);
    CHECK(cell_missing(df, "b", 0) == 1);
    CHECK(same_string(cell_string(df, "b", 1), "x"));
    df_free(df);
}

static void test_errors(void)
{
    DataFrame *df = (DataFrame *)1;
    CHECK(df_read_csv_string("a,b\n1,2,3\n", NULL, &df) == DF_ERR_PARSE);
    CHECK(df == NULL);
    CHECK(df_read_csv_string("a,b\n1\n", NULL, &df) == DF_ERR_PARSE);
    CHECK(df_read_csv_string("a\n\"never closed\n", NULL, &df) == DF_ERR_PARSE);
    CHECK(df_read_csv_string("a\n\"ab\"c\n", NULL, &df) == DF_ERR_PARSE);
    CHECK(df_read_csv_string("a,a\n1,2\n", NULL, &df) == DF_ERR_DUPLICATE_NAME);
    CHECK(df == NULL);

    csv_options opts = csv_default_options();
    opts.delimiter = '"';
    CHECK(df_read_csv_string("a\n", &opts, &df) == DF_ERR_INVALID_ARG);
    opts.delimiter = '\n';
    CHECK(df_read_csv_string("a\n", &opts, &df) == DF_ERR_INVALID_ARG);
    opts = csv_default_options();
    opts.n_na_values = 1;       /* but na_values is NULL */
    CHECK(df_read_csv_string("a\n", &opts, &df) == DF_ERR_NULL_ARG);

    CHECK(df_read_csv_string(NULL, NULL, &df) == DF_ERR_NULL_ARG);
    CHECK(df_read_csv_string("a\n", NULL, NULL) == DF_ERR_NULL_ARG);
    CHECK(df_read_csv("no_such_file_here.csv", NULL, &df) == DF_ERR_IO);
    CHECK(df == NULL);
    CHECK(df_read_csv(NULL, NULL, &df) == DF_ERR_NULL_ARG);
}

static void test_empty_input(void)
{
    DataFrame *df = read_text("");
    CHECK(df_ncols(df) == 0 && df_nrows(df) == 0);
    df_free(df);

    df = read_text("\n\n  \n");
    CHECK(df_ncols(df) == 0);
    df_free(df);

    /* only a header: columns with 0 rows, all text */
    df = read_text("a,b\n");
    CHECK(df_ncols(df) == 2 && df_nrows(df) == 0);
    CHECK(type_of(df, "a") == DTYPE_STRING);
    df_free(df);

    /* an empty header cell gets a made-up name */
    df = read_text("a,,c\n1,2,3\n");
    CHECK(same_string(name_of(df, 1), "col1"));
    df_free(df);
}

static void test_write_and_read_back(void)
{
    const char *texts[] = {"plain", "has, comma", "has \"quotes\"",
                           "two\nlines", " spaces ", NULL, "NA"};
    int64_t ints[] = {1, -2, 0, MISSING_NUM, 9223372036854775807LL, 5, 6};
    double floats[] = {72.0, 0.1, -0.5, 1e300, MISSING_NUM, 1.0 / 3.0, 2.5e-8};

    DataFrame *df = new_frame();
    add_column(df, make_string_column("text", texts, 7));
    add_column(df, make_int_column("number", ints, 7));
    add_column(df, make_float_column("real, value", floats, 7));

    CHECK(df_write_csv(df, TMP_FILE, NULL) == DF_OK);
    DataFrame *back = NULL;
    CHECK(df_read_csv(TMP_FILE, NULL, &back) == DF_OK);
    CHECK(frames_equal(df, back));       /* every cell and type is the same */
    CHECK(type_of(back, "real, value") == DTYPE_FLOAT);  /* "72.0", not "72" */
    df_free(back);

    /* without a header */
    csv_options opts = csv_default_options();
    opts.has_header = 0;
    CHECK(df_write_csv(df, TMP_FILE, &opts) == DF_OK);
    CHECK(df_read_csv(TMP_FILE, &opts, &back) == DF_OK);
    CHECK(df_nrows(back) == 7 && same_string(name_of(back, 0), "col0"));
    df_free(back);

    /* with another delimiter */
    opts = csv_default_options();
    opts.delimiter = ';';
    CHECK(df_write_csv(df, TMP_FILE, &opts) == DF_OK);
    CHECK(df_read_csv(TMP_FILE, &opts, &back) == DF_OK);
    CHECK(frames_equal(df, back));
    df_free(back);
    df_free(df);

    /* floats that are whole numbers must stay FLOAT ("72.0", not "72") */
    double whole[] = {72.0, 3.0, -1.0};
    df = new_frame();
    add_column(df, make_float_column("w", whole, 3));
    CHECK(df_write_csv(df, TMP_FILE, NULL) == DF_OK);
    CHECK(df_read_csv(TMP_FILE, NULL, &back) == DF_OK);
    CHECK(type_of(back, "w") == DTYPE_FLOAT);
    CHECK(frames_equal(df, back));
    df_free(back);
    df_free(df);

    /* one column with missing cells: no row may get lost */
    int64_t one_col[] = {1, MISSING_NUM, 3};
    df = new_frame();
    add_column(df, make_int_column("v", one_col, 3));
    CHECK(df_write_csv(df, TMP_FILE, NULL) == DF_OK);
    CHECK(df_read_csv(TMP_FILE, NULL, &back) == DF_OK);
    CHECK(frames_equal(df, back));
    df_free(back);
    df_free(df);

    remove(TMP_FILE);
}

static void test_write_errors(void)
{
    DataFrame *df = new_frame();
    CHECK(df_write_csv(df, "no_such_folder/x.csv", NULL) == DF_ERR_IO);
    CHECK(df_write_csv(NULL, TMP_FILE, NULL) == DF_ERR_NULL_ARG);
    CHECK(df_write_csv(df, NULL, NULL) == DF_ERR_NULL_ARG);
    csv_options opts = csv_default_options();
    opts.delimiter = '\r';
    CHECK(df_write_csv(df, TMP_FILE, &opts) == DF_ERR_INVALID_ARG);

    /* an empty frame writes an empty file, which reads back empty */
    CHECK(df_write_csv(df, TMP_FILE, NULL) == DF_OK);
    DataFrame *back = NULL;
    CHECK(df_read_csv(TMP_FILE, NULL, &back) == DF_OK);
    CHECK(df_ncols(back) == 0);
    df_free(back);
    df_free(df);

    /* a file with a '\0' byte inside is refused */
    FILE *f = fopen(TMP_FILE, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fwrite("a\n1\0\n", 1, 5, f);
        fclose(f);
        back = (DataFrame *)1;
        CHECK(df_read_csv(TMP_FILE, NULL, &back) == DF_ERR_PARSE);
        CHECK(back == NULL);
    }
    remove(TMP_FILE);
}

static void test_large(void)
{
    /* 100000 lines, built as one big string */
    enum { N = 100000 };
    size_t size = (size_t)N * 32 + 32;
    char *text = malloc(size);
    CHECK(text != NULL);
    if (text == NULL) return;
    size_t len = (size_t)snprintf(text, size, "id,half,name\n");
    for (int i = 0; i < N; i++) {
        len += (size_t)snprintf(text + len, size - len, "%d,%d.5,n%d\n",
                                i, i, i);
    }

    DataFrame *df = NULL;
    CHECK(df_read_csv_string(text, NULL, &df) == DF_OK);
    free(text);
    CHECK(df_nrows(df) == N && df_ncols(df) == 3);
    CHECK(type_of(df, "id") == DTYPE_INT && type_of(df, "half") == DTYPE_FLOAT);
    CHECK(cell_int(df, "id", N - 1) == N - 1);
    CHECK(same_string(cell_string(df, "name", N - 1), "n99999"));

    /* write it out and read it back */
    DataFrame *back = NULL;
    CHECK(df_write_csv(df, TMP_FILE, NULL) == DF_OK);
    CHECK(df_read_csv(TMP_FILE, NULL, &back) == DF_OK);
    CHECK(frames_equal(df, back));
    remove(TMP_FILE);
    df_free(back);
    df_free(df);
}

int main(void)
{
    test_basic_read();
    test_type_inference();
    test_spaces_are_trimmed();
    test_quotes();
    test_na_values();
    test_line_endings_and_blank_lines();
    test_options();
    test_errors();
    test_empty_input();
    test_write_and_read_back();
    test_write_errors();
    test_large();
    return finish_tests();
}
