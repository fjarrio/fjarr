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
agent-build: ## Build libfjarr + fjarr-agent + demo-robot
	cmake --build --preset $(BUILD_PRESET)

.PHONY: agent-test
agent-test: ## Run C++ tests
	ctest --preset $(BUILD_PRESET) --output-on-failure

.PHONY: agent-test-asan
agent-test-asan: ## Unit + loop tests under Address/Undefined/Leak sanitizers (a gate, docs/15)
	cmake --preset asan && cmake --build --preset asan && ctest --preset asan --output-on-failure

.PHONY: agent-test-tsan
agent-test-tsan: ## Unit + loop tests under ThreadSanitizer (a trend until 3c; needs host vm.mmap_rnd_bits=28, docs/12)
	@echo "TSan needs ASLR entropy <= 28 bits: on the host run 'sudo sysctl -w vm.mmap_rnd_bits=28' (Docker's seccomp blocks setarch -R in the container)"
	cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure

.PHONY: agent-raii-gate
agent-raii-gate: ## Refuse raw GObject/GLib refcount and source calls outside the RAII kit (docs/23 memory ladder)
	@bad=$$(grep -rnE '\b(g_object_ref|gst_object_ref|g_object_unref|gst_object_unref|gst_sample_unref|gst_buffer_unref|gst_promise_unref|g_source_remove|g_signal_connect)\s*\(' agent/src agent/daemon demos/demo-robot --include='*.cpp' --include='*.hpp' | grep -v 'agent/src/core/glib/' | grep -v 'NOLINT' || true); \
	if [ -n "$$bad" ]; then echo "raw refcount/source calls outside agent/src/core/glib/ (use the RAII kit):"; echo "$$bad"; exit 1; fi; echo "agent-raii-gate: clean"

OPSIM_SERVER ?= ws://fjarr-server:8080/ws
OPSIM_ROBOT ?= demo-robot-01
OPSIM_SCENARIO ?= smoke
.PHONY: opsim
OPSIM_IN ?= demo-robot
opsim: ## Run one fjarr-opsim scenario against the demo robot, from inside its container (OPSIM_SCENARIO=smoke|toggle|…)
	docker compose exec -T $(OPSIM_IN) ./build/$(BUILD_PRESET)/agent/tools/fjarr-opsim --server $(OPSIM_SERVER) --robot $(OPSIM_ROBOT) --grant-secret $${FJARR_GRANT_HS256_SECRET:-dev-only-grant-secret} --scenario $(OPSIM_SCENARIO) --introspect http://127.0.0.1:7381 --timeout 90

.PHONY: opsim-all
opsim-all: ## Every CI opsim scenario (docs/23: all but soak and netem-*)
	@for s in smoke toggle hotplug silent-operator no-answer socket-drop ice-restart deadman; do echo "== $$s"; $(MAKE) --no-print-directory opsim OPSIM_SCENARIO=$$s || exit 1; done

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
web-build: ## Build @fjarr/core, @fjarr/react, demo dashboard
	pnpm -r --filter './web/packages/**' --filter fjarr-demo-dashboard build

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
