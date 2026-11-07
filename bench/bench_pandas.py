"""The same operations as bench/bench.c, done with pandas.

    python bench/bench_pandas.py employees.csv bonus.csv

Prints "operation|seconds" lines with the same operation names as bench.c.
"""
import os
import sys
import time

import pandas as pd

OLD_JOIN_ROWS = 20000


def timed(label, work):
    start = time.perf_counter()
    result = work()
    print("%s|%.4f" % (label, time.perf_counter() - start), flush=True)
    return result


def main():
    employees_path, bonus_path = sys.argv[1], sys.argv[2]
    # parse_dates: CPandas recognises the date column by itself, pandas
    # has to be told.
    df = timed("read_csv employees",
               lambda: pd.read_csv(employees_path, parse_dates=["joined"]))
    bonus = timed("read_csv bonus", lambda: pd.read_csv(bonus_path))

    timed("filter salary > 50000", lambda: df[df["salary"] > 50000])
    timed("sort dept, salary desc",
          lambda: df.sort_values(["dept", "salary"], ascending=[True, False],
                                 kind="stable"))
    timed("groupby dept",
          lambda: df.groupby("dept").agg(salary_mean=("salary", "mean"),
                                         salary_count=("salary", "count")))
    timed("groupby dept, city",
          lambda: df.groupby(["dept", "city"]).agg(
              salary_mean=("salary", "mean")))
    timed("groupby city, nunique name",
          lambda: df.groupby("city")["name"].nunique())
    timed("merge on id (inner)",
          lambda: pd.merge(df, bonus, on="id", how="inner"))
    timed("describe", lambda: df.describe())

    copy = df.copy()

    def add_tax():
        copy["tax"] = copy["salary"] * 0.1
    timed("new column salary * 0.1", add_tax)
    timed("concat with itself", lambda: pd.concat([df, df]))
    timed("write_csv", lambda: df.to_csv("build/bench_out_pandas.csv",
                                         index=False))
    os.remove("build/bench_out_pandas.csv")

    def chunked():
        total = 0.0
        for chunk in pd.read_csv(employees_path, chunksize=100000):
            total += chunk["salary"].sum()
        return total
    timed("read in 100k-row chunks", chunked)

    small_left = df.head(OLD_JOIN_ROWS)
    small_right = bonus.head(OLD_JOIN_ROWS)
    timed("join 20k x 20k, hash (new)",
          lambda: pd.merge(small_left, small_right, on="id"))


if __name__ == "__main__":
    main()
