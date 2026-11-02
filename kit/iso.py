"""Extrator minimo de ISO9660 para imagens de PS1 (.bin 2352 / .iso 2048 / 2336). So le o necessario."""
import os, struct

def _modo(f):
    for ssz, off in ((2352, 24), (2048, 0), (2336, 8)):
        f.seek(16 * ssz + off)
        if f.read(6)[1:6] == b'CD001':
            return ssz, off
        f.seek(16 * ssz + off - 0)
    return None

class Disco:
    def __init__(self, caminho):
        self.f = open(caminho, 'rb')
        m = _modo(self.f)
        if not m:
            raise ValueError('imagem nao reconhecida (esperado .bin/.iso de PS1)')
        self.ssz, self.off = m

    def setor(self, lba, n=1):
        out = bytearray()
        for i in range(n):
            self.f.seek((lba + i) * self.ssz + self.off)
            out += self.f.read(2048)
        return bytes(out)

    def _lista(self, lba, tam):
        dados = self.setor(lba, (tam + 2047) // 2048)[:tam]
        p = 0
        while p < len(dados):
            n = dados[p]
            if n == 0:
                p = (p // 2048 + 1) * 2048
                continue
            reg = dados[p:p + n]
            ext, = struct.unpack('<I', reg[2:6]); tamf, = struct.unpack('<I', reg[10:14])
            flags = reg[25]; nome = reg[33:33 + reg[32]].decode('latin1')
            p += n
            if nome in ('\x00', '\x01'):
                continue
            yield nome.split(';')[0], ext, tamf, bool(flags & 2)

    def arquivos(self, lba=None, tam=None, prefixo=''):
        if lba is None:
            pvd = self.setor(16)
            lba, = struct.unpack('<I', pvd[158:162]); tam, = struct.unpack('<I', pvd[166:170])
        for nome, ext, tamf, dir_ in self._lista(lba, tam):
            if dir_:
                yield from self.arquivos(ext, tamf, prefixo + nome + '/')
            else:
                yield prefixo + nome, ext, tamf

    def ler(self, ext, tam):
        return self.setor(ext, (tam + 2047) // 2048)[:tam]

def abrir_imagem(caminho):
    """Aceita .cue (acha o .bin) ou .bin/.iso direto. Devolve (caminho_bin, Disco)."""
    if caminho.lower().endswith('.cue'):
        base = os.path.dirname(caminho)
        for linha in open(caminho, encoding='latin1'):
            l = linha.strip()
            if l.upper().startswith('FILE'):
                nome = l.split('"')[1] if '"' in l else l.split()[1]
                caminho = os.path.join(base, nome)
                break
    return caminho, Disco(caminho)
