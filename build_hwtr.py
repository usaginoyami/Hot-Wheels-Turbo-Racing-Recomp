#!/usr/bin/env python3
"""Hot Wheels Turbo Racing: gera C do carregador (SLUS_009.64) e do jogo (CCCPSX.EXE, tratado como overlay em 0x80010000).
uso: build_hwtr.py PASTA_COM_EXES SAIDA"""
import subprocess, sys, os, struct
src, out = sys.argv[1:3]
os.makedirs(out, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
def run(a): subprocess.run([sys.executable, os.path.join(here, 'recomp.py')] + a, check=True)
def fnv(b):
    h = 0xcbf29ce484222325
    for x in b: h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h
def setjmp_roots(data, base):
    # retorno de cada setjmp(): o tratador de interrupcao entra ali (longjmp), entao precisa ser raiz de funcao
    w = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4]); n = len(w); r = []
    sj = [base + i * 4 for i in range(n - 1) if w[i] == 0xac9f0000 and w[i + 1] == 0xac9c002c]
    for i, x in enumerate(w):
        if (x >> 26) == 3 and (((base + i * 4) & 0xf0000000) | ((x & 0x3ffffff) << 2)) in sj: r.append(base + i * 4 + 8)
    return r
def ptr_roots(data, base):
    # alvos de ponteiros de funcao: lui+addiu/ori e palavras de dados que caem logo depois de um 'jr ra' (+ delay slot)
    w = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4]); n = len(w); end = base + n * 4; r = set()
    def ok(t): return base <= t < end and t % 4 == 0 and w[(t - base) // 4 - 2] == 0x03e00008 and w[(t - base) // 4] != 0
    for i, x in enumerate(w):
        if (x >> 26) == 15:
            rt = (x >> 16) & 31; hi = (x & 0xffff) << 16
            for j in range(i + 1, min(i + 8, n)):
                y = w[j]; op = y >> 26
                if op in (9, 13) and ((y >> 21) & 31) == rt:
                    t = (hi + ((y & 0xffff) - (0x10000 if y & 0x8000 and op == 9 else 0))) & 0xffffffff
                    if ok(t): r.add(t)
                if ((y >> 16) & 31) == rt and op in (9, 13, 15, 32, 33, 35, 36, 37): break
        if ok(x): r.add(x)
    return sorted(r)
sl = open(os.path.join(src, 'SLUS_009.64'), 'rb').read(); sa, ss = struct.unpack('<2I', sl[0x18:0x20])
rs = ptr_roots(sl[0x800:0x800 + ss], sa) + setjmp_roots(sl[0x800:0x800 + ss], sa); print('raizes extras SLUS:', len(rs))
pass  # (loader agora tambem tem LOOP_TICK: espera ocupada do libcd precisa avancar o relogio virtual)
run([os.path.join(src, 'SLUS_009.64'), os.path.join(out, 'game.c')] + ['0x%x' % x for x in rs])
pass
exe = open(os.path.join(src, 'CCCPSX.EXE'), 'rb').read()
pc, gp, addr, size = struct.unpack('<4I', exe[0x10:0x20])
raw = os.path.join(out, 'CCCPSX.RAW'); open(raw, 'wb').write(exe[0x800:0x800 + size])
ro = ptr_roots(exe[0x800:0x800 + size], addr) + setjmp_roots(exe[0x800:0x800 + size], addr); print('raizes extras CCCPSX:', len(ro))
run([raw, os.path.join(out, 'ov0.c'), '--raw', '0x%x' % addr, '--prefix', 'ov0_', '0x%x' % pc] + ['0x%x' % x for x in ro])
h = fnv(exe[0x800:0x800 + size])
o = ['#include "runtime.h"', '#include "overlay.h"',
     'extern const FuncEntry ov0_func_table[]; extern const int ov0_func_table_len;',
     'Overlay overlays[] = { { "CCCPSX", 0x%x, %d, 0x%xULL, ov0_func_table, 0 } };' % (addr, size, h),
     'const int overlays_n = 1;', 'const int overlay_counts[] = {0};',
     'void overlay_init(void) { overlays[0].n = ov0_func_table_len; }']
open(os.path.join(out, 'overlays.c'), 'w').write('\n'.join(o))
print('overlay CCCPSX base=%08x size=%d pc=%08x hash=%x' % (addr, size, pc, h))
