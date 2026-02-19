#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASES="$SCRIPT_DIR/releases"
CMAKE="$SCRIPT_DIR/CMakeLists.txt"
VERSIONJS="$SCRIPT_DIR/docs/version.js"

current_line() { tail -1 "$RELEASES"; }
current_ver() { current_line | cut -d'|' -f1; }
current_title() {
    local line; line=$(current_line)
    [[ "$line" == *"|"* ]] && echo "${line#*|}" || echo ""
}

apply() {
    local ver="$1"
    sed -i '' "s/project(tccp VERSION [0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*/project(tccp VERSION $ver/" "$CMAKE"
    sed -i '' "s/var TCCP_VERSION = '[^']*'/var TCCP_VERSION = '$ver'/" "$VERSIONJS"
}

bump() {
    local new="$1" title="$2"
    [ -n "$title" ] && echo "${new}|${title}" >> "$RELEASES" || echo "$new" >> "$RELEASES"
    apply "$new"
    [ -n "$title" ] && echo "$new - $title" || echo "$new"
}

cur=$(current_ver)
IFS='.' read -r maj min pat <<< "$cur"

case "$1" in
    M|major)  bump "$((maj + 1)).0.0" "$2" ;;
    m|minor)  bump "$maj.$((min + 1)).0" "$2" ;;
    p|patch)  bump "$maj.$min.$((pat + 1))" "$2" ;;
    revert)
        count=$(wc -l < "$RELEASES" | tr -d ' ')
        [ "$count" -lt 2 ] && echo "Nothing to revert to" && exit 1
        sed -i '' '$d' "$RELEASES"
        apply "$(current_ver)"
        echo "Reverted to $(current_ver)"
        ;;
    "")
        title=$(current_title)
        [ -n "$title" ] && echo "$cur - $title" || echo "$cur"
        ;;
    *)
        echo "Usage: ./rel.sh [M|m|p] [\"title\"]"
        echo "       ./rel.sh revert"
        echo "       ./rel.sh"
        echo ""
        echo "  M  - bump major    m  - bump minor    p  - bump patch"
        echo "  revert - roll back    (no args) - print current"
        echo ""
        echo "  Example: ./rel.sh p \"Windows support\""
        ;;
esac
