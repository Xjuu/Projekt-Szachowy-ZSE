# =============================================================================
#  Chess Clock Pi — Makefile
# =============================================================================
#  Zależności systemu (Debian/Raspbian):
#    sudo apt install \
#        libsdl2-dev libsdl2-ttf-dev \
#        libqrencode-dev \
#        libbluetooth-dev \
#        libpthread-stubs0-dev   # zwykle już zainstalowane
#
#  chess-library musi być sklonowana do podkatalogu chess-library/:
#    git clone https://github.com/Disservin/chess-library.git
#
#  Typowe użycie:
#    make            — kompilacja (tryb release)
#    make debug      — kompilacja z -g3 -fsanitize=address,undefined
#    make clean      — usunięcie artefaktów
#    make install    — skopiowanie binarki do /usr/local/bin
#    make run        — build + uruchomienie (jako root, wymagane przez BlueZ)
# =============================================================================

# ── Nazwa pliku wykonywalnego ─────────────────────────────────────────────────
TARGET  := chess_pi

# ── Źródła ───────────────────────────────────────────────────────────────────
SRCS    := main.cpp

# ── Kompilator i standard ─────────────────────────────────────────────────────
CXX     := g++
STD     := -std=c++17

# ── Flagi wspólne ─────────────────────────────────────────────────────────────
COMMON_FLAGS := \
    $(STD) \
    -Wall \
    -Wextra \
    -Wshadow \
    -Wno-unused-parameter \
    -pthread

# ── Katalogi nagłówków ────────────────────────────────────────────────────────
# chess-library to header-only, wystarczy -I.
INCLUDES := \
    -I. \
    $(shell sdl2-config --cflags)

# ── Biblioteki ────────────────────────────────────────────────────────────────
LIBS := \
    $(shell sdl2-config --libs) \
    -lSDL2_ttf \
    -lqrencode \
    -lbluetooth \
    -lpthread

# ── Tryb release (domyślny) ───────────────────────────────────────────────────
RELEASE_FLAGS := -O2 -DNDEBUG

# ── Tryb debug ────────────────────────────────────────────────────────────────
DEBUG_FLAGS := \
    -O0 -g3 \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -DDEBUG

# ── Połączone flagi (domyślnie release) ───────────────────────────────────────
CXXFLAGS := $(COMMON_FLAGS) $(RELEASE_FLAGS) $(INCLUDES)

# ── Katalog obiektów ──────────────────────────────────────────────────────────
OBJDIR  := .build
OBJS    := $(SRCS:%.cpp=$(OBJDIR)/%.o)
DEPS    := $(OBJS:.o=.d)

# =============================================================================
#  Reguły
# =============================================================================

.PHONY: all release debug clean install uninstall run help

all: release

# ── Release ───────────────────────────────────────────────────────────────────
release: CXXFLAGS := $(COMMON_FLAGS) $(RELEASE_FLAGS) $(INCLUDES)
release: $(TARGET)
	@echo "[OK] Skompilowano: $(TARGET) (release)"

# ── Debug ─────────────────────────────────────────────────────────────────────
debug: CXXFLAGS := $(COMMON_FLAGS) $(DEBUG_FLAGS) $(INCLUDES)
debug: LIBS     := $(LIBS) -fsanitize=address,undefined
debug: $(TARGET)
	@echo "[OK] Skompilowano: $(TARGET) (debug)"

# ── Linkowanie ────────────────────────────────────────────────────────────────
$(TARGET): $(OBJS)
	@echo "[LD] $@"
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# ── Kompilacja pliku .cpp → .o ───────────────────────────────────────────────
$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(OBJDIR)
	@echo "[CC] $<"
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# ── Dołącz zależności nagłówkowe (plik .d generowany przez -MMD) ──────────────
-include $(DEPS)

# ── Czyszczenie ───────────────────────────────────────────────────────────────
clean:
	@echo "[RM] $(TARGET)  $(OBJDIR)/"
	rm -f $(TARGET)
	rm -rf $(OBJDIR)

# ── Instalacja ────────────────────────────────────────────────────────────────
PREFIX  ?= /usr/local
install: release
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@echo "[OK] Zainstalowano w $(DESTDIR)$(PREFIX)/bin/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@echo "[OK] Usunięto $(DESTDIR)$(PREFIX)/bin/$(TARGET)"

# ── Uruchomienie (wymagane root przez BlueZ RFCOMM) ───────────────────────────
run: release
	sudo ./$(TARGET)

# ── Pomoc ─────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  make            — kompilacja release"
	@echo "  make debug      — kompilacja z ASan/UBSan"
	@echo "  make clean      — usunięcie artefaktów"
	@echo "  make install    — kopia do $(PREFIX)/bin/"
	@echo "  make run        — build + sudo ./$(TARGET)"
	@echo ""
	@echo "  Wymagane apt-pakiety:"
	@echo "    libsdl2-dev  libsdl2-ttf-dev  libqrencode-dev  libbluetooth-dev"
	@echo "  Wymagane podkatalogi:"
	@echo "    chess-library/   (git clone https://github.com/Disservin/chess-library.git)"
	@echo ""