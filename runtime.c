#include "krom_font.h"
#include "runtime.h"
#include "overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
int hw_verbose; void hw_poll(void); void hw_set_bin(const char*); uint32_t hw_critical(int); extern uint32_t irq_handler_struct; void irq_check(void);
double now_s(void);

uint32_t R[32], HI, LO;
static uint8_t ram[0x200000], spad[0x400];
static uint32_t cop0[32];
static uint64_t g_calls; static uint64_t g_limit = 4000000000ull;
static int g_verbose; static uint32_t ring[64], ring_ra[64]; static int ring_i;
static uint32_t heap_addr, heap_size, heap_top;
/* malloc simples (primeiro-que-couber, com juncao): bloco = [tam|usado][dados] */
static uint32_t hl_malloc(uint32_t n, int zero) {
    n = (n + 7) & ~7u; if (!n) n = 8;
    if (!heap_addr) { heap_addr = 0x80180000u; heap_size = 0x70000u; }
    if (!heap_top) { heap_top = 1; wr32(heap_addr, (heap_size - 8) & ~7u); }   /* um bloco livre do tamanho todo */
    uint32_t p = heap_addr, end = heap_addr + heap_size;
    while (p + 8 <= end) {
        uint32_t h = rd32(p), sz = h & ~7u, used = h & 1;
        if (!sz) break;
        if (!used) {
            uint32_t q = p + 8 + sz;                                  /* junta com os livres seguintes */
            while (q + 8 <= end && !(rd32(q) & 1) && (rd32(q) & ~7u)) { sz += 8 + (rd32(q) & ~7u); q = p + 8 + sz; }
            if (sz >= n) {
                if (sz >= n + 16) { wr32(p + 8 + n, sz - n - 8); sz = n; }
                wr32(p, sz | 1);
                if (zero) for (uint32_t k = 0; k < sz; k++) wr8(p + 8 + k, 0);
                return p + 8;
            }
            wr32(p, sz);
        }
        p += 8 + sz;
    }
    return 0;
}
static void hl_free(uint32_t a) { if (a >= heap_addr + 8 && a < heap_addr + heap_size) wr32(a - 8, rd32(a - 8) & ~1u); }


static uint8_t* ptr(uint32_t a);
uint8_t* ram_ptr(uint32_t a) { return ptr(a); }
static uint8_t* ptr(uint32_t a) {
    a &= 0x1fffffff;
    if (a < 0x800000) return ram + (a & 0x1fffff);   /* RAM de 2 MB espelhada 4x */
    if (a >= 0x1f800000 && a < 0x1f800400) return spad + (a - 0x1f800000);
    return NULL;
}
uint32_t io_rd(uint32_t a, int n); void io_wr(uint32_t a, int n, uint32_t v);
uint8_t rd8(uint32_t a) { uint8_t* p = ptr(a); return p ? *p : (uint8_t)io_rd(a, 1); }
uint16_t rd16(uint32_t a) { uint8_t* p = ptr(a); if (!p) return (uint16_t)io_rd(a, 2); uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t rd32(uint32_t a) { uint8_t* p = ptr(a); if (!p) return io_rd(a, 4); uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t g_wlo, g_whi; static int g_winit;
extern uint32_t R[]; uint32_t dbg_lastfn(void);
static void wchk(uint32_t a, int n, uint32_t v) {
    if (!g_winit) { g_winit = 1; const char* e = getenv("WRANGE"); if (e) sscanf(e, "%x,%x", &g_wlo, &g_whi); }
    extern uint32_t g_frames; if (g_whi && g_frames >= 8200 && (a & 0x1fffffff) >= g_wlo && (a & 0x1fffffff) < g_whi) { static int cnt; static uint32_t lastra; static int rep; if (R[31] != lastra) { lastra = R[31]; rep = 0; } if (rep++ < 2 && cnt++ < 3000) fprintf(stderr, "[wr] %08x n=%d v=%08x ra=%08x fn=%08x\n", a, n, v, R[31], dbg_lastfn()); }
}
void wr8(uint32_t a, uint32_t v) { wchk(a, 1, v); uint8_t* p = ptr(a); if (p) *p = (uint8_t)v; else io_wr(a, 1, v); }
void wr16(uint32_t a, uint32_t v) { wchk(a, 2, v); uint8_t* p = ptr(a); if (p) { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); } else io_wr(a, 2, v); }
void wr32(uint32_t a, uint32_t v) { wchk(a, 4, v); uint8_t* p = ptr(a); if (p) memcpy(p, &v, 4); else io_wr(a, 4, v); }
uint32_t lwl(uint32_t a, uint32_t old) { uint32_t b = a & 3, w = rd32(a & ~3u); uint32_t m[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0}; int s[4] = {24, 16, 8, 0}; return (old & m[b]) | (w << s[b]); }
uint32_t lwr(uint32_t a, uint32_t old) { uint32_t b = a & 3, w = rd32(a & ~3u); uint32_t m[4] = {0, 0xff000000, 0xffff0000, 0xffffff00}; int s[4] = {0, 8, 16, 24}; return (old & m[b]) | (w >> s[b]); }
void swl(uint32_t a, uint32_t v) { uint32_t b = a & 3, w = rd32(a & ~3u); uint32_t m[4] = {0xffffff00, 0xffff0000, 0xff000000, 0}; int s[4] = {24, 16, 8, 0}; wr32(a & ~3u, (w & m[b]) | (v >> s[b])); }
void swr(uint32_t a, uint32_t v) { uint32_t b = a & 3, w = rd32(a & ~3u); uint32_t m[4] = {0, 0x000000ff, 0x0000ffff, 0x00ffffff}; int s[4] = {0, 8, 16, 24}; wr32(a & ~3u, (w & m[b]) | (v << s[b])); }

uint32_t dbg_lastfn(void) { return ring[(ring_i - 1) & 63]; }
uint32_t dbg_ra(void) { return R[31]; }
void dbg_ring(int n) { fprintf(stderr, "   ring:"); for (int i = n; i >= 1; i--) fprintf(stderr, " %08x(%08x)", ring[(ring_i - i) & 63], ring_ra[(ring_i - i) & 63]); fprintf(stderr, "\n"); }
static void dump_ring(void) {
    fprintf(stderr, "ultimas funcoes (mais recente por ultimo):\n");
    for (int i = 0; i < 64; i++) { uint32_t a = ring[(ring_i + i) & 63]; if (a) fprintf(stderr, "  %08x (ra=%08x)\n", a, ring_ra[(ring_i + i) & 63]); }
}
static void on_alarm(int s) { (void)s; fprintf(stderr, "\n[travou] loop sem fim (provavelmente espera de hardware). Chamadas: %llu\n", (unsigned long long)g_calls); dump_ring(); _exit(3); }
static void run_vblank(void); void card_poll(void);
static uint32_t g_watch, g_wval; static int g_watch_init;
int g_trace_n;
void trace_enter(uint32_t a) {
    if (g_trace_n > 0) { extern int hw_in_isr(void); if (!hw_in_isr()) { g_trace_n--; fprintf(stderr, "-> %08x (ra=%08x a0=%08x a1=%08x)\n", a, R[31], R[4], R[5]); } }
    if (!g_watch_init) { g_watch_init = 1; if (getenv("WATCH")) g_watch = strtoul(getenv("WATCH"), 0, 16); }
    if (g_watch) { uint32_t v = rd32(g_watch); if (v != g_wval) { fprintf(stderr, "[watch] %08x: %x -> %x em %08x (ra=%08x) chamada %llu\n", g_watch, g_wval, v, a, R[31], (unsigned long long)g_calls); g_wval = v; } }
    { extern double g_vclock; g_vclock += 1.5e-6; }
    ring_ra[ring_i & 63] = R[31]; ring[ring_i++ & 63] = a; g_calls++;
    if (g_verbose) fprintf(stderr, "-> %08x (ra=%08x a0=%08x a1=%08x)\n", a, R[31], R[4], R[5]);
    if (!(g_calls & 0xff)) { run_vblank(); hw_poll(); }
    if (g_calls > g_limit) { fprintf(stderr, "[limite] %llu chamadas\n", (unsigned long long)g_calls); dump_ring(); exit(4); }
}
void cpu_syscall(uint32_t pc) {
    if (R[4] == 1) { R[2] = hw_critical(1); return; }      /* EnterCriticalSection */
    if (R[4] == 2) { hw_critical(0); return; }             /* ExitCriticalSection */
    fprintf(stderr, "[syscall] em %08x a0=%u\n", pc, R[4]);
}
void cpu_break(uint32_t pc) { fprintf(stderr, "[break] em %08x\n", pc); dump_ring(); exit(5); }
void cpu_unknown(uint32_t pc, uint32_t w) { fprintf(stderr, "[instrucao desconhecida] %08x em %08x\n", w, pc); dump_ring(); exit(6); }
void cpu_fell_off(uint32_t pc) { fprintf(stderr, "[fim de funcao] executou %08x, fora do codigo traduzido\n", pc); dump_ring(); exit(7); }
void cpu_div(uint32_t a, uint32_t b) { int32_t x = a, y = b; if (!y) { LO = x < 0 ? 1 : 0xffffffff; HI = x; } else if (x == INT32_MIN && y == -1) { LO = 0x80000000; HI = 0; } else { LO = x / y; HI = x % y; } }
void cpu_divu(uint32_t a, uint32_t b) { if (!b) { LO = 0xffffffff; HI = a; } else { LO = a / b; HI = a % b; } }
uint32_t cop0_rd(int r) { return cop0[r]; }
void cop0_wr(int r, uint32_t v) { cop0[r] = v; }

/* ---- ganchos HLE: substituem funcoes da biblioteca Sony por versoes em C ---- */
static uint32_t g_vcount;
char* fast_getenv(const char* k) {
    static struct { const char* k; char* v; } t[1024]; static int init;
    size_t h = ((size_t)k >> 3) & 1023;
    for (int i = 0; i < 1024; i++, h = (h + 1) & 1023) { if (t[h].k == k) return t[h].v; if (!t[h].k) { t[h].k = k; t[h].v = (getenv)(k); return t[h].v; } }
    (void)init; return (getenv)(k);
}
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
double g_vbonus; double g_vclock; static int g_vtime = 0;   /* no Windows o relogio e sempre o real */
double now_s(void) { static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart + g_vbonus; }   /* relogio de alta resolucao (clock_gettime no Windows pode ser grosseiro: ~15 ms) */
static void win_usleep(unsigned us) { if (us >= 400) { Sleep(1); return; } double t = now_s() + us * 1e-6; while (now_s() < t) YieldProcessor(); }   /* usleep do Windows tem granularidade de ~15 ms: quebrava o ritmo dos setores do CD (video) */
#define usleep(x) win_usleep(x)
#else
double g_vbonus;   /* tempo virtual: cada consulta (polling) do jogo a hardware avanca o relogio, p/ laços de espera com limite de iteracoes nao estourarem antes dos eventos do CD */
double g_vclock; static int g_vtime = -1;
double now_s(void) { if (g_vtime < 0) g_vtime = getenv("VTIME") != NULL; if (g_vtime) return 1000.0 + g_vclock + g_vbonus; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9 + g_vbonus; }
#endif
static double g_t0; static uint32_t vblank_count(void) { if (!g_t0) g_t0 = now_s(); return (uint32_t)((now_s() - g_t0) * 59.94); }
static uint32_t g_vcb; static int in_cb; static uint32_t last_disp;
void hw_set_speed(double); void spu_mix(int16_t*, int); int audio_queued(void); void audio_push(const int16_t*, int);
static double audio_t = 0; static void audio_run(void) {
    double now = now_s(); if (audio_t == 0) audio_t = now;
    int n = (int)((now - audio_t) * 44100); if (n <= 0) return; if (n > 4410) { n = 4410; audio_t = now; } else audio_t += n / 44100.0;
    static int16_t buf[4410 * 2]; spu_mix(buf, n);
    { static FILE* af; static int init; if (!init) { init = 1; if (getenv("AUDIO_RAW")) af = fopen(getenv("AUDIO_RAW"), "wb"); } if (af) fwrite(buf, 4, n, af); }
    int q = audio_queued(); if (q >= 0 && q > 6000) return;   /* fila cheia: descarta */
    audio_push(buf, n);
} extern int gpu_logn; void gpu_vblank(void); int gpu_dump(const char*); int gpu_dump_vram(const char*); void gpu_info(void);
uint32_t g_frames; static uint32_t g_shots[32], g_nshots, g_maxf = 0xffffffff;
static uint32_t force_state=0xffffffff, force_at=0;
static int g_pad2; void pad_has2(int v) { g_pad2 = v; }
static uint32_t pad_buf[2]; static uint32_t pad_mask, pad_start_at = 0xffffffff, pad_period = 90;
static uint32_t pad_mask2;
void pad_set(uint32_t m) { pad_mask = m; }
void pad_set2(uint32_t m) { pad_mask2 = m; }
static int g_win;
int video_init(void); void video_present(void);
static struct { uint32_t m, f; } presses[64]; static int npress;
static void add_press(const char* s) {
    static const char* nm[] = { "SELECT","L3","R3","START","UP","RIGHT","DOWN","LEFT","L2","R2","L1","R1","TRIANGLE","CIRCLE","CROSS","SQUARE" };
    char b[32]; strncpy(b, s, 31); b[31] = 0; char* at = strchr(b, '@'); if (!at || npress >= 64) return; *at = 0;
    for (int i = 0; i < 16; i++) if (!strcmp(b, nm[i])) { presses[npress].m = 1u << i; presses[npress++].f = atoi(at + 1); }
}
static void pad_update(void) {
    uint32_t m = pad_mask;
    for (int i = 0; i < npress; i++) if (g_frames >= presses[i].f && g_frames < presses[i].f + 4) m |= presses[i].m;
    if (g_frames >= pad_start_at && (g_frames - pad_start_at) % pad_period < 4) m |= 0x0008;     /* Start automatico (teste) */
    for (int p = 0; p < 2; p++) if (pad_buf[p]) {
        uint32_t a = pad_buf[p];
        wr8(a, 0x00); wr8(a + 1, 0x41);
        uint32_t mm = p == 0 ? m : pad_mask2;
        wr8(a + 2, ~mm & 0xff); wr8(a + 3, (~mm >> 8) & 0xff);
        if (p == 1 && !g_pad2) { wr8(a, 0xff); }   /* porta 2: sem controle ate haver um controle 2 */
    }
}
uint32_t pad_active_low(int p) { uint32_t m = pad_mask; for (int i = 0; i < npress; i++) if (g_frames >= presses[i].f && g_frames < presses[i].f + 4) m |= presses[i].m; if (g_frames >= pad_start_at && (g_frames - pad_start_at) % pad_period < 4) m |= 0x0008; if (p == 1) m = pad_mask2; return ~m & 0xffff; }
static void mc_flush(void);
static void run_vblank(void) { card_poll();
    if (in_cb) return;
    { static int t; if ((++t & 63) == 0) mc_flush(); }
    uint32_t c = vblank_count(); if (c == last_disp) return; last_disp = c; g_frames++; gpu_vblank(); audio_run(); { extern void gpu_resume(void), gpu_force_flush(void); extern int g_gpu_forced; gpu_resume(); gpu_force_flush(); { static int lf; if (g_gpu_forced != lf) { lf = g_gpu_forced; if (lf <= 20 || !(lf % 500)) fprintf(stderr, "[gpu] quadro %u: no sem ligacao tratado como fim de lista (%d)\n", g_frames, lf); } } if (0) {} else if (g_win) { extern int gpu_flat(void); static int fl; if (gpu_flat() && fl < 3) { fl++; static int sf; if (++sf <= 10) fprintf(stderr, "[gpu] quadro %u de cor unica - mantem imagem anterior (%d)\n", g_frames, sf); } else { fl = gpu_flat() ? fl : 0; video_present(); } } } pad_update();
    if (getenv("GPU_LOGAT") && g_frames == (uint32_t)atoi(getenv("GPU_LOGAT"))) gpu_logn = 3000;
    if (getenv("VERBOSEAT")) { int f = atoi(getenv("VERBOSEAT")); if ((int)g_frames == f) g_verbose = 1; if ((int)g_frames == f + 1) g_verbose = 0; }
    if (getenv("DUMPAT") && (int)g_frames == atoi(getenv("DUMPAT"))) { FILE* df = fopen("ram.bin", "wb"); fwrite(ram, 1, sizeof ram, df); fclose(df); }
    if (force_state != 0xffffffff && g_frames == force_at) { wr16(0x80034fdc, force_state); wr16(0x80056878, 1); fprintf(stderr, "[estado] forcado %x no quadro %u\n", force_state, g_frames); }
    { static uint32_t last_state = 0xffffffff; uint32_t st = (rd32(0x80034fdc) & 0xffff) | (getenv("DBGSUB") ? rd32(0x80057484) << 16 : 0); if (st != last_state) { if (getenv("DBGSTATE")) fprintf(stderr, "[estado] quadro %u: %02x -> %02x (sub %x)\n", g_frames, last_state & 0xff, st, rd32(0x80057484)); last_state = st; } }
    { static int ta; if (!ta) ta = getenv("TRACEAT") ? atoi(getenv("TRACEAT")) : -1; extern int g_trace_n; if (ta > 0 && g_frames == (uint32_t)ta) { g_trace_n = 400; ta = -2; } }
    if (getenv("TRACE_SUB")) { static int done; extern int g_trace_n; if (!done && rd32(0x80057484) == (uint32_t)strtoul(getenv("TRACE_SUB"), 0, 16) && g_frames % 60 == 0) { done = 1; g_trace_n = 20000; } }
    if (getenv("DBGS") && g_frames % 120 == 0) fprintf(stderr, "[estado] quadro %u substado=%x main=%x\n", g_frames, rd32(0x80057484), rd32(R[28] + 0x108));
    for (uint32_t k = 0; k < g_nshots; k++) if (g_frames == g_shots[k]) { char fn[64]; sprintf(fn, "shot_%u.ppm", g_frames); gpu_dump(fn); if (k == g_nshots - 1) gpu_dump_vram("vram.ppm"); gpu_info(); }
    if (g_frames >= g_maxf) { fprintf(stderr, "[frames] fim\n"); { extern void hw_dump(void); hw_dump(); dump_ring(); if (getenv("HIST")) { extern void hw_hist(void); hw_hist(); } } exit(0); }
    { extern void hw_vblank_irq(void); in_cb = 1; hw_vblank_irq(); in_cb = 0; }
}
void card_poll(void);
uint32_t g_looppc; uint32_t g_bk; void loop_tick(void) { extern double g_vclock; static uint64_t last; if (getenv("NOTICK")) return; g_vclock += 100e-6; if (g_calls == last) { run_vblank(); hw_poll(); } last = g_calls; }
static void wait_vblank(void) { uint32_t c = vblank_count(); while (vblank_count() == c) { if (g_vtime > 0) { g_vclock += 500e-6; hw_poll(); } else { hw_poll(); audio_run(); usleep(500); } }   /* tempo real: eventos de hardware (CD, IRQ da SPU) e audio nao podem esperar o fim do quadro */ run_vblank(); }
static int hle_log;
static int ov_active = -1; static int g_skipvid = 0; static int ov_valid;
int hle_hook(uint32_t a) {
    switch (a) {
    case 0x8009fe2c: case 0x800a1850: case 0x800a25bc: { if (getenv("TRQ") && g_frames >= 8230 && g_frames < 8330) fprintf(stderr, "[q] %08x q%u a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x\n", a, g_frames, R[4], R[5], R[6], R[7], R[31]); return 0; }
    case 0x8001219c: { static int n; if (getenv("DBGCB") && n++ < 5) fprintf(stderr, "[cb] 8001219c chamado (quadro %u)\n", g_frames); return 0; }
    case 0x801059b4: if (g_skipvid) { { extern void hw_cd_stop(void); hw_cd_stop(); } fprintf(stderr, "[hle] filme pulado\n"); R[2] = 0; return 1; } { extern int g_trace_n; static int tm; if (getenv("TRACEMOV") && tm++ == 0) g_trace_n = atoi(getenv("TRACEMOV")); } return 0;
    case 0x80111540: case 0x80111550: case 0x800a38cc: case 0x800a38dc: /* VSync(mode) (PsyQ do loader e do CCCPSX) */
        { int32_t m = (int32_t)R[4]; if (m == 1) { g_vbonus += 3e-6; hw_poll(); vblank_count(); R[2] = (uint32_t)((now_s() - g_t0) * 15734.26); if (hle_log++ < 6) fprintf(stderr, "[hle] VSync(1) -> hcount %u\n", R[2]); return 1; }
          if (m >= 0) { int n = m > 1 ? m : 1; while (n--) wait_vblank(); } else { g_vbonus += 3e-6; hw_poll(); } }
        g_vcount = vblank_count(); R[2] = g_vcount;
        if (hle_log++ < 6) fprintf(stderr, "[hle] VSync(%d) -> %u\n", (int)R[4], g_vcount); return 1;
    case 0x8010d7bc: case 0x800a2480: {   /* DrawSync(mode): a GPU emulada termina na hora; mas depois de um comando, a primeira consulta (modo 1) ainda a ve 'ocupada', como no hardware */
        extern int g_gpu_dirty;
        if (R[4] == 0) { hw_poll(); R[2] = 0; } else { R[2] = g_gpu_dirty ? 1 : 0; }
        g_gpu_dirty = 0; return 1; }
    default: return 0;
    }
}

/* ---- BIOS em alto nivel ---- */
static uint32_t next_event = 0xf1000000;

/* ---- eventos do kernel e memory card (HLE) ---- */
typedef struct { uint32_t cls, spec, mode, func; int used, delivered; } KEv;
static KEv kev[64];
static uint8_t mc[131072]; static int mc_init, mc_new = 1;
static void mc_load(void) {
    if (mc_init) return; mc_init = 1;
    FILE* f = fopen("memcard1.mcd", "rb"); if (f) { size_t n = fread(mc, 1, sizeof mc, f); fclose(f); if (n == sizeof mc) return; }
    memset(mc, 0, sizeof mc);
    mc[0] = 'M'; mc[1] = 'C'; mc[127] = 'M' ^ 'C';                                   /* cabecalho formatado */
    for (int i = 1; i < 16; i++) { uint8_t* e = mc + i * 128; e[0] = 0xa0; e[8] = e[9] = 0xff; e[127] = 0xa0; }
    for (int i = 16; i < 36; i++) { uint8_t* e = mc + i * 128; memset(e, 0xff, 4); e[8] = e[9] = 0xff; }
    memcpy(mc + 63 * 128, mc, 128);
}
static int mc_dirty;
static void mc_flush(void) { if (!mc_dirty) return; mc_dirty = 0; FILE* f = fopen("memcard1.mcd", "wb"); if (f) { fwrite(mc, 1, sizeof mc, f); fclose(f); } }
static void mc_save(void) { mc_dirty = 1; }
/* ---- sistema de arquivos do cartao (API de arquivos da BIOS: open/read/write/firstfile...) ---- */
typedef struct { int used, nblk, chain[15], pos, wr; } McFd;
static McFd mcfd[16];
static int mc_scan_slot = 0; static char mc_scan_pat[32];
static void mc_sum(int slot) { uint8_t* e = mc + slot * 128; uint8_t x = 0; for (int i = 0; i < 127; i++) x ^= e[i]; e[127] = x; }
static int mc_match(const char* pat, const char* name) {
    for (;; pat++, name++) { if (*pat == '*') return 1; if (*pat == 0) return *name == 0; if (*pat == '?') { if (!*name) return 0; continue; } if (*pat != *name) return 0; }
}
static void mc_name(uint32_t a, char* out, int n) { int i = 0; for (; i < n - 1; i++) { char c = (char)rd8(a + i); if (!c) break; out[i] = c; } out[i] = 0; }
static const char* mc_path(const char* p) { const char* c = strchr(p, ':'); return c ? c + 1 : p; }
static int mc_find_first(const char* pat, int from) {
    for (int i = from; i < 16; i++) { uint8_t* e = mc + i * 128; if (e[0] == 0x51) { char nm[21]; memcpy(nm, e + 10, 20); nm[20] = 0; if (mc_match(pat, nm)) return i; } }
    return 0;
}
static int mc_chain(int first, int* chain) { int n = 0, s = first; while (s > 0 && s < 16 && n < 15) { chain[n++] = s; uint8_t* e = mc + s * 128; int nx = e[8] | (e[9] << 8); if (nx == 0xffff) break; s = nx + 1; } return n; }
static void mc_fill_dirent(uint32_t d, int slot) {
    uint8_t* e = mc + slot * 128; for (int i = 0; i < 20; i++) wr8(d + i, e[10 + i]);
    wr32(d + 20, 0); wr32(d + 24, e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24)); wr32(d + 28, 0); wr16(d + 32, slot - 1);
}
static int mc_open(uint32_t pathp, uint32_t mode) {
    mc_load(); char p[48]; mc_name(pathp, p, sizeof p);
    fprintf(stderr, "[mcard] open \"%s\" modo=%x\n", p, mode);
    if (strncmp(p, "bu00", 4)) { fprintf(stderr, "[mcard] open: cartao nao e bu00\n"); return -1; }
    const char* nm = mc_path(p); int slot = mc_find_first(nm, 1);
    int fd = -1; for (int i = 0; i < 16; i++) if (!mcfd[i].used) { fd = i; break; } if (fd < 0) { fprintf(stderr, "[mcard] open: sem fd livre\n"); return -1; }
    McFd* f = &mcfd[fd]; memset(f, 0, sizeof *f);
    if (!slot) {
        if (!(mode & 0x200)) { fprintf(stderr, "[mcard] open: arquivo nao existe e sem O_CREAT\n"); return -1; }
        int nb = (mode >> 16) & 15; if (nb < 1) nb = 1; int got = 0, sl[15];
        for (int i = 1; i < 16 && got < nb; i++) if (mc[i * 128] == 0xa0 || mc[i * 128] == 0xa1 || mc[i * 128] == 0xa2 || mc[i * 128] == 0xa3) sl[got++] = i;
        if (got < nb) { fprintf(stderr, "[mcard] open: sem blocos livres (%d de %d)\n", got, nb); return -1; }
        for (int k = 0; k < nb; k++) {
            uint8_t* e = mc + sl[k] * 128; memset(e, 0, 128);
            e[0] = nb == 1 || k == 0 ? 0x51 : (k == nb - 1 ? 0x53 : 0x52); if (nb > 1 && k == 0) e[0] = 0x51;
            int sz = k == 0 ? nb * 8192 : 0; e[4] = sz & 255; e[5] = (sz >> 8) & 255; e[6] = (sz >> 16) & 255; e[7] = (sz >> 24) & 255;
            int nx = k < nb - 1 ? sl[k + 1] - 1 : 0xffff; e[8] = nx & 255; e[9] = nx >> 8;
            if (k == 0) { strncpy((char*)e + 10, nm, 20); }
            mc_sum(sl[k]);
        }
        for (int k = 0; k < nb; k++) { memset(mc + sl[k] * 8192, 0, 8192); }
        slot = sl[0]; mc_dirty = 1;
    }
    f->nblk = mc_chain(slot, f->chain); f->used = 1; f->wr = (mode & 2) != 0;
    fprintf(stderr, "[mcard] open ok fd=%d slot=%d blocos=%d\n", fd, slot, f->nblk);
    return fd;
}
static int mc_rw(int fd, uint32_t buf, int n, int write) {
    if (fd < 0 || fd > 15 || !mcfd[fd].used) { fprintf(stderr, "[mcard] %s: fd invalido %d\n", write ? "write" : "read", fd); return -1; } McFd* f = &mcfd[fd]; int done = 0;
    int total = f->nblk * 8192;
    while (done < n && f->pos < total) {
        int blk = f->pos / 8192, off = f->pos % 8192; uint8_t* d = mc + f->chain[blk] * 8192 + off;
        if (write) *d = (uint8_t)rd8(buf + done); else wr8(buf + done, *d);
        f->pos++; done++;
    }
    if (write && done) mc_dirty = 1;
    fprintf(stderr, "[mcard] %s fd=%d pedido=%d feito=%d\n", write ? "write" : "read", fd, n, done);
    return done;
}
static void kev_deliver(uint32_t cls, uint32_t spec) {
    if (getenv("DBGCARD")) { int m = 0; for (int i = 0; i < 64; i++) if (kev[i].used && kev[i].cls == cls && kev[i].spec == spec) m++; fprintf(stderr, "[kev] deliver cls=%08x spec=%x (casam %d)\n", cls, spec, m); }
    for (int i = 0; i < 64; i++) if (kev[i].used && kev[i].cls == cls && kev[i].spec == spec) {
        if ((kev[i].mode & 0x1000) && kev[i].func) {   /* modo callback: chama a funcao do jogo */
            uint32_t sv[32], shi = HI, slo = LO; memcpy(sv, R, sizeof sv); R[29] -= 0x100; R[31] = 0; call_addr(kev[i].func); memcpy(R, sv, sizeof sv); HI = shi; LO = slo;
        } else kev[i].delivered = 1;
    }
}
static int kev_has(uint32_t cls) { for (int i = 0; i < 64; i++) if (kev[i].used && kev[i].cls == cls) return 1; return 0; }
static struct { double t; uint32_t cls, spec; } cardq[16]; static int cardn;
static void card_ev(uint32_t spec) { if (cardn < 16) { cardq[cardn].t = now_s() + 3e-3; cardq[cardn].cls = kev_has(0xf0000011u) ? 0xf0000011u : 0xf4000001u; cardq[cardn].spec = spec; cardn++; if (kev_has(0xf0000011u) && kev_has(0xf4000001u) && cardn < 16) { cardq[cardn].t = now_s() + 6e-3; cardq[cardn].cls = 0xf4000001u; cardq[cardn].spec = spec; cardn++; } } }   /* o BIOS entrega os eventos do cartao depois, nao dentro da chamada */
void card_poll(void) { while (cardn > 0 && now_s() >= cardq[0].t) { uint32_t c = cardq[0].cls, s = cardq[0].spec; memmove(cardq, cardq + 1, (--cardn) * sizeof cardq[0]); kev_deliver(c, s); } }
static int card_class(uint32_t c) { return c == 0xf4000001u || c == 0xf0000011u; }
uint32_t g_rand = 1;
static void bios_call(uint32_t tbl, uint32_t fn);
static void init_kernel_tables(void) {
    for (uint32_t i = 0; i < 0xc0; i++) wr32(0x200 + i * 4, 0xbfc00000u | (0xa0u << 8) | i);
    for (uint32_t i = 0; i < 0x60; i++) wr32(0x874 + i * 4, 0xbfc00000u | (0xb0u << 8) | i);
    for (uint32_t i = 0; i < 0x20; i++) wr32(0x674 + i * 4, 0xbfc00000u | (0xc0u << 8) | i);
}
static void bios_call(uint32_t tbl, uint32_t fn) {
    uint32_t a0 = R[4], a1 = R[5];
    const char* name = "?";
    R[2] = 0;
    if (tbl == 0xa0) switch (fn) {
        case 0x39: name = "InitHeap"; heap_addr = a0; heap_size = a1; heap_top = 0; break;
        case 0x33: name = "malloc"; R[2] = hl_malloc(a0, 0); break;
        case 0x34: name = "free"; hl_free(a0); break;
        case 0x35: name = "calloc"; R[2] = hl_malloc(a0 * a1, 1); break;
        case 0x9f: name = "SetMem"; break;
        case 0x13: name = "setjmp"; wr32(a0, R[31]); wr32(a0 + 4, R[29]); wr32(a0 + 8, R[30]);
            for (int i = 0; i < 8; i++) wr32(a0 + 12 + 4 * i, R[16 + i]);
            wr32(a0 + 44, R[28]); R[2] = 0; break;
        case 0xab: { name = "_card_info"; mc_load(); int slot = (a0 & 0x10) ? 1 : 0;
            if (slot) card_ev(0x0100); else if (mc_new && !kev_has(0xf0000011u)) card_ev(0x2000); else card_ev(0x0004); R[2] = 1; break; }
        case 0xac: { name = "_card_load"; mc_load(); if (a0 & 0x10) card_ev(0x0100); else { mc_new = 0; card_ev(0x0004); } R[2] = 1; break; }
        /* ---- libc da BIOS (o PsyQ chama estas funcoes por A0) ---- */
        case 0x0a: name = "abs"; R[2] = (int32_t)a0 < 0 ? -(int32_t)a0 : a0; break;
        case 0x0c: case 0x0d: { name = "atoi"; int sg = 1; uint32_t p = a0; while (rd8(p) == ' ') p++; if (rd8(p) == '-') { sg = -1; p++; } else if (rd8(p) == '+') p++; int32_t v = 0; while (rd8(p) >= '0' && rd8(p) <= '9') v = v * 10 + (int)(rd8(p++) - '0'); R[2] = (uint32_t)(v * sg); break; }
        case 0x15: { name = "strcat"; uint32_t d = a0; while (rd8(d)) d++; uint32_t sp = a1; for (;;) { uint32_t c = rd8(sp++); wr8(d++, c); if (!c) break; } R[2] = a0; break; }
        case 0x16: { name = "strncat"; uint32_t d = a0; while (rd8(d)) d++; uint32_t sp = a1, n = R[6]; while (n--) { uint32_t c = rd8(sp++); if (!c) break; wr8(d++, c); } wr8(d, 0); R[2] = a0; break; }
        case 0x17: { name = "strcmp"; uint32_t p = a0, q = a1; int r = 0; for (;;) { int x = (int)rd8(p++), y = (int)rd8(q++); if (x != y) { r = x - y; break; } if (!x) break; } R[2] = (uint32_t)r; break; }
        case 0x18: { name = "strncmp"; uint32_t p = a0, q = a1, n = R[6]; int r = 0; while (n--) { int x = (int)rd8(p++), y = (int)rd8(q++); if (x != y) { r = x - y; break; } if (!x) break; } R[2] = (uint32_t)r; break; }
        case 0x19: { name = "strcpy"; uint32_t d = a0, sp = a1; for (;;) { uint32_t c = rd8(sp++); wr8(d++, c); if (!c) break; } R[2] = a0; break; }
        case 0x1a: { name = "strncpy"; uint32_t d = a0, sp = a1, n = R[6]; int end = 0; while (n--) { uint32_t c = end ? 0 : rd8(sp++); if (!c) end = 1; wr8(d++, c); } R[2] = a0; break; }
        case 0x1b: { name = "strlen"; uint32_t n = 0; while (rd8(a0 + n)) n++; R[2] = n; break; }
        case 0x1c: case 0x1e: { name = "strchr"; uint32_t p = a0; R[2] = 0; for (;;) { uint32_t c = rd8(p); if (c == (a1 & 0xff)) { R[2] = p; break; } if (!c) break; p++; } break; }
        case 0x1d: case 0x1f: { name = "strrchr"; uint32_t p = a0, r = 0; for (;;) { uint32_t c = rd8(p); if (c == (a1 & 0xff)) r = p; if (!c) break; p++; } R[2] = r; break; }
        case 0x25: name = "toupper"; R[2] = (a0 >= 'a' && a0 <= 'z') ? a0 - 32 : a0; break;
        case 0x26: name = "tolower"; R[2] = (a0 >= 'A' && a0 <= 'Z') ? a0 + 32 : a0; break;
        case 0x27: { name = "bcopy"; uint32_t n = R[6]; if (a1 <= a0 || a1 >= a0 + n) for (uint32_t i = 0; i < n; i++) wr8(a1 + i, rd8(a0 + i)); else for (uint32_t i = n; i-- > 0;) wr8(a1 + i, rd8(a0 + i)); break; }
        case 0x28: { name = "bzero"; for (uint32_t i = 0; i < a1; i++) wr8(a0 + i, 0); break; }
        case 0x29: { name = "bcmp"; int r = 0; for (uint32_t i = 0; i < R[6]; i++) { int x = (int)rd8(a0 + i), y = (int)rd8(a1 + i); if (x != y) { r = x - y; break; } } R[2] = (uint32_t)r; break; }
        case 0x2a: case 0x2c: { name = "memcpy"; uint32_t n = R[6]; if (a0 <= a1 || a0 >= a1 + n) for (uint32_t i = 0; i < n; i++) wr8(a0 + i, rd8(a1 + i)); else for (uint32_t i = n; i-- > 0;) wr8(a0 + i, rd8(a1 + i)); R[2] = a0; break; }
        case 0x2b: { name = "memset"; for (uint32_t i = 0; i < R[6]; i++) wr8(a0 + i, a1 & 0xff); R[2] = a0; break; }
        case 0x2d: { name = "memcmp"; int r = 0; for (uint32_t i = 0; i < R[6]; i++) { int x = (int)rd8(a0 + i), y = (int)rd8(a1 + i); if (x != y) { r = x - y; break; } } R[2] = (uint32_t)r; break; }
        case 0x2e: { name = "memchr"; R[2] = 0; for (uint32_t i = 0; i < R[6]; i++) if (rd8(a0 + i) == (a1 & 0xff)) { R[2] = a0 + i; break; } break; }
        case 0x2f: { name = "rand"; static uint32_t seed = 1; (void)seed; extern uint32_t g_rand; g_rand = g_rand * 0x41c64e6du + 0x3039u; R[2] = (g_rand >> 16) & 0x7fff; break; }
        case 0x30: { name = "srand"; extern uint32_t g_rand; g_rand = a0; break; }
        case 0x43: { name = "Exec"; uint32_t pc0 = rd32(a0), gp0 = rd32(a0 + 4), sp0 = rd32(a0 + 0x20) ? rd32(a0 + 0x20) + rd32(a0 + 0x24) : 0;
            { extern void hw_exec_reset(void); hw_exec_reset(); } fprintf(stderr, "[exec] a0=%08x a1=%08x a2=%08x ra=%08x pc=%08x gp=%08x sp=%08x\n", a0, a1, R[6], R[31], pc0, gp0, sp0); if (getenv("DBGEXEC")) dump_ring();
            uint32_t bs = rd32(a0 + 0x18), bl = rd32(a0 + 0x1c); /* bss addr/size */
            for (uint32_t i = 0; i < bl; i++) wr8(bs + i, 0);
            ov_valid = 0; R[28] = gp0; if (sp0) R[29] = sp0; else R[29] = 0x801fff00; R[31] = 0; call_addr(pc0); exit(0); }
        case 0x44: name = "FlushCache"; break;
        case 0x45: name = "init_a0_table"; break;
        case 0x57: name = "GetA0Table?"; R[2] = 0x200; break;
        case 0x3f: { name = "printf"; if (getenv("DBGPRINTF")) { extern uint32_t g_frames; char b[200]; int k = 0; while (k < 199 && rd8(a0 + k)) { b[k] = rd8(a0 + k); k++; } b[k] = 0; { char s1[40] = "", s2[40] = "", s3[40] = "", s4[40] = ""; uint32_t ar[4] = { a1, R[6], R[7], 0 }; char* sp[4] = { s1, s2, s3, s4 }; for (int q = 0; q < 3; q++) { int z = 0; if (ar[q] >= 0x80000000u && ar[q] < 0x80200000u) while (z < 39 && rd8(ar[q] + z) >= 32 && rd8(ar[q] + z) < 127) { sp[q][z] = rd8(ar[q] + z); z++; } sp[q][z] = 0; } fprintf(stderr, "[printf] quadro %u: %s :: [%s] [%s] [%s]\n", g_frames, b, s1, s2, s3); } } break; }
        default: break;
    } else if (tbl == 0xb0) switch (fn) {
        case 0x08: { name = "OpenEvent"; if (a0 == 0xf4000001u || a0 == 0xf0000011u) fprintf(stderr, "[card] OpenEvent cls=%08x spec=%x modo=%x func=%08x\n", a0, a1, R[6], R[7]); R[2] = next_event++; int i = R[2] & 63; kev[i].cls = a0; kev[i].spec = a1; kev[i].mode = R[6]; kev[i].func = R[7]; kev[i].used = 1; kev[i].delivered = 0; if (g_verbose || getenv("BIOSLOG")) fprintf(stderr, "[ev] OpenEvent cls=%08x spec=%x modo=%x func=%08x -> %08x\n", a0, a1, R[6], R[7], R[2]); break; }
        case 0x07: name = "DeliverEvent"; kev_deliver(a0, a1); break;
        case 0x20: { name = "UnDeliverEvent"; for (int i = 0; i < 64; i++) if (kev[i].used && kev[i].cls == a0 && kev[i].spec == a1) kev[i].delivered = 0; break; }
        case 0x4e: { name = "_card_write"; mc_load(); uint32_t s = a1 & 0x3ff; if (!(a0 & 0x10) && s < 1024) { for (int k = 0; k < 128; k++) mc[s * 128 + k] = rd8(R[6] + k); mc_save(); card_ev(0x0004); R[2] = 1; } else card_ev(0x0100); break; }
        case 0x4f: { name = "_card_read"; mc_load(); uint32_t s = a1 & 0x3ff; if (!(a0 & 0x10) && s < 1024) { for (int k = 0; k < 128; k++) wr8(R[6] + k, mc[s * 128 + k]); card_ev(0x0004); R[2] = 1; } else card_ev(0x0100); break; }
        case 0x32: { name = "open"; R[2] = (uint32_t)mc_open(a0, a1); kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x33: { name = "lseek"; int fd = (int)a0; if (fd >= 0 && fd < 16 && mcfd[fd].used) { int w = (int)R[6], np = (int)a1; if (w == 1) np += mcfd[fd].pos; else if (w == 2) np += mcfd[fd].nblk * 8192; mcfd[fd].pos = np < 0 ? 0 : np; R[2] = mcfd[fd].pos; } else R[2] = (uint32_t)-1; break; }
        case 0x34: { name = "read"; R[2] = (uint32_t)mc_rw((int)a0, a1, (int)R[6], 0); kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x35: { name = "write"; if (a0 == 1 || a0 == 2) { if (getenv("TTY")) { if (getenv("TTYF")) fprintf(stderr, "[%u] ", g_frames); for (uint32_t k = 0; k < R[6]; k++) fputc(rd8(a1 + k), stderr); } R[2] = R[6]; break; } R[2] = (uint32_t)mc_rw((int)a0, a1, (int)R[6], 1); kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x36: { name = "close"; if (a0 < 16) mcfd[a0].used = 0; R[2] = a0; kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x41: { name = "format"; mc_load(); for (int i = 1; i < 16; i++) { uint8_t* e = mc + i * 128; memset(e, 0, 128); e[0] = 0xa0; e[8] = e[9] = 0xff; mc_sum(i); } memset(mc + 8192, 0, 15 * 8192); mc_dirty = 1; R[2] = 1; kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x42: { name = "firstfile"; mc_load(); char p[48]; mc_name(a0, p, sizeof p); if (strncmp(p, "bu00", 4)) { R[2] = 0; break; }
            snprintf(mc_scan_pat, sizeof mc_scan_pat, "%s", mc_path(p)); int sl = mc_find_first(mc_scan_pat, 1); mc_scan_slot = sl;
            if (sl) { mc_fill_dirent(a1, sl); R[2] = a1; } else R[2] = 0; kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x43: { name = "nextfile"; int sl = mc_scan_slot ? mc_find_first(mc_scan_pat, mc_scan_slot + 1) : 0; mc_scan_slot = sl;
            if (sl) { mc_fill_dirent(a0, sl); R[2] = a0; } else R[2] = 0; kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x45: { name = "erase"; mc_load(); char p[48]; mc_name(a0, p, sizeof p); int sl = mc_find_first(mc_path(p), 1);
            if (sl) { int ch[15]; int n = mc_chain(sl, ch); for (int k = 0; k < n; k++) { uint8_t* e = mc + ch[k] * 128; e[0] = 0xa0 | 0; e[8] = e[9] = 0xff; mc_sum(ch[k]); } mc_dirty = 1; R[2] = 1; } else R[2] = 0; kev_deliver(0xf0000011u, 0x0004); kev_deliver(0xf4000001u, 0x0004); break; }
        case 0x0c: name = "EnableEvent"; R[2] = 1; break;
        case 0x17: name = "ReturnFromException"; { extern void isr_exit(void); isr_exit(); } break;
        case 0x19: name = "SetCustomExitFromException"; irq_handler_struct = a0; break;
        case 0x0d: name = "DisableEvent"; R[2] = 1; break;
        case 0x0a: { name = "WaitEvent"; KEv* e = &kev[a0 & 63]; if (e->used && card_class(e->cls)) e->delivered = 0; R[2] = 1; break; }
        case 0x0b: { name = "TestEvent"; KEv* e = &kev[a0 & 63]; if (e->used && card_class(e->cls)) { R[2] = e->delivered; e->delivered = 0; } else R[2] = 1; break; }
        case 0x12: name = "InitPad"; pad_buf[0] = R[4]; pad_buf[1] = R[6]; R[2] = 1; break;
        case 0x13: name = "StartPad"; break;
        case 0x56: name = "GetC0Table"; R[2] = 0x674; break;
        case 0x57: name = "GetB0Table"; R[2] = 0x874; break;
        case 0x51: { name = "Krom2RawAdd"; uint32_t c = a0 & 0xffff, ch = '?';
            if (c >= 0x824f && c <= 0x8258) ch = '0' + (c - 0x824f); else if (c >= 0x8260 && c <= 0x8279) ch = 'A' + (c - 0x8260); else if (c >= 0x8281 && c <= 0x829a) ch = 'a' + (c - 0x8281);
            else if (c < 0x80) ch = c ? c : ' ';
            else switch (c) { case 0x8140: ch = ' '; break; case 0x8143: ch = ','; break; case 0x8144: ch = '.'; break; case 0x8146: ch = ':'; break; case 0x8147: ch = ';'; break; case 0x8148: ch = '?'; break; case 0x8149: ch = '!'; break; case 0x815e: ch = '/'; break; case 0x817b: ch = '+'; break; case 0x817c: ch = '-'; break; case 0x8193: ch = '%'; break; case 0x8169: ch = '('; break; case 0x816a: ch = ')'; break; case 0x8195: ch = '&'; break; case 0x8163: ch = '.'; break; case 0x8181: ch = '='; break; case 0x819b: case 0x819c: ch = 'O'; break; case 0x817e: ch = 'X'; break; case 0x81a2: case 0x81a3: ch = '^'; break; case 0x81a0: case 0x819f: ch = '#'; break; }
            if (ch < 0x20 || ch > 0x7e) ch = '?';
            uint32_t dst = 0x80008000u + ((ch - 0x20) * 32u); for (int k = 0; k < 30; k++) wr8(dst + k, krom_font[ch - 0x20][k]); R[2] = dst; break; }
        case 0x4a: name = "InitCARD"; break;
        case 0x4b: name = "StartCARD"; break;
        default: break;
    } else if (tbl == 0xc0) switch (fn) {
        case 0x0a: name = "ChangeClearRCnt"; break;
        case 0x02: { name = "SysEnqIntRP"; extern uint32_t g_enq[8]; extern int g_nenq; if (a1 && g_nenq < 8) { int dup = 0; for (int i = 0; i < g_nenq; i++) if (g_enq[i] == a1) dup = 1; if (!dup) g_enq[g_nenq++] = a1; } R[2] = 0; break; }
        case 0x03: { name = "SysDeqIntRP"; extern uint32_t g_enq[8]; extern int g_nenq; for (int i = 0; i < g_nenq; i++) if (g_enq[i] == a1) { g_enq[i] = g_enq[--g_nenq]; break; } R[2] = 0; break; }
        default: break;
    }
    { static int bl = -1; if (bl < 0) bl = getenv("BIOSLOG") != NULL; if (g_verbose || bl) fprintf(stderr, "[bios] %02x:%02x %s(%08x, %08x)\n", tbl & 0xff, fn, name, a0, a1); }
}

void overlay_invalidate(void) { ov_valid = 0; }
static uint64_t fnv_ram(uint32_t base, uint32_t size) {
    uint64_t h = 0xcbf29ce484222325ULL; uint8_t* p = ptr(base);
    for (uint32_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}
static void (*overlay_find(uint32_t k))(void) {
    if (!(ov_valid && ov_active >= 0)) {
        ov_active = -1;
        for (int i = 0; i < overlays_n; i++) {
            if (k < overlays[i].base || k >= overlays[i].base + overlays[i].size) continue;
            if (fnv_ram(overlays[i].base, overlays[i].size) == overlays[i].hash) { ov_active = i; ov_valid = 1; { extern uint32_t g_frames, g_mdec_frames; static double t0; if (!t0) t0 = now_s(); fprintf(stderr, "[overlay] %s ativo (quadro %u, %.1f s, %u quadros de video)\n", overlays[i].name, g_frames, now_s() - t0, g_mdec_frames); } break; }
        }
        if (ov_active < 0) return NULL;
    }
    const Overlay* o = &overlays[ov_active];
    int lo = 0, hi = o->n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (o->tab[m].addr == k) return o->tab[m].fn; if (o->tab[m].addr < k) lo = m + 1; else hi = m - 1; }
    return NULL;
}
void call_addr(uint32_t a) {
    if (a == 0xa0 || a == 0xb0 || a == 0xc0) { bios_call(a, R[9]); return; }
    if ((a & 0xffff0000u) == 0xbfc00000u) { bios_call((a >> 8) & 0xff, a & 0xff); return; }
    uint32_t k = a; if (k < 0x200000) k |= 0x80000000;
    int lo = 0, hi = func_table_len - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (func_table[m].addr == k) { func_table[m].fn(); return; }
        if (func_table[m].addr < k) lo = m + 1; else hi = m - 1;
    }
    if (k >= 0x80010000 && k < 0x800d2800) { void (*f)(void) = overlay_find(k); if (f) { f(); return; } }
    if (getenv("DUMP_OV")) { FILE* d = fopen(getenv("DUMP_OV"), "wb"); fwrite(ptr(0x8003dfa0), 1, 0x2000, d); fclose(d); }
    fprintf(stderr, "[nao traduzido] chamada para %08x ra=%08x t1=%08x (overlay ou ponteiro de BIOS?)\n", a, R[31], R[9]); dump_ring(); exit(8);
}

/* ---- diagnostico: coracao (a cada 5 s) e deteccao de travamento, gravados no log (--log ARQ) ---- */
#ifdef _WIN32
#define WD_SLEEP(ms) Sleep(ms)
#else
#include <pthread.h>
#define WD_SLEEP(ms) usleep((ms) * 1000)
#endif
static void* wd_main(void* arg) {
    (void)arg; uint32_t last = 0; int still = 0, t = 0;
    for (;;) {
        WD_SLEEP(1000); t++;
        uint32_t f = g_frames;
        if (f == last) { if (++still == 6 || (still > 6 && still % 30 == 0)) { fprintf(stderr, "\n[travou] sem quadros ha %d s (quadro %u, chamadas %llu, ultimo laco %08x)\n", still, f, (unsigned long long)g_calls, g_looppc); { extern void hw_dump(void); hw_dump(); } dump_ring(); fflush(stderr); } }
        else still = 0;
        if (t % 5 == 0) { fprintf(stderr, "[hb] %ds quadro=%u chamadas=%llu\n", t, f, (unsigned long long)g_calls); fflush(stderr); }
        last = f;
    }
    return 0;
}
#ifdef _WIN32
static DWORD WINAPI wd_win(LPVOID a) { wd_main(a); return 0; }
#endif
static void wd_start(void) {
#ifdef _WIN32
    CreateThread(NULL, 0, wd_win, NULL, 0, NULL);
#else
    pthread_t th; if (!pthread_create(&th, NULL, wd_main, NULL)) pthread_detach(th);
#endif
}
extern void func_entry_dummy(void);
int main(int argc, char** argv) {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    if (argc < 2) { fprintf(stderr, "uso: %s SLUS_004.93 [-v]\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror("exe"); return 1; }
    uint8_t h[0x800]; if (fread(h, 1, 0x800, f) != 0x800) return 1;
    uint32_t pc, gp, addr, size; memcpy(&pc, h + 0x10, 4); memcpy(&gp, h + 0x14, 4); memcpy(&addr, h + 0x18, 4); memcpy(&size, h + 0x1c, 4);
    if (fread(ram + (addr & 0x1fffffff), 1, size, f) != size) return 1; fclose(f);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-cd")) hw_verbose = 1;
        else if (!strcmp(argv[i], "-cdd")) hw_verbose = 2;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) { if (g_nshots < 32) g_shots[g_nshots++] = atoi(argv[++i]); else ++i; }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) add_press(argv[++i]);
        else if (!strcmp(argv[i], "--cd-turbo") && i + 1 < argc) hw_set_speed(atof(argv[++i]));
        else if (!strcmp(argv[i], "--pular-video")) g_skipvid = 1;
        else if (!strcmp(argv[i], "--video")) g_skipvid = 0;
        else if (!strcmp(argv[i], "--janela")) g_win = 1;
        else if (!strcmp(argv[i], "--estado") && i + 1 < argc) { char *a = argv[++i]; force_state = strtoul(a, &a, 16); if (*a == '@') force_at = atoi(a + 1); }
        else if (!strcmp(argv[i], "--start-at") && i + 1 < argc) pad_start_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_maxf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bin") && i + 1 < argc) hw_set_bin(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) { if (freopen(argv[++i], "w", stderr)) { setvbuf(stderr, NULL, _IOLBF, 0); wd_start(); } }
    }
    overlay_init(); init_kernel_tables();
    R[28] = gp; R[29] = 0x801fff00; R[31] = 0;
    if (g_win) { if (!video_init()) { fprintf(stderr, "janela indisponivel (compile com SDL2: veja build.sh)\n"); return 1; } putenv("ALARM=0"); }
atexit(mc_flush);
#ifndef _WIN32
    signal(SIGALRM, on_alarm); alarm(getenv("ALARM") ? atoi(getenv("ALARM")) : 30);
#else
    (void)on_alarm;   /* Windows nao tem SIGALRM/alarm: sem watchdog */
#endif
    fprintf(stderr, "[inicio] entry=%08x\n", pc);
    if (getenv("TEST_SPRINTF")) {   /* teste: sprintf(buf, "%s-%02u%X", buf, 0, 0) com buf = "bu00:BASLUS-00493" */
        const char* nm = "bu00:BASLUS-00493"; const char* fm = "%s-%02u%X"; uint32_t b = 0x80100000;
        for (int i = 0; i <= 17; i++) wr8(b + i, nm[i]); for (int i = 0; i <= 9; i++) wr8(0x80100100 + i, fm[i]);
        R[29] = 0x801f0000; R[4] = b; R[5] = 0x80100100; R[6] = b; R[7] = 0; wr32(R[29] + 16, 0); R[31] = 0;
        call_addr(0x8002a9a8);
        char o[64]; for (int i = 0; i < 63; i++) o[i] = (char)rd8(b + i); o[63] = 0; fprintf(stderr, "[teste sprintf] resultado=\"%s\"\n", o); return 0;
    }
    call_addr(pc);
    fprintf(stderr, "[fim] entry retornou. chamadas=%llu\n", (unsigned long long)g_calls);
    return 0;
}
