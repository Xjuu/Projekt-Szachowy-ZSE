CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 \
            $(shell sdl2-config --cflags) \
            -I chess-library/include

LDFLAGS  := $(shell sdl2-config --libs) \
            -lSDL2_ttf \
            -lbluetooth \
            -lqrencode \
            -lpthread

OUT      := chess_pi

.PHONY: all clean server

all: $(OUT)

$(OUT): main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Build the Go relay server as well
server:
	go build -o chess_server server.go

clean:
	rm -f $(OUT) chess_server
