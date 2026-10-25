/*
 * dataframe.h - an ordered collection of equal-length, uniquely named Series.
 *
 * DataFrame is an OPAQUE type.
 *
 * INVARIANTS (the library maintains these; callers can rely on them)
 *   - All columns have the same length (= df_nrows).
 *   - Column names are unique and non-empty.
 *   - A frame with no columns has 0 rows. The first column added sets the
 *     row count; an empty frame may also hold zero-length columns.
 *
 * MEMORY OWNERSHIP
 *   - df_create() / df_clone() / df_select_columns() / df_drop_missing_rows()
 *     and df_make_sample() allocate; the caller owns *out and releases it with
 *     df_free().
 *   - df_add_column() TAKES OWNERSHIP of the Series on success. On failure the
 *     caller still owns it and must free it.
 *   - df_get_column*() returns a const pointer that BORROWS from the frame:
 *     valid until the frame is modified or freed. Never free it. To keep a
 *     column independently, use series_clone().
 *   - df_column_name() borrows in the same way.
 *   - Operations that "return a new DataFrame" always deep-copy; the new
 *     frame shares no memory with the source.
 *   - A failing function leaves its input frame unchanged.
 *
 * Owner: Person A
 */
#ifndef DF_DATAFRAME_H
#define DF_DATAFRAME_H

#include <stddef.h>
#include "df_error.h"
#include "series.h"

typedef struct DataFrame DataFrame;

/* ---- lifecycle ---------------------------------------------------------- */

df_status df_create(DataFrame **out);          /* empty frame: 0 cols, 0 rows */
void      df_free(DataFrame *df);              /* safe to call with NULL      */
df_status df_clone(const DataFrame *df, DataFrame **out);

/* Builds a small hard-coded frame (columns: id INT, name STRING, score FLOAT,
 * ~6 rows, includes at least one missing cell) so Person B can test against
 * real data before the CSV reader exists. */
df_status df_make_sample(DataFrame **out);

/* ---- shape -------------------------------------------------------------- */

size_t df_nrows(const DataFrame *df);
size_t df_ncols(const DataFrame *df);

/* ---- columns: add / remove / rename / look up --------------------------- */

/* Appends a column. Fails with DF_ERR_DUPLICATE_NAME if the name exists and
 * DF_ERR_LENGTH_MISMATCH if the frame already has columns and
 * series_len(s) != df_nrows(df). Takes ownership of s on DF_OK only. */
df_status df_add_column(DataFrame *df, Series *s);

df_status df_remove_column(DataFrame *df, const char *name);
df_status df_remove_column_at(DataFrame *df, size_t idx);

/* Fails with DF_ERR_DUPLICATE_NAME if new_name is already used by a
 * different column. */
df_status df_rename_column(DataFrame *df, const char *old_name,
                           const char *new_name);

/* Lookups. NOT_FOUND / INDEX_OUT_OF_RANGE on failure. */
df_status df_find_column(const DataFrame *df, const char *name, size_t *idx_out);
df_status df_get_column(const DataFrame *df, const char *name,
                        const Series **out);
df_status df_get_column_at(const DataFrame *df, size_t idx, const Series **out);
df_status df_column_name(const DataFrame *df, size_t idx, const char **out);

/* ---- selection and missing data ----------------------------------------- */

/* New frame containing deep copies of the named columns, in the order given.
 * Fails with DF_ERR_NOT_FOUND for an unknown name and
 * DF_ERR_DUPLICATE_NAME if the same name appears twice. */
df_status df_select_columns(const DataFrame *df, const char *const *names,
                            size_t n, DataFrame **out);

/* New frame with every row that has at least one missing cell removed. */
df_status df_drop_missing_rows(const DataFrame *df, DataFrame **out);

/* ---- debugging ---------------------------------------------------------- */

/* Pretty-prints up to max_rows rows to `stream` (a FILE *, passed as void *
 * to avoid pulling <stdio.h> into every includer). Missing cells print "NA".
 * Pass max_rows = 0 to print all rows. */
df_status df_print(const DataFrame *df, void *stream, size_t max_rows);

#endif /* DF_DATAFRAME_H */