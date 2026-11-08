#!/bin/sh
# Release gate: succeeds only when the GitHub CI workflow passed for the given commit (default HEAD).
# Usage: tools/ci_green.sh [commit]   (release scripts call it before tagging)
set -e
C=$(git rev-parse "${1:-HEAD}")
R=$(gh run list -R OnkelTheorie/schneidi --workflow ci.yml --commit "$C" -L 1 --json status,conclusion,url \
    -q '.[0] | "\(.status) \(.conclusion) \(.url)"')
case "$R" in
    "completed success "*) echo "CI green for ${C%"${C#????????}"}"; exit 0 ;;
    "") echo "No CI run for $C - push it first" >&2 ;;
    *) echo "CI not green for $C: $R" >&2 ;;
esac
exit 1
