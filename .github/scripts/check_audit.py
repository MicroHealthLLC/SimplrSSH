#!/usr/bin/env python3
"""
Audit gate for the Tests workflow: reads the JSON report of `esp-idf-sbom check` (CVEs from
the National Vulnerability Database for ESP-IDF, its bundled libraries and the registry
components in the firmware) and fails on

  - a HIGH or CRITICAL vulnerability that is not in .github/audit-baseline.txt;
  - any vulnerability in CISA's Known Exploited Vulnerabilities list, baseline or not.

MEDIUM/LOW findings and possible matches ("MAYBE") are reported as warnings. Baseline
entries that no longer show up are reported so they can be removed.

Usage: check_audit.py <vulns.json> [baseline]
"""

import json
import os
import re
import sys

BLOCKING = {"CRITICAL", "HIGH"}


def load_baseline(path):
    accepted = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 2)
            if not re.fullmatch(r"CVE-\d{4}-\d{4,}", parts[0]) or len(parts) < 3:
                raise ValueError(f"bad baseline line (need '<CVE> <package> <reason>'): {line}")
            accepted[parts[0]] = parts[2]
    return accepted


def evaluate(records, accepted):
    """(failures, warnings, seen CVE ids) for the report's records."""
    failures, warnings, seen = [], [], set()
    for r in records:
        status = r.get("vulnerable")
        cve = r.get("cve_id") or ""
        if status not in ("YES", "MAYBE") or not cve:
            continue
        seen.add(cve)
        severity = (r.get("cvss_base_severity") or "UNKNOWN").upper()
        label = f"{cve} ({severity} {r.get('cvss_base_score', '')}) in {r.get('pkg_name')} {r.get('pkg_version')}"
        if r.get("kev_added") or r.get("kev_name"):
            failures.append(f"{label} is a known exploited vulnerability")
        elif status == "YES" and severity in BLOCKING and cve not in accepted:
            failures.append(f"{label}: fix it or review it into .github/audit-baseline.txt")
        elif cve not in accepted:
            warnings.append(f"{label}" + (" (possible match)" if status == "MAYBE" else ""))
    return failures, warnings, seen


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    baseline_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(".github", "audit-baseline.txt")
    with open(sys.argv[1], encoding="utf-8") as f:
        report = json.load(f)
    records = report.get("records", [])
    if not records:
        print("::error::The vulnerability report has no records: the SBOM or the NVD query failed")
        return 1
    accepted = load_baseline(baseline_path)
    failures, warnings, seen = evaluate(records, accepted)

    for message in failures:
        print(f"::error::{message}")
    for message in warnings:
        print(f"::warning::{message}")
    for cve in sorted(set(accepted) - seen):
        print(f"::warning file={baseline_path}::{cve} is no longer reported: remove it from the baseline")

    summary = report.get("cves_summary", {})
    lines = ["## Audit (esp-idf-sbom, NVD)", "",
             f"{summary.get('packages_count', len(records))} packages checked; "
             f"{len(failures)} blocking, {len(warnings)} warnings, "
             f"{len(seen & set(accepted))} accepted in the baseline.", ""]
    lines += [f"- :x: {m}" for m in failures] + [f"- :warning: {m}" for m in warnings]
    text = "\n".join(lines) + "\n"
    out = os.environ.get("GITHUB_STEP_SUMMARY")
    if out:
        with open(out, "a") as f:
            f.write(text)
    print(text)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
