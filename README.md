# CPandas

A small C library inspired by Python's **pandas**, for working with tables of
data. You can load a CSV file into a DataFrame, look at it, filter, sort,
compute statistics, group and aggregate, join two tables and save the result
back to CSV. Columns can hold integers, floating-point numbers or text, and
any cell can be missing.

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
| Series | typed column (INT / FLOAT / STRING) with missing values, growing storage | `series.h` |
| DataFrame | named columns of equal length; add, remove, rename, select columns; drop rows with missing cells; pretty printing | `dataframe.h` |
| CSV | reader with type inference, quotes, custom delimiter and NA words; writer that round-trips | `csv.h` |
| Row selection | head, tail, slice, take rows by number | `df_rows.h` |
| Filtering | `== != < <= > >=` on numbers and text, missing / present, your own condition | `df_filter.h` |
| Sorting | by one or more columns, each ascending or descending, stable | `df_sort.h` |
| Statistics | count, sum, mean, median, min, max | `df_stats.h` |
| Group-by | group by a key column, several aggregations at once | `df_groupby.h` |
| Join | inner, left, right and outer merge on a key column | `df_join.h` |
| Errors | every failure returns a status code; `df_strerror()` explains it | `df_error.h` |

`#include "cpandas.h"` includes all of them.

---

## Project structure

```
CPandas/
├── include/            public headers (the API)
│   ├── cpandas.h       includes everything
│   ├── df_error.h  series.h  dataframe.h  csv.h               data layer
│   └── df_rows.h  df_filter.h  df_sort.h  df_stats.h  df_groupby.h  df_join.h
├── src/                one .c file per header (the library has no main)
├── tests/              one test program per module + test_helpers.h
├── examples/
│   ├── demo.c          interactive menu demo
│   ├── basic.c         short step-by-step example
│   └── data/           employees.csv, departments.csv
├── documentation/
│   ├── part_a_data_layer.md     Series, DataFrame, CSV, errors
│   └── part_b_query_layer.md    rows, filter, sort, stats, group-by, join
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
remove a column, drop missing rows, and save. Every operation that makes a
new table asks whether to keep it, so you can chain them. Try a join with
`examples/data/departments.csv` on `dept`. Without a file, the demo starts
with a built-in sample table.

**basic** loads the employee table, prints statistics, filters and sorts,
groups by department, joins with the departments, and saves the result to
`build/basic_output.csv`.

---

## Tests

There are nine test programs and about 3,100 checks. They cover normal,
boundary, invalid, empty, large (100,000 rows) and failure cases for every
module, and all run clean under AddressSanitizer and UBSan.

| Test | Module |
|---|---|
| `test_series` | Series |
| `test_dataframe` | DataFrame |
| `test_csv` | CSV reader / writer, including write→read round trips |
| `test_rows`, `test_filter`, `test_sort`, `test_stats`, `test_groupby`, `test_join` | query layer |

The tables of what each test covers are in the two documents in
`documentation/`.

---

## Documentation

- [documentation/part_a_data_layer.md](documentation/part_a_data_layer.md):
  Series, DataFrame, CSV and errors. Design, data structures, algorithms,
  API, memory ownership, limitations, tests.
- [documentation/part_b_query_layer.md](documentation/part_b_query_layer.md):
  rows, filter, sort, statistics, group-by, join and the demo. Same
  sections.
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

- One key column for group-by and join; the join is O(n·m) nested loops.
- Numbers are compared and summed as `double` (exact up to 2⁵³).
- CSV files are loaded fully into memory. An empty string and a missing
  cell are the same thing in CSV. There are no date or boolean types.
- There are no error line numbers for malformed CSV.

The full lists are in the two documents.

---

## Division of work

> **TODO (to be decided by the team).**
