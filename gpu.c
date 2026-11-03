/* GPU do PS1 em software: VRAM 1024x512 15bpp, comandos GP0/GP1, DMA canal 2 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "runtime.h"

uint16_t vram[512 * 1024];
int gpu_dump(const char*);

/* ---- diagnostico: historico recente de listas/quadros e despejo automatico quando uma lista vem quase vazia ---- */
typedef struct { uint32_t frame, start, nodes, words; long cmds; int term; int isr; unsigned long long calls; } LRec;
static LRec lring[256]; static int lring_i;
typedef struct { uint32_t frame; int dx, dy, ax0, ay0, dmas; long cmds; } VRec;
static VRec vring[240]; static int vring_i;
static int diag_n; static uint32_t diag_last = 0;
void gpu_diag_dump(const char* tag);
#define TH_N 60
static uint8_t th_img[TH_N][120][160][3]; static uint32_t th_frame[TH_N]; static int th_i;
static void thumb_dump(const char* path);
static int dmas_f; static long cmds_f0;
#define VR(x, y) vram[((y) & 511) * 1024 + ((x) & 1023)]
/* ---- resolucao interna maior: um framebuffer 'hr' S vezes maior que a VRAM. A VRAM nativa continua sendo desenhada e e a fonte das texturas;
   o hr recebe as mesmas primitivas com coordenadas x S (bordas sem serrilhado) e e o que vai pra tela. S=1 desliga (igual ao original). ---- */
static int S = 1; static uint16_t* hr;
#define HRW (1024 * S)
#define HR(X, Y) hr[(((Y) & (512 * S - 1)) * HRW) + ((X) & (HRW - 1))]
static void rep_rect(int x0, int y0, int w, int h) {   /* copia a area nativa da VRAM pro hr (replicando cada pixel S x S) */
    if (S < 2 || !hr) return;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { uint16_t v = VR(x0 + i, y0 + j); for (int b = 0; b < S; b++) for (int a = 0; a < S; a++) HR((x0 + i) * S + a, (y0 + j) * S + b) = v; }
}
int gpu_scale(void) { return S; }
void gpu_cycle_scale(void);
void gpu_set_scale(int s) {
    if (s < 1) s = 1; if (s == 3) s = 4; if (s > 4) s = 4; if (s == S && (hr || s == 1)) return;
    free(hr); hr = NULL; S = s;
    if (S > 1) { hr = malloc((size_t)1024 * S * 512 * S * 2); if (!hr) { S = 1; return; } rep_rect(0, 0, 1024, 512); }
    fprintf(stderr, "[gpu] resolucao interna %dx\n", S);
}

static struct {
    int tpx, tpy, semi, depth, dither, drawdisp, setmask, chkmask, texdis;
    int twx, twy, twox, twoy;                    /* janela de textura (mascara/offset em unidades de 8) */
    int ax0, ay0, ax1, ay1, ox, oy;              /* area de desenho e offset */
    int dx, dy, hres, vres, vmode, d24, dispoff, dir, irq;
    int field;
} g;

static uint32_t cmd[16]; static int ncmd, need;
static int polyline, polyg; static uint32_t pl_prev_xy, pl_prev_c, pl_c0; static int pl_have, pl_wait;
/* transferencias */
static int xfer_mode;                          /* 1 = CPU->VRAM, 2 = VRAM->CPU */
static int xx, xy, xw, xh, xpos, xtotal;
static uint32_t readlatch;
long gpu_cmds, gpu_tris; int gpu_logn;

static int sx(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }
static int sy(uint32_t v) { return ((int32_t)(v << 5)) >> 21; }

static inline uint16_t c15(int r, int gg, int b) { if (r > 31) r = 31; if (gg > 31) gg = 31; if (b > 31) b = 31; if (r < 0) r = 0; if (gg < 0) gg = 0; if (b < 0) b = 0; return r | (gg << 5) | (b << 10); }

static int g_blk = 1;   /* 1: plot() tambem escreve o bloco S x S no hr (linhas e sprites); tri desliga e usa tri_s */
static inline void plot_hr(int X, int Y, int r, int gg, int b, int semi) {
    uint16_t* d = &HR(X, Y);
    if (g.chkmask && (*d & 0x8000)) return;
    if (semi) {
        int br = *d & 31, bg = (*d >> 5) & 31, bb = (*d >> 10) & 31;
        switch (g.semi) {
        case 0: r = (br + r) / 2; gg = (bg + gg) / 2; b = (bb + b) / 2; break;
        case 1: r += br; gg += bg; b += bb; break;
        case 2: r = br - r; gg = bg - gg; b = bb - b; break;
        default: r = br + r / 4; gg = bg + gg / 4; b = bb + b / 4; break;
        }
    }
    r = r > 31 ? 31 : r < 0 ? 0 : r; gg = gg > 31 ? 31 : gg < 0 ? 0 : gg; b = b > 31 ? 31 : b < 0 ? 0 : b;
    *d = r | (gg << 5) | (b << 10) | (g.setmask ? 0x8000 : 0);
}
static inline void plot(int x, int y, int r, int gg, int b, int semi, int mb15) {
    if (x < g.ax0 || x > g.ax1 || y < g.ay0 || y > g.ay1) return;
    if (S > 1 && g_blk) for (int bb2 = 0; bb2 < S; bb2++) for (int aa = 0; aa < S; aa++) plot_hr(x * S + aa, y * S + bb2, r, gg, b, semi);
    uint16_t* d = &VR(x, y);
    if (g.chkmask && (*d & 0x8000)) return;
    if (semi) {
        int br = *d & 31, bg = (*d >> 5) & 31, bb = (*d >> 10) & 31;
        switch (g.semi) {
        case 0: r = (br + r) / 2; gg = (bg + gg) / 2; b = (bb + b) / 2; break;
        case 1: r += br; gg += bg; b += bb; break;
        case 2: r = br - r; gg = bg - gg; b = bb - b; break;
        default: r = br + r / 4; gg = bg + gg / 4; b = bb + b / 4; break;
        }
    }
    *d = c15(r, gg, b) | (g.setmask ? 0x8000 : 0) | (mb15 ? 0 : 0);
}

/* amostra de textura: retorna 15bit (0 = transparente) */
static uint16_t tex_sample(int u, int v, int clx, int cly) {
    u &= 255; v &= 255;
    u = (u & ~(g.twx * 8)) | ((g.twox & g.twx) * 8);
    v = (v & ~(g.twy * 8)) | ((g.twoy & g.twy) * 8);
    int bx = g.tpx * 64, by = g.tpy * 256;
    if (g.depth == 0) { uint16_t w = VR(bx + u / 4, by + v); int i = (w >> ((u & 3) * 4)) & 15; return VR(clx * 16 + i, cly); }
    if (g.depth == 1) { uint16_t w = VR(bx + u / 2, by + v); int i = (w >> ((u & 1) * 8)) & 255; return VR(clx * 16 + i, cly); }
    return VR(bx + u, by + v);
}

typedef struct { int x, y, r, g, b, u, v; } V;

static void tri_n(V a, V b, V c, int textured, int semi, int raw, int gouraud, int clx, int cly) {
    gpu_tris++;
    int area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (!area) return;
    if (area < 0) { V t = b; b = c; c = t; area = -area; }
    int minx = a.x, maxx = a.x, miny = a.y, maxy = a.y;
    V* p[3] = { &a, &b, &c };
    for (int i = 1; i < 3; i++) { if (p[i]->x < minx) minx = p[i]->x; if (p[i]->x > maxx) maxx = p[i]->x; if (p[i]->y < miny) miny = p[i]->y; if (p[i]->y > maxy) maxy = p[i]->y; }
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    if (minx < g.ax0) minx = g.ax0; if (maxx > g.ax1) maxx = g.ax1; if (miny < g.ay0) miny = g.ay0; if (maxy > g.ay1) maxy = g.ay1;
    /* regra topo-esquerda aproximada */
    #define BIAS(p0, p1) ((((p1)->y == (p0)->y) ? ((p1)->x > (p0)->x) : ((p1)->y < (p0)->y)) ? 0 : -1)
    int b0 = BIAS(&b, &c), b1 = BIAS(&c, &a), b2 = BIAS(&a, &b);
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        int w0 = (c.x - b.x) * (y - b.y) - (c.y - b.y) * (x - b.x);
        int w1 = (a.x - c.x) * (y - c.y) - (a.y - c.y) * (x - c.x);
        int w2 = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
        int r, gg, bl, u, v;
        if (gouraud) { r = (a.r * w0 + b.r * w1 + c.r * w2) / area; gg = (a.g * w0 + b.g * w1 + c.g * w2) / area; bl = (a.b * w0 + b.b * w1 + c.b * w2) / area; }
        else { r = a.r; gg = a.g; bl = a.b; }
        if (textured) {
            u = (a.u * w0 + b.u * w1 + c.u * w2) / area; v = (a.v * w0 + b.v * w1 + c.v * w2) / area;
            uint16_t t = tex_sample(u, v, clx, cly);
            if (!t) continue;
            int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
            if (!raw) { tr = tr * r / 128; tg = tg * gg / 128; tb = tb * bl / 128; }
            plot(x, y, tr, tg, tb, semi && (t & 0x8000), 0);
        } else plot(x, y, r >> 3, gg >> 3, bl >> 3, semi, 0);
    }
}

static void tri_s(V a, V b, V c, int textured, int semi, int raw, int gouraud, int clx, int cly) {
    typedef long long ll;
    if (((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) == 0) return;
    { int mnx = a.x, mxx = a.x, mny = a.y, mxy = a.y; V* q[3] = { &a, &b, &c }; for (int i = 1; i < 3; i++) { if (q[i]->x < mnx) mnx = q[i]->x; if (q[i]->x > mxx) mxx = q[i]->x; if (q[i]->y < mny) mny = q[i]->y; if (q[i]->y > mxy) mxy = q[i]->y; } if (mxx - mnx > 1023 || mxy - mny > 511) return; }
    a.x *= S; a.y *= S; b.x *= S; b.y *= S; c.x *= S; c.y *= S;
    ll area = (ll)(b.x - a.x) * (c.y - a.y) - (ll)(b.y - a.y) * (c.x - a.x);
    if (area < 0) { V t = b; b = c; c = t; area = -area; }
    int minx = a.x, maxx = a.x, miny = a.y, maxy = a.y; V* p[3] = { &a, &b, &c };
    for (int i = 1; i < 3; i++) { if (p[i]->x < minx) minx = p[i]->x; if (p[i]->x > maxx) maxx = p[i]->x; if (p[i]->y < miny) miny = p[i]->y; if (p[i]->y > maxy) maxy = p[i]->y; }
    int cx0 = g.ax0 * S, cy0 = g.ay0 * S, cx1 = (g.ax1 + 1) * S - 1, cy1 = (g.ay1 + 1) * S - 1;
    if (minx < cx0) minx = cx0; if (maxx > cx1) maxx = cx1; if (miny < cy0) miny = cy0; if (maxy > cy1) maxy = cy1;
    #define BIAS2(p0, p1) ((((p1)->y == (p0)->y) ? ((p1)->x > (p0)->x) : ((p1)->y < (p0)->y)) ? 0 : -1)
    int b0 = BIAS2(&b, &c), b1 = BIAS2(&c, &a), b2 = BIAS2(&a, &b);
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        ll w0 = (ll)(c.x - b.x) * (y - b.y) - (ll)(c.y - b.y) * (x - b.x);
        ll w1 = (ll)(a.x - c.x) * (y - c.y) - (ll)(a.y - c.y) * (x - c.x);
        ll w2 = (ll)(b.x - a.x) * (y - a.y) - (ll)(b.y - a.y) * (x - a.x);
        if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
        int r, gg, bl, u, v;
        if (gouraud) { r = (int)((a.r * w0 + b.r * w1 + c.r * w2) / area); gg = (int)((a.g * w0 + b.g * w1 + c.g * w2) / area); bl = (int)((a.b * w0 + b.b * w1 + c.b * w2) / area); }
        else { r = a.r; gg = a.g; bl = a.b; }
        if (textured) {
            u = (int)((a.u * w0 + b.u * w1 + c.u * w2) / area); v = (int)((a.v * w0 + b.v * w1 + c.v * w2) / area);
            uint16_t t = tex_sample(u, v, clx, cly);
            if (!t) continue;
            int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
            if (!raw) { tr = tr * r / 128; tg = tg * gg / 128; tb = tb * bl / 128; }
            plot_hr(x, y, tr, tg, tb, semi && (t & 0x8000));
        } else plot_hr(x, y, r >> 3, gg >> 3, bl >> 3, semi);
    }
}
static void tri(V a, V b, V c, int textured, int semi, int raw, int gouraud, int clx, int cly) {
    g_blk = 0; tri_n(a, b, c, textured, semi, raw, gouraud, clx, cly); g_blk = 1;
    if (S > 1) tri_s(a, b, c, textured, semi, raw, gouraud, clx, cly);
}

static void rect(int x0, int y0, int w, int h, uint32_t col, int textured, int semi, int raw, int u0, int v0, int clx, int cly) {
    int cr = col & 255, cg = (col >> 8) & 255, cb = (col >> 16) & 255;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int x = x0 + i, y = y0 + j;
        if (x < g.ax0 || x > g.ax1 || y < g.ay0 || y > g.ay1) continue;
        if (textured) {
            uint16_t t = tex_sample(u0 + i, v0 + j, clx, cly); if (!t) continue;
            int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
            if (!raw) { tr = tr * cr / 128; tg = tg * cg / 128; tb = tb * cb / 128; }
            plot(x, y, tr, tg, tb, semi && (t & 0x8000), 0);
        } else plot(x, y, cr >> 3, cg >> 3, cb >> 3, semi, 0);
    }
}

static void line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int gouraud, int semi) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    if (dx > 1023 || dy > 511) return;
    for (int i = 0; i <= n; i++) {
        int x = n ? x0 + (x1 - x0) * i / n : x0, y = n ? y0 + (y1 - y0) * i / n : y0;
        int r = c0 & 255, gg = (c0 >> 8) & 255, b = (c0 >> 16) & 255;
        if (gouraud && n) { r += ((int)(c1 & 255) - r) * i / n; gg += ((int)((c1 >> 8) & 255) - gg) * i / n; b += ((int)((c1 >> 16) & 255) - b) * i / n; }
        plot(x, y, r >> 3, gg >> 3, b >> 3, semi, 0);
    }
}

static void set_tpage(uint32_t t) { g.tpx = t & 15; g.tpy = (t >> 4) & 1; g.semi = (t >> 5) & 3; g.depth = (t >> 7) & 3; g.texdis = (t >> 11) & 1; }

static int cmd_len(uint32_t w) {
    int c = w >> 24;
    if (c >= 0x20 && c < 0x40) { int v = (c & 8) ? 4 : 3, t = (c & 4) ? 1 : 0, gr = (c & 0x10) ? 1 : 0; return 1 + v * (1 + t) + (gr ? v - 1 : 0); }
    if (c >= 0x40 && c < 0x60) { return (c & 0x10) ? 4 : 3; }
    if (c >= 0x60 && c < 0x80) { int t = (c & 4) ? 1 : 0, var = ((c >> 3) & 3) == 0; return 2 + t + var; }
    if (c == 0x02) return 3;
    if (c >= 0x80 && c < 0xa0) return 4;
    if (c >= 0xa0 && c < 0xc0) return 3;
    if (c >= 0xc0 && c < 0xe0) return 3;
    return 1;
}

static void exec_cmd(void) {
    uint32_t c0 = cmd[0]; int c = c0 >> 24; gpu_cmds++;
    { extern int gpu_logn; int lg = gpu_logn; if (lg > 0) { gpu_logn--; fprintf(stderr, "[gp0]"); for (int k = 0; k < need && k < 16; k++) fprintf(stderr, " %08x", cmd[k]); fprintf(stderr, "\n"); } }
    if (c >= 0x20 && c < 0x40) {
        int quad = c & 8, tex = c & 4, gr = c & 0x10, semi = c & 2, raw = c & 1, nv = quad ? 4 : 3;
        V v[4]; int i = 0, clx = 0, cly = 0;
        i = 0;
        for (int k = 0; k < nv; k++) {
            uint32_t col = (k == 0) ? (cmd[i++] & 0xffffff) : (gr ? (cmd[i++] & 0xffffff) : (c0 & 0xffffff));
            uint32_t pos = cmd[i++];
            uint32_t uv = tex ? cmd[i++] : 0;
            v[k].x = sx(pos) + g.ox; v[k].y = sy(pos) + g.oy;
            v[k].r = col & 255; v[k].g = (col >> 8) & 255; v[k].b = (col >> 16) & 255;
            v[k].u = uv & 255; v[k].v = (uv >> 8) & 255;
            if (k == 0 && tex) { clx = (uv >> 16) & 63; cly = (uv >> 22) & 511; }
            if (k == 1 && tex) set_tpage(uv >> 16);
        }
        { static int tp=-2; if (tp==-2) tp = getenv("GPU_TP") ? atoi(getenv("GPU_TP")) : -1; extern uint32_t g_frames; if (tex && tp>=0 && g_frames==(uint32_t)tp && v[0].x>120 && v[0].x<250 && v[0].y>100 && v[0].y<200) fprintf(stderr,"[tp] c=%02x xy=%d,%d tp=%d,%d d=%d clut=%d,%d uv=%d,%d tw=%d,%d,%d,%d\n",c,v[0].x,v[0].y,g.tpx,g.tpy,g.depth,clx*16,cly,v[0].u,v[0].v,g.twx,g.twy,g.twox,g.twoy); }
        tri(v[0], v[1], v[2], tex, semi, raw, gr, clx, cly);
        if (quad) tri(v[1], v[2], v[3], tex, semi, raw, gr, clx, cly);
        return;
    }
    if (c >= 0x60 && c < 0x80) {
        int tex = c & 4, semi = c & 2, raw = c & 1, sz = (c >> 3) & 3, i = 1;
        uint32_t pos = cmd[i++], uv = tex ? cmd[i++] : 0; int w = 1, h = 1;
        if (sz == 0) { uint32_t wh = cmd[i++]; w = wh & 0x3ff; h = (wh >> 16) & 0x1ff; }
        else if (sz == 2) w = h = 8; else if (sz == 3) w = h = 16;
        rect(sx(pos) + g.ox, sy(pos) + g.oy, w, h, c0 & 0xffffff, tex, semi, raw, uv & 255, (uv >> 8) & 255, (uv >> 16) & 63, (uv >> 22) & 511);
        return;
    }
    if (c == 0x02) {
        int x = cmd[1] & 0x3f0, y = (cmd[1] >> 16) & 0x1ff, w = ((cmd[2] & 0x3ff) + 0xf) & ~0xf, h = (cmd[2] >> 16) & 0x1ff;
        uint16_t col = c15((c0 & 255) >> 3, ((c0 >> 8) & 255) >> 3, ((c0 >> 16) & 255) >> 3);
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) VR(x + i, y + j) = col;
        rep_rect(x, y, w, h);
        return;
    }
    if (c >= 0x80 && c < 0xa0) {
        int sx0 = cmd[1] & 0x3ff, sy0 = (cmd[1] >> 16) & 0x1ff, dx0 = cmd[2] & 0x3ff, dy0 = (cmd[2] >> 16) & 0x1ff;
        int w = cmd[3] & 0x3ff, h = (cmd[3] >> 16) & 0x1ff; if (!w) w = 0x400; if (!h) h = 0x200;
        if (getenv("GPU_LOADS")) { extern uint32_t g_frames; fprintf(stderr, "[vramcopy] quadro %u (%d,%d)->(%d,%d) %dx%d\n", g_frames, sx0, sy0, dx0, dy0, w, h); }
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) VR(dx0 + i, dy0 + j) = VR(sx0 + i, sy0 + j);
        if (S > 1 && hr) for (int j = 0; j < h * S; j++) for (int i = 0; i < w * S; i++) HR(dx0 * S + i, dy0 * S + j) = HR(sx0 * S + i, sy0 * S + j);
        return;
    }
    if (c >= 0xa0 && c < 0xc0) {
        xx = cmd[1] & 0x3ff; xy = (cmd[1] >> 16) & 0x1ff; xw = cmd[2] & 0x3ff; xh = (cmd[2] >> 16) & 0x1ff; if (!xw) xw = 0x400; if (!xh) xh = 0x200;
        if (getenv("GPU_LOADS")) { extern uint32_t g_frames; fprintf(stderr, "[loadimage] quadro %u -> (%d,%d) %dx%d\n", g_frames, xx, xy, xw, xh); }
        xpos = 0; xtotal = xw * xh; xfer_mode = 1; return;
    }
    if (c >= 0xc0 && c < 0xe0) {
        xx = cmd[1] & 0x3ff; xy = (cmd[1] >> 16) & 0x1ff; xw = cmd[2] & 0x3ff; xh = (cmd[2] >> 16) & 0x1ff; if (!xw) xw = 0x400; if (!xh) xh = 0x200;
        xpos = 0; xtotal = xw * xh; xfer_mode = 2; return;
    }
    switch (c) {
    case 0xe1: set_tpage(c0); g.dither = (c0 >> 9) & 1; g.drawdisp = (c0 >> 10) & 1; break;
    case 0xe2: g.twx = c0 & 31; g.twy = (c0 >> 5) & 31; g.twox = (c0 >> 10) & 31; g.twoy = (c0 >> 15) & 31; break;
    case 0xe3: g.ax0 = c0 & 0x3ff; g.ay0 = (c0 >> 10) & 0x1ff; break;
    case 0xe4: g.ax1 = c0 & 0x3ff; g.ay1 = (c0 >> 10) & 0x1ff; break;
    case 0xe5: g.ox = ((int32_t)(c0 << 21)) >> 21; g.oy = ((int32_t)(c0 << 10)) >> 21; break;
    case 0xe6: g.setmask = c0 & 1; g.chkmask = (c0 >> 1) & 1; break;
    }
}

double now_s(void); extern double g_vbonus; static double gpu_busy_until; int g_gpu_dirty;
void gpu_gp0(uint32_t w) {
    g_gpu_dirty = 1; gpu_busy_until = now_s() + 20e-6; if (getenv("DBGGPU")) { static int n; if (n++ < 400 && n > 60) fprintf(stderr, "[gp0w] %08x\n", w); }
    if (xfer_mode == 1) {
        for (int k = 0; k < 2 && xpos < xtotal; k++, xpos++) {
            VR(xx + xpos % xw, xy + xpos / xw) = (w >> (16 * k)) & 0xffff;
            if (S > 1) rep_rect(xx + xpos % xw, xy + xpos / xw, 1, 1);
        }
        if (xpos >= xtotal) { xfer_mode = 0; if (getenv("GPU_LOADS")) fprintf(stderr, "[xfer] fim (%d,%d) %dx%d\n", xx, xy, xw, xh); }
        return;
    }
    if (polyline) {                                   /* polilinha em andamento */
        if ((w & 0xf000f000) == 0x50005000) { polyline = 0; ncmd = 0; return; }
        if (pl_wait) { pl_c0 = w; pl_wait = 0; return; }
        int gr = polyg; uint32_t nx = w;
        line(sx(pl_prev_xy) + g.ox, sy(pl_prev_xy) + g.oy, pl_prev_c, sx(nx) + g.ox, sy(nx) + g.oy, gr ? pl_c0 : pl_prev_c, gr, cmd[0] & 0x02000000);
        pl_prev_xy = nx; pl_prev_c = gr ? pl_c0 : pl_prev_c; if (gr) pl_wait = 1;
        return;
    }
    if (ncmd == 0) {
        int c = w >> 24;
        if (c == 0x1f) { extern int g_gpu_irq_pending; g_gpu_irq_pending = 1; return; }
        if (c == 0 || c == 1 || (c >= 0xe1 && c <= 0xe6)) { if (c >= 0xe1 && c <= 0xe6) { cmd[0] = w; exec_cmd(); } return; }
        if (c >= 0x40 && c < 0x60 && (c & 8)) {      /* polilinha */
            cmd[0] = w; polyg = c & 0x10; ncmd = 1; need = polyg ? 4 : 3; return;
        }
        need = cmd_len(w);
        if (c < 0x02 || (c > 0x02 && c < 0x20) || (c >= 0xe0)) return;   /* desconhecido: ignora */
    }
    cmd[ncmd++] = w;
    if (ncmd >= need) {
        int c = cmd[0] >> 24;
        if (c >= 0x40 && c < 0x60 && (c & 8)) {      /* primeiro segmento da polilinha */
            int gr = c & 0x10; uint32_t c0 = cmd[0] & 0xffffff;
            uint32_t p0 = cmd[1], c1 = gr ? cmd[2] : c0, p1 = gr ? cmd[3] : cmd[2];
            line(sx(p0) + g.ox, sy(p0) + g.oy, c0, sx(p1) + g.ox, sy(p1) + g.oy, c1 & 0xffffff, gr, c & 2);
            pl_prev_xy = p1; pl_prev_c = c1 & 0xffffff; polyline = 1; pl_wait = 0; polyg = gr;
            ncmd = 0; return;
        }
        if (c >= 0x40 && c < 0x60) {
            int gr = c & 0x10; uint32_t c0 = cmd[0] & 0xffffff;
            uint32_t p0 = cmd[1], c1 = gr ? cmd[2] : c0, p1 = gr ? cmd[3] : cmd[2];
            line(sx(p0) + g.ox, sy(p0) + g.oy, c0, sx(p1) + g.ox, sy(p1) + g.oy, c1 & 0xffffff, gr, c & 2);
            ncmd = 0; return;
        }
        exec_cmd(); ncmd = 0;
    }
}

void gpu_gp1(uint32_t w) {
    switch (w >> 24) {
    case 0x00: memset(&g, 0, sizeof g); g.ax1 = 1023; g.ay1 = 511; g.dispoff = 1; ncmd = 0; xfer_mode = 0; polyline = 0; break;
    case 0x01: ncmd = 0; polyline = 0; xfer_mode = 0; break;
    case 0x02: g.irq = 0; break;
    case 0x03: g.dispoff = w & 1; break;
    case 0x04: g.dir = w & 3; break;
    case 0x05: g.dx = w & 0x3ff; g.dy = (w >> 10) & 0x1ff; break;
    case 0x08: g.hres = (w & 3) | (((w >> 6) & 1) << 2); g.vres = (w >> 2) & 1; g.vmode = (w >> 3) & 1; g.d24 = (w >> 4) & 1; break;
    case 0x10: switch (w & 15) {
        case 2: readlatch = g.twx | (g.twy << 5) | (g.twox << 10) | (g.twoy << 15); break;
        case 3: readlatch = g.ax0 | (g.ay0 << 10); break;
        case 4: readlatch = g.ax1 | (g.ay1 << 10); break;
        case 5: readlatch = (g.ox & 0x7ff) | ((g.oy & 0x7ff) << 11); break;
        case 7: readlatch = 2; break;
        } break;
    }
}

void gpu_resume(void);
uint32_t gpu_stat(void) {
    gpu_resume();
    uint32_t s = g.tpx | (g.tpy << 4) | (g.semi << 5) | (g.depth << 7) | (g.dither << 9) | (g.drawdisp << 10) | (g.setmask << 11) | (g.chkmask << 12) | (g.texdis << 15);
    s |= ((g.hres & 3) << 17) | (((g.hres >> 2) & 1) << 16) | (g.vres << 19) | (g.vmode << 20) | (g.d24 << 21) | (g.dispoff << 23);
    s |= 0x1c000000u | (1u << 25) | (g.dir << 29) | ((uint32_t)g.field << 31);
    if (getenv("DBGGPU")) { static int n; if (n++ < 40) fprintf(stderr, "[gpustat] busy_until-now=%.1f us\n", (gpu_busy_until - now_s()) * 1e6); }
    g_vbonus += 2e-6; if (gpu_busy_until > now_s()) s &= ~((1u << 26) | (1u << 28));   /* GPU 'ocupada' desenhando: a PsyQ do HWTR espera ver isso (DrawSync(1)) */
    return s;
}

uint32_t gpu_read(void) {
    if (xfer_mode == 2) {
        uint32_t v = 0;
        for (int k = 0; k < 2 && xpos < xtotal; k++, xpos++) v |= (uint32_t)VR(xx + xpos % xw, xy + xpos / xw) << (16 * k);
        if (xpos >= xtotal) xfer_mode = 0;
        readlatch = v;
    }
    return readlatch;
}


/* Lista encadeada. No PS1 a DMA e lenta e o jogo continua montando o fim da lista enquanto ela anda;
   aqui a DMA e instantanea, entao um no que aponta pra si mesmo (ainda nao ligado) pausa a DMA e ela
   continua depois, quando o jogo ja ligou o no. */
static uint32_t rs_a; static int rs_pend, rs_tries; static uint64_t rs_t0;
static void walk_list(uint32_t a, int first) {
    int guard = 0; uint32_t slow = a; rs_pend = 0; uint32_t a0 = a; long c0 = gpu_cmds; int nw = 0;
    while (guard++ < 200000) {
        uint32_t h;
        if (!first || guard > 1) { h = rd32(a | 0x80000000); }
        else h = rd32(a | 0x80000000);
        int n = h >> 24;
        if (first || guard > 1) for (int i = 0; i < n; i++) gpu_gp0(rd32((a + 4 + 4 * i) | 0x80000000));
        nw += n;
        if ((h & 0xffffff) == 0xffffff || (h & 0x800000)) { { extern uint64_t get_calls(void); extern uint32_t g_frames; extern int hw_in_isr(void); LRec* r = &lring[lring_i++ & 255]; r->frame = g_frames; r->start = a0; r->nodes = guard; r->words = nw; r->cmds = gpu_cmds - c0; r->term = 0; r->isr = hw_in_isr(); r->calls = get_calls(); }
 if (getenv("LISTLOG") && gpu_cmds - c0 < 600) { extern uint32_t g_frames; if (g_frames > 7000) { static int dn; extern void dump_ring(void); if (dn++ < 3) dump_ring(); } if (g_frames > 7000) fprintf(stderr, "[lista] quadro %u inicio=%06x nos=%d palavras=%d cmds=%ld fim: terminador hdr=%08x\n", g_frames, a0, guard, nw, gpu_cmds - c0, h); } break; }
        uint32_t nx = h & 0x1ffffc;
        if (nx == a) break;   /* no que aponta pra si mesmo: fim (e o que o jogo faz com o ultimo primitivo) */
        a = nx;
        if (!(guard & 1)) slow = rd32(slow | 0x80000000) & 0x1ffffc;
        if (a == slow && guard > 2) { fprintf(stderr, "[gpu] lista DMA em ciclo (no=%08x) - cortada\n", a); break; }
    }
}

/* Listas "parciais": o jogo as monta em outra ordem/tempo que no PS1 e, de vez em quando, a lista e enviada ainda quase vazia
   (so alguns primitivos). Em vez de desenhar o quadro quebrado, pula-se essa lista e o buffer fica como estava. */
static void diag_partial(uint32_t words, uint32_t ema, uint32_t madr);
static struct { uint32_t madr; double ema; int streak; } lst[8];
int g_gpu_skipped; static int skip_frame;
/* Classifica a lista antes de desenhar: 0 = normal, 1 = parcial (so parte dos primitivos: desenha mas o quadro nao e exibido),
   2 = quase vazia (so um restinho: nao desenha, senao estraga o quadro que esta na tela). */
static int bad_buf[2]; int g_gpu_held;
static int list_class(uint32_t madr) {
    uint32_t a = madr & 0x1ffffc; long words = 0; int guard = 0;
    while (guard++ < 20000) { uint32_t h = rd32(a | 0x80000000); words += h >> 24; if ((h & 0xffffff) == 0xffffff || (h & 0x800000)) break; uint32_t nx = h & 0x1ffffc; if (nx == a) break; a = nx; }
    int k = -1; for (int i = 0; i < 8; i++) if (lst[i].madr == (madr & 0x1ffffc)) k = i;
    if (k < 0) { if (words < 500) return 0;   /* listas pequenas em enderecos variados nao sao rastreadas (senao expulsam as listas principais) */
        k = 0; for (int i = 0; i < 8; i++) { if (!lst[i].madr) { k = i; break; } if (lst[i].ema < lst[k].ema) k = i; }
        lst[k].madr = madr & 0x1ffffc; lst[k].ema = (double)words; lst[k].streak = 0; return 0; }
    if (lst[k].ema > 500 && words < lst[k].ema * 0.6) {
        diag_partial((uint32_t)words, (uint32_t)lst[k].ema, madr);
        if (lst[k].streak < 3) { lst[k].streak++; g_gpu_skipped++; return words < lst[k].ema * 0.05 ? 2 : 1; }
        lst[k].ema = (double)words; lst[k].streak = 0; return 0;     /* muda o regime (menu, cena nova): aceita */
    }
    lst[k].streak = 0; lst[k].ema = lst[k].ema * 0.995; if ((double)words > lst[k].ema) lst[k].ema = (double)words; return 0;
}
int gpu_display_bad(void) { return bad_buf[g.dy >= 240]; }
static uint32_t q_madr[16]; static int q_n;
int gpu_pending(void) { return rs_pend || q_n; }
static void drain(void) {
    for (int it = 0; it < 64; it++) {
        if (rs_pend) {
            uint32_t a = rs_a, h = rd32(a | 0x80000000), nx = h & 0x1ffffc;
            if ((h & 0xffffff) == 0xffffff || (h & 0x800000)) { rs_pend = 0; continue; }
            if (nx == a) { if (++rs_tries > 4000) { rs_pend = 0; { extern uint64_t get_calls(void); fprintf(stderr, "[gpu] no sem ligacao ate o fim - descartado (no=%06x hdr=%08x apos %llu chamadas)\n", a, h, (unsigned long long)(get_calls() - rs_t0)); } continue; } return; }
            { extern uint64_t get_calls(void); if (getenv("PAUSELOG")) { static int rn; if (rn++ < 60) fprintf(stderr, "[retoma] no=%06x novo_link=%06x apos %llu chamadas\n", a, nx, (unsigned long long)(get_calls() - rs_t0)); } } rs_pend = 0; walk_list(nx, 1); continue;
        }
        if (!q_n) return;
        uint32_t m = q_madr[0]; memmove(q_madr, q_madr + 1, (--q_n) * sizeof q_madr[0]); walk_list(m, 1);
    }
}
/* chamada de tempos em tempos: tenta continuar a DMA pausada */
void gpu_resume(void) { if (rs_pend || q_n) drain(); }
int g_gpu_forced;
/* no vblank: o que ainda esta sem ligacao e tratado como fim de lista */
void gpu_force_flush(void) { for (int i = 0; i < 20 && (rs_pend || q_n); i++) { drain(); if (rs_pend) { rs_pend = 0; g_gpu_forced++; } } }

/* DMA canal 2: sync 1 = bloco, sync 2 = lista encadeada; dir bit0 = RAM->GPU */
static void gpu_dma_inner(uint32_t madr, uint32_t bcr, uint32_t chcr);
void gpu_dma(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    /* o tempo que ESTE emulador gasta rasterizando nao conta no relogio do jogo (a GPU real desenha em paralelo) */
    double t0 = now_s(); gpu_dma_inner(madr, bcr, chcr); if (getenv("RASTERCLK")) { double dt = now_s() - t0; if (dt > 0) g_vbonus -= dt; }
}
static void gpu_dma_inner(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    dmas_f++; g_gpu_dirty = 1; gpu_busy_until = now_s() + 20e-6; { extern int g_gpu_cb_pending; extern double g_gpu_cb_pending_t; g_gpu_cb_pending++; { static double cbd = -1; if (cbd < 0) cbd = getenv("GPUCB") ? atof(getenv("GPUCB")) * 1e-6 : 300e-6; g_gpu_cb_pending_t = now_s() + cbd; } }
    uint32_t sync = (chcr >> 9) & 3;
    if (getenv("GPU_DMAT")) { extern uint32_t g_frames; if (g_frames >= 8230) fprintf(stderr, "[gdma] q%u madr=%08x bcr=%08x chcr=%08x\n", g_frames, madr, bcr, chcr); }
    if (sync == 2) {
        int cls = list_class(madr);
        static uint32_t mb_m[4]; static int mb_b[4], mb_n;
        int mi = -1; for (int i = 0; i < 4; i++) if (mb_m[i] == (madr & 0x1ffffc)) mi = i;
        if (cls) {   /* lista parcial/quase vazia: nao desenha por cima do quadro antigo (daria dois quadros misturados); segura a imagem anterior */
            static int sk; if (++sk <= 20 || !(sk % 500)) fprintf(stderr, "[gpu] lista parcial pulada cls=%d (%d)\n", cls, sk);
            if (mi >= 0) bad_buf[mb_b[mi]] = cls; else bad_buf[0] = bad_buf[1] = cls;
            return;
        }
        walk_list(madr & 0x1ffffc, 1);
        if (mi < 0) { mi = mb_n++ & 3; mb_m[mi] = madr & 0x1ffffc; }
        mb_b[mi] = g.ay0 >= 240; bad_buf[mb_b[mi]] = 0;
    } else {
        uint32_t words = (bcr & 0xffff) * (sync == 1 ? (bcr >> 16) : 1); if (!words) words = 0x10000;
        uint32_t a = madr & 0x1ffffc;
        for (uint32_t i = 0; i < words; i++, a = (a + 4) & 0x1ffffc) {
            if (chcr & 1) gpu_gp0(rd32(a | 0x80000000)); else wr32(a | 0x80000000, gpu_read());
        }
    }
}


int gpu_flat(void) {   /* area visivel de cor (quase) unica: quadro provavelmente incompleto */
    int w = (g.hres & 4) ? 368 : (int[]){ 256, 320, 512, 640 }[g.hres & 3], h = g.vres ? 480 : 240, mn = 99999, mx = 0;
    if (g.dispoff || g.d24) return 0;
    for (int k = 0; k < 600; k++) { int x = (k * 37) % w, y = (k * 53) % h; uint16_t v = VR(g.dx + x, g.dy + y); int l = (v & 31) + ((v >> 5) & 31) + ((v >> 10) & 31); if (l < mn) mn = l; if (l > mx) mx = l; }
    return mx - mn < 6;
}
int gpu_dump_vram(const char* path);

void gpu_diag_dump(const char* tag) {
    char fn[96]; extern uint32_t g_frames; sprintf(fn, "diag_%s.txt", tag); FILE* f = fopen(fn, "w"); if (!f) return;
    fprintf(f, "quadro atual %u, disp=(%d,%d) draw=(%d,%d) off=(%d,%d)\n", g_frames, g.dx, g.dy, g.ax0, g.ay0, g.ox, g.oy);
    fprintf(f, "--- listas DMA recentes (quadro inicio nos palavras cmds fim isr chamadas) ---\n");
    for (int i = 0; i < 256; i++) { LRec* r = &lring[(lring_i + i) & 255]; if (!r->start) continue; fprintf(f, "%u %06x %u %u %ld %s isr=%d %llu\n", r->frame, r->start, r->nodes, r->words, r->cmds, r->term ? "SEM-LIGACAO" : "ok", r->isr, r->calls); }
    fprintf(f, "--- quadros (quadro disp draw dmas cmds) ---\n");
    for (int i = 0; i < 240; i++) { VRec* v = &vring[(vring_i + i) % 240]; if (!v->frame) continue; fprintf(f, "%u (%d,%d) (%d,%d) %d %ld\n", v->frame, v->dx, v->dy, v->ax0, v->ay0, v->dmas, v->cmds); }
    fprintf(f, "--- folha de contato (esq->dir, cima->baixo, mais antigo primeiro) quadros: ");
    for (int n = 0; n < TH_N; n++) { int k = (th_i + n) % TH_N; if (th_i < TH_N) k = n < th_i ? n : 0; fprintf(f, "%u ", th_frame[k]); } fprintf(f, "\n");
    fclose(f);
    sprintf(fn, "diag_%s_ring.ppm", tag); thumb_dump(fn);
    sprintf(fn, "diag_%s.ppm", tag); gpu_dump(fn); sprintf(fn, "diag_%s_vram.ppm", tag); gpu_dump_vram(fn);
}

/* anel de miniaturas dos ultimos quadros exibidos (F9 salva uma folha de contato, assim da pra pegar o piscar DEPOIS que passou) */
static void thumb_push(void) {
    if (g.dispoff || g.d24 || g.vres) return; int tw = (g.hres & 4) ? 368 : (int[]){ 256, 320, 512, 640 }[g.hres & 3];
    int k = th_i++ % TH_N; extern uint32_t g_frames; th_frame[k] = g_frames;
    for (int y = 0; y < 120; y++) for (int x = 0; x < 160; x++) { uint16_t v = VR(g.dx + x * tw / 160, g.dy + y * 2); th_img[k][y][x][0] = (v & 31) << 3; th_img[k][y][x][1] = ((v >> 5) & 31) << 3; th_img[k][y][x][2] = ((v >> 10) & 31) << 3; }
}
static void thumb_dump(const char* path) {
    FILE* f = fopen(path, "wb"); if (!f) return; int cols = 10, rows = 6;
    fprintf(f, "P6\n%d %d\n255\n", cols * 160, rows * 120);
    for (int r = 0; r < rows; r++) for (int y = 0; y < 120; y++) for (int c = 0; c < cols; c++) { int n = r * cols + c; int k = (th_i + n) % TH_N; if (th_i < TH_N) k = n < th_i ? n : 0; fwrite(th_img[k][y], 3, 160, f); }
    fclose(f);
}
static uint32_t diag_due; int g_diag_req;
static void diag_partial(uint32_t words, uint32_t ema, uint32_t madr) {
    extern uint32_t g_frames; if (getenv("PARTLOG")) fprintf(stderr, "[p] %u\n", g_frames); if (diag_due || diag_n >= 6 || (diag_last && g_frames - diag_last < 120)) return;
    diag_due = g_frames + 1; diag_last = g_frames;
    fprintf(stderr, "[diag] lista parcial no quadro %u (%u palavras de ~%u, inicio %06x)\n", g_frames, words, ema, madr & 0x1ffffc);
}
int gpu_dump_vram(const char* path);
static void hr_shot(void) {
    static uint32_t buf[2560 * 1920]; int w, h; gpu_render(buf, &w, &h);
    FILE* f = fopen("hr_shot.ppm", "wb"); if (!f) return; fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t p[3] = { buf[i] >> 16, buf[i] >> 8, buf[i] }; fwrite(p, 1, 3, f); } fclose(f);
}
void gpu_vblank(void) { g.field ^= 1; skip_frame = 0; thumb_push();
    { static int init; if (!init) { init = 1; if (getenv("HIRES")) gpu_set_scale(atoi(getenv("HIRES"))); }
      static int sh = -1; extern uint32_t g_frames; if (sh < 0) sh = getenv("HRSHOT") ? atoi(getenv("HRSHOT")) : 0; if (sh && g_frames == (uint32_t)sh) hr_shot(); }
    { extern uint32_t g_frames; if (diag_due && g_frames >= diag_due) { char tag[40]; sprintf(tag, "auto%d_q%u", ++diag_n, g_frames); diag_due = 0; fprintf(stderr, "[diag] salvando diag_%s.*\n", tag); gpu_diag_dump(tag); } if (g_diag_req) { char tag[40]; static int mn; g_diag_req = 0; sprintf(tag, "F9_%d_q%u", ++mn, g_frames); fprintf(stderr, "[diag] F9: salvando diag_%s.*\n", tag); gpu_diag_dump(tag); } }
    if (getenv("FLATLOG")) { extern uint32_t g_frames; int w = 320, h = 240, mn = 99999, mx = 0; int dis = 0;
        for (int k = 0; k < 400; k++) { int x = (k * 37) % w, y = (k * 53) % h; uint16_t v = VR(g.dx + x, g.dy + y); int l = (v & 31) + ((v >> 5) & 31) + ((v >> 10) & 31); if (l < mn) mn = l; if (l > mx) mx = l; }
        if (mx - mn < 6 && gpu_cmds - cmds_f0 > 500) { static int nf; if (nf < 4) { char fn[64]; sprintf(fn, "flat_%d.ppm", nf); extern int gpu_dump_vram(const char*); gpu_dump_vram(fn); sprintf(fn, "flatd_%d.ppm", nf); gpu_dump(fn); nf++; } }
        if (mx - mn < 6) fprintf(stderr, "[flat] quadro %u disp=(%d,%d) dmas=%d cmds=%ld pend=%d dispoff=%d draw=(%d,%d) ox,oy=%d,%d\n", g_frames, g.dx, g.dy, dmas_f, gpu_cmds - cmds_f0, rs_pend, g.dispoff, g.ax0, g.ay0, g.ox, g.oy);
        else if (getenv("FLATLOG")[0] == '2') fprintf(stderr, "[ok] quadro %u disp=(%d,%d) dmas=%d cmds=%ld draw=(%d,%d) ox,oy=%d,%d\n", g_frames, g.dx, g.dy, dmas_f, gpu_cmds - cmds_f0, g.ax0, g.ay0, g.ox, g.oy); }
    { extern uint32_t g_frames; VRec* v = &vring[vring_i++ % 240]; v->frame = g_frames; v->dx = g.dx; v->dy = g.dy; v->ax0 = g.ax0; v->ay0 = g.ay0; v->dmas = dmas_f; v->cmds = gpu_cmds - cmds_f0; }
    dmas_f = 0; cmds_f0 = gpu_cmds; }

int gpu_dump(const char* path) {
    int w = (g.hres & 4) ? 368 : (int[]){ 256, 320, 512, 640 }[g.hres & 3], h = g.vres ? 480 : 240;
    FILE* f = fopen(path, "wb"); if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t px[3];
        if (g.dispoff) { px[0] = px[1] = px[2] = 0; }
        else if (g.d24) { int bx = g.dx * 2 + x * 3; int yy = (g.dy + y) & 511; px[0] = ((uint8_t*)&VR(0, yy))[bx & 2047]; px[1] = ((uint8_t*)&VR(0, yy))[(bx + 1) & 2047]; px[2] = ((uint8_t*)&VR(0, yy))[(bx + 2) & 2047]; }
        else { uint16_t v = VR(g.dx + x, g.dy + y); px[0] = (v & 31) << 3; px[1] = ((v >> 5) & 31) << 3; px[2] = ((v >> 10) & 31) << 3; }
        fwrite(px, 1, 3, f);
    }
    fclose(f); return 1;
}
void gpu_info(void) { fprintf(stderr, "[gpu] cmds=%ld tris=%ld disp=(%d,%d) hres=%d vres=%d d24=%d off=%d area=(%d,%d)-(%d,%d)\n", gpu_cmds, gpu_tris, g.dx, g.dy, g.hres, g.vres, g.d24, g.dispoff, g.ax0, g.ay0, g.ax1, g.ay1); }
int gpu_dump_vram(const char* path) {
    FILE* f = fopen(path, "wb"); if (!f) return 0;
    fprintf(f, "P6\n1024 512\n255\n");
    for (int i = 0; i < 1024 * 512; i++) { uint16_t v = vram[i]; uint8_t px[3] = { (v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3 }; fwrite(px, 1, 3, f); }
    fclose(f); return 1;
}

/* converte a area de exibicao para ARGB8888; retorna largura/altura */
void gpu_render(uint32_t* out, int* pw, int* ph) {
    int w = (g.hres & 4) ? 368 : (int[]){ 256, 320, 512, 640 }[g.hres & 3], h = g.vres ? 480 : 240;
    if (S > 1 && hr && !g.d24 && !g.dispoff) {   /* tela em resolucao interna maior */
        *pw = w * S; *ph = h * S;
        for (int y = 0; y < h * S; y++) for (int x = 0; x < w * S; x++) {
            uint16_t v = HR(g.dx * S + x, g.dy * S + y);
            out[y * w * S + x] = 0xff000000u | (((v & 31) << 3) << 16) | ((((v >> 5) & 31) << 3) << 8) | (((v >> 10) & 31) << 3);
        }
        return;
    }
    *pw = w; *ph = h;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t r, gg, b;
        if (g.dispoff) r = gg = b = 0;
        else if (g.d24) { int bx = g.dx * 2 + x * 3; uint8_t* p = (uint8_t*)&VR(0, (g.dy + y) & 511); r = p[bx & 2047]; gg = p[(bx + 1) & 2047]; b = p[(bx + 2) & 2047]; }
        else { uint16_t v = VR(g.dx + x, g.dy + y); r = (v & 31) << 3; gg = ((v >> 5) & 31) << 3; b = ((v >> 10) & 31) << 3; }
        out[y * w + x] = 0xff000000u | (r << 16) | (gg << 8) | b;
    }
}

void gpu_cycle_scale(void) { gpu_set_scale(S == 1 ? 2 : S == 2 ? 4 : 1); }
