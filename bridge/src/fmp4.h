/* Fragmented MP4 muxer for a single H.264 track, one fragment per frame. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t len, cap;
} bytebuf_t;

void bb_free(bytebuf_t *b);

typedef struct {
    int width, height, fps;
    uint8_t sps[512];
    size_t sps_len;
    uint8_t pps[512];
    size_t pps_len;
    bool have_params;
    uint32_t seq;
    uint64_t dts;
    char codec[32]; /* e.g. "avc1.640028" */
} fmp4_t;

void fmp4_init(fmp4_t *m, int width, int height, int fps);

/*
 * Processes one Annex-B access unit (start-code separated NAL units).
 * SPS/PPS units update the stream parameters; if they changed, *init_out
 * receives a new initialization segment (must be sent before the fragment).
 * *frag_out receives the moof+mdat for the remaining NAL units.
 * Returns false if no fragment could be produced (e.g. no SPS/PPS seen yet).
 */
bool fmp4_mux_annexb(fmp4_t *m, const uint8_t *au, size_t len, bool keyframe,
                     bytebuf_t *init_out, bytebuf_t *frag_out);
