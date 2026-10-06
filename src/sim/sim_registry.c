/*
 * sim_registry — generic registry container + the service catalog.
 *
 * See include/sim/sim_registry.h.  Board glue lives in sim_board.c and
 * mote-kind glue in mote_kinds.c (each domain registers itself and implements
 * its own forwarding accessor), and so do the services
 * (src/services/builtin_services.c); this file owns the name → ops service
 * and radio-medium catalogs.
 */
#include "sim_registry.h"

#include <string.h>

int sim_registry_register_service(sim_registry_t *r,
                                  const sim_service_ops_t *ops) {
    if (!r || !ops || !ops->name)
        return -1;
    /* Reject a duplicate name (the catalog is a set keyed by name) and a full
     * table. */
    if (sim_registry_find_service(r, ops->name))
        return -1;
    if (r->service_count >= SIM_REGISTRY_MAX_SERVICES)
        return -1;
    int idx = r->service_count++;
    r->services[idx] = ops;
    return idx;
}

const sim_service_ops_t *sim_registry_find_service(const sim_registry_t *r,
                                                   const char *name) {
    if (!r || !name)
        return NULL;
    for (int i = 0; i < r->service_count; i++) {
        const sim_service_ops_t *ops = r->services[i];
        if (ops && ops->name && strcmp(ops->name, name) == 0)
            return ops;
    }
    return NULL;
}

/* --- Radio-medium catalog (Phase 11), mirroring the service catalog. --- */

int sim_registry_register_radio_medium(sim_registry_t *r,
                                       const sim_medium_type_t *m) {
    if (!r || !m || !m->name)
        return -1;
    if (sim_registry_find_radio_medium(r, m->name))
        return -1;
    if (r->media_count >= SIM_REGISTRY_MAX_MEDIA)
        return -1;
    int idx = r->media_count++;
    r->media[idx] = m;
    return idx;
}

const sim_medium_type_t *sim_registry_find_radio_medium(const sim_registry_t *r,
                                                        const char *name) {
    if (!r || !name)
        return NULL;
    for (int i = 0; i < r->media_count; i++) {
        const sim_medium_type_t *m = r->media[i];
        if (m && m->name && strcmp(m->name, name) == 0)
            return m;
    }
    return NULL;
}

/* The built-in media: UDGM (the full filter pipeline) + NONE (the all-to-all
 * bypass).  Their ops are radio_medium.c's csim_udgm_ops / csim_none_ops. */
void csim_register_builtin_media(sim_registry_t *r) {
    if (!r) return;
    static const sim_medium_type_t udgm = {
        .name = "udgm", .pipeline = RADIO_MEDIUM_UDGM, .ops = &csim_udgm_ops };
    static const sim_medium_type_t none = {
        .name = "none", .pipeline = RADIO_MEDIUM_NONE, .ops = &csim_none_ops };
    sim_registry_register_radio_medium(r, &udgm);
    sim_registry_register_radio_medium(r, &none);
}
