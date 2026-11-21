#!/bin/sh
# The GitHub release title of a version: "schneidi x.y.0 – <name>" for a feature release (name from
# tools/release-names.txt), "schneidi x.y.z" for a fix release. Fails for a feature release without a name.
# Usage: gh release create vX.Y.Z --title "$(tools/release_title.sh X.Y.Z)" ...   (or: tools/release_title.sh --check vX.Y.Z
# compares the published release's title with it)
set -e
DIR=$(cd "$(dirname "$0")" && pwd -P)
check=
[ "$1" = "--check" ] && { check=1; shift; }
V=${1#v}
case "$V" in
    *.*.*) ;;
    *) echo "usage: $0 [--check] x.y.z" >&2; exit 2 ;;
esac
case "$V" in
    *.0)
        NAME=$(awk -v v="$V" '$1 == v { $1 = ""; sub(/^ /, ""); print; exit }' "$DIR/release-names.txt")
        if [ -z "$NAME" ]; then
            echo "$V is a feature release without a name: add '$V <Name> Update' to tools/release-names.txt" >&2
            exit 1
        fi
        TITLE="schneidi $V – $NAME" ;;
    *) TITLE="schneidi $V" ;;
esac
if [ -n "$check" ]; then
    NOW=$(gh release view "v$V" -R OnkelTheorie/schneidi --json name -q .name)
    [ "$NOW" = "$TITLE" ] && { echo "ok: $NOW"; exit 0; }
    echo "title is '$NOW', should be '$TITLE':  gh release edit v$V -R OnkelTheorie/schneidi --title \"$TITLE\"" >&2
    exit 1
fi
echo "$TITLE"
