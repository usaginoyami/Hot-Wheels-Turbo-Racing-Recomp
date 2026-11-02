/* GTE (COP2) do PS1 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"

static int32_t D[32];   /* registradores de dados */
static int32_t C[32];   /* registradores de controle */
#define FLAG C[31]

static inline int16_t lo16(int32_t v) { return (int16_t)(v & 0xffff); }
static inline int16_t hi16(int32_t v) { return (int16_t)((v >> 16) & 0xffff); }
#define VX(n) lo16(D[(n) * 2])
#define VY(n) hi16(D[(n) * 2])
#define VZ(n) lo16(D[(n) * 2 + 1])
#define IR0 D[8]
#define IR1 D[9]
#define IR2 D[10]
#define IR3 D[11]
#define MAC0 D[24]
#define MAC1 D[25]
#define MAC2 D[26]
#define MAC3 D[27]

uint32_t gte_rd(int r) {
    switch (r) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (int32_t)(int16_t)D[r];
    case 7: case 16: case 17: case 18: case 19: return (uint16_t)D[r];
    case 15: return D[14];
    case 28: case 29: {
        int a = (D[9] / 0x80), b = (D[10] / 0x80), c = (D[11] / 0x80);
        a = a < 0 ? 0 : a > 31 ? 31 : a; b = b < 0 ? 0 : b > 31 ? 31 : b; c = c < 0 ? 0 : c > 31 ? 31 : c;
        return a | (b << 5) | (c << 10);
    }
    }
    return D[r];
}
void gte_wr(int r, uint32_t v) {
    switch (r) {
    case 15: D[12] = D[13]; D[13] = D[14]; D[14] = v; return;       /* SXYP empurra a fila */
    case 28: D[9] = (v & 31) * 0x80; D[10] = ((v >> 5) & 31) * 0x80; D[11] = ((v >> 10) & 31) * 0x80; D[28] = v; return;
    case 29: return;
    case 30: { D[30] = v; uint32_t x = (int32_t)v < 0 ? ~v : v; int n = 0; for (int i = 31; i >= 0 && ((x >> i) & 1) == 0; i--) n++; D[31] = n; return; }
    case 31: return;
    }
    D[r] = v;
}
uint32_t gte_ctl_rd(int r) {
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: return (int32_t)(int16_t)C[r];
    case 31: { uint32_t f = C[31] & 0x7ffff000; if (f & 0x7f87e000) f |= 0x80000000; return f; }
    }
    return C[r];
}
void gte_ctl_wr(int r, uint32_t v) { C[r] = v; if (r == 31) C[31] = v & 0x7ffff000; }

static int sf, lm;
static int64_t clampflag(int64_t v, int64_t lo, int64_t hi, uint32_t bit) { if (v < lo) { FLAG |= bit; return lo; } if (v > hi) { FLAG |= bit; return hi; } return v; }
static int32_t irclamp(int64_t v, int which) { /* which 1..3 */
    int64_t lo = lm ? 0 : -0x8000;
    return (int32_t)clampflag(v, lo, 0x7fff, 1u << (25 - which));
}
static void setmac(int i, int64_t v) { /* i = 1..3, detecta overflow de 44 bits */
    if (v > 0x7ffffffffffLL) FLAG |= 1u << (31 - i); else if (v < -0x80000000000LL) FLAG |= 1u << (28 - i);
    v = (v << 20) >> 20; /* mantem 44 bits */
    D[24 + i] = (int32_t)(v >> (sf * 12));
}
#define SHR(v) ((int64_t)(v) >> (sf * 12))
static void ir_from_mac(void) { IR1 = irclamp(MAC1, 1); IR2 = irclamp(MAC2, 2); IR3 = irclamp(MAC3, 3); }

static int32_t mat(int m, int r, int c) {
    int base = m * 8, idx = r * 3 + c;       /* 0 RT, 1 LLM, 2 LCM */
    int32_t w = C[base + idx / 2];
    return (idx & 1) ? hi16(w) : lo16(w);
}
static void getvec(int vx, int32_t* v) {
    if (vx < 3) { v[0] = VX(vx); v[1] = VY(vx); v[2] = VZ(vx); }
    else { v[0] = (int16_t)IR1; v[1] = (int16_t)IR2; v[2] = (int16_t)IR3; }
}
static void mvmva_core(int m, const int32_t* v, const int64_t* t) {   /* MAC = (t<<12 + M*v) >> sf*12 */
    for (int r = 0; r < 3; r++) {
        int64_t s = (t[r] << 12) + (int64_t)mat(m, r, 0) * v[0] + (int64_t)mat(m, r, 1) * v[1] + (int64_t)mat(m, r, 2) * v[2];
        setmac(r + 1, s);
    }
}
static void push_color(void) {
    int r = MAC1 >> 4, g = MAC2 >> 4, b = MAC3 >> 4;
    uint32_t f1 = 0, f2 = 0, f3 = 0; (void)f1; (void)f2; (void)f3;
    r = (int)clampflag(r, 0, 255, 1u << 21); g = (int)clampflag(g, 0, 255, 1u << 20); b = (int)clampflag(b, 0, 255, 1u << 19);
    D[20] = D[21]; D[21] = D[22]; D[22] = r | (g << 8) | (b << 16) | (D[6] & 0xff000000);
}
static void push_sz(int64_t v) { D[16] = D[17]; D[17] = D[18]; D[18] = D[19]; D[19] = (int32_t)clampflag(v, 0, 0xffff, 1u << 18); }
static void push_sxy(int64_t x, int64_t y) {
    x = clampflag(x, -0x400, 0x3ff, 1u << 14); y = clampflag(y, -0x400, 0x3ff, 1u << 13);
    D[12] = D[13]; D[13] = D[14]; D[14] = (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}
static int64_t gte_div(void) {
    uint32_t h = (uint16_t)C[26], z = (uint16_t)D[19];
    if (z * 2 <= h) { FLAG |= 1u << 17; return 0x1ffff; }
    uint64_t n = (((uint64_t)h * 0x20000 / z) + 1) / 2;
    if (n > 0x1ffff) { FLAG |= 1u << 17; n = 0x1ffff; }
    return n;
}
static void rtp(int v, int last) {
    int32_t vec[3]; getvec(v, vec);
    int64_t t[3] = { C[5], C[6], C[7] };
    mvmva_core(0, vec, t);
    /* SZ3 usa MAC3 antes de saturar IR */
    int64_t z = (int64_t)(((int64_t)(t[2] << 12) + (int64_t)mat(0, 2, 0) * vec[0] + (int64_t)mat(0, 2, 1) * vec[1] + (int64_t)mat(0, 2, 2) * vec[2]) >> 12);
    IR1 = irclamp(MAC1, 1); IR2 = irclamp(MAC2, 2);
    { int64_t m3 = MAC3; int64_t lo = lm ? 0 : -0x8000; if ((sf ? m3 : (int64_t)(z)) < -0x8000 || (sf ? m3 : (int64_t)z) > 0x7fff) FLAG |= 1u << 22; IR3 = (int32_t)(m3 < lo ? lo : m3 > 0x7fff ? 0x7fff : m3); }
    push_sz(z);
    int64_t n = gte_div();
    int64_t sx = n * IR1 + C[24], sy = n * IR2 + C[25];
    if (sx > 0x7fffffffLL || sx < -0x80000000LL) FLAG |= 1u << 16;
    if (sy > 0x7fffffffLL || sy < -0x80000000LL) FLAG |= 1u << 15;
    push_sxy(sx >> 16, sy >> 16);
    if (last) {
        int64_t m0 = n * C[27] + C[28];
        if (m0 > 0x7fffffffLL || m0 < -0x80000000LL) FLAG |= 1u << 15 | 1u << 16;
        D[24] = (int32_t)m0;
        IR0 = (int32_t)clampflag(m0 >> 12, 0, 0x1000, 1u << 12);
    }
}
static int rgbc(int c) { return (D[6] >> (8 * c)) & 255; }

static void light(int v) {          /* MAC/IR = LLM*V ; depois BK + LCM*IR */
    int32_t vec[3]; getvec(v, vec);
    int64_t z[3] = { 0, 0, 0 };
    mvmva_core(1, vec, z); ir_from_mac();
    int32_t ir[3] = { IR1, IR2, IR3 };
    int64_t bk[3] = { C[13], C[14], C[15] };
    mvmva_core(2, ir, bk); ir_from_mac();
}
static void color_nc(int mode_color, int far) {   /* mode_color: multiplica por RGBC; far: mistura FC */
    if (mode_color) {
        int64_t tmp[3] = { (int64_t)(rgbc(0) << 4) * IR1, (int64_t)(rgbc(1) << 4) * IR2, (int64_t)(rgbc(2) << 4) * IR3 };
        if (far) {
            for (int i = 0; i < 3; i++) {
                int64_t d = ((int64_t)C[21 + i] << 12) - tmp[i];
                int32_t t2 = irclamp(d >> (sf * 12), i + 1);
                setmac(i + 1, tmp[i] + (int64_t)IR0 * t2);
            }
        } else for (int i = 0; i < 3; i++) setmac(i + 1, tmp[i]);
        ir_from_mac();
    }
    push_color();
}

void gte_cmd(uint32_t w) {
    sf = (w >> 19) & 1; lm = (w >> 10) & 1; FLAG = 0;
    int op = w & 0x3f;
    switch (op) {
    case 0x01: rtp(0, 1); break;
    case 0x30: rtp(0, 0); rtp(1, 0); rtp(2, 1); break;
    case 0x06: { int64_t a = (int64_t)VX(0) * 0; (void)a;
        int64_t v = (int64_t)hi16(D[12]) * 0; (void)v;
        int32_t x0 = lo16(D[12]), y0 = hi16(D[12]), x1 = lo16(D[13]), y1 = hi16(D[13]), x2 = lo16(D[14]), y2 = hi16(D[14]);
        int64_t m = (int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 - (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1;
        if (m > 0x7fffffffLL || m < -0x80000000LL) FLAG |= 1u << 16;
        MAC0 = (int32_t)m; break; }
    case 0x0c: { int32_t d1 = mat(0, 0, 0), d2 = mat(0, 1, 1), d3 = mat(0, 2, 2); int32_t i1 = IR1, i2 = IR2, i3 = IR3;
        setmac(1, (int64_t)d2 * i3 - (int64_t)d3 * i2); setmac(2, (int64_t)d3 * i1 - (int64_t)d1 * i3); setmac(3, (int64_t)d1 * i2 - (int64_t)d2 * i1);
        ir_from_mac(); break; }
    case 0x28: { setmac(1, (int64_t)IR1 * IR1); setmac(2, (int64_t)IR2 * IR2); setmac(3, (int64_t)IR3 * IR3); ir_from_mac(); break; }
    case 0x2d: { int64_t s = (int64_t)(uint16_t)C[29] * ((uint16_t)D[17] + (uint16_t)D[18] + (uint16_t)D[19]); MAC0 = (int32_t)s; D[7] = (int32_t)clampflag(s >> 12, 0, 0xffff, 1u << 18); break; }
    case 0x2e: { int64_t s = (int64_t)(uint16_t)C[30] * ((uint16_t)D[16] + (uint16_t)D[17] + (uint16_t)D[18] + (uint16_t)D[19]); MAC0 = (int32_t)s; D[7] = (int32_t)clampflag(s >> 12, 0, 0xffff, 1u << 18); break; }
    case 0x12: { int32_t vec[3]; getvec((w >> 15) & 3, vec); int mx = (w >> 17) & 3, tx = (w >> 13) & 3;
        int64_t t[3] = { 0, 0, 0 }; if (tx == 0) { t[0] = C[5]; t[1] = C[6]; t[2] = C[7]; } else if (tx == 1) { t[0] = C[13]; t[1] = C[14]; t[2] = C[15]; } else if (tx == 2) { t[0] = C[21]; t[1] = C[22]; t[2] = C[23]; }
        mvmva_core(mx == 3 ? 0 : mx, vec, t); ir_from_mac(); break; }
    case 0x11: { /* INTPL */
        int64_t base[3] = { (int64_t)IR1 << 12, (int64_t)IR2 << 12, (int64_t)IR3 << 12 };
        for (int i = 0; i < 3; i++) { int64_t d = ((int64_t)C[21 + i] << 12) - base[i]; int32_t t2 = irclamp(d >> (sf * 12), i + 1); setmac(i + 1, base[i] + (int64_t)IR0 * t2); }
        ir_from_mac(); push_color(); break; }
    case 0x1e: light(0); color_nc(0, 0); break;                                            /* NCS */
    case 0x1b: light(0); color_nc(1, 0); break;                                            /* NCCS */
    case 0x13: light(0); color_nc(1, 1); break;                                            /* NCDS */
    case 0x20: light(0); color_nc(0, 0); light(1); color_nc(0, 0); light(2); color_nc(0, 0); break; /* NCT */
    case 0x3f: light(0); color_nc(1, 0); light(1); color_nc(1, 0); light(2); color_nc(1, 0); break; /* NCCT */
    case 0x16: light(0); color_nc(1, 1); light(1); color_nc(1, 1); light(2); color_nc(1, 1); break; /* NCDT */
    case 0x1c: { int32_t ir[3] = { IR1, IR2, IR3 }; int64_t bk[3] = { C[13], C[14], C[15] }; mvmva_core(2, ir, bk); ir_from_mac(); color_nc(1, 0); break; }  /* CC */
    case 0x14: { int32_t ir[3] = { IR1, IR2, IR3 }; int64_t bk[3] = { C[13], C[14], C[15] }; mvmva_core(2, ir, bk); ir_from_mac(); color_nc(1, 1); break; }  /* CDP */
    case 0x29: color_nc(1, 1); break;                                                      /* DCPL */
    default: break;
    }
    /* DPCS/DPCT/GPF/GPL tratados abaixo */
    switch (op) {
    case 0x10: case 0x2a: {                           /* DPCS / DPCT */
        int n = op == 0x2a ? 3 : 1;
        for (int k = 0; k < n; k++) {
            uint32_t src = op == 0x2a ? (uint32_t)D[20] : (uint32_t)D[6];
            for (int i = 0; i < 3; i++) {
                int64_t base = (int64_t)((src >> (8 * i)) & 255) << 16;
                int64_t d = ((int64_t)C[21 + i] << 12) - base;
                int32_t t2 = irclamp(d >> (sf * 12), i + 1);
                setmac(i + 1, base + (int64_t)IR0 * t2);
            }
            ir_from_mac();
            push_color();
        }
        break; }
    case 0x3d: case 0x3e: {                            /* GPF / GPL */
        int64_t b[3] = { 0, 0, 0 };
        if (op == 0x3e) { b[0] = (int64_t)MAC1 << (sf * 12); b[1] = (int64_t)MAC2 << (sf * 12); b[2] = (int64_t)MAC3 << (sf * 12); }
        setmac(1, b[0] + (int64_t)IR0 * IR1); setmac(2, b[1] + (int64_t)IR0 * IR2); setmac(3, b[2] + (int64_t)IR0 * IR3);
        ir_from_mac(); push_color(); break; }
    }
}
