/*
 * dataframe.c - implementation of the DataFrame type declared in
 * dataframe.h.
 *
 * A DataFrame is just a growing array of Series pointers (the columns),
 * plus the number of rows. The frame owns every Series in the array and
 * frees them in df_free().
 *
 * Invariants (see dataframe.h):
 *   - every column has exactly `nrows` cells,
 *   - column names are unique,
 *   - a frame with 0 columns has 0 rows.
 */
#include "dataframe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DF_DEFAULT_CAP 4        /* room for this many columns at first */
#define PRINT_MAX_WIDTH 24      /* longest cell df_print() shows       */

struct DataFrame {
    Series **columns;   /* columns[0 .. ncols-1], owned by the frame */
    size_t   ncols;
    size_t   cap;       /* how many pointers fit in `columns`        */
    size_t   nrows;
};

/* ---- lifecycle ---------------------------------------------------------- */

df_status df_create(DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;

    /* calloc sets every field to 0 / NULL: no columns, no rows. */
    DataFrame *df = calloc(1, sizeof(DataFrame));
    if (df == NULL) {
        return DF_ERR_NOMEM;
    }
    *out = df;
    return DF_OK;
}

void df_free(DataFrame *df)
{
    if (df == NULL) {
        return;
    }
    for (size_t c = 0; c < df->ncols; c++) {
        series_free(df->columns[c]);
    }
    free(df->columns);
    free(df);
}

df_status df_clone(const DataFrame *df, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }

    DataFrame *copy = NULL;
    df_status st = df_create(&copy);
    if (st != DF_OK) {
        return st;
    }

    /* Deep copy every column, one by one. */
    for (size_t c = 0; c < df->ncols; c++) {
        Series *col = NULL;
        st = series_clone(df->columns[c], &col);
        if (st != DF_OK) {
            df_free(copy);
            return st;
        }
        st = df_add_column(copy, col);
        if (st != DF_OK) {
            series_free(col);   /* not taken by the frame */
            df_free(copy);
            return st;
        }
    }

    *out = copy;
    return DF_OK;
}

/* ---- the sample table --------------------------------------------------- */

df_status df_make_sample(DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;

    /* 6 students. Chen's score is missing (marked with -1 below). */
    const char *names[6] = {"Asha", "Ben", "Chen", "Dev", "Esha", "Farid"};
    double scores[6] = {88.5, 72.0, -1, 95.0, 72.0, 60.25};

    Series *id = NULL;
    Series *name = NULL;
    Series *score = NULL;
    DataFrame *df = NULL;

    df_status st = series_create("id", DTYPE_INT, 6, &id);
    if (st == DF_OK) st = series_create("name", DTYPE_STRING, 6, &name);
    if (st == DF_OK) st = series_create("score", DTYPE_FLOAT, 6, &score);

    for (int i = 0; i < 6 && st == DF_OK; i++) {
        st = series_append_int(id, i + 1);
        if (st == DF_OK) {
            st = series_append_string(name, names[i]);
        }
        if (st == DF_OK) {
            if (scores[i] < 0) {
                st = series_append_missing(score);
            } else {
                st = series_append_float(score, scores[i]);
            }
        }
    }

    if (st == DF_OK) {
        st = df_create(&df);
    }

    /* Hand each column to the frame. After a successful df_add_column the
       frame owns the Series, so we set our pointer to NULL to make sure we
       don't free it a second time below. */
    if (st == DF_OK) {
        st = df_add_column(df, id);
        if (st == DF_OK) id = NULL;
    }
    if (st == DF_OK) {
        st = df_add_column(df, name);
        if (st == DF_OK) name = NULL;
    }
    if (st == DF_OK) {
        st = df_add_column(df, score);
        if (st == DF_OK) score = NULL;
    }

    if (st != DF_OK) {
        /* series_free and df_free accept NULL, so this is always safe */
        series_free(id);
        series_free(name);
        series_free(score);
        df_free(df);
        return st;
    }
    *out = df;
    return DF_OK;
}

/* ---- shape -------------------------------------------------------------- */

size_t df_nrows(const DataFrame *df)
{
    if (df == NULL) {
        return 0;
    }
    return df->nrows;
}

size_t df_ncols(const DataFrame *df)
{
    if (df == NULL) {
        return 0;
    }
    return df->ncols;
}

/* ---- columns ------------------------------------------------------------ */

df_status df_find_column(const DataFrame *df, const char *name, size_t *idx_out)
{
    if (df == NULL || name == NULL || idx_out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    for (size_t c = 0; c < df->ncols; c++) {
        if (strcmp(series_name(df->columns[c]), name) == 0) {
            *idx_out = c;
            return DF_OK;
        }
    }
    return DF_ERR_NOT_FOUND;
}

df_status df_add_column(DataFrame *df, Series *s)
{
    if (df == NULL || s == NULL) {
        return DF_ERR_NULL_ARG;
    }

    /* Check everything first, so a failure changes nothing. */
    size_t existing = 0;
    if (df_find_column(df, series_name(s), &existing) == DF_OK) {
        return DF_ERR_DUPLICATE_NAME;
    }
    if (df->ncols > 0 && series_len(s) != df->nrows) {
        return DF_ERR_LENGTH_MISMATCH;
    }

    /* Make the array bigger when it is full (double its size). */
    if (df->ncols == df->cap) {
        size_t new_cap = DF_DEFAULT_CAP;
        if (df->cap > 0) {
            new_cap = df->cap * 2;
        }
        /* temporary pointer: if realloc fails the old array is still ok */
        Series **bigger = realloc(df->columns, new_cap * sizeof(Series *));
        if (bigger == NULL) {
            return DF_ERR_NOMEM;
        }
        df->columns = bigger;
        df->cap = new_cap;
    }

    /* The first column decides how many rows the frame has. */
    if (df->ncols == 0) {
        df->nrows = series_len(s);
    }
    df->columns[df->ncols] = s;    /* the frame owns s from now on */
    df->ncols++;
    return DF_OK;
}

df_status df_remove_column_at(DataFrame *df, size_t idx)
{
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (idx >= df->ncols) {
        return DF_ERR_INDEX_OUT_OF_RANGE;
    }

    series_free(df->columns[idx]);

    /* Move the columns after idx one place to the left to fill the gap. */
    for (size_t c = idx; c + 1 < df->ncols; c++) {
        df->columns[c] = df->columns[c + 1];
    }
    df->ncols--;

    /* A frame with no columns has no rows. */
    if (df->ncols == 0) {
        df->nrows = 0;
    }
    return DF_OK;
}

df_status df_remove_column(DataFrame *df, const char *name)
{
    if (df == NULL || name == NULL) {
        return DF_ERR_NULL_ARG;
    }
    size_t idx = 0;
    df_status st = df_find_column(df, name, &idx);
    if (st != DF_OK) {
        return st;
    }
    return df_remove_column_at(df, idx);
}

df_status df_rename_column(DataFrame *df, const char *old_name,
                           const char *new_name)
{
    if (df == NULL || old_name == NULL || new_name == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (new_name[0] == '\0') {
        return DF_ERR_INVALID_ARG;
    }

    size_t idx = 0;
    df_status st = df_find_column(df, old_name, &idx);
    if (st != DF_OK) {
        return st;
    }

    /* The new name may only be used by this same column (renaming a
       column to its own name is allowed and does nothing). */
    size_t other = 0;
    if (df_find_column(df, new_name, &other) == DF_OK && other != idx) {
        return DF_ERR_DUPLICATE_NAME;
    }
    return series_rename(df->columns[idx], new_name);
}

df_status df_get_column_at(const DataFrame *df, size_t idx, const Series **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (idx >= df->ncols) {
        return DF_ERR_INDEX_OUT_OF_RANGE;
    }
    *out = df->columns[idx];
    return DF_OK;
}

df_status df_get_column(const DataFrame *df, const char *name,
                        const Series **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || name == NULL) {
        return DF_ERR_NULL_ARG;
    }
    size_t idx = 0;
    df_status st = df_find_column(df, name, &idx);
    if (st != DF_OK) {
        return st;
    }
    *out = df->columns[idx];
    return DF_OK;
}

df_status df_column_name(const DataFrame *df, size_t idx, const char **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    const Series *col = NULL;
    df_status st = df_get_column_at(df, idx, &col);
    if (st != DF_OK) {
        return st;
    }
    *out = series_name(col);
    return DF_OK;
}

/* ---- selection and missing data ----------------------------------------- */

df_status df_select_columns(const DataFrame *df, const char *const *names,
                            size_t n, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL || (n > 0 && names == NULL)) {
        return DF_ERR_NULL_ARG;
    }

    /* Check all the names before copying anything. */
    for (size_t k = 0; k < n; k++) {
        if (names[k] == NULL) {
            return DF_ERR_NULL_ARG;
        }
        size_t idx = 0;
        df_status st = df_find_column(df, names[k], &idx);
        if (st != DF_OK) {
            return st;
        }
        /* the same name twice is not allowed */
        for (size_t j = 0; j < k; j++) {
            if (strcmp(names[j], names[k]) == 0) {
                return DF_ERR_DUPLICATE_NAME;
            }
        }
    }

    DataFrame *result = NULL;
    df_status st = df_create(&result);
    if (st != DF_OK) {
        return st;
    }
    for (size_t k = 0; k < n; k++) {
        const Series *src = NULL;
        df_get_column(df, names[k], &src);      /* checked above */
        Series *copy = NULL;
        st = series_clone(src, &copy);
        if (st == DF_OK) {
            st = df_add_column(result, copy);
            if (st != DF_OK) {
                series_free(copy);
            }
        }
        if (st != DF_OK) {
            df_free(result);
            return st;
        }
    }

    *out = result;
    return DF_OK;
}

df_status df_drop_missing_rows(const DataFrame *df, DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (df == NULL) {
        return DF_ERR_NULL_ARG;
    }

    /* Write down every row that has no missing cell in any column. */
    size_t *keep = NULL;
    if (df->nrows > 0) {
        keep = malloc(df->nrows * sizeof(size_t));
        if (keep == NULL) {
            return DF_ERR_NOMEM;
        }
    }
    size_t count = 0;
    for (size_t r = 0; r < df->nrows; r++) {
        int has_missing = 0;
        for (size_t c = 0; c < df->ncols; c++) {
            if (series_is_missing(df->columns[c], r)) {
                has_missing = 1;
                break;
            }
        }
        if (!has_missing) {
            keep[count] = r;
            count++;
        }
    }

    /* Copy just those rows of every column. */
    DataFrame *result = NULL;
    df_status st = df_create(&result);
    for (size_t c = 0; c < df->ncols && st == DF_OK; c++) {
        Series *part = NULL;
        st = series_take(df->columns[c], keep, count, &part);
        if (st == DF_OK) {
            st = df_add_column(result, part);
            if (st != DF_OK) {
                series_free(part);
            }
        }
    }
    free(keep);

    if (st != DF_OK) {
        df_free(result);
        return st;
    }
    *out = result;
    return DF_OK;
}

/* ---- printing ----------------------------------------------------------- */

/* Writes cell `row` of `col` as text into buf, e.g. "42", "3.5", "Asha" or
   "NA". Text longer than PRINT_MAX_WIDTH is cut and ends with "...". */
static void format_cell(const Series *col, size_t row, char *buf, size_t size)
{
    if (series_is_missing(col, row)) {
        snprintf(buf, size, "NA");
        return;
    }

    if (series_dtype(col) == DTYPE_INT) {
        int64_t v = 0;
        series_get_int(col, row, &v);
        snprintf(buf, size, "%lld", (long long)v);
    } else if (series_dtype(col) == DTYPE_FLOAT) {
        double v = 0.0;
        series_get_float(col, row, &v);
        snprintf(buf, size, "%g", v);
    } else {
        const char *v = NULL;
        series_get_string(col, row, &v);
        if (strlen(v) > PRINT_MAX_WIDTH) {
            /* keep the first PRINT_MAX_WIDTH-3 characters, then "..." */
            snprintf(buf, size, "%.*s...", PRINT_MAX_WIDTH - 3, v);
        } else {
            snprintf(buf, size, "%s", v);
        }
    }
}

df_status df_print(const DataFrame *df, void *stream, size_t max_rows)
{
    if (df == NULL || stream == NULL) {
        return DF_ERR_NULL_ARG;
    }
    FILE *f = stream;

    /* How many rows to show (0 means all of them). */
    size_t shown = df->nrows;
    if (max_rows > 0 && max_rows < shown) {
        shown = max_rows;
    }

    /* Width of each column = the longest text in it (name or cell). */
    size_t *widths = NULL;
    if (df->ncols > 0) {
        widths = malloc(df->ncols * sizeof(size_t));
        if (widths == NULL) {
            return DF_ERR_NOMEM;
        }
    }
    char cell[PRINT_MAX_WIDTH + 40];
    for (size_t c = 0; c < df->ncols; c++) {
        widths[c] = strlen(series_name(df->columns[c]));
        for (size_t r = 0; r < shown; r++) {
            format_cell(df->columns[c], r, cell, sizeof cell);
            if (strlen(cell) > widths[c]) {
                widths[c] = strlen(cell);
            }
        }
    }

    /* Width of the row-number column on the left. */
    char number[32];
    snprintf(number, sizeof number, "%zu", shown > 0 ? shown - 1 : 0);
    int index_width = (int)strlen(number);

    /* Header line: the column names, lined up the same way as the cells
       below them (numbers right-aligned, text left-aligned). */
    fprintf(f, "%*s", index_width, "");
    for (size_t c = 0; c < df->ncols; c++) {
        const char *name = series_name(df->columns[c]);
        if (series_dtype(df->columns[c]) == DTYPE_STRING) {
            fprintf(f, "  %-*s", (int)widths[c], name);
        } else {
            fprintf(f, "  %*s", (int)widths[c], name);
        }
    }
    fprintf(f, "\n");

    /* One line per row. Numbers are right-aligned, text left-aligned. */
    for (size_t r = 0; r < shown; r++) {
        fprintf(f, "%*zu", index_width, r);
        for (size_t c = 0; c < df->ncols; c++) {
            format_cell(df->columns[c], r, cell, sizeof cell);
            if (series_dtype(df->columns[c]) == DTYPE_STRING) {
                fprintf(f, "  %-*s", (int)widths[c], cell);
            } else {
                fprintf(f, "  %*s", (int)widths[c], cell);
            }
        }
        fprintf(f, "\n");
    }

    if (shown < df->nrows) {
        fprintf(f, "... (%zu more rows)\n", df->nrows - shown);
    }
    fprintf(f, "[%zu rows x %zu columns]\n", df->nrows, df->ncols);

    free(widths);
    return DF_OK;
}
