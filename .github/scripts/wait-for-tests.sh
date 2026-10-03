#!/usr/bin/env bash
#
# First stage of a release (build.yml, release job). Tests (tests.yml) runs on its own for
# every push; this waits for THAT run - the one for exactly this commit - and passes only
# when it succeeded. Tests is never run a second time, and no firmware is published from a
# commit that did not pass it. (Same approach as TueriCXO's wait-for-tests.sh.)
#
# Env: GH_TOKEN (actions: read), REPO (owner/name), SHA (the commit being released).
# A failed, cancelled or missing Tests run fails this stage. Re-run the failed Tests jobs,
# then re-run the release workflow: this reads the run's latest attempt.
set -euo pipefail

: "${GH_TOKEN:?}" "${REPO:?}" "${SHA:?}"
FIND_ATTEMPTS="${FIND_ATTEMPTS:-30}"   # x 10 s: the push run is created within seconds
POLL_SECONDS="${POLL_SECONDS:-30}"

runs_url="repos/${REPO}/actions/workflows/tests.yml/runs?event=push&head_sha=${SHA}&per_page=1"

run_id=""
for _ in $(seq 1 "$FIND_ATTEMPTS"); do
    run_id="$(gh api "$runs_url" --jq '.workflow_runs[0].id // empty')"
    [ -n "$run_id" ] && break
    sleep 10
done
if [ -z "$run_id" ]; then
    echo "::error::No Tests run was started for ${SHA}. Push the commit to a branch first (Tests runs on every push)."
    exit 1
fi

run_url="https://github.com/${REPO}/actions/runs/${run_id}"
echo "Waiting for Tests: ${run_url}"

while :; do
    read -r status conclusion < <(gh api "repos/${REPO}/actions/runs/${run_id}" --jq '"\(.status) \(.conclusion // "none")"')
    [ "$status" = "completed" ] && break
    echo "  Tests is ${status}..."
    sleep "$POLL_SECONDS"
done

if [ "$conclusion" != "success" ]; then
    echo "::error::Tests finished '${conclusion}' for ${SHA}: ${run_url}"
    exit 1
fi
echo "Tests passed for ${SHA}: ${run_url}"
