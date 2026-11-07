/*
 * basic.c - a short example of using the CPandas library from C code.
 *
 * Usage:   basic [employees.csv] [departments.csv]
 * (default: the files in examples/data/, so run it from the project folder)
 *
 * It does, step by step, what you would write in pandas as:
 *     df   = pd.read_csv("employees.csv")
 *     high = df[df.salary > 40000].sort_values("salary", ascending=False)
 *     avg  = df.groupby("dept").agg(salary_mean=("salary", "mean"))
 *     both = pd.merge(df, pd.read_csv("departments.csv"), on="dept", how="left")
 *     both.to_csv("build/basic_output.csv")
 *
 * Every library call returns a df_status. We check each one: if a step
 * fails we print why and skip the remaining steps. At the end everything we
 * created is freed (df_free accepts NULL, so frames that were never created
 * are fine).
 */
#include <stdio.h>

#include "cpandas.h"

/* Prints an error message if a step failed. Returns 1 if it succeeded. */
static int step_ok(df_status st, const char *what)
{
    if (st != DF_OK) {
        printf("Could not %s: %s\n", what, df_strerror(st));
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *employees_path = "examples/data/employees.csv";
    const char *departments_path = "examples/data/departments.csv";
    if (argc > 1) {
        employees_path = argv[1];
    }
    if (argc > 2) {
        departments_path = argv[2];
    }

    DataFrame *df = NULL;
    DataFrame *high = NULL;
    DataFrame *sorted = NULL;
    DataFrame *averages = NULL;
    DataFrame *departments = NULL;
    DataFrame *joined = NULL;
    int ok = 1;

    /* 1. Load the CSV file. */
    ok = step_ok(df_read_csv(employees_path, NULL, &df), "read the CSV file");
    if (ok) {
        printf("== The whole table\n");
        df_print(df, stdout, 0);
    }

    /* 2. Statistics of one column. */
    if (ok) {
        const Series *salary = NULL;
        double mean = 0.0;
        double median = 0.0;
        size_t count = 0;
        ok = step_ok(df_get_column(df, "salary", &salary), "find 'salary'");
        if (ok) {
            series_count(salary, &count);
            series_mean(salary, &mean);
            series_median(salary, &median);
            printf("\n== salary: %zu values, mean %.2f, median %.2f\n",
                   count, mean, median);
        }
    }

    /* 3. Filter, then sort the result. */
    if (ok) {
        ok = step_ok(df_filter_number(df, "salary", DF_OP_GT, 40000, &high),
                     "filter");
    }
    if (ok) {
        ok = step_ok(df_sort_by_column(high, "salary", 0, &sorted), "sort");
    }
    if (ok) {
        printf("\n== salary > 40000, highest first\n");
        df_print(sorted, stdout, 0);
    }

    /* 4. Group by department. */
    if (ok) {
        df_aggregation aggs[] = {
            {"salary", DF_AGG_MEAN},
            {"name", DF_AGG_COUNT}
        };
        ok = step_ok(df_groupby(df, "dept", aggs, 2, &averages), "group by");
    }
    if (ok) {
        printf("\n== Average salary per department\n");
        df_print(averages, stdout, 0);
    }

    /* 5. Join with the departments table and save the result. */
    if (ok) {
        ok = step_ok(df_read_csv(departments_path, NULL, &departments),
                     "read the departments file");
    }
    if (ok) {
        ok = step_ok(df_merge(df, departments, "dept", DF_JOIN_LEFT, &joined),
                     "join");
    }
    if (ok) {
        printf("\n== Employees with their department's floor and manager\n");
        df_print(joined, stdout, 0);
        ok = step_ok(df_write_csv(joined, "build/basic_output.csv", NULL),
                     "save the result");
    }
    if (ok) {
        printf("\nSaved the joined table to build/basic_output.csv\n");
    }

    /* Free everything we created (NULL is fine for df_free). */
    df_free(joined);
    df_free(departments);
    df_free(averages);
    df_free(sorted);
    df_free(high);
    df_free(df);

    if (ok) {
        return 0;
    }
    return 1;
}
