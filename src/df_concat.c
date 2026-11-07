/*
 * df_concat.c - stack DataFrames on top of each other.
 *
 * How it works:
 *   1. Make a plan: the list of result columns (name and type), going
 *      through the frames' columns in order and merging equal names.
 *   2. For every planned column, append the cells of each frame in turn:
 *      its own cells if it has that column (INT turned into FLOAT where
 *      the plan says FLOAT), otherwise missing cells.
 */
#include "df_concat.h"

#include <stdlib.h>
#include <string.h>

/* One column of the result. */
typedef struct {
    const char *name;   /* borrowed from the first frame that has it */
    df_dtype    type;
} PlannedColumn;

/* The type for a column that is `a` in one frame and `b` in another, or
   -1 if they cannot be put in one column. */
static int combined_type(df_dtype a, df_dtype b)
{
    if (a == b) {
        return (int)a;
    }
    if ((a == DTYPE_INT && b == DTYPE_FLOAT) ||
        (a == DTYPE_FLOAT && b == DTYPE_INT)) {
        return (int)DTYPE_FLOAT;
    }
    return -1;
}

/* Step 1. On success the caller frees *out_plan. */
static df_status make_plan(const DataFrame *const *frames, size_t nframes,
                           PlannedColumn **out_plan, size_t *out_count)
{
    /* There can't be more result columns than columns in all frames. */
    size_t max_cols = 0;
    for (size_t f = 0; f < nframes; f++) {
        max_cols += df_ncols(frames[f]);
    }
    PlannedColumn *plan = malloc((max_cols + 1) * sizeof(PlannedColumn));
    if (plan == NULL) {
        return DF_ERR_NOMEM;
    }

    size_t count = 0;
    for (size_t f = 0; f < nframes; f++) {
        size_t ncols = df_ncols(frames[f]);
        for (size_t c = 0; c < ncols; c++) {
            const Series *col = NULL;
            df_get_column_at(frames[f], c, &col);

            /* Is this name already planned? */
            size_t k = 0;
            while (k < count && strcmp(plan[k].name, series_name(col)) != 0) {
                k++;
            }
            if (k == count) {
                plan[count].name = series_name(col);    /* a new column */
                plan[count].type = series_dtype(col);
                count++;
                continue;
            }
            int type = combined_type(plan[k].type, series_dtype(col));
            if (type < 0) {
                free(plan);
                return DF_ERR_TYPE_MISMATCH;
            }
            plan[k].type = (df_dtype)type;
        }
    }

    *out_plan = plan;
    *out_count = count;
    return DF_OK;
}

/* Appends every cell of `src` to `dst`. dst is either the same type, or
   FLOAT while src is INT. */
static df_status append_all(Series *dst, const Series *src)
{
    size_t n = series_len(src);
    df_status st = DF_OK;
    if (series_dtype(dst) == series_dtype(src)) {
        for (size_t i = 0; i < n && st == DF_OK; i++) {
            st = series_append_from(dst, src, i);
        }
        return st;
    }
    for (size_t i = 0; i < n && st == DF_OK; i++) {
        int64_t v = 0;
        if (series_get_int(src, i, &v) == DF_OK) {
            st = series_append_float(dst, (double)v);
        } else {
            st = series_append_missing(dst);    /* a missing cell */
        }
    }
    return st;
}

/* Step 2 for one planned column. */
static df_status build_column(const DataFrame *const *frames, size_t nframes,
                              const PlannedColumn *planned, size_t total_rows,
                              Series **out)
{
    Series *s = NULL;
    df_status st = series_create(planned->name, planned->type, total_rows, &s);
    for (size_t f = 0; f < nframes && st == DF_OK; f++) {
        const Series *src = NULL;
        if (df_get_column(frames[f], planned->name, &src) == DF_OK) {
            st = append_all(s, src);
        } else {
            size_t n = df_nrows(frames[f]);
            for (size_t i = 0; i < n && st == DF_OK; i++) {
                st = series_append_missing(s);
            }
        }
    }
    if (st != DF_OK) {
        series_free(s);     /* safe with NULL */
        return st;
    }
    *out = s;
    return DF_OK;
}

df_status df_concat(const DataFrame *const *frames, size_t nframes,
                    DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (frames == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (nframes == 0) {
        return DF_ERR_INVALID_ARG;
    }
    size_t total_rows = 0;
    for (size_t f = 0; f < nframes; f++) {
        if (frames[f] == NULL) {
            return DF_ERR_NULL_ARG;
        }
        total_rows += df_nrows(frames[f]);
    }

    PlannedColumn *plan = NULL;
    size_t ncols = 0;
    df_status st = make_plan(frames, nframes, &plan, &ncols);
    if (st != DF_OK) {
        return st;
    }

    DataFrame *result = NULL;
    st = df_create(&result);
    for (size_t k = 0; k < ncols && st == DF_OK; k++) {
        Series *col = NULL;
        st = build_column(frames, nframes, &plan[k], total_rows, &col);
        if (st == DF_OK) {
            st = df_add_column(result, col);
            if (st != DF_OK) {
                series_free(col);
            }
        }
    }
    free(plan);

    if (st != DF_OK) {
        df_free(result);
        return st;
    }
    *out = result;
    return DF_OK;
}
