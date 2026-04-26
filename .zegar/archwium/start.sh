#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$SCRIPT_DIR"

if [[ ! -f "Makefile" || ! -f "src/main.cpp" || ! -f "src/clock.cpp" || ! -f "src/ui.cpp" ]]; then
	echo "Błąd: brakuje plików projektu w katalogu: $SCRIPT_DIR"
	echo "Wymagane: Makefile oraz src/main.cpp, src/clock.cpp, src/ui.cpp"
	echo "Uruchom skrypt z katalogu głównego projektu albo skopiuj cały projekt na Raspberry Pi."
	exit 1
fi

echo "[1/2] Budowanie projektu..."
make

echo "[2/2] Uruchamianie zegara..."
exec ./build/chess_clock "$@"