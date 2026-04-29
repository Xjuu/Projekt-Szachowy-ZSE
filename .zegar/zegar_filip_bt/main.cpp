// =============================================================================
//  Chess Clock Pi — Bluetooth <-> Go server middleman
// =============================================================================
//  This program runs on a Raspberry Pi (or any Linux box with Bluetooth + SDL2).
//  It combines the chess clock that used to live in github/src/*.c (ported to
//  C++), a Bluetooth RFCOMM server that receives moves from a paired phone, an
//  on-screen QR code shown at startup that tells the phone how to connect, and
//  the HTTP relay to server.go that was already in the original main.cpp.
//
//  Build:   make
//  Run:     ./chess_pi  [server_ip[:port]]  [bt_channel]  [clock_code]  [api_key]
//
//  Auth:  clock_code = CHS-XXXX-XXXX-XXXX  (from `npm run generate`)
//         api_key    = sk_...               (private key — required for move/newgame)
//
//  Bluetooth protocol (plain text, CRLF or LF separated):
//      HELLO
//      NEWGAME|WhiteName|BlackName|minutes|increment_seconds
//      MOVE|<uci>                         legacy form, e.g. MOVE|e2e4
//      MOVE|<seq>|<uci>                   preferred, e.g. MOVE|17|e7e8q  (dedup by seq)
//      PAUSE | RESUME | RESET
//      ARBITER_STOP | ARBITER_RESUME
//      ERROR|white|black                  records an arbiter-registered error
//      BONUS|white|black|<ms>             arbiter time bonus
//      SYNC_REQUEST                       request full state replay after BT reconnect
//      ACK|<context>                      confirm receipt of GAME_OVER (or other)
//      QUIT
//
//  Replies from Pi to the phone:
//      OK|<context>               ACK a command
//      ERR|<reason>               parse / illegal-move error
//      DUP|<uci>                  duplicate move (already accepted; not an error)
//      CLOCK|<whiteMs>|<blackMs>|<active>|<state>   periodic clock snapshot
//      GAME_OVER|<winner>|<reason>          end of game (retransmitted until ACK)
//      SYNC_BEGIN
//      SYNC_GAME|<white>|<black>|<minutes>|<increment>
//      SYNC_CLOCK|<whiteMs>|<blackMs>|<active>|<state>
//      SYNC_HISTORY|<count>
//      SYNC_MOVE|<index>|<uci>              … repeated count times
//      SYNC_END
// =============================================================================

// chess-library jest zewnętrzna i ma wewnętrzne -Wshadow w konstruktorach
// (parametry tak samo nazwane jak pola). Uciszamy je tylko dla tego nagłówka.
// =============================================================================
//  Chess Clock Pi — Bluetooth <-> Go server middleman
// =============================================================================
//  This program runs on a Raspberry Pi (or any Linux box with Bluetooth + SDL2).
//  It combines the chess clock that used to live in github/src/*.c (ported to
//  C++), a Bluetooth RFCOMM server that receives moves from a paired phone, an
//  on-screen QR code shown at startup that tells the phone how to connect, and
//  the HTTP relay to server.go that was already in the original main.cpp.
//
//  Build:   make
//  Run:     ./chess_pi  [server_ip[:port]]  [bt_channel]  [clock_code]  [api_key]
//
//  Auth:  clock_code = CHS-XXXX-XXXX-XXXX  (from `npm run generate`)
//         api_key    = sk_...               (private key — required for move/newgame)
//
//  Bluetooth protocol (plain text, CRLF or LF separated):
//      HELLO
//      NEWGAME|WhiteName|BlackName|minutes|increment_seconds
//      MOVE|<uci>                         legacy form, e.g. MOVE|e2e4
//      MOVE|<seq>|<uci>                   preferred, e.g. MOVE|17|e7e8q  (dedup by seq)
//      PAUSE | RESUME | RESET
//      ARBITER_STOP | ARBITER_RESUME
//      ERROR|white|black                  records an arbiter-registered error
//      BONUS|white|black|<ms>             arbiter time bonus
//      SYNC_REQUEST                       request full state replay after BT reconnect
//      ACK|<context>                      confirm receipt of GAME_OVER (or other)
//      QUIT
//
//  Replies from Pi to the phone:
//      OK|<context>               ACK a command
//      ERR|<reason>               parse / illegal-move error
//      DUP|<uci>                  duplicate move (already accepted; not an error)
//      CLOCK|<whiteMs>|<blackMs>|<active>|<state>   periodic clock snapshot
//      GAME_OVER|<winner>|<reason>          end of game (retransmitted until ACK)
//      SYNC_BEGIN
//      SYNC_GAME|<white>|<black>|<minutes>|<increment>
//      SYNC_CLOCK|<whiteMs>|<blackMs>|<active>|<state>
//      SYNC_HISTORY|<count>
//      SYNC_MOVE|<index>|<uci>              … repeated count times
//      SYNC_END
// =============================================================================

// chess-library jest zewnętrzna i ma wewnętrzne -Wshadow w konstruktorach
// (parametry tak samo nazwane jak pola). Uciszamy je tylko dla tego nagłówka.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "chess-library/include/chess.hpp"
#pragma GCC diagnostic pop

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <qrencode.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace chess;
using SteadyClock = std::chrono::steady_clock;
using Ms          = std::chrono::milliseconds;

// ─── Global configuration ────────────────────────────────────────────────────

enum {
    WINDOW_WIDTH      = 1280,
    WINDOW_HEIGHT     = 400,
    START_MINUTES     = 5,
    INCREMENT_SECONDS = 0,
    FPS               = 30,
    DEFAULT_BT_CHAN   = 1
};

static const int ACK_TIMEOUT_MS    = 1000;
static const int ACK_MAX_RETRIES   = 5;

static const int HTTP_BACKOFF_BASE_MS = 1000;
static const int HTTP_BACKOFF_MAX_MS  = 30000;
static const int HTTP_CONNECT_TIMEOUT_MS = 3000;   // F2: nieblokujący connect z timeoutem
static const int HTTP_IO_TIMEOUT_MS      = 5000;

// F5: limity ochronne
static const size_t BT_RECV_BUFFER_MAX = 64 * 1024;   // 64 KiB
static const size_t BT_LINE_MAX        = 4 * 1024;    // 4 KiB jedna linia
static const size_t BT_QUEUE_MAX       = 1024;        // wiadomości w kolejce wejściowej
static const size_t BT_SEND_QUEUE_MAX  = 256;         // wiadomości w kolejce wyjściowej
static const int    BT_PER_FRAME_MAX   = 32;          // ile komend max na klatkę
static const int    BT_SEND_TIMEOUT_MS = 1500;        // SO_SNDTIMEO klienta

// F2: persist kolejki nie częściej niż co 250 ms (debounce)
static const int    QUEUE_PERSIST_MIN_INTERVAL_MS = 250;

// F8: watchdog klatki UI
static const uint32_t FRAME_WATCHDOG_MS = 2000;

static const char* DEFAULT_QUEUE_FILE = "chess-clock-queue.dat";
static const char* DEFAULT_LOG_FILE   = "chess-clock.log";

// ─── Logger (asynchronicznie, F3) ───────────────────────────────────────────
//
// Logger zbiera wiadomości w pamięci i flushuje je w osobnym wątku co ~200 ms
// (lub przy zamknięciu). LOG_* nigdy nie blokuje wątku wywołującego na SD.

class Logger {
public:
    static Logger& instance() { static Logger l; return l; }

    void open(const std::string& path) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (file_.is_open()) file_.close();
            file_.open(path, std::ios::app);
            path_ = path;
        }
        if (!running_.exchange(true)) {
            thr_ = std::thread(&Logger::flushLoop, this);
        }
    }

    void close() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thr_.joinable()) thr_.join();
        std::lock_guard<std::mutex> g(mu_);
        if (file_.is_open()) {
            for (const auto& line : pending_) file_ << line;
            pending_.clear();
            file_.flush();
            file_.close();
        }
    }

    void log(const char* level, const std::string& msg) {
        if (!running_.load()) return;
        auto now  = std::chrono::system_clock::now();
        auto t    = std::chrono::system_clock::to_time_t(now);
        auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
        std::tm tm{};
        localtime_r(&t, &tm);
        char tbuf[32];
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
        std::ostringstream os;
        os << tbuf << '.' << std::setw(3) << std::setfill('0') << ms.count()
           << " [" << level << "] " << msg << '\n';
        {
            std::lock_guard<std::mutex> g(mu_);
            // Limit pamięci dla loggera — gdy ktoś floodu, nie eksplodujemy.
            if (pending_.size() < 4096) pending_.push_back(os.str());
        }
        cv_.notify_one();
    }

    void error(const std::string& m) { log("ERR ", m); }
    void warn (const std::string& m) { log("WARN", m); }
    void info (const std::string& m) { log("INFO", m); }

private:
    Logger() = default;
    ~Logger() { close(); }

    void flushLoop() {
        std::vector<std::string> batch;
        while (running_.load()) {
            {
                std::unique_lock<std::mutex> g(mu_);
                cv_.wait_for(g, std::chrono::milliseconds(200),
                             [&]{ return !running_.load() || !pending_.empty(); });
                if (!pending_.empty()) {
                    batch.swap(pending_);
                }
            }
            // F3-FIX: NIE trzymamy mu_ podczas zapisu na SD! Tylko flush thread
            // pisze do file_, więc dostęp jest bezpieczny bez blokady. Wcześniej
            // LOG_* z UI thread czekał na zakończenie file_.flush() (50-200ms na
            // SD card), powodując mikro-zawieszenia.
            if (!batch.empty()) {
                if (file_.is_open()) {
                    for (const auto& line : batch) file_ << line;
                    file_.flush();
                }
                batch.clear();
            }
        }
    }

    std::mutex     mu_;
    std::condition_variable cv_;
    std::ofstream  file_;
    std::string    path_;
    std::vector<std::string> pending_;
    std::atomic<bool> running_{false};
    std::thread    thr_;
};

#define LOG_ERR(msg)  Logger::instance().error(msg)
#define LOG_WARN(msg) Logger::instance().warn(msg)
#define LOG_INFO(msg) Logger::instance().info(msg)

// ─── Clock domain ────────────────────────────────────────────────────────────

enum TimeFormat { TIME_FORMAT_MM_SS, TIME_FORMAT_MM_SS_MS };

struct PlayerClock {
    uint32_t remaining_ms  = 0;
    uint32_t increment_ms  = 0;
    uint32_t total_time_ms = 0;
    uint8_t  error_count   = 0;
};

enum ActiveSide { ACTIVE_LEFT = 0, ACTIVE_RIGHT = 1 };

enum GameState {
    STATE_SETUP,
    STATE_PAUSED,
    STATE_RUNNING,
    STATE_STOPPED_BY_ARBITER,
    STATE_FINISHED_DRAW,
    STATE_FINISHED_LEFT_WIN,
    STATE_FINISHED_RIGHT_WIN
};

struct ClockState {
    PlayerClock left;
    PlayerClock right;
    ActiveSide  active     = ACTIVE_LEFT;
    GameState   state      = STATE_PAUSED;
    uint32_t    move_count = 0;
    // Powód zakończenia gry — używany do animowanego komunikatu końca gry.
    // Wartości: "timeout", "errors", "checkmate", "stalemate".
    std::string finish_reason;
};

static void format_time(uint32_t ms, char* out, size_t out_len, TimeFormat fmt) {
    uint32_t total_seconds = ms / 1000;
    uint32_t minutes       = total_seconds / 60;
    uint32_t seconds       = total_seconds % 60;
    uint32_t milliseconds  = ms % 1000;
    if (fmt == TIME_FORMAT_MM_SS) {
        std::snprintf(out, out_len, "%02u:%02u", minutes, seconds);
    } else {
        if (minutes > 0) std::snprintf(out, out_len, "%02u:%02u", minutes, seconds);
        else             std::snprintf(out, out_len, "%02u.%03u", seconds, milliseconds);
    }
}

static void init_clock(ClockState* c, uint32_t start_ms, uint32_t increment_ms) {
    c->left  = PlayerClock{start_ms, increment_ms, start_ms, 0};
    c->right = PlayerClock{start_ms, increment_ms, start_ms, 0};
    c->active        = ACTIVE_LEFT;
    c->state         = STATE_PAUSED;
    c->move_count    = 0;
    c->finish_reason.clear();
}

static void reset_clock(ClockState* c, uint32_t start_ms, uint32_t increment_ms) {
    init_clock(c, start_ms, increment_ms);
}

static void pause_resume_clock(ClockState* c) {
    if (c->state == STATE_FINISHED_DRAW      ||
        c->state == STATE_FINISHED_LEFT_WIN  ||
        c->state == STATE_FINISHED_RIGHT_WIN) return;
    if (c->state == STATE_PAUSED)  c->state = STATE_RUNNING;
    else if (c->state == STATE_RUNNING) c->state = STATE_PAUSED;
}

static void switch_side(ClockState* c, ActiveSide pressed) {
    if (c->state != STATE_RUNNING) return;
    if (pressed != c->active)      return;
    PlayerClock* a = (c->active == ACTIVE_LEFT) ? &c->left : &c->right;
    a->remaining_ms += a->increment_ms;
    c->active = (c->active == ACTIVE_LEFT) ? ACTIVE_RIGHT : ACTIVE_LEFT;
    c->move_count++;
}

static void update_clock(ClockState* c, uint32_t delta_ms) {
    if (c->state != STATE_RUNNING) return;
    // F8: zabezpieczenie przed wielkim delta po wybudzeniu/blokadzie.
    if (delta_ms > 5000) delta_ms = 5000;
    PlayerClock* a = (c->active == ACTIVE_LEFT) ? &c->left : &c->right;
    if (delta_ms >= a->remaining_ms) {
        a->remaining_ms = 0;
        c->state = (c->active == ACTIVE_LEFT) ? STATE_FINISHED_RIGHT_WIN
                                              : STATE_FINISHED_LEFT_WIN;
        c->finish_reason = "timeout";
        return;
    }
    a->remaining_ms -= delta_ms;
}

static void stop_by_arbiter(ClockState* c) {
    if (c->state == STATE_RUNNING || c->state == STATE_PAUSED)
        c->state = STATE_STOPPED_BY_ARBITER;
}

static void resume_by_arbiter(ClockState* c) {
    if (c->state == STATE_STOPPED_BY_ARBITER) c->state = STATE_PAUSED;
}

static void player_error(ClockState* c, ActiveSide who) {
    PlayerClock* pc = (who == ACTIVE_LEFT) ? &c->left : &c->right;
    pc->error_count++;
    if (pc->error_count >= 2) {
        c->state = (who == ACTIVE_LEFT) ? STATE_FINISHED_RIGHT_WIN
                                        : STATE_FINISHED_LEFT_WIN;
        c->finish_reason = "errors";
    }
}

static void add_bonus_time(ClockState* c, ActiveSide who, uint32_t bonus_ms) {
    PlayerClock* pc = (who == ACTIVE_LEFT) ? &c->left : &c->right;
    pc->remaining_ms += bonus_ms;
}

// ─── UI resources ───────────────────────────────────────────────────────────

enum UIMode { UI_MODE_QR, UI_MODE_SETUP, UI_MODE_GAME, UI_MODE_HELP, UI_MODE_ARBITER };

// F4: prosty cache tekstur. Klucz: pointer fonta + tekst + kolor (32-bit).
struct TextCacheKey {
    TTF_Font*   font;
    std::string text;
    uint32_t    color; // RGBA pakowany
    bool operator==(const TextCacheKey& o) const {
        return font == o.font && color == o.color && text == o.text;
    }
};
struct TextCacheKeyHash {
    size_t operator()(const TextCacheKey& k) const noexcept {
        size_t h = std::hash<void*>()((void*)k.font);
        h ^= std::hash<std::string>()(k.text) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>()(k.color)   + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};
struct TextCacheEntry {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    uint32_t last_used_ms = 0;
};

class TextCache {
public:
    void clear() {
        for (auto& kv : cache_) {
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        }
        cache_.clear();
    }
    SDL_Texture* get(SDL_Renderer* r, TTF_Font* f, const char* text,
                     SDL_Color col, int* w, int* h) {
        if (!text || !*text) return nullptr;
        TextCacheKey key{f, text,
            ((uint32_t)col.r << 24) | ((uint32_t)col.g << 16) |
            ((uint32_t)col.b << 8)  |  (uint32_t)col.a};
        uint32_t now = SDL_GetTicks();
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            it->second.last_used_ms = now;
            if (w) *w = it->second.w;
            if (h) *h = it->second.h;
            return it->second.tex;
        }
        SDL_Surface* s = TTF_RenderUTF8_Blended(f, text, col);
        if (!s) return nullptr;
        SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
        TextCacheEntry e;
        e.tex = t; e.w = s->w; e.h = s->h; e.last_used_ms = now;
        SDL_FreeSurface(s);
        if (!t) return nullptr;
        if (w) *w = e.w;
        if (h) *h = e.h;
        // Limit pamięci cache — eviction LRU.
        if (cache_.size() >= 512) evictOldest();
        cache_.emplace(std::move(key), e);
        return t;
    }
private:
    void evictOldest() {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.last_used_ms < oldest->second.last_used_ms) oldest = it;
        }
        if (oldest != cache_.end()) {
            if (oldest->second.tex) SDL_DestroyTexture(oldest->second.tex);
            cache_.erase(oldest);
        }
    }
    std::unordered_map<TextCacheKey, TextCacheEntry, TextCacheKeyHash> cache_;
};

// Animowany komunikat końca gry. Pokazuje się na środku ekranu po zwycięstwie
// (na czas, błędy, mat) lub remisie. Można go zamknąć przyciskiem X lub ESC.
// Animacja: scale-in + fade-in (350 ms, ease-out cubic) + delikatne pulsowanie.
struct WinnerOverlay {
    bool        visible      = false;
    bool        dismissed    = false;
    uint32_t    start_ms     = 0;
    std::string winner_text;   // np. "BIALY WYGRAL", "REMIS"
    std::string reason_text;   // np. "Przekroczenie czasu", "2 bledy"
    GameState   shown_for     = STATE_PAUSED;
};

struct AppResources {
    SDL_Window*   window       = nullptr;
    SDL_Renderer* renderer     = nullptr;
    TTF_Font*     font_xlarge  = nullptr;
    TTF_Font*     font_large   = nullptr;
    TTF_Font*     font_medium  = nullptr;
    TTF_Font*     font_small   = nullptr;
    UIMode        mode         = UI_MODE_QR;
    uint32_t      setup_minutes   = START_MINUTES;
    uint32_t      setup_increment = INCREMENT_SECONDS;
    SDL_Texture*  qr_texture   = nullptr;
    int           qr_w         = 0;
    int           qr_h         = 0;
    std::string   qr_payload;
    std::string   bt_mac;
    int           bt_channel   = DEFAULT_BT_CHAN;
    std::string   bt_status    = "Oczekiwanie na polaczenie Bluetooth...";
    std::string   server_info;
    TextCache     text_cache;
    WinnerOverlay overlay;
};

struct Button {
    SDL_Rect    rect;
    const char* label;
    bool        hover;
};

static const SDL_Color COLOR_BG                 {20, 20, 25, 255};
static const SDL_Color COLOR_FG                 {245, 245, 245, 255};
static const SDL_Color COLOR_ACCENT             {41, 175, 117, 255};
static const SDL_Color COLOR_ACTIVE             {100, 200, 150, 255};
static const SDL_Color COLOR_INACTIVE           {60, 60, 70, 255};
static const SDL_Color COLOR_BUTTON_HOVER       {80, 180, 130, 255};
static const SDL_Color COLOR_BUTTON_NORMAL      {41, 175, 117, 255};
static const SDL_Color COLOR_BUTTON_ERROR       {200, 70, 70, 255};
static const SDL_Color COLOR_BUTTON_ERROR_HOVER {230, 100, 100, 255};
static const SDL_Color COLOR_BUTTON_BONUS       {200, 180, 70, 255};
static const SDL_Color COLOR_BUTTON_BONUS_HOVER {230, 210, 100, 255};
static const SDL_Color COLOR_BUTTON_STOP        {150, 150, 150, 255};
static const SDL_Color COLOR_BUTTON_STOP_HOVER  {180, 180, 180, 255};

// F4: render z cache. Caller NIE niszczy texture (cache jest jej właścicielem).
static void blit_text(SDL_Renderer* r, TextCache& cache, TTF_Font* f,
                      const char* text, SDL_Color col, int x, int y,
                      int* out_w = nullptr, int* out_h = nullptr) {
    if (!text) return;
    int w = 0, h = 0;
    SDL_Texture* t = cache.get(r, f, text, col, &w, &h);
    if (!t) return;
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(r, t, nullptr, &dst);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

// Wersja zwracająca rozmiar tekstu bez kopiowania (np. do centrowania).
static void measure_text(SDL_Renderer* r, TextCache& cache, TTF_Font* f,
                         const char* text, SDL_Color col, int* w, int* h) {
    cache.get(r, f, text, col, w, h);
}

static void draw_filled_rounded_rect(SDL_Renderer* r, SDL_Rect rect,
                                     int radius, SDL_Color color) {
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    SDL_Rect fill = {rect.x + radius, rect.y, rect.w - 2 * radius, rect.h};
    SDL_RenderFillRect(r, &fill);
    fill = SDL_Rect{rect.x, rect.y + radius, rect.w, rect.h - 2 * radius};
    SDL_RenderFillRect(r, &fill);
    for (int i = -radius; i <= radius; i++) {
        for (int j = -radius; j <= radius; j++) {
            if (i*i + j*j <= radius*radius) {
                SDL_RenderDrawPoint(r, rect.x + radius + i, rect.y + radius + j);
                SDL_RenderDrawPoint(r, rect.x + (rect.w - radius) + i, rect.y + radius + j);
                SDL_RenderDrawPoint(r, rect.x + radius + i, rect.y + (rect.h - radius) + j);
                SDL_RenderDrawPoint(r, rect.x + (rect.w - radius) + i, rect.y + (rect.h - radius) + j);
            }
        }
    }
}

static void draw_button(SDL_Renderer* r, TextCache& cache, TTF_Font* f, Button* b, bool hover) {
    SDL_Color bg = hover ? COLOR_BUTTON_HOVER : COLOR_BUTTON_NORMAL;
    draw_filled_rounded_rect(r, b->rect, 8, bg);
    int tw = 0, th = 0;
    measure_text(r, cache, f, b->label, COLOR_FG, &tw, &th);
    blit_text(r, cache, f, b->label, COLOR_FG,
              b->rect.x + (b->rect.w - tw)/2, b->rect.y + (b->rect.h - th)/2);
}

static void draw_colored_button(SDL_Renderer* r, TextCache& cache, TTF_Font* f, Button* b,
                                SDL_Color normal, SDL_Color hover, bool is_hover) {
    SDL_Color bg = is_hover ? hover : normal;
    draw_filled_rounded_rect(r, b->rect, 8, bg);
    int tw = 0, th = 0;
    measure_text(r, cache, f, b->label, COLOR_FG, &tw, &th);
    blit_text(r, cache, f, b->label, COLOR_FG,
              b->rect.x + (b->rect.w - tw)/2, b->rect.y + (b->rect.h - th)/2);
}

static void draw_close_button(SDL_Renderer* r, TextCache& cache, TTF_Font* f) {
    SDL_Rect close_rect{WINDOW_WIDTH - 55, 10, 45, 45};
    draw_filled_rounded_rect(r, close_rect, 8, COLOR_BUTTON_ERROR);
    int tw = 0, th = 0;
    measure_text(r, cache, f, "X", COLOR_FG, &tw, &th);
    blit_text(r, cache, f, "X", COLOR_FG,
              close_rect.x + (close_rect.w - tw)/2,
              close_rect.y + (close_rect.h - th)/2);
}

// ─── QR code rendering ──────────────────────────────────────────────────────

static SDL_Texture* make_qr_texture(SDL_Renderer* r, const std::string& text,
                                    int pixel_size, int* out_w, int* out_h) {
    QRcode* code = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!code) return nullptr;

    const int size   = code->width;
    const int margin = 4;
    const int imgSize = (size + margin * 2) * pixel_size;

    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, imgSize, imgSize, 32,
                                                    SDL_PIXELFORMAT_RGBA32);
    if (!s) { QRcode_free(code); return nullptr; }

    SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, 255, 255, 255, 255));
    uint32_t black = SDL_MapRGBA(s->format, 0, 0, 0, 255);
    uint32_t* px = static_cast<uint32_t*>(s->pixels);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (code->data[y * size + x] & 1) {
                for (int dy = 0; dy < pixel_size; dy++) {
                    for (int dx = 0; dx < pixel_size; dx++) {
                        int xx = (margin + x) * pixel_size + dx;
                        int yy = (margin + y) * pixel_size + dy;
                        px[yy * imgSize + xx] = black;
                    }
                }
            }
        }
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, s);
    if (out_w) *out_w = imgSize;
    if (out_h) *out_h = imgSize;
    SDL_FreeSurface(s);
    QRcode_free(code);
    return tex;
}

// ─── Screen drawing (z TextCache, F4) ───────────────────────────────────────

static void draw_qr_screen(SDL_Renderer* r, AppResources* a) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);

    blit_text(r, a->text_cache, a->font_medium, "ZESKANUJ ABY POLACZYC",
              COLOR_ACCENT, 20, 15);

    if (a->qr_texture) {
        int qr_draw_h = WINDOW_HEIGHT - 70;
        int qr_draw_w = qr_draw_h;
        SDL_Rect dst{20, 60, qr_draw_w, qr_draw_h};
        SDL_RenderCopy(r, a->qr_texture, nullptr, &dst);
    }

    int text_x = 20 + (WINDOW_HEIGHT - 70) + 40;
    int y = 70;
    blit_text(r, a->text_cache, a->font_medium,
              "Krok 1: Sparuj telefon z tym Pi", COLOR_FG, text_x, y); y += 36;
    blit_text(r, a->text_cache, a->font_small,
              "(Ustawienia -> Bluetooth -> chess-clock-pi)", COLOR_FG, text_x, y); y += 26;
    blit_text(r, a->text_cache, a->font_medium,
              "Krok 2: Otworz 'Serial Bluetooth Terminal'", COLOR_FG, text_x, y); y += 36;
    blit_text(r, a->text_cache, a->font_small,
              "lub aplikacje ktora skanuje QR i laczy RFCOMM.", COLOR_FG, text_x, y); y += 30;

    blit_text(r, a->text_cache, a->font_small,
              ("MAC: " + a->bt_mac).c_str(), COLOR_ACCENT, text_x, y); y += 22;
    char chbuf[32]; std::snprintf(chbuf, sizeof(chbuf), "Kanal RFCOMM: %d", a->bt_channel);
    blit_text(r, a->text_cache, a->font_small, chbuf, COLOR_ACCENT, text_x, y); y += 22;
    if (!a->server_info.empty()) {
        blit_text(r, a->text_cache, a->font_small,
                  ("Serwer: " + a->server_info).c_str(), COLOR_FG, text_x, y); y += 22;
    }

    blit_text(r, a->text_cache, a->font_small, a->bt_status.c_str(),
              SDL_Color{200, 150, 80, 255}, text_x, WINDOW_HEIGHT - 30);

    int htw = 0, hth = 0;
    const char* hint = "ESC / X = pomin parowanie  |  pozniej: B = wroc tutaj";
    measure_text(r, a->text_cache, a->font_small, hint,
                 SDL_Color{150,150,150,255}, &htw, &hth);
    blit_text(r, a->text_cache, a->font_small, hint,
              SDL_Color{150,150,150,255},
              WINDOW_WIDTH - htw - 20, WINDOW_HEIGHT - 30);

    draw_close_button(r, a->text_cache, a->font_small);
    /* RenderPresent przeniesiony do ui_render_frame */
}

static void draw_setup_screen(SDL_Renderer* r, AppResources* a) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);

    draw_close_button(r, a->text_cache, a->font_small);

    blit_text(r, a->text_cache, a->font_medium, "ZEGAR SZACHOWY",
              COLOR_ACCENT, 10, 10);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "Czas: %um", a->setup_minutes);
    blit_text(r, a->text_cache, a->font_small, buf, COLOR_FG, 340, 30);

    Button minus_time{{330, 60, 40, 35}, "-", false};
    Button plus_time {{440, 60, 40, 35}, "+", false};
    draw_button(r, a->text_cache, a->font_small, &minus_time, false);
    draw_button(r, a->text_cache, a->font_small, &plus_time,  false);

    std::snprintf(buf, sizeof(buf), "Inc: %us", a->setup_increment);
    blit_text(r, a->text_cache, a->font_small, buf, COLOR_FG, 810, 30);

    Button minus_inc{{810, 60, 40, 35}, "-", false};
    Button plus_inc {{920, 60, 40, 35}, "+", false};
    draw_button(r, a->text_cache, a->font_small, &minus_inc, false);
    draw_button(r, a->text_cache, a->font_small, &plus_inc,  false);

    Button start_btn{{340, 130, 300, 70}, "ROZP. GRE", false};
    draw_button(r, a->text_cache, a->font_medium, &start_btn, false);

    int tw = 0, th = 0;
    const char* hh = "Kliknij +/- aby ustawic, potem START  (telefon moze wyslac NEWGAME)";
    measure_text(r, a->text_cache, a->font_small, hh,
                 SDL_Color{150,150,150,255}, &tw, &th);
    blit_text(r, a->text_cache, a->font_small, hh,
              SDL_Color{150,150,150,255},
              (WINDOW_WIDTH - tw)/2, WINDOW_HEIGHT - 45);

    /* RenderPresent przeniesiony do ui_render_frame */
}

static void draw_time_display(SDL_Renderer* r, TextCache& cache,
                              TTF_Font* time_font, TTF_Font* lbl_font,
                              const char* label, uint32_t ms, SDL_Rect rect,
                              bool active, bool winner) {
    SDL_Color bg = winner ? COLOR_ACCENT : (active ? COLOR_ACTIVE : COLOR_INACTIVE);
    draw_filled_rounded_rect(r, rect, 12, bg);

    char tbuf[16];
    format_time(ms, tbuf, sizeof(tbuf), TIME_FORMAT_MM_SS_MS);

    int tw = 0, th = 0;
    measure_text(r, cache, time_font, tbuf, COLOR_FG, &tw, &th);
    blit_text(r, cache, time_font, tbuf, COLOR_FG,
              rect.x + (rect.w - tw)/2, rect.y + 40);

    measure_text(r, cache, lbl_font, label, COLOR_FG, &tw, &th);
    blit_text(r, cache, lbl_font, label, COLOR_FG,
              rect.x + (rect.w - tw)/2, rect.y + 10);
}

static void draw_status_bar(SDL_Renderer* r, TextCache& cache, TTF_Font* f,
                            const ClockState* c) {
    const char* txt = "";
    SDL_Color col = COLOR_FG;
    switch (c->state) {
        case STATE_STOPPED_BY_ARBITER: txt = "ZATRZYMANO PRZEZ ARBITRA"; col = {255,165,0,255}; break;
        case STATE_RUNNING:            txt = "GRA TRWA"; col = COLOR_ACCENT; break;
        case STATE_PAUSED:             txt = "PAUZA"; col = {200,150,80,255}; break;
        case STATE_FINISHED_LEFT_WIN:  txt = "BIALY WYGRAL!"; col = COLOR_ACCENT; break;
        case STATE_FINISHED_RIGHT_WIN: txt = "CZARNY WYGRAL!"; col = COLOR_ACCENT; break;
        case STATE_FINISHED_DRAW:      txt = "REMIS"; col = COLOR_ACCENT; break;
        default: break;
    }
    int tw = 0, th = 0;
    if (txt && *txt) {
        measure_text(r, cache, f, txt, col, &tw, &th);
        blit_text(r, cache, f, txt, col, (WINDOW_WIDTH - tw)/2, 30);
    }
    char info[64];
    std::snprintf(info, sizeof(info), "Ruch: %u | B:%u | C:%u",
                  c->move_count, c->left.error_count, c->right.error_count);
    measure_text(r, cache, f, info, COLOR_FG, &tw, &th);
    blit_text(r, cache, f, info, COLOR_FG, WINDOW_WIDTH - 250, 35);
}

static void draw_game_screen(SDL_Renderer* r, AppResources* a, const ClockState* c) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);

    draw_status_bar(r, a->text_cache, a->font_small, c);

    SDL_Rect left_rect {20,  80, 600, 220};
    SDL_Rect right_rect{660, 80, 600, 220};
    bool left_winner  = c->state == STATE_FINISHED_LEFT_WIN;
    bool right_winner = c->state == STATE_FINISHED_RIGHT_WIN;
    bool left_active  = c->active == ACTIVE_LEFT  && c->state == STATE_RUNNING;
    bool right_active = c->active == ACTIVE_RIGHT && c->state == STATE_RUNNING;

    draw_time_display(r, a->text_cache, a->font_xlarge, a->font_small, "BIALY",
                      c->left.remaining_ms,  left_rect,  left_active,  left_winner);
    draw_time_display(r, a->text_cache, a->font_xlarge, a->font_small, "CZARNY",
                      c->right.remaining_ms, right_rect, right_active, right_winner);

    Button pause_btn {{ 50, 325, 120, 60},
                      (c->state == STATE_PAUSED) ? "WZNOW" : "PAUZA", false};
    Button reset_btn {{200, 325, 100, 60}, "RESET", false};
    Button arbiter_btn{{350, 325, 150, 60}, "ARBITER", false};
    draw_button(r, a->text_cache, a->font_medium, &pause_btn,   false);
    draw_button(r, a->text_cache, a->font_medium, &reset_btn,   false);
    draw_button(r, a->text_cache, a->font_medium, &arbiter_btn, false);

    if (c->state == STATE_PAUSED || c->state == STATE_STOPPED_BY_ARBITER) {
        const char* big = (c->state == STATE_PAUSED) ? "PAUZA" : "STOP";
        const char* hint = (c->state == STATE_PAUSED)
            ? "Klik zegara lub WZNOW = start"
            : "Tylko arbiter moze wznowic (Q)";
        int tw = 0, th = 0;
        measure_text(r, a->text_cache, a->font_xlarge, big,
                     SDL_Color{255,200,0,255}, &tw, &th);
        blit_text(r, a->text_cache, a->font_xlarge, big,
                  SDL_Color{255,200,0,255}, (WINDOW_WIDTH - tw)/2, 100);
        measure_text(r, a->text_cache, a->font_small, hint,
                     SDL_Color{220,220,220,255}, &tw, &th);
        blit_text(r, a->text_cache, a->font_small, hint,
                  SDL_Color{220,220,220,255}, (WINDOW_WIDTH - tw)/2, 200);
    }

    draw_close_button(r, a->text_cache, a->font_small);

    SDL_SetRenderDrawColor(r, COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, COLOR_ACCENT.a);
    SDL_Rect help_box{1230, 365, 50, 35};
    SDL_RenderFillRect(r, &help_box);
    int tw = 0, th = 0;
    measure_text(r, a->text_cache, a->font_small, "?", COLOR_BG, &tw, &th);
    blit_text(r, a->text_cache, a->font_small, "?", COLOR_BG,
              1230 + (50 - tw)/2, 365 + (35 - th)/2);

    /* RenderPresent przeniesiony do ui_render_frame */
}

static void draw_help_screen(SDL_Renderer* r, AppResources* a) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);
    draw_close_button(r, a->text_cache, a->font_small);

    blit_text(r, a->text_cache, a->font_medium, "POMOC", COLOR_ACCENT, 20, 10);
    blit_text(r, a->text_cache, a->font_small, "Gracze:", COLOR_ACCENT, 20, 60);

    const char* pl[] = {
        "Lewa polowa: BIALY", "Prawa polowa: CZARNY",
        "Klik zegara w pauzie: WZNOW", "SPACJA: Pauza/Wznow",
        "R: Reset", "B: Parowanie BT",
        "H/ESC: Menu", "Ctrl+Q: Wyjscie awaryjne"
    };
    int y = 85;
    for (int i = 0; i < 8; i++) {
        blit_text(r, a->text_cache, a->font_small, pl[i], COLOR_FG, 30, y);
        y += 25;
    }

    blit_text(r, a->text_cache, a->font_small, "Arbitra:", COLOR_ACCENT, 650, 60);
    const char* al[] = { "A: Stop/Q: Wznow", "1/2: Blad B/C", "3/4: +2min B/C" };
    y = 85;
    for (int i = 0; i < 3; i++) {
        blit_text(r, a->text_cache, a->font_small, al[i], COLOR_FG, 660, y);
        y += 25;
    }

    int tw = 0, th = 0;
    const char* fl = "ESC: Powrot do gry";
    measure_text(r, a->text_cache, a->font_small, fl,
                 SDL_Color{150,150,150,255}, &tw, &th);
    blit_text(r, a->text_cache, a->font_small, fl,
              SDL_Color{150,150,150,255},
              (WINDOW_WIDTH - tw)/2, WINDOW_HEIGHT - 40);
    /* RenderPresent przeniesiony do ui_render_frame */
}

static void draw_arbiter_menu(SDL_Renderer* r, AppResources* a, const ClockState* c) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);
    draw_close_button(r, a->text_cache, a->font_small);
    blit_text(r, a->text_cache, a->font_medium, "MENU ARBITRA", COLOR_ACCENT, 20, 20);

    const char* stop_label = (c->state == STATE_STOPPED_BY_ARBITER) ? "WZNOW" : "STOP";
    Button stop_btn   {{ 80,  90, 220, 65}, stop_label,    false};
    Button error_w    {{350,  90, 220, 65}, "BLAD BIALY",  false};
    Button error_b    {{620,  90, 220, 65}, "BLAD CZARNY", false};
    Button bonus_w    {{ 80, 175, 220, 65}, "+2MIN BIALY", false};
    Button bonus_b    {{350, 175, 220, 65}, "+2MIN CZARNY",false};
    Button close_btn  {{620, 175, 220, 65}, "ZAMKNIJ",     false};

    draw_colored_button(r, a->text_cache, a->font_small, &stop_btn,  COLOR_BUTTON_NORMAL,      COLOR_BUTTON_HOVER,       false);
    draw_colored_button(r, a->text_cache, a->font_small, &error_w,   COLOR_BUTTON_ERROR,       COLOR_BUTTON_ERROR_HOVER, false);
    draw_colored_button(r, a->text_cache, a->font_small, &error_b,   COLOR_BUTTON_ERROR,       COLOR_BUTTON_ERROR_HOVER, false);
    draw_colored_button(r, a->text_cache, a->font_small, &bonus_w,   COLOR_BUTTON_BONUS,       COLOR_BUTTON_BONUS_HOVER, false);
    draw_colored_button(r, a->text_cache, a->font_small, &bonus_b,   COLOR_BUTTON_BONUS,       COLOR_BUTTON_BONUS_HOVER, false);
    draw_colored_button(r, a->text_cache, a->font_small, &close_btn, COLOR_BUTTON_STOP,        COLOR_BUTTON_STOP_HOVER,  false);

    char info[64];
    std::snprintf(info, sizeof(info), "B: %u bledy | C: %u bledy",
                  c->left.error_count, c->right.error_count);
    blit_text(r, a->text_cache, a->font_small, info, COLOR_FG, 20, 270);

    /* RenderPresent przeniesiony do ui_render_frame */
}

// ─── UI init / cleanup ──────────────────────────────────────────────────────

static bool init_sdl_and_ttf() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) {
        std::fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return false;
    }
    return true;
}

static bool create_window_and_renderer(AppResources* a) {
    a->window = SDL_CreateWindow("Chess Clock Pi",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 WINDOW_WIDTH, WINDOW_HEIGHT,
                                 SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!a->window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
    // F10: NIE używamy VSYNC. Na Pi (zwłaszcza KMSDRM/X11 + Mesa) zdarza się,
    // że SDL_RenderPresent z VSYNC blokuje na vblank, który nigdy nie nadchodzi
    // (kompozytor zatkany, GPU zatrzymany, brak focus). Manualne SDL_Delay daje
    // 30 FPS i nie ma żadnej blokującej operacji w renderze.
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    a->renderer = SDL_CreateRenderer(a->window, -1, SDL_RENDERER_ACCELERATED);
    if (!a->renderer) {
        // Fallback: software renderer (jeszcze raz, gdyby ACCEL z jakiegoś
        // powodu nie wstał — wtedy w ogóle nic byśmy nie wyrenderowali).
        a->renderer = SDL_CreateRenderer(a->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!a->renderer) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }
    SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
    return true;
}

static bool load_fonts(AppResources* a) {
    const char* path = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    a->font_xlarge = TTF_OpenFont(path, 72);
    a->font_large  = TTF_OpenFont(path, 36);
    a->font_medium = TTF_OpenFont(path, 24);
    a->font_small  = TTF_OpenFont(path, 16);
    if (!a->font_xlarge || !a->font_large || !a->font_medium || !a->font_small) {
        std::fprintf(stderr, "TTF_OpenFont failed: %s\n", TTF_GetError());
        return false;
    }
    return true;
}

static void ui_cleanup(AppResources* a) {
    a->text_cache.clear();
    if (a->qr_texture)  SDL_DestroyTexture(a->qr_texture);
    if (a->font_xlarge) TTF_CloseFont(a->font_xlarge);
    if (a->font_large)  TTF_CloseFont(a->font_large);
    if (a->font_medium) TTF_CloseFont(a->font_medium);
    if (a->font_small)  TTF_CloseFont(a->font_small);
    if (a->renderer)    SDL_DestroyRenderer(a->renderer);
    if (a->window)      SDL_DestroyWindow(a->window);
    TTF_Quit();
    SDL_Quit();
}

static bool ui_init(AppResources* a) {
    if (!init_sdl_and_ttf()) return false;
    if (!create_window_and_renderer(a) || !load_fonts(a)) { ui_cleanup(a); return false; }
    a->mode            = UI_MODE_QR;
    a->setup_minutes   = START_MINUTES;
    a->setup_increment = INCREMENT_SECONDS;
    return true;
}

// ─── Forward declarations ───────────────────────────────────────────────────

class ApiClient;
static void reportArbiter(ApiClient* api, const std::string& action, long long valueMs);

// ─── Event handling ─────────────────────────────────────────────────────────

static void handle_setup_events(ClockState* c, AppResources* a,
                                const SDL_Event* e, bool* running) {
    if (e->type == SDL_QUIT) { *running = false; return; }
    if (e->type == SDL_KEYDOWN) {
        if (e->key.keysym.sym == SDLK_ESCAPE) *running = false;
        else if (e->key.keysym.sym == SDLK_b) a->mode = UI_MODE_QR;
        return;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN) return;
    int x = e->button.x, y = e->button.y;
    if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
        *running = false; return;
    }
    if (x >= 330 && x < 370 && y >= 60 && y < 95) { if (a->setup_minutes > 1)  a->setup_minutes--; return; }
    if (x >= 440 && x < 480 && y >= 60 && y < 95) { if (a->setup_minutes < 60) a->setup_minutes++; return; }
    if (x >= 810 && x < 850 && y >= 60 && y < 95) { if (a->setup_increment > 0)  a->setup_increment--; return; }
    if (x >= 920 && x < 960 && y >= 60 && y < 95) { if (a->setup_increment < 30) a->setup_increment++; return; }
    if (x >= 340 && x <= 640 && y >= 130 && y <= 200) {
        uint32_t start_ms = a->setup_minutes * 60 * 1000;
        uint32_t inc_ms   = a->setup_increment * 1000;
        init_clock(c, start_ms, inc_ms);
        a->mode = UI_MODE_GAME;
        c->state = STATE_PAUSED;
    }
}

static void handle_game_events(ClockState* c, AppResources* a, ApiClient* api,
                               const SDL_Event* e, bool* running) {
    if (e->type == SDL_QUIT) { *running = false; return; }
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
            case SDLK_ESCAPE: a->mode = UI_MODE_SETUP; return;
            case SDLK_h:      a->mode = UI_MODE_HELP;  return;
            case SDLK_b:      a->mode = UI_MODE_QR;    return;
            case SDLK_SPACE: {
                GameState before = c->state;
                pause_resume_clock(c);
                if (c->state != before) {
                    reportArbiter(api,
                                  c->state == STATE_RUNNING ? "resume" : "pause", 0);
                }
                return;
            }
            case SDLK_r: {
                uint32_t s = a->setup_minutes * 60 * 1000;
                uint32_t i = a->setup_increment * 1000;
                reset_clock(c, s, i);
                reportArbiter(api, "reset", 0);
                return;
            }
            case SDLK_a: { stop_by_arbiter(c);   reportArbiter(api, "arbiter_stop", 0); return; }
            case SDLK_q: { resume_by_arbiter(c); reportArbiter(api, "arbiter_resume", 0); return; }
            case SDLK_1: player_error(c, ACTIVE_LEFT);  reportArbiter(api, "error_white", 0); return;
            case SDLK_2: player_error(c, ACTIVE_RIGHT); reportArbiter(api, "error_black", 0); return;
            case SDLK_3: add_bonus_time(c, ACTIVE_LEFT,  2000); reportArbiter(api, "bonus_white", 2000); return;
            case SDLK_4: add_bonus_time(c, ACTIVE_RIGHT, 2000); reportArbiter(api, "bonus_black", 2000); return;
        }
        return;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN) return;
    int x = e->button.x, y = e->button.y;
    if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
        *running = false; return;
    }
    if (x >= 1230 && x <= 1280 && y >= 365 && y <= 400) { a->mode = UI_MODE_HELP; return; }
    if (x >=   50 && x <=  170 && y >= 325 && y <= 385) {
        GameState before = c->state;
        pause_resume_clock(c);
        if (c->state != before)
            reportArbiter(api, c->state == STATE_RUNNING ? "resume" : "pause", 0);
        return;
    }
    if (x >=  200 && x <=  300 && y >= 325 && y <= 385) {
        uint32_t s = a->setup_minutes * 60 * 1000;
        uint32_t i = a->setup_increment * 1000;
        reset_clock(c, s, i);
        reportArbiter(api, "reset", 0);
        return;
    }
    if (x >= 350 && x <= 500 && y >= 325 && y <= 385) { a->mode = UI_MODE_ARBITER; return; }

    ActiveSide side = (x < WINDOW_WIDTH / 2) ? ACTIVE_LEFT : ACTIVE_RIGHT;
    if (c->state == STATE_PAUSED) {
        c->state = STATE_RUNNING;
        reportArbiter(api, "resume", 0);
    }
    switch_side(c, side);
}

static void handle_arbiter_events(ClockState* c, AppResources* a, ApiClient* api,
                                  const SDL_Event* e, bool* running) {
    if (e->type == SDL_QUIT) { *running = false; return; }
    if (e->type == SDL_KEYDOWN) {
        if (e->key.keysym.sym == SDLK_ESCAPE) a->mode = UI_MODE_GAME;
        return;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN) return;
    int x = e->button.x, y = e->button.y;
    if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
        a->mode = UI_MODE_GAME; return;
    }
    if (x >=  80 && x <= 300 && y >=  90 && y <= 155) {
        if (c->state == STATE_STOPPED_BY_ARBITER) {
            resume_by_arbiter(c); reportArbiter(api, "arbiter_resume", 0);
        } else {
            stop_by_arbiter(c);   reportArbiter(api, "arbiter_stop", 0);
        }
        return;
    }
    if (x >= 350 && x <= 570 && y >=  90 && y <= 155) { player_error(c, ACTIVE_LEFT);  reportArbiter(api, "error_white", 0); return; }
    if (x >= 620 && x <= 840 && y >=  90 && y <= 155) { player_error(c, ACTIVE_RIGHT); reportArbiter(api, "error_black", 0); return; }
    if (x >=  80 && x <= 300 && y >= 175 && y <= 240) { add_bonus_time(c, ACTIVE_LEFT,  2000); reportArbiter(api, "bonus_white", 2000); return; }
    if (x >= 350 && x <= 570 && y >= 175 && y <= 240) { add_bonus_time(c, ACTIVE_RIGHT, 2000); reportArbiter(api, "bonus_black", 2000); return; }
    if (x >= 620 && x <= 840 && y >= 175 && y <= 240) { a->mode = UI_MODE_GAME; return; }
}

// F9: w QR mode samo Q nie zamyka aplikacji (myli z innymi ekranami).
static void handle_qr_events(AppResources* a, const SDL_Event* e, bool* running) {
    if (e->type == SDL_QUIT) { *running = false; return; }
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
            case SDLK_ESCAPE:
            case SDLK_RETURN:
            case SDLK_SPACE:
                a->mode = UI_MODE_SETUP;
                return;
        }
        return;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN) {
        int x = e->button.x, y = e->button.y;
        if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
            a->mode = UI_MODE_SETUP;
        }
    }
}

static void ui_process_events(ClockState* c, AppResources* a, ApiClient* api,
                              bool* running) {
    SDL_Event e;
    int processed = 0;
    while (SDL_PollEvent(&e)) {
        // Awaryjne globalne wyjście: Ctrl+Q.
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q &&
            (e.key.keysym.mod & KMOD_CTRL)) {
            *running = false;
            continue;
        }
        // SDL_QUIT zawsze zamyka.
        if (e.type == SDL_QUIT) { *running = false; continue; }

        // Overlay końca gry: jeśli widoczny, pochłania klik na X i ESC, ale
        // nie blokuje innych klawiszy (R, A, Q itp. nadal działają, w razie
        // gdyby user chciał szybko zacząć kolejną grę).
        if (a->overlay.visible && !a->overlay.dismissed) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                a->overlay.dismissed = true;
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN) {
                // Współrzędne X muszą być spójne z draw_winner_overlay
                // (OverlayLayout::PANEL_W=700, PANEL_H=240, CLOSE=44, MARGIN=12).
                const int PANEL_W = 700, PANEL_H = 240;
                const int CB = 44, CM = 12;
                int px = (WINDOW_WIDTH  - PANEL_W) / 2;
                int py = (WINDOW_HEIGHT - PANEL_H) / 2;
                int cbx = px + PANEL_W - CB - CM;
                int cby = py + CM;
                int x = e.button.x, y = e.button.y;
                if (x >= cbx && x <= cbx + CB &&
                    y >= cby && y <= cby + CB) {
                    a->overlay.dismissed = true;
                }
                // Klik gdziekolwiek indziej ignorujemy — nie wyzwalamy
                // przypadkowo podświetlonego przycisku pod spodem.
                continue;
            }
        }

        switch (a->mode) {
            case UI_MODE_QR:      handle_qr_events(a, &e, running);            break;
            case UI_MODE_SETUP:   handle_setup_events(c, a, &e, running);      break;
            case UI_MODE_ARBITER: handle_arbiter_events(c, a, api, &e, running); break;
            case UI_MODE_HELP:
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                    a->mode = UI_MODE_GAME;
                else if (e.type == SDL_MOUSEBUTTONDOWN) {
                    int x = e.button.x, y = e.button.y;
                    if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55)
                        a->mode = UI_MODE_GAME;
                }
                break;
            case UI_MODE_GAME:
            default:              handle_game_events(c, a, api, &e, running); break;
        }
        // Bezpiecznik: nie utknij w pętli eventów na zawsze.
        if (++processed > 256) break;
    }
}

// ─── Animated game-end overlay ──────────────────────────────────────────────

// Wersja blit_text z alpha-modulacją tekstu (do animacji fade-in/out).
static void blit_text_alpha(SDL_Renderer* r, TextCache& cache, TTF_Font* f,
                            const char* text, SDL_Color col, int x, int y, Uint8 alpha,
                            int* out_w = nullptr, int* out_h = nullptr) {
    if (!text) return;
    int w = 0, h = 0;
    SDL_Texture* t = cache.get(r, f, text, col, &w, &h);
    if (!t) return;
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(r, t, nullptr, &dst);
    SDL_SetTextureAlphaMod(t, 255);  // przywróć — ten sam tekstura jest w cache
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

// Geometria panelu overlay — używana zarówno w renderze jak i w obsłudze
// kliknięć, żeby były spójne.
struct OverlayLayout {
    static const int PANEL_W = 700;
    static const int PANEL_H = 240;
    static const int CLOSE_BTN_SIZE = 44;
    static const int CLOSE_BTN_MARGIN = 12;

    int panel_x() const { return (WINDOW_WIDTH  - PANEL_W) / 2; }
    int panel_y() const { return (WINDOW_HEIGHT - PANEL_H) / 2; }
    SDL_Rect close_rect() const {
        return SDL_Rect{panel_x() + PANEL_W - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN,
                        panel_y() + CLOSE_BTN_MARGIN,
                        CLOSE_BTN_SIZE, CLOSE_BTN_SIZE};
    }
};

static void draw_winner_overlay(SDL_Renderer* r, AppResources* a) {
    if (!a->overlay.visible || a->overlay.dismissed) return;

    uint32_t now = SDL_GetTicks();
    uint32_t elapsed = now - a->overlay.start_ms;
    const float ANIM_MS = 350.0f;
    float t  = std::min(1.0f, (float)elapsed / ANIM_MS);
    float te = 1.0f - std::pow(1.0f - t, 3.0f);   // ease-out cubic
    Uint8 fade_alpha = (Uint8)(255.0f * te);

    // Subtelny pulse po zakończeniu animacji (1.0 ± 1.5%).
    float pulse = 1.0f;
    if (t >= 1.0f) {
        float pt = (float)(elapsed - ANIM_MS) / 1000.0f; // sekundy
        pulse = 1.0f + 0.015f * std::sin(pt * 2.0f * 3.14159265f * 0.7f);
    }

    // Tło — półprzezroczysta zasłona.
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(190.0f * te));
    SDL_Rect full{0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
    SDL_RenderFillRect(r, &full);

    // Panel — skalowany od 70% do 100% rozmiaru bazowego, plus pulse.
    OverlayLayout ol;
    float scale = (0.7f + 0.3f * te) * pulse;
    int pw = (int)((float)OverlayLayout::PANEL_W * scale);
    int ph = (int)((float)OverlayLayout::PANEL_H * scale);
    int px = (WINDOW_WIDTH  - pw) / 2;
    int py = (WINDOW_HEIGHT - ph) / 2;

    // Kolor panelu zależy od zwycięzcy.
    SDL_Color panel_bg, text_col;
    if (a->overlay.winner_text == "REMIS") {
        panel_bg = {70, 70, 95, fade_alpha};
        text_col = {245, 245, 250, 255};
    } else if (a->overlay.shown_for == STATE_FINISHED_LEFT_WIN) {
        // BIALY wygrał — jasny panel, ciemny tekst.
        panel_bg = {235, 235, 240, fade_alpha};
        text_col = {25, 25, 35, 255};
    } else {
        // CZARNY wygrał — ciemny panel, jasny tekst.
        panel_bg = {28, 30, 42, fade_alpha};
        text_col = {245, 245, 250, 255};
    }

    SDL_Rect panel{px, py, pw, ph};
    draw_filled_rounded_rect(r, panel, 16, panel_bg);

    // Akcent — paseczek u góry panelu w kolorze "wygranego" gracza.
    SDL_Color accent = (a->overlay.winner_text == "REMIS")
        ? SDL_Color{120, 180, 220, fade_alpha}
        : COLOR_ACCENT;
    accent.a = fade_alpha;
    SDL_Rect strip{px + 20, py + ph - 6, pw - 40, 4};
    SDL_SetRenderDrawColor(r, accent.r, accent.g, accent.b, accent.a);
    SDL_RenderFillRect(r, &strip);

    // Tytuł — "BIALY WYGRAL" itp. Skalujemy ręcznie przez SDL_RenderCopy
    // z dużym dst (tekst już jest cachowany w bazowym rozmiarze).
    {
        int tw = 0, th = 0;
        SDL_Texture* tex = a->text_cache.get(r, a->font_xlarge,
                                             a->overlay.winner_text.c_str(),
                                             text_col, &tw, &th);
        if (tex && tw > 0 && th > 0) {
            int dst_w = (int)(tw * scale);
            int dst_h = (int)(th * scale);
            SDL_Rect dst{px + (pw - dst_w)/2, py + (int)(35 * scale), dst_w, dst_h};
            SDL_SetTextureAlphaMod(tex, fade_alpha);
            SDL_RenderCopy(r, tex, nullptr, &dst);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }

    // Powód (mniejszy tekst).
    if (!a->overlay.reason_text.empty()) {
        int tw = 0, th = 0;
        SDL_Color rc = text_col;
        SDL_Texture* tex = a->text_cache.get(r, a->font_medium,
                                             a->overlay.reason_text.c_str(),
                                             rc, &tw, &th);
        if (tex && tw > 0 && th > 0) {
            int dst_w = (int)(tw * scale);
            int dst_h = (int)(th * scale);
            SDL_Rect dst{px + (pw - dst_w)/2, py + (int)(150 * scale), dst_w, dst_h};
            SDL_SetTextureAlphaMod(tex, (Uint8)(fade_alpha * 0.85f));
            SDL_RenderCopy(r, tex, nullptr, &dst);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }

    // Przycisk X — zawsze w niezmiennej pozycji (klikanie spójne z layoutem).
    SDL_Rect close = ol.close_rect();
    if (t >= 0.6f) {  // przycisk pojawia się dopiero pod koniec animacji
        SDL_Color cb_col = {200, 70, 70, fade_alpha};
        draw_filled_rounded_rect(r, close, 8, cb_col);
        int tw = 0, th = 0;
        SDL_Color xcol{245, 245, 245, 255};
        SDL_Texture* tex = a->text_cache.get(r, a->font_medium, "X", xcol, &tw, &th);
        if (tex) {
            SDL_SetTextureAlphaMod(tex, fade_alpha);
            SDL_Rect xdst{close.x + (close.w - tw)/2, close.y + (close.h - th)/2, tw, th};
            SDL_RenderCopy(r, tex, nullptr, &xdst);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }
}

// Tłumaczenie kodu powodu na polski tekst dla overlay.
static std::string overlay_reason_pl(const std::string& reason) {
    if (reason == "timeout")    return "Przekroczenie czasu";
    if (reason == "errors")     return "Dwa bledy zawodnika";
    if (reason == "checkmate")  return "Mat";
    if (reason == "stalemate")  return "Pat - remis";
    return "";
}

// Aktualizacja stanu overlay na podstawie stanu zegara. Wywoływana raz na klatkę.
static void update_overlay_state(AppResources& app, const ClockState& cs) {
    bool finished = cs.state == STATE_FINISHED_LEFT_WIN ||
                    cs.state == STATE_FINISHED_RIGHT_WIN ||
                    cs.state == STATE_FINISHED_DRAW;

    if (!finished) {
        // Gra wróciła do aktywnego stanu — gotowi na następną.
        app.overlay.visible = false;
        app.overlay.dismissed = false;
        return;
    }

    if (app.overlay.visible || app.overlay.dismissed) return;

    // Pierwszy raz widzimy ten finished state — pokaż overlay.
    app.overlay.visible   = true;
    app.overlay.dismissed = false;
    app.overlay.start_ms  = SDL_GetTicks();
    app.overlay.shown_for = cs.state;
    if      (cs.state == STATE_FINISHED_LEFT_WIN)  app.overlay.winner_text = "BIALY WYGRAL";
    else if (cs.state == STATE_FINISHED_RIGHT_WIN) app.overlay.winner_text = "CZARNY WYGRAL";
    else                                            app.overlay.winner_text = "REMIS";
    app.overlay.reason_text = overlay_reason_pl(cs.finish_reason);
}

static void ui_render_frame(SDL_Renderer* r, AppResources* a, const ClockState* c) {
    switch (a->mode) {
        case UI_MODE_QR:      draw_qr_screen(r, a);          break;
        case UI_MODE_SETUP:   draw_setup_screen(r, a);       break;
        case UI_MODE_HELP:    draw_help_screen(r, a);        break;
        case UI_MODE_ARBITER: draw_arbiter_menu(r, a, c);    break;
        case UI_MODE_GAME:
        default:              draw_game_screen(r, a, c);     break;
    }
    // Overlay końca gry rysujemy na ekranach gry/arbitra — żeby było widać
    // wynik nawet gdy ktoś wszedł do menu arbitra po zakończeniu.
    if (a->mode == UI_MODE_GAME || a->mode == UI_MODE_ARBITER) {
        draw_winner_overlay(r, a);
    }
    // F10: jeden RenderPresent na klatkę, BEZ VSYNC. Frame timing kontroluje
    // SDL_Delay w głównej pętli.
    SDL_RenderPresent(r);
}

// ─── HTTP relay ─────────────────────────────────────────────────────────────

static std::string stripPort(const std::string& ip) {
    auto p = ip.find(':');
    return (p == std::string::npos) ? ip : ip.substr(0, p);
}

static int portFromHost(const std::string& host, int fallback) {
    auto p = host.find(':');
    if (p == std::string::npos) return fallback;
    try { return std::stoi(host.substr(p + 1)); } catch (...) { return fallback; }
}

// F2: nieblokujący connect z timeoutem przez select(). Sprawdza także
// flagę abort co iterację — pozwala szybko zatrzymać wątek HTTP przy stop().
static int connect_with_timeout(int sock, const sockaddr* addr, socklen_t alen,
                                int timeout_ms, std::atomic<bool>* abort_flag) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = ::connect(sock, addr, alen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);  // tryb blokujący z powrotem
        return 0;
    }
    if (errno != EINPROGRESS) return -1;

    int elapsed = 0;
    const int slice = 200;
    while (elapsed < timeout_ms) {
        if (abort_flag && abort_flag->load()) { errno = ECANCELED; return -1; }
        fd_set wf; FD_ZERO(&wf); FD_SET(sock, &wf);
        struct timeval tv{0, slice * 1000};
        rc = ::select(sock + 1, nullptr, &wf, nullptr, &tv);
        if (rc < 0) {
            if (errno == EINTR) { elapsed += slice; continue; }
            return -1;
        }
        if (rc > 0) {
            int err = 0; socklen_t elen = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) return -1;
            if (err != 0) { errno = err; return -1; }
            fcntl(sock, F_SETFL, flags);
            return 0;
        }
        elapsed += slice;
    }
    errno = ETIMEDOUT;
    return -1;
}

static std::string httpPost(const std::string& ip, int port,
                            const std::string& path, const std::string& body,
                            const std::string& clockCode,
                            const std::string& apiKey,
                            std::atomic<bool>* abort_flag) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_ERR(std::string("httpPost socket() failed: ") + std::strerror(errno));
        return "";
    }

    struct timeval tv{HTTP_IO_TIMEOUT_MS / 1000, (HTTP_IO_TIMEOUT_MS % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(sock);
        LOG_ERR("httpPost: bad IP " + ip);
        return "";
    }

    if (connect_with_timeout(sock, (sockaddr*)&addr, sizeof(addr),
                             HTTP_CONNECT_TIMEOUT_MS, abort_flag) < 0) {
        LOG_WARN("httpPost connect() failed for " + ip + ":" + std::to_string(port) +
                 " path=" + path + " errno=" + std::to_string(errno));
        close(sock);
        return "";
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << ip << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    if (!clockCode.empty()) req << "X-Clock-Code: " << clockCode << "\r\n";
    if (!apiKey.empty())    req << "X-API-Key: "    << apiKey    << "\r\n";
    req << "Connection: close\r\n\r\n" << body;
    std::string r = req.str();
    if (send(sock, r.c_str(), r.size(), MSG_NOSIGNAL) < 0) {
        LOG_WARN("httpPost send() failed for path=" + path);
        close(sock);
        return "";
    }

    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        if (abort_flag && abort_flag->load()) break;
        buf[n] = '\0';
        response += buf;
        if (response.size() > 256 * 1024) break; // ochrona pamięci
    }
    close(sock);
    return response;
}

static std::string httpGet(const std::string& ip, int port,
                           const std::string& path,
                           const std::string& clockCode,
                           const std::string& apiKey,
                           std::atomic<bool>* abort_flag) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";
    struct timeval tv{HTTP_IO_TIMEOUT_MS / 1000, (HTTP_IO_TIMEOUT_MS % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(sock); return "";
    }

    if (connect_with_timeout(sock, (sockaddr*)&addr, sizeof(addr),
                             HTTP_CONNECT_TIMEOUT_MS, abort_flag) < 0) {
        LOG_WARN("httpGet connect() failed for " + ip + ":" + std::to_string(port) +
                 " path=" + path);
        close(sock);
        return "";
    }

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << ip << ":" << port << "\r\n";
    if (!clockCode.empty()) req << "X-Clock-Code: " << clockCode << "\r\n";
    if (!apiKey.empty())    req << "X-API-Key: "    << apiKey    << "\r\n";
    req << "Connection: close\r\n\r\n";
    std::string r = req.str();
    send(sock, r.c_str(), r.size(), MSG_NOSIGNAL);

    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        if (abort_flag && abort_flag->load()) break;
        buf[n] = '\0';
        response += buf;
        if (response.size() > 256 * 1024) break;
    }
    close(sock);
    return response;
}

static bool verifyApiKey(const std::string& ip, int port,
                         const std::string& clockCode, const std::string& apiKey) {
    if (clockCode.empty() || apiKey.empty() || ip.empty()) return false;
    std::atomic<bool> abort_flag{false};
    std::string resp = httpGet(ip, port, "/api/clock/info", clockCode, apiKey, &abort_flag);
    auto bodyPos = resp.find("\r\n\r\n");
    std::string body = (bodyPos != std::string::npos) ? resp.substr(bodyPos + 4) : resp;
    return body.find("\"ok\":true")  != std::string::npos ||
           body.find("\"ok\": true") != std::string::npos;
}

static std::string parseGameId(const std::string& response) {
    auto pos = response.find("\"game_id\"");
    if (pos == std::string::npos) return "";
    auto colon = response.find(':', pos);
    if (colon == std::string::npos) return "";
    size_t s = colon + 1;
    while (s < response.size() && (response[s] == ' ' || response[s] == '\n' || response[s] == '\r')) s++;
    if (s >= response.size()) return "";
    if (response[s] == '"') {
        auto q = response.find('"', s + 1);
        if (q == std::string::npos) return "";
        return response.substr(s + 1, q - s - 1);
    }
    size_t e = s;
    while (e < response.size() && std::isdigit(static_cast<unsigned char>(response[e]))) e++;
    return response.substr(s, e - s);
}

// ─── ApiClient ──────────────────────────────────────────────────────────────

enum ApiKind {
    K_NEWGAME = 0,
    K_MOVE    = 1,
    K_STATUS  = 2,
    K_ARBITER = 3
};

struct PendingRequest {
    ApiKind     kind;
    std::string white;
    std::string black;
    long long   tcMs = 0;
    std::string uci;
    std::string color;
    long long   timeLeftMs = 0;
    std::string status;
    std::string winner;
    std::string action;
    long long   value = 0;
    long long   tsMs  = 0;
    int         attempts = 0;
};

class ApiClient {
public:
    void configure(bool networked, const std::string& ip, int port,
                   const std::string& clockCode, const std::string& apiKey,
                   const std::string& queueFile) {
        networked_  = networked;
        ip_         = ip;
        port_       = port;
        clockCode_  = clockCode;
        apiKey_     = apiKey;
        queueFile_  = queueFile;
    }

    void start() {
        if (!networked_) return;
        loadQueue();
        running_ = true;
        thr_ = std::thread(&ApiClient::workerLoop, this);
        // F2: osobny wątek persist z debounce — UI/worker nigdy nie blokują na SD.
        persist_thr_ = std::thread(&ApiClient::persistLoop, this);
        LOG_INFO("ApiClient started (queue size=" + std::to_string(queue_.size()) + ")");
    }

    void stop() {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> g(qmu_);
            running_ = false;
        }
        abort_flag_.store(true);  // przerwij blokujące I/O w workerze
        qcv_.notify_all();
        persist_cv_.notify_all();
        if (thr_.joinable()) thr_.join();
        if (persist_thr_.joinable()) persist_thr_.join();
        persistQueueNow(); // ostatni zapis przy wyjściu
        LOG_INFO("ApiClient stopped (queue size=" + std::to_string(queue_.size()) + ")");
    }

    bool networked() const { return networked_; }
    size_t pendingCount() {
        std::lock_guard<std::mutex> g(qmu_);
        return queue_.size();
    }

    void enqueueNewGame(const std::string& white, const std::string& black, long long tcMs) {
        if (!networked_) return;
        PendingRequest r{}; r.kind = K_NEWGAME;
        r.white = white; r.black = black; r.tcMs = tcMs; r.tsMs = nowMs();
        push(std::move(r));
    }
    void enqueueMove(const std::string& uci, const std::string& color, long long timeLeftMs) {
        if (!networked_) return;
        PendingRequest r{}; r.kind = K_MOVE;
        r.uci = uci; r.color = color; r.timeLeftMs = timeLeftMs; r.tsMs = nowMs();
        push(std::move(r));
    }
    void enqueueStatus(const std::string& status, const std::string& winner) {
        if (!networked_) return;
        PendingRequest r{}; r.kind = K_STATUS;
        r.status = status; r.winner = winner; r.tsMs = nowMs();
        push(std::move(r));
    }
    void enqueueArbiter(const std::string& action, long long value) {
        if (!networked_) return;
        PendingRequest r{}; r.kind = K_ARBITER;
        r.action = action; r.value = value; r.tsMs = nowMs();
        push(std::move(r));
    }

    void setGameId(const std::string& gid) {
        std::lock_guard<std::mutex> g(idmu_);
        gameId_ = gid;
    }
    std::string gameId() {
        std::lock_guard<std::mutex> g(idmu_);
        return gameId_;
    }
    void clearGameId() {
        std::lock_guard<std::mutex> g(idmu_);
        gameId_.clear();
    }

private:
    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void push(PendingRequest&& r) {
        {
            std::lock_guard<std::mutex> g(qmu_);
            queue_.push_back(std::move(r));
        }
        // F2: nie persistuj synchronicznie — tylko zaznacz, że treba zapisać.
        markDirty();
        qcv_.notify_one();
    }

    void markDirty() {
        dirty_.store(true);
        persist_cv_.notify_one();
    }

    void workerLoop() {
        int backoff = HTTP_BACKOFF_BASE_MS;
        while (true) {
            PendingRequest req;
            {
                std::unique_lock<std::mutex> g(qmu_);
                qcv_.wait(g, [&]{ return !running_.load() || !queue_.empty(); });
                if (!running_.load() && queue_.empty()) return;
                req = queue_.front();
                queue_.pop_front();
            }

            if (req.kind != K_NEWGAME && gameId().empty()) {
                {
                    std::lock_guard<std::mutex> g(qmu_);
                    queue_.push_front(std::move(req));
                }
                std::unique_lock<std::mutex> g(qmu_);
                qcv_.wait_for(g, std::chrono::milliseconds(500),
                              [&]{ return !running_.load(); });
                if (!running_.load()) return;
                continue;
            }

            bool ok = trySend(req);
            if (ok) {
                backoff = HTTP_BACKOFF_BASE_MS;
                markDirty();
                continue;
            }

            req.attempts++;
            LOG_WARN("ApiClient send failed (kind=" + std::to_string(req.kind) +
                     " attempts=" + std::to_string(req.attempts) +
                     " backoff_ms=" + std::to_string(backoff) + ")");
            {
                std::lock_guard<std::mutex> g(qmu_);
                queue_.push_front(std::move(req));
            }
            markDirty();

            std::unique_lock<std::mutex> g(qmu_);
            qcv_.wait_for(g, std::chrono::milliseconds(backoff),
                          [&]{ return !running_.load(); });
            if (!running_.load()) return;
            backoff = std::min(backoff * 2, HTTP_BACKOFF_MAX_MS);
        }
    }

    // F2: osobny wątek persist. Debounce: jeśli wiele zmian w krótkim czasie,
    // zapisuje tylko raz na ~250 ms. Atomic write: najpierw .tmp, potem rename.
    void persistLoop() {
        while (running_.load()) {
            std::unique_lock<std::mutex> g(persist_mu_);
            persist_cv_.wait_for(g, std::chrono::milliseconds(QUEUE_PERSIST_MIN_INTERVAL_MS),
                                 [&]{ return !running_.load() || dirty_.load(); });
            if (!running_.load()) break;
            if (dirty_.exchange(false)) {
                g.unlock();
                persistQueueNow();
            }
        }
    }

    bool trySend(PendingRequest& req) {
        switch (req.kind) {
            case K_NEWGAME: return sendNewGame(req);
            case K_MOVE:    return sendMoveReq(req);
            case K_STATUS:  return sendStatusReq(req);
            case K_ARBITER: return sendArbiterReq(req);
        }
        return false;
    }

    bool sendNewGame(const PendingRequest& r) {
        std::ostringstream b;
        b << "{\"white_player\":\"" << r.white << "\","
          << "\"black_player\":\""  << r.black << "\","
          << "\"time_control_ms\":" << r.tcMs << "}";
        std::string resp = httpPost(ip_, port_, "/api/clock/newgame", b.str(),
                                    clockCode_, apiKey_, &abort_flag_);
        if (resp.empty()) return false;
        std::string gid = parseGameId(resp);
        if (gid.empty()) {
            LOG_ERR("ApiClient: NEWGAME response without game_id");
            return false;
        }
        setGameId(gid);
        LOG_INFO("ApiClient: game_id=" + gid + " (white=" + r.white + " black=" + r.black + ")");
        return true;
    }

    bool sendMoveReq(const PendingRequest& r) {
        std::string gid = gameId();
        if (gid.empty()) return false;
        std::ostringstream b;
        b << "{\"game_id\":" << gid << ","
          << "\"uci\":\""    << r.uci << "\","
          << "\"player\":\"" << r.color << "\","
          << "\"time_left_ms\":" << r.timeLeftMs << "}";
        std::string resp = httpPost(ip_, port_, "/api/clock/move", b.str(),
                                    clockCode_, apiKey_, &abort_flag_);
        return !resp.empty();
    }

    bool sendStatusReq(const PendingRequest& r) {
        std::string gid = gameId();
        if (gid.empty()) return false;
        std::ostringstream b;
        b << "{\"game_id\":" << gid << ","
          << "\"status\":\"" << r.status << "\","
          << "\"winner\":\"" << r.winner << "\"}";
        std::string resp = httpPost(ip_, port_, "/api/clock/status", b.str(),
                                    clockCode_, apiKey_, &abort_flag_);
        return !resp.empty();
    }

    bool sendArbiterReq(const PendingRequest& r) {
        std::string gid = gameId();
        if (gid.empty()) return false;
        std::ostringstream b;
        b << "{\"game_id\":" << gid << ","
          << "\"action\":\"" << r.action << "\","
          << "\"value_ms\":" << r.value << ","
          << "\"ts_ms\":"    << r.tsMs  << "}";
        std::string resp = httpPost(ip_, port_, "/api/clock/arbiter", b.str(),
                                    clockCode_, apiKey_, &abort_flag_);
        return !resp.empty();
    }

    // F2: atomic write — najpierw .tmp, potem rename. Awaria zasilania nie
    // pozostawi uszkodzonego pliku.
    void persistQueueNow() {
        if (queueFile_.empty()) return;
        std::string tmp = queueFile_ + ".tmp";
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.good()) {
            LOG_WARN("Cannot write queue tmp file: " + tmp);
            return;
        }
        f << "GAMEID\t" << gameId() << "\n";
        {
            std::lock_guard<std::mutex> g(qmu_);
            for (const auto& r : queue_) {
                f << r.kind << "\t";
                switch (r.kind) {
                    case K_NEWGAME:
                        f << escape(r.white) << "\t" << escape(r.black) << "\t" << r.tcMs;
                        break;
                    case K_MOVE:
                        f << escape(r.uci) << "\t" << escape(r.color) << "\t" << r.timeLeftMs;
                        break;
                    case K_STATUS:
                        f << escape(r.status) << "\t" << escape(r.winner);
                        break;
                    case K_ARBITER:
                        f << escape(r.action) << "\t" << r.value;
                        break;
                }
                f << "\t" << r.tsMs << "\t" << r.attempts << "\n";
            }
        }
        f.flush();
        f.close();
        if (std::rename(tmp.c_str(), queueFile_.c_str()) != 0) {
            LOG_WARN("Cannot rename queue file: " + std::string(std::strerror(errno)));
        }
    }

    void loadQueue() {
        std::ifstream f(queueFile_);
        if (!f.good()) return;
        std::string line;
        std::lock_guard<std::mutex> g(qmu_);
        queue_.clear();
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto tokens = splitTab(line);
            if (tokens.empty()) continue;
            if (tokens[0] == "GAMEID") {
                if (tokens.size() >= 2) {
                    std::lock_guard<std::mutex> gg(idmu_);
                    gameId_ = tokens[1];
                }
                continue;
            }
            try {
                int kind = std::stoi(tokens[0]);
                PendingRequest r{};
                r.kind = static_cast<ApiKind>(kind);
                switch (r.kind) {
                    case K_NEWGAME:
                        if (tokens.size() < 6) continue;
                        r.white = unescape(tokens[1]); r.black = unescape(tokens[2]);
                        r.tcMs  = std::stoll(tokens[3]); r.tsMs = std::stoll(tokens[4]);
                        r.attempts = std::stoi(tokens[5]);
                        break;
                    case K_MOVE:
                        if (tokens.size() < 6) continue;
                        r.uci   = unescape(tokens[1]); r.color = unescape(tokens[2]);
                        r.timeLeftMs = std::stoll(tokens[3]); r.tsMs = std::stoll(tokens[4]);
                        r.attempts = std::stoi(tokens[5]);
                        break;
                    case K_STATUS:
                        if (tokens.size() < 5) continue;
                        r.status = unescape(tokens[1]); r.winner = unescape(tokens[2]);
                        r.tsMs = std::stoll(tokens[3]); r.attempts = std::stoi(tokens[4]);
                        break;
                    case K_ARBITER:
                        if (tokens.size() < 5) continue;
                        r.action = unescape(tokens[1]); r.value = std::stoll(tokens[2]);
                        r.tsMs = std::stoll(tokens[3]); r.attempts = std::stoi(tokens[4]);
                        break;
                }
                queue_.push_back(std::move(r));
            } catch (...) {
                LOG_WARN("Skipping malformed queue line: " + line);
            }
        }
        if (!queue_.empty())
            LOG_INFO("Loaded " + std::to_string(queue_.size()) +
                     " pending requests from " + queueFile_);
    }

    static std::string escape(const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) {
            if (c == '\t') out += "\\t";
            else if (c == '\n') out += "\\n";
            else if (c == '\\') out += "\\\\";
            else out.push_back(c);
        }
        return out;
    }
    static std::string unescape(const std::string& s) {
        std::string out; out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[i+1];
                if      (n == 't')  { out.push_back('\t'); ++i; }
                else if (n == 'n')  { out.push_back('\n'); ++i; }
                else if (n == '\\') { out.push_back('\\'); ++i; }
                else out.push_back(s[i]);
            } else out.push_back(s[i]);
        }
        return out;
    }
    static std::vector<std::string> splitTab(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == '\t') { out.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        out.push_back(cur);
        return out;
    }

    bool networked_ = false;
    std::string ip_;
    int port_ = 9090;
    std::string clockCode_;
    std::string apiKey_;
    std::string queueFile_;

    std::thread thr_;
    std::thread persist_thr_;
    std::atomic<bool> running_{false};
    std::atomic<bool> abort_flag_{false};
    std::atomic<bool> dirty_{false};
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::mutex persist_mu_;
    std::condition_variable persist_cv_;
    std::deque<PendingRequest> queue_;

    std::mutex idmu_;
    std::string gameId_;
};

static void reportArbiter(ApiClient* api, const std::string& action, long long valueMs) {
    if (!api || !api->networked()) return;
    api->enqueueArbiter(action, valueMs);
}

// ─── BluetoothServer (F1, F5, F7) ───────────────────────────────────────────
//
// Trzy wątki:
//   - acceptLoop: accept() + recv() (klient → kolejka odbiorcza)
//   - sendLoop:   wysyła wiadomości z kolejki nadawczej (NIE blokuje UI)
//   - main UI thread czyta z kolejki odbiorczej i wkłada do nadawczej
//
// cli_sock_ jest atomic + chroniony krótkim mutexem dla mocniejszej semantyki.

class BluetoothServer {
public:
    bool start(int channel);
    void stop();

    bool is_connected() const { return connected_.load(); }
    bool pop_line(std::string& out);
    void send_line(const std::string& line);

    std::string mac()     const { return mac_; }
    int         channel() const { return channel_; }

private:
    void acceptLoop();
    void sendLoop();
    void closeClientLocked();   // wywoływane z trzymanym sock_mu_

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread       accept_thr_;
    std::thread       send_thr_;

    int               srv_sock_ = -1;
    std::atomic<int>  cli_sock_{-1};
    std::mutex        sock_mu_;   // chroni operacje na cli_sock_
    int               channel_  = DEFAULT_BT_CHAN;
    std::string       mac_      = "(niedostepne)";

    std::mutex               in_mu_;
    std::deque<std::string>  in_queue_;
    std::string              recv_buffer_;

    std::mutex               out_mu_;
    std::condition_variable  out_cv_;
    std::deque<std::string>  out_queue_;
};

bool BluetoothServer::start(int channel) {
    channel_ = channel;

    int dev_id = hci_get_route(nullptr);
    if (dev_id >= 0) {
        bdaddr_t bdaddr{};
        if (hci_devba(dev_id, &bdaddr) == 0) {
            char addr[18] = {0};
            ba2str(&bdaddr, addr);
            mac_ = addr;
        }
    }

    srv_sock_ = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (srv_sock_ < 0) {
        LOG_ERR(std::string("bluetooth socket() failed: ") + std::strerror(errno));
        return false;
    }

    sockaddr_rc loc{};
    loc.rc_family           = AF_BLUETOOTH;
    bdaddr_t any_addr{{0,0,0,0,0,0}};
    loc.rc_bdaddr           = any_addr;
    loc.rc_channel          = static_cast<uint8_t>(channel);

    if (bind(srv_sock_, (sockaddr*)&loc, sizeof(loc)) < 0) {
        LOG_ERR(std::string("bluetooth bind() failed: ") + std::strerror(errno));
        close(srv_sock_); srv_sock_ = -1;
        return false;
    }
    if (listen(srv_sock_, 1) < 0) {
        LOG_ERR(std::string("bluetooth listen() failed: ") + std::strerror(errno));
        close(srv_sock_); srv_sock_ = -1;
        return false;
    }

    running_ = true;
    accept_thr_ = std::thread(&BluetoothServer::acceptLoop, this);
    send_thr_   = std::thread(&BluetoothServer::sendLoop,   this);
    LOG_INFO("BluetoothServer listening on RFCOMM channel " + std::to_string(channel));
    return true;
}

void BluetoothServer::closeClientLocked() {
    int cs = cli_sock_.exchange(-1);
    if (cs >= 0) {
        ::shutdown(cs, SHUT_RDWR);
        close(cs);
    }
    connected_ = false;
}

void BluetoothServer::stop() {
    running_ = false;

    {
        std::lock_guard<std::mutex> g(sock_mu_);
        closeClientLocked();
        if (srv_sock_ >= 0) {
            ::shutdown(srv_sock_, SHUT_RDWR);
            close(srv_sock_);
            srv_sock_ = -1;
        }
    }
    out_cv_.notify_all();

    if (accept_thr_.joinable()) accept_thr_.join();
    if (send_thr_.joinable())   send_thr_.join();
}

void BluetoothServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_rc rem{};
        socklen_t len = sizeof(rem);
        int cs = accept(srv_sock_, (sockaddr*)&rem, &len);
        if (cs < 0) {
            if (!running_) return;
            LOG_WARN(std::string("bluetooth accept() failed: ") + std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // F1: Ustawiamy timeouty wysyłania i odbioru, by send() i recv() nie
        // blokowały w nieskończoność jeśli klient się zawiesi.
        struct timeval tv_send{BT_SEND_TIMEOUT_MS / 1000,
                               (BT_SEND_TIMEOUT_MS % 1000) * 1000};
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));
        struct timeval tv_recv{30, 0};  // generous timeout dla recv (heartbeat protection)
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));

        {
            std::lock_guard<std::mutex> g(sock_mu_);
            cli_sock_.store(cs);
        }
        connected_ = true;

        {
            std::lock_guard<std::mutex> g(in_mu_);
            in_queue_.push_back("__BT_CONNECTED__");
            if (in_queue_.size() > BT_QUEUE_MAX) in_queue_.pop_front();
        }
        out_cv_.notify_all();

        char buf[1024];
        while (running_.load()) {
            int n = recv(cs, buf, sizeof(buf) - 1, 0);
            if (n == 0) break;             // klient zamknął
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // recv timeout — OK, sprawdź running
                if (errno == EINTR) continue;
                break;                     // realny błąd
            }

            // F5: limit bufora przed dopisaniem.
            if (recv_buffer_.size() + (size_t)n > BT_RECV_BUFFER_MAX) {
                LOG_WARN("BT recv_buffer overflow — czyszczenie");
                recv_buffer_.clear();
            }
            recv_buffer_.append(buf, n);

            size_t pos;
            while ((pos = recv_buffer_.find('\n')) != std::string::npos) {
                std::string line = recv_buffer_.substr(0, pos);
                recv_buffer_.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > BT_LINE_MAX) {
                    LOG_WARN("BT line too long — odrzucam");
                    continue;
                }
                if (!line.empty()) {
                    std::lock_guard<std::mutex> g(in_mu_);
                    if (in_queue_.size() < BT_QUEUE_MAX) {
                        in_queue_.push_back(std::move(line));
                    } else {
                        LOG_WARN("BT in_queue full — drop");
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> g(sock_mu_);
            closeClientLocked();
        }
        recv_buffer_.clear();
        {
            std::lock_guard<std::mutex> g(in_mu_);
            in_queue_.push_back("__BT_DISCONNECTED__");
            if (in_queue_.size() > BT_QUEUE_MAX) in_queue_.pop_front();
        }
        out_cv_.notify_all();  // obudź sender, niech zauważy disconnect
    }
}

// F1: osobny wątek wysyłający. Bierze wiadomości z kolejki, pisze na socket
// z timeoutem (SO_SNDTIMEO). Jeśli send() zwróci błąd, wiadomość JEST PORZUCANA
// (bo i tak klient odpadł — kolejne CLOCK|... pojawi się za 500 ms).
void BluetoothServer::sendLoop() {
    while (running_.load()) {
        std::string msg;
        {
            std::unique_lock<std::mutex> g(out_mu_);
            out_cv_.wait(g, [&]{ return !running_.load() || !out_queue_.empty(); });
            if (!running_.load() && out_queue_.empty()) return;
            if (out_queue_.empty()) continue;
            msg = std::move(out_queue_.front());
            out_queue_.pop_front();
        }

        int cs = cli_sock_.load();
        if (cs < 0) continue;  // brak klienta — porzuć wiadomość

        if (msg.empty() || msg.back() != '\n') msg.push_back('\n');

        ssize_t off = 0;
        bool failed = false;
        while (off < (ssize_t)msg.size()) {
            ssize_t n = send(cs, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    LOG_WARN("BT send timeout — klient nieresponsywny, zamykam");
                } else {
                    LOG_WARN("BT send error: " + std::string(std::strerror(errno)));
                }
                failed = true;
                break;
            }
            off += n;
        }

        // Jeśli klient nie odbiera, wymuś rozłączenie — niech acceptLoop
        // zauważy disconnect i przyjmie nowe połączenie. Tablet sam się
        // zsynchronizuje przez SYNC_REQUEST.
        if (failed) {
            std::lock_guard<std::mutex> g(sock_mu_);
            closeClientLocked();
        }
    }
}

bool BluetoothServer::pop_line(std::string& out) {
    std::lock_guard<std::mutex> g(in_mu_);
    if (in_queue_.empty()) return false;
    out = std::move(in_queue_.front());
    in_queue_.pop_front();
    return true;
}

// F1: send_line WRZUCA do kolejki i kończy. Nigdy nie blokuje wątku UI.
void BluetoothServer::send_line(const std::string& line) {
    if (!is_connected()) return;
    {
        std::lock_guard<std::mutex> g(out_mu_);
        if (out_queue_.size() >= BT_SEND_QUEUE_MAX) {
            // Zalewa nas kolejka nadawcza (klient nie odbiera). Wyrzuć stare
            // CLOCK|... — i tak nieaktualne. Zostaw OK/ERR/MOVE_ACCEPTED/GAME_OVER.
            for (auto it = out_queue_.begin(); it != out_queue_.end();) {
                if (it->rfind("CLOCK|", 0) == 0) it = out_queue_.erase(it);
                else ++it;
                if (out_queue_.size() < BT_SEND_QUEUE_MAX / 2) break;
            }
            if (out_queue_.size() >= BT_SEND_QUEUE_MAX) {
                LOG_WARN("BT out_queue full — dropping line");
                return;
            }
        }
        out_queue_.push_back(line);
    }
    out_cv_.notify_one();
}

// ─── Bluetooth command handling ─────────────────────────────────────────────

struct Session {
    Board                   board{};
    std::list<std::string>  moveHistory;
    std::string             whiteName = "White";
    std::string             blackName = "Black";
    bool                    networked = false;
    bool                    tcEnabled  = false;
    long long               startMs    = 0;
    int                     setupMinutes   = START_MINUTES;
    int                     setupIncrement = INCREMENT_SECONDS;
    bool                    over       = false;

    int                     lastAcceptedSeq = -1;
    std::string             lastAcceptedUci;

    bool                    awaiting_ack = false;
    std::string             ack_context;
    std::string             ack_message;
    uint32_t                ack_last_sent_ms = 0;
    int                     ack_retries = 0;
};

static std::vector<std::string> splitPipe(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '|') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

static void send_with_ack(BluetoothServer& bt, Session& sess,
                          const std::string& context, const std::string& message) {
    sess.awaiting_ack    = true;
    sess.ack_context     = context;
    sess.ack_message     = message;
    sess.ack_last_sent_ms = SDL_GetTicks();
    sess.ack_retries     = 0;
    bt.send_line(message);
}

static void clear_ack(Session& sess) {
    sess.awaiting_ack = false;
    sess.ack_context.clear();
    sess.ack_message.clear();
    sess.ack_retries = 0;
}

static void tick_ack(BluetoothServer& bt, Session& sess) {
    if (!sess.awaiting_ack) return;
    if (!bt.is_connected()) return;
    uint32_t now = SDL_GetTicks();
    if (now - sess.ack_last_sent_ms < (uint32_t)ACK_TIMEOUT_MS) return;
    if (sess.ack_retries >= ACK_MAX_RETRIES) {
        LOG_WARN("ACK timeout after " + std::to_string(sess.ack_retries) +
                 " retries for context=" + sess.ack_context);
        clear_ack(sess);
        return;
    }
    sess.ack_retries++;
    sess.ack_last_sent_ms = now;
    bt.send_line(sess.ack_message);
    LOG_INFO("Retransmitting (ack=" + sess.ack_context +
             " try=" + std::to_string(sess.ack_retries) + ")");
}

// F6: bezpieczne tworzenie boardu — Board ctor może rzucić.
static bool resetBoardSafe(Board& b) {
    try {
        b = Board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        return true;
    } catch (const std::exception& e) {
        LOG_ERR(std::string("Board reset failed: ") + e.what());
        return false;
    } catch (...) {
        LOG_ERR("Board reset failed: unknown");
        return false;
    }
}

static void startNewGame(Session& sess, AppResources& app, ClockState& cs,
                         BluetoothServer& bt, ApiClient& api,
                         const std::string& white, const std::string& black,
                         int minutes, int incSec) {
    sess.whiteName = white.empty() ? "White" : white;
    sess.blackName = black.empty() ? "Black" : black;
    sess.moveHistory.clear();
    if (!resetBoardSafe(sess.board)) {
        bt.send_line("ERR|board_init_failed");
        return;
    }
    sess.over  = false;
    sess.lastAcceptedSeq = -1;
    sess.lastAcceptedUci.clear();
    clear_ack(sess);

    if (minutes > 0) {
        sess.tcEnabled = true;
        sess.startMs   = (long long)minutes * 60 * 1000;
    } else {
        sess.tcEnabled = false;
        sess.startMs   = 0;
    }

    sess.setupMinutes   = std::max(1, minutes > 0 ? minutes : (int)START_MINUTES);
    sess.setupIncrement = std::max(0, incSec);
    app.setup_minutes   = sess.setupMinutes;
    app.setup_increment = sess.setupIncrement;
    uint32_t startClockMs = app.setup_minutes * 60 * 1000;
    uint32_t incMs        = app.setup_increment * 1000;
    init_clock(&cs, startClockMs, incMs);
    cs.state = STATE_RUNNING;
    app.mode = UI_MODE_GAME;

    if (sess.networked) {
        api.clearGameId();
        api.enqueueNewGame(sess.whiteName, sess.blackName,
                           sess.tcEnabled ? sess.startMs : 0);
        bt.send_line("OK|newgame|pending");
    } else {
        bt.send_line("OK|newgame|local");
    }
    LOG_INFO("New game: " + sess.whiteName + " vs " + sess.blackName +
             " tc=" + std::to_string(sess.startMs) + "ms inc=" +
             std::to_string(sess.setupIncrement) + "s");
}

static bool normalizeUci(std::string& uci) {
    if (uci.size() != 4 && uci.size() != 5) return false;
    if (uci.size() == 5) {
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(uci[4])));
        if (p != 'q' && p != 'r' && p != 'b' && p != 'n') return false;
        uci[4] = p;
    }
    return true;
}

// F6: tryMove opakowuje ruchy chess-library w try/catch — żaden wyjątek nie
// rozsadzi aplikacji.
static void tryMove(Session& sess, AppResources& /*app*/, ClockState& cs,
                    BluetoothServer& bt, ApiClient& api,
                    int seq, std::string uciStr) {
    if (sess.over) { bt.send_line("ERR|game_over"); return; }

    if (!normalizeUci(uciStr)) {
        bt.send_line("ERR|bad_format|" + uciStr);
        return;
    }

    if (seq >= 0 && seq == sess.lastAcceptedSeq) {
        bt.send_line("DUP|" + uciStr);
        return;
    }
    if (!sess.lastAcceptedUci.empty() && sess.lastAcceptedUci == uciStr) {
        bt.send_line("DUP|" + uciStr);
        return;
    }

    try {
        Movelist moves;
        movegen::legalmoves(moves, sess.board);
        if (moves.size() == 0) { bt.send_line("ERR|no_legal_moves"); return; }

        bool isWhiteTurn = (sess.board.sideToMove() == Color::WHITE);

        Move m;
        try { m = uci::uciToMove(sess.board, uciStr); }
        catch (...) { bt.send_line("ERR|bad_format|" + uciStr); return; }

        bool legal = false;
        for (auto mv : moves) if (uci::moveToUci(mv) == uciStr) { legal = true; break; }
        if (!legal) { bt.send_line("ERR|illegal_move|" + uciStr); return; }

        sess.board.makeMove(m);
        sess.moveHistory.push_back(uciStr);
        sess.lastAcceptedSeq = seq;
        sess.lastAcceptedUci = uciStr;

        ActiveSide side = isWhiteTurn ? ACTIVE_LEFT : ACTIVE_RIGHT;
        if (cs.state != STATE_RUNNING) cs.state = STATE_RUNNING;
        switch_side(&cs, side);

        long long timeLeftMs = isWhiteTurn ? cs.left.remaining_ms : cs.right.remaining_ms;
        std::string playerColor = isWhiteTurn ? "White" : "Black";
        if (sess.networked)
            api.enqueueMove(uciStr, playerColor, sess.tcEnabled ? timeLeftMs : 0);
        bt.send_line("MOVE_ACCEPTED|" + uciStr);

        Movelist next;
        movegen::legalmoves(next, sess.board);
        if (next.size() == 0) {
            std::string status, winner;
            if (sess.board.inCheck()) {
                status = "checkmate";
                winner = (sess.board.sideToMove() == Color::WHITE) ? "Black" : "White";
                cs.state = (winner == "White") ? STATE_FINISHED_LEFT_WIN : STATE_FINISHED_RIGHT_WIN;
                cs.finish_reason = "checkmate";
            } else {
                status = "stalemate"; winner = "Draw";
                cs.state = STATE_FINISHED_DRAW;
                cs.finish_reason = "stalemate";
            }
            sess.over = true;
            if (sess.networked) api.enqueueStatus(status, winner);
            send_with_ack(bt, sess, "game_over",
                          "GAME_OVER|" + winner + "|" + status);
            LOG_INFO("Game over: winner=" + winner + " status=" + status);
        }
    } catch (const std::exception& e) {
        LOG_ERR(std::string("tryMove exception: ") + e.what());
        bt.send_line("ERR|engine_exception");
    } catch (...) {
        LOG_ERR("tryMove unknown exception");
        bt.send_line("ERR|engine_exception");
    }
}

static void checkTimeout(Session& sess, ClockState& cs, BluetoothServer& bt,
                         ApiClient& api) {
    if (sess.over) return;
    if (cs.state != STATE_FINISHED_LEFT_WIN && cs.state != STATE_FINISHED_RIGHT_WIN)
        return;
    std::string winner = (cs.state == STATE_FINISHED_LEFT_WIN) ? "White" : "Black";
    sess.over = true;
    if (sess.networked) api.enqueueStatus("timeout", winner);
    send_with_ack(bt, sess, "game_over",
                  "GAME_OVER|" + winner + "|timeout");
    LOG_INFO("Timeout: winner=" + winner);
}

static void sendFullSync(BluetoothServer& bt, const Session& sess, const ClockState& cs) {
    bt.send_line("SYNC_BEGIN");
    {
        std::ostringstream os;
        os << "SYNC_GAME|" << sess.whiteName << "|" << sess.blackName
           << "|" << sess.setupMinutes << "|" << sess.setupIncrement;
        bt.send_line(os.str());
    }
    {
        std::ostringstream os;
        os << "SYNC_CLOCK|" << cs.left.remaining_ms << "|" << cs.right.remaining_ms
           << "|" << (cs.active == ACTIVE_LEFT ? "white" : "black")
           << "|" << (int)cs.state;
        bt.send_line(os.str());
    }
    {
        std::ostringstream os;
        os << "SYNC_HISTORY|" << sess.moveHistory.size();
        bt.send_line(os.str());
    }
    int idx = 0;
    for (const auto& mv : sess.moveHistory) {
        std::ostringstream os;
        os << "SYNC_MOVE|" << idx++ << "|" << mv;
        bt.send_line(os.str());
    }
    bt.send_line("SYNC_END");
    LOG_INFO("Sent SYNC snapshot (history=" +
             std::to_string(sess.moveHistory.size()) + " moves)");
}

static void handleBluetoothCommand(const std::string& line,
                                   Session& sess, AppResources& app,
                                   ClockState& cs, BluetoothServer& bt,
                                   ApiClient& api) {
    if (line == "__BT_CONNECTED__") {
        app.bt_status = "Telefon polaczony. Oczekiwanie na NEWGAME...";
        if (app.mode == UI_MODE_QR) app.mode = UI_MODE_SETUP;
        bt.send_line("HELLO|chess_clock_pi");
        if (sess.awaiting_ack) {
            sess.ack_last_sent_ms = SDL_GetTicks() - ACK_TIMEOUT_MS;
        }
        LOG_INFO("Bluetooth client connected");
        return;
    }
    if (line == "__BT_DISCONNECTED__") {
        app.bt_status = "Telefon rozlaczony. Czekam ponownie...";
        LOG_INFO("Bluetooth client disconnected");
        return;
    }

    auto parts = splitPipe(line);
    for (auto& p : parts) p = trim(p);
    if (parts.empty() || parts[0].empty()) return;
    const std::string& cmd = parts[0];

    if (cmd == "HELLO")  { bt.send_line("OK|hello"); return; }

    if (cmd == "ACK") {
        if (parts.size() >= 2 && sess.awaiting_ack && parts[1] == sess.ack_context) {
            LOG_INFO("Received ACK for context=" + parts[1]);
            clear_ack(sess);
        }
        return;
    }

    if (cmd == "SYNC_REQUEST") { sendFullSync(bt, sess, cs); return; }

    if (cmd == "PAUSE") {
        GameState before = cs.state;
        pause_resume_clock(&cs);
        if (cs.state != before)
            api.enqueueArbiter(cs.state == STATE_RUNNING ? "resume" : "pause", 0);
        bt.send_line("OK|pause");
        return;
    }
    if (cmd == "RESUME") {
        if (cs.state == STATE_PAUSED) {
            cs.state = STATE_RUNNING;
            api.enqueueArbiter("resume", 0);
        }
        bt.send_line("OK|resume");
        return;
    }
    if (cmd == "RESET") {
        uint32_t s = app.setup_minutes * 60 * 1000;
        uint32_t i = app.setup_increment * 1000;
        reset_clock(&cs, s, i);
        sess.over = false;
        sess.moveHistory.clear();
        sess.lastAcceptedSeq = -1;
        sess.lastAcceptedUci.clear();
        clear_ack(sess);
        resetBoardSafe(sess.board);
        api.enqueueArbiter("reset", 0);
        bt.send_line("OK|reset");
        return;
    }
    if (cmd == "ARBITER_STOP") {
        stop_by_arbiter(&cs);
        api.enqueueArbiter("arbiter_stop", 0);
        bt.send_line("OK|arbiter_stop");
        return;
    }
    if (cmd == "ARBITER_RESUME") {
        resume_by_arbiter(&cs);
        api.enqueueArbiter("arbiter_resume", 0);
        bt.send_line("OK|arbiter_resume");
        return;
    }
    if (cmd == "QUIT") { bt.send_line("OK|quit"); return; }

    if (cmd == "NEWGAME") {
        std::string w = parts.size() > 1 ? parts[1] : "White";
        std::string b = parts.size() > 2 ? parts[2] : "Black";
        int mins = parts.size() > 3 ? std::atoi(parts[3].c_str()) : START_MINUTES;
        int inc  = parts.size() > 4 ? std::atoi(parts[4].c_str()) : INCREMENT_SECONDS;
        startNewGame(sess, app, cs, bt, api, w, b, mins, inc);
        return;
    }

    if (cmd == "MOVE") {
        if (parts.size() < 2) { bt.send_line("ERR|move_requires_uci"); return; }
        int seq = -1;
        std::string uciStr;
        if (parts.size() >= 3 &&
            !parts[1].empty() &&
            std::all_of(parts[1].begin(), parts[1].end(),
                        [](unsigned char c){ return std::isdigit(c); })) {
            seq = std::atoi(parts[1].c_str());
            uciStr = parts[2];
        } else {
            uciStr = parts[1];
        }
        tryMove(sess, app, cs, bt, api, seq, uciStr);
        return;
    }

    if (cmd == "ERROR") {
        if (parts.size() < 2) { bt.send_line("ERR|who?"); return; }
        ActiveSide who = (parts[1] == "white" || parts[1] == "White") ? ACTIVE_LEFT : ACTIVE_RIGHT;
        player_error(&cs, who);
        api.enqueueArbiter(who == ACTIVE_LEFT ? "error_white" : "error_black", 0);
        bt.send_line("OK|error");
        return;
    }

    if (cmd == "BONUS") {
        if (parts.size() < 3) { bt.send_line("ERR|who?|ms?"); return; }
        ActiveSide who = (parts[1] == "white" || parts[1] == "White") ? ACTIVE_LEFT : ACTIVE_RIGHT;
        uint32_t ms = (uint32_t)std::atoi(parts[2].c_str());
        add_bonus_time(&cs, who, ms);
        api.enqueueArbiter(who == ACTIVE_LEFT ? "bonus_white" : "bonus_black", ms);
        bt.send_line("OK|bonus");
        return;
    }

    bt.send_line("ERR|unknown_command|" + cmd);
    LOG_WARN("Unknown BT command: " + cmd);
}

// ─── Main ───────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string serverHost;
    std::string serverIp;
    int         serverPort = 9090;
    int         btChannel  = DEFAULT_BT_CHAN;
    bool        networked  = false;
    std::string clockCode;
    std::string apiKey;
    std::string queueFile  = DEFAULT_QUEUE_FILE;
    std::string logFile    = DEFAULT_LOG_FILE;
};

static std::string jsonString(const std::string& j, const std::string& key) {
    auto kpos = j.find("\"" + key + "\"");
    if (kpos == std::string::npos) return "";
    auto colon = j.find(':', kpos);
    if (colon == std::string::npos) return "";
    auto q1 = j.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = j.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return j.substr(q1 + 1, q2 - q1 - 1);
}

static int jsonInt(const std::string& j, const std::string& key, int fallback) {
    auto kpos = j.find("\"" + key + "\"");
    if (kpos == std::string::npos) return fallback;
    auto colon = j.find(':', kpos);
    if (colon == std::string::npos) return fallback;
    size_t s = colon + 1;
    while (s < j.size() && (j[s] == ' ' || j[s] == '\t' || j[s] == '\n' || j[s] == '\r')) s++;
    if (s >= j.size() || !std::isdigit((unsigned char)j[s])) return fallback;
    try { return std::stoi(j.substr(s)); } catch (...) { return fallback; }
}

static void loadJsonConfig(AppConfig& c, const std::string& path = "clock.json") {
    std::ifstream f(path);
    if (!f.good()) return;
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    if (c.serverHost.empty()) {
        auto sv = jsonString(json, "server");
        if (!sv.empty()) {
            c.serverHost = sv;
            c.serverIp   = stripPort(sv);
            c.serverPort = portFromHost(sv, 9090);
            c.networked  = true;
        }
    }
    if (c.clockCode.empty()) {
        auto cc = jsonString(json, "clock_code");
        if (!cc.empty() && cc.find("XXXX") == std::string::npos) c.clockCode = cc;
    }
    if (c.apiKey.empty()) {
        auto pk = jsonString(json, "private_key");
        if (!pk.empty() && pk != "sk_...") c.apiKey = pk;
    }
    if (c.btChannel == DEFAULT_BT_CHAN) {
        int bt = jsonInt(json, "bt_channel", 0);
        if (bt > 0) c.btChannel = bt;
    }
    auto qf = jsonString(json, "queue_file");
    if (!qf.empty()) c.queueFile = qf;
    auto lf = jsonString(json, "log_file");
    if (!lf.empty()) c.logFile = lf;
}

static AppConfig readConfig(int argc, char** argv) {
    AppConfig c;
    loadJsonConfig(c);
    if (argc >= 2) {
        c.serverHost = argv[1];
        c.serverIp   = stripPort(c.serverHost);
        c.serverPort = portFromHost(c.serverHost, 9090);
        c.networked  = !c.serverIp.empty();
    }
    if (argc >= 3) c.btChannel  = std::atoi(argv[2]);
    if (argc >= 4) c.clockCode  = argv[3];
    if (argc >= 5) c.apiKey     = argv[4];
    if (c.btChannel <= 0) c.btChannel = DEFAULT_BT_CHAN;
    return c;
}

// ─── Watchdog (F11) ─────────────────────────────────────────────────────────
//
// Twardy bezpiecznik: jeśli wątek UI nie zaktualizuje "heartbeatu" przez ponad
// WATCHDOG_TIMEOUT_MS (5 s), watchdog wymusza wyjście (_Exit). To gwarantuje,
// że nawet gdy coś nieoczekiwanie zablokuje główny wątek (np. driver SDL,
// X server, błąd Mesy), użytkownik zawsze może odzyskać aplikację — wystarczy
// ją uruchomić ponownie. Sam watchdog nie używa mutexów ani SDL — pisze do
// stderr i wywołuje _Exit.

static std::atomic<uint32_t> g_main_heartbeat_ms{0};
static std::atomic<bool>     g_watchdog_running{false};
static const uint32_t        WATCHDOG_TIMEOUT_MS = 5000;

static void watchdog_thread_fn() {
    // Uwaga: ten wątek MUSI być prosty i nie polegać na żadnym zasobie aplikacji.
    while (g_watchdog_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!g_watchdog_running.load()) break;
        uint32_t hb = g_main_heartbeat_ms.load();
        if (hb == 0) continue;  // jeszcze nie wystartowała pętla UI
        uint32_t now = SDL_GetTicks();
        if (now - hb > WATCHDOG_TIMEOUT_MS) {
            std::fprintf(stderr,
                "[WATCHDOG] UI thread bez heartbeatu od %u ms — wymuszam exit(2)\n",
                now - hb);
            // Spróbuj zapisać minimalną informację do osobnego pliku (logger
            // mógł też utknąć).
            std::FILE* wf = std::fopen("chess-clock-watchdog.log", "a");
            if (wf) {
                time_t t = std::time(nullptr);
                std::tm tm{};
                localtime_r(&t, &tm);
                char tbuf[32];
                std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
                std::fprintf(wf, "%s WATCHDOG TRIGGERED stale=%u ms\n", tbuf, now - hb);
                std::fclose(wf);
            }
            std::_Exit(2);  // _Exit nie wywołuje destruktorów, ale to OK
        }
    }
}

int main(int argc, char** argv) {
    // Ignoruj SIGPIPE — i tak używamy MSG_NOSIGNAL, ale dla pewności.
    signal(SIGPIPE, SIG_IGN);

    AppConfig cfg = readConfig(argc, argv);

    Logger::instance().open(cfg.logFile);
    LOG_INFO("=== chess_pi start ===");
    LOG_INFO("server=" + cfg.serverHost + " bt_channel=" + std::to_string(cfg.btChannel) +
             " queue=" + cfg.queueFile + " log=" + cfg.logFile);

    AppResources app;
    if (!ui_init(&app)) { LOG_ERR("UI init failed"); Logger::instance().close(); return 1; }
    app.bt_channel = cfg.btChannel;

    if (cfg.networked && !cfg.clockCode.empty() && !cfg.apiKey.empty()) {
        std::printf("[API] Weryfikacja klucza %s na %s:%d...\n",
                    cfg.clockCode.c_str(), cfg.serverIp.c_str(), cfg.serverPort);
        bool valid = verifyApiKey(cfg.serverIp, cfg.serverPort,
                                  cfg.clockCode, cfg.apiKey);
        if (valid) {
            std::printf("[API] OK Klucz API poprawny\n");
            app.server_info = cfg.serverHost + "  ✓ API OK";
            LOG_INFO("API key verified");
        } else {
            std::fprintf(stderr, "[API] Bledna weryfikacja\n");
            app.server_info = cfg.serverHost + "  ✗ niepoprawny klucz";
            LOG_ERR("API key verification failed");
        }
    } else if (!cfg.serverHost.empty()) {
        app.server_info = cfg.serverHost;
    }

    ApiClient api;
    api.configure(cfg.networked, cfg.serverIp, cfg.serverPort,
                  cfg.clockCode, cfg.apiKey, cfg.queueFile);
    api.start();

    BluetoothServer bt;
    bool bt_ok = bt.start(cfg.btChannel);
    if (bt_ok) app.bt_mac = bt.mac();
    else       app.bt_status = "Blad: nie mozna uruchomic serwera Bluetooth (uruchom jako root?)";

    std::ostringstream payload;
    payload << "chessclock://" << app.bt_mac << "/" << app.bt_channel;
    if (cfg.networked) payload << "?server=" << cfg.serverHost;
    app.qr_payload = payload.str();
    app.qr_texture = make_qr_texture(app.renderer, app.qr_payload, 6, &app.qr_w, &app.qr_h);

    ClockState cs;
    init_clock(&cs, START_MINUTES * 60 * 1000, INCREMENT_SECONDS * 1000);

    Session sess;
    sess.networked = cfg.networked;
    sess.setupMinutes   = app.setup_minutes;
    sess.setupIncrement = app.setup_increment;
    if (!resetBoardSafe(sess.board)) {
        LOG_ERR("Initial board init failed — kontynuuję, NEWGAME odbuduje");
    }

    bool app_running = true;
    uint32_t prev_tick = SDL_GetTicks();
    uint32_t last_clock_push = 0;
    uint32_t last_frame_start = SDL_GetTicks();

    // F11: Start watchdoga DOPIERO po pełnej inicjalizacji UI/BT/API. Heartbeat
    // ustawiamy najpierw, by uniknąć fałszywego trafienia w pierwszej klatce.
    g_main_heartbeat_ms.store(SDL_GetTicks());
    g_watchdog_running.store(true);
    std::thread watchdog_thr(watchdog_thread_fn);
    LOG_INFO("Watchdog started (timeout=" + std::to_string(WATCHDOG_TIMEOUT_MS) + " ms)");

    while (app_running) {
        uint32_t frame_begin = SDL_GetTicks();

        // F11: heartbeat na samym początku klatki — watchdog wie, że żyjemy.
        g_main_heartbeat_ms.store(frame_begin);

        // F8: watchdog dla poprzedniej klatki (loguje, ale nie zabija).
        if (frame_begin - last_frame_start > FRAME_WATCHDOG_MS) {
            LOG_WARN("Frame took " +
                     std::to_string(frame_begin - last_frame_start) + " ms (slow frame)");
        }
        last_frame_start = frame_begin;

        ui_process_events(&cs, &app, &api, &app_running);

        // F5: limit liczby BT komend per frame, by nie zablokować UI floodem.
        std::string line;
        int processed = 0;
        while (processed < BT_PER_FRAME_MAX && bt.pop_line(line)) {
            handleBluetoothCommand(line, sess, app, cs, bt, api);
            processed++;
        }

        uint32_t now   = SDL_GetTicks();
        uint32_t delta = now - prev_tick;
        prev_tick      = now;
        update_clock(&cs, delta);
        checkTimeout(sess, cs, bt, api);
        tick_ack(bt, sess);

        if (bt.is_connected() && now - last_clock_push > 500) {
            last_clock_push = now;
            std::ostringstream os;
            os << "CLOCK|" << cs.left.remaining_ms << "|" << cs.right.remaining_ms
               << "|" << (cs.active == ACTIVE_LEFT ? "white" : "black")
               << "|" << (int)cs.state;
            bt.send_line(os.str());  // F1: nie blokuje (kolejka)
        }

        // Stan animowanego komunikatu końca gry — tuż przed renderem, by
        // pokazać/ukryć overlay zgodnie z aktualnym stanem zegara.
        update_overlay_state(app, cs);

        ui_render_frame(app.renderer, &app, &cs);

        // Adaptywny delay — jeśli klatka była wolna, nie dodawaj 33 ms.
        uint32_t frame_time = SDL_GetTicks() - frame_begin;
        uint32_t target = 1000 / FPS;
        if (frame_time < target) SDL_Delay(target - frame_time);
    }

    LOG_INFO("Shutting down...");
    g_watchdog_running.store(false);
    if (watchdog_thr.joinable()) watchdog_thr.join();
    bt.stop();
    api.stop();
    ui_cleanup(&app);
    LOG_INFO("=== chess_pi stop ===");
    Logger::instance().close();
    return 0;
}
