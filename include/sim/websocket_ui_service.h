/*
 * websocket_ui_service — the live WebSocket UI (state viewer) as a kernel
 * service.
 *
 * Phase 6 milestone 39 (§3.19).  Owns the ws_server, the per-node console
 * ring buffers, the UI control flags (speed / paused / full-state /
 * restart), the incoming-message handler, and the outgoing state
 * serialization (full-state JSON on connect, CBOR deltas thereafter).
 *
 * Per the §3.19 Decision-3 boundary, the runner keeps what is genuinely
 * loop control or shared with the RF path: the time-advance / pause /
 * restart decisions, the wall-clock pacing (t_start is shared with
 * serial-socket mode and the end-of-run perf print), and node_states[] /
 * update_radio_state / update_led_state (the RF-frame callbacks also write
 * node_states).  The service reads that shared state through pointers and a
 * node-describe callback handed in at start, so it needs no nodes[] access;
 * the loop consults the query functions below.
 *
 * NOT byte-identical on its own path: the pacing is wall-clock, so output
 * ordering of ws polls is inherently nondeterministic.  The headless
 * (non-UI) path stays byte-identical — the service is dormant unless
 * ws_server_init() succeeded.
 */
#ifndef WEBSOCKET_UI_SERVICE_H
#define WEBSOCKET_UI_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sim_service.h"
#include "ws_server.h"
#include "sim_state.h"
#include "timeline.h"
#include "radio_medium.h"
#include "sim_event_queue.h"   /* SIM_EQ_MAX_NODES */
#include "sim_control.h"       /* pause/speed/move go through the control API */

#ifdef __cplusplus
extern "C" {
#endif

#define UI_SVC_MAX_NODES   SIM_EQ_MAX_NODES
#define UI_CONSOLE_LINES   20
#define UI_CONSOLE_LINELEN 256

/* Fill the per-node scalars the full-state/delta serialization needs
 * (the service supplies x/y/last_tx from its stored medium/last_tx
 * pointers, and the console from its own rings). */
typedef void (*ui_describe_fn)(int i, int *id, const char **type,
                               int64_t *cycles, uint32_t *freq,
                               int64_t *sim_time);

typedef struct websocket_ui_service {
    ws_server_t *server;          /* NULL = inactive                       */

    /* Console ring buffers (full history + new-since-last-broadcast). */
    char  console[UI_SVC_MAX_NODES][UI_CONSOLE_LINES][UI_CONSOLE_LINELEN];
    int   console_head[UI_SVC_MAX_NODES];
    int   console_count[UI_SVC_MAX_NODES];
    char  console_new[UI_SVC_MAX_NODES][UI_CONSOLE_LINES][UI_CONSOLE_LINELEN];
    int   console_new_count[UI_SVC_MAX_NODES];

    /* Control flags driven by ui_message_handler / the loop.  Pause and
     * speed live in sim_control now (shared with the shell and the loop);
     * the UI only keeps what is UI-private. */
    int    full_state_requested;  /* send full state on next broadcast      */
    int    restart_requested;

    /* Shared runner state (pointers set at start; service never writes the
     * node_states it doesn't own — it does own prev_* delta snapshots). */
    const sim_node_state_t *node_states;
    sim_node_state_t       *prev_node_states;
    const int64_t          *node_last_tx_ns;
    int64_t                *prev_last_tx_ns;
    radio_medium_t         *medium;
    timeline_t             *tl;
    const int              *node_count;
    ui_describe_fn          describe;
    sim_control_t          *ctl;      /* pause/play/speed/move target       */

    /* Live global stats the runner refreshes each broadcast. */
    int stat_rf_bytes, stat_uart_bytes;
    int stat_rx_frames_queued, stat_rx_frames_collided;

    /* Runtime handle for plugin UI panels (set by the runner after start);
     * NULL = no panel source.  ui_service_broadcast reads the published
     * panels through it and ships a {"type":"panels"} frame. */
    struct sim_runtime *rt;

    /* --ui-record: the broadcast stream written to a file as one
     * cooja-ng-ui-replay/1 document (docs/ui-replay.md) for the browser's
     * replay player.  The file is created once, before the run starts
     * (ui_service_record_open); the document begins when the run does
     * (ui_service_record_begin) and is finished once — at the run's end, at
     * a restart, or at destroy.  While it records, the recorder counts as a
     * client that is always connected, so deltas are built with no browser
     * attached.  record == NULL: no file open (never asked, or finished). */
    FILE   *record;
    char    record_path[512];
    bool    record_begun;         /* the document's header is written     */
    long    record_deltas;        /* deltas written; -1 = no full state yet */
    int64_t record_end_ns;        /* stop recording here (the run's end)  */
    int64_t record_last_ns;       /* sim time of the last recorded tick   */
    bool    record_failed;        /* a write or the close failed          */
} websocket_ui_service_t;

/* Start the UI: open the ws_server on `bind_addr`:`port` (NULL = loopback,
 * see ws_server_init), load ui/index.html, wire the
 * message handler, and store the shared-state pointers/callbacks.  Returns
 * true if the server came up (the service is then "active").  A failure
 * leaves the service inactive; what that means is the caller's: the
 * runner exits when an explicit --ui cannot start, the shell's `ui`
 * command reports it and the run goes on. */
bool ui_service_start(websocket_ui_service_t *svc, const char *bind_addr, int port,
                      const sim_node_state_t *node_states,
                      sim_node_state_t *prev_node_states,
                      const int64_t *node_last_tx_ns,
                      int64_t *prev_last_tx_ns,
                      radio_medium_t *medium, timeline_t *tl,
                      const int *node_count,
                      ui_describe_fn describe, sim_control_t *ctl);

static inline bool ui_service_active(const websocket_ui_service_t *svc) {
    return svc->server != NULL;
}

/* --ui-record, in two steps so a bad path fails before anything starts and
 * a restart can never re-open (and so empty) the file:
 *
 * ui_service_record_open creates `path`, nothing more; call it while
 * validating arguments.  Returns false (with a message) if it cannot.
 *
 * ui_service_record_begin writes the document's start and starts recording
 * the stream until simulation time `end_ns`, a restart, or
 * ui_service_destroy — whichever comes first; the document is then
 * finished.  A no-op unless the file is open and not yet begun, so the
 * runner calls it on every pass and only the first one counts.  `run_json`
 * is the document's "run" object, written as given.  The state pointers are
 * ui_service_start's; with --ui the two calls store the same values.  Works
 * with or without the server: without it the run is headless and unpaced,
 * and only the recording sees the stream.
 *
 * The recorder writes nothing to stdout: its two status lines go to stderr,
 * so a run's stdout is byte-identical with and without --ui-record. */
bool ui_service_record_open(websocket_ui_service_t *svc, const char *path);
void ui_service_record_begin(websocket_ui_service_t *svc,
                             const char *run_json, int64_t end_ns,
                             const sim_node_state_t *node_states,
                             sim_node_state_t *prev_node_states,
                             const int64_t *node_last_tx_ns,
                             int64_t *prev_last_tx_ns,
                             radio_medium_t *medium, timeline_t *tl,
                             const int *node_count,
                             ui_describe_fn describe, sim_control_t *ctl);

/* True if writing the recording failed (full disk, I/O error): the file
 * is incomplete, and the runner fails the run rather than report it. */
static inline bool ui_service_record_failed(const websocket_ui_service_t *svc) {
    return svc->record_failed;
}

/* True while anything consumes the UI stream — a server or a recording.
 * Gates the runner's UI-only state tracking (radio/LED state, frame
 * events, console lines) and the broadcast; ui_service_active() alone
 * gates what only a live server needs (pacing, polling, the run going on
 * after its end). */
static inline bool ui_service_observing(const websocket_ui_service_t *svc) {
    return svc->server != NULL || (svc->record != NULL && svc->record_begun);
}
static inline bool ui_service_restart_requested(const websocket_ui_service_t *svc) {
    return svc->restart_requested != 0;
}
static inline void ui_service_clear_restart(websocket_ui_service_t *svc) {
    svc->restart_requested = 0;
}

/* Poll the socket (process incoming frames / accept clients). */
void ui_service_poll(websocket_ui_service_t *svc);

/* Append a timestamped console line for node slot `idx`. */
void ui_service_add_console_line(websocket_ui_service_t *svc, int idx,
                                 int64_t sim_ns, const char *line);

/* Refresh the live global stats used by the next broadcast. */
void ui_service_set_stats(websocket_ui_service_t *svc, int rf_bytes,
                          int uart_bytes, int rx_frames_queued,
                          int rx_frames_collided);

/* Serialize and broadcast the current state (full-state JSON on the first
 * post-connect call, CBOR delta thereafter). */
void ui_service_broadcast(websocket_ui_service_t *svc, int64_t sim_ns);

/* Clear console rings + delta snapshots (UI restart). */
void ui_service_reset(websocket_ui_service_t *svc);

/* Close the socket, and finish the recording if one is open. */
void ui_service_destroy(websocket_ui_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_UI_SERVICE_H */
