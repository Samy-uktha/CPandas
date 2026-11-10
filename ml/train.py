"""Step 2 of the attrition pipeline: train and test a model with scikit-learn
on the files that CPandas made (ml/preprocess.c).

    python ml/train.py [DIR]          (default build/ml)

Reads   DIR/train.csv, DIR/test.csv   (all cleaning, filling, encoding and
                                       scaling was already done in C, so
                                       nothing is preprocessed here)
Writes  DIR/predictions.csv           emp_id, probability, predicted, actual
        DIR/metrics.json              the test scores of the chosen model
        DIR/model.joblib              the trained model

What it does:
  1. compares three models with 5-fold cross-validation on the TRAINING
     rows only (a "majority class" baseline, logistic regression and a
     random forest), scored by ROC AUC;
  2. trains the best one on all training rows;
  3. scores it once on the test rows, which were not used for anything
     before this point.
"""
import json
import os
import sys

import joblib
import pandas as pd
from sklearn.dummy import DummyClassifier
from sklearn.ensemble import RandomForestClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import (accuracy_score, confusion_matrix, f1_score,
                             precision_score, recall_score, roc_auc_score)
from sklearn.model_selection import StratifiedKFold, cross_val_score

SEED = 0


def load(path):
    df = pd.read_csv(path)
    X = df.drop(columns=["emp_id", "left"])
    y = df["left"]
    return df["emp_id"], X, y


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "build/ml"
    _, X_train, y_train = load(os.path.join(folder, "train.csv"))
    test_ids, X_test, y_test = load(os.path.join(folder, "test.csv"))
    print("train: %d rows, test: %d rows, %d features"
          % (len(X_train), len(X_test), X_train.shape[1]))
    assert not X_train.isna().any().any(), "CPandas should have filled every cell"

    # "balanced" weights: only about 1 in 6 employees leaves, so without
    # them a model can score well by mostly saying "stays".
    models = {
        "baseline (always 'stays')": DummyClassifier(strategy="most_frequent"),
        "logistic regression": LogisticRegression(
            class_weight="balanced", max_iter=2000, random_state=SEED),
        "random forest": RandomForestClassifier(
            n_estimators=300, min_samples_leaf=5,
            class_weight="balanced_subsample", n_jobs=-1, random_state=SEED),
    }

    print("\n1. Cross-validation on the training rows (ROC AUC, 5 folds)")
    folds = StratifiedKFold(n_splits=5, shuffle=True, random_state=SEED)
    cv_scores = {}
    for name, model in models.items():
        scores = cross_val_score(model, X_train, y_train, cv=folds,
                                 scoring="roc_auc")
        cv_scores[name] = scores.mean()
        print("   %-28s %.3f  (+/- %.3f)" % (name, scores.mean(), scores.std()))

    best_name = max(cv_scores, key=cv_scores.get)
    best = models[best_name]
    print("\n2. Training the best model on all training rows: %s" % best_name)
    best.fit(X_train, y_train)

    print("\n3. Test set (used once, only now)")
    probability = best.predict_proba(X_test)[:, 1]
    predicted = (probability >= 0.5).astype(int)
    metrics = {
        "model": best_name,
        "cv_roc_auc": round(cv_scores[best_name], 4),
        "test_roc_auc": round(roc_auc_score(y_test, probability), 4),
        "test_accuracy": round(accuracy_score(y_test, predicted), 4),
        "test_precision": round(precision_score(y_test, predicted), 4),
        "test_recall": round(recall_score(y_test, predicted), 4),
        "test_f1": round(f1_score(y_test, predicted), 4),
        "baseline_accuracy": round(1 - y_test.mean(), 4),
    }
    for key in ["test_roc_auc", "test_accuracy", "test_precision",
                "test_recall", "test_f1", "baseline_accuracy"]:
        print("   %-20s %.3f" % (key, metrics[key]))

    tn, fp, fn, tp = confusion_matrix(y_test, predicted).ravel()
    metrics["confusion_matrix"] = {"tn": int(tn), "fp": int(fp),
                                   "fn": int(fn), "tp": int(tp)}
    print("\n   confusion matrix     predicted stays   predicted leaves")
    print("   actually stayed      %15d   %16d" % (tn, fp))
    print("   actually left        %15d   %16d" % (fn, tp))

    # What the model relies on most. The features were scaled in C, so the
    # logistic regression weights can be compared with each other.
    print("\n4. Strongest signals")
    if isinstance(best, LogisticRegression):
        weights = pd.Series(best.coef_[0], index=X_train.columns)
        label = "weight (+ means more likely to leave)"
    else:
        weights = pd.Series(best.feature_importances_, index=X_train.columns)
        label = "importance"
    top = weights.reindex(weights.abs().sort_values(ascending=False).index)[:8]
    print("   %-22s %s" % ("feature", label))
    for feature, value in top.items():
        print("   %-22s %+.3f" % (feature, value))
    metrics["top_features"] = {k: round(float(v), 4) for k, v in top.items()}

    pd.DataFrame({"emp_id": test_ids, "probability": probability.round(4),
                  "predicted": predicted, "actual": y_test}).to_csv(
        os.path.join(folder, "predictions.csv"), index=False)
    with open(os.path.join(folder, "metrics.json"), "w") as f:
        json.dump(metrics, f, indent=2)
    joblib.dump(best, os.path.join(folder, "model.joblib"))
    print("\n   wrote predictions.csv, metrics.json, model.joblib in %s" % folder)


if __name__ == "__main__":
    main()
