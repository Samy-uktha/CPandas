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
 *   - The caller owns *out and frees it with df_free(). On any failure *out
 *     is NULL and no memory is leaked.
 *
 * WRITING
 *   - Fields containing the delimiter, a quote or a newline are quoted, with
 *     embedded quotes doubled. Missing cells are written as empty fields.
 *     Floats are written with enough precision to round-trip.
 *
 * Owner: Person A
 */
#ifndef DF_CSV_H
#define DF_CSV_H

#include <stddef.h>
#include "error.h"
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