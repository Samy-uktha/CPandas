/*
 * csv.h - read a DataFrame from CSV and write one back out.
 *
 * READING
 *   - First row is the header (column names) unless opts.has_header == 0, in
 *     which case columns are named "col0", "col1", ...
 *   - Quoted fields are supported: "a,b" is one field and "" inside quotes is
 *     a literal quote. Quoted fields may contain newlines.
 *   - An empty field, or any field equal to one of opts.na_values, becomes a
 *     missing cell. (A quoted empty string "" is still treated as missing.)
 *   - Each column's type is inferred from its non-missing cells: INT if all
 *     parse as integers, else FLOAT if all parse as numbers, else STRING.
 *     A column that is entirely missing becomes STRING.
 *   - Rows with the wrong number of fields fail with DF_ERR_PARSE (nothing is
 *     partially returned).
 *   - Spaces and tabs around an UNQUOTED field are removed, so "1, 42" reads
 *     as 1 and 42. A quoted field keeps its spaces exactly: " x " stays " x ".
 *   - INT means an optional sign and digits only ("-12", "007") that fit in
 *     int64_t. FLOAT also allows a decimal point and an exponent ("3.5",
 *     ".5", "1e3"). Words like "nan", "inf" or hex numbers are text.
 *   - Lines may end with "\n", "\r\n" or "\r". Blank lines (only spaces) are
 *     skipped. A UTF-8 byte order mark at the very start (Excel adds one) is
 *     skipped.
 *   - A header cell that is empty is named "col<N>" (N = column position).
 *     Two columns with the same name fail with DF_ERR_DUPLICATE_NAME.
 *   - An unclosed quote, or text after a closing quote ("ab"c), fails with
 *     DF_ERR_PARSE. A file containing a '\0' byte fails with DF_ERR_PARSE.
 *   - Empty input (or only blank lines) gives a frame with 0 columns. Only a
 *     header gives the columns with 0 rows (all DTYPE_STRING).
 *   - An invalid delimiter ('\0', '"', '\n' or '\r') fails with
 *     DF_ERR_INVALID_ARG. A file that cannot be opened or read fails with
 *     DF_ERR_IO.
 *   - The caller owns *out and frees it with df_free(). On any failure *out
 *     is NULL and no memory is leaked.
 *
 * WRITING
 *   - Fields containing the delimiter, a quote, a newline, or starting or
 *     ending with a space/tab are quoted, with embedded quotes doubled.
 *     Missing cells are written as empty fields (as "" when the frame has a
 *     single column, so the line is not mistaken for a blank line). An
 *     empty string is written as "", so it is read back as missing.
 *   - Floats are written with enough precision to round-trip, and always
 *     with a '.' or exponent ("72.0", not "72") so they are read back as
 *     FLOAT. Lines end with "\n".
 *   - opts.has_header == 0 skips the header line.
 *   - Fails with DF_ERR_IO if the file cannot be opened or written. The
 *     frame is only read, never changed.
 *
 * Owner: Person A
 */
#ifndef DF_CSV_H
#define DF_CSV_H

#include <stddef.h>
#include "df_error.h"
#include "dataframe.h"

typedef struct {
    char               delimiter;   /* default ','                           */
    int                has_header;  /* default 1                             */
    const char *const *na_values;   /* extra tokens meaning "missing", e.g.
                                       {"NA", "N/A", "null"}; may be NULL    */
    size_t             n_na_values;
} csv_options;

/* Returns the defaults described above. Pass NULL instead of options anywhere
 * below to use them directly. */
csv_options csv_default_options(void);

df_status df_read_csv(const char *path, const csv_options *opts,
                      DataFrame **out);

/* Same as df_read_csv but parses an in-memory, NUL-terminated string.
 * Handy for unit tests (no temp files needed). */
df_status df_read_csv_string(const char *text, const csv_options *opts,
                             DataFrame **out);

df_status df_write_csv(const DataFrame *df, const char *path,
                       const csv_options *opts);

#endif /* DF_CSV_H */