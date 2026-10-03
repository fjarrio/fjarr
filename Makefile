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
lab-up: ## Start the lab browser (CDP at http://localhost:9222) beside fjarr-server and coturn (docs/25)
	docker compose --profile lab --profile stack up -d --build browser fjarr-server coturn

.PHONY: lab-down
lab-down: ## Stop the lab browser
	docker compose --profile lab down browser

.PHONY: e2e
e2e: ## Browser e2e suites against the lab browser (run inside dev; `make lab-up` first)
	pnpm --filter @fjarr/e2e exec playwright test

.PHONY: latency
latency: ## Glass-to-glass latency on THIS machine, recorded to web/e2e/latency.csv (LABEL=<hardware>; STRICT=1 holds it to the docs/16 budgets)
	E2E_LATENCY_LABEL=$(or $(LABEL),$(shell hostname)) E2E_LATENCY_STRICT=$(or $(STRICT),0) \
	  pnpm --filter @fjarr/e2e exec playwright test tests/stack/latency.spec.ts --project stack

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

# The log source, overridable so the gate's selftest can feed it a known-bad line.
ROBOT_LOG ?= docker compose logs --no-color --tail 20000 demo-robot
.PHONY: agent-log-gate
agent-log-gate: ## Fail if the robot logged a GLib/GStreamer CRITICAL or a failed assertion (docs/15)
	@bad=$$($(ROBOT_LOG) 2>&1 | grep -E 'CRITICAL \*\*|ERROR \*\*|assertion .* failed' || true); \
	if [ -n "$$bad" ]; then \
	  echo "agent-log-gate: the robot logged criticals, so this run is not green (docs/15):"; \
	  echo "$$bad" | head -20; exit 1; fi; \
	echo "agent-log-gate: clean"

.PHONY: agent-log-gate-selftest
agent-log-gate-selftest: ## The log gate must fire on a known-bad line (a gate that cannot fire is no gate)
	@if $(MAKE) --no-print-directory agent-log-gate ROBOT_LOG='printf "%s\n" "(demo-robot:1): GLib-CRITICAL **: g_source_unref_internal: assertion failed"' >/dev/null 2>&1; then \
	  echo "agent-log-gate-selftest: FAILED — the gate did not fire on the line that CI carried for weeks"; exit 1; fi; \
	echo "agent-log-gate-selftest: the gate fires"

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
	  ./build/$(BUILD_PRESET)/agent/tests/fjarr-tests --gtest_filter='LoopMedia.*:ConsumerNegotiation.*:FrameHub.*:Introspect.*:Session.*' >/dev/null; rc=$$?; \
	grep -E "ERROR SUMMARY|definitely lost:|indirectly lost:" build/memcheck/loop-tests.log | tail -4; \
	[ $$rc -eq 0 ] && echo "agent-memcheck: clean (build/memcheck/loop-tests.log)" || { echo "agent-memcheck: valgrind exit $$rc — see build/memcheck/loop-tests.log (each error carries a ready suppression block: only GLib/GStreamer-internal ones belong in agent/tests/valgrind/fjarr.supp)"; exit $$rc; }

.PHONY: agent-heaptrack
agent-heaptrack: ## heaptrack the streaming loop test and print the allocators in fjarr code (nightly, docs/16 hot-path budget)
	@mkdir -p build/heaptrack && rm -f build/heaptrack/loop-media.gz build/heaptrack/loop-media.zst
	@# The streaming hot path only: no test that completes DTLS. libsrtp's first init loads and dlcloses
	@# NSS, and heaptrack's allocation hook takes the dynamic-loader lock under its own — a lock-order
	@# deadlock with any thread allocating at that moment (nightly 2026-09-30). ConsumerNegotiation and
	@# the Session DTLS test are covered by memcheck instead.
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
# One command, shared by both targets, because the exit code matters: a nested `$(MAKE) opsim`
# reports its own 2 when the recipe dies of a signal, so opsim-all's retry guard below could never
# see the 139 it was written for and had been dead since it was added (found in CI, slice 4.5b).
OPSIM_RUN = docker compose exec -T $(OPSIM_IN) ./build/$(BUILD_PRESET)/agent/tools/fjarr-opsim --server $(OPSIM_SERVER) --robot $(OPSIM_ROBOT) --grant-secret $${FJARR_GRANT_HS256_SECRET:-dev-only-grant-secret} --introspect $(OPSIM_INTROSPECT) --timeout 90
opsim: ## Run one fjarr-opsim scenario against the demo robot, from inside its container (OPSIM_SCENARIO=smoke|toggle|…)
	$(OPSIM_RUN) --scenario $(OPSIM_SCENARIO) $(OPSIM_EXTRA)

.PHONY: opsim-all
opsim-all: ## Every CI opsim scenario (docs/23: all but soak and netem-*)
	@log=$$(mktemp); for s in smoke toggle hotplug rejected-track silent-operator no-answer socket-drop ice-restart deadman; do echo "== $$s"; \
	  $(OPSIM_RUN) --scenario $$s $(OPSIM_EXTRA) >$$log 2>&1; rc=$$?; cat $$log; \
	  if [ $$rc -ge 128 ]; then \
	    if grep -q "SUMMARY $$s: .*, 0 failed" $$log; then \
	      echo "opsim-all: $$s died of signal $$((rc - 128)) AFTER returning a clean verdict, in the simulator's own peer teardown (the upstream DTLS race, docs/23 slice 5b notes) — one retry"; \
	      $(OPSIM_RUN) --scenario $$s $(OPSIM_EXTRA) >$$log 2>&1; rc=$$?; cat $$log; \
	    else \
	      echo "opsim-all: $$s died of signal $$((rc - 128)) BEFORE its verdict — that is a crash in the run, not in its teardown, and it is not retried"; \
	    fi; \
	  fi; \
	  [ $$rc -eq 0 ] || { rm -f $$log; exit 1; }; done; rm -f $$log

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

TUN_DEV ?= fjarr0
TUN_OPERATOR ?= 100.64.0.1
# The interfaces' MTU. The agent reads it from its device and reports it on `open`, so both ends agree.
TUN_MTU ?= 1184
# Containers that share the robot's network namespace (`network_mode: service:demo-robot`), which is
# what puts them on its tunnel address. Recreating the robot leaves them pointing at a namespace
# that no longer exists — reachable enough to answer with a TCP reset, which reads as "nothing is
# listening" rather than "your sidecar is stale" — so tun-up recreates them after it.
# `ROS=1` adds the ROS 2 participant — but not here: it is recreated at the END of tun-up, once the
# interface exists and the agent has attached. A participant created before that never advertises
# the tunnel, however long it runs (docs/27#lifecycle, fact A, and `tunnel-ros-ordering`), and
# recreating it beside sshd here is exactly how the ROS gate went red in the M4.5 gate run.
ROBOT_SIDECARS ?= robot-services
# The size of the file the docs/27 scp gate transfers. Passed explicitly rather than left to the
# environment: `docker compose up` re-resolves a service's variables, so anything set only on an
# earlier command is lost the next time the sidecar is recreated — the same trap that cost CI the
# encoder setting (e739216) and TURN before it.
LAB_FILE_MB ?= 1024
.PHONY: tun-up
tun-up: ## Create the tunnel interfaces the installer creates on a real robot (docs/27): fjarr0 on demo-robot and on dev, then restart the robot with fjarr.net on
	@addr=$$(docker compose exec -T -e FJARR_ROBOT_ID=$(OPSIM_ROBOT) dev ./build/$(BUILD_PRESET)/agent/daemon/fjarr-agent --net-address | tail -1); \
	  echo "tun-up: $(OPSIM_ROBOT) derives $$addr (docs/27#addressing)"; \
	  FJARR_DEMO_NET=1 docker compose up -d demo-robot && \
	  FJARR_DEMO_NET=1 FJARR_LAB_FILE_MB=$(LAB_FILE_MB) FJARR_TUN_SELF="$$addr" FJARR_TUN_PEER=$(TUN_OPERATOR) \
	    docker compose --profile demo --profile ros up -d --no-deps --force-recreate $(ROBOT_SIDECARS) && \
	  docker/lab/tundev.sh up demo-robot "$$addr" $(TUN_OPERATOR) $(TUN_DEV) $(TUN_MTU) && \
	  docker/lab/tundev.sh up dev $(TUN_OPERATOR) "$$addr" $(TUN_DEV) $(TUN_MTU)
	@echo "tun-up: the robot waited for its interface before starting — the ordering the whole design rests on (docs/27#lifecycle)"
	@# `up -d` leaves a running container alone, and its supervisor keeps the agent process it
	@# started — which may be a binary built hours ago. Every measurement after a rebuild then runs
	@# against stale code and says nothing about the change (it did, twice, in slice 4.5e). So the
	@# agent process is always restarted here, the way systemd would restart it, onto whatever
	@# build/ holds now; the device survives, as docs/27#lifecycle requires. The wait below counts
	@# hello-ack lines rather than grepping for one, because an old line matches too.
	@before=$$(docker compose logs --no-color demo-robot 2>/dev/null | grep -c "hello-ack: online"); \
	  docker compose exec -T demo-robot pkill -x demo-robot >/dev/null 2>&1 || true; \
	  for i in $$(seq 60); do \
	    now=$$(docker compose logs --no-color demo-robot 2>/dev/null | grep -c "hello-ack: online"); \
	    [ "$$now" -gt "$$before" ] && break; sleep 1; done; \
	  now=$$(docker compose logs --no-color demo-robot 2>/dev/null | grep -c "hello-ack: online"); \
	  [ "$$now" -gt "$$before" ] \
	  || { echo "tun-up: the robot never registered with the server; its last lines were:"; \
	       docker compose logs --no-color --tail 20 demo-robot; exit 1; }
	@# The sidecars share the robot's namespace and are what the gate actually talks to, and the one
	@# that serves ssh builds its payload file before it binds. Waiting for the agent alone left a
	@# window where the link was up and port 22 answered with a reset.
	@for i in $$(seq 60); do docker compose exec -T -u root demo-robot sh -c 'ss -ltn 2>/dev/null | grep -q ":22 "' && break; sleep 1; done; \
	  docker compose exec -T -u root demo-robot sh -c 'ss -ltn 2>/dev/null | grep -q ":22 "' \
	  || { echo "tun-up: nothing is serving ssh in the robot's namespace; docker compose logs robot-services"; exit 1; }
	@echo "tun-up: the robot is online, attached, and serving ssh; 'make opsim-tunnel' will find it"
	@if [ -n "$(ROS)" ]; then \
	  addr=$$(docker compose exec -T -e FJARR_ROBOT_ID=$(OPSIM_ROBOT) dev ./build/$(BUILD_PRESET)/agent/daemon/fjarr-agent --net-address | tail -1 | tr -d '\r'); \
	  FJARR_DEMO_NET=1 FJARR_TUN_SELF="$$addr" FJARR_TUN_PEER=$(TUN_OPERATOR) docker compose --profile demo --profile ros up -d --no-deps --force-recreate robot-ros operator-ros >/dev/null; \
	  for i in $$(seq 60); do docker compose --profile demo --profile ros logs --no-color robot-ros 2>/dev/null | grep -q "publishing /fjarr" && break; sleep 1; done; \
	  docker compose --profile demo --profile ros logs --no-color robot-ros 2>/dev/null | grep -q "publishing /fjarr" \
	    && echo "tun-up: ROS 2 is publishing on the robot, after the interface existed (docs/27#lifecycle)" \
	    || { echo "tun-up: the robot's ROS participant never started; docker compose logs robot-ros"; exit 1; }; \
	fi

.PHONY: tun-down
tun-down: ## Remove the lab's tunnel interfaces and put the demo robot back to its usual state
	@docker/lab/tundev.sh down demo-robot $(TUN_DEV); docker/lab/tundev.sh down dev $(TUN_DEV)
	@FJARR_DEMO_NET=0 docker compose up -d demo-robot >/dev/null 2>&1 || true

.PHONY: opsim-tunnel
opsim-tunnel: ## The docs/27 tunnel scenario: real IP over fjarr:stream:fjarr.net, from dev's own network namespace
	$(MAKE) --no-print-directory opsim OPSIM_SCENARIO=tunnel OPSIM_IN=dev OPSIM_INTROSPECT=http://demo-robot:7381 OPSIM_EXTRA="--introspect-token $(INTROSPECT_TOKEN) $(OPSIM_EXTRA)"

.PHONY: ros-up
ros-up: ## Start ROS 2 on both ends of the link (docs/27#ros2). Prefer `make tun-up ROS=1`, which orders it correctly
	@# --no-deps is not optional: without it compose recreates demo-robot as a dependency, which
	@# takes the tunnel device with it and leaves every namespace-sharing sidecar pointing at a
	@# container that no longer exists (found the hard way, slice 4.5d).
	docker compose --profile demo --profile ros up -d --no-deps --force-recreate robot-ros operator-ros
	@for i in $$(seq 60); do docker compose --profile demo --profile ros logs --no-color robot-ros 2>/dev/null | grep -q "publishing /fjarr" && break; sleep 1; done; \
	  docker compose --profile demo --profile ros logs --no-color robot-ros 2>/dev/null | grep -q "publishing /fjarr" \
	  || { echo "ros-up: the robot's participant never started; docker compose logs robot-ros"; exit 1; }
	@echo "ros-up: both ends are running ROS 2"

.PHONY: ros-down
ros-down: ## Stop the ROS 2 sidecars
	docker compose --profile demo --profile ros stop robot-ros operator-ros >/dev/null 2>&1 || true

.PHONY: ros-topics
ros-topics: ## What the operator's ROS 2 can see of the robot (docs/27#ros2 gate)
	docker compose exec -T operator-ros bash -lc 'source /opt/ros/jazzy/setup.bash && timeout 20 ros2 topic list'

.PHONY: tunnel-ssh
tunnel-ssh: ## docs/27 gate: a shell on the robot over the link, through the tools a developer owns
	$(MAKE) --no-print-directory opsim-tunnel OPSIM_EXTRA="--exec 'docker/lab/tunnel-checks.sh ssh' $(OPSIM_EXTRA)"

.PHONY: tunnel-ros
tunnel-ros: ## docs/27#ros2 gate: ros2 topic list and echo against the robot, with the direct path removed
	@docker/lab/dds-isolate.sh on
	$(MAKE) --no-print-directory opsim-tunnel OPSIM_EXTRA="--ice-policy relay --exec 'docker/lab/tunnel-checks.sh ros2' $(OPSIM_EXTRA)"; \
	  rc=$$?; docker/lab/dds-isolate.sh off >/dev/null; exit $$rc

.PHONY: connect-dry
connect-dry: ## fjarr-connect against the demo robot as far as the offer (signaling only, slice 4.5e)
	@grant=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT) fjarr.net); \
	  docker compose exec -T -e FJARR_GRANT="$$grant" dev sh -c 'cd /workspace/signaling && cargo build -q -p fjarr-connect && ./target/debug/fjarr-connect $(OPSIM_ROBOT) --server $(OPSIM_SERVER) --dry-run'

OPSIM_ROBOT_2 ?= demo-robot-02
.PHONY: tun-up-2
tun-up-2: ## Bring the second robot up with its own tunnel interface, for the two-robot isolation regression
	@addr=$$(docker compose exec -T -e FJARR_ROBOT_ID=$(OPSIM_ROBOT_2) dev ./build/$(BUILD_PRESET)/agent/daemon/fjarr-agent --net-address | tail -1 | tr -d '\r'); \
	  echo "tun-up-2: $(OPSIM_ROBOT_2) derives $$addr (docs/27#addressing)"; \
	  FJARR_DEMO_NET_2=1 docker compose --profile demo up -d --no-deps demo-robot-2 >/dev/null && \
	  docker compose restart demo-robot-2 >/dev/null && \
	  docker/lab/tundev.sh up demo-robot-2 "$$addr" $(TUN_OPERATOR) $(TUN_DEV)
	@# This robot has no supervisor, so it cannot be restarted onto a device that appears later: it
	@# waits 20 s for one at boot, and the line above creates it inside that window. That ordering is
	@# the same one a real robot's systemd unit enforces (docs/27#lifecycle). The restart is what
	@# makes the agent boot *now*: when the container was already running, `up -d` does nothing, the
	@# device appeared long after the agent's window, and its attach was refused (found 2026-09-28).
	@for i in $$(seq 60); do docker compose logs --no-color demo-robot-2 2>/dev/null | grep -q "hello-ack: online" && break; sleep 1; done; \
	  docker compose exec -T demo-robot-2 sh -c 'ip link show $(TUN_DEV) >/dev/null 2>&1' \
	  || { echo "tun-up-2: the second robot has no $(TUN_DEV); docker compose logs demo-robot-2"; exit 1; }
	@echo "tun-up-2: the second robot is online with its own interface"

.PHONY: connect-build
connect-build: ## Build fjarr-connect --release and grant it CAP_NET_ADMIN the way the installer does
	@docker compose exec -T dev sh -c 'cd /workspace/signaling && cargo build -q --release -p fjarr-connect'
	@# The same grant docs/27 describes for a real install: the capability lives on the client binary,
	@# never on the agent, and never on the whole container's user. Without it the client can still
	@# attach an interface somebody else created — it just cannot add the second robot's route.
	@docker compose exec -T -u root dev setcap cap_net_admin+p /workspace/signaling/target/release/fjarr-connect
	@docker compose exec -T dev getcap /workspace/signaling/target/release/fjarr-connect

.PHONY: tunnel-isolation
tunnel-isolation: ## docs/15 safety class: two robots on one operator interface cannot reach each other, either way
	$(MAKE) --no-print-directory tun-up
	$(MAKE) --no-print-directory tun-up-2
	@a=$$(docker compose exec -T -e FJARR_ROBOT_ID=$(OPSIM_ROBOT) dev ./build/$(BUILD_PRESET)/agent/daemon/fjarr-agent --net-address | tail -1 | tr -d '\r'); \
	  b=$$(docker compose exec -T -e FJARR_ROBOT_ID=$(OPSIM_ROBOT_2) dev ./build/$(BUILD_PRESET)/agent/daemon/fjarr-agent --net-address | tail -1 | tr -d '\r'); \
	  docker/lab/tundev.sh up dev $(TUN_OPERATOR) "$$a" $(TUN_DEV) >/dev/null; \
	  grant_a=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT) fjarr.net); \
	  grant_b=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT_2) fjarr.net); \
	  $(MAKE) --no-print-directory connect-build && \
	  docker compose exec -T -e FJARR_LOG=warn dev \
	    ./signaling/target/release/fjarr-connect $(OPSIM_ROBOT) $(OPSIM_ROBOT_2) \
	    --server $(OPSIM_SERVER) --dev $(TUN_DEV) --grant "$$grant_a" --grant "$$grant_b" \
	    -- docker/lab/two-robot-isolation.sh

.PHONY: connect-unprivileged
connect-unprivileged: ## docs/27's promise that an existing, addressed interface needs no privilege: strip the capability and run ssh over the link
	@$(MAKE) --no-print-directory connect-build >/dev/null
	@docker compose exec -T -u root dev setcap -r /workspace/signaling/target/release/fjarr-connect
	@caps=$$(docker compose exec -T dev getcap /workspace/signaling/target/release/fjarr-connect); \
	  echo "connect-unprivileged: binary has $${caps:-no capabilities} — attaching to the interface tun-up created"
	@grant=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT) fjarr.net); \
	  docker compose exec -T -e FJARR_GRANT="$$grant" -e FJARR_LOG=warn dev \
	    ./signaling/target/release/fjarr-connect $(OPSIM_ROBOT) --server $(OPSIM_SERVER) --dev $(TUN_DEV) -- docker/lab/tunnel-checks.sh ssh \
	  || { echo "connect-unprivileged: FAIL — an addressed interface must not need CAP_NET_ADMIN to attach (docs/27#the-operator-client)"; \
	       docker compose exec -T -u root dev setcap cap_net_admin+p /workspace/signaling/target/release/fjarr-connect; exit 1; }
	@# Put the capability back: every other lab target expects the installed shape.
	@docker compose exec -T -u root dev setcap cap_net_admin+p /workspace/signaling/target/release/fjarr-connect
	@echo "connect-unprivileged: PASS"

.PHONY: connect-shell
connect-shell: ## docs/27#shell gate: the robot's terminal in a pty — round trip, stty size, a resize, exit status, termios restored after exit, a killed link and SIGTERM
	@# A copy of the binary, because `cp` drops the file capability connect-build grants: the shell
	@# must work with no privilege at all, and this is where that is proved (docs/27#shell). The
	@# terminal is in no control domain (docs/10#terminal), so this runs beside anything else in the lab.
	@docker compose exec -T dev sh -c 'cd /workspace/signaling && cargo build -q --release -p fjarr-connect && cp target/release/fjarr-connect /tmp/fjarr-connect-shell'
	@grant=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT) fjarr.terminal); \
	  denied=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT) fjarr.test); \
	  docker compose exec -T -e SHELL_GRANT="$$grant" -e SHELL_GRANT_DENIED="$$denied" dev \
	    python3 docker/lab/shell-checks.py /tmp/fjarr-connect-shell $(OPSIM_ROBOT) $(OPSIM_SERVER)

.PHONY: connect-platform-check
connect-platform-check: ## Type-check fjarr-connect's platform code for macOS (tunnel + shell) and Windows (shell), which nothing here runs (docs/04)
	@docker compose exec -T dev bash docker/lab/platform-check.sh /workspace/signaling/crates/fjarr-connect/src

.PHONY: tunnel-collision
tunnel-collision: ## Two robots claiming one address: the operator must refuse the pair by name (docs/27#addressing)
	$(MAKE) --no-print-directory tun-up
	@a=$$(docker compose exec -T -e FJARR_ROBOT_ID=$(OPSIM_ROBOT) dev ./build/$(BUILD_PRESET)/agent/daemon/fjarr-agent --net-address | tail -1 | tr -d '\r'); \
	  echo "tunnel-collision: pinning $(OPSIM_ROBOT_2) to $$a, which $(OPSIM_ROBOT) already derives"; \
	  FJARR_DEMO_NET_2=1 FJARR_DEMO_NET_ADDRESS_2="$$a" docker compose --profile demo up -d --no-deps --force-recreate demo-robot-2 >/dev/null && \
	  docker/lab/tundev.sh up demo-robot-2 "$$a" $(TUN_OPERATOR) $(TUN_DEV) >/dev/null && \
	  for i in $$(seq 60); do docker compose logs --no-color demo-robot-2 2>/dev/null | grep -q "hello-ack: online" && break; sleep 1; done; \
	  $(MAKE) --no-print-directory connect-build >/dev/null && \
	  grant_a=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT) fjarr.net); \
	  grant_b=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT_2) fjarr.net); \
	  out=$$(docker compose exec -T -e FJARR_LOG=error dev \
	    ./signaling/target/release/fjarr-connect $(OPSIM_ROBOT) $(OPSIM_ROBOT_2) \
	    --server $(OPSIM_SERVER) --dev $(TUN_DEV) --grant "$$grant_a" --grant "$$grant_b" \
	    -- true 2>&1); rc=$$?; \
	  echo "$$out" | grep -viE "pingAllCandidates|remote_addr|refusing first|TURN transaction|Failed to connect TCP"; \
	  if [ $$rc -eq 0 ]; then echo "tunnel-collision: FAIL the operator attached a colliding pair"; exit 1; fi; \
	  echo "$$out" | grep -q "both claim $$a" \
	    && echo "tunnel-collision: PASS refused by name, with the address line that fixes it" \
	    || { echo "tunnel-collision: FAIL it failed for some other reason than the collision"; exit 1; }
	@# Put the second robot back to a derived address, so the isolation regression is not left looking
	@# at a robot pinned to its neighbour's address.
	@FJARR_DEMO_NET_2=1 docker compose --profile demo up -d --no-deps --force-recreate demo-robot-2 >/dev/null 2>&1 || true

# The M4.5 gate's own claims are made through fjarr-connect, the product, with the direct path
# removed (dds-isolate) and the client relay-only: the lab's stand-in for a robot behind carrier
# NAT (docs/17 M4.5 gate, docs/27#testing). The candidate log proves the client offered nothing
# but relay candidates; a green run with a host candidate in it would be measuring the bridge.
CONNECT_RELAY = docker compose exec -T -e FJARR_LOG=warn,fjarr_connect=debug dev ./signaling/target/release/fjarr-connect $(OPSIM_ROBOT) \
  --server $(OPSIM_SERVER) --dev $(TUN_DEV) --relay-only
define connect_relay_check
	@$(MAKE) --no-print-directory connect-build >/dev/null
	@docker/lab/dds-isolate.sh on
	@grant=$$(docker/lab/mint-grant.sh $(OPSIM_ROBOT) fjarr.net); \
	  out=$$($(CONNECT_RELAY) --grant "$$grant" -- $(1) 2>&1); rc=$$?; docker/lab/dds-isolate.sh off >/dev/null; \
	  echo "$$out" | grep -viE "pingAllCandidates|remote_addr|refusing first|TURN transaction|Failed to connect TCP|local candidate|route outlived"; \
	  if echo "$$out" | grep -q 'candidate.kind=host'; then echo "$(2): FAIL — a host candidate was offered under --relay-only"; exit 1; fi; \
	  echo "$$out" | grep -q 'candidate.kind=relay' || { echo "$(2): FAIL — no relay candidate was offered (is coturn up?)"; exit 1; }; \
	  [ $$rc -eq 0 ] && echo "$(2): PASS — over the relay, with the direct path removed" || { echo "$(2): FAIL (exit $$rc)"; exit $$rc; }
endef

.PHONY: tunnel-ssh-connect
tunnel-ssh-connect: ## M4.5 gate: ssh over the link through fjarr-connect, relay-only, direct path removed
	$(call connect_relay_check,docker/lab/tunnel-checks.sh ssh,tunnel-ssh-connect)

.PHONY: tunnel-scp-connect
tunnel-scp-connect: ## M4.5 gate: a hash-verified LAB_FILE_MB scp through fjarr-connect, relay-only, direct path removed
	$(call connect_relay_check,docker/lab/tunnel-checks.sh scp,tunnel-scp-connect)

.PHONY: tunnel-ros-connect
tunnel-ros-connect: ## M4.5 gate: ros2 topic list and echo through fjarr-connect, relay-only, direct path removed (needs tun-up ROS=1)
	$(call connect_relay_check,docker/lab/tunnel-checks.sh ros2,tunnel-ros-connect)

.PHONY: m45-gate
m45-gate: ## The M4.5 gate, criterion by criterion (docs/06 fjarr.net, docs/17): every claim through fjarr-connect on the relay path, plus the lab's two-robot and login gates. Run from the HOST (it recreates services)
	@if [ -n "$(IN_DEV)" ]; then echo "m45-gate: run this from the host — it recreates services, and a recreate from inside dev mounts the wrong workspace"; exit 2; fi
	@echo "m45-gate: 0/6 the lab the way the installer leaves a robot: interface, then agent, then its ROS stack"; $(MAKE) --no-print-directory tun-up ROS=1 >/dev/null
	@echo "m45-gate: 1/6 ssh over the link, relay-only, direct path removed";        $(MAKE) --no-print-directory tunnel-ssh-connect
	@echo "m45-gate: 2/6 hash-verified $(LAB_FILE_MB) MiB scp, the same way";           $(MAKE) --no-print-directory tunnel-scp-connect
	@echo "m45-gate: 3/6 ros2 topic list and echo, Fast DDS unconfigured, the same way"; $(MAKE) --no-print-directory tunnel-ros-connect
	@echo "m45-gate: 4/6 two robots attached at once, unreachable from each other";   $(MAKE) --no-print-directory tunnel-isolation
	@echo "m45-gate: 5/6 login through the dashboard, list, pick, connect, audit log"; $(MAKE) --no-print-directory tunnel-login
	@echo "m45-gate: 6/6 the agent restarts without the robot's ROS stack restarting"; $(MAKE) --no-print-directory tunnel-ros-ordering
	@echo "m45-gate: every criterion the lab can measure passed. Cyclone DDS is open question #29 and is not claimed."

# Where a target runs matters in this lab. A `docker compose up` from INSIDE dev hands the host's
# daemon `/workspace` as the bind source — a path that exists only in the container — and every
# service it recreates comes up with an empty workspace ("demo-robot not built yet"). So targets
# that recreate services run on the host. The browser suites are the opposite: their defaults name
# compose services (`browser:9222`), so they run inside dev, where those names resolve — unless the
# host provides the E2E_* view of the same stack, as CI does.
IN_DEV := $(shell test -d /workspace -a -f /.dockerenv && echo 1)
E2E_IN_DEV = $(if $(IN_DEV),,docker compose exec -T dev )pnpm --filter @fjarr/e2e exec playwright test

.PHONY: tunnel-login
tunnel-login: ## The 4.5f gate: fjarr-connect login through the demo dashboard, then list, pick and connect with nobody typing a robot id
	@$(MAKE) --no-print-directory connect-build >/dev/null
	$(if $(E2E_BROWSER),pnpm --filter @fjarr/e2e exec playwright test,$(E2E_IN_DEV)) tests/stack/cli-login.spec.ts --project stack

.PHONY: tunnel-ros-ordering
tunnel-ros-ordering: ## The three docs/27#lifecycle facts as a regression: attached, detached, and across an agent restart
	docker/lab/ros-ordering.sh

.PHONY: tunnel-scp
tunnel-scp: ## docs/27 gate: pull LAB_FILE_MB over the link and verify its sha256 (asserted since ADR-0027 closed #28)
	$(MAKE) --no-print-directory opsim-tunnel OPSIM_EXTRA="--exec 'docker/lab/tunnel-checks.sh scp' --timeout 300 $(OPSIM_EXTRA)"

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

# --------------------------------------------------------------- packaging --
# docs/26#releases, ADR-0031. Run on the HOST: they start their own throwaway containers.
DEB_ARCH ?= $(shell dpkg --print-architecture 2>/dev/null || uname -m)
# `make deb` builds its builder image; CI pulls it by content hash first and sets DEB_BUILDER=prebuilt
# (tools/ci/image.py, docs/30#ci-images).
DEB_BUILDER ?= build
DEB_BUILDER_IMAGE = $(if $(filter prebuilt,$(DEB_BUILDER)),docker image inspect fjarr-deb-builder >/dev/null 2>&1 || { echo "DEB_BUILDER=prebuilt but there is no fjarr-deb-builder image (tools/ci/image.py build fjarr-deb-builder docker/deb-builder)"; exit 1; },docker build -q -t fjarr-deb-builder docker/deb-builder >/dev/null)
.PHONY: desktop-e2e
desktop-e2e: ## M3: the fixture's desktop end to end — see it (frames, a still screen), drive it (input, text, input-to-photon, release on close), hot-plug monitors, the clipboard both ways, the local cursor, virtual monitors, through helper → module E → agent → opsim (host-run; make agent-build first)
	docker compose --profile desktop --profile stack up -d --build --wait desktop-fixture fjarr-server
	docker compose --profile desktop --profile stack up -d --no-deps --force-recreate desktop-robot
	@for i in $$(seq 30); do docker compose --profile desktop logs --no-color desktop-robot 2>/dev/null | grep -q "capture of .* ready" && break; sleep 1; done
	@rc=0; for s in desktop-see desktop-control desktop-hotplug desktop-clipboard desktop-cursor desktop-virtual; do \
	  $(MAKE) --no-print-directory opsim OPSIM_ROBOT=desktop-robot-01 OPSIM_SCENARIO=$$s OPSIM_INTROSPECT=http://desktop-robot:7381 \
	    OPSIM_EXTRA="--introspect-token $(INTROSPECT_TOKEN) --desktop-oracle http://desktop-fixture:8090/testwin.log --desktop-plug http://desktop-fixture:8091" || rc=1; \
	done; \
	  [ $$rc -eq 0 ] || docker compose --profile desktop logs --no-color --tail 40 desktop-robot desktop-fixture; \
	  docker compose --profile desktop stop desktop-robot desktop-fixture >/dev/null 2>&1; exit $$rc

.PHONY: lab-test lab-runner-install
ci-test: ## tools/ci's own tests: the change classifier and the CI images' content hash (docs/30)
	@tools/ci/test.sh

lab-test: ## fjarr-lab's tests: schedule, reservations, graceful stop, against fake systemctl (docs/12#lab-machines-and-fjarr-lab)
	@tools/fjarr-lab/test.sh

lab-runner-install: ## ON A LAB MACHINE, as root: install fjarr-lab, its units and a config (the GitHub runner itself comes from its own ./svc.sh install)
	install -m 0755 tools/fjarr-lab/fjarr-lab /usr/local/bin/fjarr-lab
	install -m 0644 tools/fjarr-lab/fjarr-lab-apply.service tools/fjarr-lab/fjarr-lab-open.timer tools/fjarr-lab/fjarr-lab-close.timer /etc/systemd/system/
	[ -f /etc/fjarr-lab.conf ] || install -m 0644 tools/fjarr-lab/fjarr-lab.conf.example /etc/fjarr-lab.conf
	systemctl daemon-reload
	systemctl enable fjarr-lab-apply.service fjarr-lab-open.timer fjarr-lab-close.timer
	systemctl start fjarr-lab-open.timer fjarr-lab-close.timer
	@echo "lab-runner-install: set RUNNER_SERVICE in /etc/fjarr-lab.conf, then: sudo fjarr-lab window 00:00-06:00 && fjarr-lab status"

.PHONY: desktop-fixture-test
desktop-browser: ## M3: the demo dashboard's Desktop panel in the lab browser against the fixture's desktop — view, input, hot-plug; every test must run, none may skip (host-run; needs the demo stack, the lab browser and make agent-build)
	docker compose --profile desktop --profile stack up -d --build --wait desktop-fixture fjarr-server
	docker compose --profile desktop --profile stack up -d --no-deps --force-recreate desktop-robot
	@for i in $$(seq 30); do docker compose --profile desktop logs --no-color desktop-robot 2>/dev/null | grep -q "capture of .* ready" && break; sleep 1; done
	@# From dev, where the fixture's and the robot's compose names resolve (the runner cannot reach them).
	@# The JSON report carries each skip's reason, which the list reporter does not print.
	@mkdir -p build; rm -f build/desktop-browser.json; rc=0; \
	  $(if $(IN_DEV),,docker compose exec -T dev )env PLAYWRIGHT_JSON_OUTPUT_NAME=/workspace/build/desktop-browser.json \
	    pnpm --filter @fjarr/e2e exec playwright test tests/stack/dashboard.spec.ts --project stack -g "Desktop panel" --reporter=list,json || rc=1; \
	  skips=$$(python3 -c 'import json,sys; r=[]; w=lambda o: [w(v) for v in (o.values() if isinstance(o,dict) else o if isinstance(o,list) else [])] + ([r.append(o.get("description",""))] if isinstance(o,dict) and o.get("type")=="skip" else []); w(json.load(open("build/desktop-browser.json"))); print("\n".join(sorted(set(r))))' 2>/dev/null); \
	  if [ ! -f build/desktop-browser.json ]; then echo "desktop-browser: no report was written"; rc=1; \
	  elif [ -n "$$skips" ]; then echo "desktop-browser: a test skipped — they must all run here:"; echo "$$skips" | sed 's/^/  /'; rc=1; fi; \
	  [ $$rc -eq 0 ] || { docker compose --profile desktop --profile demo logs --no-color --tail 300 desktop-robot; docker compose --profile desktop --profile demo logs --no-color --tail 40 desktop-fixture demo-dashboard; }; \
	  docker compose --profile desktop stop desktop-robot desktop-fixture >/dev/null 2>&1; exit $$rc

desktop-fixture-test: ## M3: headless mutter proves capture and EIS input, then the session helper's handover of both (docs/15#the-desktop-test-lab)
	docker compose --profile desktop up -d --build --wait desktop-fixture
	@docker compose --profile desktop exec -T desktop-fixture sh -c 'fixture-selftest && fixture-helper-check /usr/local/bin/fjarr-desktop-session'; rc=$$?; \
	  [ $$rc -eq 0 ] || docker compose --profile desktop logs --no-color --tail 30 desktop-fixture; \
	  docker compose --profile desktop stop desktop-fixture >/dev/null 2>&1; exit $$rc

.PHONY: compose-gate
compose-gate: ## M2.5 gate: the reference compose file runs a robot, tunnel included (host-run; needs make deb + the lab stack; docs/26#containerized-robots)
	@BUILD_PRESET=$(BUILD_PRESET) DEB_ARCH=$(DEB_ARCH) packaging/compose/gate.sh

.PHONY: deb deb-install-test deb-embed-test install-script-test set-version
deb: ## Build the .debs (fjarr-agent, fjarr-tools, fjarr-desktop-wayland, libfjarr-dev) for this host's architecture into dist/deb/<arch>/
	@if [ -f /.dockerenv ]; then echo "make deb runs on the host (it starts its own builder container)"; exit 1; fi
	@$(DEB_BUILDER_IMAGE)
	mkdir -p dist/deb/$(DEB_ARCH)
	@# Only this build's packages: the tests install dist/deb/<arch>/*.deb, and a stale version beside
	@# a new one would be tested as a mix.
	rm -f dist/deb/$(DEB_ARCH)/*.deb
	docker run --rm --user "$$(id -u):$$(id -g)" -e HOME=/tmp -v "$(CURDIR)":/src:ro -v "$(CURDIR)/dist/deb/$(DEB_ARCH)":/out fjarr-deb-builder

.PHONY: lab-artifacts
lab-artifacts: ## fjarr-server and fjarr-opsim for a lab machine's nightly (Ubuntu 26.04, built in the deb builder) into dist/lab/ (docs/15#unattended-access-test-the-industrial-gate)
	@if [ -f /.dockerenv ]; then echo "make lab-artifacts runs on the host (it starts the builder container)"; exit 1; fi
	@$(DEB_BUILDER_IMAGE)
	mkdir -p dist/lab && rm -f dist/lab/fjarr-server dist/lab/fjarr-opsim
	docker run --rm --user "$$(id -u):$$(id -g)" -e HOME=/tmp -v "$(CURDIR)":/src:ro -v "$(CURDIR)/dist/lab":/out --entrypoint sh fjarr-deb-builder -euc '\
	  mkdir -p /tmp/w && tar -C /src --exclude=./node_modules --exclude="./**/node_modules" --exclude=./build --exclude=./signaling/target --exclude=./dist --exclude=./inspiration -cf - . | tar -C /tmp/w -xf - && \
	  cmake -S /tmp/w -B /tmp/w/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DFJARR_BUILD_TESTS=OFF >/dev/null && cmake --build /tmp/w/b --target fjarr-opsim >/dev/null && \
	  cp /tmp/w/b/agent/tools/fjarr-opsim /out/ && \
	  cd /tmp/w/signaling && cargo build -q --release --locked -p fjarr-server && cp target/release/fjarr-server /out/'
	@ls -l dist/lab

deb-install-test: ## Install dist/deb/<arch>/*.deb on a clean Ubuntu 26.04 and check what they promise
	@if [ -f /.dockerenv ]; then echo "make deb-install-test runs on the host"; exit 1; fi
	docker run --rm -v "$(CURDIR)/dist/deb/$(DEB_ARCH)":/debs:ro -v "$(CURDIR)/packaging/install-test.sh":/install-test.sh:ro ubuntu:26.04 sh /install-test.sh

set-version: ## Set one version everywhere it is declared (V=X.Y.Z), before tagging vX.Y.Z (docs/26#releases)
	@docker compose exec -T dev bash packaging/set-version.sh $(V)

install-script-test: ## install.sh against a signed test repository on a clean Ubuntu 26.04: dry run, wrong key refused, install; and a non-Ubuntu refused
	@if [ -f /.dockerenv ]; then echo "make install-script-test runs on the host"; exit 1; fi
	docker run --rm -v "$(CURDIR)/dist/deb/$(DEB_ARCH)":/debs:ro -v "$(CURDIR)/packaging":/pk:ro ubuntu:26.04 sh /pk/install-script-test.sh
	@out=$$(docker run --rm -v "$(CURDIR)/packaging":/pk:ro debian:13 sh /pk/install.sh 2>&1 || true); \
	  echo "$$out" | grep -q "this is Debian" && echo "$$out" | grep -q "docker run ghcr.io/fjarrio/fjarr-agent" \
	  && echo "ok   a non-Ubuntu system is refused, naming the container route" || { echo "$$out"; echo "FAIL: debian was not refused"; exit 1; }

deb-embed-test: ## Build demo-robot out of tree against the installed libfjarr-dev (find_package(fjarr)) on a clean Ubuntu 26.04
	@if [ -f /.dockerenv ]; then echo "make deb-embed-test runs on the host"; exit 1; fi
	docker run --rm -v "$(CURDIR)/dist/deb/$(DEB_ARCH)":/debs:ro -v "$(CURDIR)/demos/demo-robot/main.cpp":/src/main.cpp:ro -v "$(CURDIR)/packaging/embed-test.sh":/embed-test.sh:ro ubuntu:26.04 sh /embed-test.sh

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

.PHONY: docs-mermaid
docs-mermaid: ## Parse every ```mermaid diagram in docs/, and check the built site still holds its source
	pnpm --filter fjarr-website run check:mermaid

# -------------------------------------------------------------- hygiene ---
.PHONY: fmt
# The C++ is hand-formatted and there is no `.clang-format`, so this target used to
# run clang-format with its LLVM defaults and rewrite every file in the agent: a 115-file diff
# from asking for formatting. Machine-formatting the C++ is a fine idea, but it needs a
# config that matches the code and a commit of its own; until then clang-format-21 is still in the
# image for a deliberate `clang-format-21 -i <file>`. No web package defines a `format` script
# yet, so that line is a no-op the day prettier or similar arrives.
fmt: ## Format Rust (the C++ is hand-formatted, and no web package formats yet — see above)
	cd signaling && cargo fmt
	pnpm -r format 2>/dev/null || true

.PHONY: lint
lint: signaling-clippy web-lint docs-lint agent-raii-gate ## All lints

.PHONY: help
help: ## List targets
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) \
	  | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'
