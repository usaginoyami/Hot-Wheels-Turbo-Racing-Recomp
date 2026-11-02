import os
#!/usr/bin/env python3
"""Recompilador estatico MIPS R3000 (PS1) -> C.  Uso: recomp.py EXE saida.c [raiz_extra ...]"""
import struct, sys, collections

def sx16(v): return v - 0x10000 if v & 0x8000 else v

PFX = ''
class Exe:
    def __init__(s, path, raw_base=None):
        raw = open(path, 'rb').read()
        if raw_base is not None:
            s.pc = raw_base; s.gp = 0; s.addr = raw_base; s.size = len(raw) & ~3; s.sp = 0; s.data = raw[:s.size]; s.end = s.addr + s.size
            return
        assert raw[:8] == b'PS-X EXE'
        s.pc, s.gp, s.addr, s.size, = struct.unpack('<IIII', raw[0x10:0x20])
        s.sp = struct.unpack('<I', raw[0x30:0x34])[0]
        s.data = raw[0x800:0x800 + s.size]
        s.end = s.addr + s.size
    def has(s, a): return s.addr <= a < s.end and a % 4 == 0
    def w(s, a): o = a - s.addr; return struct.unpack('<I', s.data[o:o+4])[0]

# ---- decodificacao ----
def decode(w):
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
    sh = (w >> 6) & 31; fn = w & 63; imm = w & 0xffff; tgt = w & 0x3ffffff
    return op, rs, rt, rd, sh, fn, imm, tgt

SPECIAL_OK = {0,2,3,4,6,7,8,9,12,13,16,17,18,19,24,25,26,27,32,33,34,35,36,37,38,39,42,43}
def valid(w):
    if w == 0: return True
    op, rs, rt, rd, sh, fn, imm, tgt = decode(w)
    if op == 0: return fn in SPECIAL_OK
    if op == 1: return rt in (0, 1, 16, 17)
    if op in (2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,38,40,41,42,43,46,50,58): return True
    if op == 16: return rs in (0, 4) or (w >> 25) & 1
    if op == 18: return True
    return False

def is_branch(w):
    op, rs, rt, *_ = decode(w)
    return op in (4,5,6,7) or (op == 1 and rt in (0,1,16,17))
def btarget(a, w): return (a + 4 + (sx16(w & 0xffff) << 2)) & 0xffffffff
def jtarget(a, w): return (((a + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2))

class Func:
    def __init__(s, start): s.start = start; s.insns = set(); s.labels = set(); s.calls = set(); s.tails = set(); s.jr_other = False; s.fall = None; s.cases = set()

def analyze(exe, roots):
    funcs = {}
    work = list(roots)
    seen_roots = set()
    while work:
        r = work.pop()
        if r in seen_roots or not exe.has(r): continue
        seen_roots.add(r)
        f = Func(r); funcs[r] = f
        extra = [r]
        done_cases = set()
        while True:
            q = list(extra); extra = []
            while q:
                a = q.pop()
                while True:
                    if a in f.insns or not exe.has(a): break
                    if a != r and a in seen_roots and a != r and a in funcs and False: break
                    w = exe.w(a)
                    if not valid(w): break
                    f.insns.add(a)
                    op, rs, rt, rd, sh, fn, imm, tgt = decode(w)
                    if is_branch(w):
                        d = a + 4
                        if exe.has(d): f.insns.add(d)
                        t = btarget(a, w); f.labels.add(t); q.append(t)
                        if op == 1 and rt in (16, 17): f.calls.add(t)
                        a += 8; continue
                    if op == 2:   # j
                        d = a + 4
                        if exe.has(d): f.insns.add(d)
                        t = jtarget(a, w); f.labels.add(t); q.append(t); break
                    if op == 3:   # jal
                        d = a + 4
                        if exe.has(d): f.insns.add(d)
                        f.calls.add(jtarget(a, w)); a += 8; continue
                    if op == 0 and fn == 8:   # jr
                        d = a + 4
                        if exe.has(d): f.insns.add(d)
                        if rs != 31: f.jr_other = True
                        break
                    if op == 0 and fn == 9:   # jalr
                        d = a + 4
                        if exe.has(d): f.insns.add(d)
                        a += 8; continue
                    a += 4
            if not f.jr_other: break
            # tabelas de salto: palavras de dados que apontam para dentro do corpo da funcao
            lo = f.start; hi = max(f.insns) + 4
            added = False
            for a in range(exe.addr, exe.end, 4):
                if a in f.insns: continue
                v = exe.w(a)
                if lo <= v < hi + 0x800 and v % 4 == 0 and exe.has(v) and v not in f.cases:
                    # so aceita se for dentro da faixa [lo,hi] ou continuar logo apos
                    if valid(exe.w(v)) and v >= lo:
                        f.cases.add(v); extra.append(v); f.labels.add(v); added = True
            if not added: break
        # removido: fallthrough entre funcoes tratado na emissao
        work.extend(f.calls)
    return funcs

# ---- emissao de C ----
R = lambda n: 'R[%d]' % n if n else '0u'
def gen_insn(a, w, exe, funcs, cur):
    op, rs, rt, rd, sh, fn, imm, tgt = decode(w)
    si = sx16(imm); ui = imm
    def dst(n, expr): return '' if n == 0 else 'R[%d] = %s;' % (n, expr)
    A = lambda: '(%s + %du)' % (R(rs), si & 0xffffffff) if si >= 0 else '(%s - %du)' % (R(rs), -si)
    if w == 0: return '/* nop */'
    if op == 0:
        if fn == 0: return dst(rd, '%s << %d' % (R(rt), sh))
        if fn == 2: return dst(rd, '%s >> %d' % (R(rt), sh))
        if fn == 3: return dst(rd, '(uint32_t)((int32_t)%s >> %d)' % (R(rt), sh))
        if fn == 4: return dst(rd, '%s << (%s & 31)' % (R(rt), R(rs)))
        if fn == 6: return dst(rd, '%s >> (%s & 31)' % (R(rt), R(rs)))
        if fn == 7: return dst(rd, '(uint32_t)((int32_t)%s >> (%s & 31))' % (R(rt), R(rs)))
        if fn == 12: return 'cpu_syscall(0x%x);' % a
        if fn == 13: return 'cpu_break(0x%x);' % a
        if fn == 16: return dst(rd, 'HI')
        if fn == 17: return 'HI = %s;' % R(rs)
        if fn == 18: return dst(rd, 'LO')
        if fn == 19: return 'LO = %s;' % R(rs)
        if fn == 24: return '{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); }' % (R(rs), R(rt))
        if fn == 25: return '{ uint64_t p = (uint64_t)%s * %s; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); }' % (R(rs), R(rt))
        if fn == 26: return 'cpu_div(%s, %s);' % (R(rs), R(rt))
        if fn == 27: return 'cpu_divu(%s, %s);' % (R(rs), R(rt))
        if fn in (32, 33): return dst(rd, '%s + %s' % (R(rs), R(rt)))
        if fn in (34, 35): return dst(rd, '%s - %s' % (R(rs), R(rt)))
        if fn == 36: return dst(rd, '%s & %s' % (R(rs), R(rt)))
        if fn == 37: return dst(rd, '%s | %s' % (R(rs), R(rt)))
        if fn == 38: return dst(rd, '%s ^ %s' % (R(rs), R(rt)))
        if fn == 39: return dst(rd, '~(%s | %s)' % (R(rs), R(rt)))
        if fn == 42: return dst(rd, '(int32_t)%s < (int32_t)%s' % (R(rs), R(rt)))
        if fn == 43: return dst(rd, '%s < %s' % (R(rs), R(rt)))
    if op in (8, 9): return dst(rt, '%s + %du' % (R(rs), si & 0xffffffff))
    if op == 10: return dst(rt, '(int32_t)%s < %d' % (R(rs), si))
    if op == 11: return dst(rt, '%s < %du' % (R(rs), si & 0xffffffff))
    if op == 12: return dst(rt, '%s & %du' % (R(rs), ui))
    if op == 13: return dst(rt, '%s | %du' % (R(rs), ui))
    if op == 14: return dst(rt, '%s ^ %du' % (R(rs), ui))
    if op == 15: return dst(rt, '%du' % (ui << 16))
    if op == 32: return dst(rt, '(uint32_t)(int32_t)(int8_t)rd8(%s)' % A())
    if op == 33: return dst(rt, '(uint32_t)(int32_t)(int16_t)rd16(%s)' % A())
    if op == 34: return dst(rt, 'lwl(%s, %s)' % (A(), R(rt)))
    if op == 35: return dst(rt, 'rd32(%s)' % A())
    if op == 36: return dst(rt, 'rd8(%s)' % A())
    if op == 37: return dst(rt, 'rd16(%s)' % A())
    if op == 38: return dst(rt, 'lwr(%s, %s)' % (A(), R(rt)))
    if op == 40: return 'wr8(%s, %s);' % (A(), R(rt))
    if op == 41: return 'wr16(%s, %s);' % (A(), R(rt))
    if op == 42: return 'swl(%s, %s);' % (A(), R(rt))
    if op == 43: return 'wr32(%s, %s);' % (A(), R(rt))
    if op == 46: return 'swr(%s, %s);' % (A(), R(rt))
    if op == 50: return 'gte_wr(%d, rd32(%s));' % (rt, A())          # lwc2
    if op == 58: return 'wr32(%s, gte_rd(%d));' % (A(), rt)           # swc2
    if op == 16:
        if rs == 0: return dst(rt, 'cop0_rd(%d)' % rd)
        if rs == 4: return 'cop0_wr(%d, %s);' % (rd, R(rt))
        return '/* rfe */'
    if op == 18:
        if rs == 0: return dst(rt, 'gte_rd(%d)' % rd)
        if rs == 2: return dst(rt, 'gte_ctl_rd(%d)' % rd)
        if rs == 4: return 'gte_wr(%d, %s);' % (rd, R(rt))
        if rs == 6: return 'gte_ctl_wr(%d, %s);' % (rd, R(rt))
        return 'gte_cmd(0x%08x);' % w
    return 'cpu_unknown(0x%x, 0x%08x);' % (a, w)

def label(a): return 'L_%08x' % a
def fname(a): return '%sfunc_%08x' % (PFX, a)

def emit_func(f, exe, funcs, out):
    out.append('void %s(void) {' % fname(f.start)); out.append('  if (hle_hook(0x%x)) return;' % f.start); out.append('  trace_enter(0x%x);' % f.start)
    addrs = sorted(f.insns)
    fs = f.start
    # gera cada instrucao que nao e delay slot isolado; labels em todos os alvos
    ds = set()
    for a in addrs:
        w = exe.w(a)
        op, rs, rt, rd, sh, fn, imm, tgt = decode(w)
        if is_branch(w) or op in (2, 3) or (op == 0 and fn in (8, 9)): ds.add(a + 4)
    def tgt_goto(t, cur=None):
        if t in f.insns and (t in f.labels or t in f.cases or t == fs): return ('{ LOOP_TICK(); goto %s; }' % label(t)) if (cur is not None and t <= cur and not os.environ.get('RECOMP_NOTICK')) else 'goto %s;' % label(t)
        return '{ %s(); return; }' % fname(t) if (t in funcs) else '{ call_addr(0x%x); return; }' % t
    for i, a in enumerate(addrs):
        w = exe.w(a)
        op, rs, rt, rd, sh, fn, imm, tgt = decode(w)
        if a in f.labels or a == fs or a in f.cases: out.append('%s:;' % label(a))
        if a in ds and (a - 4) in f.insns:
            # ja emitido junto do branch anterior; mas pode ser alvo de salto
            if a not in f.labels:
                continue
        is_ctl = is_branch(w) or op in (2, 3) or (op == 0 and fn in (8, 9))
        if not is_ctl:
            out.append('  %s /* %08x */' % (gen_insn(a, w, exe, funcs, f), a))
            # fallthrough para o fim da funcao / proxima funcao
            nxt = a + 4
            if nxt not in f.insns and (a not in ds):
                out.append('  { %s(); return; } /* queda para fora */' % fname(nxt) if nxt in funcs else '  cpu_fell_off(0x%x); return;' % nxt)
            continue
        dw = exe.w(a + 4) if exe.has(a + 4) else 0
        dslot = gen_insn(a + 4, dw, exe, funcs, f)
        ins = []
        if is_branch(w):
            t = btarget(a, w)
            if op == 4: cond = '%s == %s' % (R(rs), R(rt))
            elif op == 5: cond = '%s != %s' % (R(rs), R(rt))
            elif op == 6: cond = '(int32_t)%s <= 0' % R(rs)
            elif op == 7: cond = '(int32_t)%s > 0' % R(rs)
            else:
                cond = {0: '(int32_t)%s < 0', 1: '(int32_t)%s >= 0', 16: '(int32_t)%s < 0', 17: '(int32_t)%s >= 0'}[rt] % R(rs)
            link = ''
            if op == 1 and rt in (16, 17): link = 'R[31] = 0x%x;' % (a + 8)
            if op == 1 and rt in (16, 17):
                # bltzal/bgezal: raro; chama funcao se a condicao for verdadeira
                out.append('  { int c = %s; %s %s if (c) { %s(); } }' % (cond, link, dslot, fname(t)))
            else:
                out.append('  { int c = %s; %s if (c) %s } /* %08x */' % (cond, dslot, tgt_goto(t, a), a))
        elif op == 2:
            t = jtarget(a, w)
            out.append('  %s %s /* j %08x */' % (dslot, tgt_goto(t, a), t))
        elif op == 3:
            t = jtarget(a, w)
            out.append('  R[31] = 0x%x; %s %s; /* jal */' % (a + 8, dslot, (fname(t) + '()') if exe.has(t) else 'call_addr(0x%x)' % t))
        elif op == 0 and fn == 8:
            if rs == 31:
                out.append('  { %s return; } /* jr ra */' % dslot)
            else:
                cases = sorted(f.cases | set(x for x in f.labels if x != fs))
                sw = ''.join('case 0x%x: goto %s; ' % (c, label(c)) for c in sorted(f.cases))
                out.append('  { uint32_t t = %s; %s switch (t) { %s default: call_addr(t); return; } } /* jr */' % (R(rs), dslot, sw))
        elif op == 0 and fn == 9:
            out.append('  { uint32_t t = %s; %s R[%d] = 0x%x; call_addr(t); } /* jalr */' % (R(rs), dslot, rd if rd else 31, a + 8))
        # apos delay slot, fluxo continua em a+8 (ja e proxima instrucao em addrs)
        if (op != 2) and not (op == 0 and fn == 8):
            nxt = a + 8
            if nxt not in f.insns:
                out.append('  cpu_fell_off(0x%x); return;' % nxt)
    out.append('}\n')

def main():
    global PFX
    args = sys.argv[1:]
    raw_base = None
    if '--raw' in args:
        i = args.index('--raw'); raw_base = int(args[i + 1], 16); del args[i:i + 2]
    if '--prefix' in args:
        i = args.index('--prefix'); PFX = args[i + 1]; del args[i:i + 2]
    exe = Exe(args[0], raw_base); outp = args[1]
    roots = [exe.pc] + [int(x, 16) for x in args[2:]]
    # prologos de funcao (addiu sp,sp,-N) e alvos de ponteiros em dados viram raizes
    for a in range(exe.addr, exe.end, 4):
        w = exe.w(a)
        if (w >> 16) == 0x27bd and (w & 0x8000) and (w & 0xffff) >= 0xff00:
            roots.append(a)
        if exe.has(w) and (exe.w(w) >> 16) == 0x27bd:
            roots.append(w)
    # tabelas de ponteiros de funcao (>=2 ponteiros seguidos para codigo valido) -> raizes (funcoes folha sem prologo)
    CODE_LO, CODE_HI = (0x80010b90, 0x8002bd00) if raw_base is None else (exe.addr, exe.end)
    def ptr_ok(v): return CODE_LO <= v < CODE_HI and v % 4 == 0 and valid(exe.w(v)) and exe.w(v) != 0
    for a in range(exe.addr, exe.end - 8, 4):
        if ptr_ok(exe.w(a)) and ptr_ok(exe.w(a + 4)):
            roots.append(exe.w(a)); roots.append(exe.w(a + 4))
    # constantes de endereco (lui+addiu/ori) que apontam para codigo valido: callbacks e ponteiros de funcao
    n_const = 0
    for a in range(exe.addr, exe.end - 24, 4):
        w = exe.w(a)
        if (w >> 26) == 15:
            reg = (w >> 16) & 31
            for k in range(1, 5):
                v = exe.w(a + 4 * k)
                op = v >> 26
                if op in (9, 13) and ((v >> 21) & 31) == reg:
                    im = v & 0xffff
                    if op == 9 and (v & 0x8000): im -= 0x10000
                    t = ((w & 0xffff) << 16) + im
                    if ptr_ok(t): roots.append(t); n_const += 1
                    break
    funcs = analyze(exe, roots)
    out = ['#include "runtime.h"', '']
    for a in sorted(funcs): out.append('void %s(void);' % fname(a))
    out.append('')
    for a in sorted(funcs): emit_func(funcs[a], exe, funcs, out)
    out.append('const FuncEntry %sfunc_table[] = {' % PFX)
    for a in sorted(funcs): out.append('  { 0x%08x, %s },' % (a, fname(a)))
    out.append('};\nconst int %sfunc_table_len = %d;' % (PFX, len(funcs)))
    # jal para enderecos fora do exe (overlays): resolvidos em runtime
    ext = set()
    open(outp, 'w').write('\n'.join(out))
    ext = set()
    for f in funcs.values():
        for c in f.calls:
            if not exe.has(c): ext.add(c)
    open(outp + '.ext', 'w').write('\n'.join('%08x' % c for c in sorted(ext)))
    covered = set()
    for f in funcs.values(): covered |= f.insns
    print('funcoes:', len(funcs), ' instrucoes cobertas:', len(covered), ' de', exe.size // 4)
if __name__ == '__main__': main()
