/* Tests for the BOOL and DATE column types (series.c, df_date.c), the
 * shared string storage of STRING columns, and how printing, sorting and
 * filtering handle the new types. Build and run with `make test`. */
#include <stdlib.h>

#include "df_date.h"
#include "df_filter.h"
#include "df_sort.h"
#include "test_helpers.h"

#define TMP_PRINT "build/test_types_print.txt"

/* ---- dates -------------------------------------------------------------- */

static int64_t day(int y, int m, int d)
{
    int64_t days = -999999;
    CHECK(df_date_from_ymd(y, m, d, &days) == DF_OK);
    return days;
}

static void test_date_numbers(void)
{
    CHECK(day(1970, 1, 1) == 0);
    CHECK(day(1970, 1, 2) == 1);
    CHECK(day(1969, 12, 31) == -1);
    CHECK(day(2000, 1, 1) == 10957);
    CHECK(day(2000, 3, 1) - day(2000, 2, 28) == 2);     /* 2000 is leap   */
    CHECK(day(1900, 3, 1) - day(1900, 2, 28) == 1);     /* 1900 is not    */
    CHECK(day(2024, 3, 1) - day(2024, 2, 28) == 2);
    CHECK(day(2025, 1, 1) - day(2024, 1, 1) == 366);
    CHECK(day(1, 1, 1) == -719162);
    CHECK(day(9999, 12, 31) == 2932896);

    int64_t days = 123;
    CHECK(df_date_from_ymd(2023, 2, 29, &days) == DF_ERR_INVALID_ARG);
    CHECK(df_date_from_ymd(2024, 13, 1, &days) == DF_ERR_INVALID_ARG);
    CHECK(df_date_from_ymd(2024, 4, 31, &days) == DF_ERR_INVALID_ARG);
    CHECK(df_date_from_ymd(2024, 1, 0, &days) == DF_ERR_INVALID_ARG);
    CHECK(df_date_from_ymd(0, 1, 1, &days) == DF_ERR_INVALID_ARG);
    CHECK(df_date_from_ymd(10000, 1, 1, &days) == DF_ERR_INVALID_ARG);
    CHECK(days == 123);                                 /* unchanged */
    CHECK(df_date_from_ymd(2024, 1, 1, NULL) == DF_ERR_NULL_ARG);

    /* Every day from year 1 to 9999 converts there and back, and the
       next day is always the next number. */
    int ok = 1;
    int y = 0, m = 0, d = 0;
    int64_t last = day(9999, 12, 31);
    for (int64_t n = day(1, 1, 1); n <= last; n++) {
        df_date_to_ymd(n, &y, &m, &d);
        int64_t back = 0;
        if (df_date_from_ymd(y, m, d, &back) != DF_OK || back != n) {
            ok = 0;
            break;
        }
    }
    CHECK(ok);
    df_date_to_ymd(day(2024, 2, 29), &y, NULL, &d);     /* NULLs allowed */
    CHECK(y == 2024 && d == 29);
}

static void test_date_text(void)
{
    int64_t days = 0;
    CHECK(df_date_parse("2024-02-29", &days) == DF_OK);
    CHECK(days == day(2024, 2, 29));
    CHECK(df_date_parse("1970-01-01", &days) == DF_OK && days == 0);

    const char *bad[] = {"2023-02-29", "2024-2-29", "2024/02/29", "24-02-29",
                         "2024-02-29x", "2024-02-2", " 2024-02-29", "",
                         "abcd-ef-gh", "2024-00-10", "2024-1-"};
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        CHECK(df_date_parse(bad[k], &days) == DF_ERR_PARSE);
    }
    CHECK(df_date_parse(NULL, &days) == DF_ERR_NULL_ARG);

    char buf[DF_DATE_TEXT_SIZE];
    df_date_format(day(2024, 3, 5), buf, sizeof buf);
    CHECK(same_string(buf, "2024-03-05"));
    df_date_format(0, buf, sizeof buf);
    CHECK(same_string(buf, "1970-01-01"));
    df_date_format(day(1, 1, 1), buf, sizeof buf);
    CHECK(same_string(buf, "0001-01-01"));
    char tiny[5];
    df_date_format(0, tiny, sizeof tiny);               /* cut short */
    CHECK(same_string(tiny, "1970"));
}

/* ---- bool and date Series ----------------------------------------------- */

static void test_bool_series(void)
{
    Series *s = NULL;
    CHECK(series_create("ok", DTYPE_BOOL, 0, &s) == DF_OK);
    CHECK(series_append_bool(s, 1) == DF_OK);
    CHECK(series_append_bool(s, 0) == DF_OK);
    CHECK(series_append_bool(s, 42) == DF_OK);          /* any non-zero */
    CHECK(series_append_missing(s) == DF_OK);
    CHECK(series_len(s) == 4);

    int v = -1;
    CHECK(series_get_bool(s, 0, &v) == DF_OK && v == 1);
    CHECK(series_get_bool(s, 1, &v) == DF_OK && v == 0);
    CHECK(series_get_bool(s, 2, &v) == DF_OK && v == 1);
    CHECK(series_get_bool(s, 3, &v) == DF_ERR_MISSING_VALUE);
    CHECK(series_get_bool(s, 9, &v) == DF_ERR_INDEX_OUT_OF_RANGE);
    int64_t iv = 0;
    CHECK(series_get_int(s, 0, &iv) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_append_int(s, 1) == DF_ERR_TYPE_MISMATCH);

    CHECK(series_set_bool(s, 1, 7) == DF_OK);
    CHECK(series_get_bool(s, 1, &v) == DF_OK && v == 1);
    CHECK(series_fill_missing_bool(s, 0) == DF_OK);
    CHECK(series_get_bool(s, 3, &v) == DF_OK && v == 0);
    CHECK(series_fill_missing_date(s, 0) == DF_ERR_TYPE_MISMATCH);

    /* growing past the first capacity */
    for (int i = 0; i < 100; i++) {
        CHECK(series_append_bool(s, i % 3 == 0) == DF_OK);
    }
    CHECK(series_get_bool(s, 4 + 99, &v) == DF_OK && v == 1);

    Series *c = NULL;
    CHECK(series_clone(s, &c) == DF_OK);
    CHECK(series_dtype(c) == DTYPE_BOOL && series_len(c) == 104);
    series_free(c);
    series_free(s);
}

static void test_date_series(void)
{
    Series *s = NULL;
    CHECK(series_create("joined", DTYPE_DATE, 2, &s) == DF_OK);
    CHECK(series_append_date(s, day(2024, 1, 15)) == DF_OK);
    CHECK(series_append_missing(s) == DF_OK);
    CHECK(series_append_date(s, day(2023, 6, 1)) == DF_OK);
    CHECK(series_append_float(s, 1.0) == DF_ERR_TYPE_MISMATCH);

    int64_t v = 0;
    CHECK(series_get_date(s, 0, &v) == DF_OK && v == day(2024, 1, 15));
    CHECK(series_get_date(s, 1, &v) == DF_ERR_MISSING_VALUE);
    CHECK(series_get_int(s, 0, &v) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_set_date(s, 1, 5) == DF_OK);
    CHECK(series_get_date(s, 1, &v) == DF_OK && v == 5);
    CHECK(series_set_missing(s, 1) == DF_OK);
    CHECK(series_fill_missing_date(s, 9) == DF_OK);
    CHECK(series_get_date(s, 1, &v) == DF_OK && v == 9);

    size_t idx[] = {2, SERIES_TAKE_MISSING, 0};
    Series *t = NULL;
    CHECK(series_take(s, idx, 3, &t) == DF_OK);
    CHECK(series_get_date(t, 0, &v) == DF_OK && v == day(2023, 6, 1));
    CHECK(series_is_missing(t, 1));
    series_free(t);
    series_free(s);
}

static void test_append_from(void)
{
    Series *a = NULL;
    Series *b = NULL;
    Series *f = NULL;
    CHECK(series_create("a", DTYPE_DATE, 0, &a) == DF_OK);
    CHECK(series_create("b", DTYPE_DATE, 0, &b) == DF_OK);
    CHECK(series_create("f", DTYPE_FLOAT, 0, &f) == DF_OK);
    series_append_date(a, 100);
    series_append_missing(a);

    CHECK(series_append_from(b, a, 0) == DF_OK);
    CHECK(series_append_from(b, a, 1) == DF_OK);
    CHECK(series_append_from(b, a, 2) == DF_ERR_INDEX_OUT_OF_RANGE);
    CHECK(series_append_from(f, a, 0) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_append_from(NULL, a, 0) == DF_ERR_NULL_ARG);
    int64_t v = 0;
    CHECK(series_len(b) == 2);
    CHECK(series_get_date(b, 0, &v) == DF_OK && v == 100);
    CHECK(series_is_missing(b, 1));
    series_free(a);
    series_free(b);
    series_free(f);
}

/* ---- shared string storage ---------------------------------------------- */

static void test_strings_stored_once(void)
{
    Series *s = NULL;
    CHECK(series_create("dept", DTYPE_STRING, 0, &s) == DF_OK);
    const char *depts[] = {"Sales", "IT", "HR"};
    for (int i = 0; i < 3000; i++) {
        CHECK(series_append_string(s, depts[i % 3]) == DF_OK);
    }
    CHECK(series_len(s) == 3000);
    CHECK(series_distinct_strings(s) == 3);

    /* Equal cells share one stored copy... */
    const char *x = NULL;
    const char *y = NULL;
    series_get_string(s, 0, &x);
    series_get_string(s, 3, &y);
    CHECK(x == y);
    /* ...but changing one cell does not change the others. */
    CHECK(series_set_string(s, 0, "Ops") == DF_OK);
    series_get_string(s, 0, &x);
    series_get_string(s, 3, &y);
    CHECK(same_string(x, "Ops") && same_string(y, "Sales"));
    CHECK(series_distinct_strings(s) == 4);

    /* Setting a cell to its own text works (the text is used twice for a
       moment, then once). */
    series_get_string(s, 0, &x);
    CHECK(series_set_string(s, 0, x) == DF_OK);
    series_get_string(s, 0, &x);
    CHECK(same_string(x, "Ops"));

    /* The last cell using "Ops" goes away: so does the stored text. */
    CHECK(series_set_missing(s, 0) == DF_OK);
    CHECK(series_distinct_strings(s) == 3);
    CHECK(series_fill_missing_string(s, "Ops") == DF_OK);
    CHECK(series_distinct_strings(s) == 4);

    /* fill_missing shares one copy among all the filled cells */
    for (int i = 1; i < 10; i++) {
        series_set_missing(s, (size_t)i);
    }
    CHECK(series_fill_missing_string(s, "Legal") == DF_OK);
    CHECK(series_distinct_strings(s) == 5);
    series_get_string(s, 1, &x);
    series_get_string(s, 9, &y);
    CHECK(x == y && same_string(x, "Legal"));

    CHECK(series_distinct_strings(NULL) == 0);
    Series *n = NULL;
    series_create("n", DTYPE_INT, 0, &n);
    CHECK(series_distinct_strings(n) == 0);
    series_free(n);
    series_free(s);
}

/* Many random changes, checked against a plain array of what every cell
   should hold. This exercises the hash table's growing and its deletions
   (including the "backward shift" that keeps lookups working). */
static void test_strings_random_changes(void)
{
    enum { CELLS = 400, WORDS = 150, STEPS = 20000 };
    char words[WORDS][16];
    for (int w = 0; w < WORDS; w++) {
        snprintf(words[w], sizeof words[w], "w%d", w * 7919 % 1000);
    }
    int expect[CELLS];      /* word number, or -1 for missing */

    Series *s = NULL;
    CHECK(series_create("w", DTYPE_STRING, 0, &s) == DF_OK);
    for (int i = 0; i < CELLS; i++) {
        series_append_missing(s);
        expect[i] = -1;
    }

    srand(12345);
    int ok = 1;
    for (int step = 0; step < STEPS && ok; step++) {
        int i = rand() % CELLS;
        if (rand() % 4 == 0) {
            ok = ok && series_set_missing(s, (size_t)i) == DF_OK;
            expect[i] = -1;
        } else {
            int w = rand() % WORDS;
            ok = ok && series_set_string(s, (size_t)i, words[w]) == DF_OK;
            expect[i] = w;
        }
        if (step % 1000 == 0 || step == STEPS - 1) {
            /* every cell is right, and the stored count is the number of
               different words in use */
            int used[WORDS] = {0};
            size_t distinct = 0;
            for (int k = 0; k < CELLS; k++) {
                const char *v = NULL;
                if (expect[k] < 0) {
                    ok = ok && series_is_missing(s, (size_t)k);
                    continue;
                }
                ok = ok && series_get_string(s, (size_t)k, &v) == DF_OK &&
                     same_string(v, words[expect[k]]);
                if (!used[expect[k]]) {
                    used[expect[k]] = 1;
                    distinct++;
                }
            }
            ok = ok && series_distinct_strings(s) == distinct;
        }
    }
    CHECK(ok);

    /* take / clone of a pooled Series */
    Series *c = NULL;
    CHECK(series_clone(s, &c) == DF_OK);
    CHECK(series_distinct_strings(c) == series_distinct_strings(s));
    series_free(s);         /* the clone owns its own copies */
    const char *v = NULL;
    for (int k = 0; k < CELLS; k++) {
        if (expect[k] >= 0) {
            CHECK(series_get_string(c, (size_t)k, &v) == DF_OK &&
                  same_string(v, words[expect[k]]));
        }
    }
    series_free(c);
}

/* ---- printing, sorting and filtering the new types ---------------------- */

static DataFrame *make_people(void)
{
    const char *names[] = {"Asha", "Ben", "Chen", "Dev"};
    DataFrame *df = new_frame();
    add_column(df, make_string_column("name", names, 4));

    Series *joined = NULL;
    series_create("joined", DTYPE_DATE, 4, &joined);
    series_append_date(joined, day(2024, 3, 5));
    series_append_missing(joined);
    series_append_date(joined, day(2021, 11, 30));
    series_append_date(joined, day(2024, 3, 5));
    add_column(df, joined);

    Series *remote = NULL;
    series_create("remote", DTYPE_BOOL, 4, &remote);
    series_append_bool(remote, 1);
    series_append_bool(remote, 0);
    series_append_missing(remote);
    series_append_bool(remote, 0);
    add_column(df, remote);
    return df;
}

static void test_print(void)
{
    DataFrame *df = make_people();
    FILE *f = fopen(TMP_PRINT, "w");
    CHECK(f != NULL);
    if (f == NULL) {
        df_free(df);
        return;
    }
    CHECK(df_print(df, f, 0) == DF_OK);
    fclose(f);

    char text[1024] = {0};
    f = fopen(TMP_PRINT, "r");
    size_t got = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[got] = '\0';
    CHECK(strstr(text, "2024-03-05") != NULL);
    CHECK(strstr(text, "2021-11-30") != NULL);
    CHECK(strstr(text, "True") != NULL);
    CHECK(strstr(text, "False") != NULL);
    CHECK(strstr(text, "NA") != NULL);
    df_free(df);
}

static void test_sort_and_filter(void)
{
    DataFrame *df = make_people();
    DataFrame *out = NULL;

    /* dates: earliest first, missing last */
    CHECK(df_sort_by_column(df, "joined", 1, &out) == DF_OK);
    CHECK(same_string(cell_string(out, "name", 0), "Chen"));
    CHECK(same_string(cell_string(out, "name", 1), "Asha"));  /* stable */
    CHECK(same_string(cell_string(out, "name", 2), "Dev"));
    CHECK(same_string(cell_string(out, "name", 3), "Ben"));
    df_free(out);

    /* bools: False before True, missing last; descending flips that */
    CHECK(df_sort_by_column(df, "remote", 0, &out) == DF_OK);
    CHECK(same_string(cell_string(out, "name", 0), "Asha"));
    CHECK(same_string(cell_string(out, "name", 3), "Chen"));
    df_free(out);

    CHECK(df_filter_date(df, "joined", DF_OP_GE, day(2024, 1, 1), &out) ==
          DF_OK);
    CHECK(df_nrows(out) == 2);
    df_free(out);
    CHECK(df_filter_date(df, "joined", DF_OP_NE, day(2024, 3, 5), &out) ==
          DF_OK);
    CHECK(df_nrows(out) == 1);          /* Chen; Ben is missing */
    df_free(out);
    CHECK(df_filter_date(df, "name", DF_OP_EQ, 0, &out) ==
          DF_ERR_TYPE_MISMATCH);
    CHECK(out == NULL);
    CHECK(df_filter_date(df, "joined", (df_compare_op)99, 0, &out) ==
          DF_ERR_INVALID_ARG);
    CHECK(df_filter_date(df, "nope", DF_OP_EQ, 0, &out) == DF_ERR_NOT_FOUND);

    /* a bool column can be filtered like a 0/1 number */
    CHECK(df_filter_number(df, "remote", DF_OP_EQ, 1, &out) == DF_OK);
    CHECK(df_nrows(out) == 1);
    CHECK(same_string(cell_string(out, "name", 0), "Asha"));
    df_free(out);
    CHECK(df_filter_number(df, "joined", DF_OP_EQ, 1, &out) ==
          DF_ERR_TYPE_MISMATCH);
    df_free(df);
}

int main(void)
{
    test_date_numbers();
    test_date_text();
    test_bool_series();
    test_date_series();
    test_append_from();
    test_strings_stored_once();
    test_strings_random_changes();
    test_print();
    test_sort_and_filter();
    return finish_tests();
}
