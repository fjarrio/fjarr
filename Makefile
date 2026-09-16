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
	pnpm -r --filter './web/**' --filter fjarr-demo-dashboard build

.PHONY: web-lint
web-lint: ## Typecheck the JS/TS workspace (sources + tests)
	pnpm -r --filter './web/**' --filter fjarr-demo-dashboard typecheck

.PHONY: web-test
web-test: ## Unit tests for @fjarr/core and @fjarr/react (vitest)
	pnpm -r --filter './web/**' test

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
	find agent demos/demo-robot -name '*.[ch]pp' 2>/dev/null | xargs -r clang-format-18 -i
	cd signaling && cargo fmt
	pnpm -r format 2>/dev/null || true

.PHONY: lint
lint: signaling-clippy web-lint docs-lint ## All lints

.PHONY: help
help: ## List targets
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) \
	  | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'
