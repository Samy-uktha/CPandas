"""Generates the raw CSV files for the attrition pipeline.

    python3 ml/make_data.py [OUT_DIR]        (default ml/data/raw)

Plain Python, no pandas needed. A fixed random seed makes the files the
same every time.

The data is made up, but it is made the way real HR exports are messy:
  - department names typed in different ways ("Sales", "sales", "SALES"),
  - missing cells, and missing-value words ("N/A", "null"),
  - impossible ages (0, 150, -3),
  - the same employee exported twice,
  - reviews for employees that are not in the employee file,
  - a few employees whose outcome ("left") is unknown.

Whether an employee left is decided by a hidden formula (overtime, low
satisfaction, low pay for their department, short tenure, bad reviews, a
long commute, ...) plus randomness, so a model can learn real patterns
but cannot be perfect.
"""
import datetime
import math
import os
import random
import sys

SNAPSHOT = datetime.date(2026, 1, 1)   # "today" for this export

DEPTS = {  # name: (base salary, effect on leaving, budget per head, size)
    "Sales": (52000, 0.35, 41000, 14),
    "Engineering": (78000, -0.25, 65000, 9),
    "HR": (50000, 0.0, 38000, 6),
    "Support": (40000, 0.55, 30000, 18),
    "Finance": (68000, -0.15, 52000, 7),
    "Marketing": (56000, 0.15, 45000, 8),
}
CITIES = ["Pune", "Mumbai", "Delhi", "Bengaluru", "Chennai", "Hyderabad"]
FIRST = ["Asha", "Ben", "Chen", "Dev", "Esha", "Farid", "Gita", "Hari",
         "Isha", "Jay", "Kiran", "Leela", "Meera", "Nikhil", "Om", "Priya",
         "Rahul", "Sana", "Tara", "Uma", "Vikram", "Wen", "Yash", "Zoya"]
LAST = ["Rao", "Shah", "Iyer", "Khan", "Das", "Nair", "Patel", "Singh",
        "Gupta", "Menon", "Bose", "Joshi"]

N_EMPLOYEES = 6000


def messy_dept(name, rng):
    """The department as a person might have typed it."""
    r = rng.random()
    if r < 0.70:
        return name
    if r < 0.82:
        return name.lower()
    if r < 0.92:
        return name.upper()
    return " " + name.lower() + " "     # extra spaces, written in quotes


def maybe(value, rng, p_missing, missing_words=("",)):
    """value, or (with probability p_missing) one way of writing 'missing'."""
    if rng.random() < p_missing:
        return rng.choice(missing_words)
    return value


def quoted(text):
    return '"' + text + '"' if text != text.strip() else text


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "ml/data/raw"
    os.makedirs(out_dir, exist_ok=True)
    rng = random.Random(42)

    employees = []
    for k in range(N_EMPLOYEES):
        dept = rng.choice(list(DEPTS))
        base, dept_effect, _, _ = DEPTS[dept]
        age = rng.randint(21, 60)
        hired = SNAPSHOT - datetime.timedelta(days=rng.randint(30, 14 * 365))
        tenure = (SNAPSHOT - hired).days / 365.25
        level = rng.gauss(0, 1)
        salary = base * (1 + 0.012 * (age - 21) + 0.04 * tenure + 0.15 * level)
        salary = int(round(max(salary, 18000), -2))
        remote = rng.random() < 0.3
        overtime = rng.random() < 0.28
        satisfaction = min(5.0, max(1.0, rng.gauss(3.4 - 0.6 * overtime, 0.9)))
        distance = round(max(0.5, rng.expovariate(1 / 12)), 1)
        review_mean = min(5.0, max(1.0, rng.gauss(3.5, 0.7)))

        # The hidden formula for leaving.
        pay_ratio = salary / (base * (1 + 0.012 * (age - 21) + 0.04 * tenure))
        logit = (-1.6 + 1.2 * overtime - 0.8 * (satisfaction - 3.4)
                 - 2.5 * (pay_ratio - 1.0) - 0.12 * min(tenure, 10)
                 - 0.7 * (review_mean - 3.5) - 0.5 * remote
                 + 0.025 * (distance - 12) + dept_effect
                 - 0.02 * (age - 35))
        left = rng.random() < 1 / (1 + math.exp(-logit))

        employees.append({
            "emp_id": 1001 + k,
            "name": rng.choice(FIRST) + " " + rng.choice(LAST),
            "dept": maybe(quoted(messy_dept(dept, rng)), rng, 0.01),
            "city": maybe(rng.choice(CITIES), rng, 0.03),
            "age": maybe(str(rng.choice([0, 150, -3])) if rng.random() < 0.012
                         else str(age), rng, 0.02),
            "salary": maybe(str(salary), rng, 0.04, ("", "N/A")),
            "hire_date": maybe(hired.isoformat(), rng, 0.02),
            "remote": "True" if remote else "False",
            "overtime": "True" if overtime else "False",
            "satisfaction": maybe("%.1f" % satisfaction, rng, 0.06,
                                  ("", "N/A", "null")),
            "distance_km": maybe("%.1f" % distance, rng, 0.02),
            "left": maybe("True" if left else "False", rng, 0.015),
            "_review_mean": review_mean,
            "_hired": hired,
        })

    # The same employee exported twice (identical row) for about 2%.
    rows = list(employees)
    for e in rng.sample(employees, 120):
        rows.insert(rng.randrange(len(rows)), e)

    cols = ["emp_id", "name", "dept", "city", "age", "salary", "hire_date",
            "remote", "overtime", "satisfaction", "distance_km", "left"]
    with open(os.path.join(out_dir, "employees.csv"), "w", newline="\n") as f:
        f.write(",".join(cols) + "\n")
        for e in rows:
            f.write(",".join(str(e[c]) for c in cols) + "\n")

    # Performance reviews: 0 to 6 per employee, after their hire date,
    # around their hidden review mean. Plus some for unknown employees.
    with open(os.path.join(out_dir, "reviews.csv"), "w", newline="\n") as f:
        f.write("emp_id,review_date,score,reviewer\n")
        for e in employees:
            for _ in range(rng.randint(0, 6)):
                days = (SNAPSHOT - e["_hired"]).days
                when = e["_hired"] + datetime.timedelta(days=rng.randint(0, days))
                score = int(min(5, max(1, round(rng.gauss(e["_review_mean"], 0.8)))))
                f.write("%d,%s,%d,%s\n" % (e["emp_id"], when.isoformat(), score,
                                           rng.choice(LAST)))
        for _ in range(40):
            f.write("%d,2024-06-30,%d,%s\n" % (rng.randint(20000, 29999),
                                             rng.randint(1, 5), rng.choice(LAST)))

    with open(os.path.join(out_dir, "departments.csv"), "w", newline="\n") as f:
        f.write("dept,budget_per_head,avg_team_size,floor\n")
        for k, (name, (_, _, budget, size)) in enumerate(DEPTS.items()):
            f.write("%s,%d,%d,%d\n" % (name, budget, size, k + 1))


if __name__ == "__main__":
    main()
