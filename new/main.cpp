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
//  Run:     ./chess_pi  [server_ip[:port]]  [bt_channel]  [clock_code]
//
//  Note (item #3): API key is no longer required. The clock identifies itself
//  with the clock_code header only. To bind the clock to a competition / club
//  session, an organiser generates a 6-digit pairing code on the web UI and
//  the operator types it on the clock — see /api/clock/pair endpoint.
//
// =============================================================================

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
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

// HTTPS — wszystkie wywołania do serwera Go (port 8443) idą przez TLS.
// Certyfikat serwera jest self-signed: nie weryfikujemy go po stronie klienta.
#include <openssl/ssl.h>
#include <openssl/err.h>

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
#include <memory>
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
static const int HTTP_CONNECT_TIMEOUT_MS = 3000;
static const int HTTP_IO_TIMEOUT_MS      = 5000;

static const size_t BT_RECV_BUFFER_MAX = 64 * 1024;
static const size_t BT_LINE_MAX        = 4 * 1024;
static const size_t BT_QUEUE_MAX       = 1024;
static const size_t BT_SEND_QUEUE_MAX  = 256;
static const int    BT_PER_FRAME_MAX   = 32;
static const int    BT_SEND_TIMEOUT_MS = 1500;

static const int    QUEUE_PERSIST_MIN_INTERVAL_MS = 250;
static const uint32_t FRAME_WATCHDOG_MS = 2000;

static const char* DEFAULT_QUEUE_FILE = "chess-clock-queue.dat";
static const char* DEFAULT_LOG_FILE   = "chess-clock.log";

// ─── Logger (asynchronicznie) ───────────────────────────────────────────────

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
           << "[" << level << "] " << msg << '\n';
        {
            std::lock_guard<std::mutex> g(mu_);
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
                cv_.wait_for(g, std::chrono::milliseconds(200),[&]{ return !running_.load() || !pending_.empty(); });
                if (!pending_.empty()) {
                    batch.swap(pending_);
                }
            }
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
    std::string finish_reason;
    std::string pending_uci;
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
    c->pending_uci.clear();
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

enum UIMode { UI_MODE_QR, UI_MODE_SETUP, UI_MODE_GAME, UI_MODE_HELP, UI_MODE_ARBITER, UI_MODE_PAIRING };

struct TextCacheKey {
    TTF_Font*   font;
    std::string text;
    uint32_t    color;
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

struct WinnerOverlay {
    bool        visible      = false;
    bool        dismissed    = false;
    uint32_t    start_ms     = 0;
    std::string winner_text;
    std::string reason_text;
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
    int           bt_peer_count = 0;          // ile tabletów podłączonych (0..MAX_PEERS)
    int           bt_peer_max   = 2;          // = BluetoothServer::MAX_PEERS
    std::string   server_info;
    TextCache     text_cache;
    WinnerOverlay overlay;
    // ── Pairing (item #3) — 6-cyfrowy kod wpisany przez operatora ────────────
    std::string   pair_input;          // bieżące cyfry: 0–6 znaków
    std::string   pair_status;         // komunikat statusu ("Sparowane: ...", "Błąd: ...")
    bool          pair_in_flight = false; // true gdy POST trwa, blokuje submit
    std::string   pair_session_label;  // co aktualnie zegar gra (np. "Zawody: Mistrzostwa")
    // ── Oferta kodu dla arbitra (wymaganie #2) ──────────────────────────────
    // Wypełniane co frame z ApiClient::offerView(). offer_ttl0 + offer_fetch_local_ms
    // dają płynne odliczanie bez polegania na zegarze ściennym Pi vs serwer.
    bool          offer_active   = false;
    std::string   offer_code;
    long long     offer_ttl0     = 0;     // sekundy do wygaśnięcia w chwili pobrania
    uint32_t      offer_fetch_local_ms = 0;
    long long     offer_server_now_seen = 0;
    bool          offer_claimed  = false;
    std::string   offer_target_kind;
    std::string   offer_target_name;
    long long     offer_table_no = 0;
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

    // Wymaganie #2 — panel kodu dla sędziego (prawy-górny róg). Niezaklejmowany:
    // duży 6-cyfrowy kod + odliczanie do wygaśnięcia (10 min). Zaklejmowany:
    // nazwa zawodów/meczu + numer stołu do weryfikacji.
    if (a->offer_active) {
        int ox = WINDOW_WIDTH - 400;
        int oy = 66;
        if (a->offer_claimed) {
            blit_text(r, a->text_cache, a->font_small, "PODLACZONO:",
                      SDL_Color{80, 200, 120, 255}, ox, oy); oy += 24;
            blit_text(r, a->text_cache, a->font_medium,
                      a->offer_target_name.c_str(), COLOR_FG, ox, oy); oy += 34;
            char tb[48]; std::snprintf(tb, sizeof(tb), "STOL %lld", a->offer_table_no);
            blit_text(r, a->text_cache, a->font_large, tb,
                      SDL_Color{80, 200, 120, 255}, ox, oy); oy += 46;
            blit_text(r, a->text_cache, a->font_small,
                      "Zweryfikuj zawody/mecz.", COLOR_FG, ox, oy);
        } else {
            blit_text(r, a->text_cache, a->font_small, "KOD DLA SEDZIEGO:",
                      COLOR_ACCENT, ox, oy); oy += 26;
            blit_text(r, a->text_cache, a->font_xlarge,
                      a->offer_code.c_str(), COLOR_FG, ox, oy); oy += 58;
            long long elapsed   = ((long long)SDL_GetTicks() -
                                   (long long)a->offer_fetch_local_ms) / 1000;
            long long remaining = a->offer_ttl0 - elapsed;
            if (remaining < 0) remaining = 0;
            char cd[48];
            std::snprintf(cd, sizeof(cd), "Wazny: %02d:%02d",
                          (int)(remaining / 60), (int)(remaining % 60));
            blit_text(r, a->text_cache, a->font_small, cd,
                      SDL_Color{200, 150, 80, 255}, ox, oy);
        }
    }

    blit_text(r, a->text_cache, a->font_small, a->bt_status.c_str(),
              SDL_Color{200, 150, 80, 255}, text_x, WINDOW_HEIGHT - 30);

    Button next_btn{{WINDOW_WIDTH - 300, WINDOW_HEIGHT - 90, 250, 60}, "PRZEJDZ DO GRY >", false};
    draw_colored_button(r, a->text_cache, a->font_medium, &next_btn, COLOR_ACCENT, COLOR_ACCENT, false);
}

static void draw_setup_screen(SDL_Renderer* r, AppResources* a) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);

    Button bt_btn{{20, 10, 60, 45}, "BT", false};
    draw_colored_button(r, a->text_cache, a->font_small, &bt_btn, COLOR_ACCENT, COLOR_ACCENT, false);

    Button exit_btn{{WINDOW_WIDTH - 130, 10, 110, 45}, "WYJDZ", false};
    draw_colored_button(r, a->text_cache, a->font_small, &exit_btn, COLOR_BUTTON_ERROR, COLOR_BUTTON_ERROR, false);

    blit_text(r, a->text_cache, a->font_medium, "ZEGAR SZACHOWY",
              COLOR_ACCENT, 100, 18);

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

    // START button — szary i nieaktywny dopóki nie podłączą się 2 tablety.
    bool ready = (a->bt_peer_count >= a->bt_peer_max);
    Button start_btn{{340, 130, 300, 70},
                     ready ? "ROZP. GRE" : "CZEKAM NA 2 TABLETY",
                     false};
    if (ready) {
        draw_button(r, a->text_cache, a->font_medium, &start_btn, false);
    } else {
        draw_colored_button(r, a->text_cache, a->font_small, &start_btn,
                            COLOR_INACTIVE, COLOR_INACTIVE, false);
    }

    // Status peerów — duża etykieta poniżej START.
    char peer_buf[80];
    if (a->bt_peer_count == 0) {
        std::snprintf(peer_buf, sizeof(peer_buf),
            "Brak tabletow. Wlacz Bluetooth i znajdz 'Chess-Clock-Pi'.");
    } else if (a->bt_peer_count < a->bt_peer_max) {
        std::snprintf(peer_buf, sizeof(peer_buf),
            "Oczekiwanie na tablety... (%d/%d)",
            a->bt_peer_count, a->bt_peer_max);
    } else {
        std::snprintf(peer_buf, sizeof(peer_buf),
            "Gotowe — oba tablety podlaczone (%d/%d).",
            a->bt_peer_count, a->bt_peer_max);
    }
    int pw = 0, ph = 0;
    SDL_Color peer_col = ready ? COLOR_ACCENT : SDL_Color{200, 150, 80, 255};
    measure_text(r, a->text_cache, a->font_small, peer_buf, peer_col, &pw, &ph);
    blit_text(r, a->text_cache, a->font_small, peer_buf, peer_col,
              (WINDOW_WIDTH - pw)/2, 215);

    int tw = 0, th = 0;
    const char* hh = "Kliknij +/- aby ustawic, potem START  (telefon moze wyslac NEWGAME)";
    measure_text(r, a->text_cache, a->font_small, hh,
                 SDL_Color{150,150,150,255}, &tw, &th);
    blit_text(r, a->text_cache, a->font_small, hh,
              SDL_Color{150,150,150,255},
              (WINDOW_WIDTH - tw)/2, WINDOW_HEIGHT - 45);
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
    } else if (!c->pending_uci.empty() && c->state == STATE_RUNNING) {
        const char* big = "ZATWIERDZ RUCH";
        int tw = 0, th = 0;
        measure_text(r, a->text_cache, a->font_xlarge, big, SDL_Color{255, 200, 0, 255}, &tw, &th);
        blit_text(r, a->text_cache, a->font_xlarge, big, SDL_Color{255, 200, 0, 255}, (WINDOW_WIDTH - tw) / 2, 100);

        std::string rm = "Propozycja: " + c->pending_uci;
        measure_text(r, a->text_cache, a->font_medium, rm.c_str(), SDL_Color{220, 220, 220, 255}, &tw, &th);
        blit_text(r, a->text_cache, a->font_medium, rm.c_str(), SDL_Color{220, 220, 220, 255}, (WINDOW_WIDTH - tw) / 2, 200);
    }

    Button bt_btn{{20, 10, 60, 45}, "BT", false};
    draw_colored_button(r, a->text_cache, a->font_small, &bt_btn, COLOR_ACCENT, COLOR_ACCENT, false);

    Button menu_btn{{WINDOW_WIDTH - 130, 10, 110, 45}, "MENU", false};
    draw_colored_button(r, a->text_cache, a->font_small, &menu_btn, COLOR_INACTIVE, COLOR_INACTIVE, false);

    SDL_SetRenderDrawColor(r, COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, COLOR_ACCENT.a);
    SDL_Rect help_box{1230, 365, 50, 35};
    SDL_RenderFillRect(r, &help_box);
    int tw = 0, th = 0;
    measure_text(r, a->text_cache, a->font_small, "?", COLOR_BG, &tw, &th);
    blit_text(r, a->text_cache, a->font_small, "?", COLOR_BG,
              1230 + (50 - tw)/2, 365 + (35 - th)/2);
}

static void draw_help_screen(SDL_Renderer* r, AppResources* a) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);

    blit_text(r, a->text_cache, a->font_medium, "POMOC - GRA DOTYKOWA", COLOR_ACCENT, 20, 20);

    const char* pl[] = {
        "1. Dotknij swojej polowy ekranu aby potwierdzic / zakonczyc ruch.",
        "2. Przyciski na dole: PAUZA, RESET, oraz ARBITER (kary, bonusy, stop).",
        "3. Przyciski na gorze: BT (Powrot do parowania), MENU (Ekran ustawien).",
        "4. Klawiatura PC: Spacja (Pauza), R (Reset), A (Stop Arbiter), Q (Wznow)",
        "5. Skroty Arbitra PC: 1/2 (Blad gracza), 3/4 (+2 min dla gracza), Ctrl+Q (Wyjscie)"
    };
    int y = 70;
    for (int i = 0; i < 5; i++) {
        blit_text(r, a->text_cache, a->font_small, pl[i], COLOR_FG, 30, y);
        y += 40;
    }

    Button close_btn{{(WINDOW_WIDTH - 300)/2, WINDOW_HEIGHT - 80, 300, 60}, "ZROZUMIANO / ZAMKNIJ", false};
    draw_colored_button(r, a->text_cache, a->font_medium, &close_btn, COLOR_ACCENT, COLOR_ACCENT, false);
}

static void draw_arbiter_menu(SDL_Renderer* r, AppResources* a, const ClockState* c) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);
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
}

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
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    a->renderer = SDL_CreateRenderer(a->window, -1, SDL_RENDERER_ACCELERATED);
    if (!a->renderer) {
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
class BluetoothServer;
struct Session;
static void reportArbiter(ApiClient* api, const std::string& action, long long valueMs);
static void tryMove(Session& sess, AppResources& app, ClockState& cs,
                    BluetoothServer& bt, ApiClient& api,
                    int src_peer_id, int seq, std::string uciStr,
                    bool is_commit = false);

// ─── Animated game-end overlay ──────────────────────────────────────────────

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
    SDL_SetTextureAlphaMod(t, 255);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

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
    float te = 1.0f - std::pow(1.0f - t, 3.0f);
    Uint8 fade_alpha = (Uint8)(255.0f * te);

    float pulse = 1.0f;
    if (t >= 1.0f) {
        float pt = (float)(elapsed - ANIM_MS) / 1000.0f;
        pulse = 1.0f + 0.015f * std::sin(pt * 2.0f * 3.14159265f * 0.7f);
    }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(190.0f * te));
    SDL_Rect full{0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
    SDL_RenderFillRect(r, &full);

    OverlayLayout ol;
    float scale = (0.7f + 0.3f * te) * pulse;
    int pw = (int)((float)OverlayLayout::PANEL_W * scale);
    int ph = (int)((float)OverlayLayout::PANEL_H * scale);
    int px = (WINDOW_WIDTH  - pw) / 2;
    int py = (WINDOW_HEIGHT - ph) / 2;

    SDL_Color panel_bg, text_col;
    if (a->overlay.winner_text == "REMIS") {
        panel_bg = {70, 70, 95, fade_alpha};
        text_col = {245, 245, 250, 255};
    } else if (a->overlay.shown_for == STATE_FINISHED_LEFT_WIN) {
        panel_bg = {235, 235, 240, fade_alpha};
        text_col = {25, 25, 35, 255};
    } else {
        panel_bg = {28, 30, 42, fade_alpha};
        text_col = {245, 245, 250, 255};
    }

    SDL_Rect panel{px, py, pw, ph};
    draw_filled_rounded_rect(r, panel, 16, panel_bg);

    SDL_Color accent = (a->overlay.winner_text == "REMIS")
        ? SDL_Color{120, 180, 220, fade_alpha}
        : COLOR_ACCENT;
    accent.a = fade_alpha;
    SDL_Rect strip{px + 20, py + ph - 6, pw - 40, 4};
    SDL_SetRenderDrawColor(r, accent.r, accent.g, accent.b, accent.a);
    SDL_RenderFillRect(r, &strip);

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

    SDL_Rect close = ol.close_rect();
    if (t >= 0.6f) {
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

static std::string overlay_reason_pl(const std::string& reason) {
    if (reason == "timeout")    return "Przekroczenie czasu";
    if (reason == "errors")     return "Dwa bledy zawodnika";
    if (reason == "checkmate")  return "Mat";
    if (reason == "stalemate")  return "Pat - remis";
    return "";
}

static void update_overlay_state(AppResources& app, const ClockState& cs) {
    bool finished = cs.state == STATE_FINISHED_LEFT_WIN ||
                    cs.state == STATE_FINISHED_RIGHT_WIN ||
                    cs.state == STATE_FINISHED_DRAW;

    if (!finished) {
        app.overlay.visible = false;
        app.overlay.dismissed = false;
        return;
    }

    if (app.overlay.visible || app.overlay.dismissed) return;

    app.overlay.visible   = true;
    app.overlay.dismissed = false;
    app.overlay.start_ms  = SDL_GetTicks();
    app.overlay.shown_for = cs.state;
    if      (cs.state == STATE_FINISHED_LEFT_WIN)  app.overlay.winner_text = "BIALY WYGRAL";
    else if (cs.state == STATE_FINISHED_RIGHT_WIN) app.overlay.winner_text = "CZARNY WYGRAL";
    else                                            app.overlay.winner_text = "REMIS";
    app.overlay.reason_text = overlay_reason_pl(cs.finish_reason);
}

// ── Pairing screen (item #3) — wpisanie 6-cyfrowego kodu parowania ─────────
// Wyświetla wpisane cyfry, numeryczny keypad 3x4 (touch), status komunikatu.
// Cyfry można też pisać z klawiatury (0-9), BACKSPACE = wstecz, ENTER = wyślij.
static void draw_pairing_screen(SDL_Renderer* r, AppResources* a) {
    SDL_SetRenderDrawColor(r, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(r);

    blit_text(r, a->text_cache, a->font_medium, "SPARUJ ZEGAR Z SESJA",
              COLOR_ACCENT, 40, 20);
    blit_text(r, a->text_cache, a->font_small,
              "Wpisz 6-cyfrowy kod z panelu organizatora (web).",
              COLOR_FG, 40, 65);

    // Pole z cyframi — 6 dużych kropek/cyfr.
    SDL_Rect codeBox{ 40, 110, 600, 110 };
    draw_filled_rounded_rect(r, codeBox, 12, COLOR_INACTIVE);

    std::string disp;
    for (int i = 0; i < 6; i++) {
        if ((int)a->pair_input.size() > i) { disp += a->pair_input[i]; disp += ' '; }
        else                                { disp += "_ "; }
    }
    blit_text(r, a->text_cache, a->font_xlarge, disp.c_str(),
              COLOR_FG, codeBox.x + 30, codeBox.y + 20);

    // Status.
    if (a->pair_in_flight) {
        blit_text(r, a->text_cache, a->font_small, "Wysylanie...",
                  SDL_Color{255, 213, 107, 255}, 40, 240);
    } else if (!a->pair_status.empty()) {
        bool ok = (a->pair_status.rfind("OK", 0) == 0);
        SDL_Color col = ok ? SDL_Color{107, 230, 117, 255} : SDL_Color{255, 107, 107, 255};
        blit_text(r, a->text_cache, a->font_small, a->pair_status.c_str(),
                  col, 40, 240);
    }
    if (!a->pair_session_label.empty()) {
        blit_text(r, a->text_cache, a->font_small,
                  ("Aktualnie: " + a->pair_session_label).c_str(),
                  SDL_Color{184, 197, 224, 255}, 40, 275);
    }

    // Keypad 3x4 (1 2 3 / 4 5 6 / 7 8 9 / ← 0 OK)
    const char* labels[12] = { "1","2","3","4","5","6","7","8","9","<","0","OK" };
    int kx0 = 700, ky0 = 110, bw = 110, bh = 90, gap = 10;
    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        SDL_Rect btn{ kx0 + col * (bw + gap), ky0 + row * (bh + gap), bw, bh };
        SDL_Color bg = (i == 11) ? COLOR_ACCENT : COLOR_ACTIVE;
        draw_filled_rounded_rect(r, btn, 10, bg);
        int tw = 0, th = 0;
        measure_text(r, a->text_cache, a->font_large, labels[i], COLOR_FG, &tw, &th);
        blit_text(r, a->text_cache, a->font_large, labels[i], COLOR_FG,
                  btn.x + (bw - tw) / 2, btn.y + (bh - th) / 2);
    }

    // Powrót.
    SDL_Rect back{ 40, 540, 220, 60 };
    draw_filled_rounded_rect(r, back, 10, COLOR_INACTIVE);
    blit_text(r, a->text_cache, a->font_small, "Powrot (ESC)",
              COLOR_FG, back.x + 30, back.y + 20);
}

static void ui_render_frame(SDL_Renderer* r, AppResources* a, const ClockState* c) {
    switch (a->mode) {
        case UI_MODE_QR:      draw_qr_screen(r, a);          break;
        case UI_MODE_SETUP:   draw_setup_screen(r, a);       break;
        case UI_MODE_HELP:    draw_help_screen(r, a);        break;
        case UI_MODE_ARBITER: draw_arbiter_menu(r, a, c);    break;
        case UI_MODE_PAIRING: draw_pairing_screen(r, a);     break;
        case UI_MODE_GAME:
        default:              draw_game_screen(r, a, c);     break;
    }
    if (a->mode == UI_MODE_GAME || a->mode == UI_MODE_ARBITER) {
        draw_winner_overlay(r, a);
    }
    SDL_RenderPresent(r);
}

// ─── HTTPS relay ────────────────────────────────────────────────────────────
//  Wszystkie wywołania do serwera Go (8443) idą przez TLS. Serwer ma
//  self-signed cert, więc weryfikacja jest wyłączona — ufamy ścieżce
//  LAN-only (port 8443 ma być wystawiony tylko na sieć lokalną).

static SSL_CTX* g_ssl_ctx = nullptr;
static std::once_flag g_ssl_once;

static void initOpenSSL_once() {
    std::call_once(g_ssl_once, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        const SSL_METHOD* m = TLS_client_method();
        g_ssl_ctx = SSL_CTX_new(m);
        if (!g_ssl_ctx) {
            LOG_ERR("SSL_CTX_new failed");
            return;
        }
        SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
        // Self-signed cert: do not enforce verification.
        SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, nullptr);
    });
}

// tlsWrap wykonuje handshake TLS na już-połączonym sockecie. Zwraca SSL*
// (do zamknięcia przez SSL_free); nullptr przy błędzie. Socket musi być
// w trybie blokującym (po `connect_with_timeout` jest, bo flagi są
// przywracane do oryginalnych).
static SSL* tlsWrap(int sock) {
    initOpenSSL_once();
    if (!g_ssl_ctx) return nullptr;
    SSL* ssl = SSL_new(g_ssl_ctx);
    if (!ssl) return nullptr;
    if (SSL_set_fd(ssl, sock) != 1) {
        SSL_free(ssl);
        return nullptr;
    }
    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        LOG_WARN(std::string("SSL_connect failed: ") + buf);
        SSL_free(ssl);
        return nullptr;
    }
    return ssl;
}

static void tlsClose(SSL* ssl) {
    if (!ssl) return;
    SSL_shutdown(ssl);
    SSL_free(ssl);
}

static std::string stripPort(const std::string& ip) {
    auto p = ip.find(':');
    return (p == std::string::npos) ? ip : ip.substr(0, p);
}

static int portFromHost(const std::string& host, int fallback) {
    auto p = host.find(':');
    if (p == std::string::npos) return fallback;
    try { return std::stoi(host.substr(p + 1)); } catch (...) { return fallback; }
}

static int connect_with_timeout(int sock, const sockaddr* addr, socklen_t alen,
                                int timeout_ms, std::atomic<bool>* abort_flag) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = ::connect(sock, addr, alen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
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

// Wspólny szkielet: utwórz TCP, połącz, zawiń w TLS, wyślij `request`,
// zbierz odpowiedź. Zwraca pełny tekst odpowiedzi HTTP (status+headery+body)
// albo "" przy błędzie.
static std::string httpsRequest(const std::string& ip, int port,
                                const std::string& request,
                                const std::string& logLabel,
                                std::atomic<bool>* abort_flag) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_ERR(logLabel + " socket() failed: " + std::strerror(errno));
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
        LOG_ERR(logLabel + ": bad IP " + ip);
        return "";
    }

    if (connect_with_timeout(sock, (sockaddr*)&addr, sizeof(addr),
                             HTTP_CONNECT_TIMEOUT_MS, abort_flag) < 0) {
        LOG_WARN(logLabel + " connect() failed for " + ip + ":" + std::to_string(port) +
                 " errno=" + std::to_string(errno));
        close(sock);
        return "";
    }

    SSL* ssl = tlsWrap(sock);
    if (!ssl) {
        close(sock);
        return "";
    }

    // SSL_write może zwrócić mniej niż żądano — pętla aż wszystko pójdzie.
    size_t off = 0;
    while (off < request.size()) {
        int wn = SSL_write(ssl, request.data() + off, (int)(request.size() - off));
        if (wn <= 0) {
            LOG_WARN(logLabel + " SSL_write failed");
            tlsClose(ssl);
            close(sock);
            return "";
        }
        off += (size_t)wn;
    }

    std::string response;
    char buf[4096];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf) - 1)) > 0) {
        if (abort_flag && abort_flag->load()) break;
        buf[n] = '\0';
        response.append(buf, (size_t)n);
        if (response.size() > 256 * 1024) break;
    }
    tlsClose(ssl);
    close(sock);
    return response;
}

static std::string httpPost(const std::string& ip, int port,
                            const std::string& path, const std::string& body,
                            const std::string& clockCode,
                            const std::string& apiKey,
                            std::atomic<bool>* abort_flag) {
    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << ip << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    if (!clockCode.empty()) req << "X-Clock-Code: " << clockCode << "\r\n";
    if (!apiKey.empty())    req << "X-API-Key: "    << apiKey    << "\r\n";
    req << "Connection: close\r\n\r\n" << body;
    return httpsRequest(ip, port, req.str(), "httpPost " + path, abort_flag);
}

static std::string httpGet(const std::string& ip, int port,
                           const std::string& path,
                           const std::string& clockCode,
                           const std::string& apiKey,
                           std::atomic<bool>* abort_flag) {
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << ip << ":" << port << "\r\n";
    if (!clockCode.empty()) req << "X-Clock-Code: " << clockCode << "\r\n";
    if (!apiKey.empty())    req << "X-API-Key: "    << apiKey    << "\r\n";
    req << "Connection: close\r\n\r\n";
    return httpsRequest(ip, port, req.str(), "httpGet " + path, abort_flag);
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

// ── Pairing (item #3) ───────────────────────────────────────────────────────
// Wysyła 6-cyfrowy kod parowania do serwera. Zwraca: { ok, label }
struct PairResult { bool ok; std::string error; std::string label; std::string target; };
static PairResult apiPair(const std::string& ip, int port,
                          const std::string& clockCode, const std::string& code) {
    PairResult r{false, "", "", ""};
    if (ip.empty() || clockCode.empty() || code.size() != 6) {
        r.error = "bad_input";
        return r;
    }
    std::atomic<bool> abort_flag{false};
    std::string body = std::string("{\"code\":\"") + code + "\"}";
    std::string resp = httpPost(ip, port, "/api/clock/pair", body, clockCode, "", &abort_flag);
    auto bodyPos = resp.find("\r\n\r\n");
    std::string payload = (bodyPos != std::string::npos) ? resp.substr(bodyPos + 4) : resp;
    if (payload.find("\"ok\":true") == std::string::npos &&
        payload.find("\"ok\": true") == std::string::npos) {
        // Wyciągnij "error":"..." jeśli jest.
        auto ep = payload.find("\"error\"");
        if (ep != std::string::npos) {
            auto q1 = payload.find('"', payload.find(':', ep) + 1);
            auto q2 = payload.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                r.error = payload.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        if (r.error.empty()) r.error = "pair_failed";
        return r;
    }
    r.ok = true;
    // Wyciągnij label.
    auto lp = payload.find("\"label\"");
    if (lp != std::string::npos) {
        auto q1 = payload.find('"', payload.find(':', lp) + 1);
        auto q2 = payload.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) {
            r.label = payload.substr(q1 + 1, q2 - q1 - 1);
        }
    }
    auto tp = payload.find("\"target_kind\"");
    if (tp != std::string::npos) {
        auto q1 = payload.find('"', payload.find(':', tp) + 1);
        auto q2 = payload.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) {
            r.target = payload.substr(q1 + 1, q2 - q1 - 1);
        }
    }
    return r;
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

// ─── JSON micro-parsery (wymaganie #2/#3) ───────────────────────────────────
// jsonString/jsonInt z dołu pliku są zdefiniowane PO ApiClient, więc tutaj mamy
// własne, lokalne wersje do parsowania odpowiedzi serwera w wątkach ApiClient.

static std::string objStr(const std::string& j, const std::string& key) {
    auto k = j.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    auto colon = j.find(':', k);
    if (colon == std::string::npos) return "";
    auto q1 = j.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = j.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return j.substr(q1 + 1, q2 - q1 - 1);
}

static long long objInt(const std::string& j, const std::string& key, long long fb) {
    auto k = j.find("\"" + key + "\"");
    if (k == std::string::npos) return fb;
    auto colon = j.find(':', k);
    if (colon == std::string::npos) return fb;
    size_t s = colon + 1;
    while (s < j.size() && (j[s]==' '||j[s]=='\t'||j[s]=='\n'||j[s]=='\r')) s++;
    bool neg = false;
    if (s < j.size() && j[s] == '-') { neg = true; s++; }
    if (s >= j.size() || !std::isdigit((unsigned char)j[s])) return fb;
    long long v = 0;
    while (s < j.size() && std::isdigit((unsigned char)j[s])) { v = v*10 + (j[s]-'0'); s++; }
    return neg ? -v : v;
}

static bool objBool(const std::string& j, const std::string& key) {
    auto k = j.find("\"" + key + "\"");
    if (k == std::string::npos) return false;
    auto colon = j.find(':', k);
    if (colon == std::string::npos) return false;
    size_t s = colon + 1;
    while (s < j.size() && (j[s]==' '||j[s]=='\t')) s++;
    return j.compare(s, 4, "true") == 0;
}

// ServerClockCmd — komenda od serwera (sędzia w panelu arbitra). Pobierana
// przez /clock/commands i stosowana lokalnie do ClockState + rozsyłana do tabletów.
struct ServerClockCmd {
    long long   id      = 0;
    long long   game_id = 0;
    std::string action;   // ADD | SUBTRACT | STOP | PLAY | FINISH | SET
    std::string side;     // White | Black | ""
    int         amount_ms = 0;
    std::string reason;   // dla FINISH: White|Black|Draw
};

// parseClockCommands wyłuskuje tablicę "commands":[ {…}, … ] z odpowiedzi serwera.
static std::vector<ServerClockCmd> parseClockCommands(const std::string& body) {
    std::vector<ServerClockCmd> out;
    auto cpos = body.find("\"commands\"");
    if (cpos == std::string::npos) return out;
    auto lb = body.find('[', cpos);
    if (lb == std::string::npos) return out;
    int depth = 0;
    size_t objStart = std::string::npos;
    for (size_t i = lb + 1; i < body.size(); ++i) {
        char c = body[i];
        if (c == '{') { if (depth == 0) objStart = i; depth++; }
        else if (c == '}') {
            if (depth > 0) depth--;
            if (depth == 0 && objStart != std::string::npos) {
                std::string obj = body.substr(objStart, i - objStart + 1);
                ServerClockCmd cmd;
                cmd.id        = objInt(obj, "id", 0);
                cmd.game_id   = objInt(obj, "game_id", 0);
                cmd.action    = objStr(obj, "action");
                cmd.side      = objStr(obj, "side");
                cmd.amount_ms = (int)objInt(obj, "amount_ms", 0);
                cmd.reason    = objStr(obj, "reason");
                if (!cmd.action.empty()) out.push_back(cmd);
                objStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

static std::vector<ServerClockCmd> apiFetchCommands(const std::string& ip, int port,
        const std::string& clockCode, long long since,
        std::atomic<bool>* abort_flag, long long* maxId) {
    std::string path = "/clock/commands?since=" + std::to_string(since);
    std::string resp = httpGet(ip, port, path, clockCode, "", abort_flag);
    auto bp = resp.find("\r\n\r\n");
    std::string body = (bp != std::string::npos) ? resp.substr(bp + 4) : resp;
    auto cmds = parseClockCommands(body);
    if (maxId) for (auto& c : cmds) if (c.id > *maxId) *maxId = c.id;
    return cmds;
}

// Oferta kodu dla arbitra (wymaganie #2).
struct OfferInfo {
    bool        ok = false;
    std::string code;
    long long   expires_at = 0;
    long long   server_now = 0;
    std::string error;
};

static OfferInfo apiOfferCreate(const std::string& ip, int port,
        const std::string& clockCode, std::atomic<bool>* abort_flag) {
    OfferInfo r;
    std::string resp = httpPost(ip, port, "/api/clock/offer", "{}", clockCode, "", abort_flag);
    auto bp = resp.find("\r\n\r\n");
    std::string body = (bp != std::string::npos) ? resp.substr(bp + 4) : resp;
    if (!objBool(body, "ok")) {
        r.error = objStr(body, "error");
        if (r.error.empty()) r.error = "offer_failed";
        return r;
    }
    r.ok = true;
    r.code       = objStr(body, "code");
    r.expires_at = objInt(body, "expires_at", 0);
    r.server_now = objInt(body, "server_now", 0);
    return r;
}

struct OfferStatusInfo {
    bool        ok = false;
    bool        has_offer = false;
    bool        claimed = false;
    bool        expired = false;
    std::string code;
    long long   expires_at = 0;
    long long   server_now = 0;
    std::string target_kind;
    std::string target_name;
    long long   table_no = 0;
};

static OfferStatusInfo apiOfferStatus(const std::string& ip, int port,
        const std::string& clockCode, std::atomic<bool>* abort_flag) {
    OfferStatusInfo r;
    std::string resp = httpGet(ip, port, "/api/clock/offer/status", clockCode, "", abort_flag);
    auto bp = resp.find("\r\n\r\n");
    std::string body = (bp != std::string::npos) ? resp.substr(bp + 4) : resp;
    if (!objBool(body, "ok")) return r;
    r.ok         = true;
    r.has_offer  = objBool(body, "has_offer");
    r.claimed    = objBool(body, "claimed");
    r.expired    = objBool(body, "expired");
    r.code       = objStr(body, "code");
    r.expires_at = objInt(body, "expires_at", 0);
    r.server_now = objInt(body, "server_now", 0);
    if (r.claimed) {
        r.target_kind = objStr(body, "target_kind");
        r.target_name = objStr(body, "target_name");
        r.table_no    = objInt(body, "table_no", 0);
    }
    return r;
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
        persist_thr_ = std::thread(&ApiClient::persistLoop, this);
        hb_thr_ = std::thread(&ApiClient::heartbeatLoop, this);
        offer_thr_ = std::thread(&ApiClient::offerLoop, this);   // wymaganie #2
        cmd_thr_   = std::thread(&ApiClient::commandLoop, this); // wymaganie #3
        LOG_INFO("ApiClient started (queue size=" + std::to_string(queue_.size()) +
                 ", heartbeat=" + std::to_string(HB_INTERVAL_MS) + "ms)");
    }

    void stop() {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> g(qmu_);
            running_ = false;
        }
        abort_flag_.store(true);
        qcv_.notify_all();
        persist_cv_.notify_all();
        if (thr_.joinable()) thr_.join();
        if (persist_thr_.joinable()) persist_thr_.join();
        if (hb_thr_.joinable()) hb_thr_.join();
        if (offer_thr_.joinable()) offer_thr_.join();
        if (cmd_thr_.joinable()) cmd_thr_.join();
        persistQueueNow();
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

    // item #3 — synchroniczne wywołanie pairingu. Wykonywane w wątku UI
    // (jednorazowa akcja na żądanie operatora), nie ma sensu kolejkować.
    PairResult pairWithCode(const std::string& code) {
        if (!networked_) return PairResult{false, "no_network", "", ""};
        return apiPair(ip_, port_, clockCode_, code);
    }

    // ── Heartbeat (live clock push) ────────────────────────────────────────
    // Nadpisuje cache najnowszych wartości. Wątek hbLoop_ co ~HB_INTERVAL_MS
    // wysyła POST /api/clock/heartbeat ze świeżymi liczbami. Brak retry, brak
    // persistencji — kolejny pakiet i tak będzie świeższy.
    void enqueueHeartbeat(long long whiteMs, long long blackMs) {
        if (!networked_) return;
        std::lock_guard<std::mutex> g(hb_mu_);
        hb_white_ms_  = whiteMs;
        hb_black_ms_  = blackMs;
        hb_has_data_  = true;
        // Nie notyfikujemy CV — wątek i tak wysyła co HB_INTERVAL_MS
        // (wybudzanie częściej tylko marnuje CPU; ostatnie wartości i tak
        // będą wysłane w następnym cyklu).
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

    // ── Oferta kodu dla arbitra (wymaganie #2) — snapshot do UI ──────────────
    OfferStatusInfo offerView() {
        std::lock_guard<std::mutex> g(offer_mu_);
        return offer_;
    }

    // ── Komendy od serwera (wymaganie #3) — drenowane przez główną pętlę ─────
    bool popServerCommand(ServerClockCmd& out) {
        std::lock_guard<std::mutex> g(srv_cmd_mu_);
        if (srv_cmd_queue_.empty()) return false;
        out = srv_cmd_queue_.front();
        srv_cmd_queue_.pop_front();
        return true;
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

    // ── Heartbeat state ────────────────────────────────────────────────────
    // ~co HB_INTERVAL_MS hbLoop_ pcha aktualny stan zegara na serwer.
    // Lekki POST (krótka wartość, krótki timeout), bez kolejki.
    static constexpr int HB_INTERVAL_MS = 500;
    std::thread             hb_thr_;
    std::mutex              hb_mu_;
    long long               hb_white_ms_ = 0;
    long long               hb_black_ms_ = 0;
    bool                    hb_has_data_ = false;

    void heartbeatLoop() {
        using namespace std::chrono;
        while (running_.load()) {
            // Czekaj HB_INTERVAL_MS, ale obudź się natychmiast jeśli running_=false
            // (stop() ustawia abort_flag_; sprawdzamy w pętli).
            for (int slept = 0; slept < HB_INTERVAL_MS && running_.load(); slept += 50) {
                std::this_thread::sleep_for(milliseconds(50));
            }
            if (!running_.load()) return;

            long long w = 0, b = 0;
            bool have = false;
            {
                std::lock_guard<std::mutex> g(hb_mu_);
                w = hb_white_ms_;
                b = hb_black_ms_;
                have = hb_has_data_;
            }
            if (!have) continue;

            std::string gid = gameId();
            if (gid.empty()) continue; // partia jeszcze nieutworzona na serwerze

            std::ostringstream body;
            body << "{\"game_id\":" << gid
                 << ",\"white_ms\":" << w
                 << ",\"black_ms\":" << b << "}";
            // Fire-and-forget — wynik ignorujemy. Krótki timeout (httpPost
            // używa abort_flag_ + ma własny timeout per OpenSSL).
            (void)httpPost(ip_, port_, "/api/clock/heartbeat", body.str(),
                           clockCode_, apiKey_, &abort_flag_);
        }
    }

    // ── Wymaganie #2: pętla oferty kodu dla arbitra ──────────────────────────
    // Na starcie generuje 6-cyfrowy kod (POST /api/clock/offer), potem co ~3 s
    // sprawdza status (GET /api/clock/offer/status). Gdy kod wygaśnie nie będąc
    // zaklejmowanym — generuje nowy. Gdy zostanie zaklejmowany — zapamiętuje
    // cel + numer stołu (do weryfikacji na ekranie).
    void offerLoop() {
        if (!networked_) return;
        auto nap = [&](int ms) {
            for (int s = 0; s < ms && running_.load(); s += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        };
        nap(800);
        bool need_new = true;
        while (running_.load()) {
            if (need_new) {
                OfferInfo oi = apiOfferCreate(ip_, port_, clockCode_, &abort_flag_);
                if (oi.ok) {
                    std::lock_guard<std::mutex> g(offer_mu_);
                    offer_ = OfferStatusInfo{};
                    offer_.ok = true; offer_.has_offer = true;
                    offer_.code       = oi.code;
                    offer_.expires_at = oi.expires_at;
                    offer_.server_now = oi.server_now;
                    need_new = false;
                    LOG_INFO("Offer code = " + oi.code + " (kod dla sedziego)");
                } else {
                    nap(3000);
                    continue;
                }
            }
            nap(3000);
            if (!running_.load()) break;
            OfferStatusInfo st = apiOfferStatus(ip_, port_, clockCode_, &abort_flag_);
            if (st.ok && st.has_offer) {
                { std::lock_guard<std::mutex> g(offer_mu_); offer_ = st; }
                if (st.claimed)      nap(7000);
                else if (st.expired) need_new = true;
            } else if (st.ok && !st.has_offer) {
                need_new = true;
            }
        }
    }

    // ── Wymaganie #3: pętla pobierania komend sędziego ───────────────────────
    // Co 1.5 s pyta /clock/commands?since=N. Komendy dla NASZEGO game_id trafiają
    // do kolejki, którą główna pętla drenuje i stosuje do ClockState + rozsyła
    // do tabletów. Dopóki partia nie istnieje na serwerze (brak game_id) — czeka.
    void commandLoop() {
        if (!networked_) return;
        auto nap = [&](int ms) {
            for (int s = 0; s < ms && running_.load(); s += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        };
        while (running_.load()) {
            nap(1500);
            if (!running_.load()) break;
            std::string gid = gameId();
            if (gid.empty()) continue;
            long long mygame = 0;
            try { mygame = std::stoll(gid); } catch (...) { continue; }
            long long since = cmd_since_.load();
            long long maxId = since;
            auto cmds = apiFetchCommands(ip_, port_, clockCode_, since, &abort_flag_, &maxId);
            for (auto& c : cmds) {
                if (c.game_id != mygame) continue;
                std::lock_guard<std::mutex> g(srv_cmd_mu_);
                if (srv_cmd_queue_.size() < 256) srv_cmd_queue_.push_back(c);
            }
            if (maxId > since) cmd_since_.store(maxId);
        }
    }

    // Wymaganie #2/#3 — stan współdzielony z UI / główną pętlą.
    std::thread                offer_thr_;
    std::thread                cmd_thr_;
    std::mutex                 offer_mu_;
    OfferStatusInfo            offer_;
    std::atomic<long long>     cmd_since_{0};
    std::mutex                 srv_cmd_mu_;
    std::deque<ServerClockCmd> srv_cmd_queue_;
};

static void reportArbiter(ApiClient* api, const std::string& action, long long valueMs) {
    if (!api || !api->networked()) return;
    api->enqueueArbiter(action, valueMs);
}

// ─── BluetoothServer ─────────────────────────────────────────────────────────

// BtPeer — jedna fizyczna sesja RFCOMM. Każdy peer ma własny socket, własne
// kolejki I/O i własną parę wątków (reader + sender). Lifetime jest zarządzany
// przez std::shared_ptr — gdy wszystkie referencje wygasną (vector peers_ +
// detached threads), struct jest niszczony. Dzięki temu accept loop nie musi
// joinować nic — wystarczy że usunie shared_ptr z vectora.
struct BtPeer {
    int                       id          = 0;
    int                       sock        = -1;
    std::string               addr;          // "AA:BB:CC:DD:EE:FF" — do log i remove
    std::string               recv_buffer;
    std::mutex                out_mu;
    std::condition_variable   out_cv;
    std::deque<std::string>   out_queue;
    std::atomic<bool>         alive{true};
};

class BluetoothServer {
public:
    bool start(int channel);
    void stop();

    int  connected_count() const;
    bool is_connected()   const { return connected_count() > 0; }

    // Pop next inbound event from queue. src_peer_id identyfikuje od którego
    // peera przyszła linia (potrzebne do forwardingu i targetowanych odpowiedzi).
    // System events (__BT_CONNECTED__ / __BT_DISCONNECTED__) też mają peer_id.
    bool pop_line(std::string& out, int& src_peer_id);

    // Broadcast do wszystkich połączonych peerów. Używane przez main loop dla
    // CLOCK|... oraz przez ack-retransmit (oba muszą trafić do obu tabletów).
    int  send_line(const std::string& line);

    // Targetowany wysyłka — używana do odpowiedzi na komendy konkretnego peera
    // (OK|hello, ERR|illegal_move) i do initial sync nowo-dołączonego peera.
    int  send_line_to(int peer_id, const std::string& line);

    // Broadcast do wszystkich peerów OPRÓCZ except_peer_id — używane do
    // forwardingu ruchów (sender już ma swój echo, nie chcemy duplikatu).
    int  send_line_except(int except_peer_id, const std::string& line);

    std::string mac()     const { return mac_; }
    int         channel() const { return channel_; }

    static constexpr size_t MAX_PEERS = 2;

    // Wymaganie #1: zegar jest wykrywalny TYLKO dopóki nie podłączą się oba
    // tablety. Po komplecie wyłączamy inquiry scan (zostaje page scan, więc
    // tablet który odpadł może się wciąż wpiąć z powrotem). Publiczne — main
    // wywołuje przy __BT_CONNECTED__ / __BT_DISCONNECTED__.
    void setDiscoverable(bool on);

private:
    void acceptLoop();
    void readerLoop(std::shared_ptr<BtPeer> peer);
    void senderLoop(std::shared_ptr<BtPeer> peer);
    void enableDiscoverable();
    void forgetAllPairedDevices();
    // Wymaganie #3 (Linux) — zegar sam doprowadza system BT do stanu używalności
    // (rfkill, power, SSP, pairable) i rejestruje rekord SDP "Serial Port", żeby
    // telefon mógł się łączyć standardowo po UUID, bez ręcznej konfiguracji.
    void ensureAdapterReady();
    void registerSdpRecord(uint8_t channel);
    void unregisterSdpRecord();
    void pushInEvent(std::string line, int peer_id);
    void enqueueToPeer(BtPeer& peer, const std::string& line);

    std::atomic<bool>                       running_{false};
    std::thread                             accept_thr_;
    int                                     srv_sock_ = -1;
    int                                     channel_  = DEFAULT_BT_CHAN;
    std::string                             mac_      = "(niedostepne)";
    std::atomic<int>                        next_peer_id_{1};

    mutable std::mutex                      peers_mu_;
    std::vector<std::shared_ptr<BtPeer>>    peers_;

    struct InEvent { std::string line; int peer_id; };
    mutable std::mutex                      in_mu_;
    std::deque<InEvent>                     in_queue_;

    // Sesja lokalnego serwera SDP — trzymana, bo zamknięcie sesji usuwa rekord.
    sdp_session_t*                          sdp_session_ = nullptr;
};

// ── Helpers — discoverability + zapomnij sparowane ─────────────────────────

void BluetoothServer::enableDiscoverable() {
    // Spróbuj programatycznie przez HCI; fallback do shella (hciconfig) gdy
    // bez uprawnień. Cel: zegar jest WIDOCZNY (inquiry scan) + można się z nim
    // POŁĄCZYĆ (page scan) zawsze przy starcie, niezależnie od stanu systemu.
    int dev_id = hci_get_route(nullptr);
    if (dev_id >= 0) {
        int hci_sock = hci_open_dev(dev_id);
        if (hci_sock >= 0) {
            uint8_t scan_enable = SCAN_INQUIRY | SCAN_PAGE;
            // HCI_OP_WRITE_SCAN_ENABLE = OGF 0x03, OCF 0x001A
            struct hci_request rq{};
            rq.ogf    = OGF_HOST_CTL;
            rq.ocf    = OCF_WRITE_SCAN_ENABLE;
            rq.cparam = &scan_enable;
            rq.clen   = sizeof(scan_enable);
            uint8_t status = 0;
            rq.rparam = &status;
            rq.rlen   = sizeof(status);
            if (hci_send_req(hci_sock, &rq, 1000) == 0 && status == 0) {
                LOG_INFO("BT: discoverable + connectable (HCI scan enable)");
            } else {
                LOG_WARN("BT: HCI write_scan_enable failed, fallback to hciconfig");
                if (system("hciconfig hci0 piscan >/dev/null 2>&1") == 0) {
                    LOG_INFO("BT: discoverable (via hciconfig hci0 piscan)");
                }
            }
            // Ustaw nazwę widoczną dla skanerów.
            const char* nm = "Chess-Clock-Pi";
            if (hci_write_local_name(hci_sock, const_cast<char*>(nm), 2000) == 0) {
                LOG_INFO("BT: local name set to 'Chess-Clock-Pi'");
            }
            hci_close_dev(hci_sock);
        } else {
            LOG_WARN("BT: hci_open_dev failed — używam hciconfig fallback");
            if (system("hciconfig hci0 piscan >/dev/null 2>&1") == 0) {
                LOG_INFO("BT: discoverable (via hciconfig hci0 piscan)");
            }
            system("hciconfig hci0 name 'Chess-Clock-Pi' >/dev/null 2>&1");
        }
    }
}

void BluetoothServer::setDiscoverable(bool on) {
    // on=true  → inquiry + page scan (widoczny w skanie + można się łączyć)
    // on=false → tylko page scan (niewidoczny dla nowych, ale paired/known
    //            urządzenie wciąż się połączy — potrzebne do reconnectu po dropie)
    int dev_id = hci_get_route(nullptr);
    if (dev_id >= 0) {
        int hci_sock = hci_open_dev(dev_id);
        if (hci_sock >= 0) {
            uint8_t scan_enable = on ? (SCAN_INQUIRY | SCAN_PAGE) : SCAN_PAGE;
            struct hci_request rq{};
            rq.ogf    = OGF_HOST_CTL;
            rq.ocf    = OCF_WRITE_SCAN_ENABLE;
            rq.cparam = &scan_enable;
            rq.clen   = sizeof(scan_enable);
            uint8_t status = 0;
            rq.rparam = &status;
            rq.rlen   = sizeof(status);
            if (hci_send_req(hci_sock, &rq, 1000) == 0 && status == 0) {
                LOG_INFO(on ? "BT: discoverable ON (inquiry+page scan)"
                            : "BT: discoverable OFF (komplet tabletów — tylko page scan)");
                hci_close_dev(hci_sock);
                return;
            }
            hci_close_dev(hci_sock);
        }
    }
    // Fallback przez hciconfig.
    if (system(on ? "hciconfig hci0 piscan >/dev/null 2>&1"
                  : "hciconfig hci0 pscan  >/dev/null 2>&1") == 0) {
        LOG_INFO(on ? "BT: discoverable ON (hciconfig piscan)"
                    : "BT: discoverable OFF (hciconfig pscan)");
    }
}

// ensureAdapterReady — wymaganie #3: system BT na Linuksie ma działać "z pudełka".
// Zamiast wymagać od operatora ręcznego bluetoothctl/rfkill, zegar sam:
//   * odblokowuje radio (rfkill),
//   * podnosi interfejs hci0 i włącza zasilanie adaptera,
//   * włącza SSP (Simple Secure Pairing) — telefon łączy się bez kodu PIN,
//   * ustawia pairable + brak timeoutów wykrywalności.
// Wszystko best-effort przez narzędzia systemowe (bluez jest runtime dependency,
// tak jak w forgetAllPairedDevices). Błędy logujemy, ale nie przerywają startu.
void BluetoothServer::ensureAdapterReady() {
    const char* steps[] = {
        "rfkill unblock bluetooth >/dev/null 2>&1",
        "hciconfig hci0 up >/dev/null 2>&1",
        "hciconfig hci0 sspmode 1 >/dev/null 2>&1",
        "bluetoothctl --timeout 2 power on >/dev/null 2>&1",
        "bluetoothctl --timeout 2 pairable on >/dev/null 2>&1",
        "bluetoothctl --timeout 2 discoverable-timeout 0 >/dev/null 2>&1",
    };
    int ok = 0;
    for (const char* cmd : steps) if (system(cmd) == 0) ok++;
    LOG_INFO("BT: adapter przygotowany (" + std::to_string(ok) + "/6 kroków OK)");
}

// registerSdpRecord — publikuje rekord SDP "Serial Port" (SPP) dla naszego
// kanału RFCOMM. Dzięki temu Android może się łączyć standardowym
// createRfcommSocketToServiceRecord(SPP_UUID), a nie tylko po numerze kanału.
// Uwaga: na BlueZ 5 lokalna rejestracja SDP wymaga bluetoothd w trybie compat
// (ExecStart=... bluetoothd -C). Gdy się nie uda — logujemy wskazówkę i jedziemy
// dalej: aplikacja Android i tak ma fallback na połączenie po numerze kanału.
void BluetoothServer::registerSdpRecord(uint8_t channel) {
    sdp_record_t* record = sdp_record_alloc();
    if (!record) { LOG_WARN("SDP: sdp_record_alloc failed"); return; }

    uuid_t spp_uuid, root_uuid, l2cap_uuid, rfcomm_uuid;
    sdp_profile_desc_t profile{};

    sdp_uuid16_create(&spp_uuid, SERIAL_PORT_SVCLASS_ID);
    sdp_list_t* svc_class = sdp_list_append(nullptr, &spp_uuid);
    sdp_set_service_classes(record, svc_class);

    sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
    profile.version = 0x0100;
    sdp_list_t* profiles = sdp_list_append(nullptr, &profile);
    sdp_set_profile_descs(record, profiles);

    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    sdp_list_t* root = sdp_list_append(nullptr, &root_uuid);
    sdp_set_browse_groups(record, root);

    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    sdp_list_t* l2cap = sdp_list_append(nullptr, &l2cap_uuid);
    sdp_list_t* proto = sdp_list_append(nullptr, l2cap);

    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
    sdp_data_t* chan_data = sdp_data_alloc(SDP_UINT8, &channel);
    sdp_list_t* rfcomm = sdp_list_append(nullptr, &rfcomm_uuid);
    sdp_list_append(rfcomm, chan_data);
    sdp_list_append(proto, rfcomm);

    sdp_list_t* access = sdp_list_append(nullptr, proto);
    sdp_set_access_protos(record, access);

    sdp_set_info_attr(record, "Chess-Clock-Pi", "ChessClock",
                      "Chess clock RFCOMM relay");

    // BDADDR_ANY/BDADDR_LOCAL to compound-literals C — w C++ robimy lokalne kopie.
    bdaddr_t any{};                                  // 00:00:00:00:00:00
    bdaddr_t local{{0, 0, 0, 0xff, 0xff, 0xff}};     // adres "lokalny SDP serwer"
    sdp_session_ = sdp_connect(&any, &local, SDP_RETRY_IF_BUSY);
    if (sdp_session_ && sdp_record_register(sdp_session_, record, 0) == 0) {
        LOG_INFO("SDP: rekord SPP zarejestrowany (kanal " +
                 std::to_string((int)channel) + ")");
    } else {
        if (sdp_session_) { sdp_close(sdp_session_); sdp_session_ = nullptr; }
        LOG_WARN("SDP: rejestracja nieudana — telefon polaczy sie po numerze kanalu. "
                 "Aby wlaczyc SDP: dodaj '-C' do ExecStart bluetoothd "
                 "(/lib/systemd/system/bluetooth.service) i zrestartuj usluge.");
        // Fallback przez sdptool (tez wymaga trybu compat, ale nie zaszkodzi).
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd),
                      "sdptool add --channel=%d SP >/dev/null 2>&1", (int)channel);
        if (system(cmd) == 0) LOG_INFO("SDP: rekord dodany przez sdptool");
        sdp_record_free(record);
    }

    sdp_data_free(chan_data);
    sdp_list_free(svc_class, nullptr);
    sdp_list_free(profiles, nullptr);
    sdp_list_free(root, nullptr);
    sdp_list_free(l2cap, nullptr);
    sdp_list_free(rfcomm, nullptr);
    sdp_list_free(proto, nullptr);
    sdp_list_free(access, nullptr);
}

void BluetoothServer::unregisterSdpRecord() {
    if (sdp_session_) {
        sdp_close(sdp_session_); // zamknięcie sesji usuwa zarejestrowany rekord
        sdp_session_ = nullptr;
    }
}

void BluetoothServer::forgetAllPairedDevices() {
    // Cel: czysty stan przy następnym uruchomieniu. Wyciągamy MAC-i wszystkich
    // sparowanych urządzeń przez `bluetoothctl paired-devices` (część pakietu
    // bluez — runtime dependency, nie build) i każdy usuwamy. Bez zależności
    // od D-Bus / libbluetooth pairing API.
    FILE* f = popen("bluetoothctl --timeout 2 paired-devices 2>/dev/null", "r");
    if (!f) {
        LOG_WARN("BT forget: popen(bluetoothctl) failed");
        return;
    }
    std::vector<std::string> macs;
    char buf[256];
    // Format linii: "Device AA:BB:CC:DD:EE:FF FriendlyName"
    while (fgets(buf, sizeof(buf), f)) {
        std::string l(buf);
        auto a = l.find(' ');
        auto b = (a != std::string::npos) ? l.find(' ', a + 1) : std::string::npos;
        if (a != std::string::npos && b != std::string::npos) {
            macs.push_back(l.substr(a + 1, b - a - 1));
        }
    }
    pclose(f);
    for (const auto& m : macs) {
        std::string cmd = "bluetoothctl --timeout 2 remove " + m + " >/dev/null 2>&1";
        if (system(cmd.c_str()) == 0) {
            LOG_INFO("BT: forgot " + m);
        } else {
            LOG_WARN("BT: forget " + m + " failed");
        }
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

bool BluetoothServer::start(int channel) {
    channel_ = channel;

    // Wymaganie #3 — najpierw doprowadź systemowy BT (Linux) do porządku.
    ensureAdapterReady();

    int dev_id = hci_get_route(nullptr);
    if (dev_id >= 0) {
        bdaddr_t bdaddr{};
        if (hci_devba(dev_id, &bdaddr) == 0) {
            char addr[18] = {0};
            ba2str(&bdaddr, addr);
            mac_ = addr;
        }
    }

    // Zawsze wykrywalny przy starcie (wymaganie #1).
    enableDiscoverable();

    srv_sock_ = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (srv_sock_ < 0) {
        LOG_ERR(std::string("bluetooth socket() failed: ") + std::strerror(errno));
        return false;
    }

    sockaddr_rc loc{};
    loc.rc_family  = AF_BLUETOOTH;
    bdaddr_t any_addr{{0,0,0,0,0,0}};
    loc.rc_bdaddr  = any_addr;
    loc.rc_channel = static_cast<uint8_t>(channel);

    if (bind(srv_sock_, (sockaddr*)&loc, sizeof(loc)) < 0) {
        LOG_ERR(std::string("bluetooth bind() failed: ") + std::strerror(errno));
        close(srv_sock_); srv_sock_ = -1;
        return false;
    }
    // Backlog = MAX_PEERS + 2 (mały zapas żeby kernel kolejkował 3-ci connect
    // attempt zanim go odrzucimy w acceptLoop).
    if (listen(srv_sock_, (int)MAX_PEERS + 2) < 0) {
        LOG_ERR(std::string("bluetooth listen() failed: ") + std::strerror(errno));
        close(srv_sock_); srv_sock_ = -1;
        return false;
    }

    // Wymaganie #3 — rekord SDP "Serial Port", żeby Android mógł łączyć się
    // standardowo po UUID SPP (z fallbackiem na numer kanału w aplikacji).
    registerSdpRecord(static_cast<uint8_t>(channel));

    running_ = true;
    accept_thr_ = std::thread(&BluetoothServer::acceptLoop, this);
    LOG_INFO("BluetoothServer listening on RFCOMM channel " +
             std::to_string(channel) + " (max " +
             std::to_string(MAX_PEERS) + " peers)");
    return true;
}

void BluetoothServer::stop() {
    running_ = false;

    // Zamknij wszystkie peer sockets — to obudzi blokujące recv() w reader
    // threadach. Sender threads zostaną wybudzone przez notify w pętli.
    {
        std::lock_guard<std::mutex> g(peers_mu_);
        for (auto& p : peers_) {
            p->alive = false;
            if (p->sock >= 0) {
                ::shutdown(p->sock, SHUT_RDWR);
                ::close(p->sock);
                p->sock = -1;
            }
            p->out_cv.notify_all();
        }
        peers_.clear(); // shared_ptr wciąż żyją w detached wątkach
    }

    if (srv_sock_ >= 0) {
        ::shutdown(srv_sock_, SHUT_RDWR);
        ::close(srv_sock_);
        srv_sock_ = -1;
    }

    if (accept_thr_.joinable()) accept_thr_.join();

    // Usuń rekord SDP (zamknięcie sesji wyrejestrowuje wpis).
    unregisterSdpRecord();

    // Wymaganie #1: zapomnij sparowane urządzenia przy shutdown.
    forgetAllPairedDevices();
}

void BluetoothServer::pushInEvent(std::string line, int peer_id) {
    std::lock_guard<std::mutex> g(in_mu_);
    if (in_queue_.size() >= BT_QUEUE_MAX) in_queue_.pop_front();
    in_queue_.push_back({std::move(line), peer_id});
}

// ── Accept loop ────────────────────────────────────────────────────────────

void BluetoothServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_rc rem{};
        socklen_t   len = sizeof(rem);
        int         cs  = accept(srv_sock_, (sockaddr*)&rem, &len);
        if (cs < 0) {
            if (!running_) return;
            LOG_WARN(std::string("bluetooth accept() failed: ") + std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Sprawdź limit peerów (z lockiem). Reaper: usuń dead peery najpierw.
        std::shared_ptr<BtPeer> peer;
        {
            std::lock_guard<std::mutex> g(peers_mu_);
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                [](const std::shared_ptr<BtPeer>& p){ return !p->alive.load(); }),
                peers_.end());

            if (peers_.size() >= MAX_PEERS) {
                LOG_WARN("BT: max peers (" + std::to_string(MAX_PEERS) +
                         ") osiągnięte — odrzucam nowe połączenie");
                ::close(cs);
                continue;
            }

            peer       = std::make_shared<BtPeer>();
            peer->id   = next_peer_id_.fetch_add(1);
            peer->sock = cs;
            char addrbuf[18] = {0};
            ba2str(&rem.rc_bdaddr, addrbuf);
            peer->addr = addrbuf;
            peers_.push_back(peer);
        }

        struct timeval tv_send{BT_SEND_TIMEOUT_MS / 1000,
                               (BT_SEND_TIMEOUT_MS % 1000) * 1000};
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));
        struct timeval tv_recv{30, 0};
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));

        LOG_INFO("BT peer " + std::to_string(peer->id) +
                 " connected from " + peer->addr +
                 " (" + std::to_string(connected_count()) + "/" +
                 std::to_string(MAX_PEERS) + ")");

        // System event do kolejki — handleBluetoothCommand użyje peer_id do
        // wysłania initial sync (sendFullSync) tylko do tego peera.
        pushInEvent("__BT_CONNECTED__", peer->id);

        // Reader + sender — detached. shared_ptr<BtPeer> trzyma struct przy
        // życiu dopóki oba wątki działają.
        std::thread(&BluetoothServer::readerLoop, this, peer).detach();
        std::thread(&BluetoothServer::senderLoop, this, peer).detach();
    }
}

// ── Per-peer reader ────────────────────────────────────────────────────────

void BluetoothServer::readerLoop(std::shared_ptr<BtPeer> peer) {
    char buf[1024];
    while (running_.load() && peer->alive.load()) {
        int sock = peer->sock;
        if (sock < 0) break;
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n == 0) break; // peer disconnected
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }

        if (peer->recv_buffer.size() + (size_t)n > BT_RECV_BUFFER_MAX) {
            LOG_WARN("BT peer " + std::to_string(peer->id) +
                     " recv_buffer overflow — czyszczę");
            peer->recv_buffer.clear();
        }
        peer->recv_buffer.append(buf, n);

        size_t pos;
        while ((pos = peer->recv_buffer.find('\n')) != std::string::npos) {
            std::string line = peer->recv_buffer.substr(0, pos);
            peer->recv_buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > BT_LINE_MAX) {
                LOG_WARN("BT peer " + std::to_string(peer->id) +
                         " line too long — odrzucam");
                continue;
            }
            if (!line.empty()) pushInEvent(std::move(line), peer->id);
        }
    }

    // Cleanup: oznacz peer jako dead, usuń socket, notify sender żeby się obudził.
    peer->alive = false;
    if (peer->sock >= 0) {
        ::shutdown(peer->sock, SHUT_RDWR);
        ::close(peer->sock);
        peer->sock = -1;
    }
    peer->out_cv.notify_all();
    {
        std::lock_guard<std::mutex> g(peers_mu_);
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
            [pid = peer->id](const std::shared_ptr<BtPeer>& p){ return p->id == pid; }),
            peers_.end());
    }
    pushInEvent("__BT_DISCONNECTED__", peer->id);
    LOG_INFO("BT peer " + std::to_string(peer->id) + " disconnected (" +
             std::to_string(connected_count()) + "/" +
             std::to_string(MAX_PEERS) + ")");
}

// ── Per-peer sender ────────────────────────────────────────────────────────

void BluetoothServer::senderLoop(std::shared_ptr<BtPeer> peer) {
    while (running_.load() && peer->alive.load()) {
        std::string msg;
        {
            std::unique_lock<std::mutex> g(peer->out_mu);
            peer->out_cv.wait(g, [&]{
                return !running_.load() || !peer->alive.load() || !peer->out_queue.empty();
            });
            if (!peer->alive.load() || !running_.load()) return;
            if (peer->out_queue.empty()) continue;
            msg = std::move(peer->out_queue.front());
            peer->out_queue.pop_front();
        }

        int sock = peer->sock;
        if (sock < 0) return;

        if (msg.empty() || msg.back() != '\n') msg.push_back('\n');

        ssize_t off    = 0;
        bool    failed = false;
        while (off < (ssize_t)msg.size()) {
            ssize_t n = send(sock, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
            if (n <= 0) {
                LOG_WARN("BT peer " + std::to_string(peer->id) +
                         " send error: " + std::string(std::strerror(errno)));
                failed = true;
                break;
            }
            off += n;
        }
        if (failed) {
            peer->alive = false;
            if (peer->sock >= 0) {
                ::shutdown(peer->sock, SHUT_RDWR);
                ::close(peer->sock);
                peer->sock = -1;
            }
            // reader wykryje shutdown, doda __BT_DISCONNECTED__ + zdejmie z peers_
            return;
        }
    }
}

// ── Input ─────────────────────────────────────────────────────────────────

bool BluetoothServer::pop_line(std::string& out, int& src_peer_id) {
    std::lock_guard<std::mutex> g(in_mu_);
    if (in_queue_.empty()) return false;
    out         = std::move(in_queue_.front().line);
    src_peer_id = in_queue_.front().peer_id;
    in_queue_.pop_front();
    return true;
}

int BluetoothServer::connected_count() const {
    std::lock_guard<std::mutex> g(peers_mu_);
    int n = 0;
    for (const auto& p : peers_) if (p->alive.load()) n++;
    return n;
}

// ── Output ────────────────────────────────────────────────────────────────

void BluetoothServer::enqueueToPeer(BtPeer& peer, const std::string& line) {
    std::lock_guard<std::mutex> g(peer.out_mu);
    if (peer.out_queue.size() >= BT_SEND_QUEUE_MAX) {
        // Drop najstarsze CLOCK|... messages (są ulotne, kolejny CLOCK przyjdzie
        // za 500 ms). Inne komunikaty zachowujemy.
        for (auto it = peer.out_queue.begin(); it != peer.out_queue.end();) {
            if (it->rfind("CLOCK|", 0) == 0) it = peer.out_queue.erase(it);
            else ++it;
            if (peer.out_queue.size() < BT_SEND_QUEUE_MAX / 2) break;
        }
        if (peer.out_queue.size() >= BT_SEND_QUEUE_MAX) {
            LOG_WARN("BT peer " + std::to_string(peer.id) +
                     " out_queue full — dropping line");
            return;
        }
    }
    peer.out_queue.push_back(line);
    peer.out_cv.notify_one();
}

int BluetoothServer::send_line(const std::string& line) {
    std::lock_guard<std::mutex> g(peers_mu_);
    int n = 0;
    for (auto& p : peers_) {
        if (!p->alive.load()) continue;
        enqueueToPeer(*p, line);
        n++;
    }
    return n;
}

int BluetoothServer::send_line_to(int peer_id, const std::string& line) {
    std::lock_guard<std::mutex> g(peers_mu_);
    for (auto& p : peers_) {
        if (p->id == peer_id && p->alive.load()) {
            enqueueToPeer(*p, line);
            return 1;
        }
    }
    return 0;
}

int BluetoothServer::send_line_except(int except_peer_id, const std::string& line) {
    std::lock_guard<std::mutex> g(peers_mu_);
    int n = 0;
    for (auto& p : peers_) {
        if (!p->alive.load() || p->id == except_peer_id) continue;
        enqueueToPeer(*p, line);
        n++;
    }
    return n;
}

// ─── Event handlers ─────────────────────────────────────────────────────────

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
    
    std::string             pendingName; // do CHESS_PROPOSE
};

static void handle_setup_events(ClockState* c, AppResources* a,
                                const SDL_Event* e, bool* running) {
    if (e->type == SDL_QUIT) { *running = false; return; }
    if (e->type == SDL_KEYDOWN) {
        if (e->key.keysym.sym == SDLK_ESCAPE) *running = false;
        else if (e->key.keysym.sym == SDLK_b) a->mode = UI_MODE_QR;
        // item #3 — P uruchamia ekran parowania (6-cyfrowy kod od organizatora).
        else if (e->key.keysym.sym == SDLK_p) {
            a->mode = UI_MODE_PAIRING;
            a->pair_input.clear();
            a->pair_status.clear();
        }
        return;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN) return;
    int x = e->button.x, y = e->button.y;
    
    // Przycisk BT (Góra lewa)
    if (x >= 20 && x <= 80 && y >= 10 && y <= 55) { a->mode = UI_MODE_QR; return; }
    // Przycisk WYJDŹ (Góra prawa)
    if (x >= WINDOW_WIDTH - 130 && x <= WINDOW_WIDTH - 20 && y >= 10 && y <= 55) { *running = false; return; }

    if (x >= 330 && x < 370 && y >= 60 && y < 95) { if (a->setup_minutes > 1)  a->setup_minutes--; return; }
    if (x >= 440 && x < 480 && y >= 60 && y < 95) { if (a->setup_minutes < 60) a->setup_minutes++; return; }
    if (x >= 810 && x < 850 && y >= 60 && y < 95) { if (a->setup_increment > 0)  a->setup_increment--; return; }
    if (x >= 920 && x < 960 && y >= 60 && y < 95) { if (a->setup_increment < 30) a->setup_increment++; return; }
    
    if (x >= 340 && x <= 640 && y >= 130 && y <= 200) {
        // Wymaganie #1: blokuj START dopóki nie podłączą się 2 tablety.
        if (a->bt_peer_count < a->bt_peer_max) {
            LOG_INFO("START click ignored — only " +
                     std::to_string(a->bt_peer_count) + "/" +
                     std::to_string(a->bt_peer_max) + " tablets connected");
            return;
        }
        uint32_t start_ms = a->setup_minutes * 60 * 1000;
        uint32_t inc_ms   = a->setup_increment * 1000;
        init_clock(c, start_ms, inc_ms);
        a->mode = UI_MODE_GAME;
        c->state = STATE_PAUSED;
    }
}

static void handle_game_events(ClockState* c, AppResources* a, ApiClient* api,
                               Session& sess, BluetoothServer& bt,
                               const SDL_Event* e, bool* running) {
    if (e->type == SDL_QUIT) { *running = false; return; }
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
            case SDLK_ESCAPE: a->mode = UI_MODE_SETUP; a->overlay.visible = false; c->state = STATE_PAUSED; return;
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
                c->pending_uci.clear(); 
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

    // Przycisk BT (Góra lewa)
    if (x >= 20 && x <= 80 && y >= 10 && y <= 55) { a->mode = UI_MODE_QR; return; }
    // Przycisk MENU (Góra prawa) - ZABEZPIECZA PRZED ZAWIESZENIEM
    if (x >= WINDOW_WIDTH - 130 && x <= WINDOW_WIDTH - 20 && y >= 10 && y <= 55) { 
        a->mode = UI_MODE_SETUP; 
        a->overlay.visible = false; 
        c->state = STATE_PAUSED; 
        return; 
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
        c->pending_uci.clear();
        reportArbiter(api, "reset", 0);
        return;
    }
    if (x >= 350 && x <= 500 && y >= 325 && y <= 385) { a->mode = UI_MODE_ARBITER; return; }

    ActiveSide side = (x < WINDOW_WIDTH / 2) ? ACTIVE_LEFT : ACTIVE_RIGHT;
    if (c->state == STATE_PAUSED) {
        c->state = STATE_RUNNING;
        reportArbiter(api, "resume", 0);
    }
    
    if (c->state == STATE_RUNNING && c->active == side) {
        if (!c->pending_uci.empty()) {
            std::string uci = c->pending_uci;
            c->pending_uci.clear();
            sess.pendingName.clear();
            // src_peer_id = -1 (commit z UI zegara, nie od konkretnego tabletu).
            // tryMove broadcastuje wynik do obu tabletów.
            tryMove(sess, *a, *c, bt, *api, /*src_peer_id*/-1, -1, uci, true);
        } else {
            switch_side(c, side);
        }
    } else {
        switch_side(c, side);
    }
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
    
    // Przycisk ZAMKNIJ
    if (x >= 620 && x <= 840 && y >= 175 && y <= 240) { a->mode = UI_MODE_GAME; return; }
}

// ── handle_pairing_events (item #3) ────────────────────────────────────────
// Klawiatura: 0-9 dodaje cyfrę, BACKSPACE kasuje, ENTER wysyła, ESC wraca.
// Myszka/touch: keypad 3x4 dokładnie ułożony jak w draw_pairing_screen.
static void submit_pair_code(AppResources* a, ApiClient* api) {
    if (a->pair_input.size() != 6 || a->pair_in_flight) return;
    a->pair_in_flight = true;
    a->pair_status = "Wysylanie...";
    // Synchroniczne wywołanie — krótkie HTTPS post, akceptowalne w UI thread.
    PairResult res = api->pairWithCode(a->pair_input);
    a->pair_in_flight = false;
    if (res.ok) {
        a->pair_status = std::string("OK ") + (res.target.empty() ? "" : res.target);
        a->pair_session_label = res.label;
        a->pair_input.clear();
    } else {
        a->pair_status = std::string("Blad: ") + res.error;
    }
}

static void handle_pairing_events(AppResources* a, ApiClient* api,
                                  const SDL_Event* e, bool* running) {
    if (e->type == SDL_QUIT) { *running = false; return; }
    if (a->pair_in_flight) return;
    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_ESCAPE)            { a->mode = UI_MODE_SETUP; return; }
        if (k == SDLK_BACKSPACE && !a->pair_input.empty()) { a->pair_input.pop_back(); a->pair_status.clear(); return; }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER)       { submit_pair_code(a, api); return; }
        // Cyfry — z głównej klawiatury lub keypadu.
        char digit = -1;
        if      (k >= SDLK_0 && k <= SDLK_9)             digit = '0' + (k - SDLK_0);
        else if (k >= SDLK_KP_0 && k <= SDLK_KP_9) {
            digit = (k == SDLK_KP_0) ? '0' : '1' + (k - SDLK_KP_1);
        }
        if (digit >= 0 && a->pair_input.size() < 6) {
            a->pair_input.push_back(digit);
            a->pair_status.clear();
        }
        return;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN) return;
    int x = e->button.x, y = e->button.y;

    // Klawiatura numeryczna 3x4 (zgodna z draw_pairing_screen).
    const int kx0 = 700, ky0 = 110, bw = 110, bh = 90, gap = 10;
    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        int bx = kx0 + col * (bw + gap), by = ky0 + row * (bh + gap);
        if (x < bx || x > bx + bw || y < by || y > by + bh) continue;
        a->pair_status.clear();
        if (i == 9) {           // ← backspace
            if (!a->pair_input.empty()) a->pair_input.pop_back();
        } else if (i == 11) {   // OK
            submit_pair_code(a, api);
        } else {                // cyfra
            char d = (i == 10) ? '0' : ('1' + i);
            if (a->pair_input.size() < 6) a->pair_input.push_back(d);
        }
        return;
    }
    // Powrót.
    if (x >= 40 && x <= 260 && y >= 540 && y <= 600) {
        a->mode = UI_MODE_SETUP;
    }
}

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
        // Przycisk PRZEJDŹ DO GRY (Zamiast iksa, wieksza wygoda dotykowa)
        if (x >= WINDOW_WIDTH - 300 && x <= WINDOW_WIDTH - 50 && y >= WINDOW_HEIGHT - 90 && y <= WINDOW_HEIGHT - 30) {
            a->mode = UI_MODE_SETUP;
        }
    }
}

static void ui_process_events(ClockState* c, AppResources* a, ApiClient* api,
                              Session& sess, BluetoothServer& bt,
                              bool* running) {
    SDL_Event e;
    int processed = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q &&
            (e.key.keysym.mod & KMOD_CTRL)) {
            *running = false;
            continue;
        }
        if (e.type == SDL_QUIT) { *running = false; continue; }

        // ROZWIĄZANIE PROBLEMU ZAWIESZANIA
        // Sprawdzamy czy okienko wygranej ma prawo pochłaniać kliknięcia
        bool overlay_active = (a->mode == UI_MODE_GAME || a->mode == UI_MODE_ARBITER) &&
                              a->overlay.visible && !a->overlay.dismissed;

        if (overlay_active) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                a->overlay.dismissed = true;
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN) {
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
                continue; // Pochłaniamy resztę kliknięć w okno pod spodem (tylko w grze!)
            }
        }

        switch (a->mode) {
            case UI_MODE_QR:      handle_qr_events(a, &e, running);            break;
            case UI_MODE_SETUP:   handle_setup_events(c, a, &e, running);      break;
            case UI_MODE_ARBITER: handle_arbiter_events(c, a, api, &e, running); break;
            case UI_MODE_PAIRING: handle_pairing_events(a, api, &e, running);  break;
            case UI_MODE_HELP:
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                    a->mode = UI_MODE_GAME;
                else if (e.type == SDL_MOUSEBUTTONDOWN) {
                    int x = e.button.x, y = e.button.y;
                    // Przycisk ZROZUMIANO / ZAMKNIJ POMOC
                    if (x >= (WINDOW_WIDTH - 300)/2 && x <= (WINDOW_WIDTH + 300)/2 && y >= WINDOW_HEIGHT - 80 && y <= WINDOW_HEIGHT - 20)
                        a->mode = UI_MODE_GAME;
                }
                break;
            case UI_MODE_GAME:
            default:              handle_game_events(c, a, api, sess, bt, &e, running); break;
        }
        if (++processed > 256) break;
    }
}

// ─── Bluetooth command handling ─────────────────────────────────────────────

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
    // Wymaganie #1: nie pozwól startować bez 2 podłączonych tabletów (gdy
    // sieciowo gramy). Lokalny "solo" run zawsze dopuszczalny.
    if (sess.networked && bt.connected_count() < (int)BluetoothServer::MAX_PEERS) {
        bt.send_line("ERR|need_two_tablets|" + std::to_string(bt.connected_count()) +
                     "/" + std::to_string(BluetoothServer::MAX_PEERS));
        LOG_WARN("startNewGame zablokowane — connected_count=" +
                 std::to_string(bt.connected_count()));
        return;
    }
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
    cs.pending_uci.clear();
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
    // Wymaganie #5 — jednoznaczny sygnał startu dla telefonów (oba przechodzą do
    // ekranu partii). Zawiera nazwy i tempo, niezależnie od źródła startu.
    {
        std::ostringstream gs;
        gs << "GAME_START|" << sess.whiteName << "|" << sess.blackName
           << "|" << sess.setupMinutes << "|" << sess.setupIncrement;
        bt.send_line(gs.str());
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

// tryMove — src_peer_id wskazuje peera od którego przyszedł ruch:
//   * błędy / DUP / ACK lecą TYLKO do source (`send_line_to`)
//   * po udanym ruchu broadcast `MOVE|...` leci do POZOSTAŁYCH peerów
//     (`send_line_except`), tak żeby druga szachownica się zsynchronizowała.
//   * GAME_OVER + retransmit lecą do wszystkich (broadcast — oba tablety
//     mają zobaczyć koniec partii).
// src_peer_id == -1 → wywołanie lokalne (np. CHESS_COMMIT z UI zegara) →
// w takim razie wszystkie wiadomości lecą jako broadcast (nie ma "source").
static void tryMove(Session& sess, AppResources& /*app*/, ClockState& cs,
                    BluetoothServer& bt, ApiClient& api,
                    int src_peer_id, int seq, std::string uciStr, bool is_commit) {
    auto reply = [&](const std::string& msg) {
        if (src_peer_id > 0) bt.send_line_to(src_peer_id, msg);
        else                 bt.send_line(msg);
    };

    if (sess.over) { reply("ERR|game_over"); return; }

    if (!normalizeUci(uciStr)) {
        reply("ERR|bad_format|" + uciStr);
        return;
    }

    if (seq >= 0 && seq == sess.lastAcceptedSeq) {
        reply("DUP|" + uciStr);
        return;
    }
    if (!sess.lastAcceptedUci.empty() && sess.lastAcceptedUci == uciStr) {
        reply("DUP|" + uciStr);
        return;
    }

    try {
        Movelist moves;
        movegen::legalmoves(moves, sess.board);
        if (moves.size() == 0) { reply("ERR|no_legal_moves"); return; }

        bool isWhiteTurn = (sess.board.sideToMove() == Color::WHITE);

        Move m;
        try { m = uci::uciToMove(sess.board, uciStr); }
        catch (...) { reply("ERR|bad_format|" + uciStr); return; }

        bool legal = false;
        for (auto mv : moves) if (uci::moveToUci(mv) == uciStr) { legal = true; break; }
        if (!legal) { reply("ERR|illegal_move|" + uciStr); return; }

        sess.board.makeMove(m);

        cs.pending_uci.clear();

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

        // Echo do sender peera (jak dzisiaj — żeby tablet wiedział że ruch
        // przyjęty). is_commit ma własny format używany przez CHESS_PROPOSE flow.
        if (is_commit) {
            reply("CHESS_CLOCK_COMMIT");
        } else {
            reply(uciStr);
        }

        // Forwarding do drugiego tabletu (wymaganie #2) — żeby jego
        // szachownica też wiedziała o ruchu. seq w formacie "MOVE|seq|uci"
        // pozwala tabletowi deduplikować jeśli sam by go zobaczył (ale i tak
        // przesyłamy tylko legalne ruchy, więc duplikat byłby rzadkością).
        if (src_peer_id > 0) {
            std::ostringstream fwd;
            fwd << "MOVE|" << (seq >= 0 ? std::to_string(seq) : "0")
                << "|" << uciStr;
            bt.send_line_except(src_peer_id, fwd.str());
        }

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
            // GAME_OVER broadcast — oba tablety muszą wiedzieć (send_with_ack
            // używa bt.send_line który jest broadcastem).
            send_with_ack(bt, sess, "game_over",
                          "GAME_OVER|" + winner + "|" + status);
            LOG_INFO("Game over: winner=" + winner + " status=" + status);
        }
    } catch (const std::exception& e) {
        LOG_ERR(std::string("tryMove exception: ") + e.what());
        reply("ERR|engine_exception");
    } catch (...) {
        LOG_ERR("tryMove unknown exception");
        reply("ERR|engine_exception");
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

// sendFullSync — wyślij snapshot do KONKRETNEGO peera. target_peer_id == -1
// oznacza broadcast (legacy callers / SYNC_REQUEST bez kontekstu peera).
static void sendFullSync(BluetoothServer& bt, int target_peer_id,
                         const Session& sess, const ClockState& cs) {
    auto out = [&](const std::string& line) {
        if (target_peer_id > 0) bt.send_line_to(target_peer_id, line);
        else                    bt.send_line(line);
    };
    out("SYNC_BEGIN");
    {
        std::ostringstream os;
        os << "SYNC_GAME|" << sess.whiteName << "|" << sess.blackName
           << "|" << sess.setupMinutes << "|" << sess.setupIncrement;
        out(os.str());
    }
    {
        std::ostringstream os;
        os << "SYNC_CLOCK|" << cs.left.remaining_ms << "|" << cs.right.remaining_ms
           << "|" << (cs.active == ACTIVE_LEFT ? "white" : "black")
           << "|" << (int)cs.state;
        out(os.str());
    }
    {
        std::ostringstream os;
        os << "SYNC_HISTORY|" << sess.moveHistory.size();
        out(os.str());
    }
    int idx = 0;
    for (const auto& mv : sess.moveHistory) {
        std::ostringstream os;
        os << "SYNC_MOVE|" << idx++ << "|" << mv;
        out(os.str());
    }
    out("SYNC_END");
    LOG_INFO("Sent SYNC snapshot to peer " + std::to_string(target_peer_id) +
             " (history=" + std::to_string(sess.moveHistory.size()) + " moves)");
}

static bool isUci(const std::string& s) {
    if (s.size() != 4 && s.size() != 5) return false;
    if (s[0] < 'a' || s[0] > 'h') return false;
    if (s[1] < '1' || s[1] > '8') return false;
    if (s[2] < 'a' || s[2] > 'h') return false;
    if (s[3] < '1' || s[3] > '8') return false;
    if (s.size() == 5) {
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(s[4])));
        if (p != 'q' && p != 'r' && p != 'b' && p != 'n') return false;
    }
    return true;
}

static void handleBluetoothCommand(const std::string& line, int src_peer_id,
                                   Session& sess, AppResources& app,
                                   ClockState& cs, BluetoothServer& bt,
                                   ApiClient& api) {
    // Helper — odpowiedzi do KONKRETNEGO source peera (nie broadcast).
    // Jeśli src_peer_id == -1 (np. lokalne wywołanie) — broadcast.
    auto reply = [&](const std::string& msg) {
        if (src_peer_id > 0) bt.send_line_to(src_peer_id, msg);
        else                 bt.send_line(msg);
    };

    if (line == "__BT_CONNECTED__") {
        int cnt = bt.connected_count();
        if (cnt < (int)BluetoothServer::MAX_PEERS) {
            app.bt_status = "Oczekiwanie na tablety... (" +
                            std::to_string(cnt) + "/" +
                            std::to_string(BluetoothServer::MAX_PEERS) + ")";
        } else {
            app.bt_status = "Gotowe — oba tablety podlaczone.";
            // Wymaganie #1: komplet → przestań być wykrywalny (zostaje page scan).
            bt.setDiscoverable(false);
        }
        if (app.mode == UI_MODE_QR) app.mode = UI_MODE_SETUP;
        // Powitanie + initial sync TYLKO do nowego peera.
        bt.send_line_to(src_peer_id, "HELLO|chess_clock_pi");
        // Jeśli partia już trwa (są ruchy w historii LUB stan != PAUSED na
        // świeżo zresetowanym zegarze), wyślij snapshot do peera żeby jego
        // szachownica się zsynchronizowała. To załatwia case: tablet B dołącza
        // mid-game po tym jak tablet A już zagrał kilka ruchów.
        if (!sess.moveHistory.empty()) {
            sendFullSync(bt, src_peer_id, sess, cs);
        }
        if (sess.awaiting_ack) {
            sess.ack_last_sent_ms = SDL_GetTicks() - ACK_TIMEOUT_MS;
        }
        LOG_INFO("Bluetooth peer " + std::to_string(src_peer_id) +
                 " connected (count " + std::to_string(cnt) + ")");
        return;
    }
    if (line == "__BT_DISCONNECTED__") {
        int cnt = bt.connected_count();
        // Wymaganie #1: poniżej kompletu znów stań się wykrywalny, by brakujący
        // (lub nowy) tablet mógł dołączyć.
        bt.setDiscoverable(true);
        if (cnt == 0) {
            app.bt_status = "Brak tabletow. Wlacz Bluetooth i znajdz 'Chess-Clock-Pi'.";
        } else {
            app.bt_status = "Tablet odlaczony — czekam na powrot (" +
                            std::to_string(cnt) + "/" +
                            std::to_string(BluetoothServer::MAX_PEERS) + ")";
        }
        LOG_INFO("Bluetooth peer " + std::to_string(src_peer_id) +
                 " disconnected (count " + std::to_string(cnt) + ")");
        return;
    }

    auto parts = splitPipe(line);
    for (auto& p : parts) p = trim(p);
    if (parts.empty() || parts[0].empty()) return;
    const std::string& cmd = parts[0];

    if (isUci(cmd)) {
        tryMove(sess, app, cs, bt, api, src_peer_id, -1, cmd, false);
        return;
    }

    if (cmd == "HELLO")  { reply("OK|hello"); return; }

    if (cmd == "CHESS_PROPOSE") {
        if (parts.size() >= 4) {
            sess.pendingName = parts[2];
            cs.pending_uci = parts[3];
        }
        return;
    }

    if (cmd == "ACK") {
        if (parts.size() >= 2 && sess.awaiting_ack && parts[1] == sess.ack_context) {
            LOG_INFO("Received ACK for context=" + parts[1] +
                     " from peer " + std::to_string(src_peer_id));
            clear_ack(sess);
        }
        return;
    }

    if (cmd == "SYNC_REQUEST") { sendFullSync(bt, src_peer_id, sess, cs); return; }

    if (cmd == "PAUSE") {
        GameState before = cs.state;
        pause_resume_clock(&cs);
        if (cs.state != before)
            api.enqueueArbiter(cs.state == STATE_RUNNING ? "resume" : "pause", 0);
        bt.send_line("OK|pause"); // broadcast — oba tablety widzą pauzę
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
        cs.pending_uci.clear();
        clear_ack(sess);
        resetBoardSafe(sess.board);
        api.enqueueArbiter("reset", 0);
        bt.send_line("OK|reset"); // broadcast — oba tablety mają zresetować board
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
    if (cmd == "QUIT") { reply("OK|quit"); return; }

    if (cmd == "NEWGAME") {
        std::string w = parts.size() > 1 ? parts[1] : "White";
        std::string b = parts.size() > 2 ? parts[2] : "Black";
        int mins = parts.size() > 3 ? std::atoi(parts[3].c_str()) : START_MINUTES;
        int inc  = parts.size() > 4 ? std::atoi(parts[4].c_str()) : INCREMENT_SECONDS;
        startNewGame(sess, app, cs, bt, api, w, b, mins, inc);
        return;
    }

    if (cmd == "MOVE") {
        if (parts.size() < 2) { reply("ERR|move_requires_uci"); return; }
        int seq = -1;
        std::string uciStr;
        if (parts.size() >= 3 &&
            !parts[1].empty() &&
            std::all_of(parts[1].begin(), parts[1].end(),[](unsigned char c){ return std::isdigit(c); })) {
            seq = std::atoi(parts[1].c_str());
            uciStr = parts[2];
        } else {
            uciStr = parts[1];
        }
        tryMove(sess, app, cs, bt, api, src_peer_id, seq, uciStr, false);
        return;
    }

    if (cmd == "ERROR") {
        if (parts.size() < 2) { reply("ERR|who?"); return; }
        ActiveSide who = (parts[1] == "white" || parts[1] == "White") ? ACTIVE_LEFT : ACTIVE_RIGHT;
        player_error(&cs, who);
        api.enqueueArbiter(who == ACTIVE_LEFT ? "error_white" : "error_black", 0);
        bt.send_line("OK|error");
        return;
    }

    if (cmd == "BONUS") {
        if (parts.size() < 3) { reply("ERR|who?|ms?"); return; }
        ActiveSide who = (parts[1] == "white" || parts[1] == "White") ? ACTIVE_LEFT : ACTIVE_RIGHT;
        uint32_t ms = (uint32_t)std::atoi(parts[2].c_str());
        add_bonus_time(&cs, who, ms);
        api.enqueueArbiter(who == ACTIVE_LEFT ? "bonus_white" : "bonus_black", ms);
        bt.send_line("OK|bonus");
        return;
    }

    if (cmd == "PLAYER") {
        // Wymaganie #5 — aplikacja telefonu zgłasza imię gracza dla swojego koloru.
        // Zapamiętujemy nazwy i rozgłaszamy obu telefonom PLAYERS|white|black, żeby
        // strona inicjująca (białe) mogła wysłać NEWGAME z obiema nazwami, a nazwy
        // trafiły na serwer (sendNewGame). reply OK|player do nadawcy.
        if (parts.size() >= 3) {
            const std::string& color = parts[1];
            const std::string& nm = parts[2];
            if (color == "White" || color == "white")      sess.whiteName = nm;
            else if (color == "Black" || color == "black") sess.blackName = nm;
        }
        bt.send_line("PLAYERS|" + sess.whiteName + "|" + sess.blackName);
        reply("OK|player");
        return;
    }

    reply("ERR|unknown_command|" + cmd);
    LOG_WARN("Unknown BT command from peer " +
             std::to_string(src_peer_id) + ": " + cmd);
}

// applyServerCommand — wymaganie #3: stosuje komendę sędziego (z panelu arbitra,
// dostarczoną przez serwer i /clock/commands) do lokalnego ClockState i rozsyła
// efekt do obu tabletów. ADD/SUBTRACT/SET = korekta czasu; STOP/PLAY = zatrzymaj/
// wznów; FINISH (reason = White|Black|Draw) = orzeczenie wygranej/remisu.
static void applyServerCommand(const ServerClockCmd& c, Session& sess,
                               ClockState& cs, BluetoothServer& bt) {
    ActiveSide   side = (c.side == "Black" || c.side == "black") ? ACTIVE_RIGHT : ACTIVE_LEFT;
    PlayerClock& pc   = (side == ACTIVE_LEFT) ? cs.left : cs.right;

    if (c.action == "ADD") {
        if (c.amount_ms > 0) add_bonus_time(&cs, side, (uint32_t)c.amount_ms);
        bt.send_line("ARBITER|add|" + c.side + "|" + std::to_string(c.amount_ms));
        LOG_INFO("Arbiter ADD " + c.side + " +" + std::to_string(c.amount_ms) + "ms");
    } else if (c.action == "SUBTRACT") {
        uint32_t dec = (uint32_t)(c.amount_ms > 0 ? c.amount_ms : 0);
        pc.remaining_ms = (dec >= pc.remaining_ms) ? 0u : (pc.remaining_ms - dec);
        bt.send_line("ARBITER|sub|" + c.side + "|" + std::to_string(c.amount_ms));
        LOG_INFO("Arbiter SUBTRACT " + c.side + " -" + std::to_string(c.amount_ms) + "ms");
    } else if (c.action == "SET") {
        pc.remaining_ms = (uint32_t)(c.amount_ms > 0 ? c.amount_ms : 0);
        bt.send_line("ARBITER|set|" + c.side + "|" + std::to_string(c.amount_ms));
        LOG_INFO("Arbiter SET " + c.side + " =" + std::to_string(c.amount_ms) + "ms");
    } else if (c.action == "STOP") {
        stop_by_arbiter(&cs);
        bt.send_line("OK|arbiter_stop");
        LOG_INFO("Arbiter STOP");
    } else if (c.action == "PLAY") {
        resume_by_arbiter(&cs);
        if (cs.state == STATE_PAUSED) cs.state = STATE_RUNNING;
        bt.send_line("OK|arbiter_resume");
        LOG_INFO("Arbiter PLAY");
    } else if (c.action == "FINISH") {
        std::string winner = c.reason; // White | Black | Draw
        if (winner == "White")      cs.state = STATE_FINISHED_LEFT_WIN;
        else if (winner == "Black") cs.state = STATE_FINISHED_RIGHT_WIN;
        else { cs.state = STATE_FINISHED_DRAW; winner = "Draw"; }
        cs.finish_reason = "arbiter";
        sess.over = true;
        send_with_ack(bt, sess, "game_over", "GAME_OVER|" + winner + "|arbiter");
        LOG_INFO("Arbiter FINISH winner=" + winner);
    } else {
        LOG_WARN("Unknown server command action: " + c.action);
    }
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
    // item #3 — od reformy zegar NIE wymaga API key. Pole zostawione dla
    // back-compat z istniejącymi clock.json; jeśli puste, nie wysyłamy nic.
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
    
    if (c.clockCode.empty() || c.clockCode.find("XXXX") != std::string::npos) {
        c.clockCode = "CHS-7GRK-MVNN-V3V4";
    }
    // item #3 — API key świadomie POMIJANY. Zegar autoryzuje się samym clock_code.
    // Jeśli w clock.json istnieje pole private_key, zostanie wczytane ale serwer
    // i tak je zignoruje. Nie dopisujemy żadnego hardcoded fallbacku.

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

// ─── Watchdog ───────────────────────────────────────────────────────────────

static std::atomic<uint32_t> g_main_heartbeat_ms{0};
static std::atomic<bool>     g_watchdog_running{false};
static const uint32_t        WATCHDOG_TIMEOUT_MS = 5000;

static void watchdog_thread_fn() {
    while (g_watchdog_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!g_watchdog_running.load()) break;
        uint32_t hb = g_main_heartbeat_ms.load();
        if (hb == 0) continue;
        uint32_t now = SDL_GetTicks();
        if (now - hb > WATCHDOG_TIMEOUT_MS) {
            std::fprintf(stderr,
                "[WATCHDOG] UI thread bez heartbeatu od %u ms — wymuszam exit(2)\n",
                now - hb);
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
            std::_Exit(2);
        }
    }
}

int main(int argc, char** argv) {
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
    app.bt_peer_max = (int)BluetoothServer::MAX_PEERS;

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

    g_main_heartbeat_ms.store(SDL_GetTicks());
    g_watchdog_running.store(true);
    std::thread watchdog_thr(watchdog_thread_fn);
    LOG_INFO("Watchdog started (timeout=" + std::to_string(WATCHDOG_TIMEOUT_MS) + " ms)");

    while (app_running) {
        uint32_t frame_begin = SDL_GetTicks();

        g_main_heartbeat_ms.store(frame_begin);

        if (frame_begin - last_frame_start > FRAME_WATCHDOG_MS) {
            LOG_WARN("Frame took " +
                     std::to_string(frame_begin - last_frame_start) + " ms (slow frame)");
        }
        last_frame_start = frame_begin;

        ui_process_events(&cs, &app, &api, sess, bt, &app_running);

        std::string line;
        int processed = 0;
        int src_peer = 0;
        while (processed < BT_PER_FRAME_MAX && bt.pop_line(line, src_peer)) {
            handleBluetoothCommand(line, src_peer, sess, app, cs, bt, api);
            processed++;
        }

        // Wymaganie #3 — komendy sędziego pobrane z serwera (panel arbitra).
        ServerClockCmd scmd;
        int sprocessed = 0;
        while (sprocessed < 32 && api.popServerCommand(scmd)) {
            applyServerCommand(scmd, sess, cs, bt);
            sprocessed++;
        }

        uint32_t now   = SDL_GetTicks();
        uint32_t delta = now - prev_tick;
        prev_tick      = now;
        update_clock(&cs, delta);
        checkTimeout(sess, cs, bt, api);
        tick_ack(bt, sess);

        // Aktualizuj counter peerów dla UI (draw_setup_screen + gate START).
        app.bt_peer_count = bt.connected_count();

        // Wymaganie #2 — snapshot oferty kodu do UI. ttl0/fetch_local odświeżamy
        // tylko gdy serwer poda nowy server_now (między pollami odliczanie biegnie
        // lokalnie po SDL_GetTicks, więc countdown jest płynny).
        {
            OfferStatusInfo ov = api.offerView();
            if (ov.has_offer) {
                if (ov.server_now != app.offer_server_now_seen) {
                    app.offer_ttl0 = (ov.expires_at > ov.server_now)
                                     ? (ov.expires_at - ov.server_now) : 0;
                    app.offer_fetch_local_ms  = SDL_GetTicks();
                    app.offer_server_now_seen = ov.server_now;
                }
                app.offer_active      = true;
                app.offer_code        = ov.code;
                app.offer_claimed     = ov.claimed;
                app.offer_target_kind = ov.target_kind;
                app.offer_target_name = ov.target_name;
                app.offer_table_no    = ov.table_no;
            } else {
                app.offer_active = false;
            }
        }

        if (now - last_clock_push > 500) {
            last_clock_push = now;
            // BT broadcast — gdy są podłączone tablety.
            if (bt.is_connected()) {
                std::ostringstream os;
                os << "CLOCK|" << cs.left.remaining_ms << "|" << cs.right.remaining_ms
                   << "|" << (cs.active == ACTIVE_LEFT ? "white" : "black")
                   << "|" << (int)cs.state;
                bt.send_line(os.str());
            }
            // Live push czasu na serwer (wymaganie #3) — jeżeli partia
            // istnieje na serwerze (game_id ustawione przez sendNewGame).
            // hbLoop sam wyśle co HB_INTERVAL_MS; tu tylko aktualizujemy cache.
            api.enqueueHeartbeat(cs.left.remaining_ms, cs.right.remaining_ms);
        }

        update_overlay_state(app, cs);

        ui_render_frame(app.renderer, &app, &cs);

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
