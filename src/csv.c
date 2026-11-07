/*
 * csv.c - read a DataFrame from CSV text and write one back out.
 *
 * Reading happens in two steps:
 *   1. Parse: split the text into rows of fields (plain strings), handling
 *      quotes, line endings and blank lines. Nothing is converted yet.
 *   2. Build: for every column, look at all its fields to decide the type
 *      (INT, FLOAT, BOOL, DATE or STRING), then create the Series and fill
 *      it.
 * Doing it in two steps means a column's type can depend on all of its
 * values, e.g. "1, 2, 3.5" becomes FLOAT, not INT.
 *
 * The parser reads its characters from a Source: either a string in
 * memory, or a file read READ_CHUNK bytes at a time. So a file is never
 * loaded into memory as a whole, and df_read_csv_chunks() can go through
 * a big file while only ever holding one chunk of rows.
 *
 * The Source also counts lines, so every parse error can say on which line
 * (and in which field) it happened.
 */
#include "csv.h"
#include "df_date.h"

#include <errno.h>
#include <math.h>       /* only for HUGE_VAL */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_CHUNK 65536    /* bytes read from the file at a time */
#define WRITE_BUFFER 65536  /* bytes collected before each write   */
#define END_OF_INPUT (-1)   /* what peek_char() gives after the last char */

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

/* ---- options and error reports ------------------------------------------ */

csv_options csv_default_options(void)
{
    csv_options opts;
    opts.delimiter = ',';
    opts.has_header = 1;
    opts.na_values = NULL;
    opts.n_na_values = 0;
    opts.error = NULL;
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

/* Fills in the caller's error report (if they asked for one). Only the
   first problem is kept: later calls do nothing once a message is set. */
static void report(csv_error *err, size_t line, size_t field,
                   const char *format, ...)
{
    if (err == NULL || err->message[0] != '\0') {
        return;
    }
    err->line = line;
    err->field = field;
    va_list args;
    va_start(args, format);
    vsnprintf(err->message, sizeof err->message, format, args);
    va_end(args);
}

static void clear_report(csv_error *err)
{
    if (err != NULL) {
        err->line = 0;
        err->field = 0;
        err->message[0] = '\0';
    }
}

/* ---- where the characters come from ------------------------------------- */

typedef struct {
    FILE       *file;       /* NULL when reading from a string             */
    const char *text;       /* string mode: the whole text                 */
    char        buf[READ_CHUNK]; /* file mode: the part read so far        */
    size_t      pos;        /* next character (in text or in buf)          */
    size_t      len;        /* file mode: how many bytes buf holds         */
    int         at_end;     /* file mode: the file has nothing more        */
    int         read_error; /* file mode: fread failed                     */
    int         nul_byte;   /* file mode: the file contains a '\0' byte    */
    size_t      line;       /* line of the next character, 1 = first line  */
} Source;

static void source_from_text(Source *src, const char *text)
{
    memset(src, 0, sizeof *src);
    src->text = text;
    src->line = 1;
}

static void source_from_file(Source *src, FILE *file)
{
    memset(src, 0, sizeof *src);
    src->file = file;
    src->line = 1;
}

/* File mode: makes sure buf holds at least `want` unread bytes, unless the
   file ends first. The unread bytes are moved to the front of buf, then
   the rest of buf is filled from the file. */
static void source_fill(Source *src, size_t want)
{
    while (src->len - src->pos < want && !src->at_end) {
        size_t unread = src->len - src->pos;
        memmove(src->buf, src->buf + src->pos, unread);
        src->pos = 0;
        src->len = unread;

        size_t got = fread(src->buf + unread, 1, READ_CHUNK - unread,
                           src->file);
        if (got == 0) {
            src->at_end = 1;
            if (ferror(src->file)) {
                src->read_error = 1;
            }
        }
        /* A '\0' byte would be taken for the end of a string later on, so
           the file is treated as ending there and the read fails. */
        void *nul = memchr(src->buf + unread, '\0', got);
        if (nul != NULL) {
            got = (size_t)((char *)nul - (src->buf + unread));
            src->at_end = 1;
            src->nul_byte = 1;
        }
        src->len = unread + got;
    }
}

/* The character `ahead` places after the next one (0 = the next one), or
   END_OF_INPUT. ahead must be less than 4. */
static int peek_char(Source *src, size_t ahead)
{
    if (src->file == NULL) {
        /* look one character at a time, never past the final '\0' */
        for (size_t k = 0; k <= ahead; k++) {
            if (src->text[src->pos + k] == '\0') {
                return END_OF_INPUT;
            }
        }
        return (unsigned char)src->text[src->pos + ahead];
    }
    source_fill(src, ahead + 1);
    if (src->pos + ahead >= src->len) {
        return END_OF_INPUT;
    }
    return (unsigned char)src->buf[src->pos + ahead];
}

/* Moves past the next character and keeps the line count up to date:
   "\n", "\r\n" and a lone "\r" each end one line. */
static void next_char(Source *src)
{
    int c = peek_char(src, 0);
    if (c == END_OF_INPUT) {
        return;
    }
    src->pos++;
    if (c == '\n' || (c == '\r' && peek_char(src, 0) != '\n')) {
        src->line++;
    }
}

/* ---- step 1: parsing the text into rows of fields ----------------------- */

/* Everything the parser needs while it goes through the text. */
typedef struct {
    Source    *src;
    char       delimiter;
    size_t     nfields;     /* fields per row, set by the first row (0 = none yet) */
    size_t     first_line;  /* line where the first row started            */
    csv_error *err;
} Parser;

/* A space or tab that is not the delimiter (the delimiter may be a tab). */
static int is_blank_char(int c, char delimiter)
{
    if (c == (unsigned char)delimiter) {
        return 0;
    }
    return c == ' ' || c == '\t';
}

/* 1 if c ends a field without being part of it. */
static int ends_field(int c, char delimiter)
{
    return c == (unsigned char)delimiter || c == '\n' || c == '\r' ||
           c == END_OF_INPUT;
}

/* Reads one field and stops in front of the delimiter / end of line / end
   of text. `field` is its position on the line (1 = first), for error
   messages. On success *out_text is a new string (maybe "") that the
   caller must free. */
static df_status read_field(Parser *ps, size_t field, char **out_text)
{
    Source *src = ps->src;
    char delimiter = ps->delimiter;
    TextBuffer buf = {NULL, 0, 0};
    df_status st = DF_OK;

    /* spaces before the field are ignored */
    while (is_blank_char(peek_char(src, 0), delimiter)) {
        next_char(src);
    }

    if (peek_char(src, 0) == '"') {
        /* Quoted field: everything up to the closing quote, where "" means
           one " character. It may contain delimiters and newlines. */
        size_t start_line = src->line;
        next_char(src);
        while (st == DF_OK) {
            int c = peek_char(src, 0);
            if (c == END_OF_INPUT) {
                st = DF_ERR_PARSE;          /* the quote was never closed */
                report(ps->err, start_line, field,
                       "the quote that opens field %zu is never closed",
                       field);
            } else if (c == '"' && peek_char(src, 1) == '"') {
                st = text_add_char(&buf, '"');
                next_char(src);
                next_char(src);
            } else if (c == '"') {
                next_char(src);             /* the closing quote */
                break;
            } else {
                st = text_add_char(&buf, (char)c);
                next_char(src);
            }
        }
        /* only spaces may come between the closing quote and the end */
        while (st == DF_OK && is_blank_char(peek_char(src, 0), delimiter)) {
            next_char(src);
        }
        if (st == DF_OK && !ends_field(peek_char(src, 0), delimiter)) {
            st = DF_ERR_PARSE;              /* like "ab"c */
            report(ps->err, src->line, field,
                   "unexpected text after the closing quote of field %zu",
                   field);
        }
    } else {
        /* Unquoted field: everything up to the delimiter or end of line. */
        while (st == DF_OK && !ends_field(peek_char(src, 0), delimiter)) {
            st = text_add_char(&buf, (char)peek_char(src, 0));
            next_char(src);
        }
        /* remove spaces at the end */
        while (st == DF_OK && buf.len > 0 &&
               is_blank_char((unsigned char)buf.text[buf.len - 1],
                             delimiter)) {
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
    return DF_OK;
}

/* Reads one whole line of fields into `row` and moves to the start of the
   next line. On failure the caller frees the row. */
static df_status read_row(Parser *ps, Row *row)
{
    Source *src = ps->src;
    while (1) {
        char *text = NULL;
        df_status st = read_field(ps, row->count + 1, &text);
        if (st != DF_OK) {
            return st;
        }
        st = row_add_field(row, text);
        if (st != DF_OK) {
            free(text);
            return st;
        }
        if (peek_char(src, 0) == (unsigned char)ps->delimiter) {
            next_char(src);     /* another field follows */
        } else {
            break;              /* end of line or end of text */
        }
    }

    /* skip the line ending: "\r\n", "\n" or "\r" */
    if (peek_char(src, 0) == '\r') {
        next_char(src);
    }
    if (peek_char(src, 0) == '\n') {
        next_char(src);
    }
    return DF_OK;
}

/* Reads rows into `list` until the text ends or, if max_rows > 0, until
   max_rows rows were added. Every row must have the same number of fields
   as the first row of the whole text. */
static df_status parse_rows(Parser *ps, RowList *list, size_t max_rows)
{
    Source *src = ps->src;
    size_t added = 0;

    while (max_rows == 0 || added < max_rows) {
        /* Skip blank lines (empty, or only spaces). The spaces at the start
           of a real line are skipped too, read_field would skip them
           anyway. */
        while (is_blank_char(peek_char(src, 0), ps->delimiter)) {
            next_char(src);
        }
        int c = peek_char(src, 0);
        if (c == END_OF_INPUT) {
            break;                  /* end of the text */
        }
        if (c == '\n' || c == '\r') {
            next_char(src);
            if (c == '\r' && peek_char(src, 0) == '\n') {
                next_char(src);
            }
            continue;
        }

        size_t line = src->line;
        Row row = {NULL, 0, 0};
        df_status st = read_row(ps, &row);
        if (st == DF_OK && ps->nfields == 0) {
            ps->nfields = row.count;    /* the first row sets the width */
            ps->first_line = line;
        } else if (st == DF_OK && row.count != ps->nfields) {
            st = DF_ERR_PARSE;          /* wrong number of fields */
            report(ps->err, line, 0, "expected %zu fields but found %zu",
                   ps->nfields, row.count);
        }
        if (st == DF_OK) {
            st = list_add_row(list, row);
        }
        if (st != DF_OK) {
            row_free(&row);
            return st;
        }
        added++;
    }

    /* Problems with the file itself only show once its end is reached. */
    if (src->read_error) {
        report(ps->err, 0, 0, "the file could not be read");
        return DF_ERR_IO;
    }
    if (src->nul_byte && peek_char(src, 0) == END_OF_INPUT) {
        report(ps->err, src->line, 0, "the file contains a '\\0' byte");
        return DF_ERR_PARSE;
    }
    return DF_OK;
}

/* Skips the UTF-8 "byte order mark" that Excel puts at the start. */
static void skip_byte_order_mark(Source *src)
{
    if (peek_char(src, 0) == 0xEF && peek_char(src, 1) == 0xBB &&
        peek_char(src, 2) == 0xBF) {
        next_char(src);
        next_char(src);
        next_char(src);
    }
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

/* The words pandas reads as booleans. Returns 1 for a true word, 0 for a
   false word and -1 if the text is not one of them. */
static int bool_value(const char *text)
{
    if (strcmp(text, "True") == 0 || strcmp(text, "true") == 0 ||
        strcmp(text, "TRUE") == 0) {
        return 1;
    }
    if (strcmp(text, "False") == 0 || strcmp(text, "false") == 0 ||
        strcmp(text, "FALSE") == 0) {
        return 0;
    }
    return -1;
}

static int is_date_text(const char *text)
{
    int64_t days = 0;
    return df_date_parse(text, &days) == DF_OK;
}

/* Looks at every non-missing cell of column `col` to choose its type: the
   first of INT, FLOAT, BOOL, DATE that fits every value, else STRING. */
static df_dtype infer_type(const RowList *list, size_t first_row, size_t col,
                           const csv_options *opts)
{
    int any_value = 0;
    int all_int = 1;
    int all_number = 1;
    int all_bool = 1;
    int all_date = 1;

    for (size_t r = first_row; r < list->count; r++) {
        const char *text = list->rows[r].fields[col];
        if (is_missing_text(text, opts)) {
            continue;
        }
        any_value = 1;
        if (all_int && !is_int_text(text)) {
            all_int = 0;
        }
        if (all_number && !is_number_text(text)) {
            all_number = 0;
        }
        if (all_bool && bool_value(text) < 0) {
            all_bool = 0;
        }
        if (all_date && !is_date_text(text)) {
            all_date = 0;
        }
        if (!all_number && !all_bool && !all_date) {
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
    if (all_bool) {
        return DTYPE_BOOL;
    }
    if (all_date) {
        return DTYPE_DATE;
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
        } else if (type == DTYPE_BOOL) {
            st = series_append_bool(s, bool_value(text));
        } else if (type == DTYPE_DATE) {
            int64_t days = 0;
            df_date_parse(text, &days);     /* checked by infer_type */
            st = series_append_date(s, days);
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

/* Turns the parsed rows list->rows[first_row ..] into a DataFrame with
   ncols columns. The names come from `header` (NULL: "col0", "col1", ...),
   which was read from line header_line. */
static df_status build_frame(const RowList *list, size_t first_row,
                             const Row *header, size_t header_line,
                             size_t ncols, const csv_options *opts,
                             DataFrame **out)
{
    DataFrame *df = NULL;
    df_status st = df_create(&df);
    if (st != DF_OK) {
        return st;
    }

    for (size_t c = 0; c < ncols && st == DF_OK; c++) {
        /* The column name: from the header, or "col<N>". */
        char made_up_name[32];
        const char *name = made_up_name;
        snprintf(made_up_name, sizeof made_up_name, "col%zu", c);
        if (header != NULL && header->fields[c][0] != '\0') {
            name = header->fields[c];
        }

        Series *s = NULL;
        st = build_column(list, first_row, c, name, opts, &s);
        if (st == DF_OK) {
            st = df_add_column(df, s);  /* e.g. DUPLICATE_NAME */
            if (st != DF_OK) {
                series_free(s);
            }
            if (st == DF_ERR_DUPLICATE_NAME) {
                report(opts->error, header_line, c + 1,
                       "the column name \"%.60s\" is used twice", name);
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

/* Reads everything from src into one frame. */
static df_status read_all(Source *src, const csv_options *opts,
                          DataFrame **out)
{
    Parser ps = {src, opts->delimiter, 0, 0, opts->error};
    RowList list = {NULL, 0, 0};

    skip_byte_order_mark(src);
    df_status st = parse_rows(&ps, &list, 0);
    if (st == DF_OK) {
        const Row *header = NULL;
        size_t first_row = 0;
        if (opts->has_header && list.count > 0) {
            header = &list.rows[0];     /* row 0 holds the column names */
            first_row = 1;
        }
        /* Empty input gives ncols = 0: a frame with no columns. */
        st = build_frame(&list, first_row, header, ps.first_line, ps.nfields,
                         opts, out);
    }
    list_free(&list);   /* the frame has its own copies of everything */
    return st;
}

/* ---- public reading functions ------------------------------------------- */

/* Shared start of every reading function: picks the options to use and
   checks them. *use points at opts or at `defaults`. */
static df_status start_reading(const csv_options *opts, csv_options *defaults,
                               const csv_options **use)
{
    *defaults = csv_default_options();
    *use = opts;
    if (opts == NULL) {
        *use = defaults;
    }
    df_status st = check_options(*use);
    if (st != DF_OK) {
        report((*use)->error, 0, 0, "invalid options: %s", df_strerror(st));
    }
    return st;
}

/* Puts a general message into the report if nothing more exact is there. */
static df_status finish_reading(const csv_options *opts, df_status st)
{
    if (st != DF_OK) {
        report(opts->error, 0, 0, "%s", df_strerror(st));
    }
    return st;
}

df_status df_read_csv_string(const char *text, const csv_options *opts,
                             DataFrame **out)
{
    if (opts != NULL) {
        clear_report(opts->error);
    }
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (text == NULL) {
        return DF_ERR_NULL_ARG;
    }

    csv_options defaults;
    df_status st = start_reading(opts, &defaults, &opts);
    if (st != DF_OK) {
        return st;
    }

    Source src;
    source_from_text(&src, text);
    st = read_all(&src, opts, out);
    return finish_reading(opts, st);
}

df_status df_read_csv(const char *path, const csv_options *opts,
                      DataFrame **out)
{
    if (opts != NULL) {
        clear_report(opts->error);
    }
    if (out == NULL) {
        return DF_ERR_NULL_ARG;
    }
    *out = NULL;
    if (path == NULL) {
        return DF_ERR_NULL_ARG;
    }

    csv_options defaults;
    df_status st = start_reading(opts, &defaults, &opts);
    if (st != DF_OK) {
        return st;
    }

    /* "rb" so Windows does not change the line endings; read_row() handles
       "\r\n" itself. */
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        report(opts->error, 0, 0, "cannot open \"%.80s\"", path);
        return DF_ERR_IO;
    }

    Source *src = malloc(sizeof(Source));   /* too big for the stack */
    if (src == NULL) {
        fclose(f);
        return finish_reading(opts, DF_ERR_NOMEM);
    }
    source_from_file(src, f);
    st = read_all(src, opts, out);
    free(src);
    fclose(f);
    return finish_reading(opts, st);
}

df_status df_read_csv_chunks(const char *path, const csv_options *opts,
                             size_t chunk_rows, csv_chunk_handler handle,
                             void *user_data)
{
    if (opts != NULL) {
        clear_report(opts->error);
    }
    if (path == NULL || handle == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (chunk_rows == 0) {
        return DF_ERR_INVALID_ARG;
    }

    csv_options defaults;
    df_status st = start_reading(opts, &defaults, &opts);
    if (st != DF_OK) {
        return st;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        report(opts->error, 0, 0, "cannot open \"%.80s\"", path);
        return DF_ERR_IO;
    }
    Source *src = malloc(sizeof(Source));
    if (src == NULL) {
        fclose(f);
        return finish_reading(opts, DF_ERR_NOMEM);
    }
    source_from_file(src, f);
    skip_byte_order_mark(src);
    Parser ps = {src, opts->delimiter, 0, 0, opts->error};

    /* The header is read once and used for every chunk. */
    RowList header = {NULL, 0, 0};
    if (opts->has_header) {
        st = parse_rows(&ps, &header, 1);
    }

    size_t rows_done = 0;
    while (st == DF_OK) {
        RowList chunk = {NULL, 0, 0};
        st = parse_rows(&ps, &chunk, chunk_rows);
        if (st == DF_OK && chunk.count == 0) {
            list_free(&chunk);
            break;                          /* no rows left */
        }

        DataFrame *frame = NULL;
        if (st == DF_OK) {
            const Row *names = NULL;
            if (header.count > 0) {
                names = &header.rows[0];
            }
            st = build_frame(&chunk, 0, names, ps.first_line, ps.nfields,
                             opts, &frame);
        }
        list_free(&chunk);      /* the frame has its own copies */

        if (st == DF_OK) {
            st = handle(frame, rows_done, user_data);
            rows_done += df_nrows(frame);
            df_free(frame);
            if (st != DF_OK) {
                report(opts->error, 0, 0,
                       "stopped by the chunk handler: %s", df_strerror(st));
            }
        }
    }

    list_free(&header);
    free(src);
    fclose(f);
    return finish_reading(opts, st);
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
    /* A bigger buffer means fewer (slow) writes to the disk. If setvbuf
       fails, the normal buffer is still there, so the result is ignored. */
    setvbuf(f, NULL, _IOFBF, WRITE_BUFFER);

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
            } else if (series_dtype(col) == DTYPE_BOOL) {
                int v = 0;
                series_get_bool(col, r, &v);
                fputs(v ? "True" : "False", f);
            } else if (series_dtype(col) == DTYPE_DATE) {
                int64_t v = 0;
                series_get_date(col, r, &v);
                df_date_format(v, number, sizeof number);
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
