CC := gcc
CFLAGS := -Wall -Wextra -O2 $(shell sdl2-config --cflags)
LDFLAGS := $(shell sdl2-config --libs) -lSDL2_ttf

SRC := src/main.c src/clock.c src/ui.c
OUT := build/chess_clock

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

clean:
	rm -rf build