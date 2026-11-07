"""Prints the CPandas and pandas timings side by side.

    python3 bench/compare.py bench_c.txt bench_pandas.txt ROWS
"""
import sys


def read(path):
    times = {}
    order = []
    with open(path) as f:
        for line in f:
            if "|" in line:
                name, seconds = line.strip().split("|")
                times[name] = float(seconds)
                order.append(name)
    return times, order


def main():
    c_times, order = read(sys.argv[1])
    pd_times, _ = read(sys.argv[2])
    print()
    print("%s rows" % format(int(sys.argv[3]), ","))
    print("%-36s %10s %10s %10s" % ("operation", "CPandas", "pandas",
                                    "CPandas/pd"))
    for name in order:
        c = c_times[name]
        line = "%-36s %9.3fs" % (name, c)
        if name in pd_times:
            p = pd_times[name]
            ratio = c / p if p > 0 else float("inf")
            line += " %9.3fs %9.1fx" % (p, ratio)
        print(line)


if __name__ == "__main__":
    main()
