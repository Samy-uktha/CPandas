/*
 * df_sort.c - sorting rows by one or more columns.
 *
 * We never move the actual data while sorting. Instead we sort an array of
 * row numbers {0, 1, 2, ...} using merge sort, comparing the rows they
 * point to. At the end df_take_rows() copies the rows in the new order.
 *
 * Why merge sort and not qsort()?
 *   - qsort() is not stable (equal rows may change order), and its compare
 *     function cannot be given extra data (which columns, which direction)
 *     without using a global variable.
 *   - Merge sort is stable and always O(n log n).
 */
#include "df_sort.h"
#include "df_rows.h"

#include <stdlib.h>
#include <string.h>

/* Everything the compare function needs to know about the sort. */
typedef struct {
    const Series **columns;   /* the sort columns, in priority order    */
    const int     *ascending; /* NULL means every column is ascending   */
    size_t         ncolumns;
} SortInfo;

/* ---- comparing cells ---------------------------------------------------- */

int series_compare_cells(const Series *a, size_t i, const Series *b, size_t j)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (series_dtype(a) != series_dtype(b)) {
        return 0;
    }
    if (i >= series_len(a) || j >= series_len(b)) {
        return 0;
    }

    /* Missing cells: equal to each other, and after every real value. */
    int a_missing = series_is_missing(a, i);
    int b_missing = series_is_missing(b, j);
    if (a_missing && b_missing) {
        return 0;
    }
    if (a_missing) {
        return 1;
    }
    if (b_missing) {
        return -1;
    }

    if (series_dtype(a) == DTYPE_INT) {
        int64_t x = 0, y = 0;
        series_get_int(a, i, &x);
        series_get_int(b, j, &y);
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    if (series_dtype(a) == DTYPE_FLOAT) {
        double x = 0.0, y = 0.0;
        series_get_float(a, i, &x);
        series_get_float(b, j, &y);
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    /* DTYPE_STRING */
    const char *x = NULL;
    const char *y = NULL;
    series_get_string(a, i, &x);
    series_get_string(b, j, &y);
    int diff = strcmp(x, y);
    if (diff < 0) return -1;
    if (diff > 0) return 1;
    return 0;
}

/* Compares two rows using all the sort columns, one after another.
   Negative: row_a goes first, positive: row_b goes first, 0: equal. */
static int compare_rows(const SortInfo *info, size_t row_a, size_t row_b)
{
    for (size_t k = 0; k < info->ncolumns; k++) {
        const Series *col = info->columns[k];

        /* Missing cells go last in BOTH directions, so check them before
           the result gets flipped for descending order. */
        int a_missing = series_is_missing(col, row_a);
        int b_missing = series_is_missing(col, row_b);
        if (a_missing && b_missing) {
            continue;           /* equal in this column, try the next one */
        }
        if (a_missing) {
            return 1;
        }
        if (b_missing) {
            return -1;
        }

        int diff = series_compare_cells(col, row_a, col, row_b);
        if (diff != 0) {
            if (info->ascending != NULL && info->ascending[k] == 0) {
                diff = -diff;   /* descending: flip the order */
            }
            return diff;
        }
        /* equal in this column: look at the next sort column */
    }
    return 0;   /* equal in every sort column */
}

/* ---- merge sort --------------------------------------------------------- */

/* Sorts rows[left .. right-1]. `temp` is a helper array of the same size
   as `rows`. */
static void merge_sort(size_t *rows, size_t *temp, size_t left, size_t right,
                       const SortInfo *info)
{
    if (right - left < 2) {
        return;     /* 0 or 1 element is already sorted */
    }

    /* 1. Sort the left half and the right half separately. */
    size_t mid = left + (right - left) / 2;
    merge_sort(rows, temp, left, mid, info);
    merge_sort(rows, temp, mid, right, info);

    /* 2. Merge the two sorted halves into temp. */
    size_t i = left;    /* next element of the left half  */
    size_t j = mid;     /* next element of the right half */
    size_t k = left;    /* next free place in temp        */
    while (i < mid && j < right) {
        /* Take from the right half only if it is strictly smaller. On a
           tie the left one goes first, which keeps the sort stable. */
        if (compare_rows(info, rows[j], rows[i]) < 0) {
            temp[k] = rows[j];
            j++;
        } else {
            temp[k] = rows[i];
            i++;
        }
        k++;
    }
    while (i < mid) {           /* leftovers from the left half */
        temp[k] = rows[i];
        i++;
        k++;
    }
    while (j < right) {         /* leftovers from the right half */
        temp[k] = rows[j];
        j++;
        k++;
    }

    /* 3. Copy the merged part back. */
    for (k = left; k < right; k++) {
        rows[k] = temp[k];
    }
}

/* ---- public functions --------------------------------------------------- */

df_status df_sort_indices(const DataFrame *df, const char *const *columns,
                          const int *ascending, size_t ncolumns,
                          size_t **out_rows)
{
    if (out_rows == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out_rows = NULL;
    if (df == NULL || columns == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (ncolumns == 0) {
        return DF_ERR_INVALID_ARG;
    }

    /* Look up every sort column by name. */
    const Series **cols = malloc(ncolumns * sizeof(const Series *));
    if (cols == NULL) {
        return DF_ERR_NOMEM;
    }
    for (size_t k = 0; k < ncolumns; k++) {
        if (columns[k] == NULL) {
            free(cols);
            return DF_ERR_NULL_ARG;
        }
        df_status st = df_get_column(df, columns[k], &cols[k]);
        if (st != DF_OK) {
            free(cols);
            return st;
        }
    }

    /* Nothing to sort in an empty frame. */
    size_t nrows = df_nrows(df);
    if (nrows == 0) {
        free(cols);
        return DF_OK;
    }

    size_t *rows = malloc(nrows * sizeof(size_t));
    size_t *temp = malloc(nrows * sizeof(size_t));
    if (rows == NULL || temp == NULL) {
        free(rows);
        free(temp);
        free(cols);
        return DF_ERR_NOMEM;
    }

    /* Start with the rows in their original order: 0, 1, 2, ... */
    for (size_t r = 0; r < nrows; r++) {
        rows[r] = r;
    }

    SortInfo info;
    info.columns = cols;
    info.ascending = ascending;
    info.ncolumns = ncolumns;
    merge_sort(rows, temp, 0, nrows, &info);

    free(temp);
    free(cols);
    *out_rows = rows;
    return DF_OK;
}

df_status df_sort_by(const DataFrame *df, const char *const *columns,
                     const int *ascending, size_t ncolumns, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;

    size_t *rows = NULL;
    df_status st = df_sort_indices(df, columns, ascending, ncolumns, &rows);
    if (st != DF_OK) {
        return st;
    }

    /* Copy the rows in sorted order (rows is NULL when the frame is empty,
       which df_take_rows accepts because the count is 0 too). */
    st = df_take_rows(df, rows, df_nrows(df), out);
    free(rows);
    return st;
}

df_status df_sort_by_column(const DataFrame *df, const char *column,
                            int ascending, DataFrame **out)
{
    /* A list with one column name and one direction. */
    const char *columns[1];
    int directions[1];
    columns[0] = column;
    directions[0] = ascending;
    return df_sort_by(df, columns, directions, 1, out);
}
