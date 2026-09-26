#!/usr/bin/env python3
"""
Turn compiler warnings for this project's own sources (main/) into GitHub
annotations and a step-summary table. Report only: always exits 0.

Usage: report_warnings.py <build-log>
"""

import collections
import os
import re
import sys

# e.g. /app/org/repo/main/ssh_terminal.cpp:123:5: warning: unused variable 'x' [-Wunused-variable]
WARNING_RE = re.compile(r"(?:^|/)(main/[^:\s]+):(\d+):(\d+): warning: (.*?)(?: \[(-W[^\]]+)\])?$")


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("No build log to scan")
        return 0

    warnings = {}
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            match = WARNING_RE.search(line.strip())
            if match:
                path, row, col, message, flag = match.groups()
                warnings[(path, int(row), int(col), message)] = flag or "other"

    by_flag = collections.Counter(warnings.values())
    for (path, row, col, message), flag in sorted(warnings.items()):
        print(f"::warning file={path},line={row},col={col},title=Compiler {flag}::{message}")

    lines = ["## Compiler warnings (main/)", ""]
    if not warnings:
        lines.append("No compiler warnings in project sources. :tada:")
    else:
        lines += [f"**{len(warnings)} warnings** - see the annotations on the Files tab / job log.", "",
                  "| Warning | Count |", "|---|---|"]
        lines += [f"| `{flag}` | {count} |" for flag, count in by_flag.most_common()]
    report = "\n".join(lines) + "\n"

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(report)
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
