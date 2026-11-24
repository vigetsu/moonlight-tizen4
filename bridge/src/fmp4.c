#include "fmp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMESCALE 90000

/* ---------- byte buffer ---------- */

static void bb_reserve(bytebuf_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *d = realloc(b->data, cap);
    if (!d) abort();
    b->data = d;
    b->cap = cap;
}

void bb_free(bytebuf_t *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static void put(bytebuf_t *b, const void *p, size_t n) {
    bb_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void u8(bytebuf_t *b, uint8_t v) { put(b, &v, 1); }
static void u16(bytebuf_t *b, uint16_t v) { uint8_t x[2] = { v >> 8, v & 0xff }; put(b, x, 2); }
static void u32(bytebuf_t *b, uint32_t v) {
    uint8_t x[4] = { v >> 24, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff };
    put(b, x, 4);
}
static void u64(bytebuf_t *b, uint64_t v) { u32(b, (uint32_t)(v >> 32)); u32(b, (uint32_t)v); }
static void zeros(bytebuf_t *b, size_t n) { bb_reserve(b, n); memset(b->data + b->len, 0, n); b->len += n; }
static void set32(bytebuf_t *b, size_t at, uint32_t v) {
    b->data[at] = v >> 24; b->data[at + 1] = (v >> 16) & 0xff;
    b->data[at + 2] = (v >> 8) & 0xff; b->data[at + 3] = v & 0xff;
}

/* Opens a box and returns its offset; close with box_end. */
static size_t box(bytebuf_t *b, const char *type) {
    size_t at = b->len;
    u32(b, 0);
    put(b, type, 4);
    return at;
}
static size_t fullbox(bytebuf_t *b, const char *type, uint8_t ver, uint32_t flags) {
    size_t at = box(b, type);
    u32(b, ((uint32_t)ver << 24) | flags);
    return at;
}
static void box_end(bytebuf_t *b, size_t at) { set32(b, at, (uint32_t)(b->len - at)); }

static void matrix(bytebuf_t *b) {
    static const uint32_t m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
    for (int i = 0; i < 9; i++) u32(b, m[i]);
}

/* ---------- init segment ---------- */

static void write_init(fmp4_t *m, bytebuf_t *b) {
    size_t ftyp = box(b, "ftyp");
    put(b, "isom", 4); u32(b, 0x200);
    put(b, "isomiso2avc1iso6mp41", 20);
    box_end(b, ftyp);

    size_t moov = box(b, "moov");
    size_t mvhd = fullbox(b, "mvhd", 0, 0);
    u32(b, 0); u32(b, 0); u32(b, 1000); u32(b, 0);
    u32(b, 0x00010000); u16(b, 0x0100); zeros(b, 10);
    matrix(b); zeros(b, 24); u32(b, 2);
    box_end(b, mvhd);

    size_t trak = box(b, "trak");
    size_t tkhd = fullbox(b, "tkhd", 0, 3);
    u32(b, 0); u32(b, 0); u32(b, 1); u32(b, 0); u32(b, 0);
    zeros(b, 8); u16(b, 0); u16(b, 0); u16(b, 0); u16(b, 0);
    matrix(b);
    u32(b, (uint32_t)m->width << 16); u32(b, (uint32_t)m->height << 16);
    box_end(b, tkhd);

    size_t mdia = box(b, "mdia");
    size_t mdhd = fullbox(b, "mdhd", 0, 0);
    u32(b, 0); u32(b, 0); u32(b, TIMESCALE); u32(b, 0); u16(b, 0x55c4); u16(b, 0);
    box_end(b, mdhd);
    size_t hdlr = fullbox(b, "hdlr", 0, 0);
    u32(b, 0); put(b, "vide", 4); zeros(b, 12); put(b, "VideoHandler", 13);
    box_end(b, hdlr);

    size_t minf = box(b, "minf");
    size_t vmhd = fullbox(b, "vmhd", 0, 1);
    zeros(b, 8);
    box_end(b, vmhd);
    size_t dinf = box(b, "dinf");
    size_t dref = fullbox(b, "dref", 0, 0);
    u32(b, 1);
    size_t url = fullbox(b, "url ", 0, 1);
    box_end(b, url);
    box_end(b, dref);
    box_end(b, dinf);

    size_t stbl = box(b, "stbl");
    size_t stsd = fullbox(b, "stsd", 0, 0);
    u32(b, 1);
    size_t avc1 = box(b, "avc1");
    zeros(b, 6); u16(b, 1); zeros(b, 16);
    u16(b, (uint16_t)m->width); u16(b, (uint16_t)m->height);
    u32(b, 0x00480000); u32(b, 0x00480000); u32(b, 0); u16(b, 1);
    zeros(b, 32); u16(b, 0x0018); u16(b, 0xffff);
    size_t avcc = box(b, "avcC");
    u8(b, 1); u8(b, m->sps[1]); u8(b, m->sps[2]); u8(b, m->sps[3]);
    u8(b, 0xff); u8(b, 0xe1);
    u16(b, (uint16_t)m->sps_len); put(b, m->sps, m->sps_len);
    u8(b, 1); u16(b, (uint16_t)m->pps_len); put(b, m->pps, m->pps_len);
    box_end(b, avcc);
    box_end(b, avc1);
    box_end(b, stsd);
    size_t t;
    t = fullbox(b, "stts", 0, 0); u32(b, 0); box_end(b, t);
    t = fullbox(b, "stsc", 0, 0); u32(b, 0); box_end(b, t);
    t = fullbox(b, "stsz", 0, 0); u32(b, 0); u32(b, 0); box_end(b, t);
    t = fullbox(b, "stco", 0, 0); u32(b, 0); box_end(b, t);
    box_end(b, stbl);
    box_end(b, minf);
    box_end(b, mdia);
    box_end(b, trak);

    size_t mvex = box(b, "mvex");
    size_t trex = fullbox(b, "trex", 0, 0);
    u32(b, 1); u32(b, 1); u32(b, 0); u32(b, 0); u32(b, 0);
    box_end(b, trex);
    box_end(b, mvex);
    box_end(b, moov);
}

/* ---------- media segments ---------- */

void fmp4_init(fmp4_t *m, int width, int height, int fps) {
    memset(m, 0, sizeof(*m));
    m->width = width;
    m->height = height;
    m->fps = fps > 0 ? fps : 60;
}

/* Finds the next start code at or after p; returns its position and length. */
static const uint8_t *next_start(const uint8_t *p, const uint8_t *end, int *sc_len) {
    for (; p + 3 <= end; p++) {
        if (p[0] == 0 && p[1] == 0) {
            if (p[2] == 1) { *sc_len = 3; return p; }
            if (p + 4 <= end && p[2] == 0 && p[3] == 1) { *sc_len = 4; return p; }
        }
    }
    *sc_len = 0;
    return end;
}

bool fmp4_mux_annexb(fmp4_t *m, const uint8_t *au, size_t len, bool keyframe,
                     bytebuf_t *init_out, bytebuf_t *frag_out) {
    const uint8_t *end = au + len;
    bytebuf_t sample = {0};
    bool params_changed = false;
    int sc;

    const uint8_t *p = next_start(au, end, &sc);
    while (p < end) {
        const uint8_t *nal = p + sc;
        const uint8_t *nxt = next_start(nal, end, &sc);
        size_t nlen = (size_t)(nxt - nal);
        /* Trailing zero bytes belong to the next start code, not this NAL. */
        while (nlen > 0 && nal[nlen - 1] == 0) nlen--;
        if (nlen > 0) {
            int type = nal[0] & 0x1f;
            if (type == 7) {
                if (nlen >= 4 && nlen <= sizeof(m->sps) &&
                    (nlen != m->sps_len || memcmp(m->sps, nal, nlen) != 0)) {
                    memcpy(m->sps, nal, nlen);
                    m->sps_len = nlen;
                    params_changed = true;
                }
            } else if (type == 8) {
                if (nlen <= sizeof(m->pps) && (nlen != m->pps_len || memcmp(m->pps, nal, nlen) != 0)) {
                    memcpy(m->pps, nal, nlen);
                    m->pps_len = nlen;
                    params_changed = true;
                }
            } else if (type != 9) { /* drop access unit delimiters */
                u32(&sample, (uint32_t)nlen);
                put(&sample, nal, nlen);
            }
        }
        p = nxt;
    }

    if (params_changed && m->sps_len && m->pps_len) {
        m->have_params = true;
        snprintf(m->codec, sizeof(m->codec), "avc1.%02x%02x%02x", m->sps[1], m->sps[2], m->sps[3]);
        write_init(m, init_out);
    }
    if (!m->have_params || sample.len == 0) {
        bb_free(&sample);
        return false;
    }

    uint32_t dur = TIMESCALE / (uint32_t)m->fps;
    bytebuf_t *b = frag_out;
    size_t moof = box(b, "moof");
    size_t mfhd = fullbox(b, "mfhd", 0, 0);
    u32(b, ++m->seq);
    box_end(b, mfhd);
    size_t traf = box(b, "traf");
    size_t tfhd = fullbox(b, "tfhd", 0, 0x020000);
    u32(b, 1);
    box_end(b, tfhd);
    size_t tfdt = fullbox(b, "tfdt", 1, 0);
    u64(b, m->dts);
    box_end(b, tfdt);
    size_t trun = fullbox(b, "trun", 0, 0x000001 | 0x000100 | 0x000200 | 0x000400);
    u32(b, 1);
    size_t data_off = b->len;
    u32(b, 0);
    u32(b, dur);
    u32(b, (uint32_t)sample.len);
    u32(b, keyframe ? 0x02000000 : 0x01010000);
    box_end(b, trun);
    box_end(b, traf);
    box_end(b, moof);
    set32(b, data_off, (uint32_t)(b->len - moof + 8));

    u32(b, (uint32_t)sample.len + 8);
    put(b, "mdat", 4);
    put(b, sample.data, sample.len);
    bb_free(&sample);

    m->dts += dur;
    return true;
}
