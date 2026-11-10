/*
 * preprocess.c - step 1 of the attrition pipeline: clean the raw HR export
 * with CPandas and write the train / test CSV files the model learns from.
 *
 *     build/ml_preprocess [RAW_DIR] [OUT_DIR]
 *     (defaults: ml/data/raw and build/ml)
 *
 * Reads   RAW_DIR/employees.csv, reviews.csv, departments.csv
 * Writes  OUT_DIR/train.csv, test.csv        one row per employee, only
 *                                           numbers: emp_id, features, left
 *         OUT_DIR/preprocessing_params.csv  the values learned from the
 *                                           training rows (fill values,
 *                                           means, standard deviations)
 *
 * The steps, and the CPandas functions they use:
 *   1. load the three files                  df_read_csv (NA words, errors
 *                                            with line numbers)
 *   2. fix the department spellings          Series API, df_remove/rename
 *   3. remove duplicated employees           df_sort_by_column, df_filter_custom
 *   4. drop rows whose outcome is unknown    df_filter_missing
 *   5. impossible ages become missing        df_add_computed_column
 *   6. review statistics per employee        df_groupby + df_merge (left)
 *   7. department facts                      df_merge (left)
 *   8. split into train (80%) and test       df_filter_custom
 *   9. new features                          computed columns, dates,
 *                                            df_groupby (median pay per dept)
 *  10. fill missing values                   series_median (train only)
 *  11. one-hot encode dept and city          df_groupby (categories), computed
 *  12. scale to mean 0, std 1                series_mean / series_std (train)
 *  13. write the files                       df_select_columns, df_write_csv
 *
 * Everything that is LEARNED from the data (department pay medians, fill
 * values, categories, means and standard deviations) comes from the
 * training rows only, and is then applied to the test rows unchanged.
 * Otherwise the test score would be too optimistic ("data leakage").
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpandas.h"

#define MAX_FEATURES 64
#define NAME_SIZE 64

/* ---- small helpers ------------------------------------------------------ */

/* Stops with a message if a CPandas call failed. A pipeline step that
   fails cannot be skipped, so there is nothing better to do. */
static void must(df_status st, const char *what)
{
    if (st != DF_OK) {
        fprintf(stderr, "preprocess: %s failed: %s\n", what, df_strerror(st));
        exit(1);
    }
}

/* Replaces *df by `result` (the usual "new frame" pattern). */
static void replace(DataFrame **df, DataFrame *result)
{
    df_free(*df);
    *df = result;
}

static void step(const char *what, const DataFrame *df)
{
    printf("  %-46s %5zu rows x %2zu columns\n", what, df_nrows(df),
           df_ncols(df));
}

static const char *type_name(df_dtype type)
{
    switch (type) {
    case DTYPE_INT:    return "int";
    case DTYPE_FLOAT:  return "float";
    case DTYPE_STRING: return "string";
    case DTYPE_BOOL:   return "bool";
    case DTYPE_DATE:   return "date";
    }
    return "?";
}

static void print_types(const DataFrame *df)
{
    printf("    types:");
    for (size_t c = 0; c < df_ncols(df); c++) {
        const Series *col = NULL;
        df_get_column_at(df, c, &col);
        printf(" %s=%s", series_name(col), type_name(series_dtype(col)));
    }
    printf("\n");
}

/* Reads cell `row` of a number column (INT, FLOAT or BOOL) as a double.
   Returns 0 if the cell is missing. */
static int number_at(const DataFrame *df, const char *column, size_t row,
                     double *out)
{
    const Series *s = NULL;
    if (df_get_column(df, column, &s) != DF_OK || series_is_missing(s, row)) {
        return 0;
    }
    if (series_dtype(s) == DTYPE_INT) {
        int64_t v = 0;
        series_get_int(s, row, &v);
        *out = (double)v;
        return 1;
    }
    if (series_dtype(s) == DTYPE_BOOL) {
        int b = 0;
        series_get_bool(s, row, &b);
        *out = b;
        return 1;
    }
    return series_get_float(s, row, out) == DF_OK;
}

/* Turns the finished column `temp` into column `name`: the old `name`
   column is removed and `temp` takes its name. */
static void take_over(DataFrame *df, const char *name, const char *temp)
{
    must(df_remove_column(df, name), "remove column");
    must(df_rename_column(df, temp, name), "rename column");
}

/* 1 if a and b are the same text, ignoring upper / lower case. */
static int same_text_any_case(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- 1. loading --------------------------------------------------------- */

static DataFrame *load(const char *dir, const char *file)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);

    /* The export writes missing values in several ways. */
    const char *na_words[] = {"N/A", "NA", "null"};
    csv_error where;
    csv_options opts = csv_default_options();
    opts.na_values = na_words;
    opts.n_na_values = 3;
    opts.error = &where;

    DataFrame *df = NULL;
    df_status st = df_read_csv(path, &opts, &df);
    if (st != DF_OK) {
        fprintf(stderr, "preprocess: cannot read %s: %s (line %zu: %s)\n",
                path, df_strerror(st), where.line, where.message);
        exit(1);
    }
    printf("  %-46s %5zu rows x %2zu columns\n", file, df_nrows(df),
           df_ncols(df));
    print_types(df);
    return df;
}

/* ---- 2. department spellings -------------------------------------------- */

/* "sales", "SALES" and " sales " all become "Sales", the spelling in the
   department table. A name that is not in that table becomes missing. */
static void fix_department_names(DataFrame *df, const DataFrame *departments)
{
    const Series *raw = NULL;
    const Series *known = NULL;
    must(df_get_column(df, "dept", &raw), "find dept");
    must(df_get_column(departments, "dept", &known), "find dept");
    size_t before = 0;
    series_nunique(raw, &before);

    Series *clean = NULL;
    must(series_create("dept_clean", DTYPE_STRING, series_len(raw), &clean),
         "create column");
    size_t unknown = 0;
    for (size_t r = 0; r < series_len(raw); r++) {
        const char *text = NULL;
        if (series_get_string(raw, r, &text) != DF_OK) {
            must(series_append_missing(clean), "append");
            continue;
        }
        /* cut the spaces off both ends */
        char trimmed[NAME_SIZE];
        while (*text == ' ') {
            text++;
        }
        snprintf(trimmed, sizeof trimmed, "%s", text);
        size_t len = strlen(trimmed);
        while (len > 0 && trimmed[len - 1] == ' ') {
            trimmed[--len] = '\0';
        }

        const char *match = NULL;
        for (size_t k = 0; k < series_len(known) && match == NULL; k++) {
            const char *name = NULL;
            series_get_string(known, k, &name);
            if (same_text_any_case(trimmed, name)) {
                match = name;
            }
        }
        if (match != NULL) {
            must(series_append_string(clean, match), "append");
        } else {
            must(series_append_missing(clean), "append");
            unknown++;
        }
    }
    must(df_add_column(df, clean), "add column");
    take_over(df, "dept", "dept_clean");

    const Series *fixed = NULL;
    df_get_column(df, "dept", &fixed);
    size_t after = 0;
    series_nunique(fixed, &after);
    printf("  %-46s %5zu spellings -> %zu departments", "fixed department names",
           before, after);
    if (unknown > 0) {
        printf(" (%zu unknown)", unknown);
    }
    printf("\n");
}

/* ---- 3. duplicates ------------------------------------------------------ */

/* Keeps a row if it is the first one with its emp_id. The frame is sorted
   by emp_id, so duplicates are next to each other. */
static int first_of_its_id(const DataFrame *df, size_t row, void *user_data)
{
    (void)user_data;
    double id = 0.0, previous = 0.0;
    if (row == 0 || !number_at(df, "emp_id", row, &id) ||
        !number_at(df, "emp_id", row - 1, &previous)) {
        return 1;
    }
    return id != previous;
}

/* ---- 5. impossible ages ------------------------------------------------- */

static int valid_age(const DataFrame *df, size_t row, void *user_data,
                     double *out)
{
    (void)user_data;
    double age = 0.0;
    if (!number_at(df, "age", row, &age) || age < 18 || age > 70) {
        return 0;                       /* missing, or impossible */
    }
    *out = age;
    return 1;
}

/* ---- 8. train / test split ---------------------------------------------- */

/* A row goes to the test set when a hash of its emp_id ends in 0 (mod 5),
   so about 20% of employees, always the same ones, are tested. */
static int is_test_row(const DataFrame *df, size_t row)
{
    double id = 0.0;
    number_at(df, "emp_id", row, &id);
    uint32_t h = (uint32_t)(int64_t)id * 2654435761u;   /* Knuth's hash */
    return (h >> 16) % 5 == 0;
}

static int keep_split(const DataFrame *df, size_t row, void *user_data)
{
    const int *want_test = user_data;
    return is_test_row(df, row) == *want_test;
}

/* ---- 9. features -------------------------------------------------------- */

typedef struct {
    int64_t snapshot;       /* the export date, as a day number */
} TenureInfo;

static int tenure_years(const DataFrame *df, size_t row, void *user_data,
                        double *out)
{
    const TenureInfo *info = user_data;
    const Series *hired = NULL;
    int64_t day = 0;
    if (df_get_column(df, "hire_date", &hired) != DF_OK ||
        series_get_date(hired, row, &day) != DF_OK) {
        return 0;
    }
    *out = (double)(info->snapshot - day) / 365.25;
    return 1;
}

static int zero_if_missing(const DataFrame *df, size_t row, void *user_data,
                           double *out)
{
    const char *column = user_data;
    if (!number_at(df, column, row, out)) {
        *out = 0.0;
    }
    return 1;
}

static int missing_flag(const DataFrame *df, size_t row, void *user_data,
                        double *out)
{
    const char *column = user_data;
    double ignored = 0.0;
    *out = number_at(df, column, row, &ignored) ? 0.0 : 1.0;
    return 1;
}

/* Builds the model's features on one frame (train or test).
   dept_pay holds the median salary of each department, from TRAIN. */
static void add_features(DataFrame **df, const DataFrame *dept_pay,
                         int64_t snapshot)
{
    /* How long they have been here. */
    TenureInfo info = {snapshot};
    must(df_add_computed_column(*df, "tenure_years", tenure_years, &info),
         "tenure");

    /* Paid more or less than the middle of their department? */
    DataFrame *joined = NULL;
    must(df_merge(*df, dept_pay, "dept", DF_JOIN_LEFT, &joined), "merge");
    replace(df, joined);
    must(df_add_arith_column(*df, "pay_ratio", "salary", DF_ARITH_DIV,
                             "salary_median"), "pay ratio");
    must(df_remove_column(*df, "salary_median"), "remove");

    /* No reviews at all means 0 reviews, not "unknown". */
    must(df_add_computed_column(*df, "count_tmp", zero_if_missing,
                                "review_count"), "review count");
    take_over(*df, "review_count", "count_tmp");

    /* Was the cell empty? That can itself tell something. */
    must(df_add_computed_column(*df, "satisfaction_missing", missing_flag,
                                "satisfaction"), "flag");
    must(df_add_computed_column(*df, "salary_missing", missing_flag,
                                "salary"), "flag");

    /* True / False columns become 1 / 0 numbers. */
    const char *flags[] = {"remote", "overtime", "left"};
    for (int k = 0; k < 3; k++) {
        must(df_add_scalar_column(*df, "flag_tmp", flags[k], DF_ARITH_MUL, 1),
             "bool to number");
        take_over(*df, flags[k], "flag_tmp");
    }
}

/* ---- 10-12. values learned from the training rows ----------------------- */

/* What was learned for one number feature. */
typedef struct {
    char   name[NAME_SIZE];
    double fill;            /* median of the training rows */
    double mean;            /* after filling */
    double std;
    int    scaled;
} FeatureParams;

static int fill_missing(const DataFrame *df, size_t row, void *user_data,
                        double *out)
{
    const FeatureParams *p = user_data;
    if (!number_at(df, p->name, row, out)) {
        *out = p->fill;
    }
    return 1;
}

static int standardise(const DataFrame *df, size_t row, void *user_data,
                       double *out)
{
    const FeatureParams *p = user_data;
    double v = 0.0;
    number_at(df, p->name, row, &v);
    *out = (p->std > 0) ? (v - p->mean) / p->std : 0.0;
    return 1;
}

/* Learns fill value, mean and std of one feature from `train`. */
static void learn(const DataFrame *train, const char *name, int scaled,
                  FeatureParams *p)
{
    const Series *col = NULL;
    must(df_get_column(train, name, &col), name);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->scaled = scaled;
    must(series_median(col, &p->fill), "median");
}

static void learn_scaling(const DataFrame *train, FeatureParams *p)
{
    const Series *col = NULL;
    must(df_get_column(train, p->name, &col), p->name);
    must(series_mean(col, &p->mean), "mean");
    must(series_std(col, &p->std), "std");
}

/* Applies a computed-column formula to `name` in place. */
static void transform(DataFrame *df, const char *name, df_row_compute f,
                      const FeatureParams *p)
{
    must(df_add_computed_column(df, "value_tmp", f, (void *)p), name);
    take_over(df, name, "value_tmp");
}

/* ---- 11. one-hot encoding ----------------------------------------------- */

typedef struct {
    const char *column;
    char        category[NAME_SIZE];
} OneHot;

static int is_category(const DataFrame *df, size_t row, void *user_data,
                       double *out)
{
    const OneHot *h = user_data;
    const Series *s = NULL;
    const char *text = NULL;
    df_get_column(df, h->column, &s);
    *out = (series_get_string(s, row, &text) == DF_OK &&
            strcmp(text, h->category) == 0) ? 1.0 : 0.0;
    return 1;
}

/* Adds "<column>_<category>" 0/1 columns to both frames, for every
   category seen in TRAIN (a missing cell gets 0 everywhere). Appends the
   new names to names[]. */
static void one_hot(DataFrame *train, DataFrame *test, const char *column,
                    char names[][NAME_SIZE], size_t *nnames)
{
    DataFrame *categories = NULL;
    must(df_groupby(train, column, NULL, 0, &categories), "categories");
    const Series *values = NULL;
    df_get_column_at(categories, 0, &values);
    for (size_t k = 0; k < series_len(values); k++) {
        OneHot h;
        const char *text = NULL;
        series_get_string(values, k, &text);
        h.column = column;
        snprintf(h.category, sizeof h.category, "%s", text);
        snprintf(names[*nnames], NAME_SIZE, "%s_%s", column, text);
        must(df_add_computed_column(train, names[*nnames], is_category, &h),
             "one-hot");
        must(df_add_computed_column(test, names[*nnames], is_category, &h),
             "one-hot");
        (*nnames)++;
    }
    df_free(categories);
}

/* ---- 13. output --------------------------------------------------------- */

static void write_params(const char *path, const FeatureParams *params,
                         size_t n)
{
    Series *feature = NULL, *fill = NULL, *mean = NULL, *std = NULL;
    must(series_create("feature", DTYPE_STRING, n, &feature), "create");
    must(series_create("fill_value", DTYPE_FLOAT, n, &fill), "create");
    must(series_create("mean", DTYPE_FLOAT, n, &mean), "create");
    must(series_create("std", DTYPE_FLOAT, n, &std), "create");
    for (size_t k = 0; k < n; k++) {
        must(series_append_string(feature, params[k].name), "append");
        must(series_append_float(fill, params[k].fill), "append");
        if (params[k].scaled) {
            must(series_append_float(mean, params[k].mean), "append");
            must(series_append_float(std, params[k].std), "append");
        } else {
            must(series_append_missing(mean), "append");
            must(series_append_missing(std), "append");
        }
    }
    DataFrame *df = NULL;
    must(df_create(&df), "create");
    must(df_add_column(df, feature), "add");
    must(df_add_column(df, fill), "add");
    must(df_add_column(df, mean), "add");
    must(df_add_column(df, std), "add");
    must(df_write_csv(df, path, NULL), "write params");
    df_free(df);
}

static void write_frame(const DataFrame *df, const char *const *names,
                        size_t n, const char *dir, const char *file)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    DataFrame *out = NULL;
    must(df_select_columns(df, names, n, &out), "select columns");
    must(df_write_csv(out, path, NULL), "write");
    printf("  wrote %-40s %5zu rows x %2zu columns\n", path, df_nrows(out),
           df_ncols(out));
    df_free(out);
}

static void show_left_rate(const char *label, const DataFrame *df)
{
    const Series *left = NULL;
    double rate = 0.0;
    df_get_column(df, "left", &left);
    series_mean(left, &rate);
    printf("  %-46s %5zu rows, %.1f%% left\n", label, df_nrows(df),
           100.0 * rate);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *raw_dir = argc > 1 ? argv[1] : "ml/data/raw";
    const char *out_dir = argc > 2 ? argv[2] : "build/ml";
    DataFrame *result = NULL;

    printf("1. Load the raw export\n");
    DataFrame *df = load(raw_dir, "employees.csv");
    DataFrame *reviews = load(raw_dir, "reviews.csv");
    DataFrame *departments = load(raw_dir, "departments.csv");

    printf("2-5. Clean\n");
    fix_department_names(df, departments);

    must(df_sort_by_column(df, "emp_id", 1, &result), "sort");
    replace(&df, result);
    size_t before = df_nrows(df);
    must(df_filter_custom(df, first_of_its_id, NULL, &result), "dedupe");
    replace(&df, result);
    printf("  %-46s %5zu duplicates removed\n", "removed repeated employees",
           before - df_nrows(df));

    before = df_nrows(df);
    must(df_filter_missing(df, "left", 0, &result), "drop unknown outcome");
    replace(&df, result);
    printf("  %-46s %5zu rows dropped\n", "dropped rows with unknown 'left'",
           before - df_nrows(df));

    const Series *age = NULL;
    df_get_column(df, "age", &age);
    size_t missing_before = series_count_missing(age);
    must(df_add_computed_column(df, "age_tmp", valid_age, NULL), "age");
    take_over(df, "age", "age_tmp");
    df_get_column(df, "age", &age);
    printf("  %-46s %5zu ages set to missing\n", "impossible ages (not 18-70)",
           series_count_missing(age) - missing_before);

    printf("6-7. Combine with the other tables\n");
    df_aggregation review_aggs[] = {
        {"score", DF_AGG_MEAN}, {"score", DF_AGG_MIN}, {"score", DF_AGG_COUNT}
    };
    DataFrame *per_employee = NULL;
    must(df_groupby(reviews, "emp_id", review_aggs, 3, &per_employee),
         "review stats");
    must(df_rename_column(per_employee, "score_mean", "review_mean"), "rename");
    must(df_rename_column(per_employee, "score_min", "review_min"), "rename");
    must(df_rename_column(per_employee, "score_count", "review_count"),
         "rename");
    step("review stats per employee (group-by emp_id)", per_employee);
    must(df_merge(df, per_employee, "emp_id", DF_JOIN_LEFT, &result), "merge");
    replace(&df, result);
    step("joined reviews (left join on emp_id)", df);

    DataFrame *dept_facts = NULL;
    const char *fact_columns[] = {"dept", "budget_per_head", "avg_team_size"};
    must(df_select_columns(departments, fact_columns, 3, &dept_facts),
         "select");
    must(df_merge(df, dept_facts, "dept", DF_JOIN_LEFT, &result), "merge");
    replace(&df, result);
    step("joined departments (left join on dept)", df);

    printf("8. Split\n");
    int want_test = 0;
    DataFrame *train = NULL;
    DataFrame *test = NULL;
    must(df_filter_custom(df, keep_split, &want_test, &train), "split");
    want_test = 1;
    must(df_filter_custom(df, keep_split, &want_test, &test), "split");

    printf("9. Features\n");
    df_aggregation pay_aggs[] = {{"salary", DF_AGG_MEDIAN}};
    DataFrame *dept_pay = NULL;
    must(df_groupby(train, "dept", pay_aggs, 1, &dept_pay), "median pay");
    int64_t snapshot = 0;
    must(df_date_from_ymd(2026, 1, 1, &snapshot), "date");
    add_features(&train, dept_pay, snapshot);
    add_features(&test, dept_pay, snapshot);
    step("train with features", train);
    show_left_rate("train", train);
    show_left_rate("test", test);

    printf("10-12. Fill, encode, scale (learned from train only)\n");
    const char *numbers[] = {"age", "salary", "satisfaction", "distance_km",
                             "tenure_years", "pay_ratio", "review_mean",
                             "review_min", "review_count", "budget_per_head",
                             "avg_team_size"};
    const char *binary[] = {"remote", "overtime", "satisfaction_missing",
                            "salary_missing"};
    size_t nnumbers = sizeof numbers / sizeof numbers[0];
    size_t nbinary = sizeof binary / sizeof binary[0];
    FeatureParams params[MAX_FEATURES];
    size_t nparams = 0;

    for (size_t k = 0; k < nnumbers + nbinary; k++) {
        int scaled = k < nnumbers;
        const char *name = scaled ? numbers[k] : binary[k - nnumbers];
        FeatureParams *p = &params[nparams++];
        learn(train, name, scaled, p);
        transform(train, name, fill_missing, p);
        transform(test, name, fill_missing, p);
    }

    /* Before scaling, the cleaned numbers still have their real units. */
    const char *show[] = {"age", "salary", "satisfaction", "tenure_years",
                          "pay_ratio", "review_mean"};
    DataFrame *some = NULL;
    DataFrame *summary = NULL;
    must(df_select_columns(train, show, 6, &some), "select");
    must(df_describe(some, &summary), "describe");
    printf("  cleaned training features (describe):\n");
    df_print(summary, stdout, 0);
    df_free(summary);
    df_free(some);

    for (size_t k = 0; k < nparams; k++) {
        if (params[k].scaled) {
            learn_scaling(train, &params[k]);
            transform(train, params[k].name, standardise, &params[k]);
            transform(test, params[k].name, standardise, &params[k]);
        }
    }

    char names[MAX_FEATURES][NAME_SIZE];
    size_t nnames = 0;
    snprintf(names[nnames++], NAME_SIZE, "emp_id");
    for (size_t k = 0; k < nparams; k++) {
        snprintf(names[nnames++], NAME_SIZE, "%s", params[k].name);
    }
    one_hot(train, test, "dept", names, &nnames);
    one_hot(train, test, "city", names, &nnames);
    snprintf(names[nnames++], NAME_SIZE, "left");

    printf("13. Write\n");
    const char *columns[MAX_FEATURES];
    for (size_t k = 0; k < nnames; k++) {
        columns[k] = names[k];
    }
    write_frame(train, columns, nnames, out_dir, "train.csv");
    write_frame(test, columns, nnames, out_dir, "test.csv");
    char path[512];
    snprintf(path, sizeof path, "%s/preprocessing_params.csv", out_dir);
    write_params(path, params, nparams);
    printf("  wrote %s\n", path);

    df_free(dept_pay);
    df_free(train);
    df_free(test);
    df_free(dept_facts);
    df_free(per_employee);
    df_free(departments);
    df_free(reviews);
    df_free(df);
    return 0;
}
