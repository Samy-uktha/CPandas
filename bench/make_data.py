"""Writes the benchmark input files (plain Python, pandas is not needed).

    python3 bench/make_data.py ROWS OUT_DIR

makes OUT_DIR/employees_ROWS.csv and OUT_DIR/bonus_ROWS.csv. The same ROWS
always gives the same files (fixed random seed), so CPandas and pandas read
exactly the same data.
"""
import datetime
import os
import random
import sys

FIRST_NAMES = ["Asha", "Ben", "Chen", "Dev", "Esha", "Farid", "Gita", "Hari",
               "Isha", "Jay", "Kiran", "Leela", "Meera", "Nikhil", "Om",
               "Priya", "Rahul", "Sana", "Tara", "Uma", "Vikram", "Wen",
               "Yash", "Zoya"]
DEPTS = ["Sales", "IT", "HR", "Ops", "Finance", "Legal", "Support",
         "Research", "Design", "Marketing"]
CITIES = ["Pune", "Delhi", "Mumbai", "Chennai", "Kolkata", "Bengaluru",
          "Hyderabad", "Jaipur"]


def main():
    rows = int(sys.argv[1])
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    rng = random.Random(rows)
    start = datetime.date(2015, 1, 1)

    with open(os.path.join(out_dir, "employees_%d.csv" % rows), "w",
              newline="\n") as f:
        f.write("id,name,dept,city,age,salary,rating,joined,remote\n")
        for i in range(rows):
            name = "%s %d" % (rng.choice(FIRST_NAMES), rng.randrange(1000))
            salary = "" if rng.random() < 0.05 else str(rng.randrange(20000, 150000))
            rating = "" if rng.random() < 0.05 else "%.1f" % rng.uniform(1, 5)
            joined = start + datetime.timedelta(days=rng.randrange(3650))
            remote = "True" if rng.random() < 0.3 else "False"
            f.write("%d,%s,%s,%s,%d,%s,%s,%s,%s\n" % (
                i, name, rng.choice(DEPTS), rng.choice(CITIES),
                rng.randrange(21, 65), salary, rating, joined.isoformat(),
                remote))

    # One bonus row for 90% of the ids, in random order: joining on "id"
    # pairs up two big tables.
    ids = [i for i in range(rows) if rng.random() < 0.9]
    rng.shuffle(ids)
    with open(os.path.join(out_dir, "bonus_%d.csv" % rows), "w",
              newline="\n") as f:
        f.write("id,bonus\n")
        for i in ids:
            f.write("%d,%d\n" % (i, rng.randrange(0, 20000)))


if __name__ == "__main__":
    main()
