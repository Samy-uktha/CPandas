/*
 * csv.c - read a DataFrame from CSV text and write one back out.
 *
 * Reading happens in two steps:
 *   1. Parse: split the text into rows of fields (plain strings), handling
 *      quotes, line endings and blank lines. Nothing is converted yet.
 *   2. Build: for every column, look at all its fields to decide the type
 *      (INT, FLOAT or STRING), then create the Series and fill it.
 * Doing it in two steps means a column's type can depend on all of its
 * values, e.g. "1, 2, 3.5" becomes FLOAT, not INT.
 */
#include "csv.h"

#include <errno.h>
#include <math.h>       /* only for HUGE_VAL */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_CHUNK 4096     /* bytes read from the file at a time */

/* ---- small growing arrays used while parsing ---------------------------- */

/* A string that grows one character at a time. */
typedef struct {
    char  *text;
    size_t len;
    size_t cap;
} TextBuffer;

/* One line of the file: its fields as strings. */
typedef struct {
    char  **fields;
    size_t  count;
    size_t  cap;
} Row;

/* All the lines of the file. */
typedef struct {
    Row   *rows;
    size_t count;
    size_t cap;
} RowList;

static df_status text_add_char(TextBuffer *buf, char c)
{
    /* Always keep room for one more character plus the final '\0'. */
    if (buf->len + 2 > buf->cap) {
        size_t new_cap = 32;
        if (buf->cap > 0) {
            new_cap = buf->cap * 2;
        }
        char *bigger = realloc(buf->text, new_cap);
        if (bigger == NULL) {
            return DF_ERR_NOMEM;
        }
        buf->text = bigger;
        buf->cap = new_cap;
    }
    buf->text[buf->len] = c;
    buf->len++;
    buf->text[buf->len] = '\0';
    return DF_OK;
}

/* Adds a field to a row. The row takes ownership of `field` on success. */
static df_status row_add_field(Row *row, char *field)
{
    if (row->count == row->cap) {
        size_t new_cap = 8;
        if (row->cap > 0) {
            new_cap = row->cap * 2;
        }
        char **bigger = realloc(row->fields, new_cap * sizeof(char *));
        if (bigger == NULL) {
            return DF_ERR_NOMEM;
        }
        row->fields = bigger;
        row->cap = new_cap;
    }
    row->fields[row->count] = field;
    row->count++;
    return DF_OK;
}

static void row_free(Row *row)
{
    for (size_t i = 0; i < row->count; i++) {
        free(row->fields[i]);
    }
    free(row->fields);
    row->fields = NULL;
    row->count = 0;
    row->cap = 0;
}

/* Adds a row to the list. The list takes ownership of the row's memory on
   success (the Row struct itself is copied). */
static df_status list_add_row(RowList *list, Row row)
{
    if (list->count == list->cap) {
        size_t new_cap = 16;
        if (list->cap > 0) {
            new_cap = list->cap * 2;
        }
        Row *bigger = realloc(list->rows, new_cap * sizeof(Row));
        if (bigger == NULL) {
            return DF_ERR_NOMEM;
        }
        list->rows = bigger;
        list->cap = new_cap;
    }
    list->rows[list->count] = row;
    list->count++;
    return DF_OK;
}

static void list_free(RowList *list)
{
    for (size_t r = 0; r < list->count; r++) {
        row_free(&list->rows[r]);
    }
    free(list->rows);
    list->rows = NULL;
    list->count = 0;
    list->cap = 0;
}

/* ---- options ------------------------------------------------------------ */

csv_options csv_default_options(void)
{
    csv_options opts;
    opts.delimiter = ',';
    opts.has_header = 1;
    opts.na_values = NULL;
    opts.n_na_values = 0;
    return opts;
}

/* Checks the options the caller gave us. */
static df_status check_options(const csv_options *opts)
{
    char d = opts->delimiter;
    if (d == '\0' || d == '"' || d == '\n' || d == '\r') {
        return DF_ERR_INVALID_ARG;
    }
    if (opts->n_na_values > 0 && opts->na_values == NULL) {
        return DF_ERR_NULL_ARG;
    }
    for (size_t k = 0; k < opts->n_na_values; k++) {
        if (opts->na_values[k] == NULL) {
            return DF_ERR_NULL_ARG;
        }
    }
    return DF_OK;
}

/* ---- step 1: parsing the text into rows of fields ----------------------- */

/* A space or tab that is not the delimiter (the delimiter may be a tab). */
static int is_blank_char(char c, char delimiter)
{
    if (c == delimiter) {
        return 0;
    }
    return c == ' ' || c == '\t';
}

/* 1 if c ends a field without being part of it. */
static int ends_field(char c, char delimiter)
{
    return c == delimiter || c == '\n' || c == '\r' || c == '\0';
}

/* Reads one field starting at *pos and stops in front of the delimiter /
   end of line / end of text. On success *out_text is a new string (maybe
   "") that the caller must free. */
static df_status read_field(const char **pos, char delimiter, char **out_text)
{
    const char *p = *pos;
    TextBuffer buf = {NULL, 0, 0};
    df_status st = DF_OK;

    /* spaces before the field are ignored */
    while (is_blank_char(*p, delimiter)) {
        p++;
    }

    if (*p == '"') {
        /* Quoted field: everything up to the closing quote, where "" means
           one " character. It may contain delimiters and newlines. */
        p++;
        while (st == DF_OK) {
            if (*p == '\0') {
                st = DF_ERR_PARSE;          /* the quote was never closed */
            } else if (*p == '"' && p[1] == '"') {
                st = text_add_char(&buf, '"');
                p += 2;
            } else if (*p == '"') {
                p++;                        /* the closing quote */
                break;
            } else {
                st = text_add_char(&buf, *p);
                p++;
            }
        }
        /* only spaces may come between the closing quote and the end */
        while (st == DF_OK && is_blank_char(*p, delimiter)) {
            p++;
        }
        if (st == DF_OK && !ends_field(*p, delimiter)) {
            st = DF_ERR_PARSE;              /* like "ab"c */
        }
    } else {
        /* Unquoted field: everything up to the delimiter or end of line. */
        while (st == DF_OK && !ends_field(*p, delimiter)) {
            st = text_add_char(&buf, *p);
            p++;
        }
        /* remove spaces at the end */
        while (st == DF_OK && buf.len > 0 &&
               is_blank_char(buf.text[buf.len - 1], delimiter)) {
            buf.len--;
            buf.text[buf.len] = '\0';
        }
    }

    /* An empty field still needs a real (empty) string. */
    if (st == DF_OK && buf.text == NULL) {
        buf.text = malloc(1);
        if (buf.text == NULL) {
            st = DF_ERR_NOMEM;
        } else {
            buf.text[0] = '\0';
        }
    }

    if (st != DF_OK) {
        free(buf.text);
        return st;
    }
    *out_text = buf.text;
    *pos = p;
    return DF_OK;
}

/* Reads one whole line of fields into `row` and moves *pos to the start of
   the next line. On failure the caller frees the row. */
static df_status read_row(const char **pos, char delimiter, Row *row)
{
    const char *p = *pos;
    while (1) {
        char *text = NULL;
        df_status st = read_field(&p, delimiter, &text);
        if (st != DF_OK) {
            return st;
        }
        st = row_add_field(row, text);
        if (st != DF_OK) {
            free(text);
            return st;
        }
        if (*p == delimiter) {
            p++;            /* another field follows */
        } else {
            break;          /* end of line or end of text */
        }
    }

    /* skip the line ending: "\r\n", "\n" or "\r" */
    if (*p == '\r') {
        p++;
    }
    if (*p == '\n') {
        p++;
    }
    *pos = p;
    return DF_OK;
}

/* Splits the whole text into rows. Every row must have the same number of
   fields as the first one. */
static df_status parse_text(const char *text, char delimiter, RowList *list)
{
    const char *p = text;

    /* Skip the UTF-8 "byte order mark" that Excel puts at the start. */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
    }

    while (1) {
        /* Skip blank lines (empty, or only spaces). */
        const char *q = p;
        while (is_blank_char(*q, delimiter)) {
            q++;
        }
        if (*q == '\0') {
            break;                  /* end of the text */
        }
        if (*q == '\n' || *q == '\r') {
            p = q;
            if (*p == '\r') p++;
            if (*p == '\n') p++;
            continue;
        }

        Row row = {NULL, 0, 0};
        df_status st = read_row(&p, delimiter, &row);
        if (st == DF_OK && list->count > 0 &&
            row.count != list->rows[0].count) {
            st = DF_ERR_PARSE;      /* wrong number of fields */
        }
        if (st == DF_OK) {
            st = list_add_row(list, row);
        }
        if (st != DF_OK) {
            row_free(&row);
            return st;
        }
    }
    return DF_OK;
}

/* ---- step 2: deciding types and building the frame ---------------------- */

/* A cell is missing if it is empty or one of the "missing" words. */
static int is_missing_text(const char *text, const csv_options *opts)
{
    if (text[0] == '\0') {
        return 1;
    }
    for (size_t k = 0; k < opts->n_na_values; k++) {
        if (strcmp(text, opts->na_values[k]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* "-12", "+7", "007": a sign and digits, and small enough for int64_t. */
static int is_int_text(const char *text)
{
    const char *p = text;
    if (*p == '+' || *p == '-') {
        p++;
    }
    if (*p == '\0') {
        return 0;           /* just a sign, or nothing */
    }
    while (*p != '\0') {
        if (!is_digit(*p)) {
            return 0;
        }
        p++;
    }
    errno = 0;
    strtoll(text, NULL, 10);
    if (errno == ERANGE) {
        return 0;           /* too big for an int, can still be a FLOAT */
    }
    return 1;
}

/* "3.5", "-.5", "2.", "1e3", "6.02E+23": a number strtod can read, but
   checked by hand first so that "nan", "inf", "0x1F" or " 5" are text. */
static int is_number_text(const char *text)
{
    const char *p = text;
    int digits = 0;

    if (*p == '+' || *p == '-') {
        p++;
    }
    while (is_digit(*p)) {
        p++;
        digits++;
    }
    if (*p == '.') {
        p++;
        while (is_digit(*p)) {
            p++;
            digits++;
        }
    }
    if (digits == 0) {
        return 0;           /* no digit at all, e.g. "." or "-" */
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') {
            p++;
        }
        if (!is_digit(*p)) {
            return 0;       /* "1e" or "1e+" */
        }
        while (is_digit(*p)) {
            p++;
        }
    }
    if (*p != '\0') {
        return 0;           /* something else follows */
    }

    errno = 0;
    double v = strtod(text, NULL);
    if (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)) {
        return 0;           /* too big even for a double, e.g. 1e999 */
    }
    return 1;
}

/* Looks at every non-missing cell of column `col` to choose its type. */
static df_dtype infer_type(const RowList *list, size_t first_row, size_t col,
                           const csv_options *opts)
{
    int any_value = 0;
    int all_int = 1;
    int all_number = 1;

    for (size_t r = first_row; r < list->count; r++) {
        const char *text = list->rows[r].fields[col];
        if (is_missing_text(text, opts)) {
            continue;
        }
        any_value = 1;
        if (!is_int_text(text)) {
            all_int = 0;
        }
        if (!is_number_text(text)) {
            all_number = 0;
            break;          /* it is text, no need to look further */
        }
    }

    if (!any_value) {
        return DTYPE_STRING;    /* empty or all-missing column */
    }
    if (all_int) {
        return DTYPE_INT;
    }
    if (all_number) {
        return DTYPE_FLOAT;
    }
    return DTYPE_STRING;
}

/* Creates the Series for column `col` and fills it with every data row. */
static df_status build_column(const RowList *list, size_t first_row,
                              size_t col, const char *name,
                              const csv_options *opts, Series **out)
{
    df_dtype type = infer_type(list, first_row, col, opts);
    Series *s = NULL;
    df_status st = series_create(name, type, list->count - first_row, &s);
    if (st != DF_OK) {
        return st;
    }

    for (size_t r = first_row; r < list->count && st == DF_OK; r++) {
        const char *text = list->rows[r].fields[col];
        if (is_missing_text(text, opts)) {
            st = series_append_missing(s);
        } else if (type == DTYPE_INT) {
            st = series_append_int(s, (int64_t)strtoll(text, NULL, 10));
        } else if (type == DTYPE_FLOAT) {
            st = series_append_float(s, strtod(text, NULL));
        } else {
            st = series_append_string(s, text);
        }
    }

    if (st != DF_OK) {
        series_free(s);
        return st;
    }
    *out = s;
    return DF_OK;
}

/* Turns the parsed rows into a DataFrame. */
static df_status build_frame(const RowList *list, const csv_options *opts,
                             DataFrame **out)
{
    DataFrame *df = NULL;
    df_status st = df_create(&df);
    if (st != DF_OK) {
        return st;
    }
    if (list->count == 0) {
        *out = df;              /* empty input: a frame with no columns */
        return DF_OK;
    }

    size_t ncols = list->rows[0].count;
    size_t first_row = 0;
    if (opts->has_header) {
        first_row = 1;          /* row 0 holds the column names */
    }

    for (size_t c = 0; c < ncols && st == DF_OK; c++) {
        /* The column name: from the header, or "col<N>". */
        char made_up_name[32];
        const char *name = made_up_name;
        snprintf(made_up_name, sizeof made_up_name, "col%zu", c);
        if (opts->has_header && list->rows[0].fields[c][0] != '\0') {
            name = list->rows[0].fields[c];
        }

        Series *s = NULL;
        st = build_column(list, first_row, c, name, opts, &s);
        if (st == DF_OK) {
            st = df_add_column(df, s);  /* e.g. DUPLICATE_NAME */
            if (st != DF_OK) {
                series_free(s);
            }
        }
    }

    if (st != DF_OK) {
        df_free(df);
        return st;
    }
    *out = df;
    return DF_OK;
}

/* ---- public reading functions ------------------------------------------- */

df_status df_read_csv_string(const char *text, const csv_options *opts,
                             DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (text == NULL) {
        return DF_ERR_NULL_ARG;
    }

    csv_options defaults = csv_default_options();
    if (opts == NULL) {
        opts = &defaults;
    }
    df_status st = check_options(opts);
    if (st != DF_OK) {
        return st;
    }

    RowList list = {NULL, 0, 0};
    st = parse_text(text, opts->delimiter, &list);
    if (st == DF_OK) {
        st = build_frame(&list, opts, out);
    }
    list_free(&list);   /* the frame has its own copies of everything */
    return st;
}

df_status df_read_csv(const char *path, const csv_options *opts,
                      DataFrame **out)
{
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (path == NULL) {
        return DF_ERR_NULL_ARG;
    }

    /* "rb" so Windows does not change the line endings; read_row() handles
       "\r\n" itself. */
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return DF_ERR_IO;
    }

    /* Read the whole file into one string, a chunk at a time. */
    char *text = NULL;
    size_t len = 0;
    size_t cap = 0;
    df_status st = DF_OK;
    while (st == DF_OK) {
        if (len + READ_CHUNK + 1 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap < len + READ_CHUNK + 1) {
                new_cap = len + READ_CHUNK + 1;
            }
            char *bigger = realloc(text, new_cap);
            if (bigger == NULL) {
                st = DF_ERR_NOMEM;
                break;
            }
            text = bigger;
            cap = new_cap;
        }
        size_t got = fread(text + len, 1, READ_CHUNK, f);
        len += got;
        if (got < READ_CHUNK) {
            if (ferror(f)) {
                st = DF_ERR_IO;
            }
            break;          /* end of file (or an error) */
        }
    }
    fclose(f);

    if (st == DF_OK) {
        text[len] = '\0';
        /* A '\0' byte inside the file would cut the text short. */
        if (strlen(text) != len) {
            st = DF_ERR_PARSE;
        }
    }
    if (st == DF_OK) {
        st = df_read_csv_string(text, opts, out);
    }
    free(text);
    return st;
}

/* ---- writing ------------------------------------------------------------ */

/* 1 if the text must be put in quotes to be read back the same way. */
static int needs_quotes(const char *text, char delimiter)
{
    size_t len = strlen(text);
    if (len > 0 && (text[0] == ' ' || text[0] == '\t' ||
                    text[len - 1] == ' ' || text[len - 1] == '\t')) {
        return 1;           /* the reader would trim these spaces */
    }
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
            return 1;
        }
    }
    return 0;
}

static void write_text(FILE *f, const char *text, char delimiter)
{
    if (!needs_quotes(text, delimiter)) {
        fputs(text, f);
        return;
    }
    fputc('"', f);
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '"') {
            fputc('"', f);  /* a quote inside is written twice */
        }
        fputc(text[i], f);
    }
    fputc('"', f);
}

/* Shortest text that reads back as exactly the same double, and always
   with a '.' or 'e' so it is read back as FLOAT and not INT. */
static void format_double(double v, char *buf, size_t size)
{
    snprintf(buf, size, "%.15g", v);
    if (strtod(buf, NULL) != v) {
        snprintf(buf, size, "%.17g", v);   /* 17 digits always round-trip */
    }
    if (strchr(buf, '.') == NULL && strchr(buf, 'e') == NULL &&
        strchr(buf, 'n') == NULL) {        /* 'n': "inf" / "nan" */
        strcat(buf, ".0");
    }
}

df_status df_write_csv(const DataFrame *df, const char *path,
                       const csv_options *opts)
{
    if (df == NULL || path == NULL) {
        return DF_ERR_NULL_ARG;
    }
    csv_options defaults = csv_default_options();
    if (opts == NULL) {
        opts = &defaults;
    }
    df_status st = check_options(opts);
    if (st != DF_OK) {
        return st;
    }

    /* "wb" so every line ends with "\n" on Windows too. */
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return DF_ERR_IO;
    }

    char d = opts->delimiter;
    size_t ncols = df_ncols(df);
    size_t nrows = df_nrows(df);

    if (opts->has_header && ncols > 0) {
        for (size_t c = 0; c < ncols; c++) {
            const char *name = NULL;
            df_column_name(df, c, &name);
            if (c > 0) {
                fputc(d, f);
            }
            write_text(f, name, d);
        }
        fputc('\n', f);
    }

    char number[64];
    for (size_t r = 0; r < nrows; r++) {
        for (size_t c = 0; c < ncols; c++) {
            const Series *col = NULL;
            df_get_column_at(df, c, &col);
            if (c > 0) {
                fputc(d, f);
            }

            if (series_is_missing(col, r)) {
                /* An empty field. With only one column the line would be
                   empty and skipped when reading, so write "" instead. */
                if (ncols == 1) {
                    fputs("\"\"", f);
                }
            } else if (series_dtype(col) == DTYPE_INT) {
                int64_t v = 0;
                series_get_int(col, r, &v);
                fprintf(f, "%lld", (long long)v);
            } else if (series_dtype(col) == DTYPE_FLOAT) {
                double v = 0.0;
                series_get_float(col, r, &v);
                format_double(v, number, sizeof number);
                fputs(number, f);
            } else {
                const char *v = NULL;
                series_get_string(col, r, &v);
                if (v[0] == '\0') {
                    /* an empty (but not missing) text: write "" so that a
                       one-column line is not empty. It is read back as a
                       missing cell (see csv.h). */
                    fputs("\"\"", f);
                } else {
                    write_text(f, v, d);
                }
            }
        }
        fputc('\n', f);
    }

    /* Check that everything really reached the file. */
    if (ferror(f)) {
        st = DF_ERR_IO;
    }
    if (fclose(f) != 0) {
        st = DF_ERR_IO;
    }
    return st;
}
