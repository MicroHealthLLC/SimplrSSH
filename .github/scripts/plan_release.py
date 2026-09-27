#!/usr/bin/env python3
"""
Decide the firmware version for a CI run and whether to publish a release.

Versions come from git tags "vX.Y.Z". The next release is the latest tag with the
patch number (default), minor or major incremented. PROJECT_VER in CMakeLists.txt
is a floor: it is used for the very first release, and a higher value there wins
over the automatic bump (to jump to a new minor/major from code).

  push to main              -> release next patch
  push of tag vX.Y.Z        -> release X.Y.Z
  manual run (dispatch)     -> release on the chosen branch with the chosen bump or
                               exact version; branches other than main are pre-releases
  push to other branches,
  pull requests             -> build only, version "<next patch>-dev.<sha>"

Inputs come from the GitHub environment (GITHUB_EVENT_NAME, GITHUB_REF, GITHUB_REF_TYPE,
GITHUB_REF_NAME, GITHUB_SHA) plus INPUT_BRANCH, INPUT_BUMP, INPUT_VERSION for manual runs.
Existing tags are read with `git ls-remote --tags origin` (or --tags-file for tests).
Outputs (version, tag, release, prerelease) go to $GITHUB_OUTPUT.
"""

import argparse
import os
import re
import subprocess
import sys

SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def fail(message):
    """Print a GitHub error annotation (read from stdout) and stop."""
    print(f"::error::{message}")
    sys.exit(1)


def parse(version):
    m = SEMVER.match(version or "")
    return tuple(int(x) for x in m.groups()) if m else None


def fmt(v):
    return "%d.%d.%d" % v


def bump(v, kind):
    major, minor, patch = v
    if kind == "major":
        return (major + 1, 0, 0)
    if kind == "minor":
        return (major, minor + 1, 0)
    return (major, minor, patch + 1)


def release_tags(ls_remote_output):
    tags = set()
    for line in ls_remote_output.splitlines():
        ref = line.split()[-1] if line.strip() else ""
        m = re.match(r"^refs/tags/v(\d+\.\d+\.\d+)(\^\{\})?$", ref)
        if m:
            tags.add(parse(m.group(1)))
    return tags


def project_ver(cmakelists):
    with open(cmakelists) as f:
        m = re.search(r'set\(PROJECT_VER\s+"(\d+\.\d+\.\d+)"\)', f.read())
    if not m:
        fail("PROJECT_VER fallback not found in CMakeLists.txt")
    return parse(m.group(1))


def next_version(tags, floor, kind):
    if not tags:
        return floor                          # First release
    latest = max(tags)
    candidate = bump(latest, kind)
    return max(candidate, floor) if floor > latest else candidate


def plan(env, tags, floor):
    event = env.get("GITHUB_EVENT_NAME", "")
    ref = env.get("GITHUB_REF", "")
    ref_type = env.get("GITHUB_REF_TYPE", "")
    ref_name = env.get("GITHUB_REF_NAME", "")
    sha = env.get("GITHUB_SHA", "0000000")[:7]
    kind = (env.get("INPUT_BUMP") or "patch").lower()
    explicit = (env.get("INPUT_VERSION") or "").strip().lstrip("v")
    branch = (env.get("INPUT_BRANCH") or "main").strip()

    if kind not in ("patch", "minor", "major"):
        fail(f"Unknown bump '{kind}' (use patch, minor or major)")

    if event == "push" and ref_type == "tag":
        v = parse(ref_name.lstrip("v"))
        if not v or not ref_name.startswith("v"):
            fail(f"Tag {ref_name} is not a release tag like v1.4.0")
        return {"version": fmt(v), "release": True, "prerelease": False, "reason": f"tag {ref_name} pushed"}

    if event == "workflow_dispatch":
        if explicit:
            v = parse(explicit)
            if not v:
                fail(f"Version '{explicit}' must look like 1.4.0")
            if v in tags:
                fail(f"v{explicit} is already released; pick another version")
            if tags and v < max(tags):
                print(f"::warning::v{explicit} is lower than the latest release v{fmt(max(tags))}")
        else:
            v = next_version(tags, floor, kind)
        return {"version": fmt(v), "release": True, "prerelease": branch != "main",
                "reason": f"manual run on {branch}" + (f", version {explicit}" if explicit else f", {kind} bump")}

    if event == "push" and ref == "refs/heads/main":
        v = next_version(tags, floor, "patch")
        return {"version": fmt(v), "release": True, "prerelease": False, "reason": "push to main"}

    v = next_version(tags, floor, "patch")
    return {"version": f"{fmt(v)}-dev.{sha}", "release": False, "prerelease": False,
            "reason": f"{event} on {ref or 'unknown ref'} (build only)"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmakelists", default="CMakeLists.txt")
    ap.add_argument("--tags-file", help="ls-remote style tag list (tests)")
    args = ap.parse_args()

    if args.tags_file:
        with open(args.tags_file) as f:
            listing = f.read()
    else:
        listing = subprocess.run(["git", "ls-remote", "--tags", "origin"], check=True,
                                 capture_output=True, text=True).stdout
    tags = release_tags(listing)
    result = plan(os.environ, tags, project_ver(args.cmakelists))

    latest = "v" + fmt(max(tags)) if tags else "none"
    print(f"Latest release: {latest}; {result['reason']} -> v{result['version']} "
          f"(release: {result['release']}, pre-release: {result['prerelease']})")

    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as f:
            f.write(f"version={result['version']}\n")
            f.write(f"tag=v{result['version']}\n")
            f.write(f"release={'true' if result['release'] else 'false'}\n")
            f.write(f"prerelease={'true' if result['prerelease'] else 'false'}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
