# Machine-learning pipeline: predicting employee attrition

This pipeline uses **CPandas to prepare messy data for a machine-learning
model**. The question it answers: *will an employee leave the company?*
This is a yes/no (binary classification) problem.

```
ml/data/raw/*.csv  ──►  ml/preprocess.c (CPandas, C)  ──►  build/ml/train.csv
  messy HR export        clean, join, features,              build/ml/test.csv
  (3 tables)             fill, encode, scale                      │
                                                                  ▼
                       build/ml/metrics.json  ◄──  ml/train.py (scikit-learn)
                       build/ml/predictions.csv     compare models, train, test
```

All the data preparation is done in C with CPandas. The Python step only
trains and scores models; it does no cleaning, filling or scaling of its
own. It checks that CPandas left no empty cells.

## Running it

From the project folder, in WSL or Linux:

```bash
make ml
```

This builds `build/ml_preprocess` (with AddressSanitizer, like the rest of
the project) and runs `ml/run.sh`.

The Python step needs pandas and scikit-learn. `ml/run.sh` looks for them
in `build/venv/bin/python`; set `PYTHON=/path/to/python` to use another
Python. Without them, step 1 still runs and writes the cleaned files.

The raw files can be made again (always the same, thanks to a fixed seed):

```bash
python3 ml/make_data.py
```

## The data (`ml/data/raw/`)

The data is made up by `ml/make_data.py`, but in the ways real HR exports
are messy.

| File | Rows | Columns |
|---|---:|---|
| `employees.csv` | 6,120 | emp_id, name, dept, city, age, salary, hire_date, remote, overtime, satisfaction, distance_km, **left** (the answer) |
| `reviews.csv` | 18,037 | emp_id, review_date, score (1–5), reviewer; 0 to 6 reviews per employee |
| `departments.csv` | 6 | dept, budget_per_head, avg_team_size, floor |

Problems put in on purpose:

- **Department names in 24 spellings:** `Sales`, `sales`, `SALES`,
  `" sales "`, ….
- **Missing cells:** some are empty, and some say `N/A` or `null`.
- **Impossible ages:** 0, 150, −3.
- **Repeated employees:** 120 employees appear twice.
- **Unknown outcome:** 78 employees have no value for `left`.
- **Orphan reviews:** 40 reviews belong to employees who are not in the
  file.

Whether someone left is decided by a hidden formula plus randomness. Doing
overtime, low satisfaction, low pay for one's department, short tenure,
poor reviews and a long commute make leaving more likely; remote work
makes it less likely. So there are real patterns to learn, but no model
can be perfect. 17.6% of the employees left.

## Step 1: preprocessing with CPandas (`ml/preprocess.c`)

| # | Step | CPandas used | Result |
|---|---|---|---|
| 1 | Load the 3 files, with `N/A`, `NA` and `null` meaning missing | `df_read_csv` with `na_values` and `opts.error` | types found by themselves: `hire_date` is a date, `remote` / `overtime` / `left` are booleans |
| 2 | Fix department spellings by matching them with the department table, ignoring case and spaces | Series API, `series_nunique`, `df_remove_column`, `df_rename_column` | 23 spellings → 6 departments |
| 3 | Remove repeated employees | `df_sort_by_column` on emp_id, then `df_filter_custom` keeping the first of each id | 120 removed |
| 4 | Drop employees whose outcome is unknown | `df_filter_missing` | 77 dropped |
| 5 | Impossible ages become missing | `df_add_computed_column` | 69 ages |
| 6 | Review statistics per employee | `df_groupby` (mean, min, count) and `df_merge` (left join on emp_id) | orphan reviews drop out, and employees without reviews get missing cells |
| 7 | Department facts | `df_select_columns`, `df_merge` (left join on dept) | |
| 8 | Split into train (80%) and test (20%) | `df_filter_custom` on a hash of emp_id | 4,761 / 1,162 rows |
| 9 | New features | see below | |
| 10 | Fill missing values with the **training** median | `series_median`, computed columns | |
| 11 | One-hot encode dept and city, using the categories seen in training | `df_groupby` with no aggregations (the distinct values), computed 0/1 columns | 12 columns |
| 12 | Scale numbers to mean 0 and standard deviation 1, using **training** means and stds | `series_mean`, `series_std`, computed columns | |
| 13 | Write the files | `df_select_columns`, `df_write_csv` | 29 number columns, no empty cells |

The new features (step 9):

| Feature | Built with |
|---|---|
| `tenure_years` = (2026-01-01 − hire_date) / 365.25 | a computed column reading the DATE column |
| `pay_ratio` = salary / median salary of the department | `df_groupby` (median, train only), `df_merge`, `df_add_arith_column` |
| `review_count` = 0 when there are no reviews | computed column |
| `satisfaction_missing`, `salary_missing` = 1 if the cell was empty | computed columns; sometimes the gap itself tells something |
| `remote`, `overtime`, `left` as 1 / 0 | `df_add_scalar_column(…, DF_ARITH_MUL, 1)` turns a BOOL column into INT |

**Why everything is learned from the training rows only.** The department
pay medians, fill values, category lists, means and standard deviations
all come from the 80% training rows, and are then applied unchanged to the
test rows. If the test rows helped choose them, the model would have seen
a bit of the test data, and its test score would be too optimistic. This
is called *data leakage*. The values are saved in
`build/ml/preprocessing_params.csv`, so new data can be prepared exactly
the same way.

## Step 2: the model with scikit-learn (`ml/train.py`)

1. **Compare models** with 5-fold cross-validation on the training rows
   only, scored by ROC AUC. The candidates are:
   - a baseline that always says "stays";
   - logistic regression;
   - a random forest.

   Both real models use balanced class weights, because only about 1 in 6
   employees leaves.
2. **Train the best one** on all the training rows.
3. **Score it once on the test rows**, which played no part in any choice
   before this point.

Results of `make ml`:

| Cross-validation (train) | ROC AUC |
|---|---:|
| baseline (always "stays") | 0.500 |
| **logistic regression** | **0.799** ± 0.016 |
| random forest | 0.787 ± 0.020 |

| Test set, logistic regression | |
|---|---:|
| ROC AUC | 0.822 |
| recall (share of leavers it catches) | 0.744 |
| precision (share of its warnings that are right) | 0.373 |
| F1 | 0.497 |
| accuracy | 0.747 (baseline 0.832) |

|  | predicted stays | predicted leaves |
|---|---:|---:|
| actually stayed | 723 | 244 |
| actually left | 50 | 145 |

**How to read this.**

- **ROC AUC 0.82:** if you take one employee who left and one who stayed,
  the model gives the leaver the higher risk score 82% of the time. A coin
  gives 50%.
- **Recall is the point here.** HR would rather talk to some people who
  were going to stay anyway than miss three out of four leavers. The model
  catches 145 of the 195 people who left.
- **That is why accuracy is below the baseline.** Saying "stays" for
  everyone is right 83% of the time, but it never finds a single leaver.
  To catch leavers, the model raises some false alarms: 244 of the people
  it flags actually stayed.

**What the model learned.** Because the features were scaled in C, the
logistic regression weights can be compared with each other. The
strongest ones are:

| Feature | Weight |
|---|---:|
| overtime | +1.17 |
| satisfaction | −0.71 |
| remote | −0.58 |
| salary | −0.36 |
| distance_km | +0.32 |

These match the hidden formula in `make_data.py`. Small weights on single
cities (such as `city_Hyderabad` +0.25) are noise: the formula has no city
effect at all.

## Files written (`build/ml/`, not in git; `make ml` recreates them)

| File | What |
|---|---|
| `train.csv`, `test.csv` | the cleaned data: emp_id, 27 features, left |
| `preprocessing_params.csv` | fill value, mean and std learned for each number feature |
| `predictions.csv` | for every test employee: risk probability, prediction, real outcome |
| `metrics.json` | the scores above |
| `model.joblib` | the trained model, for scikit-learn's `joblib.load` |

## What CPandas could not do directly

These were written in `preprocess.c` with the library's own building
blocks instead:

- **Trimming and case-folding text:** no string functions exist, so the
  program builds a new STRING Series cell by cell.
- **Removing duplicates:** there is no `drop_duplicates`. Sorting by the id
  and then keeping the first row of each id does the same job.
- **One-hot encoding:** there is no `get_dummies`. `df_groupby` with no
  aggregations lists the categories, and one computed column is made per
  category.
- **Replacing a column in place:** a computed column is added, the old
  column is removed, and the new one is renamed.
