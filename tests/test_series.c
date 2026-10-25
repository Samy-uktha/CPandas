/* Unit tests for series.c / error.c. Build and run with `make test`
 * (AddressSanitizer + UBSan are on by default, so leaks fail the run). */
#include <stdio.h>
#include <string.h>

#include "series.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static Series *make(const char *name, df_dtype t)
{
    Series *s = NULL;
    df_status st = series_create(name, t, 0, &s);
    CHECK(st == DF_OK && s != NULL);
    return s;
}

static void test_create_and_invalid_args(void)
{
    Series *s = NULL;
    CHECK(series_create("x", DTYPE_INT, 0, NULL) == DF_ERR_NULL_ARG);
    CHECK(series_create(NULL, DTYPE_INT, 0, &s) == DF_ERR_NULL_ARG);
    CHECK(s == NULL);
    CHECK(series_create("", DTYPE_INT, 0, &s) == DF_ERR_INVALID_ARG);
    CHECK(s == NULL);
    CHECK(series_create("x", (df_dtype)99, 0, &s) == DF_ERR_INVALID_ARG);

    s = make("age", DTYPE_INT);
    CHECK(strcmp(series_name(s), "age") == 0);
    CHECK(series_dtype(s) == DTYPE_INT);
    CHECK(series_len(s) == 0);          /* empty series is valid */
    series_free(s);
    series_free(NULL);                  /* must not crash */
}

static void test_null_args_everywhere(void)
{
    int64_t i; double d; const char *c;
    CHECK(series_append_int(NULL, 1) == DF_ERR_NULL_ARG);
    CHECK(series_append_float(NULL, 1.0) == DF_ERR_NULL_ARG);
    CHECK(series_append_string(NULL, "a") == DF_ERR_NULL_ARG);
    CHECK(series_append_missing(NULL) == DF_ERR_NULL_ARG);
    CHECK(series_get_int(NULL, 0, &i) == DF_ERR_NULL_ARG);
    CHECK(series_get_float(NULL, 0, &d) == DF_ERR_NULL_ARG);
    CHECK(series_get_string(NULL, 0, &c) == DF_ERR_NULL_ARG);
    CHECK(series_len(NULL) == 0);
    CHECK(series_name(NULL) == NULL);
    CHECK(series_is_missing(NULL, 0) == 0);

    Series *s = make("s", DTYPE_STRING);
    CHECK(series_append_string(s, NULL) == DF_ERR_NULL_ARG);
    CHECK(series_len(s) == 0);
    series_free(s);
}

static void test_type_mismatch_leaves_series_unchanged(void)
{
    Series *s = make("n", DTYPE_INT);
    CHECK(series_append_int(s, 7) == DF_OK);
    CHECK(series_append_float(s, 1.5) == DF_ERR_TYPE_MISMATCH);
    CHECK(series_append_string(s, "x") == DF_ERR_TYPE_MISMATCH);
    CHECK(series_len(s) == 1);
    double d;
    CHECK(series_get_float(s, 0, &d) == DF_ERR_TYPE_MISMATCH);
    series_free(s);
}

static void test_growth_large(void)
{
    enum { N = 100000 };
    Series *s = make("big", DTYPE_INT);
    for (int k = 0; k < N; k++) {
        if (series_append_int(s, (int64_t)k * 3) != DF_OK) { CHECK(0); break; }
    }
    CHECK(series_len(s) == N);
    int64_t v = -1;
    CHECK(series_get_int(s, 0, &v) == DF_OK && v == 0);
    CHECK(series_get_int(s, N - 1, &v) == DF_OK && v == (int64_t)(N - 1) * 3);
    CHECK(series_get_int(s, N, &v) == DF_ERR_INDEX_OUT_OF_RANGE);
    series_free(s);
}

static void test_missing_values(void)
{
    Series *s = make("f", DTYPE_FLOAT);
    series_append_float(s, 1.5);
    series_append_missing(s);
    series_append_float(s, 3.5);
    CHECK(series_len(s) == 3);
    CHECK(series_count_missing(s) == 1);
    CHECK(series_is_missing(s, 0) == 0);
    CHECK(series_is_missing(s, 1) == 1);
    CHECK(series_is_missing(s, 99) == 0);
    double d = 0;
    CHECK(series_get_float(s, 1, &d) == DF_ERR_MISSING_VALUE);
    CHECK(series_fill_missing_float(s, 9.0) == DF_OK);
    CHECK(series_count_missing(s) == 0);
    CHECK(series_get_float(s, 1, &d) == DF_OK && d == 9.0);
    series_free(s);
}

static void test_strings_are_copied(void)
{
    char buf[16] = "hello";
    Series *s = make("w", DTYPE_STRING);
    CHECK(series_append_string(s, buf) == DF_OK);
    strcpy(buf, "CHANGED");                 /* caller reuses its buffer */
    const char *got = NULL;
    CHECK(series_get_string(s, 0, &got) == DF_OK);
    CHECK(strcmp(got, "hello") == 0);

    series_append_missing(s);
    series_append_string(s, "");            /* empty string is a real value */
    CHECK(series_is_missing(s, 2) == 0);

    CHECK(series_set_string(s, 0, "bye") == DF_OK);
    CHECK(series_get_string(s, 0, &got) == DF_OK && strcmp(got, "bye") == 0);
    CHECK(series_set_missing(s, 0) == DF_OK);
    CHECK(series_is_missing(s, 0) == 1);

    CHECK(series_fill_missing_string(s, "NA") == DF_OK);
    CHECK(series_count_missing(s) == 0);
    CHECK(series_get_string(s, 0, &got) == DF_OK && strcmp(got, "NA") == 0);
    CHECK(series_get_string(s, 1, &got) == DF_OK && strcmp(got, "NA") == 0);
    series_free(s);                         /* ASan flags any leaked copy */
}

static void test_many_strings_for_leaks(void)
{
    Series *s = make("names", DTYPE_STRING);
    for (int k = 0; k < 5000; k++) {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "name-%d", k);
        if (k % 7 == 0) series_append_missing(s);
        else series_append_string(s, tmp);
    }
    CHECK(series_len(s) == 5000);
    series_free(s);
}

static void test_clone_is_independent(void)
{
    Series *s = make("c", DTYPE_STRING);
    series_append_string(s, "a");
    series_append_missing(s);
    Series *c = NULL;
    CHECK(series_clone(s, &c) == DF_OK && c != NULL);
    CHECK(series_len(c) == 2 && series_is_missing(c, 1) == 1);
    series_set_string(s, 0, "zzz");
    const char *got = NULL;
    series_get_string(c, 0, &got);
    CHECK(strcmp(got, "a") == 0);
    series_free(s);
    series_free(c);

    Series *e = make("empty", DTYPE_FLOAT), *ec = NULL;
    CHECK(series_clone(e, &ec) == DF_OK && series_len(ec) == 0);
    series_free(e);
    series_free(ec);
}

static void test_take(void)
{
    Series *s = make("t", DTYPE_INT);
    for (int k = 0; k < 5; k++) series_append_int(s, k * 10);
    series_set_missing(s, 3);

    size_t idx[] = {4, 0, 0, SERIES_TAKE_MISSING, 3};
    Series *t = NULL;
    CHECK(series_take(s, idx, 5, &t) == DF_OK);
    CHECK(series_len(t) == 5);
    int64_t v;
    CHECK(series_get_int(t, 0, &v) == DF_OK && v == 40);
    CHECK(series_get_int(t, 1, &v) == DF_OK && v == 0);
    CHECK(series_get_int(t, 2, &v) == DF_OK && v == 0);   /* repeat allowed */
    CHECK(series_is_missing(t, 3) == 1);                  /* sentinel */
    CHECK(series_is_missing(t, 4) == 1);                  /* source missing */
    series_free(t);

    size_t bad[] = {1, 5};
    CHECK(series_take(s, bad, 2, &t) == DF_ERR_INDEX_OUT_OF_RANGE);
    CHECK(t == NULL);

    CHECK(series_take(s, NULL, 0, &t) == DF_OK);          /* n == 0 is fine */
    CHECK(series_len(t) == 0);
    series_free(t);
    CHECK(series_take(s, NULL, 3, &t) == DF_ERR_NULL_ARG);
    series_free(s);
}

static void test_rename_and_set(void)
{
    Series *s = make("old", DTYPE_INT);
    CHECK(series_rename(s, "new") == DF_OK);
    CHECK(strcmp(series_name(s), "new") == 0);
    CHECK(series_rename(s, "") == DF_ERR_INVALID_ARG);
    CHECK(strcmp(series_name(s), "new") == 0);            /* unchanged */

    series_append_int(s, 1);
    CHECK(series_set_int(s, 0, 42) == DF_OK);
    CHECK(series_set_int(s, 1, 1) == DF_ERR_INDEX_OUT_OF_RANGE);
    CHECK(series_set_float(s, 0, 1.0) == DF_ERR_TYPE_MISMATCH);
    series_free(s);
}

static void test_strerror(void)
{
    CHECK(strcmp(df_strerror(DF_OK), "success") == 0);
    CHECK(df_strerror((df_status)12345) != NULL);
}

int main(void)
{
    test_create_and_invalid_args();
    test_null_args_everywhere();
    test_type_mismatch_leaves_series_unchanged();
    test_growth_large();
    test_missing_values();
    test_strings_are_copied();
    test_many_strings_for_leaks();
    test_clone_is_independent();
    test_take();
    test_rename_and_set();
    test_strerror();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}