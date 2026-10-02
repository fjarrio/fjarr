#!/usr/bin/env bash
# Classifies a push for ci.yml (docs/30-continuous-integration.md#which-changes-run-what):
# prints code=true unless every changed file is on the docs list. Unknown means code.
#
#   changes.sh <base-sha> <head-sha>    the files between two commits (fetched shallowly)
#   changes.sh --classify               the file names on stdin (tests)
set -uo pipefail

docs_only() { # stdin: file names, one per line
  local f n=0
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    n=$((n + 1))
    case "$f" in
      docs/*|website/*|.claude/*) ;;
      */*) return 1 ;;   # a Markdown file inside the tree may be packaged: code
      *.md) ;;
      *) return 1 ;;
    esac
  done
  [ "$n" -gt 0 ]       # no files at all: nothing is known, so code
}

classify() { if docs_only; then echo "code=false"; else echo "code=true"; fi; }

if [ "${1:-}" = "--classify" ]; then classify; exit 0; fi

base=${1:-} head=${2:-}
if [ -z "$base" ] || [ -z "$head" ] || [ "$base" = 0000000000000000000000000000000000000000 ]; then
  echo "changes: no base to compare against (a new branch?): running everything" >&2
  echo "code=true"; exit 0
fi
# A shallow checkout has neither commit's tree; both are fetched (one commit each) when missing.
if ! files=$(git diff --name-only "$base" "$head" 2>/dev/null) \
   && { ! git fetch -q --no-tags --depth=1 origin "$base" "$head" 2>/dev/null \
        || ! files=$(git diff --name-only "$base" "$head" 2>/dev/null); }; then
  echo "changes: cannot diff $base..$head (a force push?): running everything" >&2
  echo "code=true"; exit 0
fi
echo "changes: $(printf '%s\n' "$files" | grep -c .) files" >&2
printf '%s\n' "$files" | sed 's/^/  /' >&2
printf '%s\n' "$files" | classify
