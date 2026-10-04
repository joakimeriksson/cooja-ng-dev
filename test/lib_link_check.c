/*
 * lib_link_check — the simulator linked as a library, without test/.
 *
 * Built from every object test_runner links except the test frontends
 * (CSIM_LIB_OBJECTS in the Makefile), plus this file.  The point is the
 * link: a kernel, chip or service object that calls a function only the
 * runner defines links fine inside test_runner and fails here.  The body
 * then drives the library entry points a frontend starts with — registry,
 * config, runtime — so they are reachable without the runner too.
 *
 *   make lib-link-check            # default config
 *   build/lib_link_check <config>  # any .yaml/.yml/.json
 */
#include "sim_config.h"
#include "sim_registry.h"
#include "sim_runtime.h"

#include <stdio.h>
#include <string.h>

static sim_registry_t          registry;
static sim_normalized_config_t cfg;
static sim_runtime_t           sim;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "configs/chain-4node-sky.yaml";
    int failed = 0;

    csim_register_builtin_platforms(&registry);
    csim_register_builtin_mote_types(&registry);
    csim_register_builtin_services(&registry);
    csim_register_builtin_media(&registry);
    if (!sim_registry_find_service(&registry, "pcap") ||
        !sim_registry_find_radio_medium(&registry, "udgm")) {
        printf("  FAIL: built-in service/medium not registered\n");
        failed++;
    }

    if (sim_config_load(&cfg, path) != 0) {
        printf("  FAIL: %s did not load\n", path);
        return 1;
    }
    for (int i = 0; i < cfg.node_count; i++) {
        if (!sim_registry_board_for_path(&registry, cfg.nodes[i].firmware)) {
            printf("  FAIL: node %d has no board\n", i);
            failed++;
        }
    }

    sim_runtime_init(&sim);
    sim_runtime_destroy(&sim);
    sim_config_free(&cfg);

    printf("lib-link-check: %s, %d nodes, %d services, %d media: %s\n",
           path, cfg.node_count, registry.service_count, registry.media_count,
           failed ? "FAIL" : "ok");
    return failed ? 1 : 0;
}
