# CPandas

A small C library inspired by Python's **pandas**, for working with tables of
data. You can load a CSV file into a DataFrame, look at it, filter, sort,
compute statistics, group and aggregate, join two tables, stack tables, add
computed columns and save the result back to CSV. Columns can hold
integers, floating-point numbers, text, true/false values or dates, and any
cell can be missing.

Course project for DS3601 (C Programming), by Samyuktha K and Vedant Singh.

```c
#include "cpandas.h"

DataFrame *df = NULL, *it = NULL, *avg = NULL;
df_read_csv("examples/data/employees.csv", NULL, &df);

df_filter_string(df, "dept", DF_OP_EQ, "IT", &it);       /* df[df.dept == "IT"] */
df_print(it, stdout, 0);

df_aggregation aggs[] = { {"salary", DF_AGG_MEAN} };
df_groupby(df, "dept", aggs, 1, &avg);                    /* df.groupby("dept") */
df_write_csv(avg, "averages.csv", NULL);

df_free(avg); df_free(it); df_free(df);
```

(Every function returns a `df_status`. Real code should check it, as
[examples/basic.c](examples/basic.c) does.)

---

## Features

| Area | What you get | Header |
|---|---|---|
| Series | typed column (INT / FLOAT / STRING / BOOL / DATE) with missing values, growing storage; repeated text stored once | `series.h` |
| Dates | `YYYY-MM-DD` ↔ day numbers, checked calendar dates | `df_date.h` |
| DataFrame | named columns of equal length; add, remove, rename, select columns; drop rows with missing cells; pretty printing | `dataframe.h` |
| CSV | reader with type inference, quotes, custom delimiter and NA words; reading in chunks for big files; errors with line numbers; writer that round-trips | `csv.h` |
| Row selection | head, tail, slice, take rows by number | `df_rows.h` |
| Filtering | `== != < <= > >=` on numbers, text and dates, missing / present, your own condition | `df_filter.h` |
| Sorting | by one or more columns, each ascending or descending, stable | `df_sort.h` |
| Statistics | count, sum, mean, median, min, max, std, var, quantiles, nunique, `describe` | `df_stats.h` |
| Group-by | group by one or more key columns (hash table), several aggregations at once | `df_groupby.h` |
| Join | inner, left, right and outer merge on one or more key columns (hash join) | `df_join.h` |
| Stacking | put tables on top of each other, matching columns by name | `df_concat.h` |
| Computed columns | `a + b`, `salary * 0.1`, or your own formula as a new column | `df_compute.h` |
| Errors | every failure returns a status code; `df_strerror()` explains it | `df_error.h` |

`#include "cpandas.h"` includes all of them.

---

## Project structure

```
CPandas/
├── include/            public headers (the API)
│   ├── cpandas.h       includes everything
│   ├── df_error.h  series.h  dataframe.h  csv.h               data layer
│   ├── df_rows.h  df_filter.h  df_sort.h  df_stats.h  df_groupby.h  df_join.h
│   └── df_date.h  df_concat.h  df_compute.h                    extensions
├── src/                one .c file per header (the library has no main),
│                       plus internal helpers: df_hash.h, df_keyindex.c/.h
├── tests/              one test program per module + test_helpers.h
├── examples/
│   ├── demo.c          interactive menu demo
│   ├── basic.c         short step-by-step example
│   └── data/           employees.csv, departments.csv, new_hires.csv,
│                       broken.csv
├── bench/              benchmark against pandas (make bench)
├── documentation/
│   ├── part_a_data_layer.md     Series, DataFrame, CSV, errors
│   ├── part_b_query_layer.md    rows, filter, sort, stats, group-by, join
│   └── extensions.md            new types, hash tables, chunks, benchmark…
├── Makefile
└── README.md
```

---

## Building

You need `gcc` (or another C11 compiler) and `make`. Everything goes into
`build/`.

| Command | Builds / does |
|---|---|
| `make` | `build/libcpandas.a` (the library), `build/demo` and `build/basic` |
| `make test` | builds and runs all the tests |
| `make demo` | just the interactive demo |
| `make bench` | times CPandas against pandas on 1,000,000 rows (`make bench ROWS=100000` for fewer); see [extensions.md §7](documentation/extensions.md#7-benchmark-against-pandas) |
| `make clean` | deletes `build/` |

By default the Makefile turns on **AddressSanitizer and UBSan**, so memory
leaks and invalid memory accesses make the tests fail. This works on Linux,
WSL and macOS.

**Windows (MinGW gcc in Git Bash)** has no AddressSanitizer, so set
`CFLAGS` without it, as an environment variable in front of `make`:

```bash
CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -g" make test
```

(Not `make test CFLAGS=...`: that would also drop the Makefile's
`-Iinclude`.)

To use the library in your own program, first build a plain library
(without the sanitizers), then link it:

```bash
make clean
```

```bash
CFLAGS="-std=c11 -Wall -O2" make build/libcpandas.a
```

```bash
gcc -std=c11 -Iinclude my_program.c build/libcpandas.a -o my_program
```

(The default `make` build has the sanitizers turned on, so a program linked
with that library must also be compiled with `-fsanitize=address,undefined`.)

---

## Running the examples

Run them from the project folder, so the paths to `examples/data/` work:

```bash
./build/demo examples/data/employees.csv
```

```bash
./build/basic
```

The **demo** is a numbered menu: load CSV, show, head, tail, slice, select
columns, filter, sort, statistics, group-by, join with another CSV, rename,
remove a column, drop missing rows, save, describe, stack another CSV, add
a computed column, and read a CSV in chunks. Every operation that makes a
new table asks whether to keep it, so you can chain them. Try a join with
`examples/data/departments.csv` on `dept`, stack
`examples/data/new_hires.csv` (it brings a date and a true/false column),
or load `examples/data/broken.csv` to see an error with its line number.
Without a file, the demo starts with a built-in sample table.

**basic** loads the employee table, prints statistics, filters and sorts,
groups by department, joins with the departments, and saves the result to
`build/basic_output.csv`.

---

## Tests

There are twelve test programs and about 7,200 checks. They cover normal,
boundary, invalid, empty, large (100,000 rows) and failure cases for every
module, and all run clean under AddressSanitizer and UBSan. The hash-based
group-by and join are also checked against slow, obviously correct
versions on random tables.

| Test | Module |
|---|---|
| `test_series` | Series |
| `test_types` | BOOL and DATE columns, dates, shared text storage |
| `test_dataframe` | DataFrame |
| `test_csv` | CSV reader / writer, round trips, chunks, error line numbers |
| `test_rows`, `test_filter`, `test_sort`, `test_stats`, `test_groupby`, `test_join` | query layer |
| `test_concat`, `test_compute` | stacking tables, computed columns |

The tables of what each test covers are in the documents in
`documentation/`.

---

## Documentation

- [documentation/part_a_data_layer.md](documentation/part_a_data_layer.md):
  Series, DataFrame, CSV and errors. Design, data structures, algorithms,
  API, memory ownership, limitations, tests.
- [documentation/part_b_query_layer.md](documentation/part_b_query_layer.md):
  rows, filter, sort, statistics, group-by, join and the demo. Same
  sections.
- [documentation/extensions.md](documentation/extensions.md): everything
  added after the interim presentation (dates and booleans, shared text
  storage, chunked CSV reading and error line numbers, hash group-by and
  join with several keys, more statistics, stacking, computed columns) and
  the benchmark against pandas.
- The headers in `include/` describe every function, including who owns
  which memory.

### Memory ownership in short

- Functions that return a `Series *` or `DataFrame *` allocate it. **You
  own it** and free it with `series_free()` / `df_free()`.
- `df_add_column(df, s)` takes ownership of `s` **only if it succeeds**.
- Strings you pass in are always copied. Pointers you get back from
  `df_get_column`, `series_name` or `series_get_string` are **borrowed**:
  don't free them, and don't use them after the object changes.
- Every new frame is a deep copy, and inputs are never changed.
- On failure, the out-pointer is `NULL` and nothing leaks.

---

## Limitations (main ones)

- Most operations are 2–8× slower than pandas (one function call per cell
  keeps the types opaque); group-by is faster. See the benchmark.
- Numbers are compared and summed as `double` (exact up to 2⁵³).
- `df_read_csv` keeps all parsed fields in memory; use
  `df_read_csv_chunks` for files bigger than the memory. An empty string
  and a missing cell are the same thing in CSV.
- Dates are days only (no times), written `YYYY-MM-DD`.

The full lists are in the documents.

---

## Division of work

> **TODO (to be decided by the team).**
