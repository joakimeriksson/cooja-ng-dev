# Replaying a recorded run in the web UI

`ui/index.html?replay=FILE.json` plays a recorded run from a static file: no simulator, no
WebSocket, no server beyond whatever serves the two files. Play/Pause, the speed slider and a
scrubber in the header drive the player; Restart seeks to the start. Dragging nodes and the
live commands are inert in this mode.

The file is the UI's own wire protocol as one JSON document:

```json
{ "format": "cooja-ng-ui-replay/1",
  "run":    { "simulator": "cooja-ng", "version": "…", "seed": 42, "verdict": "pass", "scenario": "…" },
  "full":   { "type": "full", "sim_time_ms": 0, "nodes": [...], "radio": {...}, "stats": {...} },
  "deltas": [ { "0": 100, "1": [rf, uart, frames, collisions, speed_x10, paused],
                "2": {"3": 1}, "3": {"3": [1,0,0]}, "4": {"1": 4628},
                "5": {"3": ["[INFO: Main ] Starting Contiki-NG…"]},
                "6": [[615575, 3, 1, 0], [4624709, 1, 7, 1, "DATA N1 -> bcast seq 198 len 97 ch 26"]] }, ... ] }
```

A delta uses the numeric keys `handleCBORDelta` reads, as strings: `"0"` sim_time_ms, `"1"`
stats, `"2"` radio state per node (off=0 on=1 tx=2 rx=3 intf=4), `"3"` LEDs per node, `"4"`
last_tx_ms per node, `"5"` console lines per node, `"6"` timeline entries
`[t_us, node, type, value, summary?]` with type off, on, tx, rx, intf, led_on, led_off, frame,
log = 0..8. Deltas are applied in order; seeking backwards rebuilds from `full`.

Producing the file: `tools/rundir2ui.py RUN_DIR` in the agent-sim-protocol repo converts a
`--run-dir` directory (`events.ndjson`, `scenario.replay.yaml`, `result.json`) into it, one
delta per 100 ms of simulation time that had events. A recorder in the WebSocket service that
writes the same document during a live or headless run is the planned second producer.

Serving a demo: put `ui/index.html` and the replay file in one directory and serve it
statically (`python3 -m http.server`, GitHub Pages, any file host).
