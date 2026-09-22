# Fjarr — single entry point for every task.
# Build/lint targets assume the dev container (docs/12); compose targets run
# anywhere Docker does. .vscode/tasks.json wraps these same targets.
.DEFAULT_GOAL := help

# ------------------------------------------------------------ environment --
.PHONY: doctor
doctor: ## Verify the dev environment (the authority on "does my setup work")
	bash .devcontainer/doctor.sh

.PHONY: sim-up
sim-up: ## Start the fake robot desktop (watch it at http://localhost:6080)
	docker compose up -d robot-sim

.PHONY: demo-up
demo-up: ## Start the full three-demo customer topology
	docker compose --profile demo up -d --build

.PHONY: demo-down
demo-down: ## Stop the demo stack
	docker compose --profile demo down

.PHONY: stack-up
stack-up: ## Start the standalone fjarr-server (production image)
	docker compose --profile stack up -d --build fjarr-server

.PHONY: turn-up
turn-up: ## Start coturn (use-auth-secret mode)
	docker compose --profile turn up -d coturn

# ------------------------------------------------------------ browser lab --
.PHONY: lab-up
lab-up: ## Start the lab browser (CDP at http://localhost:9222) beside fjarr-server (docs/25)
	docker compose --profile lab --profile stack up -d --build browser fjarr-server

.PHONY: lab-down
lab-down: ## Stop the lab browser
	docker compose --profile lab down browser

.PHONY: e2e
e2e: ## Browser e2e suites against the lab browser (run inside dev; `make lab-up` first)
	pnpm --filter @fjarr/e2e exec playwright test

.PHONY: e2e-loopback
e2e-loopback: ## Only the loopback-agent suites (no fjarr-server needed)
	pnpm --filter @fjarr/e2e exec playwright test --project loopback

# ------------------------------------------------------------------ agent --
BUILD_PRESET ?= release

.PHONY: agent-configure
agent-configure: ## CMake configure (BUILD_PRESET=release|debug|asan)
	cmake --preset $(BUILD_PRESET)

.PHONY: agent-build
agent-build: ## Build libfjarr + fjarr-agent + demo-robot (configures the preset on a fresh tree)
	cmake --preset $(BUILD_PRESET) && cmake --build --preset $(BUILD_PRESET)

.PHONY: agent-test
agent-test: ## Run C++ tests
	ctest --preset $(BUILD_PRESET) --output-on-failure

.PHONY: agent-test-asan
agent-test-asan: ## Unit + loop tests under Address/Undefined/Leak sanitizers (a gate, docs/15)
	cmake --preset asan && cmake --build --preset asan && ctest --preset asan --output-on-failure

.PHONY: agent-test-tsan
agent-test-tsan: ## Unit + loop tests under ThreadSanitizer (a gate; needs host vm.mmap_rnd_bits=28, docs/12)
	@echo "TSan needs ASLR entropy <= 28 bits: on the host run 'sudo sysctl -w vm.mmap_rnd_bits=28' (Docker's seccomp blocks setarch -R in the container)"
	cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure

.PHONY: agent-raii-gate
agent-raii-gate: ## Refuse raw GObject/GLib refcount and source calls outside the RAII kit (docs/23 memory ladder)
	@bad=$$(grep -rnE '\b(g_object_ref|gst_object_ref|g_object_unref|gst_object_unref|gst_sample_unref|gst_buffer_unref|gst_promise_unref|g_source_remove|g_signal_connect)\s*\(' agent/src agent/daemon demos/demo-robot --include='*.cpp' --include='*.hpp' | grep -v 'agent/src/core/glib/' | grep -v 'NOLINT' || true); \
	if [ -n "$$bad" ]; then echo "raw refcount/source calls outside agent/src/core/glib/ (use the RAII kit):"; echo "$$bad"; exit 1; fi; echo "agent-raii-gate: clean"

.PHONY: agent-leaks-selftest
agent-leaks-selftest: ## The leaks-tracer bracketing must catch a deliberate leak (docs/23 ladder layer 3; a gate that cannot fire is no gate)
	@if ./build/$(BUILD_PRESET)/agent/tests/fjarr-tests --gtest_filter='LeaksGate.*' --gtest_also_run_disabled_tests >/tmp/fjarr-leaks-selftest.log 2>&1; then \
	  echo "agent-leaks-selftest: FAILED — the deliberate leak was not reported"; tail -20 /tmp/fjarr-leaks-selftest.log; exit 1; fi; \
	grep -q "deliberately-leaked\|GstIdentity@" /tmp/fjarr-leaks-selftest.log && echo "agent-leaks-selftest: the bracketing reports the deliberate leak"

FJARR_LEAKS_TRACER = leaks(filters="GstElement,GstPad,GstBuffer,GstSample,GstPromise")
.PHONY: agent-leaks
agent-leaks: ## Run one opsim scenario against the demo robot under the leaks tracer and print what stayed alive (SCENARIO=smoke; docs/23)
	@echo "recreating demo-robot with GST_TRACERS (it stays on until the next 'docker compose up -d demo-robot' without FJARR_GST_TRACERS)"
	FJARR_GST_TRACERS='$(FJARR_LEAKS_TRACER)' docker compose --profile demo up -d demo-robot
	@for i in $$(seq 1 40); do $(ROBOT_CURL) localhost:7381/memory >/dev/null 2>&1 && break; sleep 0.5; done
	@quiet() { for i in $$(seq 1 60); do $(ROBOT_CURL) localhost:7381/memory | python3 -c 'import sys,json; d=json.load(sys.stdin); sys.exit(0 if d["producers_alive"]==0 and d["sessions_alive"]==0 else 1)' && return 0; sleep 0.5; done; echo "agent-leaks: the robot did not go quiet (producers/sessions alive)"; return 1; }; \
	quiet || exit 1; echo "agent-leaks: warm-up (first-session initialisation is not a leak)"; $(MAKE) --no-print-directory opsim OPSIM_SCENARIO=smoke >/dev/null || exit 1; quiet || exit 1; \
	tok=$$($(ROBOT_CURL) -X POST localhost:7381/memory/checkpoint | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["checkpoint"]); sys.exit(0 if d.get("leaks_tracer") else 3)') || { echo "agent-leaks: the leaks tracer is not active in the robot (GST_TRACERS not applied?)"; exit 1; }; \
	$(MAKE) --no-print-directory opsim OPSIM_SCENARIO=$(or $(SCENARIO),smoke); rc=$$?; quiet || exit 1; \
	$(ROBOT_CURL) "localhost:7381/memory?since=$$tok" | python3 -c 'import sys,json; d=json.load(sys.stdin); l=d["leaks"]; print("census diff:", json.dumps(d["diff"]["census"])); print("rss diff:", d["diff"]["rss_bytes"], "bytes"); print("leaks tracer: created-and-alive", len(l["created"]), "| alive now", l["alive"], "| alive at checkpoint", l["at_checkpoint"]); [print("  ", o) for o in l["created"]]; sys.exit(1 if l["created"] else 0)'; lr=$$?; \
	[ $$rc -eq 0 ] || echo "agent-leaks: note — the scenario's own assertions returned $$rc under the tracer's overhead (timing checks are gated by opsim-all without it); the verdict here is about leaks"; \
	[ $$lr -eq 0 ] && echo "agent-leaks ($(or $(SCENARIO),smoke)): clean" || { echo "agent-leaks ($(or $(SCENARIO),smoke)): FAILED — objects created by the scenario are still alive"; exit 1; }

.PHONY: agent-memcheck
agent-memcheck: ## valgrind memcheck on the loop tests (nightly, docs/15): the errors sanitizers structurally miss
	@mkdir -p build/memcheck
	G_SLICE=always-malloc G_DEBUG=gc-friendly GST_TRACERS= valgrind --leak-check=full --show-leak-kinds=definite --errors-for-leak-kinds=definite \
	  --error-exitcode=9 --suppressions=/usr/share/glib-2.0/valgrind/glib.supp --suppressions=agent/tests/valgrind/gstreamer.supp \
	  --suppressions=agent/tests/valgrind/fjarr.supp --gen-suppressions=all --log-file=build/memcheck/loop-tests.log \
	  ./build/$(BUILD_PRESET)/agent/tests/fjarr-tests --gtest_filter='LoopMedia.*:FrameHub.*:Introspect.*:Session.*' >/dev/null; rc=$$?; \
	grep -E "ERROR SUMMARY|definitely lost:|indirectly lost:" build/memcheck/loop-tests.log | tail -4; \
	[ $$rc -eq 0 ] && echo "agent-memcheck: clean (build/memcheck/loop-tests.log)" || { echo "agent-memcheck: valgrind exit $$rc — see build/memcheck/loop-tests.log (each error carries a ready suppression block: only GLib/GStreamer-internal ones belong in agent/tests/valgrind/fjarr.supp)"; exit $$rc; }

.PHONY: agent-heaptrack
agent-heaptrack: ## heaptrack the streaming loop test and print the allocators in fjarr code (nightly, docs/16 hot-path budget)
	@mkdir -p build/heaptrack && rm -f build/heaptrack/loop-media.gz build/heaptrack/loop-media.zst
	GST_TRACERS= heaptrack -o build/heaptrack/loop-media ./build/$(BUILD_PRESET)/agent/tests/fjarr-tests --gtest_filter='LoopMedia.*' >/dev/null 2>&1 || true
	@f=$$(ls build/heaptrack/loop-media.* 2>/dev/null | head -1); [ -n "$$f" ] || { echo "agent-heaptrack: no profile written"; exit 1; }; \
	heaptrack_print "$$f" -a 25 -p 0 -l 0 -t 0 2>/dev/null | tee build/heaptrack/loop-media.txt | grep -B1 -A2 -E "fjarr::" | head -60; \
	echo "agent-heaptrack: full report in build/heaptrack/loop-media.txt (budget: one GstBuffer header per subscriber per frame in FrameHub::deliver, nothing else per frame — docs/16)"

# The demo robot's endpoint (docs/24): reached inside its container with the demo's token (.env).
-include .env
INTROSPECT_TOKEN ?= $(or $(FJARR_INTROSPECT_TOKEN),dev-only-introspect-token)
ROBOT_CURL = docker compose exec -T demo-robot curl -sf -H "Authorization: Bearer $(INTROSPECT_TOKEN)"

.PHONY: introspect
introspect: ## Open the demo robot's pipeline viewer (http://localhost:7381/, docs/24); PIPELINE=<id> FORMAT=txt|json|dot prints one summary, SUMMARIES=1 every pipeline's
	@if [ -n "$(PIPELINE)" ]; then $(ROBOT_CURL) "localhost:7381/pipelines/$(PIPELINE).$(or $(FORMAT),txt)"; echo; elif [ -z "$(SUMMARIES)" ]; then \
	  echo "viewer: http://localhost:7381/  (token: $(INTROSPECT_TOKEN))"; (xdg-open http://localhost:7381/ >/dev/null 2>&1 || open http://localhost:7381/ >/dev/null 2>&1 || true); else \
	  $(ROBOT_CURL) localhost:7381/pipelines | python3 -c 'import sys,json; [print(p["id"], p["kind"], p["state"], "seq", p["seq"], p["last_trigger"]) for p in json.load(sys.stdin)["pipelines"]]'; \
	  for id in $$($(ROBOT_CURL) localhost:7381/pipelines | python3 -c 'import sys,json; [print(p["id"]) for p in json.load(sys.stdin)["pipelines"]]'); do echo "--- $$id"; $(ROBOT_CURL) "localhost:7381/pipelines/$$id.txt"; done; fi

OPSIM_SERVER ?= ws://fjarr-server:8080/ws
OPSIM_ROBOT ?= demo-robot-01
OPSIM_SCENARIO ?= smoke
.PHONY: opsim
OPSIM_IN ?= demo-robot
# The endpoint as opsim reaches it: loopback inside the robot container; the demo's exposed port (with its token) from anywhere else.
OPSIM_INTROSPECT ?= http://127.0.0.1:7381
opsim: ## Run one fjarr-opsim scenario against the demo robot, from inside its container (OPSIM_SCENARIO=smoke|toggle|…)
	docker compose exec -T $(OPSIM_IN) ./build/$(BUILD_PRESET)/agent/tools/fjarr-opsim --server $(OPSIM_SERVER) --robot $(OPSIM_ROBOT) --grant-secret $${FJARR_GRANT_HS256_SECRET:-dev-only-grant-secret} --scenario $(OPSIM_SCENARIO) --introspect $(OPSIM_INTROSPECT) --timeout 90 $(OPSIM_EXTRA)

.PHONY: opsim-all
opsim-all: ## Every CI opsim scenario (docs/23: all but soak and netem-*)
	@for s in smoke toggle hotplug silent-operator no-answer socket-drop ice-restart deadman; do echo "== $$s"; \
	  $(MAKE) --no-print-directory opsim OPSIM_SCENARIO=$$s; rc=$$?; \
	  if [ $$rc -ge 128 ]; then \
	    echo "opsim-all: $$s died of signal $$((rc - 128)) in the simulator's own peer teardown (the upstream DTLS race, docs/23 slice 5b notes) — one retry"; \
	    $(MAKE) --no-print-directory opsim OPSIM_SCENARIO=$$s; rc=$$?; \
	  fi; \
	  [ $$rc -eq 0 ] || exit 1; done

OPSIM_CYCLES ?= 200
.PHONY: opsim-soak
opsim-soak: ## The soak scenario: OPSIM_CYCLES connect/stream/close cycles (default 200; CI 20), /memory census back to the warm-up baseline (docs/23, docs/15)
	$(MAKE) --no-print-directory opsim OPSIM_SCENARIO=soak OPSIM_EXTRA="--cycles $(OPSIM_CYCLES) --timeout $$(( $(OPSIM_CYCLES) * 6 + 60 ))"

NETEM_PROFILE ?= lossy
# opsim shares the robot's network namespace: its media rides lo (eth0 is the browser lab's media path).
NETEM_DEV ?= eth0
.PHONY: opsim-netem
opsim-netem: ## Apply a docs/25 profile (NETEM_PROFILE=lan|wifi-ok|4g|lossy|bad) on the demo robot's egress (NETEM_DEV), run netem-<profile> from dev, always clear the qdisc
	@export NETEM_DEV="$(NETEM_DEV)"; trap 'docker/lab/netem.sh clear' EXIT; docker/lab/netem.sh apply $(NETEM_PROFILE) && $(MAKE) --no-print-directory opsim OPSIM_SCENARIO=netem-$(NETEM_PROFILE) OPSIM_IN=dev OPSIM_INTROSPECT=http://demo-robot:7381 OPSIM_EXTRA="--introspect-token $(INTROSPECT_TOKEN) $(OPSIM_EXTRA)"

# -------------------------------------------------------------- signaling --
.PHONY: signaling-run
signaling-run: ## Run fjarr-server from source
	cd signaling && cargo run -p fjarr-server

.PHONY: signaling-test
signaling-test: ## Rust tests
	cd signaling && cargo test --workspace

.PHONY: signaling-clippy
signaling-clippy: ## Rust lints
	cd signaling && cargo clippy --workspace --all-targets -- -D warnings

# -------------------------------------------------------------------- web --
.PHONY: web-dev
web-dev: ## Demo dashboard dev server (http://localhost:5173)
	pnpm --filter fjarr-demo-dashboard dev

.PHONY: web-build
web-build: ## Build @fjarr/core, @fjarr/react, the introspection viewer, demo dashboard
	pnpm -r --filter './web/packages/**' --filter './web/apps/**' --filter fjarr-demo-dashboard build

.PHONY: web-lint
web-lint: ## Typecheck the JS/TS workspace (sources, tests, e2e)
	pnpm -r --filter './web/**' --filter fjarr-demo-dashboard typecheck

.PHONY: web-test
web-test: ## Unit tests for @fjarr/core and @fjarr/react (vitest; browser e2e is `make e2e`)
	pnpm -r --filter './web/packages/**' test

# ---------------------------------------------------------------- website --
.PHONY: website-dev
website-dev: ## Landing + docs site preview (http://localhost:4321)
	pnpm --filter fjarr-website dev

.PHONY: website-build
website-build: ## Production build of fjarr.io
	pnpm --filter fjarr-website build

# --------------------------------------------------------------- protocol --
.PHONY: protocol-check
protocol-check: ## Validate golden fixtures against the protocol schemas
	node protocol/check.mjs

# ------------------------------------------------------------------- docs --
.PHONY: docs-lint
docs-lint: ## Markdown lint over docs and root files
	markdownlint-cli2 "docs/**/*.md" "*.md"

.PHONY: docs-links
docs-links: ## Internal link check (offline: files + anchors)
	lychee --offline --include-fragments "docs/**/*.md" "*.md"

# -------------------------------------------------------------- hygiene ---
.PHONY: fmt
fmt: ## Format everything
	find agent demos/demo-robot -name '*.[ch]pp' 2>/dev/null | xargs -r clang-format-21 -i
	cd signaling && cargo fmt
	pnpm -r format 2>/dev/null || true

.PHONY: lint
lint: signaling-clippy web-lint docs-lint agent-raii-gate ## All lints

.PHONY: help
help: ## List targets
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) \
	  | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'
