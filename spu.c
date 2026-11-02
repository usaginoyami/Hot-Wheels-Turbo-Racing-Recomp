/* SPU do PS1: 24 vozes ADPCM + ADSR, RAM de 512 KB, DMA canal 4 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "runtime.h"

#define SPU_RAM_SIZE (512 * 1024)
static uint8_t spu_ram[SPU_RAM_SIZE];
static uint16_t regs[0x200];              /* 0x1f801c00 .. 0x1f801fff, indice = (addr-0x1f801c00)/2 */
static uint32_t xfer_addr;                /* endereco de transferencia (bytes) */

typedef struct {
    int on;
    uint32_t addr, repeat, cur_block_addr;
    int16_t buf[2][28]; int cur;          /* bloco atual e proximo, ja decodificados */
    int stop_after[2];                    /* o bloco termina a voz (flag end sem repeat) */
    int s1, s2;
    uint32_t counter;                     /* 4.12 dentro do bloco: bits altos = indice da amostra */
    int env; int phase;                   /* 0 attack,1 decay,2 sustain,3 release,4 off */
    int ecount;
    int envx;
} Voice;
static Voice vo[24];
static uint32_t endx;
static int spu_irq_flag, spu_irq_pend;
static void spu_irq_hit(uint32_t addr, uint32_t len) {   /* IRQ da SPU: endereco programado em 0x1f801da4 foi lido/escrito */
    if (!(regs[0x1aa / 2] & 0x40)) return;
    uint32_t ia = (uint32_t)regs[0x1a4 / 2] * 8; addr &= SPU_RAM_SIZE - 1;
    if (ia >= addr && ia < addr + len) { if (!spu_irq_flag) spu_irq_pend = 1; spu_irq_flag = 1; }
}
int spu_irq_take(void) { int p = spu_irq_pend; spu_irq_pend = 0; return p; }

#define R(i) regs[(i)]
#define VREG(v, o) regs[((v) * 16 + (o)) / 2]

static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };

static void decode_block(Voice* v, int slot) {
    spu_irq_hit(v->addr, 16);
    uint8_t* b = &spu_ram[v->addr & (SPU_RAM_SIZE - 1)];
    int shift = b[0] & 15, filter = (b[0] >> 4) & 7, flags = b[1];
    if (shift > 12) shift = 9;
    if (filter > 4) filter = 4;
    if (flags & 4) v->repeat = v->addr;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int s = (int16_t)(nib << 12) >> shift;
        s += (v->s1 * f0[filter] + v->s2 * f1[filter] + 32) >> 6;
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        v->s2 = v->s1; v->s1 = s; v->buf[slot][i] = s;
    }
    v->stop_after[slot] = 0;
    if (flags & 1) {
        endx |= 1u << (v - vo);
        if (flags & 2) v->addr = v->repeat; else { v->stop_after[slot] = 1; v->addr += 16; }
        if (flags & 2) return;
    } else v->addr += 16;
}

static void key_on(int n) {
    Voice* v = &vo[n];
    v->addr = (uint32_t)VREG(n, 6) * 8; v->repeat = v->addr;
    v->s1 = v->s2 = 0; v->counter = 0; v->env = 0; v->phase = 0; v->ecount = 0; v->on = 1;
    endx &= ~(1u << n);
    v->cur = 0; decode_block(v, 0); decode_block(v, 1);
}
static void key_off(int n) { if (vo[n].on && vo[n].phase != 4) { vo[n].phase = 3; vo[n].ecount = 0; } }

/* um passo de envelope */
static void env_step(Voice* v, int n) {
    uint16_t lo = VREG(n, 8), hi = VREG(n, 10);
    int dec, exp, shift, step, target = -1;
    switch (v->phase) {
    case 0: exp = (lo >> 15) & 1; shift = (lo >> 10) & 31; step = (lo >> 8) & 3; dec = 0; break;
    case 1: exp = 1; shift = (lo >> 4) & 15; step = 0; dec = 1; target = ((lo & 15) + 1) * 0x800; break;
    case 2: exp = (hi >> 15) & 1; dec = (hi >> 14) & 1; shift = (hi >> 8) & 31; step = (hi >> 6) & 3; break;
    default: exp = (hi >> 5) & 1; dec = 1; shift = hi & 31; step = 0; break;
    }
    int sv = dec ? -8 + step : 7 - step;
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    if (shift < 11) sv <<= (11 - shift);
    if (exp && !dec && v->env > 0x6000) cycles *= 4;
    if (exp && dec) sv = (sv * v->env) >> 15;
    if (++v->ecount < cycles) { if (v->phase == 1 && 0) {} return; }
    v->ecount = 0;
    v->env += sv; if (v->env < 0) v->env = 0; if (v->env > 0x7fff) v->env = 0x7fff;
    if (v->phase == 0 && v->env >= 0x7fff) v->phase = 1;
    else if (v->phase == 1 && v->env <= target) v->phase = 2;
    else if (v->phase == 3 && v->env == 0) { v->phase = 4; v->on = 0; }
}

int xa_mix(int*, int*); int cdda_mix(int*, int*);
static int vol_of(uint16_t r) { if (r & 0x8000) return 0x3fff; return (int16_t)(r << 1) >> 1; }  /* modo sweep: aproximado */


/* ---- Reverb (algoritmo nocash): roda a 22050 Hz, buffer em spu_ram a partir de mBASE ---- */
static uint32_t rv_cur;          /* ponteiro relativo (bytes) dentro do buffer */
static int rv_lout, rv_rout;     /* ultima saida (mantida entre amostras) */
static int rv_odd;
#define RV(o) ((int16_t)R((o) / 2))
static int rv_vol(int16_t x, int v) { return (v * x) >> 15; }
static uint32_t rv_addr(uint32_t off_reg_val, int extra) {   /* off_reg_val em unidades de 8 bytes */
    uint32_t base = (uint32_t)R(0x1a2 / 2) * 8; if (base >= SPU_RAM_SIZE) base = SPU_RAM_SIZE - 2;
    uint32_t span = SPU_RAM_SIZE - base; if (!span) span = 2;
    uint32_t rel = (rv_cur + off_reg_val * 8 + (uint32_t)(int32_t)extra) % span;
    return base + (rel & ~1u);
}
static int rv_rd(uint32_t offreg, int extra) { uint32_t a = rv_addr(offreg, extra); return (int16_t)(spu_ram[a] | (spu_ram[a + 1] << 8)); }
static void rv_wr(uint32_t offreg, int v) {
    uint32_t a = rv_addr(offreg, 0); if (v > 32767) v = 32767; if (v < -32768) v = -32768;
    spu_ram[a] = v & 255; spu_ram[a + 1] = (v >> 8) & 255;
}
static int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static void reverb_step(int inl, int inr, int enable) {
    if (rv_odd ^= 1) {   /* a cada 2 amostras (44.1k -> 22.05k) */
        int vIIR = RV(0x1c4), vWALL = RV(0x1ce), vAPF1 = RV(0x1d0), vAPF2 = RV(0x1d2);
        int Lin = rv_vol(inl, RV(0x1fc)), Rin = rv_vol(inr, RV(0x1fe));
        #define RR(o) ((uint32_t)R((o) / 2))
        if (enable) {
            int p;
            p = rv_rd(RR(0x1d4), -2); rv_wr(RR(0x1d4), clamp16(rv_vol(clamp16(Lin + rv_vol(rv_rd(RR(0x1e0), 0), vWALL) - p), vIIR) + p));
            p = rv_rd(RR(0x1d6), -2); rv_wr(RR(0x1d6), clamp16(rv_vol(clamp16(Rin + rv_vol(rv_rd(RR(0x1e2), 0), vWALL) - p), vIIR) + p));
            p = rv_rd(RR(0x1e4), -2); rv_wr(RR(0x1e4), clamp16(rv_vol(clamp16(Lin + rv_vol(rv_rd(RR(0x1f2), 0), vWALL) - p), vIIR) + p));   /* dRDIFF */
            p = rv_rd(RR(0x1e6), -2); rv_wr(RR(0x1e6), clamp16(rv_vol(clamp16(Rin + rv_vol(rv_rd(RR(0x1f0), 0), vWALL) - p), vIIR) + p));            /* dLDIFF */
        }
        int Lo = rv_vol(rv_rd(RR(0x1d8), 0), RV(0x1c6)) + rv_vol(rv_rd(RR(0x1dc), 0), RV(0x1c8)) + rv_vol(rv_rd(RR(0x1e8), 0), RV(0x1ca)) + rv_vol(rv_rd(RR(0x1ec), 0), RV(0x1cc));
        int Ro = rv_vol(rv_rd(RR(0x1da), 0), RV(0x1c6)) + rv_vol(rv_rd(RR(0x1de), 0), RV(0x1c8)) + rv_vol(rv_rd(RR(0x1ea), 0), RV(0x1ca)) + rv_vol(rv_rd(RR(0x1ee), 0), RV(0x1cc));
        Lo = clamp16(Lo); Ro = clamp16(Ro);
        int a1 = rv_rd(RR(0x1f4), -(int)(RR(0x1c0) * 8)), b1 = rv_rd(RR(0x1f6), -(int)(RR(0x1c0) * 8));
        Lo = clamp16(Lo - rv_vol(a1, vAPF1)); Ro = clamp16(Ro - rv_vol(b1, vAPF1));
        if (enable) { rv_wr(RR(0x1f4), Lo); rv_wr(RR(0x1f6), Ro); }
        Lo = clamp16(rv_vol(Lo, vAPF1) + a1); Ro = clamp16(rv_vol(Ro, vAPF1) + b1);
        int a2 = rv_rd(RR(0x1f8), -(int)(RR(0x1c2) * 8)), b2 = rv_rd(RR(0x1fa), -(int)(RR(0x1c2) * 8));
        Lo = clamp16(Lo - rv_vol(a2, vAPF2)); Ro = clamp16(Ro - rv_vol(b2, vAPF2));
        if (enable) { rv_wr(RR(0x1f8), Lo); rv_wr(RR(0x1fa), Ro); }
        Lo = clamp16(rv_vol(Lo, vAPF2) + a2); Ro = clamp16(rv_vol(Ro, vAPF2) + b2);
        rv_lout = Lo; rv_rout = Ro;
        uint32_t base = (uint32_t)R(0x1a2 / 2) * 8, span = SPU_RAM_SIZE - (base < SPU_RAM_SIZE ? base : 0);
        rv_cur = (rv_cur + 2) % (span ? span : 2);
    }
}

void spu_mix(int16_t* out, int n) {   /* n pares estereo */
    int ctrl = R(0x1aa / 2);
    for (int i = 0; i < n; i++) {
        int64_t l = 0, r = 0; int rvl = 0, rvr = 0; uint32_t eon = R(0x198 / 2) | ((uint32_t)R(0x19a / 2) << 16);
        for (int k = 0; k < 24; k++) {
            Voice* v = &vo[k];
            if (!v->on) continue;
            int idx = v->counter >> 12, fr = v->counter & 0xfff;
            int s0 = v->buf[v->cur][idx];
            int s1 = idx < 27 ? v->buf[v->cur][idx + 1] : v->buf[v->cur ^ 1][0];
            int smp = s0 + (((s1 - s0) * fr) >> 12);
            env_step(v, k);
            int e = v->env;
            smp = (smp * e) >> 15;
            { int vl = (smp * vol_of(VREG(k, 0))) >> 14, vr = (smp * vol_of(VREG(k, 2))) >> 14; l += vl; r += vr; if (eon & (1u << k)) { rvl += vl; rvr += vr; } }
            int pitch = VREG(k, 4); if (pitch > 0x3fff) pitch = 0x3fff;
            v->counter += pitch;
            if ((v->counter >> 12) >= 28) {
                v->counter -= 28 << 12;
                int ended = v->stop_after[v->cur];
                v->cur ^= 1;
                if (ended) { v->phase = 4; v->on = 0; v->env = 0; continue; }
                decode_block(v, v->cur ^ 1);
            }
        }
        int ml = vol_of(R(0x180 / 2)), mr = vol_of(R(0x182 / 2));
        l = (l * ml) >> 14; r = (r * mr) >> 14;
        reverb_step(clamp16(rvl), clamp16(rvr), (ctrl & 0x80) != 0);
        if (ctrl & 0x80) { l += (rv_lout * vol_of(R(0x184 / 2))) >> 15; r += (rv_rout * vol_of(R(0x186 / 2))) >> 15; }
        if (!(ctrl & 0x8000) || !(ctrl & 0x4000)) l = r = 0;
        { int cl, cr; if (cdda_mix(&cl, &cr) && (ctrl & 1)) { l += (cl * vol_of(R(0x1b0 / 2))) >> 14; r += (cr * vol_of(R(0x1b2 / 2))) >> 14; } }
        { int xl, xr; if (xa_mix(&xl, &xr) && (ctrl & 1)) { l += (xl * vol_of(R(0x1b0 / 2))) >> 14; r += (xr * vol_of(R(0x1b2 / 2))) >> 14; } }
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        out[i * 2] = (int16_t)l; out[i * 2 + 1] = (int16_t)r;
    }
}

uint32_t spu_read(uint32_t a, int n) {
    uint32_t o = (a - 0x1f801c00) & ~1u;
    if (n == 4) return spu_read(a, 2) | (spu_read(a + 2, 2) << 16);
    switch (o) {
    case 0x1ae: return (R(0x1aa / 2) & 0x3f) | (spu_irq_flag ? 0x40 : 0);                    /* SPUSTAT */
    case 0x19c: return endx & 0xffff;
    case 0x19e: return (endx >> 16) & 0xff;
    case 0x1a6: return (xfer_addr / 8) & 0xffff;
    }
    if (o < 0x180 && (o & 15) == 12) { int v = o / 16; return (uint32_t)(vo[v].env) & 0xffff; }   /* ENVX */
    return R(o / 2);
}
void spu_write(uint32_t a, int n, uint32_t val) {
    if (n == 4) { spu_write(a, 2, val & 0xffff); spu_write(a + 2, 2, val >> 16); return; }
    if (n == 1) { return; }
    uint32_t o = (a - 0x1f801c00) & ~1u; uint16_t v = (uint16_t)val;
    R(o / 2) = v;
    switch (o) {
    case 0x188: for (int i = 0; i < 16; i++) if (v & (1u << i)) key_on(i); break;
    case 0x18a: for (int i = 0; i < 8; i++) if (v & (1u << i)) key_on(16 + i); break;
    case 0x18c: for (int i = 0; i < 16; i++) if (v & (1u << i)) key_off(i); break;
    case 0x18e: for (int i = 0; i < 8; i++) if (v & (1u << i)) key_off(16 + i); break;
    case 0x1a6: xfer_addr = (uint32_t)v * 8; break;
    case 0x1aa: if (!(v & 0x40)) { spu_irq_flag = 0; spu_irq_pend = 0; } break;
    case 0x1a8: spu_irq_hit(xfer_addr, 2); spu_ram[xfer_addr & (SPU_RAM_SIZE - 1)] = v & 255; spu_ram[(xfer_addr + 1) & (SPU_RAM_SIZE - 1)] = v >> 8; xfer_addr = (xfer_addr + 2) & (SPU_RAM_SIZE - 1); break;
    }
}
void spu_dma(uint32_t madr, uint32_t words, int to_spu) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t ra = (madr + 4 * i) & 0x1ffffc;
        if (to_spu) { spu_irq_hit(xfer_addr, 4); uint32_t w = rd32(ra | 0x80000000); for (int k = 0; k < 4; k++) spu_ram[(xfer_addr + k) & (SPU_RAM_SIZE - 1)] = (w >> (8 * k)) & 255; }
        else { uint32_t w = 0; for (int k = 0; k < 4; k++) w |= (uint32_t)spu_ram[(xfer_addr + k) & (SPU_RAM_SIZE - 1)] << (8 * k); wr32(ra | 0x80000000, w); }
        xfer_addr = (xfer_addr + 4) & (SPU_RAM_SIZE - 1);
    }
}
