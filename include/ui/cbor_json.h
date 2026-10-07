/*
 * cbor_json — write one CBOR item as JSON text.
 *
 * The UI recorder (websocket_ui_service.c, --ui-record) stores each CBOR
 * delta the server broadcasts as JSON, so a recording is the exact wire
 * stream in the form the browser's replay player reads.  Map keys become
 * JSON strings ("0".."6", node ids), which is how a decoded CBOR map
 * already behaves in JavaScript.  Byte strings (unused by the UI) are
 * written as hex strings; undefined as null.
 */
#ifndef CBOR_JSON_H
#define CBOR_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Write the CBOR item at buf[0..len) to out as JSON.  Returns the number of
 * bytes consumed, or -1 if the item is truncated, nested too deeply or uses
 * an encoding outside RFC 8949 — nothing is guaranteed to have been written
 * correctly in that case. */
long cbor_item_to_json(const uint8_t *buf, size_t len, FILE *out);

#endif /* CBOR_JSON_H */
