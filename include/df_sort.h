/*
 * df_sort.h - sort the rows of a DataFrame by one or more columns.
 *
 * RULES
 *   - Rows are compared by the first sort column. Only when those cells are
 *     equal is the second sort column used, and so on.
 *   - Each sort column can be ascending or descending on its own.
 *   - Numbers are compared by value, strings with strcmp() (case-sensitive
 *     alphabetical order).
 *   - Missing cells always go LAST, for ascending and descending (same as
 *     pandas' default na_position="last").
 *   - The sort is STABLE: rows that are equal in every sort column keep the
 *     order they had in the input.
 *   - Algorithm: merge sort on an array of row numbers, O(n log n). The
 *     columns are then copied once in the new order.
 *
 * MEMORY OWNERSHIP
 *   - df_sort_by() / df_sort_by_column(): the caller owns *out and frees it
 *     with df_free(). It is a deep copy; the input frame is never changed.
 *   - df_sort_indices(): the caller owns *out_rows and frees it with free().
 *   - On failure the out-parameter is set to NULL and nothing is leaked.
 *
 * Owner: Person B
 */
#ifndef DF_SORT_H
#define DF_SORT_H

#include <stddef.h>
#include "df_error.h"
#include "series.h"
#include "dataframe.h"

/* Sorts by columns[0], then columns[1], ... (ncolumns of them).
 * ascending[k] != 0 means column k is sorted small-to-large, 0 means
 * large-to-small. Pass ascending = NULL to sort every column ascending.
 * Errors: DF_ERR_INVALID_ARG (ncolumns == 0), DF_ERR_NOT_FOUND (unknown
 * column name). */
df_status df_sort_by(const DataFrame *df, const char *const *columns,
                     const int *ascending, size_t ncolumns, DataFrame **out);

/* Shortcut for sorting by a single column. */
df_status df_sort_by_column(const DataFrame *df, const char *column,
                            int ascending, DataFrame **out);

/* Like df_sort_by(), but instead of a new frame it gives back the sorted
 * row numbers: (*out_rows)[0] is the row that comes first, and so on. The
 * array has df_nrows(df) entries. If the frame has 0 rows, *out_rows is
 * NULL and DF_OK is returned. (Used by group-by; also useful on its own,
 * like pandas' argsort.) */
df_status df_sort_indices(const DataFrame *df, const char *const *columns,
                          const int *ascending, size_t ncolumns,
                          size_t **out_rows);

/* Compares cell i of `a` with cell j of `b` (they may be the same Series).
 * Returns a negative number if a[i] comes first, 0 if they are equal and a
 * positive number if b[j] comes first, in ascending order.
 * Two missing cells are equal; a missing cell comes after any value.
 * Both Series must have the same dtype; if they do not, or an index is out
 * of range, 0 is returned. */
int series_compare_cells(const Series *a, size_t i, const Series *b, size_t j);

#endif /* DF_SORT_H */
