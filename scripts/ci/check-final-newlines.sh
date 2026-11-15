#!/usr/bin/env bash
# scripts/ci/check-final-newlines.sh [<path>...] - verify that text files end in
# a newline. With no arguments it checks everything that differs from HEAD in the
# working tree (modified + untracked), which is what a pre-commit look at a patch
# wants.
#
# Binary files (anything git considers binary, plus the usual suspects) are
# skipped: a "text file must end in \n" rule only makes sense for text.
#
#   scripts/ci/check-final-newlines.sh                 # default: local changes
#   scripts/ci/check-final-newlines.sh app/src/main    # only these paths
#
# Exit status: 0 when every checked file ends in a newline, 1 otherwise (each
# offending file is printed), 2 on a usage/environment error.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

collect() {
    if [ "$#" -gt 0 ]; then
        for path in "$@"; do
            if [ -d "$path" ]; then
                find "$path" -type f
            elif [ -f "$path" ]; then
                printf '%s\n' "$path"
            else
                echo "no such path: $path" >&2
                exit 2
            fi
        done
    else
        git diff --name-only HEAD
        git ls-files --others --exclude-standard
    fi
}

is_binary() {
    case "$1" in
        *.a | *.so | *.o | *.png | *.jpg | *.jpeg | *.webp | *.gif | *.ico | *.ttf | *.otf) return 0 ;;
        *.zip | *.jar | *.apk | *.npy | *.bin) return 0 ;;
    esac
    local bytes text
    bytes=$(head -c 8000 -- "$1" | wc -c)
    text=$(head -c 8000 -- "$1" | LC_ALL=C tr -d '\000' | wc -c)
    [ "$bytes" -ne "$text" ]
}

missing=0
checked=0
while IFS= read -r file; do
    [ -f "$file" ] || continue
    [ -s "$file" ] || continue
    if is_binary "$file"; then
        continue
    fi
    checked=$((checked + 1))
    if [ -n "$(tail -c 1 -- "$file")" ]; then
        printf 'MISSING-FINAL-NEWLINE %s\n' "$file"
        missing=$((missing + 1))
    fi
done < <(collect "$@" | LC_ALL=C sort -u)

# `tail -c 1` prints the last byte; command substitution strips trailing
# newlines, so an empty result means the byte was "\n".
if [ "$missing" -ne 0 ]; then
    printf '%d of %d file(s) do not end with a newline\n' "$missing" "$checked" >&2
    exit 1
fi
printf 'OK: %d text file(s) end with a newline\n' "$checked"
