/*
 * series.h - a single typed, named column of values with missing-value support.
 *
 * Series is an OPAQUE type: use the accessors below, never touch the fields.
 * That lets Person A change the internal layout without breaking Person B.
 *
 * MEMORY OWNERSHIP
 *   - series_create() allocates; the caller owns the result and must release
 *     it with series_free() -- unless it is handed to df_add_column(), which
 *     takes ownership on success.
 *   - A Series owns its name and every string cell. The append/rename
 *     functions COPY the strings you pass in; you keep ownership of yours.
 *   - Pointers returned by series_name() and series_get_string() borrow from
 *     the Series: valid until the Series is modified or freed. Do not free
 *     them; copy them if you need them longer.
 *   - Every function that fails leaves the Series unchanged.
 *
 * Owner: Person A
 */
#ifndef DF_SERIES_H
#define DF_SERIES_H

#include <stddef.h>
#include <stdint.h>
#include "error.h"

typedef enum {
    DTYPE_INT,    /* int64_t */
    DTYPE_FLOAT,  /* double  */
    DTYPE_STRING  /* char *  */
} df_dtype;

typedef struct Series Series;

/* Pass as an index to series_take() to produce a missing cell in that slot
 * (useful for the unmatched side of left/outer joins). */
#define SERIES_TAKE_MISSING ((size_t)-1)

/* ---- lifecycle ---------------------------------------------------------- */

/* Creates an empty Series. `name` is copied and must be non-empty.
 * `initial_cap` may be 0 (a small default is used). */
df_status series_create(const char *name, df_dtype dtype,
                        size_t initial_cap, Series **out);

/* Frees the Series and everything it owns. Safe to call with NULL. */
void series_free(Series *s);

/* Deep copy. Caller owns *out. */
df_status series_clone(const Series *s, Series **out);

/* Deep copy of the rows listed in indices[0..n-1], in that order. Indices may
 * repeat. An index equal to SERIES_TAKE_MISSING yields a missing cell; any
 * other index >= series_len(s) fails with DF_ERR_INDEX_OUT_OF_RANGE.
 * This is the one primitive that filter, sort, slice and join all build on.
 * The result keeps the source's name and dtype. */
df_status series_take(const Series *s, const size_t *indices, size_t n,
                      Series **out);

/* ---- appending (grows storage automatically) ---------------------------- */

df_status series_append_int(Series *s, int64_t v);        /* DTYPE_INT only    */
df_status series_append_float(Series *s, double v);       /* DTYPE_FLOAT only  */
df_status series_append_string(Series *s, const char *v); /* DTYPE_STRING only; copies v */
df_status series_append_missing(Series *s);               /* any dtype         */

/* ---- inspection --------------------------------------------------------- */

const char *series_name(const Series *s);
df_dtype    series_dtype(const Series *s);
size_t      series_len(const Series *s);
size_t      series_count_missing(const Series *s);

/* Returns 1 if cell `i` is missing, 0 if present or if i is out of range. */
int series_is_missing(const Series *s, size_t i);

/* Typed reads. Fail with DF_ERR_TYPE_MISMATCH on the wrong dtype,
 * DF_ERR_INDEX_OUT_OF_RANGE on a bad index, DF_ERR_MISSING_VALUE on a missing
 * cell. On success the value is written to *out. */
df_status series_get_int(const Series *s, size_t i, int64_t *out);
df_status series_get_float(const Series *s, size_t i, double *out);
df_status series_get_string(const Series *s, size_t i, const char **out);

/* ---- modification ------------------------------------------------------- */

/* Renames the Series (name copied). NOTE: if the Series already belongs to a
 * DataFrame, rename through df_rename_column() instead, so the frame can
 * enforce unique names. */
df_status series_rename(Series *s, const char *new_name);

/* Overwrites cell i; same errors as the typed getters (minus MISSING_VALUE).
 * Setting a cell clears its missing flag. */
df_status series_set_int(Series *s, size_t i, int64_t v);
df_status series_set_float(Series *s, size_t i, double v);
df_status series_set_string(Series *s, size_t i, const char *v);
df_status series_set_missing(Series *s, size_t i);

/* Replace every missing cell with a value (type must match the dtype). */
df_status series_fill_missing_int(Series *s, int64_t v);
df_status series_fill_missing_float(Series *s, double v);
df_status series_fill_missing_string(Series *s, const char *v);

#endif /* DF_SERIES_H */