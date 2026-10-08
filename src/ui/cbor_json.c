/*
 * cbor_json — write one CBOR item as JSON text (include/ui/cbor_json.h).
 *
 * A walk over lib/cbor's reader, so the item is read by the same code that
 * defines what the UI's encoder writes: definite-length arrays and maps,
 * integers, strings and the simple values.  Anything outside that subset —
 * indefinite lengths, tags, floats, an integer the reader refuses — is an
 * error, never a guess.
 */
#include "cbor_json.h"
#include "cbor.h"

#define CBOR_JSON_MAX_DEPTH 32

static void put_string(FILE *out, const uint8_t *s, size_t n) {
    fputc('"', out);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c < 0x20 || c == 0x7f) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

static int item(cbor_reader_state_t *r, FILE *out, int depth, int as_key);

static int integer(cbor_reader_state_t *r, FILE *out, int as_key) {
    const char *q = as_key ? "\"" : "";
    if (cbor_peek_next(r) == CBOR_MAJOR_TYPE_UNSIGNED) {
        uint64_t v;
        if (cbor_read_unsigned(r, &v) == CBOR_SIZE_NONE) return -1;
        fprintf(out, "%s%llu%s", q, (unsigned long long)v, q);
    } else {
        int64_t v;
        if (cbor_read_signed(r, &v) == CBOR_SIZE_NONE) return -1;
        fprintf(out, "%s%lld%s", q, (long long)v, q);
    }
    return 0;
}

/* One item.  as_key: a map key, which JSON wants as a string — an integer
 * key is written in quotes, a text key as itself, anything else refused. */
static int item(cbor_reader_state_t *r, FILE *out, int depth, int as_key) {
    if (depth > CBOR_JSON_MAX_DEPTH) return -1;
    switch (cbor_peek_next(r)) {
    case CBOR_MAJOR_TYPE_UNSIGNED:
    case CBOR_MAJOR_TYPE_SIGNED:
        return integer(r, out, as_key);
    case CBOR_MAJOR_TYPE_TEXT_STRING: {
        size_t n;
        const char *s = cbor_read_text(r, &n);
        if (!s) return -1;
        put_string(out, (const uint8_t *)s, n);
        return 0;
    }
    case CBOR_MAJOR_TYPE_BYTE_STRING: {
        size_t n;
        const uint8_t *s;
        if (as_key || !(s = cbor_read_data(r, &n))) return -1;
        fputc('"', out);
        for (size_t i = 0; i < n; i++) fprintf(out, "%02x", s[i]);
        fputc('"', out);
        return 0;
    }
    case CBOR_MAJOR_TYPE_ARRAY:
    case CBOR_MAJOR_TYPE_MAP: {
        if (as_key) return -1;
        int map = cbor_peek_next(r) == CBOR_MAJOR_TYPE_MAP;
        size_t n = map ? cbor_read_map(r) : cbor_read_array(r);
        if (n == SIZE_MAX) return -1;
        fputc(map ? '{' : '[', out);
        for (size_t i = 0; i < n; i++) {
            if (i) fputc(',', out);
            if (map) {
                if (item(r, out, depth + 1, 1)) return -1;
                fputc(':', out);
            }
            if (item(r, out, depth + 1, 0)) return -1;
        }
        fputc(map ? '}' : ']', out);
        return 0;
    }
    case CBOR_MAJOR_TYPE_SIMPLE: {
        if (as_key) return -1;
        switch (cbor_read_simple(r)) {
        case CBOR_SIMPLE_VALUE_FALSE: fputs("false", out); return 0;
        case CBOR_SIMPLE_VALUE_TRUE:  fputs("true", out);  return 0;
        case CBOR_SIMPLE_VALUE_NULL:
        case CBOR_SIMPLE_VALUE_UNDEFINED: fputs("null", out); return 0;
        default: return -1;       /* floats and other simple values */
        }
    }
    default:
        return -1;                /* tags, or nothing left to read */
    }
}

long cbor_item_to_json(const uint8_t *buf, size_t len, FILE *out) {
    cbor_reader_state_t r;
    cbor_init_reader(&r, buf, len);
    if (item(&r, out, 0, 0)) return -1;
    return (long)(len - r.cbor_size);
}
