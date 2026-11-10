/*
 * cpandas_gui.c - a desktop window for CPandas, written in C with GTK 4.
 *
 *     make gui            (builds build/cpandas_gui and opens it)
 *
 * Start it from the project folder, so that examples/data/ and the ML
 * pipeline are found. Two tabs:
 *
 *   Data         open a CSV and see it as a table; filter, sort, group,
 *                join, stack, describe, add computed columns, remove
 *                columns, drop missing rows; undo; save.
 *   ML pipeline  run `make ml` (CPandas preprocessing + scikit-learn) and
 *                read the results: the log of every step, the scores, the
 *                confusion matrix, the strongest features, and the raw,
 *                cleaned and predicted data as tables.
 *
 * Zoom: the - / 100% / + buttons in the title bar, Ctrl + = / Ctrl + - /
 * Ctrl + 0, or Ctrl + mouse wheel. Every text in the window scales.
 *
 * Everything the window shows about a table comes from CPandas: tables are
 * loaded with df_read_csv, changed with the query functions and drawn cell
 * by cell with the series_get_* functions. GTK only draws.
 *
 * Every operation makes a NEW DataFrame (as everywhere in CPandas). The old
 * one is kept in a history list, so Undo just takes it back.
 */
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpandas.h"

#define HISTORY_SIZE 30     /* how many steps Undo can go back   */
#define ZOOM_MIN 60         /* text size limits, in percent      */
#define ZOOM_MAX 250
#define ZOOM_STEP 10
#define TEXT_SIZE 256

/* ---- a table widget that shows a DataFrame ------------------------------ */

typedef struct {
    GtkWidget       *view;      /* GtkColumnView                      */
    const DataFrame *df;        /* borrowed: the frame being shown    */
} TableView;

/* What one column's cells need to know. */
typedef struct {
    TableView *table;
    size_t     column;          /* (size_t)-1 = the row-number column */
} ColumnRef;

/* Writes cell `row` of `col` as text, like df_print does. */
static void cell_text(const Series *col, size_t row, char *buf, size_t size)
{
    if (series_is_missing(col, row)) {
        snprintf(buf, size, "NA");
        return;
    }
    switch (series_dtype(col)) {
    case DTYPE_INT: {
        int64_t v = 0;
        series_get_int(col, row, &v);
        snprintf(buf, size, "%lld", (long long)v);
        break;
    }
    case DTYPE_FLOAT: {
        double v = 0.0;
        series_get_float(col, row, &v);
        snprintf(buf, size, "%g", v);
        break;
    }
    case DTYPE_BOOL: {
        int v = 0;
        series_get_bool(col, row, &v);
        snprintf(buf, size, "%s", v ? "True" : "False");
        break;
    }
    case DTYPE_DATE: {
        int64_t v = 0;
        series_get_date(col, row, &v);
        df_date_format(v, buf, size);
        break;
    }
    case DTYPE_STRING: {
        const char *v = NULL;
        series_get_string(col, row, &v);
        snprintf(buf, size, "%s", v);
        break;
    }
    }
}

static const char *type_name(df_dtype type)
{
    switch (type) {
    case DTYPE_INT:    return "int";
    case DTYPE_FLOAT:  return "float";
    case DTYPE_STRING: return "text";
    case DTYPE_BOOL:   return "bool";
    case DTYPE_DATE:   return "date";
    }
    return "?";
}

static void on_cell_setup(GtkSignalListItemFactory *factory, GObject *object,
                          gpointer user_data)
{
    (void)factory;
    (void)user_data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_widget_add_css_class(label, "cell");
    gtk_list_item_set_child(GTK_LIST_ITEM(object), label);
}

static void on_cell_bind(GtkSignalListItemFactory *factory, GObject *object,
                         gpointer user_data)
{
    (void)factory;
    ColumnRef *ref = user_data;
    GtkListItem *item = GTK_LIST_ITEM(object);
    GtkWidget *label = gtk_list_item_get_child(item);
    GtkStringObject *row_text = gtk_list_item_get_item(item);
    size_t row = (size_t)strtoull(gtk_string_object_get_string(row_text),
                                  NULL, 10);
    char text[TEXT_SIZE];

    gtk_widget_remove_css_class(label, "missing");
    if (ref->column == (size_t)-1 || ref->table->df == NULL) {
        snprintf(text, sizeof text, "%zu", row);
        gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
        gtk_widget_add_css_class(label, "rownumber");
        gtk_label_set_text(GTK_LABEL(label), text);
        return;
    }
    const Series *col = NULL;
    if (df_get_column_at(ref->table->df, ref->column, &col) != DF_OK ||
        row >= series_len(col)) {
        gtk_label_set_text(GTK_LABEL(label), "");
        return;
    }
    cell_text(col, row, text, sizeof text);
    if (series_is_missing(col, row)) {
        gtk_widget_add_css_class(label, "missing");
    }
    /* numbers on the right, text on the left, like df_print */
    df_dtype type = series_dtype(col);
    gtk_label_set_xalign(GTK_LABEL(label),
                         (type == DTYPE_STRING) ? 0.0f : 1.0f);
    gtk_label_set_text(GTK_LABEL(label), text);
}

static void add_view_column(TableView *table, const char *title,
                            size_t column)
{
    ColumnRef *ref = g_new0(ColumnRef, 1);
    ref->table = table;
    ref->column = column;
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_cell_setup), NULL);
    g_signal_connect_data(factory, "bind", G_CALLBACK(on_cell_bind), ref,
                          (GClosureNotify)(void (*)(void))g_free, 0);
    GtkColumnViewColumn *c = gtk_column_view_column_new(title, factory);
    gtk_column_view_column_set_resizable(c, TRUE);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(table->view), c);
    g_object_unref(c);
}

/* Shows `df` (or nothing if NULL). The frame stays owned by the caller and
   must stay alive until the next table_show(). */
static void table_show(TableView *table, const DataFrame *df)
{
    GtkColumnView *view = GTK_COLUMN_VIEW(table->view);

    /* New rows first, so no cell is ever drawn from a freed frame. */
    GtkStringList *rows = gtk_string_list_new(NULL);
    size_t nrows = df_nrows(df);
    char number[32];
    for (size_t r = 0; r < nrows; r++) {
        snprintf(number, sizeof number, "%zu", r);
        gtk_string_list_append(rows, number);
    }
    table->df = df;
    GtkNoSelection *selection = gtk_no_selection_new(G_LIST_MODEL(rows));
    gtk_column_view_set_model(view, GTK_SELECTION_MODEL(selection));
    g_object_unref(selection);      /* the view keeps its own reference */

    GListModel *columns = gtk_column_view_get_columns(view);
    while (g_list_model_get_n_items(columns) > 0) {
        GtkColumnViewColumn *c = g_list_model_get_item(columns, 0);
        gtk_column_view_remove_column(view, c);
        g_object_unref(c);
    }
    if (df == NULL) {
        return;
    }
    add_view_column(table, "", (size_t)-1);
    for (size_t c = 0; c < df_ncols(df); c++) {
        const Series *col = NULL;
        df_get_column_at(df, c, &col);
        char title[TEXT_SIZE];
        snprintf(title, sizeof title, "%s  ·  %s", series_name(col),
                 type_name(series_dtype(col)));
        add_view_column(table, title, c);
    }
}

static GtkWidget *table_new(TableView *table)
{
    table->df = NULL;
    table->view = gtk_column_view_new(NULL);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(table->view),
                                               TRUE);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(table->view),
                                            TRUE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(table->view), FALSE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), table->view);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_css_class(scroll, "table");
    return scroll;
}

/* ---- the application's state -------------------------------------------- */

typedef struct {
    GtkWindow *window;

    /* Data tab */
    TableView  table;
    DataFrame *current;
    char       current_name[TEXT_SIZE];
    DataFrame *history[HISTORY_SIZE];
    char       history_names[HISTORY_SIZE][TEXT_SIZE];
    int        nhistory;
    GtkStringList *columns;          /* the current table's column names */
    GtkStringList *columns_or_none;  /* "(none)" + the names             */
    GtkWidget *title_label;
    GtkWidget *shape_label;
    GtkWidget *status_label;
    GtkWidget *undo_button;
    GtkWidget *filter_column, *filter_op, *filter_value;
    GtkWidget *sort_column, *sort_desc, *sort_then, *sort_then_desc;
    GtkWidget *group_key, *group_key2, *group_column, *group_func;
    GtkWidget *join_key, *join_how;
    GtkWidget *compute_name, *compute_column, *compute_op, *compute_value;
    GtkWidget *stats_column, *stats_label;
    GtkWidget *remove_column;
    GtkWidget *head_rows;

    /* ML tab */
    TableView  ml_table;
    DataFrame *ml_frame;
    GtkWidget *ml_run_button;
    GtkWidget *ml_status;
    GtkWidget *ml_log;
    GtkWidget *ml_view_choice;
    GtkWidget *ml_scores[4];
    GtkWidget *ml_score_notes[4];
    GtkWidget *ml_confusion[4];      /* tn fp fn tp */
    GtkWidget *ml_chart;
    char       feature_names[8][64];
    double     feature_weights[8];
    int        nfeatures;

    /* zoom */
    int             zoom;           /* text size in percent, 100 = normal */
    double          scroll_sum;     /* Ctrl + wheel movement not used yet */
    GtkCssProvider *zoom_css;
    GtkWidget      *zoom_label;
} App;

/* ---- status line -------------------------------------------------------- */

static void say(App *app, const char *text, int is_error)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), text);
    if (is_error) {
        gtk_widget_add_css_class(app->status_label, "error");
    } else {
        gtk_widget_remove_css_class(app->status_label, "error");
    }
}

/* Shows the error of a failed CPandas call. Returns 1 if st was DF_OK. */
static int check(App *app, df_status st, const char *what)
{
    if (st == DF_OK) {
        return 1;
    }
    char text[TEXT_SIZE * 2];
    snprintf(text, sizeof text, "%s failed: %s", what, df_strerror(st));
    say(app, text, 1);
    return 0;
}

/* ---- the current table and its history ---------------------------------- */

static void refresh_column_lists(App *app)
{
    size_t ncols = df_ncols(app->current);
    const char **names = g_new0(const char *, ncols + 2);
    for (size_t c = 0; c < ncols; c++) {
        df_column_name(app->current, c, &names[c]);
    }
    guint old = g_list_model_get_n_items(G_LIST_MODEL(app->columns));
    gtk_string_list_splice(app->columns, 0, old, names);

    const char **with_none = g_new0(const char *, ncols + 2);
    with_none[0] = "(none)";
    for (size_t c = 0; c < ncols; c++) {
        with_none[c + 1] = names[c];
    }
    old = g_list_model_get_n_items(G_LIST_MODEL(app->columns_or_none));
    gtk_string_list_splice(app->columns_or_none, 0, old, with_none);
    g_free(names);
    g_free(with_none);
}

static void show_current(App *app)
{
    table_show(&app->table, app->current);
    refresh_column_lists(app);
    char text[TEXT_SIZE * 2];
    snprintf(text, sizeof text, "%zu rows × %zu columns",
             df_nrows(app->current), df_ncols(app->current));
    gtk_label_set_text(GTK_LABEL(app->shape_label), text);
    gtk_label_set_text(GTK_LABEL(app->title_label), app->current_name);
    gtk_widget_set_sensitive(app->undo_button, app->nhistory > 0);
}

/* Makes `result` the current table (keeping the old one for Undo). */
static void set_current(App *app, DataFrame *result, const char *name,
                        const char *message)
{
    if (app->current != NULL) {
        if (app->nhistory == HISTORY_SIZE) {
            /* full: forget the oldest step */
            df_free(app->history[0]);
            memmove(&app->history[0], &app->history[1],
                    (HISTORY_SIZE - 1) * sizeof app->history[0]);
            memmove(app->history_names[0], app->history_names[1],
                    (HISTORY_SIZE - 1) * sizeof app->history_names[0]);
            app->nhistory--;
        }
        app->history[app->nhistory] = app->current;
        snprintf(app->history_names[app->nhistory], TEXT_SIZE, "%s",
                 app->current_name);
        app->nhistory++;
    }
    app->current = result;
    if (name != NULL) {
        snprintf(app->current_name, sizeof app->current_name, "%s", name);
    }
    show_current(app);
    say(app, message, 0);
}

static void on_undo(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;
    if (app->nhistory == 0) {
        return;
    }
    DataFrame *old = app->current;
    app->nhistory--;
    app->current = app->history[app->nhistory];
    snprintf(app->current_name, sizeof app->current_name, "%s",
             app->history_names[app->nhistory]);
    show_current(app);        /* shows the new frame before freeing */
    df_free(old);
    say(app, "Undone.", 0);
}

/* ---- reading the form widgets ------------------------------------------- */

static const char *selected_text(GtkWidget *dropdown)
{
    GtkStringObject *item =
        gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dropdown));
    return item ? gtk_string_object_get_string(item) : NULL;
}

static guint selected_index(GtkWidget *dropdown)
{
    return gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
}

static const char *entry_text(GtkWidget *entry)
{
    return gtk_editable_get_text(GTK_EDITABLE(entry));
}

static GtkWidget *column_dropdown(GtkStringList *model)
{
    /* gtk_drop_down_new takes a reference of its own */
    g_object_ref(model);
    return gtk_drop_down_new(G_LIST_MODEL(model), NULL);
}

static GtkWidget *choice_dropdown(const char *const *choices)
{
    return gtk_drop_down_new_from_strings(choices);
}

/* ---- loading and saving files ------------------------------------------- */

/* Reads a CSV; on failure explains where in the status line. Besides empty
   cells, the usual words for "no value" count as missing. */
static DataFrame *load_csv(App *app, const char *path)
{
    const char *na_words[] = {"N/A", "NA", "null"};
    csv_error where;
    csv_options opts = csv_default_options();
    opts.na_values = na_words;
    opts.n_na_values = 3;
    opts.error = &where;
    DataFrame *df = NULL;
    df_status st = df_read_csv(path, &opts, &df);
    if (st == DF_OK) {
        return df;
    }
    char text[TEXT_SIZE * 2];
    if (where.line > 0) {
        snprintf(text, sizeof text, "Cannot read %s: %s, line %zu: %s",
                 path, df_strerror(st), where.line, where.message);
    } else {
        snprintf(text, sizeof text, "Cannot read %s: %s", path,
                 where.message[0] ? where.message : df_strerror(st));
    }
    say(app, text, 1);
    return NULL;
}

/* What to do with the file the user picks. */
typedef enum { PICK_OPEN, PICK_JOIN, PICK_STACK } PickPurpose;

typedef struct {
    App        *app;
    PickPurpose purpose;
} PickRequest;

static void use_picked_file(App *app, PickPurpose purpose, const char *path)
{
    DataFrame *other = load_csv(app, path);
    if (other == NULL) {
        return;
    }
    char *base = g_path_get_basename(path);
    char message[TEXT_SIZE * 2];
    DataFrame *result = NULL;

    if (purpose == PICK_OPEN) {
        snprintf(message, sizeof message, "Opened %s.", path);
        set_current(app, other, base, message);
        g_free(base);
        return;
    }
    if (purpose == PICK_STACK) {
        const DataFrame *parts[2] = {app->current, other};
        if (check(app, df_concat(parts, 2, &result), "Stack")) {
            snprintf(message, sizeof message, "Stacked %zu rows of %s under "
                     "the table.", df_nrows(other), base);
            set_current(app, result, NULL, message);
        }
    } else {
        const char *key = selected_text(app->join_key);
        const char *how_names[] = {"inner", "left", "right", "outer"};
        df_join_type how_types[] = {DF_JOIN_INNER, DF_JOIN_LEFT,
                                    DF_JOIN_RIGHT, DF_JOIN_OUTER};
        guint how = selected_index(app->join_how);
        if (key == NULL || how > 3) {
            say(app, "Choose a key column and a join type first.", 1);
        } else if (check(app, df_merge(app->current, other, key,
                                       how_types[how], &result), "Join")) {
            snprintf(message, sizeof message, "%s join with %s on %s.",
                     how_names[how], base, key);
            set_current(app, result, NULL, message);
        }
    }
    df_free(other);
    g_free(base);
}

static void on_file_picked(GObject *source, GAsyncResult *res,
                           gpointer user_data)
{
    PickRequest *request = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res,
                                              &error);
    if (file != NULL) {
        char *path = g_file_get_path(file);
        use_picked_file(request->app, request->purpose, path);
        g_free(path);
        g_object_unref(file);
    }
    g_clear_error(&error);      /* cancelled: nothing to do */
    g_free(request);
}

static GtkFileDialog *csv_dialog(const char *title)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "CSV files");
    gtk_file_filter_add_pattern(filter, "*.csv");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filters);
    g_object_unref(filter);
    GFile *folder = g_file_new_for_path("examples/data");
    gtk_file_dialog_set_initial_folder(dialog, folder);
    g_object_unref(folder);
    return dialog;
}

static void pick_file(App *app, PickPurpose purpose, const char *title)
{
    PickRequest *request = g_new0(PickRequest, 1);
    request->app = app;
    request->purpose = purpose;
    GtkFileDialog *dialog = csv_dialog(title);
    gtk_file_dialog_open(dialog, app->window, NULL, on_file_picked, request);
    g_object_unref(dialog);
}

static void on_open(GtkButton *b, gpointer app)
{
    (void)b;
    pick_file(app, PICK_OPEN, "Open a CSV file");
}

static void on_join(GtkButton *b, gpointer app)
{
    (void)b;
    pick_file(app, PICK_JOIN, "CSV file to join with");
}

static void on_stack(GtkButton *b, gpointer app)
{
    (void)b;
    pick_file(app, PICK_STACK, "CSV file to stack under the table");
}

static void on_sample(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    DataFrame *df = NULL;
    if (check(app, df_make_sample(&df), "Sample")) {
        set_current(app, df, "sample table", "Loaded the built-in sample.");
    }
}

static void on_saved(GObject *source, GAsyncResult *res, gpointer user_data)
{
    App *app = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res,
                                              &error);
    if (file != NULL) {
        char *path = g_file_get_path(file);
        if (check(app, df_write_csv(app->current, path, NULL), "Save")) {
            char text[TEXT_SIZE * 2];
            snprintf(text, sizeof text, "Saved %zu rows to %s.",
                     df_nrows(app->current), path);
            say(app, text, 0);
        }
        g_free(path);
        g_object_unref(file);
    }
    g_clear_error(&error);
}

static void on_save(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    GtkFileDialog *dialog = csv_dialog("Save the table as CSV");
    GFile *folder = g_file_new_for_path("build");
    gtk_file_dialog_set_initial_folder(dialog, folder);
    g_object_unref(folder);
    gtk_file_dialog_set_initial_name(dialog, "result.csv");
    gtk_file_dialog_save(dialog, app->window, NULL, on_saved, app);
    g_object_unref(dialog);
}

/* ---- the operations ----------------------------------------------------- */

static const char *const COMPARE_OPS[] = {"==", "!=", "<", "<=", ">", ">=",
                                          "is missing", "is present", NULL};
static const char *const AGG_NAMES[] = {"count", "sum", "mean", "median",
                                        "min", "max", "std", "var",
                                        "nunique", NULL};
static const df_agg_func AGG_FUNCS[] = {DF_AGG_COUNT, DF_AGG_SUM, DF_AGG_MEAN,
                                        DF_AGG_MEDIAN, DF_AGG_MIN, DF_AGG_MAX,
                                        DF_AGG_STD, DF_AGG_VAR,
                                        DF_AGG_NUNIQUE};
static const char *const ARITH_OPS[] = {"+", "−", "×", "÷", NULL};

static void on_filter(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    const char *column = selected_text(app->filter_column);
    guint op = selected_index(app->filter_op);
    const char *value = entry_text(app->filter_value);
    const Series *col = NULL;
    if (column == NULL || op > 7 ||
        !check(app, df_get_column(app->current, column, &col), "Filter")) {
        return;
    }

    DataFrame *result = NULL;
    df_status st = DF_OK;
    if (op == 6 || op == 7) {
        st = df_filter_missing(app->current, column, op == 6, &result);
    } else {
        df_compare_op ops[] = {DF_OP_EQ, DF_OP_NE, DF_OP_LT, DF_OP_LE,
                               DF_OP_GT, DF_OP_GE};
        df_dtype type = series_dtype(col);
        if (type == DTYPE_STRING) {
            st = df_filter_string(app->current, column, ops[op], value,
                                  &result);
        } else if (type == DTYPE_DATE) {
            int64_t days = 0;
            if (df_date_parse(value, &days) != DF_OK) {
                say(app, "For a date column, type a date like 2025-01-31.",
                    1);
                return;
            }
            st = df_filter_date(app->current, column, ops[op], days, &result);
        } else {
            double number = 0.0;
            char *end = NULL;
            if (type == DTYPE_BOOL &&
                (g_ascii_strcasecmp(value, "true") == 0 ||
                 g_ascii_strcasecmp(value, "false") == 0)) {
                number = g_ascii_strcasecmp(value, "true") == 0;
            } else {
                number = g_ascii_strtod(value, &end);
                if (end == value || *end != '\0') {
                    say(app, type == DTYPE_BOOL
                                 ? "For a true/false column, type true or false."
                                 : "For a number column, type a number.", 1);
                    return;
                }
            }
            st = df_filter_number(app->current, column, ops[op], number,
                                  &result);
        }
    }
    if (check(app, st, "Filter")) {
        char text[TEXT_SIZE * 2];
        snprintf(text, sizeof text, "Filter %s %s %s: %zu of %zu rows match.",
                 column, COMPARE_OPS[op], (op >= 6) ? "" : value,
                 df_nrows(result), df_nrows(app->current));
        set_current(app, result, NULL, text);
    }
}

static void on_sort(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    const char *columns[2];
    int ascending[2];
    size_t n = 0;
    columns[n] = selected_text(app->sort_column);
    ascending[n] = !gtk_check_button_get_active(GTK_CHECK_BUTTON(app->sort_desc));
    if (columns[n] == NULL) {
        return;
    }
    n++;
    if (selected_index(app->sort_then) > 0) {
        columns[n] = selected_text(app->sort_then);
        ascending[n] = !gtk_check_button_get_active(
            GTK_CHECK_BUTTON(app->sort_then_desc));
        n++;
    }
    DataFrame *result = NULL;
    if (check(app, df_sort_by(app->current, columns, ascending, n, &result),
              "Sort")) {
        char text[TEXT_SIZE * 2];
        snprintf(text, sizeof text, "Sorted by %s%s%s.", columns[0],
                 n > 1 ? ", then " : "", n > 1 ? columns[1] : "");
        set_current(app, result, NULL, text);
    }
}

static void on_group(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    const char *keys[2];
    size_t nkeys = 0;
    keys[nkeys++] = selected_text(app->group_key);
    if (selected_index(app->group_key2) > 0) {
        keys[nkeys++] = selected_text(app->group_key2);
    }
    df_aggregation agg;
    agg.column = selected_text(app->group_column);
    guint f = selected_index(app->group_func);
    if (keys[0] == NULL || agg.column == NULL || f >= 9) {
        return;
    }
    agg.func = AGG_FUNCS[f];
    DataFrame *result = NULL;
    if (check(app, df_groupby_multi(app->current, keys, nkeys, &agg, 1,
                                    &result), "Group by")) {
        char text[TEXT_SIZE * 2];
        snprintf(text, sizeof text, "Grouped by %s%s%s: %zu groups.",
                 keys[0], nkeys > 1 ? " and " : "", nkeys > 1 ? keys[1] : "",
                 df_nrows(result));
        set_current(app, result, NULL, text);
    }
}

static void on_compute(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    const char *name = entry_text(app->compute_name);
    const char *column = selected_text(app->compute_column);
    const char *right = entry_text(app->compute_value);
    guint op_index = selected_index(app->compute_op);
    df_arith_op ops[] = {DF_ARITH_ADD, DF_ARITH_SUB, DF_ARITH_MUL,
                         DF_ARITH_DIV};
    if (column == NULL || op_index > 3) {
        return;
    }

    /* The new column goes into a copy, so Undo still has the old table. */
    DataFrame *copy = NULL;
    if (!check(app, df_clone(app->current, &copy), "Copy")) {
        return;
    }
    char *end = NULL;
    double number = g_ascii_strtod(right, &end);
    df_status st;
    if (end != right && *end == '\0') {
        st = df_add_scalar_column(copy, name, column, ops[op_index], number);
    } else {
        st = df_add_arith_column(copy, name, column, ops[op_index], right);
    }
    if (!check(app, st, "New column")) {
        df_free(copy);
        return;
    }
    char text[TEXT_SIZE * 2];
    snprintf(text, sizeof text, "Added %s = %s %s %s.", name, column,
             ARITH_OPS[op_index], right);
    set_current(app, copy, NULL, text);
}

static void on_describe(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    DataFrame *result = NULL;
    if (check(app, df_describe(app->current, &result), "Describe")) {
        set_current(app, result, NULL,
                    "Summary of the number columns (Undo to go back).");
    }
}

static void on_drop_missing(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    DataFrame *result = NULL;
    if (check(app, df_drop_missing_rows(app->current, &result), "Drop")) {
        char text[TEXT_SIZE];
        snprintf(text, sizeof text, "%zu of %zu rows have no missing cells.",
                 df_nrows(result), df_nrows(app->current));
        set_current(app, result, NULL, text);
    }
}

static void on_head(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    int n = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->head_rows));
    DataFrame *result = NULL;
    if (check(app, df_head(app->current, (size_t)n, &result), "Head")) {
        char text[TEXT_SIZE];
        snprintf(text, sizeof text, "Kept the first %d rows.", n);
        set_current(app, result, NULL, text);
    }
}

static void on_remove_column(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    const char *name = selected_text(app->remove_column);
    if (name == NULL) {
        return;
    }
    char removed[TEXT_SIZE];
    snprintf(removed, sizeof removed, "%s", name);  /* name dies with list */
    DataFrame *copy = NULL;
    if (check(app, df_clone(app->current, &copy), "Copy") &&
        check(app, df_remove_column(copy, removed), "Remove")) {
        char text[TEXT_SIZE * 2];
        snprintf(text, sizeof text, "Removed the column %s.", removed);
        set_current(app, copy, NULL, text);
    } else {
        df_free(copy);
    }
}

static void on_stats_column(GObject *dropdown, GParamSpec *pspec,
                            gpointer user_data)
{
    (void)dropdown;
    (void)pspec;
    App *app = user_data;
    const char *name = selected_text(app->stats_column);
    const Series *col = NULL;
    if (name == NULL || app->current == NULL ||
        df_get_column(app->current, name, &col) != DF_OK) {
        gtk_label_set_text(GTK_LABEL(app->stats_label), "");
        return;
    }
    char text[1024];
    size_t len = 0;
    size_t count = 0, unique = 0;
    series_count(col, &count);
    series_nunique(col, &unique);
    len += (size_t)snprintf(text + len, sizeof text - len,
                            "type      %s\ncount     %zu\nmissing   %zu\n"
                            "unique    %zu\n", type_name(series_dtype(col)),
                            count, series_count_missing(col), unique);
    if (series_dtype(col) == DTYPE_STRING) {
        snprintf(text + len, sizeof text - len, "stored    %zu texts",
                 series_distinct_strings(col));
    } else {
        const char *labels[] = {"mean", "std", "min", "median", "max"};
        for (int k = 0; k < 5; k++) {
            double v = 0.0;
            df_status st = DF_ERR_TYPE_MISMATCH;
            switch (k) {
            case 0: st = series_mean(col, &v); break;
            case 1: st = series_std(col, &v); break;
            case 2: st = series_min(col, &v); break;
            case 3: st = series_median(col, &v); break;
            case 4: st = series_max(col, &v); break;
            }
            if (st == DF_OK) {
                len += (size_t)snprintf(text + len, sizeof text - len,
                                        "%-9s %g\n", labels[k], v);
            } else {
                len += (size_t)snprintf(text + len, sizeof text - len,
                                        "%-9s –\n", labels[k]);
            }
        }
    }
    gtk_label_set_text(GTK_LABEL(app->stats_label), text);
}

/* ---- building the Data tab ---------------------------------------------- */

/* A titled group of controls in the sidebar. */
static GtkWidget *card(GtkWidget *sidebar, const char *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(box, "card");
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_add_css_class(label, "card-title");
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(sidebar), box);
    return box;
}

/* A row of widgets inside a card. */
static GtkWidget *row(GtkWidget *card_box)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(card_box), r);
    return r;
}

static GtkWidget *small_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim");
    return l;
}

static GtkWidget *action_button(const char *text, GCallback callback,
                                App *app)
{
    GtkWidget *button = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(button, "suggested-action");
    g_signal_connect(button, "clicked", callback, app);
    return button;
}

static GtkWidget *grow(GtkWidget *w)
{
    gtk_widget_set_hexpand(w, TRUE);
    return w;
}

static GtkWidget *build_sidebar(App *app)
{
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(sidebar, 10);
    gtk_widget_set_margin_end(sidebar, 10);
    gtk_widget_set_margin_top(sidebar, 10);
    gtk_widget_set_margin_bottom(sidebar, 10);
    GtkWidget *c, *r;

    c = card(sidebar, "Filter rows");
    r = row(c);
    app->filter_column = grow(column_dropdown(app->columns));
    gtk_box_append(GTK_BOX(r), app->filter_column);
    app->filter_op = choice_dropdown(COMPARE_OPS);
    gtk_box_append(GTK_BOX(r), app->filter_op);
    r = row(c);
    app->filter_value = grow(gtk_entry_new());
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->filter_value),
                                   "value, e.g. 40000, IT, 2025-01-01");
    gtk_box_append(GTK_BOX(r), app->filter_value);
    gtk_box_append(GTK_BOX(r), action_button("Filter", G_CALLBACK(on_filter),
                                             app));

    c = card(sidebar, "Sort");
    r = row(c);
    app->sort_column = grow(column_dropdown(app->columns));
    gtk_box_append(GTK_BOX(r), app->sort_column);
    app->sort_desc = gtk_check_button_new_with_label("high → low");
    gtk_box_append(GTK_BOX(r), app->sort_desc);
    r = row(c);
    gtk_box_append(GTK_BOX(r), small_label("then"));
    app->sort_then = grow(column_dropdown(app->columns_or_none));
    gtk_box_append(GTK_BOX(r), app->sort_then);
    app->sort_then_desc = gtk_check_button_new_with_label("high → low");
    gtk_box_append(GTK_BOX(r), app->sort_then_desc);
    gtk_box_append(GTK_BOX(c), action_button("Sort", G_CALLBACK(on_sort), app));

    c = card(sidebar, "Group by and aggregate");
    r = row(c);
    gtk_box_append(GTK_BOX(r), small_label("by"));
    app->group_key = grow(column_dropdown(app->columns));
    gtk_box_append(GTK_BOX(r), app->group_key);
    app->group_key2 = grow(column_dropdown(app->columns_or_none));
    gtk_box_append(GTK_BOX(r), app->group_key2);
    r = row(c);
    app->group_func = choice_dropdown(AGG_NAMES);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->group_func), 2);  /* mean */
    gtk_box_append(GTK_BOX(r), app->group_func);
    gtk_box_append(GTK_BOX(r), small_label("of"));
    app->group_column = grow(column_dropdown(app->columns));
    gtk_box_append(GTK_BOX(r), app->group_column);
    gtk_box_append(GTK_BOX(c), action_button("Group", G_CALLBACK(on_group),
                                             app));

    c = card(sidebar, "Join with another CSV");
    r = row(c);
    gtk_box_append(GTK_BOX(r), small_label("on"));
    app->join_key = grow(column_dropdown(app->columns));
    gtk_box_append(GTK_BOX(r), app->join_key);
    const char *const hows[] = {"inner", "left", "right", "outer", NULL};
    app->join_how = choice_dropdown(hows);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->join_how), 1);   /* left */
    gtk_box_append(GTK_BOX(r), app->join_how);
    gtk_box_append(GTK_BOX(c), action_button("Choose file and join…",
                                             G_CALLBACK(on_join), app));

    c = card(sidebar, "Stack another CSV under it");
    gtk_box_append(GTK_BOX(c), action_button("Choose file and stack…",
                                             G_CALLBACK(on_stack), app));

    c = card(sidebar, "New computed column");
    r = row(c);
    app->compute_name = grow(gtk_entry_new());
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->compute_name), "new name");
    gtk_box_append(GTK_BOX(r), app->compute_name);
    gtk_box_append(GTK_BOX(r), small_label("="));
    r = row(c);
    app->compute_column = grow(column_dropdown(app->columns));
    gtk_box_append(GTK_BOX(r), app->compute_column);
    app->compute_op = choice_dropdown(ARITH_OPS);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->compute_op), 2);  /* × */
    gtk_box_append(GTK_BOX(r), app->compute_op);
    app->compute_value = grow(gtk_entry_new());
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->compute_value),
                                   "0.1 or a column");
    gtk_box_append(GTK_BOX(r), app->compute_value);
    gtk_box_append(GTK_BOX(c), action_button("Add column",
                                             G_CALLBACK(on_compute), app));

    c = card(sidebar, "Column statistics");
    app->stats_column = column_dropdown(app->columns);
    g_signal_connect(app->stats_column, "notify::selected",
                     G_CALLBACK(on_stats_column), app);
    gtk_box_append(GTK_BOX(c), app->stats_column);
    app->stats_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->stats_label), 0.0f);
    gtk_widget_add_css_class(app->stats_label, "mono");
    gtk_box_append(GTK_BOX(c), app->stats_label);

    c = card(sidebar, "More");
    r = row(c);
    gtk_box_append(GTK_BOX(r), grow(action_button("Describe",
                                                  G_CALLBACK(on_describe),
                                                  app)));
    gtk_box_append(GTK_BOX(r), grow(action_button("Drop rows with NA",
                                                  G_CALLBACK(on_drop_missing),
                                                  app)));
    r = row(c);
    app->head_rows = gtk_spin_button_new_with_range(0, 1000000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->head_rows), 10);
    gtk_box_append(GTK_BOX(r), app->head_rows);
    gtk_box_append(GTK_BOX(r), grow(action_button("Keep first rows",
                                                  G_CALLBACK(on_head), app)));
    r = row(c);
    app->remove_column = grow(column_dropdown(app->columns));
    gtk_box_append(GTK_BOX(r), app->remove_column);
    gtk_box_append(GTK_BOX(r), action_button("Remove column",
                                             G_CALLBACK(on_remove_column),
                                             app));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), sidebar);
    gtk_widget_set_size_request(scroll, 360, -1);
    gtk_widget_add_css_class(scroll, "sidebar");
    return scroll;
}

static GtkWidget *build_data_tab(App *app)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* toolbar */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "toolbar");
    GtkWidget *open = gtk_button_new_with_label("Open CSV…");
    g_signal_connect(open, "clicked", G_CALLBACK(on_open), app);
    GtkWidget *sample = gtk_button_new_with_label("Sample table");
    g_signal_connect(sample, "clicked", G_CALLBACK(on_sample), app);
    app->undo_button = gtk_button_new_with_label("↶ Undo");
    g_signal_connect(app->undo_button, "clicked", G_CALLBACK(on_undo), app);
    GtkWidget *save = gtk_button_new_with_label("Save CSV…");
    g_signal_connect(save, "clicked", G_CALLBACK(on_save), app);
    app->title_label = gtk_label_new("");
    gtk_widget_add_css_class(app->title_label, "table-name");
    app->shape_label = gtk_label_new("");
    gtk_widget_add_css_class(app->shape_label, "dim");
    gtk_box_append(GTK_BOX(bar), open);
    gtk_box_append(GTK_BOX(bar), sample);
    gtk_box_append(GTK_BOX(bar), app->undo_button);
    gtk_box_append(GTK_BOX(bar), save);
    GtkWidget *spacer = grow(gtk_label_new(""));
    gtk_box_append(GTK_BOX(bar), spacer);
    gtk_box_append(GTK_BOX(bar), app->title_label);
    gtk_box_append(GTK_BOX(bar), app->shape_label);
    gtk_box_append(GTK_BOX(page), bar);

    /* table | sidebar */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), table_new(&app->table));
    gtk_paned_set_end_child(GTK_PANED(paned), build_sidebar(app));
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(page), paned);

    app->status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app->status_label),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(app->status_label, "status");
    gtk_box_append(GTK_BOX(page), app->status_label);
    return page;
}

/* ---- the ML pipeline tab ------------------------------------------------ */

/* Very small readers for the flat metrics.json that ml/train.py writes. */
static int json_number(const char *json, const char *key, double *out)
{
    char pattern[96];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return 0;
    }
    *out = g_ascii_strtod(p + strlen(pattern), NULL);
    return 1;
}

static int json_text(const char *json, const char *key, char *out,
                     size_t size)
{
    char pattern[96];
    snprintf(pattern, sizeof pattern, "\"%s\": \"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return 0;
    }
    p += strlen(pattern);
    size_t n = 0;
    while (p[n] != '"' && p[n] != '\0' && n + 1 < size) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return 1;
}

/* Reads "top_features": { "name": weight, ... } into the app. */
static void json_features(App *app, const char *json)
{
    app->nfeatures = 0;
    const char *p = strstr(json, "\"top_features\"");
    if (p == NULL || (p = strchr(p, '{')) == NULL) {
        return;
    }
    p++;
    while (app->nfeatures < 8) {
        const char *open = strchr(p, '"');
        const char *close_brace = strchr(p, '}');
        if (open == NULL || (close_brace != NULL && close_brace < open)) {
            break;
        }
        const char *close = strchr(open + 1, '"');
        const char *colon = close ? strchr(close, ':') : NULL;
        if (close == NULL || colon == NULL) {
            break;
        }
        int k = app->nfeatures;
        size_t len = (size_t)(close - open - 1);
        if (len >= sizeof app->feature_names[k]) {
            len = sizeof app->feature_names[k] - 1;
        }
        memcpy(app->feature_names[k], open + 1, len);
        app->feature_names[k][len] = '\0';
        char *end = NULL;
        app->feature_weights[k] = g_ascii_strtod(colon + 1, &end);
        app->nfeatures++;
        p = end;
    }
}

/* Counts the test rows with this predicted / actual value, with CPandas
   filters. */
static size_t count_cells(const DataFrame *predictions, int predicted,
                          int actual)
{
    DataFrame *a = NULL;
    DataFrame *b = NULL;
    size_t n = 0;
    if (df_filter_number(predictions, "predicted", DF_OP_EQ, predicted,
                         &a) == DF_OK &&
        df_filter_number(a, "actual", DF_OP_EQ, actual, &b) == DF_OK) {
        n = df_nrows(b);
    }
    df_free(a);
    df_free(b);
    return n;
}

static void set_score(App *app, int k, const char *value, const char *note)
{
    gtk_label_set_text(GTK_LABEL(app->ml_scores[k]), value);
    gtk_label_set_text(GTK_LABEL(app->ml_score_notes[k]), note);
}

/* Loads the results of the last run from build/ml/. */
static void load_ml_results(App *app)
{
    char *json = NULL;
    if (!g_file_get_contents("build/ml/metrics.json", &json, NULL, NULL)) {
        gtk_label_set_text(GTK_LABEL(app->ml_status),
                           "No results yet: press Run pipeline.");
        return;
    }
    double auc = 0.0, baseline = 0.0;
    char model[128] = "";
    json_number(json, "test_roc_auc", &auc);
    json_number(json, "baseline_accuracy", &baseline);
    json_text(json, "model", model, sizeof model);
    json_features(app, json);
    g_free(json);

    /* The confusion matrix and the other scores are counted again here,
       from predictions.csv, with CPandas. */
    DataFrame *pred = load_csv(app, "build/ml/predictions.csv");
    if (pred == NULL) {
        return;
    }
    size_t tn = count_cells(pred, 0, 0), fp = count_cells(pred, 1, 0);
    size_t fn = count_cells(pred, 0, 1), tp = count_cells(pred, 1, 1);
    df_free(pred);
    double total = (double)(tn + fp + fn + tp);
    double accuracy = total > 0 ? (double)(tn + tp) / total : 0.0;
    double recall = (tp + fn) > 0 ? (double)tp / (double)(tp + fn) : 0.0;
    double precision = (tp + fp) > 0 ? (double)tp / (double)(tp + fp) : 0.0;

    char value[64], note[TEXT_SIZE];
    snprintf(value, sizeof value, "%.3f", auc);
    set_score(app, 0, value, "ROC AUC — 0.5 is guessing, 1.0 is perfect");
    snprintf(value, sizeof value, "%.0f%%", 100 * recall);
    snprintf(note, sizeof note, "of the %zu leavers were caught", tp + fn);
    set_score(app, 1, value, note);
    snprintf(value, sizeof value, "%.0f%%", 100 * precision);
    snprintf(note, sizeof note, "of the %zu warnings were right", tp + fp);
    set_score(app, 2, value, note);
    snprintf(value, sizeof value, "%.0f%%", 100 * accuracy);
    snprintf(note, sizeof note, "accuracy (always 'stays': %.0f%%)",
             100 * baseline);
    set_score(app, 3, value, note);

    size_t cells[4] = {tn, fp, fn, tp};
    for (int k = 0; k < 4; k++) {
        snprintf(value, sizeof value, "%zu", cells[k]);
        gtk_label_set_text(GTK_LABEL(app->ml_confusion[k]), value);
    }
    gtk_widget_queue_draw(app->ml_chart);

    char status[TEXT_SIZE * 2];
    snprintf(status, sizeof status, "Model: %s, tested on %.0f employees it "
             "never saw while learning.", model, total);
    gtk_label_set_text(GTK_LABEL(app->ml_status), status);
}

/* Which file the table at the bottom shows. */
static const char *const ML_FILES[] = {
    "ml/data/raw/employees.csv",
    "build/ml/train.csv",
    "build/ml/predictions.csv",
    "build/ml/preprocessing_params.csv"
};

static void show_ml_file(App *app)
{
    guint k = selected_index(app->ml_view_choice);
    if (k > 3) {
        return;
    }
    DataFrame *df = NULL;
    if (g_file_test(ML_FILES[k], G_FILE_TEST_EXISTS)) {
        df = load_csv(app, ML_FILES[k]);
    }
    table_show(&app->ml_table, df);   /* shows the new frame first */
    df_free(app->ml_frame);
    app->ml_frame = df;
}

static void on_ml_view_changed(GObject *o, GParamSpec *p, gpointer app)
{
    (void)o;
    (void)p;
    show_ml_file(app);
}

static void on_pipeline_done(GObject *source, GAsyncResult *res,
                             gpointer user_data)
{
    App *app = user_data;
    GSubprocess *process = G_SUBPROCESS(source);
    char *output = NULL;
    GError *error = NULL;
    g_subprocess_communicate_utf8_finish(process, res, &output, NULL, &error);

    GtkTextBuffer *log = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->ml_log));
    gtk_text_buffer_set_text(log, output ? output
                                         : (error ? error->message : ""), -1);
    if (error == NULL && g_subprocess_get_successful(process)) {
        load_ml_results(app);
        show_ml_file(app);
    } else {
        gtk_label_set_text(GTK_LABEL(app->ml_status),
                           "The pipeline failed: see the log on the left.");
    }
    gtk_widget_set_sensitive(app->ml_run_button, TRUE);
    g_free(output);
    g_clear_error(&error);
    g_object_unref(process);
}

static void on_run_pipeline(GtkButton *b, gpointer user_data)
{
    (void)b;
    App *app = user_data;
    if (!g_file_test("ml/run.sh", G_FILE_TEST_EXISTS)) {
        gtk_label_set_text(GTK_LABEL(app->ml_status),
                           "Start the GUI from the CPandas folder to run "
                           "the pipeline.");
        return;
    }
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
        &error, "make", "--no-print-directory", "ml", NULL);
    if (process == NULL) {
        gtk_label_set_text(GTK_LABEL(app->ml_status), error->message);
        g_clear_error(&error);
        return;
    }
    gtk_widget_set_sensitive(app->ml_run_button, FALSE);
    gtk_label_set_text(GTK_LABEL(app->ml_status),
                       "Running: CPandas preprocessing, then scikit-learn "
                       "(about 10 seconds)…");
    GtkTextBuffer *log = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->ml_log));
    gtk_text_buffer_set_text(log, "Running make ml …", -1);
    g_subprocess_communicate_utf8_async(process, NULL, NULL,
                                        on_pipeline_done, app);
}

/* Bar chart of the strongest features: red = more likely to leave,
   green = less likely. */
static void draw_features(GtkDrawingArea *area, cairo_t *cr, int width,
                          int height, gpointer user_data)
{
    (void)area;
    App *app = user_data;
    if (app->nfeatures == 0) {
        return;
    }
    double biggest = 0.0;
    for (int k = 0; k < app->nfeatures; k++) {
        biggest = fmax(biggest, fabs(app->feature_weights[k]));
    }
    double row_height = (double)height / app->nfeatures;
    double scale = app->zoom / 100.0;
    double name_width = 170.0 * scale;
    double middle = name_width + (width - name_width - 60) / 2.0;
    double half = (width - name_width - 60) / 2.0;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5 * scale);

    for (int k = 0; k < app->nfeatures; k++) {
        double w = app->feature_weights[k];
        double y = k * row_height;
        double bar = biggest > 0 ? fabs(w) / biggest * half : 0;
        if (w >= 0) {
            cairo_set_source_rgb(cr, 0.80, 0.36, 0.30);     /* leaves  */
            cairo_rectangle(cr, middle, y + row_height * 0.2, bar,
                            row_height * 0.6);
        } else {
            cairo_set_source_rgb(cr, 0.18, 0.43, 0.39);     /* stays   */
            cairo_rectangle(cr, middle - bar, y + row_height * 0.2, bar,
                            row_height * 0.6);
        }
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.25, 0.25, 0.27);
        cairo_move_to(cr, 4, y + row_height * 0.62);
        cairo_show_text(cr, app->feature_names[k]);
        char number[32];
        snprintf(number, sizeof number, "%+.2f", w);
        cairo_move_to(cr, w >= 0 ? middle + bar + 6 : middle - bar - 42 * scale,
                      y + row_height * 0.62);
        cairo_show_text(cr, number);
    }
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, middle + 0.5, 0);
    cairo_line_to(cr, middle + 0.5, height);
    cairo_stroke(cr);
}

static GtkWidget *score_card(App *app, int k)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "card");
    gtk_widget_set_hexpand(box, TRUE);
    app->ml_scores[k] = gtk_label_new("–");
    gtk_widget_add_css_class(app->ml_scores[k], "score");
    app->ml_score_notes[k] = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(app->ml_score_notes[k]), TRUE);
    gtk_widget_add_css_class(app->ml_score_notes[k], "dim");
    gtk_box_append(GTK_BOX(box), app->ml_scores[k]);
    gtk_box_append(GTK_BOX(box), app->ml_score_notes[k]);
    return box;
}

static GtkWidget *build_confusion(App *app)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_attach(GTK_GRID(grid), small_label(""), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), small_label("predicted stays"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), small_label("predicted leaves"), 2, 0, 1,
                    1);
    gtk_grid_attach(GTK_GRID(grid), small_label("actually stayed"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), small_label("actually left"), 0, 2, 1, 1);
    const char *styles[4] = {"good", "bad", "bad", "good"};
    for (int k = 0; k < 4; k++) {
        app->ml_confusion[k] = gtk_label_new("–");
        gtk_widget_add_css_class(app->ml_confusion[k], "cm");
        gtk_widget_add_css_class(app->ml_confusion[k], styles[k]);
        gtk_widget_set_hexpand(app->ml_confusion[k], TRUE);
        gtk_grid_attach(GTK_GRID(grid), app->ml_confusion[k], 1 + k % 2,
                        1 + k / 2, 1, 1);
    }
    return grid;
}

static GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_add_css_class(l, "card-title");
    return l;
}

static GtkWidget *build_ml_tab(App *app)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "toolbar");
    app->ml_run_button = gtk_button_new_with_label("▶ Run pipeline");
    gtk_widget_add_css_class(app->ml_run_button, "suggested-action");
    g_signal_connect(app->ml_run_button, "clicked",
                     G_CALLBACK(on_run_pipeline), app);
    app->ml_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->ml_status), 0.0f);
    gtk_box_append(GTK_BOX(bar), app->ml_run_button);
    gtk_box_append(GTK_BOX(bar), app->ml_status);
    gtk_box_append(GTK_BOX(page), bar);

    /* top: log | results */
    GtkWidget *top = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    app->ml_log = gtk_text_view_new();
    gtk_widget_add_css_class(app->ml_log, "logview");
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->ml_log), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->ml_log), 8);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->ml_log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->ml_log), TRUE);
    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->ml_log)),
        "Press ▶ Run pipeline to clean the raw data with CPandas and train\n"
        "the model with scikit-learn. Every step's output appears here.", -1);
    GtkWidget *log_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scroll),
                                  app->ml_log);
    gtk_paned_set_start_child(GTK_PANED(top), log_scroll);

    GtkWidget *results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(results, 10);
    gtk_widget_set_margin_end(results, 10);
    gtk_widget_set_margin_top(results, 10);
    gtk_box_append(GTK_BOX(results), heading("Test results"));
    GtkWidget *scores = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (int k = 0; k < 4; k++) {
        gtk_box_append(GTK_BOX(scores), score_card(app, k));
    }
    gtk_box_append(GTK_BOX(results), scores);
    gtk_box_append(GTK_BOX(results), heading("Confusion matrix"));
    gtk_box_append(GTK_BOX(results), build_confusion(app));
    gtk_box_append(GTK_BOX(results),
                   heading("Strongest signals (red: more likely to leave, "
                           "green: less likely)"));
    app->ml_chart = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->ml_chart), 200);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->ml_chart),
                                   draw_features, app, NULL);
    gtk_box_append(GTK_BOX(results), app->ml_chart);
    GtkWidget *results_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(results_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results_scroll),
                                  results);
    gtk_paned_set_end_child(GTK_PANED(top), results_scroll);
    gtk_paned_set_position(GTK_PANED(top), 520);

    /* bottom: a data file as a table */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *choose = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(choose, "toolbar");
    gtk_box_append(GTK_BOX(choose), small_label("Show:"));
    const char *const views[] = {
        "raw export (ml/data/raw/employees.csv)",
        "cleaned training data (build/ml/train.csv)",
        "test predictions (build/ml/predictions.csv)",
        "learned preprocessing values (build/ml/preprocessing_params.csv)",
        NULL};
    app->ml_view_choice = choice_dropdown(views);
    g_signal_connect(app->ml_view_choice, "notify::selected",
                     G_CALLBACK(on_ml_view_changed), app);
    gtk_box_append(GTK_BOX(choose), app->ml_view_choice);
    gtk_box_append(GTK_BOX(bottom), choose);
    gtk_box_append(GTK_BOX(bottom), table_new(&app->ml_table));

    GtkWidget *split = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(split), top);
    gtk_paned_set_end_child(GTK_PANED(split), bottom);
    gtk_paned_set_position(GTK_PANED(split), 430);
    gtk_widget_set_vexpand(split, TRUE);
    gtk_box_append(GTK_BOX(page), split);
    return page;
}

/* ---- zoom --------------------------------------------------------------- */

/* All text in the window inherits the window's font size, so one CSS rule
   scales everything: table cells, labels, buttons, the dropdown lists. Only
   the bar chart is drawn by hand, so it reads app->zoom itself. */
static void apply_zoom(App *app)
{
    char css[96];
    snprintf(css, sizeof css, "window { font-size: %d%%; }", app->zoom);
    gtk_css_provider_load_from_string(app->zoom_css, css);

    char text[16];
    snprintf(text, sizeof text, "%d%%", app->zoom);
    gtk_button_set_label(GTK_BUTTON(app->zoom_label), text);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->ml_chart),
                                        200 * app->zoom / 100);
    gtk_widget_queue_draw(app->ml_chart);
}

static void change_zoom(App *app, int step)
{
    int zoom = (step == 0) ? 100 : app->zoom + step;
    if (zoom < ZOOM_MIN) {
        zoom = ZOOM_MIN;
    }
    if (zoom > ZOOM_MAX) {
        zoom = ZOOM_MAX;
    }
    if (zoom != app->zoom) {
        app->zoom = zoom;
        apply_zoom(app);
    }
}

static void on_zoom_in(GSimpleAction *a, GVariant *v, gpointer app)
{
    (void)a;
    (void)v;
    change_zoom(app, ZOOM_STEP);
}

static void on_zoom_out(GSimpleAction *a, GVariant *v, gpointer app)
{
    (void)a;
    (void)v;
    change_zoom(app, -ZOOM_STEP);
}

static void on_zoom_reset(GSimpleAction *a, GVariant *v, gpointer app)
{
    (void)a;
    (void)v;
    change_zoom(app, 0);
}

/* Ctrl + mouse wheel. The controller sees the wheel before the tables do
   (capture phase), and only takes it when Ctrl is held; otherwise the
   wheel scrolls as usual. A touchpad sends many small movements, so they
   are added up until they make one whole step. */
static gboolean on_scroll(GtkEventControllerScroll *controller, double dx,
                          double dy, gpointer user_data)
{
    (void)dx;
    App *app = user_data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(controller));
    if ((state & GDK_CONTROL_MASK) == 0) {
        return FALSE;
    }
    app->scroll_sum += dy;
    while (app->scroll_sum <= -1.0) {
        change_zoom(app, ZOOM_STEP);        /* wheel up: bigger */
        app->scroll_sum += 1.0;
    }
    while (app->scroll_sum >= 1.0) {
        change_zoom(app, -ZOOM_STEP);
        app->scroll_sum -= 1.0;
    }
    return TRUE;
}

/* The zoom actions, their keys, the title-bar buttons and Ctrl + wheel. */
static void add_zoom_controls(App *app, GtkApplication *application,
                              GtkWidget *header)
{
    const char *names[3] = {"zoom-in", "zoom-out", "zoom-reset"};
    GCallback callbacks[3] = {G_CALLBACK(on_zoom_in), G_CALLBACK(on_zoom_out),
                              G_CALLBACK(on_zoom_reset)};
    for (int k = 0; k < 3; k++) {
        GSimpleAction *action = g_simple_action_new(names[k], NULL);
        g_signal_connect(action, "activate", callbacks[k], app);
        g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(action));
        g_object_unref(action);
    }
    const char *in_keys[] = {"<Control>plus", "<Control>equal",
                             "<Control>KP_Add", NULL};
    const char *out_keys[] = {"<Control>minus", "<Control>KP_Subtract", NULL};
    const char *reset_keys[] = {"<Control>0", "<Control>KP_0", NULL};
    gtk_application_set_accels_for_action(application, "win.zoom-in",
                                          in_keys);
    gtk_application_set_accels_for_action(application, "win.zoom-out",
                                          out_keys);
    gtk_application_set_accels_for_action(application, "win.zoom-reset",
                                          reset_keys);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "linked");
    GtkWidget *out = gtk_button_new_with_label("−");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(out), "win.zoom-out");
    gtk_widget_set_tooltip_text(out, "Smaller text (Ctrl + −)");
    app->zoom_label = gtk_button_new_with_label("100%");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(app->zoom_label),
                                   "win.zoom-reset");
    gtk_widget_set_tooltip_text(app->zoom_label, "Normal size (Ctrl + 0)");
    GtkWidget *in = gtk_button_new_with_label("+");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(in), "win.zoom-in");
    gtk_widget_set_tooltip_text(in, "Bigger text (Ctrl + =)");
    gtk_box_append(GTK_BOX(box), out);
    gtk_box_append(GTK_BOX(box), app->zoom_label);
    gtk_box_append(GTK_BOX(box), in);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), box);

    GtkEventController *wheel = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
    g_signal_connect(wheel, "scroll", G_CALLBACK(on_scroll), app);
    gtk_widget_add_controller(GTK_WIDGET(app->window), wheel);

    /* One step above the main style, so its font size wins. */
    app->zoom_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(app->zoom_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    apply_zoom(app);
}

/* ---- window, style, start and end -------------------------------------- */

static const char *STYLE =
    "window { background: #f6f6f3; color: #1d2126; }"
    ".toolbar, .sidebar, .card, .status { color: #1d2126; }"
    ".toolbar { padding: 8px 10px; background: #ffffff;"
    "  border-bottom: 1px solid #e2e2dd; }"
    ".sidebar { background: #f0f0ec; border-left: 1px solid #e2e2dd; }"
    ".card { background: #ffffff; border: 1px solid #e2e2dd;"
    "  border-radius: 10px; padding: 10px; }"
    ".card-title { font-weight: bold; color: #2f6e64; }"
    ".dim { color: #6b6f76; font-size: 0.9em; }"
    ".mono { font-family: monospace; }"
    ".table-name { font-weight: bold; margin-right: 8px; }"
    ".status { padding: 6px 10px; background: #ffffff;"
    "  border-top: 1px solid #e2e2dd; color: #2f6e64; }"
    ".status.error { color: #b3261e; }"
    ".cell { padding: 1px 6px; font-family: monospace; }"
    ".cell.missing { color: #b0b0b0; font-style: italic; }"
    ".cell.rownumber { color: #9a9a9a; }"
    ".table columnview { background: #ffffff; }"
    ".score { font-size: 1.9em; font-weight: bold; color: #1f3d37; }"
    ".cm { padding: 10px; border-radius: 8px; font-size: 1.3em;"
    "  font-weight: bold; }"
    ".cm.good { background: #dcede7; color: #1f3d37; }"
    ".cm.bad { background: #f6dfdc; color: #7a2a22; }"
    "textview.logview, textview.logview text {"
    "  background-color: #1d2126; color: #e6e4de; }"
    "button.suggested-action { background: #2f6e64; color: #ffffff; }";

static void on_shutdown(GApplication *application, gpointer user_data)
{
    (void)application;
    App *app = user_data;
    df_free(app->current);
    for (int k = 0; k < app->nhistory; k++) {
        df_free(app->history[k]);
    }
    df_free(app->ml_frame);
    g_clear_object(&app->columns);
    g_clear_object(&app->columns_or_none);
    g_clear_object(&app->zoom_css);
    g_free(app);
}

/* The colours above are made for a light window. On a desktop with a dark
   theme (for example Ubuntu's Yaru-dark), the theme's white text would land
   on these light backgrounds and buttons would vanish. So the GUI always
   uses GTK's built-in Adwaita theme in its light version, whatever the
   desktop uses. */
static void use_light_theme(void)
{
    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-theme-name", "Adwaita",
                 "gtk-application-prefer-dark-theme", FALSE, NULL);
#if GTK_CHECK_VERSION(4, 20, 0)
    /* GTK 4.20 and later also follow the desktop's light / dark setting
       directly, so that has to be set to light as well. */
    g_object_set(settings, "gtk-interface-color-scheme",
                 GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
#endif
}

static void on_activate(GtkApplication *application, gpointer user_data)
{
    App *app = user_data;
    use_light_theme();
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, STYLE);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *window = gtk_application_window_new(application);
    app->window = GTK_WINDOW(window);
    gtk_window_set_title(app->window, "CPandas");
    gtk_window_set_default_size(app->window, 1360, 820);

    GtkWidget *stack = gtk_stack_new();
    gtk_stack_add_titled(GTK_STACK(stack), build_data_tab(app), "data",
                         "Data");
    gtk_stack_add_titled(GTK_STACK(stack), build_ml_tab(app), "ml",
                         "ML pipeline");
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher),
                                 GTK_STACK(stack));
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), switcher);
    gtk_window_set_titlebar(app->window, header);
    gtk_window_set_child(app->window, stack);
    add_zoom_controls(app, application, header);

    /* Start with the employees example, or the sample if it is not here. */
    const char *start = "examples/data/employees.csv";
    DataFrame *df = NULL;
    if (g_file_test(start, G_FILE_TEST_EXISTS)) {
        df = load_csv(app, start);
    }
    if (df != NULL) {
        set_current(app, df, "employees.csv",
                    "Opened examples/data/employees.csv. Pick an operation "
                    "on the right; Undo goes back.");
    } else {
        df_make_sample(&df);
        set_current(app, df, "sample table", "Loaded the built-in sample.");
    }
    load_ml_results(app);
    show_ml_file(app);
    gtk_window_present(app->window);
}

int main(int argc, char **argv)
{
    App *app = g_new0(App, 1);
    app->zoom = 100;
    app->columns = gtk_string_list_new(NULL);
    app->columns_or_none = gtk_string_list_new(NULL);

    GtkApplication *application = gtk_application_new(
        "io.github.samyuktha.CPandas", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), app);
    g_signal_connect(application, "shutdown", G_CALLBACK(on_shutdown), app);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
