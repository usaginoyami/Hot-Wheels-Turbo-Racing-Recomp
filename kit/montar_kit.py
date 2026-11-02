#!/usr/bin/env python3
"""Monta o pacote de 1 clique (zip) a partir do repositorio + ferramentas baixadas.
Uso: python3 montar_kit.py SAIDA_DIR PY.tar.gz ZIG.whl SDL2-devel-mingw.tar.gz [--linux]
 --linux: monta o pacote de Linux (PY/ZIG de Linux; SDL2 so fornece os cabecalhos, a lib vem do sistema)
 - PY_WIN: python-build-standalone (windows msvc install_only .tar.gz)
 - ZIGWIN: wheel ziglang para win_amd64 (pip download ziglang --platform win_amd64 --only-binary=:all: --no-deps)
 - SDL2-devel: SDL2-devel-2.30.x-mingw.tar.gz (libsdl.org / GitHub releases)
So ferramentas e codigo do projeto entram no pacote; nada do jogo."""
import os, sys, shutil, tarfile, zipfile, glob
LINUX = '--linux' in sys.argv
saida, pytgz, zigwhl, sdltgz = [a for a in sys.argv[1:] if a != '--linux'][:4]
aqui = os.path.dirname(os.path.abspath(__file__)); repo = os.path.dirname(aqui)
shutil.rmtree(saida, ignore_errors=True); os.makedirs(saida)
fonte = os.path.join(saida, 'fonte'); os.makedirs(fonte)
for pat in ('*.c', '*.h', '*.py'):
    for f in glob.glob(os.path.join(repo, pat)):
        shutil.copy(f, fonte)
for f in ('jogar.py', 'iso.py') + (('Jogar.sh',) if LINUX else ('Jogar.bat',)):
    shutil.copy(os.path.join(aqui, f), saida)
shutil.copy(os.path.join(aqui, 'LEIA-ME-LINUX.txt' if LINUX else 'LEIA-ME.txt'), os.path.join(saida, 'LEIA-ME.txt'))
ferr = os.path.join(saida, 'ferramentas'); os.makedirs(ferr)
# python
tmp = os.path.join(saida, '_tmp'); os.makedirs(tmp)
tarfile.open(pytgz).extractall(tmp)
py = os.path.join(ferr, 'python'); shutil.move(os.path.join(tmp, 'python'), py)
for pat in ('**/*.pdb', '**/__pycache__', 'Lib/test', 'Lib/idlelib', 'Lib/ensurepip', 'Lib/site-packages', 'Lib/turtledemo', 'tcl/tcl8/*/*.tm', 'include', 'libs', 'Scripts', 'lib/python3.12/test', 'lib/python3.12/idlelib', 'lib/python3.12/ensurepip', 'lib/python3.12/site-packages', 'lib/python3.12/turtledemo', 'lib/*.a', 'lib/python3.12/config-3.12-x86_64-linux-gnu', 'share'):
    for p in glob.glob(os.path.join(py, pat), recursive=True):
        shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
if LINUX and shutil.which('strip'):   # libpython vem com simbolos de depuracao (~200 MB)
    import subprocess
    for so in glob.glob(os.path.join(py, 'lib', 'libpython*.so*')) + glob.glob(os.path.join(py, 'lib', 'python3.12', 'lib-dynload', '*.so')):
        subprocess.run(['strip', '--strip-unneeded', so], check=False)
# zig (somente o necessario para x86_64-windows-gnu)
zipfile.ZipFile(zigwhl).extractall(tmp)
zg = os.path.join(ferr, 'zig'); shutil.move(os.path.join(tmp, 'ziglang'), zg)
if LINUX: os.chmod(os.path.join(zg, 'zig'), 0o755)
lib = os.path.join(zg, 'lib')
for d in ('libcxx', 'libcxxabi', 'libtsan', 'libunwind', 'libfuzzer', 'docs', 'build-web', 'init'):
    shutil.rmtree(os.path.join(lib, d), ignore_errors=True)
for d in (('darwin', 'freebsd', 'mingw', 'musl', 'netbsd', 'openbsd', 'wasi') if LINUX else ('darwin', 'freebsd', 'glibc', 'musl', 'netbsd', 'openbsd', 'wasi')):
    shutil.rmtree(os.path.join(lib, 'libc', d), ignore_errors=True)
inc = os.path.join(lib, 'libc', 'include')
for d in os.listdir(inc):
    if d not in (('any-linux-any', 'generic-glibc', 'x86_64-linux-any', 'x86_64-linux-gnu', 'x86-linux-any', 'x86-linux-gnu') if LINUX else ('any-windows-any',)):
        shutil.rmtree(os.path.join(inc, d), ignore_errors=True)
# SDL2
tarfile.open(sdltgz).extractall(tmp)
sd = glob.glob(os.path.join(tmp, 'SDL2-*', 'x86_64-w64-mingw32'))[0]
dst = os.path.join(ferr, 'SDL2', 'x86_64-w64-mingw32'); os.makedirs(os.path.dirname(dst))
shutil.copytree(sd, dst, ignore=shutil.ignore_patterns(*(('share', 'sdl2-config*', 'cmake', 'pkgconfig', 'lib', 'bin') if LINUX else ('share', 'sdl2-config*', 'cmake', 'pkgconfig'))))
shutil.rmtree(tmp, ignore_errors=True)
print('kit montado em', saida)
