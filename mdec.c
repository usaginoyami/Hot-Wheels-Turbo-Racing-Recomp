/* MDEC do PS1: decodificador de macroblocos (RLE + dequant + IDCT + YUV->RGB) */
#include <stdint.h>
uint32_t g_mdec_frames;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "runtime.h"

static const uint8_t zz[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5, 12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51, 58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63 };
static uint8_t qy[64], qc[64];
static int depth, is_signed, setbit;
static int state;                  /* 0 = ocioso, 1 = dados, 2 = tabelas de quant, 3 = tabela de escala */
static uint32_t remain, qcount;
static uint16_t* ibuf; static uint32_t ilen, icap;
static uint8_t* obuf; static uint32_t olen, opos, ocap;
static int dma_in_en, dma_out_en;
static float cosT[8][8];

static void init_tables(void) {
    static int done; if (done) return; done = 1;
    for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) cosT[x][u] = (u == 0 ? 0.70710678f : 1.0f) * cosf((2 * x + 1) * u * 3.14159265f / 16) * 0.5f;
}
static void idct(const int* coef, float* out) {
    float tmp[64];
    for (int v = 0; v < 8; v++) for (int x = 0; x < 8; x++) { float s = 0; for (int u = 0; u < 8; u++) s += cosT[x][u] * coef[v * 8 + u]; tmp[v * 8 + x] = s; }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) { float s = 0; for (int v = 0; v < 8; v++) s += cosT[y][v] * tmp[v * 8 + x]; out[y * 8 + x] = s; }
}
static uint32_t ipos;
static int next_hw(void) { return ipos < ilen ? ibuf[ipos++] : 0xfe00; }
static int sx10(int v) { return (v & 0x200) ? v - 0x400 : v; }
static int decode_block(const uint8_t* qt, float* out) {
    int coef[64]; memset(coef, 0, sizeof coef);
    int h = next_hw(); while (h == 0xfe00 && ipos < ilen) h = next_hw();   /* ignora padding */
    int qs = (h >> 10) & 63; int dc = sx10(h & 0x3ff) * qt[0];
    coef[0] = dc;
    int k = 0;
    for (;;) {
        if (ipos >= ilen) break;
        h = next_hw(); if (h == 0xfe00) break;
        k += ((h >> 10) & 63) + 1; if (k > 63) break;
        int lv = sx10(h & 0x3ff);
        int val = qs ? (lv * qt[k] * qs + 4) >> 3 : lv * 2;
        if (val > 1023) val = 1023; if (val < -1024) val = -1024;
        coef[zz[k]] = val;
    }
    idct(coef, out); return 1;
}
static void put24(int px, int py, int r, int g, int b, uint8_t* mb) { uint8_t* p = mb + (py * 16 + px) * 3; p[0] = r; p[1] = g; p[2] = b; }
static void ensure_out(uint32_t n) { if (olen + n > ocap) { ocap = (olen + n) * 2; obuf = realloc(obuf, ocap); } }
static void decode_all(void) {
    init_tables(); ipos = 0; olen = 0; opos = 0;
    while (ipos < ilen) {
        if (depth >= 2) {
            float cr[64], cb[64], y[4][64];
            decode_block(qc, cr); decode_block(qc, cb);
            for (int i = 0; i < 4; i++) decode_block(qy, y[i]);
            uint8_t mb[16 * 16 * 3];
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                float Y = y[(py / 8) * 2 + px / 8][(py % 8) * 8 + px % 8];
                float Cr = cr[(py / 2) * 8 + px / 2], Cb = cb[(py / 2) * 8 + px / 2];
                float r = Y + 1.402f * Cr, g = Y - 0.3437f * Cb - 0.7143f * Cr, b = Y + 1.772f * Cb;
                if (!is_signed) { r += 128; g += 128; b += 128; }
                int ri = (int)(r < (is_signed ? -128 : 0) ? (is_signed ? -128 : 0) : r > (is_signed ? 127 : 255) ? (is_signed ? 127 : 255) : r);
                int gi = (int)(g < (is_signed ? -128 : 0) ? (is_signed ? -128 : 0) : g > (is_signed ? 127 : 255) ? (is_signed ? 127 : 255) : g);
                int bi = (int)(b < (is_signed ? -128 : 0) ? (is_signed ? -128 : 0) : b > (is_signed ? 127 : 255) ? (is_signed ? 127 : 255) : b);
                put24(px, py, ri & 255, gi & 255, bi & 255, mb);
            }
            if (depth == 2) { ensure_out(768); memcpy(obuf + olen, mb, 768); olen += 768; }
            else { ensure_out(512); for (int i = 0; i < 256; i++) { uint16_t v = (mb[i * 3] >> 3) | ((mb[i * 3 + 1] >> 3) << 5) | ((mb[i * 3 + 2] >> 3) << 10) | (setbit ? 0x8000 : 0); memcpy(obuf + olen + i * 2, &v, 2); } olen += 512; }
        } else {
            float yb[64]; decode_block(qy, yb);
            ensure_out(64); for (int i = 0; i < 64; i++) { int v = (int)yb[i] + (is_signed ? 0 : 128); if (v < 0) v = 0; if (v > 255) v = 255; obuf[olen + i] = v; } olen += 64;
        }
    }
}

void mdec_cmd_write(uint32_t w) {
    if (getenv("MDECLOG") && state == 0) fprintf(stderr, "[mdec] cmd %08x\n", w);
    if (state == 0) {
        int cmd = w >> 29;
        if (cmd == 1) { depth = (w >> 27) & 3; is_signed = (w >> 26) & 1; setbit = (w >> 25) & 1; remain = w & 0xffff; ilen = 0; state = remain ? 1 : 0; }
        else if (cmd == 2) { remain = (w & 1) ? 32 : 16; qcount = 0; state = 2; }
        else if (cmd == 3) { remain = 32; state = 3; }
        return;
    }
    if (state == 1) {
        if (ilen + 2 > icap) { icap = (ilen + 2) * 2; ibuf = realloc(ibuf, icap * 2); }
        ibuf[ilen++] = w & 0xffff; ibuf[ilen++] = w >> 16;
        if (--remain == 0) { state = 0; g_mdec_frames++; decode_all(); if (getenv("MDECLOG")) { unsigned long long s = 0; for (unsigned q = 0; q < olen; q++) s += obuf[q]; { extern uint32_t g_frames; fprintf(stderr, "[mdec] quadro=%u ", g_frames); } fprintf(stderr, "[mdec] frame decodificado: %u halfwords -> %u bytes (depth %d) media=%.1f\n", ilen, olen, depth, olen ? (double)s / olen : 0.0); } }
    } else if (state == 2) {
        uint8_t* t = qcount < 16 ? qy : qc; uint32_t o = (qcount & 15) * 4;
        for (int k = 0; k < 4; k++) t[o + k] = (w >> (8 * k)) & 255;
        qcount++; if (--remain == 0) state = 0;
    } else if (state == 3) { if (--remain == 0) state = 0; }
}
void mdec_ctl_write(uint32_t w) {
    if (w & 0x80000000u) { state = 0; ilen = 0; olen = 0; opos = 0; }
    dma_in_en = (w >> 30) & 1; dma_out_en = (w >> 29) & 1;
}
uint32_t mdec_status(void) {
    uint32_t s = 0;
    if (opos >= olen) s |= 0x80000000u;
    if (state != 0) s |= 1u << 29;
    if (dma_in_en && state != 0) s |= 1u << 28;
    if (dma_out_en && opos < olen) s |= 1u << 27;
    s |= (depth & 3) << 25; s |= is_signed << 24; s |= setbit << 23;
    s |= (remain - 1) & 0xffff;
    return s;
}
void mdec_dma_in(uint32_t madr, uint32_t words) { for (uint32_t i = 0; i < words; i++) mdec_cmd_write(rd32(((madr + 4 * i) & 0x1ffffc) | 0x80000000)); }
void mdec_dma_out(uint32_t madr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0; for (int k = 0; k < 4; k++) { uint32_t o = opos + k; if (o < olen) w |= (uint32_t)obuf[o] << (8 * k); }
        opos += 4; wr32(((madr + 4 * i) & 0x1ffffc) | 0x80000000, w);
    }
}
