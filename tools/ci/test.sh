#!/usr/bin/env bash
# Tests for tools/ci (docs/30-continuous-integration.md): the change classifier and the image hash's inputs.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
fails=0
check() { # name expected actual
  if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$2', got '$3'"; fails=$((fails + 1)); fi
}
classify() { printf '%s\n' "$@" | bash tools/ci/changes.sh --classify; }

check "docs only" code=false "$(classify docs/30-continuous-integration.md docs/adr/0001-x.md)"
check "root markdown and .claude" code=false "$(classify CLAUDE.md README.md .claude/skills/x/SKILL.md)"
check "website" code=false "$(classify website/src/pages/index.astro)"
check "a spec and the agent" code=true "$(classify docs/08-protocol.md agent/src/core/x.cpp)"
check "markdown inside the tree is code" code=true "$(classify packaging/README.md)"
check "a workflow is code" code=true "$(classify .github/workflows/ci.yml)"
check "a root file other than markdown is code" code=true "$(classify Makefile)"
check "no files is code" code=true "$(classify)"
check "new branch is code" code=true "$(bash tools/ci/changes.sh 0000000000000000000000000000000000000000 HEAD 2>/dev/null)"
check "an unreadable range is code" code=true "$(bash tools/ci/changes.sh deadbeefdeadbeefdeadbeefdeadbeefdeadbeef HEAD 2>/dev/null)"

src() { python3 -c "import sys; sys.path.insert(0, 'tools/ci'); import image; print(' '.join(image.copy_sources(sys.stdin.read())))"; }
check "COPY sources" "a b" "$(printf 'FROM x\nCOPY a b /dst/\n' | src)"
check "flags are not sources" "s.sh" "$(printf 'COPY --chmod=0755 s.sh /usr/local/bin/s\n' | src)"
check "COPY --from reads a stage" "" "$(printf 'COPY --from=build /out /usr/bin/out\n' | src)"
check "continuation lines and ADD" "x y" "$(printf 'ADD x \\\n  y /dst/\n' | src)"
check "JSON form" "a" "$(printf 'COPY ["a", "/dst"]\n' | src)"
tag() { python3 tools/ci/image.py tag "$@"; }
t1=$(tag docker/robot-sim docker/robot-sim/Dockerfile)
check "the tag is stable" "$t1" "$(tag docker/robot-sim docker/robot-sim/Dockerfile)"
t2=$(tag docker/robot-sim docker/robot-sim/Dockerfile USER_UID=1001)
differs() { if [ "$1" != "$2" ]; then echo yes; else echo no; fi; }
check "build arguments change the tag" yes "$(differs "$t1" "$t2")"
t3=$(tag docker/robot-services docker/robot-services/Dockerfile)
check "COPY'd files change the tag" yes "$(differs "$t1" "$t3")"

if [ "$fails" -ne 0 ]; then echo "tools/ci: $fails failed"; exit 1; fi
echo "tools/ci: all passed"
