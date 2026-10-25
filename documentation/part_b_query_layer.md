# CPandas – Part B: the query layer

Author: Vedant Singh (Person B)

This document covers everything you can do to a DataFrame once it exists:
row selection, filtering, sorting, statistics, group-by, joins, the CLI demo
and their tests. The data layer (Series, DataFrame container, CSV reader and
writer, error codes) is Part A, by Samyuktha K, and is documented in its own
headers (`series.h`, `dataframe.h`, `csv.h`, `df_error.h`).

---

## 1. Overview

Part B is a set of small modules. Each one has a header in `include/` and
a source file in `src/`.

| Module | Header | What it does |
|---|---|---|
| Rows | `df_rows.h` | head, tail, slice, pick rows by number |
| Filter | `df_filter.h` | keep rows that match a condition |
| Sort | `df_sort.h` | sort rows by one or more columns |
| Stats | `df_stats.h` | count, sum, mean, median, min, max of a column |
| Group-by | `df_groupby.h` | group rows by a key and aggregate each group |
| Join | `df_join.h` | merge two frames on a key (inner, left, right, outer) |

`include/cpandas.h` includes every header of the library (Part A and Part B),
so a program only needs:

```c
#include "cpandas.h"
```

---

## 2. Architecture / design

### 2.1 Part B only uses Part A's public API

Series and DataFrame are opaque types, so Part B cannot see their fields.
It only calls functions from `series.h` and `dataframe.h`, such as
`df_get_column`, `series_get_int`, `series_take` and `df_add_column`.
Because of this, Part A can change how it stores data without breaking
Part B.

### 2.2 Everything is built on "a list of row numbers"

Almost every operation works in two steps:

1. **Decide which rows the result needs, and in what order.** This gives an
   array of row numbers, `size_t rows[]`.
2. **Copy those rows.** `series_take()` from Part A is called on every column
   with that array, and the copied columns go into a new DataFrame.

```
               decide rows                       copy rows
 input frame ─────────────────► size_t rows[] ─────────────────► new frame
               (filter: test     e.g. {4, 0, 3}  (df_take_rows:
                each row;                         series_take on
                sort: merge                       every column)
                sort the list)
```

| Operation | How it builds the row list |
|---|---|
| `df_head` / `df_tail` / `df_slice` | consecutive numbers `start .. end-1` |
| `df_filter_*` | the rows that pass the test, in their original order |
| `df_sort_by` | `0 .. n-1`, then merge-sorted |
| `df_groupby` | sorted rows, cut into runs with the same key |
| `df_merge` | two lists (left rows, right rows), one entry per output row |

`df_take_rows()` (in `df_rows.c`) does step 2 for filter, sort and slice.
Group-by and join call `series_take()` themselves, because each of their
result columns uses a different list.

For left, right and outer joins, an unmatched row is written as
`SERIES_TAKE_MISSING` in the list. `series_take()` turns that into a missing
cell.

### 2.3 Module dependencies

```
df_rows ◄── df_filter
   ▲
   └─────── df_sort ◄── df_groupby ──► df_stats
              ▲
              └──────── df_join
```

All modules depend on Part A (`series.h`, `dataframe.h`, `df_error.h`).

### 2.4 Error handling

Part B follows the convention in `df_error.h`:

- Every function that can fail returns a `df_status`. Results come back
  through out-parameters.
- On failure, a pointer out-parameter is set to `NULL`. A number
  out-parameter (for example in `series_mean`) is left unchanged.
- **A function that fails never changes its input frames.** Part B functions
  never change their inputs, even when they succeed. They always return a
  new frame.
- The library never prints anything and never calls `exit()`. The caller
  can get a readable message with `df_strerror(status)`.

All arguments are checked before any memory is allocated, wherever that is
possible. Then a bad argument costs nothing, and the failure path stays
simple.

### 2.5 Missing values (one rule for the whole layer)

| Where | What happens to a missing cell |
|---|---|
| Comparison filters | never matches, not even `!=` |
| `df_filter_missing` | selects rows where the cell is / is not missing |
| Sort | always goes **last**, both ascending and descending |
| Statistics | skipped; `count` counts only non-missing cells |
| Group-by key | rows with a missing key are dropped |
| Group-by value | skipped; a group with no values gets a missing result cell |
| Join key | never matches anything (like SQL `NULL`) |

---

## 3. Data structures

Part B adds only a few small internal structures. Everything else is a
plain `size_t` array of row numbers.

| Structure | File | Fields | Purpose |
|---|---|---|---|
| `SortInfo` | `df_sort.c` | `const Series **columns`, `const int *ascending`, `size_t ncolumns` | Everything the row-compare function needs: which columns and which direction. It is passed as an argument, so no global variable is needed. |
| `PairList` | `df_join.c` | `size_t *left`, `size_t *right`, `size_t count`, `size_t cap` | A growing list of matched (left row, right row) pairs. The capacity starts at 16 and doubles when full. |
| `group_start[]` | `df_groupby.c` | `size_t` array of length `ngroups + 1` | Where each group starts in the sorted row list. The extra last slot holds the end, so group `g` is always `group_start[g] .. group_start[g+1]-1`. |
| `right_used[]` | `df_join.c` | `unsigned char` array (`calloc`) | Outer join only: marks right rows that found a partner. |

Public types (in the headers):

```c
typedef enum { DF_OP_EQ, DF_OP_NE, DF_OP_LT, DF_OP_LE, DF_OP_GT, DF_OP_GE } df_compare_op;
typedef int (*df_row_condition)(const DataFrame *df, size_t row, void *user_data);
typedef enum { DF_AGG_COUNT, DF_AGG_SUM, DF_AGG_MEAN, DF_AGG_MEDIAN, DF_AGG_MIN, DF_AGG_MAX } df_agg_func;
typedef struct { const char *column; df_agg_func func; } df_aggregation;
typedef enum { DF_JOIN_INNER, DF_JOIN_LEFT, DF_JOIN_RIGHT, DF_JOIN_OUTER } df_join_type;
```

---

## 4. Algorithms

In the table below, *n* is the number of rows, *k* the number of sort or
aggregation columns, *g* the number of groups, and *m* the number of rows
in the right frame.

| Operation | Algorithm | Time | Extra memory |
|---|---|---|---|
| head / tail / slice / take | build the row list, then copy each column | O(rows × columns) | one row list |
| filter | one pass over the rows, testing each one | O(n) tests + copy | one row list (size n) |
| sort | **merge sort** on row numbers, then one copy | O(k · n log n) compares | two `size_t` arrays (size n) |
| median | copy the non-missing values, `qsort`, pick the middle | O(n log n) | one `double` array |
| other stats | one pass | O(n) | none |
| group-by | sort the rows by key, then find the runs | O(n log n + n · aggregations) | row list, group list |
| join | **nested loops** over left × right | O(n · m) compares | pair list (size of result) |

### 4.1 Why merge sort?

- **It is stable.** Rows that are equal in every sort column keep their input
  order. This gives predictable output, and sorting by B and then by A gives
  the same result as one sort by A, B. The merge step takes from the right
  half only when that element is *strictly* smaller, which keeps it stable.
- **It is always O(n log n)**, even on sorted or reversed input.
- **`qsort()` was not used for rows.** It is not guaranteed stable, and its
  compare function cannot receive extra data (which columns, which
  direction) without a global variable.
- We sort the **row numbers**, not the data. A swap moves one `size_t` rather
  than a whole row, and each column is copied only once at the end.

The recursion depth is about log₂(n), around 17 for 100,000 rows, so the
stack is not a problem.

### 4.2 Multi-column comparison

`compare_rows()` compares two rows one sort column at a time. It moves to
the next column only when the current cells are equal. The missing-value
check happens **before** the result is flipped for descending order. That
is why missing cells stay last in both directions.

### 4.3 Group-by by sorting

Sorting the row numbers by key places equal keys next to each other.
Missing keys go to the end, where they are simply ignored. One pass then
finds where each new key starts. For each group and each aggregation, the
group's cells are copied into a temporary Series with `series_take()`, and
`series_aggregate()` from `df_stats.c` is called on it. The temporary Series
is freed straight away. This reuses the statistics code instead of writing
it twice. As a side effect, the groups come out sorted by key, like pandas'
default `sort=True`.

### 4.4 Join by nested loops

Each left row is compared with each right row using `series_compare_cells`.
Every match is appended to the `PairList`. Then:

- **LEFT / OUTER:** a left row with no match adds `(i, MISSING)`.
- **RIGHT:** the loops are swapped, so the right frame drives the order, and
  an unmatched right row adds `(MISSING, j)`.
- **OUTER:** after the left pass, every right row that never matched
  (`right_used[j] == 0`) adds `(MISSING, j)`.

For a right-only row the left key cell is missing, so the key column is
then filled in from the right frame with `copy_cell()`.

Nested loops were chosen because they are short, easy to check, and keep
the natural "left order, then right order" of the rows. A hash join or a
sort-merge join would be faster for big tables (see Limitations).

---

## 5. API reference

Every function returns `DF_OK` on success, or an error code from
`df_error.h`. Any required pointer argument that is `NULL` gives
`DF_ERR_NULL_ARG`. Every function that takes an `out` parameter sets
`*out = NULL` first.

### 5.1 Row selection – `df_rows.h`

```c
df_status df_head(const DataFrame *df, size_t n, DataFrame **out);
df_status df_tail(const DataFrame *df, size_t n, DataFrame **out);
df_status df_slice(const DataFrame *df, size_t start, size_t end, DataFrame **out);
df_status df_take_rows(const DataFrame *df, const size_t *rows, size_t n, DataFrame **out);
```

| Function | Result | Notes |
|---|---|---|
| `df_head` | first `n` rows | `n` larger than the row count → all rows; `n = 0` → 0 rows, same columns |
| `df_tail` | last `n` rows, original order | same rules as `df_head` |
| `df_slice` | rows `start .. end-1` (like Python `[start:end]`) | `end` is cut back to the row count; `start >= end` → 0 rows. Never fails because of the range. |
| `df_take_rows` | rows in the order listed, repeats allowed | any row ≥ `df_nrows` → `DF_ERR_INDEX_OUT_OF_RANGE`; `rows` may be `NULL` only if `n == 0` |

### 5.2 Filtering – `df_filter.h`

```c
df_status df_filter_number(const DataFrame *df, const char *column,
                           df_compare_op op, double value, DataFrame **out);
df_status df_filter_string(const DataFrame *df, const char *column,
                           df_compare_op op, const char *value, DataFrame **out);
df_status df_filter_missing(const DataFrame *df, const char *column,
                            int keep_missing, DataFrame **out);
df_status df_filter_custom(const DataFrame *df, df_row_condition keep,
                           void *user_data, DataFrame **out);
```

| Function | Works on | Errors |
|---|---|---|
| `df_filter_number` | `DTYPE_INT` and `DTYPE_FLOAT` columns (INT cells are compared as double) | `NOT_FOUND`, `TYPE_MISMATCH` (string column), `INVALID_ARG` (bad `op`) |
| `df_filter_string` | `DTYPE_STRING` columns, compared with `strcmp` | `NOT_FOUND`, `TYPE_MISMATCH` (number column), `INVALID_ARG` |
| `df_filter_missing` | any column; `keep_missing != 0` keeps missing rows, `0` keeps present rows | `NOT_FOUND` |
| `df_filter_custom` | your function, called once per row in order; return non-zero to keep the row | `NULL_ARG` if `keep` is `NULL` |

The result keeps every column and the original row order. No match gives
0 rows with the same columns, which is not an error.

**AND:** filter the result again. **OR or anything else:** use
`df_filter_custom`.

```c
/* age < 30 OR city == "Pune" */
static int young_or_pune(const DataFrame *df, size_t row, void *data)
{
    const Series *age = NULL, *city = NULL;
    int64_t a;
    const char *c;
    (void)data;
    df_get_column(df, "age", &age);
    df_get_column(df, "city", &city);
    if (series_get_int(age, row, &a) == DF_OK && a < 30) return 1;
    if (series_get_string(city, row, &c) == DF_OK && strcmp(c, "Pune") == 0) return 1;
    return 0;
}
...
df_filter_custom(df, young_or_pune, NULL, &out);
```

### 5.3 Sorting – `df_sort.h`

```c
df_status df_sort_by(const DataFrame *df, const char *const *columns,
                     const int *ascending, size_t ncolumns, DataFrame **out);
df_status df_sort_by_column(const DataFrame *df, const char *column,
                            int ascending, DataFrame **out);
df_status df_sort_indices(const DataFrame *df, const char *const *columns,
                          const int *ascending, size_t ncolumns, size_t **out_rows);
int series_compare_cells(const Series *a, size_t i, const Series *b, size_t j);
```

| Function | Notes |
|---|---|
| `df_sort_by` | `ascending[k] != 0` sorts column k small to large; `ascending == NULL` sorts all columns ascending. `ncolumns == 0` → `INVALID_ARG`; unknown name → `NOT_FOUND`. |
| `df_sort_by_column` | shortcut for a single column |
| `df_sort_indices` | gives back only the sorted row numbers (like pandas `argsort`): `(*out_rows)[0]` is the row that comes first. For a frame with 0 rows, `*out_rows` is `NULL` and the call returns `DF_OK`. |
| `series_compare_cells` | returns < 0, 0 or > 0 (ascending order). Two missing cells are equal; missing comes after any value. Returns 0 if the dtypes differ, an index is out of range, or an argument is `NULL`. |

Ordering: numbers by value, strings by `strcmp` (byte order, so it is
case-sensitive and `"B"` comes before `"a"`).

```c
const char *cols[] = {"dept", "salary"};
int asc[] = {1, 0};                      /* dept A→Z, then salary high→low */
df_sort_by(df, cols, asc, 2, &sorted);
```

### 5.4 Statistics – `df_stats.h`

```c
df_status series_count(const Series *s, size_t *out);
df_status series_sum(const Series *s, double *out);
df_status series_mean(const Series *s, double *out);
df_status series_median(const Series *s, double *out);
df_status series_min(const Series *s, double *out);
df_status series_max(const Series *s, double *out);
df_status series_aggregate(const Series *s, df_agg_func func, double *out);
const char *df_agg_name(df_agg_func func);
```

| Function | Result | When there are no values |
|---|---|---|
| `series_count` | number of non-missing cells; works on **any** dtype | 0 |
| `series_sum` | total | 0 |
| `series_mean` | sum / count | `DF_ERR_MISSING_VALUE` |
| `series_median` | middle value; average of the two middle values for an even count | `DF_ERR_MISSING_VALUE` |
| `series_min` / `series_max` | smallest / largest value | `DF_ERR_MISSING_VALUE` |
| `series_aggregate` | runs the function named by `func`; the count comes back as a double | as above; unknown `func` → `INVALID_ARG` |
| `df_agg_name` | `"count"`, `"sum"`, `"mean"`, `"median"`, `"min"`, `"max"`, or `"unknown"` | – |

"No values" means an empty column or one where every cell is missing.
Every function except `count` needs a number column, and fails with
`DF_ERR_TYPE_MISMATCH` on a string column. On failure, `*out` is left
unchanged.

```c
const Series *col = NULL;
double avg;
if (df_get_column(df, "salary", &col) == DF_OK &&
    series_mean(col, &avg) == DF_OK)
    printf("average salary = %g\n", avg);
```

### 5.5 Group-by – `df_groupby.h`

```c
df_status df_groupby(const DataFrame *df, const char *key_column,
                     const df_aggregation *aggs, size_t naggs, DataFrame **out);
```

```c
df_aggregation aggs[] = { {"salary", DF_AGG_MEAN}, {"name", DF_AGG_COUNT} };
df_groupby(df, "dept", aggs, 2, &out);   /* dept | salary_mean | name_count */
```

- Result columns: first the key column (same name and dtype), then one
  column per aggregation named `<column>_<function>`.
- Count columns are `DTYPE_INT`; all other results are `DTYPE_FLOAT`.
- Groups are sorted by key (ascending), and rows with a missing key are
  dropped.
- `naggs = 0` (with `aggs = NULL`) returns only the list of distinct keys.
- Errors: `NOT_FOUND` (unknown key or aggregation column), `TYPE_MISMATCH`
  (a non-count function on a string column), `INVALID_ARG` (unknown
  function), `DUPLICATE_NAME` (two result columns would get the same name).
  Every aggregation is checked before any work starts.

### 5.6 Join – `df_join.h`

```c
df_status df_merge(const DataFrame *left, const DataFrame *right,
                   const char *on, df_join_type how, DataFrame **out);
```

| `how` | Rows in the result | Row order |
|---|---|---|
| `DF_JOIN_INNER` | key in both frames | left order; each left row's matches in right order |
| `DF_JOIN_LEFT` | every left row | same as inner |
| `DF_JOIN_RIGHT` | every right row | right order; each right row's matches in left order |
| `DF_JOIN_OUTER` | every row of both | left join, then the unmatched right rows in right order |

- Duplicate keys give every matching pair: 2 left × 3 right = 6 rows.
- Columns: all left columns (the key included), then the right columns
  except the key.
- A non-key name used in both frames becomes `<name>_left` and
  `<name>_right`.
- `left` and `right` may be the same frame (a self-join).
- Errors: `NOT_FOUND` (a frame has no `on` column), `TYPE_MISMATCH` (the
  key dtypes differ), `INVALID_ARG` (unknown `how`), `DUPLICATE_NAME` (a
  suffixed name already exists, for example a frame with both `v` and
  `v_left`).

---

## 6. Memory ownership rules

| Question | Answer for Part B |
|---|---|
| **Who allocates?** | The library. Every function that returns a `DataFrame *` creates it with `df_create()`. `df_sort_indices()` allocates its array with `malloc()`. |
| **Who owns the result?** | The caller. |
| **Who frees it?** | The caller: `df_free()` for a frame, `free()` for the `df_sort_indices` array. |
| **Is anything shared with the input?** | No. Every result is a deep copy, made with `series_take()`. You may change or free the input straight away, and the result stays valid. (`test_rows.c` checks this under AddressSanitizer.) |
| **Are the inputs changed?** | Never, not even on success. Input frames, the `columns`/`ascending` arrays, the `aggs` array and filter values are only read during the call. The caller keeps ownership of them. |
| **What happens when an operation fails?** | `*out` (or `*out_rows`) is `NULL`, every temporary allocation is freed, the inputs are unchanged, and nothing leaks. |
| **Borrowed pointers** | Part B never returns a pointer into a frame. Inside a `df_filter_custom` condition, pointers from `df_get_column` / `series_get_string` are borrowed for that call only, and the condition must not change or free the frame. |
| **Statistics functions / `series_compare_cells`** | Allocate nothing that the caller must free. `series_median` uses a temporary array and frees it before returning. |

### 6.1 Ownership hand-off pattern used inside the library

`df_add_column()` (Part A) takes ownership of a Series **only when it
succeeds**. So every Part B function follows this pattern:

```c
st = df_add_column(result, part);
if (st != DF_OK) {
    series_free(part);    /* the frame did not take it, so we still own it */
    df_free(result);      /* throw away the half-built result              */
    return st;
}
```

### 6.2 Ownership in the demo

In `examples/demo.c`, `current` is the one frame the program owns.
`offer_result()` either replaces `current` with the new result (and frees
the old one) or frees the result. The joined "other" CSV is freed as soon
as `df_merge` has copied it. `current` is freed before `main` returns.

---

## 7. Compilation and running

The library, tests and demo are built with Part A's `Makefile`, which picks
up every `src/*.c` and `tests/test_*.c` automatically.

**Tests, with AddressSanitizer and UBSan (Linux / WSL):**

```bash
make test
```

Each test prints `N checks, 0 failures`. A leak or invalid memory access
makes AddressSanitizer stop the run with an error.

**Without sanitizers** (for example with MinGW gcc in Git Bash on Windows,
which has no ASan), set `CFLAGS` as an environment variable in front of
`make`:

```bash
CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -g" make test
```

Don't write it as `make test CFLAGS=...`. A value given on the make command
line replaces the Makefile's own `CFLAGS += -Iinclude ...` line, so the
headers are not found.

**The CLI demo:**

```bash
gcc -std=c11 -Wall -Wextra -Iinclude examples/demo.c src/*.c -o demo
./demo examples/data/employees.csv
```

Running `./demo` with no file loads the built-in sample table
(`df_make_sample`).

### 7.1 Demo walkthrough

| Menu | Try |
|---|---|
| 8 Filter | `dept`, `==`, `IT` → 3 rows |
| 8 Filter | `age`, `missing` → Chen |
| 9 Sort | `dept asc salary desc` |
| 10 Statistics | `salary` → count 7, sum 343500, median 45500 |
| 11 Group by | `dept`, then `salary mean salary max name count` |
| 12 Join | `examples/data/departments.csv`, `dept`, `outer`: Ops has no department row (NA), and Finance has no employees |
| 16 Save | `out.csv`, then 1 Load it back |

Every operation that makes a new table asks "Keep this as the current
table?", so operations can be chained (filter, then sort, then group).

---

## 8. Test cases

There are six test programs, with **701 checks** in total, all passing
under `-fsanitize=address,undefined`.

| File | Checks | Normal | Boundary | Invalid | Empty | Large | Failure / memory |
|---|---|---|---|---|---|---|---|
| `test_rows.c` | 91 | head/tail/slice/take values | n = 0, n = nrows, n > nrows, start ≥ end, start past end, repeated rows | row ≥ nrows, `SERIES_TAKE_MISSING`, NULL args | 0-column frame | 100,000 rows | result readable after freeing the input (deep copy) |
| `test_filter.c` | 144 | all 6 operators on int, float and string; missing/present; custom OR | no match → 0 rows; case-sensitive strings | unknown column, wrong type, bad operator, NULLs | columns with 0 rows | 100,000 rows | input unchanged; chained AND |
| `test_sort.c` | 118 | int, float, string; asc/desc; multi-column; `ascending = NULL` | ties keep order (stable); NA last both ways; all-missing column; 1 row | unknown column, `ncolumns = 0`, NULL name, NULL args | 0 rows (`out_rows == NULL`) | 100,000 rows: sorted, stable, no row lost | input unchanged |
| `test_stats.c` | 79 | all 6 stats on int and float; even and odd median | single value, negatives | string column, bad function, NULLs | empty and all-missing (`*out` unchanged) | 100,001 shuffled values | – |
| `test_groupby.c` | 97 | string and int keys, 5 aggregations, sorted groups, result types | group with all values missing; missing key dropped; `naggs = 0` | unknown key/column, mean of string, bad function, NULL column | 0 rows, all keys missing | 100,000 rows, 10 groups | duplicate result name fails half-way, no leak |
| `test_join.c` | 172 | inner, left, right, outer with duplicate keys; string key | NA keys; key filled from right; `_left` / `_right`; self-join | missing key column, type mismatch, bad `how`, NULLs | empty right side for all 4 types | 2,000 × 2,000 | suffix clash fails half-way, no leak |

The tests were also checked to make sure they can fail. Five bugs were
planted on purpose (unstable sort, missing keys kept, no key fill in right
joins, wrong even median, missing cells matching filters), plus a memory
leak in group-by. Each bug made a test fail, and AddressSanitizer reported
the leak.

---

## 9. Limitations

- **One key column** for group-by and join. A join needs the key to have
  the **same name** in both frames; rename it first with
  `df_rename_column` if it doesn't.
- **The join is O(n · m).** Fine for thousands of rows, slow for millions.
  A hash join or a sort-merge join would fix this.
- **Join keys must have the same dtype.** An INT key cannot be joined with a
  FLOAT key.
- **A missing join key never matches.** This is the SQL rule. pandas
  matches NaN with NaN.
- **Outer join row order** is "left join, then the unmatched right rows".
  pandas sorts the keys instead.
- **Numbers are compared and summed as `double`.** Integers above 2⁵³ lose
  precision in `df_filter_number` and in the statistics. Very large sums are
  not checked for overflow.
- **Group-by results** are `DTYPE_FLOAT` except count, so the sum of an INT
  column comes back as a float.
- **No min/max on strings.** Only `count` works on string columns.
- **String order is `strcmp` byte order.** It is case-sensitive and does not
  follow the locale.
- **NaN values** (if the CSV reader ever produces them) have no defined
  place in the sort order.
- **Built-in filters combine only with AND** (by filtering again).
  Anything else needs `df_filter_custom`.
- **Out-of-memory paths are handled in code but not tested directly.** There
  is no malloc-failure injection; those paths were checked by reading the
  code and with ASan.
- **Demo:** column names cannot contain spaces (input is split on spaces),
  input lines are at most 255 characters, and at most 20 rows are printed
  (menu 3 prints all of them). The demo's CSV loading, saving and printing
  come from Part A.

---

## 10. Division of work

| Person A – Samyuktha K (data layer) | Person B – Vedant Singh (query layer) |
|---|---|
| `series.h/.c`: storage, types, missing values, growth, `series_take` | `df_rows.h/.c`: head, tail, slice, take rows |
| `dataframe.h/.c`: columns, add/remove/rename, column selection | `df_filter.h/.c`: comparison, missing and custom filters |
| `csv.h/.c`: reader and writer | `df_sort.h/.c`: stable multi-column merge sort |
| `df_error.h/.c`: status codes, `df_strerror` | `df_stats.h/.c`: count, sum, mean, median, min, max |
| `Makefile` | `df_groupby.h/.c`: group-by and aggregation |
| `tests/test_series.c` (and DataFrame/CSV tests) | `df_join.h/.c`: inner, left, right, outer join |
| | `cpandas.h`, `examples/demo.c`, `examples/data/*.csv` |
| | `tests/test_{rows,filter,sort,stats,groupby,join}.c`, `tests/test_helpers.h` |
| | this document |

The interface between the two parts is the set of Part A headers. Part B
uses only the public functions declared there.
