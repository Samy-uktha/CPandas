# CPandas – Part A: the data layer

This document covers how data is stored and how it gets in and out of the
library: the Series and DataFrame types, the CSV reader and writer, and the
error-code mechanism. What you can do with a DataFrame once it exists
(filter, sort, group-by, join, …) is Part B, documented in
[part_b_query_layer.md](part_b_query_layer.md).

| Module | Header | Source | Tests |
|---|---|---|---|
| Error codes | `df_error.h` | `df_error.c` | (in `test_series.c`) |
| Series | `series.h` | `series.c` | `test_series.c` |
| DataFrame | `dataframe.h` | `dataframe.c` | `test_dataframe.c` |
| CSV | `csv.h` | `csv.c` | `test_csv.c` |

---

## 1. Design

### 1.1 Opaque types

`Series` and `DataFrame` are declared in the headers only as
`typedef struct Series Series;`. Their fields are visible only inside
`series.c` and `dataframe.c`, so other code (Part B, the demo, users) can
only use the functions. This means the storage could change later (for
example, a bitmap instead of one byte per missing flag) without changing
any code that uses the library.

### 1.2 Error reporting

Every function that can fail returns a `df_status` (from `df_error.h`). The
real result comes back through an out-parameter:

```c
DataFrame *df = NULL;
df_status st = df_read_csv("data.csv", NULL, &df);
if (st != DF_OK) {
    printf("error: %s\n", df_strerror(st));
}
```

| Code | Meaning |
|---|---|
| `DF_OK` | success |
| `DF_ERR_NULL_ARG` | a required pointer argument was `NULL` |
| `DF_ERR_INVALID_ARG` | a value that is not allowed (empty name, bad delimiter, …) |
| `DF_ERR_NOMEM` | `malloc` / `realloc` failed |
| `DF_ERR_TYPE_MISMATCH` | wrong column type (e.g. appending text to an INT Series) |
| `DF_ERR_INDEX_OUT_OF_RANGE` | row or column number too big |
| `DF_ERR_NOT_FOUND` | no column with that name |
| `DF_ERR_DUPLICATE_NAME` | a column with that name already exists |
| `DF_ERR_LENGTH_MISMATCH` | a column's length differs from the frame's row count |
| `DF_ERR_MISSING_VALUE` | tried to read a missing cell (or no values to compute from) |
| `DF_ERR_IO` | a file could not be opened, read or written |
| `DF_ERR_PARSE` | malformed CSV |

The rules:
- On failure, a pointer out-parameter is set to `NULL`.
- **Every input object is left exactly as it was.**
- The library never prints and never exits.

`df_strerror()` returns a fixed string for any code and never returns
`NULL`.

### 1.3 Invariants the library keeps

- **Series:** `len <= cap`. For a STRING Series, a cell's pointer is `NULL`
  exactly when the cell is missing.
- **DataFrame:** every column has `df_nrows()` cells, column names are
  unique and non-empty, and a frame with 0 columns has 0 rows. The first
  column added decides the row count.

---

## 2. Data structures

### 2.1 Series (`series.c`)

```c
struct Series {
    char          *name;        /* owned copy                          */
    df_dtype       dtype;       /* DTYPE_INT, DTYPE_FLOAT, DTYPE_STRING */
    size_t         len, cap;
    union { int64_t *i; double *f; char **s; } data;   /* one typed array */
    unsigned char *is_missing;  /* parallel array: 1 = missing          */
};
```

- **One typed array per column**, not an array of "any value" cells. All
  cells of a column have the same type, so each value takes exactly
  8 bytes (or one pointer), and numbers are stored as real numbers.
- **Missing values** are a separate `is_missing` array. Any value can then
  be missing, including the integer 0. A missing numeric cell holds 0 in
  `data`, which is never read.
- **Strings** are separate `malloc`'d copies owned by the Series.

### 2.2 DataFrame (`dataframe.c`)

```c
struct DataFrame {
    Series **columns;   /* owned by the frame */
    size_t   ncols, cap;
    size_t   nrows;
};
```

The DataFrame is a growing array of column pointers. Columns are stored
one after another and looked up by name with a linear search, which is
fine for the usual handful of columns.

### 2.3 CSV parser (`csv.c`, internal)

| Struct | Purpose |
|---|---|
| `TextBuffer` | a string that grows one character at a time (the current field) |
| `Row` | the fields of one line, as strings |
| `RowList` | all the lines of the file |

---

## 3. Algorithms

### 3.1 Growing arrays

Series data, the DataFrame's column array and all the parser buffers grow
by **doubling** their capacity when full. That makes appending n items
O(n) in total (amortized O(1) per append).

Every `realloc` result goes into a temporary pointer first, so a failed
`realloc` never loses the old array. When a Series has to grow two arrays
and only the first `realloc` succeeds, the capacity is left unchanged and
the Series stays valid.

### 3.2 `series_take` – the shared copying step

`series_take(s, indices, n)` makes a new Series from the listed rows
(repeats allowed). The special index `SERIES_TAKE_MISSING` produces a
missing cell. Every index is checked before anything is allocated.
Slicing, filtering, sorting, `df_drop_missing_rows`, group-by and joins
all use this function to copy data.

### 3.3 Reading CSV: parse first, then build

1. **Parse** the whole text into a `RowList` of raw strings. This is a
   character-by-character scan:
   - **Quoted field:** read until the closing quote; `""` means one `"`.
     Delimiters and newlines inside the quotes are part of the field. An
     unclosed quote, or text after the closing quote, is `DF_ERR_PARSE`.
   - **Unquoted field:** read until the delimiter or the end of the line.
     Spaces and tabs at both ends are removed.
   - **Line endings:** `\n`, `\r\n` and `\r` are all accepted. Blank
     lines are skipped, and a UTF-8 byte order mark at the start is
     skipped.
   - **Field count:** every line must have the same number of fields as
     the first one, otherwise `DF_ERR_PARSE`.
2. **Infer each column's type** from all of its non-missing cells. A cell
   is missing if it is empty or equals one of `opts.na_values`.
   - **INT** if every cell is an optional sign plus digits that fit in
     `int64_t`.
   - Otherwise **FLOAT** if every cell is a decimal number
     (`[sign] digits [. digits] [e [sign] digits]`).
   - Otherwise **STRING**. A column with no values is STRING too.

   The number formats are checked by hand before calling
   `strtoll`/`strtod`. That way text like `nan`, `inf`, `0x1F` or `" 5"`
   never becomes a number by accident.
3. **Build** one Series per column and add it to a new frame. The parsed
   rows are then freed.

Parsing everything first costs memory: the whole file is held twice for a
moment, once as text and once as the frame. In return, a column's type
can depend on every value in it. For example, a column `1, 2, 3.5`
becomes FLOAT, not INT. Reading takes O(size of the file).

### 3.4 Writing CSV

- **Quoting:** a field is quoted only when it has to be. That means it
  contains the delimiter, a quote or a newline, or starts or ends with a
  space (the reader would trim those). Quotes inside are doubled.
- **Floats** are written with `%.15g`. If that does not read back exactly,
  they are written with `%.17g`, which always round-trips. A `.0` is
  added to whole numbers, so `72.0` is read back as FLOAT, not INT.
- **Missing cells** become empty fields. A one-column frame writes `""`
  instead, because an empty line would be skipped as a blank line when
  reading back.

### 3.5 `df_print`

`df_print` makes two passes over the rows it shows. The first works out
each column's width; the second prints. Numbers are right-aligned and text
is left-aligned, with row numbers on the left. Text longer than 24
characters is cut and ends with `...`.

---

## 4. API reference

### 4.1 Series (`series.h`)

| Function | What it does |
|---|---|
| `series_create(name, dtype, initial_cap, &out)` | new empty Series (`name` copied, must be non-empty) |
| `series_free(s)` | frees it and everything it owns; `NULL` is fine |
| `series_clone(s, &out)` | deep copy |
| `series_take(s, indices, n, &out)` | deep copy of the listed rows (see 3.2) |
| `series_append_int/float/string(s, v)` | add a value at the end (strings are copied); wrong type → `TYPE_MISMATCH` |
| `series_append_missing(s)` | add a missing cell (any type) |
| `series_name / dtype / len / count_missing(s)` | inspection |
| `series_is_missing(s, i)` | 1 if missing, 0 if present or out of range |
| `series_get_int/float/string(s, i, &out)` | read a cell; errors: `TYPE_MISMATCH`, `INDEX_OUT_OF_RANGE`, `MISSING_VALUE` |
| `series_set_int/float/string/missing(s, i, v)` | overwrite a cell |
| `series_rename(s, name)` | rename (use `df_rename_column` if the Series is in a frame) |
| `series_fill_missing_int/float/string(s, v)` | replace every missing cell with `v` |

### 4.2 DataFrame (`dataframe.h`)

| Function | What it does | Errors |
|---|---|---|
| `df_create(&out)` | empty frame (0 × 0) | `NOMEM` |
| `df_free(df)` | frees the frame and its columns; `NULL` is fine | – |
| `df_clone(df, &out)` | deep copy | `NOMEM` |
| `df_make_sample(&out)` | 6 × 3 sample: id INT, name STRING, score FLOAT, one missing score | `NOMEM` |
| `df_nrows(df)`, `df_ncols(df)` | shape (0 for `NULL`) | – |
| `df_add_column(df, s)` | append a column; **takes ownership of `s` on success only** | `DUPLICATE_NAME`, `LENGTH_MISMATCH` |
| `df_remove_column(df, name)` / `df_remove_column_at(df, i)` | remove and free a column; later columns move left | `NOT_FOUND` / `INDEX_OUT_OF_RANGE` |
| `df_rename_column(df, old, new)` | rename (renaming to its own name is allowed) | `NOT_FOUND`, `DUPLICATE_NAME`, `INVALID_ARG` (empty name) |
| `df_find_column(df, name, &idx)` | position of a column | `NOT_FOUND` |
| `df_get_column(df, name, &s)` / `df_get_column_at(df, i, &s)` | borrow a column (read-only) | `NOT_FOUND` / `INDEX_OUT_OF_RANGE` |
| `df_column_name(df, i, &name)` | borrow a column's name | `INDEX_OUT_OF_RANGE` |
| `df_select_columns(df, names, n, &out)` | new frame with copies of these columns, in this order | `NOT_FOUND`, `DUPLICATE_NAME` |
| `df_drop_missing_rows(df, &out)` | new frame without the rows that have any missing cell | `NOMEM` |
| `df_print(df, stream, max_rows)` | pretty-print (`max_rows = 0` prints all); missing cells show as `NA` | `NULL_ARG` |

### 4.3 CSV (`csv.h`)

```c
csv_options csv_default_options(void);        /* ',' , header on, no NA words */
df_status df_read_csv(const char *path, const csv_options *opts, DataFrame **out);
df_status df_read_csv_string(const char *text, const csv_options *opts, DataFrame **out);
df_status df_write_csv(const DataFrame *df, const char *path, const csv_options *opts);
```

| `csv_options` field | Default | Meaning |
|---|---|---|
| `delimiter` | `','` | any character except `'\0'`, `'"'`, `'\n'`, `'\r'` |
| `has_header` | `1` | `0`: no header line; columns are named `col0`, `col1`, … |
| `na_values`, `n_na_values` | `NULL`, `0` | extra words that mean "missing", e.g. `{"NA", "null"}` |

Pass `NULL` for `opts` to use the defaults.

```c
const char *missing_words[] = {"NA", "N/A"};
csv_options opts = csv_default_options();
opts.delimiter = ';';
opts.na_values = missing_words;
opts.n_na_values = 2;
df_read_csv("data.csv", &opts, &df);
```

The full reading and writing rules (type inference, quoting, blank lines,
error cases) are listed at the top of `csv.h`.

---

## 5. Memory ownership rules

| Question | Answer |
|---|---|
| **Who allocates?** | The library: `series_create`, `series_clone`, `series_take`, `df_create`, `df_clone`, `df_make_sample`, `df_select_columns`, `df_drop_missing_rows` and `df_read_csv*` all allocate what they return. |
| **Who owns it?** | The caller owns every returned `Series *` / `DataFrame *`, and frees it with `series_free()` / `df_free()`. |
| **Handing a Series to a frame** | `df_add_column(df, s)` **takes ownership on `DF_OK` only**. After success, never free `s` yourself: `df_free` will. After a failure, you still own `s` and must free it. |
| **Strings** | The library always **copies** strings you pass in (names, cell values). You keep ownership of yours and may reuse the buffer straight away. |
| **Borrowed pointers** | `df_get_column*`, `df_column_name`, `series_name` and `series_get_string` return pointers *into* the object. They stay valid until that object is changed or freed. Never free them. Copy them (`series_clone`, `strcpy`) to keep them longer. |
| **Copies** | Every function that returns a new frame or Series makes a deep copy. Nothing is shared with the source. |
| **On failure** | The out-parameter is `NULL`, every temporary allocation is freed, and the inputs are unchanged. No partial frame is ever returned (e.g. a CSV with a bad line 1000 gives `DF_ERR_PARSE` and no frame). |
| **Files** | `df_read_csv` / `df_write_csv` open and close the file themselves. If writing fails half-way, the partly written file stays on disk. |

---

## 6. Limitations

- **The whole CSV file is loaded into memory** (text plus the parsed
  rows) before the frame is built. Very large files need about 2–3× their
  size in RAM.
- **Type inference is all-or-nothing per column.** One non-number cell
  makes the whole column STRING. Text that looks like a number (such as
  postcode `"007"`) becomes INT and loses its leading zeros, even when it
  is quoted.
- **Empty strings and missing values are the same thing in a CSV.** An
  empty string in a STRING column is written as `""` and read back as
  missing. A cell whose text equals one of `na_values` is read as missing
  even if it was quoted.
- **No dates or booleans:** these columns are read as STRING.
- **No error line number:** a malformed CSV gives `DF_ERR_PARSE`, but not
  the line where the problem is.
- **Encoding:** text is read and written as raw bytes. UTF-8 works, but
  `df_print` counts bytes, not characters, so non-ASCII text can make the
  columns line up slightly wrong.
- **`df_print`** cuts cells at 24 characters, and text containing a newline
  breaks the table layout.
- **Column lookup by name is a linear search,** O(number of columns).
- **Out-of-memory paths** are handled in the code but not tested directly
  (there is no malloc-failure injection).

---

## 7. Test cases

| File | Checks | What it covers |
|---|---|---|
| `test_series.c` | 88 | create/invalid args, NULL args, type mismatch, growth to 100,000 values, missing values and fill, string copying, 5,000 strings (leaks), clone, take with `SERIES_TAKE_MISSING`, rename/set, `df_strerror` |
| `test_dataframe.c` | 2191 | create/free, add column (length mismatch, duplicate, ownership on failure, zero-length columns), lookups and their errors, remove (order kept, row count reset), rename (own name, clash, empty), deep clone, select (order, unknown, duplicate, empty), drop missing, sample frame, `df_print` output and cutting, 1,000 columns, 100,000 rows |
| `test_csv.c` | 147 | types and missing cells, type inference (big ints, `007`, `1e3`, `nan`, hex, all-missing, int64 limits, `1e999`), space trimming vs quoted spaces, quotes (`""`, delimiters and newlines inside), `na_values`, `\r\n` / `\r` / no final newline / blank lines / BOM, no header, `;` and tab delimiters, parse errors, bad options, missing file, empty input / header only / empty header cell, write→read round trips (tricky text, int64 max, floats exact, whole-number floats stay FLOAT, no header, other delimiter, one column with missing cells), write errors, a file containing `\0`, 100,000 lines read + written + read back |

Every test runs under AddressSanitizer and UBSan (`make test`), so a leak
or invalid memory access fails the run. To check that the tests can catch
mistakes, bugs were planted on purpose in `dataframe.c` and `csv.c`
(no trimming, lost one-column rows, broken `""` handling, no field-count
check, row count not reset, no length check, floats written without `.0`,
and a leak). Each one made a test fail.
