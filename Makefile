# =============================================================================
#  Chess Clock Pi — Makefile dla main.cpp (zegar SDL2 + BlueZ + HTTPS)
# =============================================================================
#
# Wymagane pakiety systemowe (Debian/Ubuntu/Raspberry Pi OS):
#   sudo apt install -y build-essential pkg-config \
#                       libsdl2-dev libsdl2-ttf-dev \
#                       bluez libbluetooth-dev \
#                       libqrencode-dev \
#                       libssl-dev
#
# Budowanie:
#   make            — buduje binarkę ./chess_pi
#   make clean      — kasuje binarkę
#
# Uruchamianie:
#   ./chess_pi  [server_ip[:port]]  [bt_channel]  [clock_code]
#   przykład:   ./chess_pi 192.168.1.100:8443 1 CHS-7GRK-MVNN-V3V4
# =============================================================================

CXX      := g++

# -std=c++17        — wymagana wersja (chess-library, std::filesystem)
# -Wall -Wextra     — wszystkie ostrzeżenia
# -O2               — optymalizacja release
# sdl2-config       — zwraca prawidłowe -I/-D dla SDL2 na danym systemie
# -I chess-library/include  — biblioteka szachowa (walidacja ruchów, FEN/PGN)
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 \
            $(shell sdl2-config --cflags) \
            -I chess-library/include

# sdl2-config --libs  — linki SDL2 (zwykle -lSDL2)
# -lSDL2_ttf          — rendering czcionek
# -lbluetooth         — BlueZ RFCOMM (komunikacja z telefonem)
# -lqrencode          — generator QR (kod do parowania BT na ekranie)
# -lssl -lcrypto      — OpenSSL (HTTPS do serwera Go na :8443)
# -lpthread           — wątki (HTTP queue + BT server)
LDFLAGS  := $(shell sdl2-config --libs) \
            -lSDL2_ttf \
            -lbluetooth \
            -lqrencode \
            -lssl -lcrypto \
            -lpthread

OUT      := chess_pi
SRC      := main.cpp

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(OUT)
