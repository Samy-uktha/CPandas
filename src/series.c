/*
 * series.c - implementation of the Series type declared in series.h.
 *
 * Internal layout: one typed data array plus a parallel is_missing array.
 * Invariants:
 *   - len <= cap, and both arrays have room for `cap` elements.
 *   - For DTYPE_STRING, data.s[i] points at the text of an entry in the
 *     Series' string pool (see below), or is NULL exactly when
 *     is_missing[i] is 1.
 *   - For the other dtypes a missing cell holds 0 in data (never read).
 *   - DTYPE_DATE uses the same int64_t array as DTYPE_INT (days since
 *     1970-01-01); DTYPE_BOOL uses one byte per cell (0 or 1).
 *
 * String pool: every different text is stored once, in a PoolEntry that
 * counts how many cells point at it (refs). Equal texts share the entry; the
 * entry is freed when its last cell lets go of it. The pool finds entries
 * with a hash table (open addressing, linear probing).
 *
 * Note: strdup() is POSIX, not ISO C11, so we use our own copy helper to keep
 * the code building cleanly with -std=c11 -Wpedantic.
 */
#include "series.h"
#include "df_hash.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SERIES_DEFAULT_CAP 8
#define POOL_FIRST_CAP     16

/* One stored text. Cells point at `text`; `refs` counts how many do. */
typedef struct {
    size_t   refs;
    uint64_t hash;
    char     text[];        /* flexible array member: the text itself */
} PoolEntry;

/* Hash table of PoolEntry pointers. cap is 0 or a power of two, and
   slots[k] == NULL means the slot is empty. */
typedef struct {
    PoolEntry **slots;
    size_t      cap;
    size_t      count;
} StringPool;

struct Series {
    char          *name;
    df_dtype       dtype;
    size_t         len;
    size_t         cap;
    union {
        int64_t       *i;   /* DTYPE_INT and DTYPE_DATE */
        double        *f;
        char         **s;
        unsigned char *b;   /* DTYPE_BOOL */
    } data;
    unsigned char *is_missing;
    StringPool     pool;    /* only used by DTYPE_STRING */
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
    case DTYPE_BOOL:   return sizeof(unsigned char);
    case DTYPE_DATE:   return sizeof(int64_t);
    }
    return 0;
}

static int valid_dtype(df_dtype t)
{
    return t == DTYPE_INT || t == DTYPE_FLOAT || t == DTYPE_STRING ||
           t == DTYPE_BOOL || t == DTYPE_DATE;
}

static void *data_ptr(const Series *s)
{
    switch (s->dtype) {
    case DTYPE_INT:    return s->data.i;
    case DTYPE_FLOAT:  return s->data.f;
    case DTYPE_STRING: return s->data.s;
    case DTYPE_BOOL:   return s->data.b;
    case DTYPE_DATE:   return s->data.i;
    }
    return NULL;
}

static void set_data_ptr(Series *s, void *p)
{
    switch (s->dtype) {
    case DTYPE_INT:    s->data.i = p; break;
    case DTYPE_FLOAT:  s->data.f = p; break;
    case DTYPE_STRING: s->data.s = p; break;
    case DTYPE_BOOL:   s->data.b = p; break;
    case DTYPE_DATE:   s->data.i = p; break;
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

/* ---- string pool -------------------------------------------------------- */

/* The entry that holds `text` (text always points inside an entry). */
static PoolEntry *entry_of(char *text)
{
    return (PoolEntry *)(void *)(text - offsetof(PoolEntry, text));
}

/* The slot that holds an entry equal to `text`, or else the empty slot
   where it would go. The table must have at least one empty slot. */
static size_t pool_find_slot(const StringPool *pool, const char *text,
                             uint64_t hash)
{
    size_t mask = pool->cap - 1;
    size_t k = (size_t)hash & mask;
    while (pool->slots[k] != NULL &&
           !(pool->slots[k]->hash == hash &&
             strcmp(pool->slots[k]->text, text) == 0)) {
        k = (k + 1) & mask;     /* linear probing: try the next slot */
    }
    return k;
}

/* Doubles the table and puts every entry into its new slot. The entries
   themselves do not move, so the cells' pointers stay valid. */
static df_status pool_grow(StringPool *pool)
{
    size_t new_cap = pool->cap ? pool->cap * 2 : POOL_FIRST_CAP;
    if (new_cap < pool->cap || new_cap > SIZE_MAX / sizeof(PoolEntry *))
        return DF_ERR_NOMEM;
    PoolEntry **new_slots = calloc(new_cap, sizeof(PoolEntry *));
    if (!new_slots) return DF_ERR_NOMEM;

    StringPool bigger = {new_slots, new_cap, pool->count};
    for (size_t k = 0; k < pool->cap; k++) {
        PoolEntry *e = pool->slots[k];
        if (e) new_slots[pool_find_slot(&bigger, e->text, e->hash)] = e;
    }
    free(pool->slots);
    *pool = bigger;
    return DF_OK;
}

/* Gives back the pooled copy of `text`, adding it if it is new. On failure
   the pool holds the same texts as before. */
static df_status pool_acquire(StringPool *pool, const char *text, char **out)
{
    uint64_t hash = df_hash_text(text);
    if (pool->cap > 0) {
        PoolEntry *found = pool->slots[pool_find_slot(pool, text, hash)];
        if (found) {
            found->refs++;
            *out = found->text;
            return DF_OK;
        }
    }

    /* New text. Keep the table at most 3/4 full so probing stays short. */
    if ((pool->count + 1) * 4 > pool->cap * 3) {
        df_status st = pool_grow(pool);
        if (st != DF_OK) return st;
    }
    size_t len = strlen(text);
    if (len > SIZE_MAX - sizeof(PoolEntry) - 1) return DF_ERR_NOMEM;
    PoolEntry *e = malloc(sizeof(PoolEntry) + len + 1);
    if (!e) return DF_ERR_NOMEM;
    e->refs = 1;
    e->hash = hash;
    memcpy(e->text, text, len + 1);

    pool->slots[pool_find_slot(pool, text, hash)] = e;
    pool->count++;
    *out = e->text;
    return DF_OK;
}

/* One cell stops using `text`. The last one frees the entry. */
static void pool_release(StringPool *pool, char *text)
{
    PoolEntry *e = entry_of(text);
    e->refs--;
    if (e->refs > 0) return;

    size_t mask = pool->cap - 1;
    size_t hole = (size_t)e->hash & mask;
    while (pool->slots[hole] != e) hole = (hole + 1) & mask;

    /* Removing it leaves a gap, and a lookup would stop at the gap too
       early. So move later entries of the same run back into the gap when
       their own home slot allows it ("backward shift deletion"). */
    size_t j = hole;
    while (1) {
        j = (j + 1) & mask;
        if (pool->slots[j] == NULL) break;
        size_t home = (size_t)pool->slots[j]->hash & mask;
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            pool->slots[hole] = pool->slots[j];
            hole = j;
        }
    }
    pool->slots[hole] = NULL;
    pool->count--;
    free(e);
}

static void pool_free(StringPool *pool)
{
    for (size_t k = 0; k < pool->cap; k++) free(pool->slots[k]);
    free(pool->slots);
    pool->slots = NULL;
    pool->cap = 0;
    pool->count = 0;
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

    Series *s = calloc(1, sizeof *s);   /* also empties the pool */
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
    pool_free(&s->pool);    /* every string cell lives in the pool */
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
    char *pooled = NULL;
    st = pool_acquire(&s->pool, v, &pooled);
    if (st != DF_OK) return st;       /* len not yet bumped: Series unchanged */
    s->data.s[s->len] = pooled;
    s->is_missing[s->len] = 0;
    s->len++;
    return DF_OK;
}

df_status series_append_bool(Series *s, int v)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_BOOL) return DF_ERR_TYPE_MISMATCH;
    df_status st = ensure_room(s);
    if (st != DF_OK) return st;
    s->data.b[s->len] = v ? 1 : 0;
    s->is_missing[s->len] = 0;
    s->len++;
    return DF_OK;
}

df_status series_append_date(Series *s, int64_t days)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_DATE) return DF_ERR_TYPE_MISMATCH;
    df_status st = ensure_room(s);
    if (st != DF_OK) return st;
    s->data.i[s->len] = days;
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
    case DTYPE_BOOL:   s->data.b[s->len] = 0;    break;
    case DTYPE_DATE:   s->data.i[s->len] = 0;    break;
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
    case DTYPE_BOOL:   return series_append_bool(dst, src->data.b[i]);
    case DTYPE_DATE:   return series_append_date(dst, src->data.i[i]);
    }
    return DF_ERR_INVALID_ARG;
}

df_status series_append_from(Series *dst, const Series *src, size_t i)
{
    if (!dst || !src) return DF_ERR_NULL_ARG;
    if (dst->dtype != src->dtype) return DF_ERR_TYPE_MISMATCH;
    if (i >= src->len) return DF_ERR_INDEX_OUT_OF_RANGE;
    return append_cell_from(dst, src, i);
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

size_t series_distinct_strings(const Series *s)
{
    if (!s || s->dtype != DTYPE_STRING) return 0;
    return s->pool.count;
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

df_status series_get_bool(const Series *s, size_t i, int *out)
{
    if (!out) return DF_ERR_NULL_ARG;
    df_status st = check_cell(s, i, DTYPE_BOOL, 1);
    if (st == DF_OK) *out = s->data.b[i];
    return st;
}

df_status series_get_date(const Series *s, size_t i, int64_t *out)
{
    if (!out) return DF_ERR_NULL_ARG;
    df_status st = check_cell(s, i, DTYPE_DATE, 1);
    if (st == DF_OK) *out = s->data.i[i];
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
    char *pooled = NULL;                /* get the new text first: */
    st = pool_acquire(&s->pool, v, &pooled);   /* failure changes nothing */
    if (st != DF_OK) return st;
    if (s->data.s[i]) pool_release(&s->pool, s->data.s[i]);
    s->data.s[i] = pooled;
    s->is_missing[i] = 0;
    return DF_OK;
}

df_status series_set_bool(Series *s, size_t i, int v)
{
    df_status st = check_cell(s, i, DTYPE_BOOL, 0);
    if (st != DF_OK) return st;
    s->data.b[i] = v ? 1 : 0;
    s->is_missing[i] = 0;
    return DF_OK;
}

df_status series_set_date(Series *s, size_t i, int64_t days)
{
    df_status st = check_cell(s, i, DTYPE_DATE, 0);
    if (st != DF_OK) return st;
    s->data.i[i] = days;
    s->is_missing[i] = 0;
    return DF_OK;
}

df_status series_set_missing(Series *s, size_t i)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (i >= s->len) return DF_ERR_INDEX_OUT_OF_RANGE;
    if (s->dtype == DTYPE_STRING && s->data.s[i]) {
        pool_release(&s->pool, s->data.s[i]);
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

/* All-or-nothing: the only step that can fail is adding v to the pool,
 * and that happens before any cell is changed. Every filled cell then
 * shares that one entry. */
df_status series_fill_missing_string(Series *s, const char *v)
{
    if (!s || !v) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_STRING) return DF_ERR_TYPE_MISMATCH;

    size_t n = series_count_missing(s);
    if (n == 0) return DF_OK;

    char *pooled = NULL;
    df_status st = pool_acquire(&s->pool, v, &pooled);
    if (st != DF_OK) return st;
    entry_of(pooled)->refs += n - 1;    /* acquire counted one cell */

    for (size_t i = 0; i < s->len; i++) {
        if (s->is_missing[i]) {
            s->data.s[i] = pooled;
            s->is_missing[i] = 0;
        }
    }
    return DF_OK;
}

df_status series_fill_missing_bool(Series *s, int v)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_BOOL) return DF_ERR_TYPE_MISMATCH;
    for (size_t i = 0; i < s->len; i++) {
        if (s->is_missing[i]) { s->data.b[i] = v ? 1 : 0; s->is_missing[i] = 0; }
    }
    return DF_OK;
}

df_status series_fill_missing_date(Series *s, int64_t days)
{
    if (!s) return DF_ERR_NULL_ARG;
    if (s->dtype != DTYPE_DATE) return DF_ERR_TYPE_MISMATCH;
    for (size_t i = 0; i < s->len; i++) {
        if (s->is_missing[i]) { s->data.i[i] = days; s->is_missing[i] = 0; }
    }
    return DF_OK;
}
