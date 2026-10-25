/*
 * series.c - implementation of the Series type declared in series.h.
 *
 * Internal layout: one typed data array plus a parallel is_missing array.
 * Invariants:
 *   - len <= cap, and both arrays have room for `cap` elements.
 *   - For DTYPE_STRING, data.s[i] is a malloc'd string owned by the Series, or
 *     NULL exactly when is_missing[i] is 1.
 *   - For numeric dtypes a missing cell holds 0 in data (never read).
 *
 * Note: strdup() is POSIX, not ISO C11, so we use our own copy helper to keep
 * the code building cleanly with -std=c11 -Wpedantic.
 */
#include "series.h"

#include <stdlib.h>
#include <string.h>

#define SERIES_DEFAULT_CAP 8

struct Series {
    char          *name;
    df_dtype       dtype;
    size_t         len;
    size_t         cap;
    union {
        int64_t *i;
        double  *f;
        char   **s;
    } data;
    unsigned char *is_missing;
};

/* ---- helpers ------------------------------------------------------------ */

static char *copy_string(const char *src)
{
    size_t n = strlen(src) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, src, n);
    return p;
}

static size_t elem_size(df_dtype t)
{
    switch (t) {
    case DTYPE_INT:    return sizeof(int64_t);
    case DTYPE_FLOAT:  return sizeof(double);
    case DTYPE_STRING: return sizeof(char *);
    }
    return 0;
}

static int valid_dtype(df_dtype t)
{
    return t == DTYPE_INT || t == DTYPE_FLOAT || t == DTYPE_STRING;
}

static void *data_ptr(const Series *s)
{
    switch (s->dtype) {
    case DTYPE_INT:    return s->data.i;
    case DTYPE_FLOAT:  return s->data.f;
    case DTYPE_STRING: return s->data.s;
    }
    return NULL;
}

static void set_data_ptr(Series *s, void *p)
{
    switch (s->dtype) {
    case DTYPE_INT:    s->data.i = p; break;
    case DTYPE_FLOAT:  s->data.f = p; break;
    case DTYPE_STRING: s->data.s = p; break;
    }
}

/* Makes room for at least one more element. Doubles capacity. Each realloc
 * goes into a temporary so a failure never loses the original pointer. If the
 * first realloc succeeds and the second fails, we keep the (valid, larger)
 * data block but leave cap unchanged, so the Series stays consistent. */
static df_status ensure_room(Series *s)
{
    if (s->len < s->cap) return DF_OK;

    size_t new_cap = s->cap ? s->cap * 2 : SERIES_DEFAULT_CAP;
    if (new_cap < s->cap) return DF_ERR_NOMEM; /* overflow */
    if (new_cap > SIZE_MAX / elem_size(s->dtype)) return DF_ERR_NOMEM;

    void *new_data = realloc(data_ptr(s), new_cap * elem_size(s->dtype));
    if (!new_data) return DF_ERR_NOMEM;
    set_data_ptr(s, new_data);

    unsigned char *new_miss = realloc(s->is_missing, new_cap);
    if (!new_miss) return DF_ERR_NOMEM;
    s->is_missing = new_miss;

    s->cap = new_cap;
    return DF_OK;
}

static void free_cells(Series *s)
{
    if (s->dtype == DTYPE_STRING && s->data.s) {
        for (size_t i = 0; i < s->len; i++) free(s->data.s[i]);
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

df_status series_create(const char *name, df_dtype dtype,
                        size_t initial_cap, Series **out)
{
    if (!out) return DF_ERR_NULL_ARG;
    *out = NULL;
    if (!name) return DF_ERR_NULL_ARG;
    if (name[0] == '\0' || !valid_dtype(dtype)) return DF_ERR_INVALID_ARG;
    if (initial_cap == 0) initial_cap = SERIES_DEFAULT_CAP;
    if (initial_cap > SIZE_MAX / elem_size(dtype)) return DF_ERR_NOMEM;

    Series *s = calloc(1, sizeof *s);
    if (!s) return DF_ERR_NOMEM;

    s->name = copy_string(name);
    s->dtype = dtype;
    s->cap = initial_cap;
    set_data_ptr(s, malloc(initial_cap * elem_size(dtype)));
    s->is_missing = malloc(initial_cap);

    if (!s->name || !data_ptr(s) || !s->is_missing) {
        free(s->name);
        free(data_ptr(s));
        free(s->is_missing);
        free(s);
        return DF_ERR_NOMEM;
    }
    *out = s;
    return DF_OK;
}

void series_free(Series *s)
{
    if (!s) return;
    free_cells(s);
    free(data_ptr(s));
    free(s->is_missing);
    free(s->name);
    free(s);
}

/* ---- appending ---------------------------------------------------------- */

df_status series_append_int(Series *s, int64_t v)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_INT) return DF_ERR_TYPE_MISMATCH;
    df_status st = ensure_room(s);
    if (st != DF_OK) return st;
    s->data.i[s->len] = v;
    s->is_missing[s->len] = 0;
    s->len++;
    return DF_OK;
}

df_status series_append_float(Series *s, double v)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_FLOAT) return DF_ERR_TYPE_MISMATCH;
    df_status st = ensure_room(s);
    if (st != DF_OK) return st;
    s->data.f[s->len] = v;
    s->is_missing[s->len] = 0;
    s->len++;
    return DF_OK;
}

df_status series_append_string(Series *s, const char *v)
{
    if (!s || !v) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_STRING) return DF_ERR_TYPE_MISMATCH;
    df_status st = ensure_room(s);
    if (st != DF_OK) return st;
    char *copy = copy_string(v);
    if (!copy) return DF_ERR_NOMEM;   /* len not yet bumped: Series unchanged */
    s->data.s[s->len] = copy;
    s->is_missing[s->len] = 0;
    s->len++;
    return DF_OK;
}

df_status series_append_missing(Series *s)
{
    if (!s) return DF_ERR_NULL_ARG;
    df_status st = ensure_room(s);
    if (st != DF_OK) return st;
    switch (s->dtype) {
    case DTYPE_INT:    s->data.i[s->len] = 0;    break;
    case DTYPE_FLOAT:  s->data.f[s->len] = 0.0;  break;
    case DTYPE_STRING: s->data.s[s->len] = NULL; break;
    }
    s->is_missing[s->len] = 1;
    s->len++;
    return DF_OK;
}

/* ---- clone / take ------------------------------------------------------- */

/* Appends a copy of cell `i` of src to dst (same dtype). */
static df_status append_cell_from(Series *dst, const Series *src, size_t i)
{
    if (src->is_missing[i]) return series_append_missing(dst);
    switch (src->dtype) {
    case DTYPE_INT:    return series_append_int(dst, src->data.i[i]);
    case DTYPE_FLOAT:  return series_append_float(dst, src->data.f[i]);
    case DTYPE_STRING: return series_append_string(dst, src->data.s[i]);
    }
    return DF_ERR_INVALID_ARG;
}

df_status series_clone(const Series *s, Series **out)
{
    if (!out) return DF_ERR_NULL_ARG;
    *out = NULL;
    if (!s) return DF_ERR_NULL_ARG;

    Series *c = NULL;
    df_status st = series_create(s->name, s->dtype, s->len, &c);
    if (st != DF_OK) return st;
    for (size_t i = 0; i < s->len; i++) {
        st = append_cell_from(c, s, i);
        if (st != DF_OK) { series_free(c); return st; }
    }
    *out = c;
    return DF_OK;
}

df_status series_take(const Series *s, const size_t *indices, size_t n,
                      Series **out)
{
    if (!out) return DF_ERR_NULL_ARG;
    *out = NULL;
    if (!s || (n > 0 && !indices)) return DF_ERR_NULL_ARG;

    /* Validate first so a bad index never costs us an allocation. */
    for (size_t k = 0; k < n; k++) {
        if (indices[k] != SERIES_TAKE_MISSING && indices[k] >= s->len)
            return DF_ERR_INDEX_OUT_OF_RANGE;
    }

    Series *t = NULL;
    df_status st = series_create(s->name, s->dtype, n, &t);
    if (st != DF_OK) return st;
    for (size_t k = 0; k < n; k++) {
        st = (indices[k] == SERIES_TAKE_MISSING)
                 ? series_append_missing(t)
                 : append_cell_from(t, s, indices[k]);
        if (st != DF_OK) { series_free(t); return st; }
    }
    *out = t;
    return DF_OK;
}

/* ---- inspection --------------------------------------------------------- */

const char *series_name(const Series *s)  { return s ? s->name : NULL; }
df_dtype    series_dtype(const Series *s) { return s ? s->dtype : DTYPE_INT; }
size_t      series_len(const Series *s)   { return s ? s->len : 0; }

size_t series_count_missing(const Series *s)
{
    size_t c = 0;
    if (!s) return 0;
    for (size_t i = 0; i < s->len; i++) c += s->is_missing[i] ? 1 : 0;
    return c;
}

int series_is_missing(const Series *s, size_t i)
{
    if (!s || i >= s->len) return 0;
    return s->is_missing[i] ? 1 : 0;
}

/* Shared checks for the typed getters/setters. */
static df_status check_cell(const Series *s, size_t i, df_dtype want,
                            int must_be_present)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != want) return DF_ERR_TYPE_MISMATCH;
    if (i >= s->len) return DF_ERR_INDEX_OUT_OF_RANGE;
    if (must_be_present && s->is_missing[i]) return DF_ERR_MISSING_VALUE;
    return DF_OK;
}

df_status series_get_int(const Series *s, size_t i, int64_t *out)
{
    if (!out) return DF_ERR_NULL_ARG;
    df_status st = check_cell(s, i, DTYPE_INT, 1);
    if (st == DF_OK) *out = s->data.i[i];
    return st;
}

df_status series_get_float(const Series *s, size_t i, double *out)
{
    if (!out) return DF_ERR_NULL_ARG;
    df_status st = check_cell(s, i, DTYPE_FLOAT, 1);
    if (st == DF_OK) *out = s->data.f[i];
    return st;
}

df_status series_get_string(const Series *s, size_t i, const char **out)
{
    if (!out) return DF_ERR_NULL_ARG;
    df_status st = check_cell(s, i, DTYPE_STRING, 1);
    if (st == DF_OK) *out = s->data.s[i];
    return st;
}

/* ---- modification ------------------------------------------------------- */

df_status series_rename(Series *s, const char *new_name)
{
    if (!s || !new_name) return DF_ERR_NULL_ARG;
    if (new_name[0] == '\0') return DF_ERR_INVALID_ARG;
    char *copy = copy_string(new_name);
    if (!copy) return DF_ERR_NOMEM;
    free(s->name);
    s->name = copy;
    return DF_OK;
}

df_status series_set_int(Series *s, size_t i, int64_t v)
{
    df_status st = check_cell(s, i, DTYPE_INT, 0);
    if (st != DF_OK) return st;
    s->data.i[i] = v;
    s->is_missing[i] = 0;
    return DF_OK;
}

df_status series_set_float(Series *s, size_t i, double v)
{
    df_status st = check_cell(s, i, DTYPE_FLOAT, 0);
    if (st != DF_OK) return st;
    s->data.f[i] = v;
    s->is_missing[i] = 0;
    return DF_OK;
}

df_status series_set_string(Series *s, size_t i, const char *v)
{
    if (!v) return DF_ERR_NULL_ARG;
    df_status st = check_cell(s, i, DTYPE_STRING, 0);
    if (st != DF_OK) return st;
    char *copy = copy_string(v);        /* copy first: failure changes nothing */
    if (!copy) return DF_ERR_NOMEM;
    free(s->data.s[i]);
    s->data.s[i] = copy;
    s->is_missing[i] = 0;
    return DF_OK;
}

df_status series_set_missing(Series *s, size_t i)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (i >= s->len) return DF_ERR_INDEX_OUT_OF_RANGE;
    if (s->dtype == DTYPE_STRING) {
        free(s->data.s[i]);
        s->data.s[i] = NULL;
    }
    s->is_missing[i] = 1;
    return DF_OK;
}

df_status series_fill_missing_int(Series *s, int64_t v)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_INT) return DF_ERR_TYPE_MISMATCH;
    for (size_t i = 0; i < s->len; i++) {
        if (s->is_missing[i]) { s->data.i[i] = v; s->is_missing[i] = 0; }
    }
    return DF_OK;
}

df_status series_fill_missing_float(Series *s, double v)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_FLOAT) return DF_ERR_TYPE_MISMATCH;
    for (size_t i = 0; i < s->len; i++) {
        if (s->is_missing[i]) { s->data.f[i] = v; s->is_missing[i] = 0; }
    }
    return DF_OK;
}

/* Strings can fail mid-way (one copy per cell), so all-or-nothing: make every
 * copy first, and only commit once they all succeeded. */
df_status series_fill_missing_string(Series *s, const char *v)
{
    if (!s || !v) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_STRING) return DF_ERR_TYPE_MISMATCH;

    size_t n = series_count_missing(s);
    if (n == 0) return DF_OK;

    char **copies = malloc(n * sizeof *copies);
    if (!copies) return DF_ERR_NOMEM;
    for (size_t k = 0; k < n; k++) {
        copies[k] = copy_string(v);
        if (!copies[k]) {
            while (k > 0) free(copies[--k]);
            free(copies);
            return DF_ERR_NOMEM;
        }
    }
    size_t k = 0;
    for (size_t i = 0; i < s->len; i++) {
        if (s->is_missing[i]) {
            s->data.s[i] = copies[k++];
            s->is_missing[i] = 0;
        }
    }
    free(copies);
    return DF_OK;
}