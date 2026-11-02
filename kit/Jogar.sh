#!/bin/sh
cd "$(dirname "$0")"
chmod +x ferramentas/python/bin/python3* ferramentas/zig/zig 2>/dev/null
./ferramentas/python/bin/python3 jogar.py "$@" || { echo; echo "O programa terminou com erro. Veja a mensagem acima."; read -r _ ; }
