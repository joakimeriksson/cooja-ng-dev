/*
 * cbor_json unit tests — the --ui-record transcoder (src/ui/cbor_json.c).
 *
 * Builds the shapes sim_state_delta_cbor writes with lib/cbor's writer and
 * checks the JSON byte for byte, then feeds it the encodings lib/cbor's
 * reader does not accept (indefinite lengths, tags, floats, an integer
 * below INT64_MIN, truncation) and checks each one is refused rather than
 * written as something plausible.
 */
#include "cbor.h"
#include "cbor_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

/* Transcode buf[0..len); returns the JSON (caller frees) and sets *used. */
static char *to_json(const uint8_t *buf, size_t len, long *used) {
    char *js = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&js, &n);
    *used = cbor_item_to_json(buf, len, m);
    fclose(m);
    return js;
}

static void expect_json(const char *what, const uint8_t *buf, size_t len,
                        const char *want) {
    long used;
    char *got = to_json(buf, len, &used);
    if (used == (long)len && strcmp(got, want) == 0) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s\n    want %s\n    got  %s (used %ld of %zu)\n",
               what, want, got, used, len);
    }
    free(got);
}

static void expect_refused(const char *what, const uint8_t *buf, size_t len) {
    long used;
    char *got = to_json(buf, len, &used);
    if (used < 0) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s: accepted as %s\n", what, got);
    }
    free(got);
}

int run_cbor_json_tests(int verbose) {
    passed = failed = 0;
    printf("=== cbor_json (--ui-record transcoder) ===\n");

    /* A delta as sim_state_delta_cbor lays it out: integer keys, a stats
     * array with a bool, a per-node map, console text with characters JSON
     * must escape, and a nested timeline array. */
    uint8_t buf[512];
    cbor_writer_state_t w;
    cbor_init_writer(&w, buf, sizeof buf);
    cbor_open_map(&w);
      cbor_write_unsigned(&w, 0); cbor_write_unsigned(&w, 4628);
      cbor_write_unsigned(&w, 1);
      cbor_open_array(&w);
        cbor_write_unsigned(&w, 12); cbor_write_unsigned(&w, 300);
        cbor_write_bool(&w, false);
      cbor_close_array(&w);
      cbor_write_unsigned(&w, 2);
      cbor_open_map(&w);
        cbor_write_unsigned(&w, 3); cbor_write_unsigned(&w, 1);
      cbor_close_map(&w);
      cbor_write_unsigned(&w, 5);
      cbor_open_map(&w);
        cbor_write_unsigned(&w, 1);
        cbor_open_array(&w);
          cbor_write_text(&w, "a \"q\"\\\n\x01", 8);
        cbor_close_array(&w);
      cbor_close_map(&w);
      cbor_write_unsigned(&w, 6);
      cbor_open_array(&w);
        cbor_open_array(&w);
          cbor_write_unsigned(&w, 615575); cbor_write_unsigned(&w, 3);
          cbor_write_unsigned(&w, 7); cbor_write_signed(&w, -1);
          cbor_write_text(&w, "DATA N1", 7);
        cbor_close_array(&w);
      cbor_close_array(&w);
    cbor_close_map(&w);
    size_t n = cbor_end_writer(&w);
    expect_json("delta layout", buf, n,
                "{\"0\":4628,\"1\":[12,300,false],\"2\":{\"3\":1},"
                "\"5\":{\"1\":[\"a \\\"q\\\"\\\\\\n\\u0001\"]},"
                "\"6\":[[615575,3,7,-1,\"DATA N1\"]]}");

    /* Scalars at the edges the reader does accept. */
    const uint8_t u64max[] = { 0x1b, 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff };
    expect_json("UINT64_MAX", u64max, sizeof u64max, "18446744073709551615");
    const uint8_t i64min[] = { 0x3b, 0x7f,0xff,0xff,0xff, 0xff,0xff,0xff,0xff };
    expect_json("INT64_MIN", i64min, sizeof i64min, "-9223372036854775808");
    const uint8_t simple[] = { 0x84, 0xf4, 0xf5, 0xf6, 0xf7 };
    expect_json("false/true/null/undefined", simple, sizeof simple,
                "[false,true,null,null]");
    const uint8_t bytes[] = { 0x42, 0xab, 0x01 };
    expect_json("byte string as hex", bytes, sizeof bytes, "\"ab01\"");
    const uint8_t textkey[] = { 0xa1, 0x61, 'k', 0x01 };
    expect_json("text map key", textkey, sizeof textkey, "{\"k\":1}");

    /* What the reader refuses — each must fail, not be approximated. */
    const uint8_t neg_u64max[] = { 0x3b, 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff };
    expect_refused("negative below INT64_MIN", neg_u64max, sizeof neg_u64max);
    const uint8_t indef_array[] = { 0x9f, 0x01, 0xff };
    expect_refused("indefinite array", indef_array, sizeof indef_array);
    const uint8_t indef_map[] = { 0xbf, 0x01, 0x02, 0xff };
    expect_refused("indefinite map", indef_map, sizeof indef_map);
    const uint8_t tag[] = { 0xc1, 0x01 };
    expect_refused("tag", tag, sizeof tag);
    const uint8_t tag_ai31[] = { 0xdf, 0x01 };
    expect_refused("tag with additional info 31", tag_ai31, sizeof tag_ai31);
    const uint8_t half[] = { 0xf9, 0x3c, 0x00 };
    expect_refused("half float", half, sizeof half);
    const uint8_t dbl[] = { 0xfb, 0x3f,0xf0,0,0, 0,0,0,0 };
    expect_refused("double", dbl, sizeof dbl);
    const uint8_t trunc_text[] = { 0x65, 'a', 'b' };
    expect_refused("truncated text", trunc_text, sizeof trunc_text);
    const uint8_t trunc_map[] = { 0xa2, 0x01, 0x02 };
    expect_refused("map missing an entry", trunc_map, sizeof trunc_map);
    const uint8_t array_key[] = { 0xa1, 0x80, 0x01 };
    expect_refused("array as a map key", array_key, sizeof array_key);
    uint8_t deep[40];
    memset(deep, 0x81, sizeof deep - 1);
    deep[sizeof deep - 1] = 0x01;
    expect_refused("nesting beyond the depth cap", deep, sizeof deep);

    printf("  %d passed, %d failed\n", passed, failed);
    (void)verbose;
    return failed;
}
