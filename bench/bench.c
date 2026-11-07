/*
 * bench.c - times the main CPandas operations on a big CSV file.
 *
 *     build/bench employees.csv bonus.csv
 *
 * (bench/run.sh makes the files, builds this program with -O2 and no
 * sanitizers, and runs bench_pandas.py on the same files for comparison.)
 *
 * Every line of output is "operation|seconds", so the two programs' output
 * can be put side by side. The operations match bench_pandas.py one for one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cpandas.h"

#define OLD_JOIN_ROWS 20000     /* rows for the nested-loop comparison */

static double now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);        /* C11 */
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Stops the program with a message if a step failed. */
static void must(df_status st, const char *what)
{
    if (st != DF_OK) {
        fprintf(stderr, "%s failed: %s\n", what, df_strerror(st));
        exit(1);
    }
}

static void report(const char *what, double seconds)
{
    printf("%s|%.4f\n", what, seconds);
    fflush(stdout);
}

/* For the chunked read: adds up the salaries of every chunk. */
static df_status add_salaries(const DataFrame *chunk, size_t first_row,
                              void *user_data)
{
    (void)first_row;
    double *total = user_data;
    const Series *salary = NULL;
    double sum = 0.0;
    if (df_get_column(chunk, "salary", &salary) == DF_OK &&
        series_sum(salary, &sum) == DF_OK) {
        *total += sum;
    }
    return DF_OK;
}

/* The join the first version of CPandas did: compare every pair of rows.
   Only counts the matches, which is the part that takes the time. */
static size_t nested_loop_join(const DataFrame *left, const DataFrame *right)
{
    const Series *a = NULL;
    const Series *b = NULL;
    df_get_column(left, "id", &a);
    df_get_column(right, "id", &b);
    size_t matches = 0;
    for (size_t i = 0; i < series_len(a); i++) {
        for (size_t j = 0; j < series_len(b); j++) {
            if (series_compare_cells(a, i, b, j) == 0) {
                matches++;
            }
        }
    }
    return matches;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s employees.csv bonus.csv\n", argv[0]);
        return 2;
    }
    double t = 0.0;
    DataFrame *df = NULL;
    DataFrame *bonus = NULL;
    DataFrame *out = NULL;

    t = now();
    must(df_read_csv(argv[1], NULL, &df), "read_csv");
    report("read_csv employees", now() - t);

    t = now();
    must(df_read_csv(argv[2], NULL, &bonus), "read_csv");
    report("read_csv bonus", now() - t);

    t = now();
    must(df_filter_number(df, "salary", DF_OP_GT, 50000, &out), "filter");
    report("filter salary > 50000", now() - t);
    df_free(out);

    const char *sort_cols[] = {"dept", "salary"};
    int ascending[] = {1, 0};
    t = now();
    must(df_sort_by(df, sort_cols, ascending, 2, &out), "sort");
    report("sort dept, salary desc", now() - t);
    df_free(out);

    df_aggregation aggs[] = {{"salary", DF_AGG_MEAN}, {"salary", DF_AGG_COUNT}};
    t = now();
    must(df_groupby(df, "dept", aggs, 2, &out), "groupby");
    report("groupby dept", now() - t);
    df_free(out);

    const char *keys[] = {"dept", "city"};
    t = now();
    must(df_groupby_multi(df, keys, 2, aggs, 1, &out), "groupby");
    report("groupby dept, city", now() - t);
    df_free(out);

    df_aggregation unique[] = {{"name", DF_AGG_NUNIQUE}};
    t = now();
    must(df_groupby(df, "city", unique, 1, &out), "groupby");
    report("groupby city, nunique name", now() - t);
    df_free(out);

    t = now();
    must(df_merge(df, bonus, "id", DF_JOIN_INNER, &out), "merge");
    report("merge on id (inner)", now() - t);
    df_free(out);

    t = now();
    must(df_describe(df, &out), "describe");
    report("describe", now() - t);
    df_free(out);

    DataFrame *copy = NULL;
    must(df_clone(df, &copy), "clone");
    t = now();
    must(df_add_scalar_column(copy, "tax", "salary", DF_ARITH_MUL, 0.1),
         "compute");
    report("new column salary * 0.1", now() - t);
    df_free(copy);

    const DataFrame *parts[] = {df, df};
    t = now();
    must(df_concat(parts, 2, &out), "concat");
    report("concat with itself", now() - t);
    df_free(out);

    t = now();
    must(df_write_csv(df, "build/bench_out.csv", NULL), "write_csv");
    report("write_csv", now() - t);
    remove("build/bench_out.csv");

    double total = 0.0;
    t = now();
    must(df_read_csv_chunks(argv[1], NULL, 100000, add_salaries, &total),
         "read_csv_chunks");
    report("read in 100k-row chunks", now() - t);

    /* Old vs new join, on a slice small enough for the old one. */
    DataFrame *small_left = NULL;
    DataFrame *small_right = NULL;
    must(df_head(df, OLD_JOIN_ROWS, &small_left), "head");
    must(df_head(bonus, OLD_JOIN_ROWS, &small_right), "head");
    t = now();
    size_t matches = nested_loop_join(small_left, small_right);
    report("join 20k x 20k, nested loops (old)", now() - t);
    t = now();
    must(df_merge(small_left, small_right, "id", DF_JOIN_INNER, &out),
         "merge");
    report("join 20k x 20k, hash (new)", now() - t);
    if (df_nrows(out) != matches) {
        fprintf(stderr, "old and new join disagree: %zu vs %zu rows\n",
                matches, df_nrows(out));
        return 1;
    }
    df_free(out);
    df_free(small_left);
    df_free(small_right);

    /* Memory: how many texts the 'dept' column really stores. */
    const Series *dept = NULL;
    df_get_column(df, "dept", &dept);
    fprintf(stderr, "dept column: %zu cells, %zu stored texts\n",
            series_len(dept), series_distinct_strings(dept));

    df_free(df);
    df_free(bonus);
    return 0;
}
