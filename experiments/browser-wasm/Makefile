# Cooja-NG in the browser — the simulator compiled to WebAssembly.
#
#   source <emsdk>/emsdk_env.sh
#   make -C experiments/browser-wasm          # dist/csim-cli.js + dist/web/
#   make -C experiments/browser-wasm check    # native vs WASM parity
#   make -C experiments/browser-wasm serve    # the site + live demos on :8765
#
# See README.md.
#
# Opt-in: nothing here is reachable from the top-level Makefile, and the
# native build is untouched.  The source lists come from the top-level
# Makefile so the two builds cannot drift; only the flags differ:
# interpreter only (no GNU Lightning), no host tuning, no LTO.

ROOT := $(abspath ../..)

# The top-level Makefile turns the JIT on when pkg-config finds Lightning.
override LIGHTNING_CFLAGS :=
override LIGHTNING_LIBS :=
include $(ROOT)/Makefile
.DEFAULT_GOAL := wasm

EMCC ?= emcc
WOUT := $(abspath build)
DIST := $(abspath dist)

# Every simulator source the native test_runner links, plus the runner itself
# (the scenario frontend lives in test/) and the browser adapter.
WASM_SOURCES := $(SOURCES) $(ARM_SOURCES) $(CHIPS_SOURCES) $(RISCV_SOURCES) \
                $(COMMON_SOURCES) $(SIM_SOURCES) $(SERVICES_SOURCES) \
                $(MOTES_SOURCES) $(NATIVE_SOURCES) $(UI_SOURCES) $(LIB_SOURCES) \
                $(TEST_SOURCES)
VENDOR_SOURCES := $(QUICKJS_SOURCES) $(YAML_SOURCES) $(LINENOISE_SOURCES)

WASM_OBJECTS := $(patsubst %.c,$(WOUT)/%.o,$(WASM_SOURCES))
VENDOR_OBJECTS := $(patsubst %.c,$(WOUT)/%.o,$(VENDOR_SOURCES))
# The browser build swaps the WebSocket server for the worker's message port
# (src/ws_server_worker.c implements the same ws_server.h); nothing else
# differs from the CLI build.
WEB_OBJECTS := $(filter-out $(WOUT)/src/ui/ws_server.o,$(WASM_OBJECTS)) \
               $(WOUT)/experiments/browser-wasm/src/ws_server_worker.o

WASM_CFLAGS := -O3 $(WARNFLAGS) -std=c11 -ffp-contract=off -D_GNU_SOURCE \
               $(INCLUDES) -I include/ui -MMD -MP

COMMON_LDFLAGS := -lm -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB

# A drop-in for build/test_runner under Node: same argv, reads the repo's
# files directly.  This is the parity oracle (`make check`).
CLI_LDFLAGS := $(COMMON_LDFLAGS) -sNODERAWFS=1 -sENVIRONMENT=node \
               -sEXIT_RUNTIME=1

# The browser build: runs in a Web Worker, firmware through MEMFS, and the
# runner loop yields to the worker's event loop (ASYNCIFY) so the page can
# pause, step and talk to the UI service while it runs.
WEB_LDFLAGS := $(COMMON_LDFLAGS) -sENVIRONMENT=worker -sMODULARIZE=1 \
               -sEXPORT_NAME=createCsim -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
               -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 -sASYNCIFY_IGNORE_INDIRECT=1 \
               -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_csim_wasm_ui_recv \
               -sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8

.PHONY: wasm cli web site check serve profile clean-wasm

wasm: cli web

cli: $(DIST)/csim-cli.js
# dist/web drops into the GitHub Pages site as is: player.html (the web UI,
# as the site already ships it) runs a scenario live with ?wasm=NAME.
web: $(DIST)/web/csim-web.js $(DIST)/web/player.html $(DIST)/web/worker.js \
     $(DIST)/web/scenarios.json

$(WOUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(EMCC) $(WASM_CFLAGS) -c $< -o $@

# Vendored code: built quiet, like the native rules do.  QuickJS tests the
# pre-2.0 `EMSCRIPTEN` macro; current emcc defines only __EMSCRIPTEN__.
$(WOUT)/lib/quickjs/%.o: $(ROOT)/lib/quickjs/%.c
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(EMCC) -O2 -std=c11 -ffp-contract=off -D_GNU_SOURCE -DEMSCRIPTEN -I lib/quickjs -DCONFIG_VERSION=\"2024-01-13\" -w -c $< -o $@
$(WOUT)/lib/yaml/%.o: $(ROOT)/lib/yaml/%.c
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(EMCC) -O2 -std=c11 -D_GNU_SOURCE -I lib/yaml $(YAML_DEFS) -w -c $< -o $@
$(WOUT)/lib/linenoise/%.o: $(ROOT)/lib/linenoise/%.c
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(EMCC) -O2 -std=c11 -D_GNU_SOURCE -I lib/linenoise -w -c $< -o $@

$(DIST)/csim-cli.js: $(WASM_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(EMCC) $^ -o $@ $(CLI_LDFLAGS)

$(DIST)/web/csim-web.js: $(WEB_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(EMCC) $^ -o $@ $(WEB_LDFLAGS)

$(DIST)/web/player.html: $(ROOT)/ui/index.html
	@mkdir -p $(dir $@)
	cp $< $@

$(DIST)/web/worker.js: web/worker.js
	@mkdir -p $(dir $@)
	cp $< $@

# The scenarios the page offers, and the firmware and configs they read,
# served as static files under files/ at their repository paths.
SCENARIO_FILES := $(shell python3 -c "import json;print(' '.join(sorted({f for v in json.load(open('web/scenarios.json')).values() for f in v['files']})))")

$(DIST)/web/scenarios.json: web/scenarios.json $(addprefix $(ROOT)/,$(SCENARIO_FILES))
	@mkdir -p $(dir $@)
	cd $(ROOT) && for f in $(SCENARIO_FILES); do mkdir -p $(DIST)/web/files/$$(dirname $$f) && cp $$f $(DIST)/web/files/$$f; done
	cp $< $@

# The web build's objects and ASYNCIFY, run under Node: for measuring what
# the UI service and the yields cost without a browser in the way.
profile: $(DIST)/csim-ui-node.js

$(DIST)/csim-ui-node.js: $(WEB_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(EMCC) $^ -o $@ $(COMMON_LDFLAGS) -sNODERAWFS=1 -sENVIRONMENT=node \
	    -sEXIT_RUNTIME=1 -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 -sASYNCIFY_IGNORE_INDIRECT=1 \
	    --profiling-funcs

check: cli
	./check-parity.sh

# The landing page with the live build beside it, as the Pages workflow
# assembles it (without the recorded replays unless DEMOS=<dir> points at
# the output of tools/record-demos.sh).
site: web
	rm -rf $(DIST)/site && mkdir -p $(DIST)/site
	cp -r $(DIST)/web/. $(DIST)/site/
	cp -r $(ROOT)/site/. $(DIST)/site/
	$(if $(DEMOS),cp -r $(DEMOS) $(DIST)/site/demos)

PORT ?= 8765
serve: site
	cd $(DIST)/site && python3 -m http.server $(PORT) --bind 127.0.0.1

clean-wasm:
	rm -rf $(WOUT) $(DIST)

-include $(WASM_OBJECTS:.o=.d)
