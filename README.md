# CPandas

A small C library inspired by Python's **pandas**, for working with tables of
data. You can load a CSV file into a DataFrame, look at it, filter, sort,
compute statistics, group and aggregate, join two tables, stack tables, add
computed columns and save the result back to CSV. Columns can hold
integers, floating-point numbers, text, true/false values or dates, and any
cell can be missing.

The repository also has a **machine-learning pipeline** that uses CPandas
to clean and prepare a messy HR dataset for a scikit-learn model that
predicts which employees will leave ([ml/README.md](ml/README.md)), a
**desktop GUI** written in C with GTK 4 (`make gui`), and a **benchmark
against pandas** on a million rows.

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

**Quick start** from the project folder, on Linux or WSL. The first
time, do the [setup steps](#setting-up-on-ubuntu) first.

| Command | What it does |
|---|---|
| `make` | builds the library and the examples |
| `make test` | runs all the tests |
| `./build/demo examples/data/employees.csv` | the menu demo in the terminal |
| `make gui` | the desktop window (needs GTK 4) |
| `make ml` | the machine-learning pipeline (needs pandas and scikit-learn) |
| `make bench ROWS=100000` | the speed comparison with pandas |

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
├── gui/                desktop window in C + GTK 4 (make gui)
├── bench/              benchmark against pandas (make bench)
├── ml/                 machine-learning pipeline: CPandas preprocessing +
│                       scikit-learn model (make ml, see ml/README.md)
├── documentation/
│   ├── part_a_data_layer.md     Series, DataFrame, CSV, errors
│   ├── part_b_query_layer.md    rows, filter, sort, stats, group-by, join
│   └── extensions.md            new types, hash tables, chunks, benchmark…
├── Makefile
└── README.md
```

---

## Setting up on Ubuntu

These steps are for Ubuntu installed on the computer itself (for example
dual boot) and for Ubuntu in WSL on Windows. They also work on other
Debian-based Linux systems. Steps 1, 5 and 6 are needed only once.

**1. Install the tools.** You need the C compiler and `make`
(`build-essential`) and `git`:

```bash
sudo apt update
```

```bash
sudo apt install build-essential git
```

**2. Get the code.** Clone it into your Linux home folder:

```bash
cd ~
```

```bash
git clone https://github.com/Samy-uktha/CPandas.git
```

```bash
cd CPandas
```

**On WSL, if the project is already on a Windows drive,** you can use it
there instead. Windows drives appear under `/mnt`, and the quotes are
needed when the path has spaces:

```bash
cd "/mnt/d/path/to/CPandas"
```

Everything works there. Reading and writing big files is slower than in
the Linux home folder, though, which shows in the benchmark.

**3. Build and test the library.** This needs nothing beyond step 1:

```bash
make
```

```bash
make test
```

Every test program should end with `N checks, 0 failures`.

**4. Try the demo:**

```bash
./build/demo examples/data/employees.csv
```

**5. Install pandas and scikit-learn.** These are needed only for
`make ml` and `make bench`, and either of these two ways works.

*The simple way:* Ubuntu's own packages, installed system-wide. This
needs no virtual environment. The scripts use the system `python3` when
they find no `.venv`.

```bash
sudo apt install python3-pandas python3-sklearn
```

*Or newer versions in a `.venv` folder inside the project,* which installs
nothing system-wide. `make clean` does not touch this folder, and git
ignores it.

```bash
sudo apt install python3-venv
```

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install pandas scikit-learn
```

Don't use plain `pip install` outside a `.venv`. Ubuntu blocks it on
purpose, to protect the system's own Python.

**6. (Optional) the desktop GUI** needs GTK 4 for building:

```bash
sudo apt install libgtk-4-dev pkg-config
```

```bash
make gui
```

On WSL the window opens like a normal Windows window, because Windows 11
shows Linux apps by itself. Close the window to get the terminal back.

**7. Run the ML pipeline and the benchmark:**

```bash
make ml
```

```bash
make bench ROWS=100000
```

`make bench` with no `ROWS` uses 1,000,000 rows. It makes about 100 MB of
test files in `build/bench_data/` and takes a minute or two.

### If something goes wrong

| Problem | Fix |
|---|---|
| `make: command not found`, or `gcc: command not found` | step 1 was skipped: `sudo apt install build-essential` |
| Every test crashes at the start with `AddressSanitizer:DEADLYSIGNAL` | A known clash between newer Linux kernels and older AddressSanitizer versions (for example on Ubuntu 24.04). Run `sudo sysctl vm.mmap_rnd_bits=28`; it lasts until the next reboot. Or build without the sanitizers: `CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -g" make test` |
| `The virtual environment was not created successfully because ensurepip is not available` | `sudo apt install python3-venv`, delete the half-made `.venv` folder, and repeat step 5 |
| `scikit-learn and pandas are needed for this step` from `make ml` | step 5 was skipped. The scripts look for Python in this order: `$PYTHON`, `.venv/`, `build/venv/`, the system `python3`. To use another one: `PYTHON=/path/to/python make ml` |
| `error: externally-managed-environment` from `pip install` | use one of the two ways in step 5 instead of plain `pip` |
| `./build/demo: No such file or directory` | run `make` first, from the `CPandas` folder |
| `dpkg was interrupted, you must manually run 'sudo dpkg --configure -a'` | an earlier install was stopped half-way: run `sudo dpkg --configure -a`, then `sudo apt --fix-broken install`, then the install again |
| `make gui` says `gtk/gtk.h: No such file or directory` or `pkg-config: not found` | `sudo apt install libgtk-4-dev pkg-config` |
| On WSL, `make gui` runs but no window appears | update WSL from PowerShell with `wsl --update`, then `wsl --shutdown`, and open Ubuntu again |
| Paths to `examples/data/...` are not found | run the programs from the `CPandas` folder itself, not from `build/` |

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
| `make gui` | builds and opens the desktop GUI (needs GTK 4, see the setup steps) |
| `make ml` | runs the machine-learning pipeline: CPandas cleans `ml/data/raw/` and scikit-learn trains a model on the result; see [ml/README.md](ml/README.md) |
| `make clean` | deletes `build/` (not `.venv/`) |

By default the Makefile turns on **AddressSanitizer and UBSan**, so memory
leaks and invalid memory accesses make the tests fail. This works on Linux,
WSL and macOS.

**Windows:** the easiest way is WSL (Ubuntu inside Windows), with the
same steps as in [Setting up on Ubuntu](#setting-up-on-ubuntu).
**MinGW gcc in Git Bash** has no AddressSanitizer, so set
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

**The GUI** (`make gui`, from the project folder) is a desktop window
with two tabs:

- **Data:**
  - Open a CSV and see it as a table, with types in the headers and grey
    `NA` cells.
  - Filter, sort, group, join, stack, add computed columns, see column
    statistics, describe, drop missing rows, and remove columns, all from
    the sidebar.
  - Undo goes back up to 30 steps, and Save CSV writes the result.
- **ML pipeline:** **Run pipeline** runs `make ml` and shows:
  - its full log;
  - the test scores;
  - the confusion matrix (counted again from the predictions with
    CPandas);
  - a bar chart of the strongest signals;
  - the raw, cleaned or predicted data as a table.

The GUI calls CPandas directly for every table it shows. GTK only draws.
It is built without the sanitizers, because GTK's own memory at start-up
would flood the leak report.

**The ML pipeline** (`make ml`) runs two steps:

1. `ml/preprocess.c` uses CPandas to clean `ml/data/raw/` into
   `build/ml/train.csv` and `test.csv`. It fixes the spellings, removes
   duplicates, joins the tables, builds features, fills gaps, makes one-hot
   columns and scales the numbers.
2. `ml/train.py` trains and tests scikit-learn models on those files.

The chosen model, logistic regression, reaches a ROC AUC of 0.82 on the
test set and catches 74% of the employees who leave. The steps and results
are in [ml/README.md](ml/README.md).

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
- [ml/README.md](ml/README.md): the attrition-prediction pipeline that
  uses CPandas to clean, join, encode and scale data for a scikit-learn
  model, with its results.
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
  keeps the types opaque); group-by is faster. See the benchmark in
  [extensions.md §7](documentation/extensions.md#7-benchmark-against-pandas).
- There are no text functions (trim, upper / lower case, replace),
  `drop_duplicates` or one-hot encoding built in; the ML pipeline shows
  how to do them with the existing functions.
- Numbers are compared and summed as `double` (exact up to 2⁵³).
- `df_read_csv` keeps all parsed fields in memory; use
  `df_read_csv_chunks` for files bigger than the memory. An empty string
  and a missing cell are the same thing in CSV.
- Dates are days only (no times), written `YYYY-MM-DD`.

The full lists are in the documents.

---

## Division of work

> **TODO (to be decided by the team).**
