#!/usr/bin/env python3
"""
Report outdated dependencies (report only, never fails the build).

Compares the versions pinned in dependencies.lock / main/idf_component.yml against
the latest releases on the ESP Component Registry, and the ESP-IDF version used by
CI against the latest ESP-IDF release on GitHub. Writes a Markdown table to
$GITHUB_STEP_SUMMARY (or stdout) and a GitHub warning annotation per outdated item.
"""

import json
import os
import re
import sys
import urllib.request

import yaml

REGISTRY_API = "https://components.espressif.com/api/components/{}"
IDF_RELEASE_API = "https://api.github.com/repos/espressif/esp-idf/releases/latest"


def fetch_json(url):
    request = urllib.request.Request(url, headers={"User-Agent": "simplrssh-dependency-check"})
    token = os.environ.get("GITHUB_TOKEN")
    if token and "api.github.com" in url:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def version_key(version):
    """Sort key for registry versions like '9.6.0~1' (the ~N suffix is a packaging revision)."""
    base, _, revision = str(version).lstrip("v").partition("~")
    numbers = [int(n) for n in re.findall(r"\d+", base)[:3]]
    numbers += [0] * (3 - len(numbers))
    prerelease = 0 if re.search(r"[a-zA-Z]", base) else 1
    return numbers + [prerelease, int(revision or 0)]


def latest_registry_version(name):
    data = fetch_json(REGISTRY_API.format(name))
    versions = [v["version"] for v in data.get("versions", []) if not v.get("yanked_at")]
    stable = [v for v in versions if not re.search(r"[a-zA-Z]", v.split("~")[0])]
    return max(stable or versions, key=version_key) if versions else None


def ci_idf_version():
    """ESP-IDF version the build workflow uses."""
    workflow = os.path.join(".github", "workflows", "build.yml")
    try:
        with open(workflow) as f:
            match = re.search(r"esp_idf_version:\s*['\"]?(v[\d.]+)", f.read())
            return match.group(1) if match else None
    except OSError:
        return None


def main():
    with open("dependencies.lock") as f:
        lock = yaml.safe_load(f)
    with open(os.path.join("main", "idf_component.yml")) as f:
        manifest = yaml.safe_load(f)

    direct = set(manifest.get("dependencies", {}))
    rows = []
    outdated = 0

    for name, info in sorted(lock.get("dependencies", {}).items()):
        if name == "idf" or info.get("source", {}).get("type") != "service":
            continue
        current = str(info.get("version"))
        try:
            latest = latest_registry_version(name)
        except Exception as e:  # Registry hiccups must not break the report
            rows.append((name, current, f"lookup failed: {e}", "?", name in direct))
            continue
        is_outdated = latest is not None and version_key(latest) > version_key(current)
        outdated += is_outdated
        rows.append((name, current, latest, "update available" if is_outdated else "up to date", name in direct))
        if is_outdated:
            where = "main/idf_component.yml" if name in direct else "dependencies.lock"
            kind = "" if name in direct else " (transitive dependency)"
            print(f"::warning file={where}::{name} {current} is outdated{kind}; latest is {latest}")

    idf_current = ci_idf_version()
    try:
        idf_latest = fetch_json(IDF_RELEASE_API)["tag_name"]
    except Exception as e:
        idf_latest = f"lookup failed: {e}"
    if idf_current:
        idf_outdated = not idf_latest.startswith("lookup") and version_key(idf_latest) > version_key(idf_current)
        outdated += idf_outdated
        rows.insert(0, ("ESP-IDF (framework)", idf_current, idf_latest,
                        "update available" if idf_outdated else "up to date", True))
        if idf_outdated:
            print(f"::warning file=.github/workflows/build.yml::ESP-IDF {idf_current} is outdated; latest is {idf_latest}")

    lines = [
        "## Dependency check",
        "",
        f"**{outdated} outdated** (report only - updating is a deliberate change: bump the pin, "
        "run `idf.py update-dependencies`, build, test on the device, commit the new `dependencies.lock`).",
        "",
        "| Dependency | Pinned | Latest | Status | Direct |",
        "|---|---|---|---|---|",
    ]
    for name, current, latest, status, is_direct in rows:
        icon = ":warning: " if status == "update available" else ""
        lines.append(f"| {name} | {current} | {latest} | {icon}{status} | {'yes' if is_direct else 'no'} |")
    report = "\n".join(lines) + "\n"

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(report)
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
