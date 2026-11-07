#!/bin/sh
# Runs the benchmark from the project folder:
#     sh bench/run.sh [ROWS]        (default 1000000 rows)
# 1. makes the test files in build/bench_data (only the first time),
# 2. times CPandas (build/bench, compiled with -O2 and no sanitizers),
# 3. times pandas on the same files, if a Python with pandas is found
#    (build/venv/bin/python, or set PYTHON=/path/to/python),
# 4. prints both side by side.
set -e
ROWS=${1:-1000000}
DATA=build/bench_data
PY=${PYTHON:-build/venv/bin/python}

if [ ! -f "$DATA/employees_$ROWS.csv" ]; then
    echo "Making $ROWS rows of test data..."
    python3 bench/make_data.py "$ROWS" "$DATA"
fi

echo "Timing CPandas..."
./build/bench "$DATA/employees_$ROWS.csv" "$DATA/bonus_$ROWS.csv" > build/bench_c.txt

if "$PY" -c "import pandas" 2>/dev/null; then
    echo "Timing pandas..."
    "$PY" bench/bench_pandas.py "$DATA/employees_$ROWS.csv" "$DATA/bonus_$ROWS.csv" > build/bench_pandas.txt
else
    echo "(pandas not found with $PY, showing CPandas only)"
    : > build/bench_pandas.txt
fi

python3 bench/compare.py build/bench_c.txt build/bench_pandas.txt "$ROWS"
