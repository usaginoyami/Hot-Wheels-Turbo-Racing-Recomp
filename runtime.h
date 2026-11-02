#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t addr; void (*fn)(void); } FuncEntry;
extern const FuncEntry func_table[]; extern const int func_table_len;
extern uint32_t R[32], HI, LO;
uint8_t rd8(uint32_t a); uint16_t rd16(uint32_t a); uint32_t rd32(uint32_t a);
void wr8(uint32_t a, uint32_t v); void wr16(uint32_t a, uint32_t v); void wr32(uint32_t a, uint32_t v);
uint32_t lwl(uint32_t a, uint32_t old); uint32_t lwr(uint32_t a, uint32_t old);
void swl(uint32_t a, uint32_t v); void swr(uint32_t a, uint32_t v);
void call_addr(uint32_t a);
void trace_enter(uint32_t a);
void cpu_syscall(uint32_t pc); void cpu_break(uint32_t pc); void cpu_unknown(uint32_t pc, uint32_t w);
void cpu_fell_off(uint32_t pc);
void cpu_div(uint32_t a, uint32_t b); void cpu_divu(uint32_t a, uint32_t b);
uint32_t cop0_rd(int r); void cop0_wr(int r, uint32_t v);
uint32_t gte_rd(int r); void gte_wr(int r, uint32_t v); uint32_t gte_ctl_rd(int r); void gte_ctl_wr(int r, uint32_t v); void gte_cmd(uint32_t w);
int hle_hook(uint32_t a);
/* perifericos (evita declaracao implicita, que e erro no GCC 14+) */
void xa_feed(const uint8_t* sector);
void mdec_dma_in(uint32_t madr, uint32_t words); void mdec_dma_out(uint32_t madr, uint32_t words);
void spu_dma(uint32_t madr, uint32_t words, int to_spu);
void gpu_dma(uint32_t madr, uint32_t bcr, uint32_t chcr);

extern uint32_t g_bk; void loop_tick(void);
#define LOOP_TICK() do { if (!(++g_bk & 0x3ff)) loop_tick(); } while (0)
