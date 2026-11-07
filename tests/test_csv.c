/* Tests for csv.c. Build and run with `make test` (from the project folder:
 * the file tests write temporary files into build/). */
#include <stdlib.h>

#include "csv.h"
#include "df_date.h"
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
            } else if (series_dtype(x) == DTYPE_BOOL) {
                int u = 0, v = 0;
                series_get_bool(x, r, &u);
                series_get_bool(y, r, &v);
                if (u != v) return 0;
            } else if (series_dtype(x) == DTYPE_DATE) {
                int64_t u = 0, v = 0;
                series_get_date(x, r, &u);
                series_get_date(y, r, &v);
                if (u != v) return 0;
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

/* ---- booleans and dates ------------------------------------------------- */

static void test_bool_and_date_columns(void)
{
    DataFrame *df = read_text(
        "flag,mixed,when,notdate,num01\n"
        "True,true,2024-02-29,2024/02/29,1\n"
        "FALSE,yes,1999-12-31,2024-02-29,0\n"
        ",False,,2023-02-29,1\n");
    CHECK(type_of(df, "flag") == DTYPE_BOOL);
    CHECK(type_of(df, "mixed") == DTYPE_STRING);    /* "yes" is text */
    CHECK(type_of(df, "when") == DTYPE_DATE);
    CHECK(type_of(df, "notdate") == DTYPE_STRING);  /* "/" and 2023-02-29 */
    CHECK(type_of(df, "num01") == DTYPE_INT);       /* 1 and 0 stay INT */

    const Series *s = NULL;
    int b = -1;
    df_get_column(df, "flag", &s);
    CHECK(series_get_bool(s, 0, &b) == DF_OK && b == 1);
    CHECK(series_get_bool(s, 1, &b) == DF_OK && b == 0);
    CHECK(series_is_missing(s, 2));

    int64_t d = 0, expect = 0;
    df_get_column(df, "when", &s);
    df_date_from_ymd(2024, 2, 29, &expect);
    CHECK(series_get_date(s, 0, &d) == DF_OK && d == expect);
    CHECK(series_is_missing(s, 2));

    /* written as True/False and YYYY-MM-DD, and read back the same */
    CHECK(df_write_csv(df, TMP_FILE, NULL) == DF_OK);
    DataFrame *back = NULL;
    CHECK(df_read_csv(TMP_FILE, NULL, &back) == DF_OK);
    CHECK(frames_equal(df, back));
    df_free(back);

    FILE *f = fopen(TMP_FILE, "rb");
    char text[512] = {0};
    if (f != NULL) {
        size_t got = fread(text, 1, sizeof text - 1, f);
        text[got] = '\0';
        fclose(f);
    }
    CHECK(strstr(text, "True,true,2024-02-29,2024/02/29,1\n") != NULL);
    CHECK(strstr(text, "False,yes,1999-12-31") != NULL);
    remove(TMP_FILE);
    df_free(df);
}

/* ---- error reports with line numbers ------------------------------------ */

/* Reads text, expecting status `want`, and checks the reported line and
   field. Returns the message (valid until the next call). */
static const char *read_error(const char *text, df_status want, size_t line,
                              size_t field)
{
    static csv_error err;
    csv_options opts = csv_default_options();
    opts.error = &err;
    DataFrame *df = NULL;
    CHECK(df_read_csv_string(text, &opts, &df) == want);
    CHECK(df == NULL);
    CHECK(err.line == line);
    CHECK(err.field == field);
    CHECK(err.message[0] != '\0');
    df_free(df);
    return err.message;
}

static void test_error_lines(void)
{
    const char *msg = NULL;

    msg = read_error("a,b\n1,2\n3,4\n5\n", DF_ERR_PARSE, 4, 0);
    CHECK(strstr(msg, "expected 2 fields but found 1") != NULL);

    /* blank lines, a quoted newline and \r\n line endings all count */
    msg = read_error("a,b\r\n\r\n1,\"x\ny\"\r\n2,3,4\r\n", DF_ERR_PARSE, 5, 0);
    CHECK(strstr(msg, "found 3") != NULL);
    /* a lone \r ends a line too */
    read_error("a\r1\r2,2\r", DF_ERR_PARSE, 3, 0);

    msg = read_error("a,b\n1,2\n3,\"never closed\n4,5\n", DF_ERR_PARSE, 3, 2);
    CHECK(strstr(msg, "never closed") != NULL);

    msg = read_error("a,b\n1,\"ab\"c\n", DF_ERR_PARSE, 2, 2);
    CHECK(strstr(msg, "closing quote") != NULL);

    msg = read_error("\n\nx,y,x\n1,2,3\n", DF_ERR_DUPLICATE_NAME, 3, 3);
    CHECK(strstr(msg, "\"x\"") != NULL);

    /* options and files */
    csv_error err;
    csv_options opts = csv_default_options();
    opts.error = &err;
    opts.delimiter = '"';
    DataFrame *df = NULL;
    CHECK(df_read_csv_string("a\n", &opts, &df) == DF_ERR_INVALID_ARG);
    CHECK(err.line == 0 && err.message[0] != '\0');
    opts.delimiter = ',';
    CHECK(df_read_csv("no_such_file_here.csv", &opts, &df) == DF_ERR_IO);
    CHECK(err.line == 0 && strstr(err.message, "no_such_file_here") != NULL);

    /* success clears an old report */
    CHECK(df_read_csv_string("a\n1\n", &opts, &df) == DF_OK);
    CHECK(err.message[0] == '\0' && err.line == 0);
    df_free(df);

    /* the '\0' byte in a file: reported on its line */
    FILE *f = fopen(TMP_FILE, "wb");
    if (f != NULL) {
        fwrite("a\n1\n2\0\n", 1, 7, f);
        fclose(f);
        CHECK(df_read_csv(TMP_FILE, &opts, &df) == DF_ERR_PARSE);
        CHECK(err.line == 3);
        remove(TMP_FILE);
    }
}

/* ---- reading in chunks -------------------------------------------------- */

typedef struct {
    size_t  calls;
    size_t  rows;
    int64_t id_sum;
    size_t  first_rows[16];
    size_t  stop_after;     /* 0 = never stop */
    int     types_ok;
} ChunkTotals;

static df_status add_chunk(const DataFrame *chunk, size_t first_row,
                           void *user_data)
{
    ChunkTotals *t = user_data;
    if (t->calls < 16) {
        t->first_rows[t->calls] = first_row;
    }
    t->calls++;
    t->rows += df_nrows(chunk);
    const Series *id = NULL;
    if (df_get_column(chunk, "id", &id) != DF_OK ||
        series_dtype(id) != DTYPE_INT) {
        t->types_ok = 0;
        return DF_OK;
    }
    for (size_t r = 0; r < series_len(id); r++) {
        int64_t v = 0;
        series_get_int(id, r, &v);
        t->id_sum += v;
    }
    if (t->stop_after > 0 && t->calls == t->stop_after) {
        return DF_ERR_INVALID_ARG;  /* any status other than DF_OK stops */
    }
    return DF_OK;
}

static int write_text_file(const char *text)
{
    FILE *f = fopen(TMP_FILE, "wb");
    if (f == NULL) {
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return 1;
}

static void test_chunks(void)
{
    /* 1000 rows, ids 0..999 */
    enum { N = 1000 };
    char *text = malloc((size_t)N * 24 + 32);
    CHECK(text != NULL);
    if (text == NULL) return;
    size_t len = (size_t)sprintf(text, "\xEF\xBB\xBFid,name\n");
    for (int i = 0; i < N; i++) {
        len += (size_t)sprintf(text + len, "%d,n%d\n", i, i % 7);
    }
    CHECK(write_text_file(text));
    free(text);

    ChunkTotals t = {0, 0, 0, {0}, 0, 1};
    CHECK(df_read_csv_chunks(TMP_FILE, NULL, 300, add_chunk, &t) == DF_OK);
    CHECK(t.calls == 4);                    /* 300 + 300 + 300 + 100 */
    CHECK(t.rows == N);
    CHECK(t.id_sum == (int64_t)N * (N - 1) / 2);
    CHECK(t.first_rows[0] == 0 && t.first_rows[1] == 300 &&
          t.first_rows[3] == 900);
    CHECK(t.types_ok);

    /* one row at a time, and one chunk for everything */
    ChunkTotals one = {0, 0, 0, {0}, 0, 1};
    CHECK(df_read_csv_chunks(TMP_FILE, NULL, 1, add_chunk, &one) == DF_OK);
    CHECK(one.calls == N && one.rows == N);
    ChunkTotals all = {0, 0, 0, {0}, 0, 1};
    CHECK(df_read_csv_chunks(TMP_FILE, NULL, 5000, add_chunk, &all) == DF_OK);
    CHECK(all.calls == 1 && all.id_sum == t.id_sum);

    /* the handler can stop early; its status is returned */
    ChunkTotals stop = {0, 0, 0, {0}, 2, 1};
    CHECK(df_read_csv_chunks(TMP_FILE, NULL, 100, add_chunk, &stop) ==
          DF_ERR_INVALID_ARG);
    CHECK(stop.calls == 2 && stop.rows == 200);

    /* bad arguments */
    CHECK(df_read_csv_chunks(TMP_FILE, NULL, 0, add_chunk, &t) ==
          DF_ERR_INVALID_ARG);
    CHECK(df_read_csv_chunks(TMP_FILE, NULL, 10, NULL, &t) == DF_ERR_NULL_ARG);
    CHECK(df_read_csv_chunks(NULL, NULL, 10, add_chunk, &t) == DF_ERR_NULL_ARG);
    CHECK(df_read_csv_chunks("no_such_file_here.csv", NULL, 10, add_chunk,
                             &t) == DF_ERR_IO);

    /* only a header: the handler is never called */
    CHECK(write_text_file("id,name\n"));
    ChunkTotals none = {0, 0, 0, {0}, 0, 1};
    CHECK(df_read_csv_chunks(TMP_FILE, NULL, 10, add_chunk, &none) == DF_OK);
    CHECK(none.calls == 0);

    /* a broken row part-way: earlier chunks were handled, then the error
       comes with its line number */
    CHECK(write_text_file("id,name\n1,a\n2,b\n3,c\n4\n5,e\n"));
    csv_error err;
    csv_options opts = csv_default_options();
    opts.error = &err;
    ChunkTotals part = {0, 0, 0, {0}, 0, 1};
    CHECK(df_read_csv_chunks(TMP_FILE, &opts, 2, add_chunk, &part) ==
          DF_ERR_PARSE);
    CHECK(part.calls == 1 && part.rows == 2);
    CHECK(err.line == 5);

    /* no header: columns col0, col1 and every row is data */
    CHECK(write_text_file("1,a\n2,b\n3,c\n"));
    opts = csv_default_options();
    opts.has_header = 0;
    ChunkTotals headless = {0, 0, 0, {0}, 0, 1};
    CHECK(df_read_csv_chunks(TMP_FILE, &opts, 2, add_chunk, &headless) ==
          DF_OK);
    CHECK(headless.rows == 3 && headless.types_ok == 0);  /* no "id" */
    remove(TMP_FILE);
}

/* A file bigger than the 4 KB read buffer, with a quoted field that spans
   the buffer boundary, reads the same as the same text from memory. */
static void test_file_bigger_than_buffer(void)
{
    enum { N = 3000 };
    char *text = malloc((size_t)N * 64 + 64);
    CHECK(text != NULL);
    if (text == NULL) return;
    size_t len = (size_t)sprintf(text, "id,note\r\n");
    for (int i = 0; i < N; i++) {
        len += (size_t)sprintf(text + len, "%d,\"line %d, \"\"quoted\"\"\n"
                               "second part\"\r\n", i, i);
    }
    CHECK(write_text_file(text));

    DataFrame *from_text = NULL;
    DataFrame *from_file = NULL;
    CHECK(df_read_csv_string(text, NULL, &from_text) == DF_OK);
    CHECK(df_read_csv(TMP_FILE, NULL, &from_file) == DF_OK);
    CHECK(df_nrows(from_file) == N);
    CHECK(frames_equal(from_text, from_file));
    CHECK(same_string(cell_string(from_file, "note", N - 1),
                      "line 2999, \"quoted\"\nsecond part"));
    free(text);
    df_free(from_text);
    df_free(from_file);
    remove(TMP_FILE);
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
    test_bool_and_date_columns();
    test_error_lines();
    test_chunks();
    test_file_bigger_than_buffer();
    return finish_tests();
}
