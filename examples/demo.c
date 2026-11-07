/*
 * demo.c - interactive command-line demo of the CPandas library.
 *
 * Usage:   demo [file.csv]
 *
 * If a CSV file is given it is loaded at the start, otherwise the built-in
 * sample table (df_make_sample) is used. Then a menu lets you try every
 * part of the library on the "current table". Operations that make a new
 * table (filter, sort, group by, ...) show the result and ask whether to
 * keep it as the new current table, so you can chain them.
 *
 * Try it with the files in examples/data/:
 *     demo examples/data/employees.csv
 * and join with examples/data/departments.csv on "dept".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpandas.h"

#define LINE_SIZE 256   /* longest line the user can type   */
#define MAX_WORDS 32    /* most words we read from one line */
#define SHOW_ROWS 20    /* how many rows to print at most   */

/* ---- reading input ------------------------------------------------------ */

/* Prints the prompt and reads one line into buf (without the newline).
   Returns 0 if there is no more input (end of file), 1 otherwise. */
static int read_line(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)size, stdin) == NULL) {
        return 0;
    }
    buf[strcspn(buf, "\r\n")] = '\0';   /* cut off the newline */
    return 1;
}

/* Reads a whole number. Returns 1 on success, 0 if the input was not a
   number (or there was no input). */
static int read_number(const char *prompt, long *out)
{
    char buf[LINE_SIZE];
    if (!read_line(prompt, buf, sizeof buf)) {
        return 0;
    }
    char *end = NULL;
    long value = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || value < 0) {
        return 0;
    }
    *out = value;
    return 1;
}

/* Splits a line into words separated by spaces. The words point into
   `line`, which gets changed. Returns how many words were found. */
static int split_words(char *line, char **words, int max_words)
{
    int count = 0;
    char *word = strtok(line, " \t");
    while (word != NULL && count < max_words) {
        words[count] = word;
        count++;
        word = strtok(NULL, " \t");
    }
    return count;
}

/* ---- printing ----------------------------------------------------------- */

static const char *type_name(df_dtype type)
{
    if (type == DTYPE_INT) return "int";
    if (type == DTYPE_FLOAT) return "float";
    return "string";
}

static void print_columns(const DataFrame *df)
{
    printf("Columns:");
    for (size_t c = 0; c < df_ncols(df); c++) {
        const Series *col = NULL;
        df_get_column_at(df, c, &col);
        printf(" %s (%s)", series_name(col), type_name(series_dtype(col)));
    }
    printf("\n");
}

static void show_table(const DataFrame *df)
{
    printf("\n");
    df_print(df, stdout, SHOW_ROWS);
    printf("\n");
}

/* Prints an error message if st is not DF_OK. Returns 1 if it was OK. */
static int check(df_status st)
{
    if (st != DF_OK) {
        printf("Error: %s\n", df_strerror(st));
        return 0;
    }
    return 1;
}

/* Shows a new result table and asks whether to keep it as the current
   table. Either way, this function takes care of freeing what is not
   needed any more. */
static void offer_result(DataFrame **current, DataFrame *result)
{
    show_table(result);
    char answer[LINE_SIZE];
    if (read_line("Keep this as the current table? (y/n): ", answer,
                  sizeof answer) && (answer[0] == 'y' || answer[0] == 'Y')) {
        df_free(*current);
        *current = result;
        printf("Kept.\n");
    } else {
        df_free(result);
    }
}

/* ---- parsing words into library values ---------------------------------- */

static int parse_op(const char *text, df_compare_op *op)
{
    if (strcmp(text, "==") == 0) { *op = DF_OP_EQ; return 1; }
    if (strcmp(text, "!=") == 0) { *op = DF_OP_NE; return 1; }
    if (strcmp(text, "<") == 0)  { *op = DF_OP_LT; return 1; }
    if (strcmp(text, "<=") == 0) { *op = DF_OP_LE; return 1; }
    if (strcmp(text, ">") == 0)  { *op = DF_OP_GT; return 1; }
    if (strcmp(text, ">=") == 0) { *op = DF_OP_GE; return 1; }
    return 0;
}

static int parse_agg(const char *text, df_agg_func *func)
{
    df_agg_func all[] = {DF_AGG_COUNT, DF_AGG_SUM, DF_AGG_MEAN,
                         DF_AGG_MEDIAN, DF_AGG_MIN, DF_AGG_MAX};
    for (int k = 0; k < 6; k++) {
        if (strcmp(text, df_agg_name(all[k])) == 0) {
            *func = all[k];
            return 1;
        }
    }
    return 0;
}

static int parse_join(const char *text, df_join_type *how)
{
    if (strcmp(text, "inner") == 0) { *how = DF_JOIN_INNER; return 1; }
    if (strcmp(text, "left") == 0)  { *how = DF_JOIN_LEFT;  return 1; }
    if (strcmp(text, "right") == 0) { *how = DF_JOIN_RIGHT; return 1; }
    if (strcmp(text, "outer") == 0) { *how = DF_JOIN_OUTER; return 1; }
    return 0;
}

/* ---- one function per menu item ----------------------------------------- */

static void do_load_csv(DataFrame **current)
{
    char path[LINE_SIZE];
    if (!read_line("CSV file path: ", path, sizeof path)) return;

    DataFrame *loaded = NULL;
    if (check(df_read_csv(path, NULL, &loaded))) {
        df_free(*current);
        *current = loaded;
        printf("Loaded %zu rows x %zu columns.\n", df_nrows(loaded),
               df_ncols(loaded));
        print_columns(loaded);
    }
}

static void do_load_sample(DataFrame **current)
{
    DataFrame *sample = NULL;
    if (check(df_make_sample(&sample))) {
        df_free(*current);
        *current = sample;
        printf("Loaded the sample table.\n");
        show_table(sample);
    }
}

static void do_head_or_tail(const DataFrame *df, int want_tail)
{
    long n = 0;
    if (!read_number("How many rows? ", &n)) {
        printf("Please type a whole number (0 or more).\n");
        return;
    }
    DataFrame *result = NULL;
    df_status st;
    if (want_tail) {
        st = df_tail(df, (size_t)n, &result);
    } else {
        st = df_head(df, (size_t)n, &result);
    }
    if (check(st)) {
        show_table(result);
        df_free(result);
    }
}

static void do_slice(DataFrame **current)
{
    long start = 0;
    long end = 0;
    if (!read_number("Start row (first row is 0): ", &start) ||
        !read_number("End row (not included): ", &end)) {
        printf("Please type whole numbers (0 or more).\n");
        return;
    }
    DataFrame *result = NULL;
    if (check(df_slice(*current, (size_t)start, (size_t)end, &result))) {
        offer_result(current, result);
    }
}

static void do_select_columns(DataFrame **current)
{
    char line[LINE_SIZE];
    char *words[MAX_WORDS];
    print_columns(*current);
    if (!read_line("Columns to keep (separated by spaces): ", line,
                   sizeof line)) return;
    int n = split_words(line, words, MAX_WORDS);
    if (n == 0) {
        printf("No columns given.\n");
        return;
    }
    DataFrame *result = NULL;
    if (check(df_select_columns(*current, (const char *const *)words,
                                (size_t)n, &result))) {
        offer_result(current, result);
    }
}

static void do_filter(DataFrame **current)
{
    char column[LINE_SIZE];
    char op_text[LINE_SIZE];
    char value[LINE_SIZE];

    print_columns(*current);
    if (!read_line("Column: ", column, sizeof column)) return;

    const Series *col = NULL;
    if (!check(df_get_column(*current, column, &col))) return;

    if (!read_line("Condition (==  !=  <  <=  >  >=  missing  present): ",
                   op_text, sizeof op_text)) return;

    DataFrame *result = NULL;
    df_status st;

    if (strcmp(op_text, "missing") == 0) {
        st = df_filter_missing(*current, column, 1, &result);
    } else if (strcmp(op_text, "present") == 0) {
        st = df_filter_missing(*current, column, 0, &result);
    } else {
        df_compare_op op;
        if (!parse_op(op_text, &op)) {
            printf("Unknown condition '%s'.\n", op_text);
            return;
        }
        if (!read_line("Value: ", value, sizeof value)) return;

        if (series_dtype(col) == DTYPE_STRING) {
            st = df_filter_string(*current, column, op, value, &result);
        } else {
            /* number column: the value must be a number */
            char *end = NULL;
            double number = strtod(value, &end);
            if (end == value || *end != '\0') {
                printf("'%s' is not a number.\n", value);
                return;
            }
            st = df_filter_number(*current, column, op, number, &result);
        }
    }

    if (check(st)) {
        printf("%zu of %zu rows match.\n", df_nrows(result),
               df_nrows(*current));
        offer_result(current, result);
    }
}

static void do_sort(DataFrame **current)
{
    char line[LINE_SIZE];
    char *words[MAX_WORDS];
    const char *columns[MAX_WORDS];
    int ascending[MAX_WORDS];
    int ncolumns = 0;

    print_columns(*current);
    printf("Type the sort columns, each optionally followed by asc or desc.\n");
    printf("Example: dept asc salary desc\n");
    if (!read_line("Sort by: ", line, sizeof line)) return;

    int n = split_words(line, words, MAX_WORDS);
    for (int k = 0; k < n; k++) {
        if (strcmp(words[k], "asc") == 0 || strcmp(words[k], "desc") == 0) {
            /* a direction belongs to the column just before it */
            if (ncolumns == 0) {
                printf("'%s' must come after a column name.\n", words[k]);
                return;
            }
            ascending[ncolumns - 1] = (strcmp(words[k], "asc") == 0);
        } else {
            columns[ncolumns] = words[k];
            ascending[ncolumns] = 1;    /* ascending unless told otherwise */
            ncolumns++;
        }
    }
    if (ncolumns == 0) {
        printf("No columns given.\n");
        return;
    }

    DataFrame *result = NULL;
    if (check(df_sort_by(*current, columns, ascending, (size_t)ncolumns,
                         &result))) {
        offer_result(current, result);
    }
}

/* Prints one statistic, or why it could not be computed. */
static void print_stat(const char *label, df_status st, double value)
{
    if (st == DF_OK) {
        printf("  %-7s %g\n", label, value);
    } else {
        printf("  %-7s n/a (%s)\n", label, df_strerror(st));
    }
}

static void do_stats(const DataFrame *df)
{
    char column[LINE_SIZE];
    print_columns(df);
    if (!read_line("Column: ", column, sizeof column)) return;

    const Series *col = NULL;
    if (!check(df_get_column(df, column, &col))) return;

    size_t count = 0;
    double value = 0.0;
    series_count(col, &count);
    printf("Statistics for '%s' (%zu missing of %zu):\n", column,
           series_count_missing(col), series_len(col));
    printf("  %-7s %zu\n", "count", count);
    df_status st = series_sum(col, &value);
    print_stat("sum", st, value);
    st = series_mean(col, &value);
    print_stat("mean", st, value);
    st = series_median(col, &value);
    print_stat("median", st, value);
    st = series_min(col, &value);
    print_stat("min", st, value);
    st = series_max(col, &value);
    print_stat("max", st, value);
}

static void do_groupby(DataFrame **current)
{
    char key[LINE_SIZE];
    char line[LINE_SIZE];
    char *words[MAX_WORDS];
    df_aggregation aggs[MAX_WORDS / 2];

    print_columns(*current);
    if (!read_line("Group by column: ", key, sizeof key)) return;
    printf("Type pairs of: column function   (functions: count sum mean "
           "median min max)\n");
    printf("Example: salary mean name count\n");
    if (!read_line("Aggregations: ", line, sizeof line)) return;

    int n = split_words(line, words, MAX_WORDS);
    if (n % 2 != 0) {
        printf("Each column needs a function after it.\n");
        return;
    }
    int naggs = n / 2;
    for (int k = 0; k < naggs; k++) {
        aggs[k].column = words[2 * k];
        if (!parse_agg(words[2 * k + 1], &aggs[k].func)) {
            printf("Unknown function '%s'.\n", words[2 * k + 1]);
            return;
        }
    }

    DataFrame *result = NULL;
    if (check(df_groupby(*current, key, aggs, (size_t)naggs, &result))) {
        offer_result(current, result);
    }
}

static void do_join(DataFrame **current)
{
    char path[LINE_SIZE];
    char key[LINE_SIZE];
    char how_text[LINE_SIZE];
    df_join_type how;

    if (!read_line("CSV file to join with: ", path, sizeof path)) return;
    DataFrame *other = NULL;
    if (!check(df_read_csv(path, NULL, &other))) return;
    printf("Other table:");
    show_table(other);

    if (!read_line("Key column (must be in both tables): ", key, sizeof key) ||
        !read_line("Join type (inner, left, right, outer): ", how_text,
                   sizeof how_text)) {
        df_free(other);
        return;
    }
    if (!parse_join(how_text, &how)) {
        printf("Unknown join type '%s'.\n", how_text);
        df_free(other);
        return;
    }

    DataFrame *result = NULL;
    df_status st = df_merge(*current, other, key, how, &result);
    df_free(other);     /* the result is a copy, we don't need it any more */
    if (check(st)) {
        offer_result(current, result);
    }
}

static void do_rename(DataFrame *df)
{
    char old_name[LINE_SIZE];
    char new_name[LINE_SIZE];
    print_columns(df);
    if (!read_line("Column to rename: ", old_name, sizeof old_name) ||
        !read_line("New name: ", new_name, sizeof new_name)) return;
    if (check(df_rename_column(df, old_name, new_name))) {
        printf("Renamed.\n");
        print_columns(df);
    }
}

static void do_remove(DataFrame *df)
{
    char name[LINE_SIZE];
    print_columns(df);
    if (!read_line("Column to remove: ", name, sizeof name)) return;
    if (check(df_remove_column(df, name))) {
        printf("Removed.\n");
        print_columns(df);
    }
}

static void do_drop_missing(DataFrame **current)
{
    DataFrame *result = NULL;
    if (check(df_drop_missing_rows(*current, &result))) {
        printf("%zu of %zu rows have no missing cells.\n", df_nrows(result),
               df_nrows(*current));
        offer_result(current, result);
    }
}

static void do_save(const DataFrame *df)
{
    char path[LINE_SIZE];
    if (!read_line("Save to file: ", path, sizeof path)) return;
    if (check(df_write_csv(df, path, NULL))) {
        printf("Saved %zu rows to %s\n", df_nrows(df), path);
    }
}

/* ---- main --------------------------------------------------------------- */

static void print_menu(const DataFrame *df)
{
    printf("\n========== CPandas demo ==========\n");
    if (df != NULL) {
        printf("Current table: %zu rows x %zu columns\n", df_nrows(df),
               df_ncols(df));
    } else {
        printf("No table loaded.\n");
    }
    printf(" 1. Load a CSV file           9. Sort rows\n");
    printf(" 2. Load the sample table    10. Column statistics\n");
    printf(" 3. Show the table           11. Group by and aggregate\n");
    printf(" 4. First N rows (head)      12. Join with a CSV file\n");
    printf(" 5. Last N rows (tail)       13. Rename a column\n");
    printf(" 6. Rows in a range (slice)  14. Remove a column\n");
    printf(" 7. Select columns           15. Drop rows with missing values\n");
    printf(" 8. Filter rows              16. Save the table to CSV\n");
    printf(" 0. Quit\n");
}

int main(int argc, char **argv)
{
    DataFrame *current = NULL;

    /* Start with the file from the command line, or the sample table. */
    if (argc > 1) {
        if (check(df_read_csv(argv[1], NULL, &current))) {
            printf("Loaded %s\n", argv[1]);
            print_columns(current);
        }
    } else {
        check(df_make_sample(&current));
    }

    while (1) {
        print_menu(current);
        long choice = -1;
        char line[LINE_SIZE];
        if (!read_line("Choice: ", line, sizeof line)) {
            break;      /* end of input */
        }
        char *end = NULL;
        choice = strtol(line, &end, 10);
        if (end == line || *end != '\0') {
            printf("Please type a number from the menu.\n");
            continue;
        }

        if (choice == 0) {
            break;
        }
        if (choice == 1) {
            do_load_csv(&current);
            continue;
        }
        if (choice == 2) {
            do_load_sample(&current);
            continue;
        }
        /* every other choice needs a table */
        if (current == NULL) {
            printf("Load a table first (1 or 2).\n");
            continue;
        }

        switch (choice) {
        case 3:  df_print(current, stdout, 0); break;
        case 4:  do_head_or_tail(current, 0); break;
        case 5:  do_head_or_tail(current, 1); break;
        case 6:  do_slice(&current); break;
        case 7:  do_select_columns(&current); break;
        case 8:  do_filter(&current); break;
        case 9:  do_sort(&current); break;
        case 10: do_stats(current); break;
        case 11: do_groupby(&current); break;
        case 12: do_join(&current); break;
        case 13: do_rename(current); break;
        case 14: do_remove(current); break;
        case 15: do_drop_missing(&current); break;
        case 16: do_save(current); break;
        default: printf("Please type a number from the menu.\n"); break;
        }
    }

    df_free(current);
    printf("Bye!\n");
    return 0;
}
