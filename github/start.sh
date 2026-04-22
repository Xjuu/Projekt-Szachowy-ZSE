#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$SCRIPT_DIR"

if [[ ! -f "Makefile" || ! -f "src/main.c" || ! -f "src/clock.c" || ! -f "src/ui.c" ]]; then
	echo "Blad: brakuje plikow projektu w katalogu: $SCRIPT_DIR"
	echo "Wymagane: Makefile oraz src/main.c, src/clock.c, src/ui.c"
	echo "Uruchom skrypt z katalogu glownego projektu albo skopiuj caly projekt na Raspberry Pi."
	exit 1
fi

echo "[1/2] Budowanie projektu..."
make

echo "[2/2] Uruchamianie zegara..."
exec ./build/chess_clock "$@"