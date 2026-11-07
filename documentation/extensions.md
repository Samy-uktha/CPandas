# CPandas – Extensions

The interim presentation listed nine ways to take CPandas further. This
document describes how each one was built. It sits next to
[part_a_data_layer.md](part_a_data_layer.md) and
[part_b_query_layer.md](part_b_query_layer.md), which describe the first
version. Where this document and those two disagree (for example on how
group-by and join work inside), this one is up to date.

| Planned | Done in | Section |
|---|---|---|
| Hash tables for faster group-by and joins | `src/df_keyindex.c`, `df_groupby.c`, `df_join.c` | [4](#4-hash-tables-for-group-by-join-and-nunique) |
| Read big CSV files in pieces, not all at once | `csv.c` (`df_read_csv_chunks`) | [3](#3-csv-reading-in-pieces-and-error-line-numbers) |
| Store repeated text values only once | `series.c` (string pool) | [2](#2-storing-repeated-text-once) |
| Benchmark against pandas on large datasets | `bench/` | [7](#7-benchmark-against-pandas) |
| Group-by and join on several key columns | `df_groupby_multi`, `df_merge_on` | [4](#4-hash-tables-for-group-by-join-and-nunique) |
| Date and boolean column types | `series.c`, `df_date.c`, `csv.c` | [1](#1-date-and-boolean-columns) |
| Std dev, unique counts and a describe() summary | `df_stats.c` | [5](#5-more-statistics-and-describe) |
| Stack tables and add computed columns | `df_concat.c`, `df_compute.c` | [6](#6-stacking-tables-and-computed-columns) |
| Line numbers in CSV error messages | `csv.c` (`csv_error`) | [3](#3-csv-reading-in-pieces-and-error-line-numbers) |

Everything that worked before still works the same way. The first 3,127
test checks pass unchanged, and the slide demo gives the same output. See
[section 9](#9-what-changed-for-existing-code) for the few places where
behaviour changed on purpose.

---

## 1. Date and boolean columns

### Storage

`df_dtype` has two new values, added after the old ones so the old numbers
stay the same:

| Type | Stored as | Missing cell |
|---|---|---|
| `DTYPE_BOOL` | one `unsigned char` per cell, 0 or 1 | flag in `is_missing` (as always) |
| `DTYPE_DATE` | `int64_t` days since 1970-01-01, in the same array the INT type uses | same |

A date is just a number of days, so comparing, sorting, hashing and
subtracting dates is integer arithmetic. 1970-01-01 is 0, 1970-01-02 is 1
and 1969-12-31 is -1.

New Series functions follow the existing pattern:
`series_append_bool / _date`, `series_get_bool / _date`,
`series_set_bool / _date`, `series_fill_missing_bool / _date`. There is also
`series_append_from(dst, src, i)`, which copies one cell of any type (used
by `df_concat`).

### Converting dates (`df_date.h`)

| Function | Does |
|---|---|
| `df_date_from_ymd(y, m, d, &days)` | calendar date to day number; rejects 2023-02-29, month 13, year 0 … |
| `df_date_to_ymd(days, &y, &m, &d)` | day number to calendar date |
| `df_date_parse("2024-02-29", &days)` | exactly `YYYY-MM-DD`, and the day must exist |
| `df_date_format(days, buf, size)` | day number to `"2024-02-29"` |

Both conversions use the "days from civil" method. The year is shifted so
that it starts on 1 March, which makes 29 February the last day of the
year. Then the 400-year cycles of the Gregorian calendar are counted
(each one is exactly 146,097 days), and the rest is arithmetic. There are
no loops over years or months. The tests convert every day from year 1 to
year 9999 there and back.

### CSV, printing, sorting, filtering, statistics

- **CSV reading** tries the types in the order INT → FLOAT → BOOL → DATE →
  STRING and picks the first one that fits every non-missing cell.
  - BOOL words are `True`, `true`, `TRUE`, `False`, `false` and `FALSE`
    (the same set as pandas).
  - DATE means exactly `YYYY-MM-DD` and a real day.
  - `1`/`0` stay INT, `2024/02/29` stays STRING, and `2023-02-29` stays
    STRING.
- **CSV writing** writes `True`/`False` and `YYYY-MM-DD`, so a write
  followed by a read gives back the same frame.
- **`df_print`** shows `True`, `False` and `2024-03-15`.
- **Sorting:** dates go from earliest to latest, and False comes before
  True. Missing cells still go last.
- **Filtering:** the new `df_filter_date(df, col, op, days, &out)` compares
  dates. `df_filter_number` also accepts a BOOL column, with False = 0 and
  True = 1.
- **Statistics:** a BOOL column counts as numbers 0 and 1, so the mean of
  `remote` is the share of remote workers, as in pandas. A DATE column only
  supports count and nunique.
- **Group-by and join** keys can be BOOL or DATE.

---

## 2. Storing repeated text once

A column like `dept` with a million cells but only ten departments used to
hold a million separate `malloc`'d strings. Now every STRING Series has a
**string pool**: each different text is stored once, together with a count
of how many cells use it.

```c
typedef struct {
    size_t   refs;     /* how many cells point at this text */
    uint64_t hash;
    char     text[];   /* the text itself (flexible array member) */
} PoolEntry;
```

- The cell array still holds `char *`, but each pointer now points at the
  `text` of a pool entry. Equal cells share one entry.
- `series_append_string` and `series_set_string` look the text up in the
  pool. The pool is a hash table: FNV-1a hashing, open addressing with
  linear probing, kept at most ¾ full.
  - If the text is already there, its `refs` goes up.
  - If not, a new entry is made.
- When a cell lets go of a text (it is set to something else, set to
  missing, or the Series is freed), `refs` goes down. The last cell frees
  the entry.
- **Deleting from the table** uses *backward-shift deletion*. With linear
  probing, simply emptying a slot would cut a run of entries in two, and
  lookups would stop too early at the gap. So the entries after the gap
  that were displaced from an earlier home slot are moved back into it.
  A random test does 20,000 sets and deletes and checks every cell against
  a plain array.
- The pool never moves an entry, even when its table grows, so the
  pointers that `series_get_string` hands out stay valid exactly as long
  as before.

**The public API is unchanged.** Every function behaves as if each cell had
its own copy. `series_distinct_strings(s)` tells how many texts are really
stored. The benchmark's `dept` column has 1,000,000 cells and 10 stored
texts.

The pool has one more benefit. When two cells of the same Series are equal,
they hold the *same pointer*, so key comparisons in the hash tables
(section 4) usually finish with one pointer comparison instead of
`strcmp`.

---

## 3. CSV: reading in pieces, and error line numbers

### The reader no longer loads the whole file

The parser used to read the whole file into one string, then walk through
it with a `const char *`. Now it reads its characters from a **Source**:
either a string in memory, or a file read 64 KB at a time.

- The parser only ever needs to look 3 characters ahead: one for `""`
  inside quotes, three for the UTF-8 byte order mark. So the Source keeps a
  64 KB buffer and refills it as the parser moves on.
- The Source also counts lines. `\n`, `\r\n` and a lone `\r` each end
  exactly one line, including the line breaks inside quoted fields.
- A `'\0'` byte in a file is found when the buffer is filled. The file is
  treated as ending there, and the read fails with `DF_ERR_PARSE` on that
  line.

`df_read_csv` therefore no longer holds the file's text, only the parsed
fields. To read a file that is bigger than the memory, use chunks.

### `df_read_csv_chunks` (like pandas' `chunksize`)

```c
static df_status add_salaries(const DataFrame *chunk, size_t first_row,
                              void *user_data)
{
    double *total = user_data, sum = 0;
    const Series *salary = NULL;
    if (df_get_column(chunk, "salary", &salary) == DF_OK &&
        series_sum(salary, &sum) == DF_OK) {
        *total += sum;
    }
    return DF_OK;                       /* anything else stops reading */
}

double total = 0;
df_read_csv_chunks("big.csv", NULL, 100000, add_salaries, &total);
```

- The header is read once. Then 100,000 rows at a time become a DataFrame,
  which is passed to the function and freed as soon as it returns. So only
  one chunk is ever in memory.
- `first_row` tells where the chunk starts (0, 100000, …).
- Each chunk works out its own column types, as with pandas' `chunksize`.
- A broken row part-way returns the error. The chunks before it were
  already handled.

### Where did it go wrong? (`csv_error`)

```c
csv_error where;
csv_options opts = csv_default_options();
opts.error = &where;
if (df_read_csv("broken.csv", &opts, &df) != DF_OK) {
    printf("line %zu: %s\n", where.line, where.message);
}
```

| Problem | Example report |
|---|---|
| wrong number of fields | `line 4: expected 3 fields but found 2` |
| quote never closed | `line 3, field 2: the quote that opens field 2 is never closed` |
| text after a closing quote | `line 2, field 2: unexpected text after the closing quote of field 2` |
| same column name twice | `line 3, field 3: the column name "x" is used twice` |
| `'\0'` byte in the file | `line 3: the file contains a '\0' byte` |
| cannot open | `cannot open "nope.csv"` (line 0 = not about a line) |

Line numbers count every line of the file from 1, including the header and
blank lines, so they match what a text editor shows. The report is cleared
when a read starts, so after a successful read `where.message` is `""`.
`opts.error` is a new last field in `csv_options`, and
`csv_default_options()` sets it to `NULL`.

---

## 4. Hash tables for group-by, join and nunique

### The shared index (`src/df_keyindex.c`, internal)

`key_index_build(&index, keys, nkeys, nrows)` puts every row into a group,
so that rows with equal values in all the key columns share a group.

1. Each row's key cells are hashed together into one 64-bit number.
2. The hash table's slots hold group numbers. A slot matches when the group
   has the same hash **and** its first row really has equal keys. Different
   keys can share a hash, so the keys are always compared too.
3. A row with no match starts a new group.
4. A counting pass then lists each group's rows together (`members`),
   keeping row order inside each group.

The table has at least twice as many slots as rows, so it never fills up.
Rows with a missing key cell are put in no group. Building the index is
O(n) on average. `key_index_find(&index, other_keys, row)` looks up a row
of *another* frame in O(1) on average.

Hashing and equality agree on every type: `-0.0` and `0.0` hash the same,
and NaN is grouped with NaN.

### Group-by

`df_groupby_multi(df, keys, nkeys, aggs, naggs, &out)` groups by several
columns together. `df_groupby` is now `df_groupby_multi` with one key.

1. Build the index: O(n).
2. Take the first row of every group (the key values) and sort only those
   g rows: O(g log g).
3. For each aggregation and group, copy the group's cells with
   `series_take` and run `series_aggregate`, as before.

The first version sorted all n rows: O(n log n). The output is identical.
Groups are still sorted by key, rows with a missing key are still left
out, and the column names and types are the same.

### Join

`df_merge_on(left, right, on, non, how, &out)` joins on several columns,
and rows match when all their keys are equal. `df_merge` is `df_merge_on`
with one key.

- **INNER / LEFT / OUTER:** index the right keys, then for each left row
  (in order) look up its partners. They come out in right row order,
  because `members` keeps row order. For OUTER, the right rows of groups
  that were never used, and right rows with a missing key, are added at the
  end in right order.
- **RIGHT:** index the left keys and go through the right rows.

The cost is O(n + m + number of result rows), where the nested loops were
O(n · m). The pairs come out in **exactly the same order** as before. A
test compares 4 join types × 3 random tables (300 × 200 rows, with missing
keys and repeated keys) against the old nested-loop algorithm, pair by
pair.

On 20,000 × 20,000 rows the old method takes about 4.7 s. The hash join
takes about 1 ms.

### nunique

`series_nunique(s, &n)` builds an index with the column as its only key,
and `n` is the number of groups. It works for every type, in O(n).

---

## 5. More statistics and describe

| Function | What | Like pandas |
|---|---|---|
| `series_var`, `series_std` | sample variance / standard deviation (divide by count − 1); fewer than 2 values → `DF_ERR_MISSING_VALUE` | `var()`, `std()` (ddof=1) |
| `series_quantile(s, q, &out)` | linear interpolation between the two nearest sorted values | `quantile(q)` |
| `series_nunique(s, &n)` | different non-missing values, any type | `nunique()` |
| `DF_AGG_STD`, `DF_AGG_VAR`, `DF_AGG_NUNIQUE` | the same in group-by (nunique gives an INT column) | `agg("std")` … |
| `df_describe(df, &out)` | `stat` column (count, mean, std, min, 25%, 50%, 75%, max) plus one FLOAT column per INT/FLOAT column | `describe()` |

The variance is computed in two passes: first the mean, then the squared
distances from it. That is more accurate than adding up x² in one pass.
`df_describe` sorts each column **once** and reads min, the three
quartiles and max from that one sorted array.

---

## 6. Stacking tables and computed columns

### `df_concat(frames, n, &out)` (like `pd.concat`)

- Columns are matched **by name**. The result has every column of every
  frame, in order of first appearance, and a frame that lacks a column gets
  missing cells there.
- Types:
  - the same type in every frame stays that type;
  - INT in one frame and FLOAT in another becomes FLOAT;
  - any other mix gives `DF_ERR_TYPE_MISMATCH`.
- Unlike pandas, an INT column that gets missing cells **stays INT**,
  because CPandas marks missing cells with a flag and needs no NaN.

### Computed columns (`df_compute.h`, like `df["tax"] = df["salary"] * 0.1`)

```c
df_add_scalar_column(df, "tax", "salary", DF_ARITH_MUL, 0.1);
df_add_arith_column(df, "total", "salary", DF_ARITH_ADD, "bonus");
df_add_computed_column(df, "score", my_formula, &settings);   /* any formula */
```

- Operands can be INT, FLOAT or BOOL columns.
- A missing operand gives a missing cell.
- `x / 0` gives a missing cell (a CSV file cannot hold `inf`).
- The result is INT when both operands are whole numbers and the operation
  is + − ×. Otherwise it is FLOAT.
- INT arithmetic checks for overflow **before** each operation, because in
  C the overflow itself would already be undefined behaviour. An overflow
  fails the whole call with the new status `DF_ERR_OVERFLOW`, and the frame
  is not changed.
- These functions add a column to `df` (like `df_add_column`). The column
  is built completely first, so a failure never leaves half a column
  behind.

---

## 7. Benchmark against pandas

```bash
make bench
```

```bash
make bench ROWS=100000
```

`bench/run.sh` does four things:

1. Generates the test files with `bench/make_data.py` (plain Python; the
   same ROWS always gives the same files).
2. Times CPandas with `build/bench`, built from the sources with `-O2` and
   **without** the sanitizers.
3. Times pandas with `bench/bench_pandas.py` on the same files.
4. Prints both side by side.

It looks for pandas in `build/venv`; set `PYTHON=/path/to/python` to use
another one. Without pandas it shows the CPandas numbers only.

The data has 1,000,000 employees with 9 columns: int, text, 10
departments, 8 cities, int, int with 5% missing, float with 5% missing,
date, bool. The second file has a bonus for 900,000 of the ids, shuffled.

Test setup:

- Ubuntu 26.04 on WSL2, on the team's laptop, gcc 15.2 `-O2`.
- pandas 3.1.1 and NumPy 2.5.4.
- Files on the Windows drive (`/mnt/d`).
- One run each, in seconds.

| Operation | CPandas | pandas | CPandas / pandas |
|---|---:|---:|---:|
| read_csv employees | 2.579 | 0.651 | 4.0× |
| read_csv bonus | 0.373 | 0.062 | 6.0× |
| filter salary > 50000 | 0.090 | 0.041 | 2.2× |
| sort dept, salary desc | 1.183 | 0.257 | 4.6× |
| groupby dept (mean, count) | **0.037** | 0.063 | **0.6×** |
| groupby dept, city | **0.065** | 0.127 | **0.5×** |
| groupby city, nunique name | **0.136** | 0.151 | **0.9×** |
| merge on id (inner, 1M × 900k) | 0.403 | 0.096 | 4.2× |
| describe | 0.354 | 0.117 | 3.0× |
| new column salary × 0.1 | 0.012 | 0.002 | 7.9× |
| concat with itself | 0.198 | 0.070 | 2.9× |
| write_csv | 4.869 | 1.944 | 2.5× |
| read in 100k-row chunks | 1.779 | 0.569 | 3.1× |
| join 20k × 20k, nested loops (first version) | 4.746 | – | – |
| join 20k × 20k, hash (now) | 0.001 | 0.002 | 0.6× |

What the numbers say:

- **Group-by is faster than pandas** at this size. The hash index finds
  the groups in one pass, and with interned strings most key comparisons
  are a single pointer comparison.
- **The hash join is about 4,700 times faster than the old nested loops**
  on 20k rows, and its time grows linearly. The old method would need
  hours for the 1M × 900k merge; the new one takes 0.4 s.
- **pandas is 2–8× faster at the rest.** It runs vectorised C loops over
  plain arrays, and its CSV parser is heavily tuned. CPandas goes through
  one function call per cell (`series_get_*`), which keeps the Series type
  opaque and safe but costs time. That is a deliberate trade-off of the
  library's design, not a bug.
- The CSV numbers include reading from and writing to the Windows drive
  through WSL, which is slower than a Linux disk for both libraries.

---

## 8. Demo, examples, tests

### Demo (`build/demo`)

The menu items 1–16 have the same numbers as before, so the presentation
keystrokes still work and give the same output. What is new:

| Menu | New |
|---|---|
| 1, 12, 18, start-up | a CSV error shows its line and field, e.g. `Error: parse error (line 4: expected 3 fields but found 2)` |
| 8 Filter | dates (`joined >= 2025-01-01`) and true/false columns |
| 10 Statistics | unique count, std, 25% and 75%; for a text column, how many texts are really stored |
| 11 Group by | several key columns (`dept remote`); std, var, nunique |
| 12 Join | several key columns |
| 17 | describe the number columns |
| 18 | stack another CSV under the table |
| 19 | add a computed column: `salary * 0.1` or `salary + bonus` |
| 20 | read a CSV in chunks and show the chunks |

New example files:

- `examples/data/new_hires.csv` has the same columns as employees.csv plus
  a date column and a true/false column. Stack it with menu 18.
- `examples/data/broken.csv` has a row with a missing field on line 4.
  Load it with menu 1.

### Tests

There are 12 test programs with about 7,200 checks, all clean under
AddressSanitizer and UBSan (`make test`).

| Test | New checks for |
|---|---|
| `test_types` (new) | date conversions (every day of years 1–9999), date parsing; BOOL/DATE Series; `series_append_from`; the string pool, including 20,000 random changes checked against a plain array; printing, sorting and filtering the new types |
| `test_csv` | BOOL/DATE inference and round trip; line numbers for every kind of error; chunks (sizes 1, 300, 5000; stopping early; header only; an error part-way; no header); a file bigger than the read buffer with quoted fields across buffer edges, compared with reading the same text from memory |
| `test_stats` | var, std, quantiles (checked against pandas' interpolation), nunique (text, -0.0), bools as numbers, describe |
| `test_groupby` | multi-key group-by; 5 random tables (2,000 rows, two keys, missing cells) against a slow scan-everything version |
| `test_join` | multi-key joins; 4 join types × 3 random tables against the old nested-loop algorithm, pair by pair and in order |
| `test_concat` (new) | matching by name, new columns, INT+FLOAT, type clash, empty frames, 60,000 rows |
| `test_compute` (new) | column and scalar arithmetic, result types, missing and ÷0, overflow in + − × both ways, custom formula |

---

## 9. What changed for existing code

- **New enum values** are added at the end: `DTYPE_BOOL`, `DTYPE_DATE`;
  `DF_AGG_STD`, `DF_AGG_VAR`, `DF_AGG_NUNIQUE`; `DF_ERR_OVERFLOW`. Old
  values keep their numbers.
- **`csv_options` has a new last field `error`.** Code that gets its
  options from `csv_default_options()` (as all the code here does) needs no
  change.
- **CSV columns whose values are all `True`/`False` words or all
  `YYYY-MM-DD` dates** are now BOOL / DATE instead of STRING. This is a
  deliberate change.
- **The join and group-by results are identical**: same rows, same order,
  same columns. Only the speed changed.
- **The Makefile:**
  - links with `-lm` (for `sqrt` in the standard deviation);
  - no longer passes header files to the linker on rebuilds (the
    `$(filter %.c %.o,$^)` change);
  - has a new `bench` target.

## 10. Limitations that remain

- **Per-cell function calls** make most operations 2–8× slower than pandas
  (section 7).
- **`df_read_csv` still keeps all parsed fields in memory** before building
  the frame. Use `df_read_csv_chunks` for files bigger than the memory.
- **Chunks infer their types separately.** One chunk can make a column INT
  and the next one FLOAT, exactly as with pandas' `chunksize`.
- **Dates:** days only (no times or time zones), years 1–9999, and only the
  `YYYY-MM-DD` form is recognised.
- **Computed columns** support + − × ÷ on numbers. There is no string
  concatenation and no date arithmetic.
- **`df_concat` does not combine BOOL with INT** (pandas would make an
  object column); it fails with `DF_ERR_TYPE_MISMATCH`.
