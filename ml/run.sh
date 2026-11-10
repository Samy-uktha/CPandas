#!/bin/sh
# Runs the whole attrition pipeline from the project folder (make ml):
#   1. CPandas cleans ml/data/raw/*.csv into build/ml/train.csv and test.csv
#   2. scikit-learn trains and tests a model on them
# The Python step needs pandas and scikit-learn: .venv/bin/python, or set
# PYTHON=/path/to/python.
set -e
# The Python to use: $PYTHON if set, else the project's .venv, else the
# older build/venv (which `make clean` deletes), else the system python3
# (for example with pandas and scikit-learn installed through apt).
if [ -n "$PYTHON" ]; then
    PY=$PYTHON
elif [ -x .venv/bin/python ]; then
    PY=.venv/bin/python
elif [ -x build/venv/bin/python ]; then
    PY=build/venv/bin/python
else
    PY=python3
fi
mkdir -p build/ml

echo "=== Step 1: preprocessing with CPandas (C) ==="
./build/ml_preprocess ml/data/raw build/ml

echo
echo "=== Step 2: model with scikit-learn (Python) ==="
if ! "$PY" -c "import sklearn, pandas" 2>/dev/null; then
    echo "scikit-learn and pandas are needed for this step (looked in $PY)."
    echo "The cleaned files are in build/ml/ already."
    exit 1
fi
"$PY" ml/train.py build/ml
