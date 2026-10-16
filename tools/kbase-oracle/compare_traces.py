#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

from normalize_trace import semantic_projection


def differences(left, right, path="$", limit=200):
    found = []

    def visit(a, b, current):
        if len(found) >= limit:
            return
        if type(a) is not type(b):
            found.append({"path": current, "left": a, "right": b})
        elif isinstance(a, dict):
            for key in sorted(set(a) | set(b)):
                if key not in a or key not in b:
                    found.append(
                        {"path": f"{current}.{key}", "left": a.get(key), "right": b.get(key)}
                    )
                else:
                    visit(a[key], b[key], f"{current}.{key}")
        elif isinstance(a, list):
            if len(a) != len(b):
                found.append(
                    {"path": f"{current}.length", "left": len(a), "right": len(b)}
                )
            for index, (left_item, right_item) in enumerate(zip(a, b)):
                visit(left_item, right_item, f"{current}[{index}]")
        elif a != b:
            found.append({"path": current, "left": a, "right": b})

    visit(left, right, path)
    return found


def compare(left, right):
    found = differences(semantic_projection(left), semantic_projection(right))
    return {
        "schema": 1,
        "equal": not found,
        "left_labels": left.get("labels", []),
        "right_labels": right.get("labels", []),
        "difference_count": len(found),
        "differences": found,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("left")
    parser.add_argument("right")
    parser.add_argument("-o", "--output")
    arguments = parser.parse_args()
    left = json.loads(Path(arguments.left).read_text())
    right = json.loads(Path(arguments.right).read_text())
    report = compare(left, right)
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        Path(arguments.output).write_text(rendered)
    else:
        print(rendered, end="")
    raise SystemExit(0 if report["equal"] else 1)


if __name__ == "__main__":
    main()
