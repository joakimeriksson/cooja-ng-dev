/*
 * cbor_json — write one CBOR item as JSON text (include/ui/cbor_json.h).
 */
#include "cbor_json.h"

#include <math.h>
#include <string.h>

#define CBOR_JSON_MAX_DEPTH 32

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
    FILE *out;
} rd_t;

/* The argument of a head: the value of an integer, the length of a string,
 * array or map, the raw bits of a float.  *indef is set for additional info
 * 31; *ai_out (if given) receives the additional info itself. */
static int head_ai(rd_t *r, int *major, uint64_t *arg, int *indef, int *ai_out) {
    if (r->pos >= r->len) return -1;
    uint8_t b = r->buf[r->pos++];
    *major = b >> 5;
    int ai = b & 0x1f;
    if (ai_out) *ai_out = ai;
    *indef = 0;
    if (ai < 24) { *arg = (uint64_t)ai; return 0; }
    if (ai == 31) { *indef = 1; *arg = 0; return 0; }
    if (ai > 27) return -1;
    int n = 1 << (ai - 24);                      /* 1, 2, 4 or 8 bytes */
    if (r->len - r->pos < (size_t)n) return -1;
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | r->buf[r->pos++];
    *arg = v;
    return 0;
}

static int head(rd_t *r, int *major, uint64_t *arg, int *indef) {
    return head_ai(r, major, arg, indef, NULL);
}

static void put_text(rd_t *r, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') { fputc('\\', r->out); fputc(c, r->out); }
        else if (c == '\n') fputs("\\n", r->out);
        else if (c == '\r') fputs("\\r", r->out);
        else if (c == '\t') fputs("\\t", r->out);
        else if (c < 0x20 || c == 0x7f) fprintf(r->out, "\\u%04x", c);
        else fputc(c, r->out);
    }
}

static double half_to_double(uint16_t h) {
    int e = (h >> 10) & 0x1f, m = h & 0x3ff;
    double v = e == 0 ? ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY)
                      : ldexp(m + 1024, e - 25);
    return (h & 0x8000) ? -v : v;
}

static int item(rd_t *r, int depth);

/* A text or byte string: definite, or indefinite as definite chunks. */
static int string(rd_t *r, int major, uint64_t n, int indef) {
    fputc('"', r->out);
    for (;;) {
        if (indef) {
            if (r->pos >= r->len) return -1;
            if (r->buf[r->pos] == 0xff) { r->pos++; break; }
            int cm, ci; uint64_t cn;
            if (head(r, &cm, &cn, &ci) || cm != major || ci) return -1;
            n = cn;
        }
        if (n > r->len - r->pos) return -1;
        const uint8_t *s = r->buf + r->pos;
        if (major == 3) put_text(r, s, (size_t)n);
        else for (uint64_t i = 0; i < n; i++) fprintf(r->out, "%02x", s[i]);
        r->pos += (size_t)n;
        if (!indef) break;
    }
    fputc('"', r->out);
    return 0;
}

/* Map keys must be JSON strings: an integer key is written in quotes, a
 * text key as itself. */
static int key(rd_t *r, int depth) {
    if (r->pos >= r->len) return -1;
    int major = r->buf[r->pos] >> 5;
    if (major == 0 || major == 1) {
        int m, indef; uint64_t v;
        if (head(r, &m, &v, &indef)) return -1;
        if (m == 0) fprintf(r->out, "\"%llu\"", (unsigned long long)v);
        else fprintf(r->out, "\"-%llu\"", (unsigned long long)v + 1);
        return 0;
    }
    if (major == 3) return item(r, depth);
    return -1;
}

static int item(rd_t *r, int depth) {
    if (depth > CBOR_JSON_MAX_DEPTH) return -1;
    int major, indef, ai; uint64_t arg;
    if (head_ai(r, &major, &arg, &indef, &ai)) return -1;
    switch (major) {
    case 0:
        fprintf(r->out, "%llu", (unsigned long long)arg);
        return 0;
    case 1:
        fprintf(r->out, "-%llu", (unsigned long long)arg + 1);
        return 0;
    case 2: case 3:
        return string(r, major, arg, indef);
    case 4: case 5: {
        fputc(major == 4 ? '[' : '{', r->out);
        for (uint64_t i = 0; indef || i < arg; i++) {
            if (indef) {
                if (r->pos >= r->len) return -1;
                if (r->buf[r->pos] == 0xff) { r->pos++; break; }
            }
            if (i) fputc(',', r->out);
            if (major == 5) {
                if (key(r, depth + 1)) return -1;
                fputc(':', r->out);
            }
            if (item(r, depth + 1)) return -1;
        }
        fputc(major == 4 ? ']' : '}', r->out);
        return 0;
    }
    case 6:                                     /* tag: write the content */
        return item(r, depth + 1);
    case 7: {
        if (indef) return -1;                   /* a stray break */
        double d;
        if (arg == 20) { fputs("false", r->out); return 0; }
        if (arg == 21) { fputs("true", r->out); return 0; }
        if (arg == 22 || arg == 23) { fputs("null", r->out); return 0; }
        /* Floats: the head read their bits as an integer argument. */
        if (ai == 25) d = half_to_double((uint16_t)arg);
        else if (ai == 26) {
            uint32_t u = (uint32_t)arg; float f; memcpy(&f, &u, 4); d = f;
        } else if (ai == 27) {
            memcpy(&d, &arg, 8);
        } else {
            fputs("null", r->out);              /* other simple values */
            return 0;
        }
        if (isfinite(d)) fprintf(r->out, "%.17g", d);
        else fputs("null", r->out);
        return 0;
    }
    }
    return -1;
}

long cbor_item_to_json(const uint8_t *buf, size_t len, FILE *out) {
    rd_t r = { buf, len, 0, out };
    if (item(&r, 0)) return -1;
    return (long)r.pos;
}
