// Chess Relay Server
// ==================
// Endpoints:
//   POST /newgame  — create game (optionally with time control)
//   POST /move     — record a move + update clocks
//   POST /status   — set final status (checkmate / stalemate / timeout)
//   GET  /state    — query game list or single game (REST fallback)
//   GET  /events   — Server-Sent Events stream (push updates to browser)
//   GET  /         — embedded web viewer

package main

// ─── Chess Relay Server ─────────────────────────────────────────────────────
// Serwer przekaźnika dla zegara szachowego.
// Wczytuje konfigurację z config.json i wyświetla status ✅/❌
//
// Endpointy:
//   POST /newgame  — nowa gra
//   POST /move     — ruch + aktualizuj zegarki
//   POST /status   — status końcowy (mat / pat / timeout)
//   GET  /state    — pobierz listę gier lub jedną grę
//   GET  /events   — Server-Sent Events (push do przeglądarki)
//   GET  /         — wbudowana przeglądarka

import (
	"crypto/rand"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/bcrypt"
	_ "modernc.org/sqlite"
)

// ─── Konfiguracja globalna ────────────────────────────────────────────────────

type Config struct {
	Server   ServerConfig   `json:"server"`
	Database DatabaseConfig `json:"database"`
	Chess    ChessConfig    `json:"chess"`
	Files    FilesConfig    `json:"files"`
	Viewer   ViewerConfig   `json:"viewer"`
}

type ServerConfig struct {
	Port int    `json:"port"`
	Host string `json:"host"`
}

type DatabaseConfig struct {
	Path    string `json:"path"`
	WALMode bool   `json:"wal_mode"`
}

type ChessConfig struct {
	DefaultTimeControlMinutes int    `json:"default_time_control_minutes"`
	DefaultIncrementSeconds   int    `json:"default_increment_seconds"`
	WhitePlayerDefault        string `json:"white_player_default"`
	BlackPlayerDefault        string `json:"black_player_default"`
}

type FilesConfig struct {
	GamesDirectory string `json:"games_directory"`
	SavePGN        bool   `json:"save_pgn"`
	SaveJSON       bool   `json:"save_json"`
}

type ViewerConfig struct {
	ViewerFile           string `json:"viewer_file"`
	EnableSSE            bool   `json:"enable_sse"`
	KeepaliveIntervalSec int    `json:"keepalive_interval_seconds"`
}

// AdminCreds przechowuje dane logowania admina (zapisane w admin.json)
type AdminCreds struct {
	Username      string `json:"username"`
	PasswordHash  string `json:"password_hash"`
	SessionTTLMin int    `json:"session_ttl_minutes"`
}

var cfg Config

// ─── Hub SSE (Server-Sent Events) ─────────────────────────────────────────────
// Każda połączona przeglądarka dostaje jeden kanał. Gdy stan gry się zmieni,
// serwer rozgłasza event SSE do wszystkich subskrybentów.

type Hub struct {
	mu      sync.Mutex
	clients map[chan []byte]struct{}
}

var sseHub = &Hub{clients: make(map[chan []byte]struct{})}

// ─── Sesje administracyjne ────────────────────────────────────────────────────

var (
	adminCreds      AdminCreds
	adminSessions   = map[string]time.Time{}
	adminSessionsMu sync.Mutex
)

func (h *Hub) Subscribe() chan []byte {
	ch := make(chan []byte, 32)
	h.mu.Lock()
	h.clients[ch] = struct{}{}
	h.mu.Unlock()
	return ch
}

func (h *Hub) Unsubscribe(ch chan []byte) {
	h.mu.Lock()
	delete(h.clients, ch)
	h.mu.Unlock()
	// drain buffered messages before closing
	for len(ch) > 0 {
		<-ch
	}
	close(ch)
}

// Rozgłaszanie SSE do wszystkich subskrybentów
// Kanały wolne lub pełne są ignorowane (non-blocking select).
func (h *Hub) Broadcast(eventType string, data []byte) {
	frame := []byte(fmt.Sprintf("event: %s\ndata: %s\n\n", eventType, data))
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.clients {
		select {
		case ch <- frame:
		default:
		}
	}
}

// ─── Baza danych ──────────────────────────────────────────────────────────────

var (
	db *sql.DB
	mu sync.Mutex // chroni wszystkie zapisy do BD
)

type MoveRecord struct {
	Move      string `json:"move"`
	Player    string `json:"player"`
	Timestamp string `json:"timestamp"`
	MoveNum   int    `json:"move_number"`
}

type GameState struct {
	ID          int64        `json:"id"`
	WhitePlayer string       `json:"white_player"`
	BlackPlayer string       `json:"black_player"`
	Moves       []MoveRecord `json:"moves"`
	Status      string       `json:"status"`
	Winner      string       `json:"winner"`
	CreatedAt   string       `json:"created_at"`
	TimeControl int          `json:"time_control"`  // seconds per side, 0 = unlimited
	WhiteTimeMs int          `json:"white_time_ms"` // remaining for white
	BlackTimeMs int          `json:"black_time_ms"` // remaining for black
}

func initDB() error {
	var err error
	db, err = sql.Open("sqlite", cfg.Database.Path)
	if err != nil {
		return err
	}
	if cfg.Database.WALMode {
		db.Exec("PRAGMA journal_mode=WAL")
	}

	_, err = db.Exec(`CREATE TABLE IF NOT EXISTS games (
		id             INTEGER PRIMARY KEY AUTOINCREMENT,
		white_player   TEXT    NOT NULL DEFAULT 'White',
		black_player   TEXT    NOT NULL DEFAULT 'Black',
		status         TEXT    NOT NULL DEFAULT 'ongoing',
		winner         TEXT    NOT NULL DEFAULT '',
		created_at     TEXT    NOT NULL,
		time_control   INTEGER NOT NULL DEFAULT 0,
		white_time_ms  INTEGER NOT NULL DEFAULT 0,
		black_time_ms  INTEGER NOT NULL DEFAULT 0
	)`)
	if err != nil {
		return err
	}

	// Migrate pre-existing databases that don't have the clock columns yet.
	// SQLite returns an error on duplicate columns — we intentionally ignore it.
	db.Exec("ALTER TABLE games ADD COLUMN time_control  INTEGER NOT NULL DEFAULT 0")
	db.Exec("ALTER TABLE games ADD COLUMN white_time_ms INTEGER NOT NULL DEFAULT 0")
	db.Exec("ALTER TABLE games ADD COLUMN black_time_ms INTEGER NOT NULL DEFAULT 0")

	_, err = db.Exec(`CREATE TABLE IF NOT EXISTS moves (
		id          INTEGER PRIMARY KEY AUTOINCREMENT,
		game_id     INTEGER NOT NULL,
		move_number INTEGER NOT NULL,
		player      TEXT    NOT NULL,
		move        TEXT    NOT NULL,
		timestamp   TEXT    NOT NULL,
		FOREIGN KEY (game_id) REFERENCES games(id)
	)`)
	return err
}

func getGame(id int64) (*GameState, error) {
	row := db.QueryRow(`
		SELECT id, white_player, black_player, status, winner, created_at,
		       time_control, white_time_ms, black_time_ms
		FROM games WHERE id = ?`, id)
	g := &GameState{}
	err := row.Scan(&g.ID, &g.WhitePlayer, &g.BlackPlayer,
		&g.Status, &g.Winner, &g.CreatedAt,
		&g.TimeControl, &g.WhiteTimeMs, &g.BlackTimeMs)
	if err != nil {
		return nil, err
	}
	rows, err := db.Query(
		"SELECT move_number, player, move, timestamp FROM moves WHERE game_id = ? ORDER BY move_number", id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var m MoveRecord
		rows.Scan(&m.MoveNum, &m.Player, &m.Move, &m.Timestamp)
		g.Moves = append(g.Moves, m)
	}
	if g.Moves == nil {
		g.Moves = []MoveRecord{}
	}
	return g, nil
}

func getAllGames() ([]*GameState, error) {
	rows, err := db.Query(`
		SELECT id, white_player, black_player, status, winner, created_at,
		       time_control, white_time_ms, black_time_ms
		FROM games ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var games []*GameState
	for rows.Next() {
		g := &GameState{Moves: []MoveRecord{}}
		rows.Scan(&g.ID, &g.WhitePlayer, &g.BlackPlayer,
			&g.Status, &g.Winner, &g.CreatedAt,
			&g.TimeControl, &g.WhiteTimeMs, &g.BlackTimeMs)
		games = append(games, g)
	}
	return games, nil
}

// broadcastUpdate pushes the current state to all SSE clients.
// It always sends the full game list (for the sidebar) and, when gameID > 0,
// the full game detail including moves (for the active viewer).
func broadcastUpdate(gameID int64) {
	if games, err := getAllGames(); err == nil {
		if data, err := json.Marshal(games); err == nil {
			sseHub.Broadcast("list", data)
		}
	}
	if gameID > 0 {
		if game, err := getGame(gameID); err == nil {
			if data, err := json.Marshal(game); err == nil {
				sseHub.Broadcast("game", data)
			}
		}
	}
}

// ─── File export ──────────────────────────────────────────────────────────────

func saveGameFiles(game *GameState) {
	os.MkdirAll(cfg.Files.GamesDirectory, 0755)

	result := "*"
	switch game.Winner {
	case "White":
		result = "1-0"
	case "Black":
		result = "0-1"
	case "Draw":
		result = "1/2-1/2"
	}

	if cfg.Files.SavePGN {
		var pgn strings.Builder
		pgn.WriteString(fmt.Sprintf("[Event \"Chess Game #%d\"]\n", game.ID))
		pgn.WriteString("[Site \"Local Network\"]\n")
		pgn.WriteString(fmt.Sprintf("[Date \"%s\"]\n", time.Now().Format("2006.01.02")))
		pgn.WriteString(fmt.Sprintf("[White \"%s\"]\n", game.WhitePlayer))
		pgn.WriteString(fmt.Sprintf("[Black \"%s\"]\n", game.BlackPlayer))
		pgn.WriteString(fmt.Sprintf("[Result \"%s\"]\n", result))
		if game.TimeControl > 0 {
			pgn.WriteString(fmt.Sprintf("[TimeControl \"%d\"]\n", game.TimeControl))
		}
		pgn.WriteString("\n")
		for i, m := range game.Moves {
			if i%2 == 0 {
				pgn.WriteString(fmt.Sprintf("%d. ", (i/2)+1))
			}
			pgn.WriteString(m.Move + " ")
		}
		pgn.WriteString(result + "\n")
		os.WriteFile(fmt.Sprintf("%s/%d.pgn", cfg.Files.GamesDirectory, game.ID), []byte(pgn.String()), 0644)
	}

	if cfg.Files.SaveJSON {
		type GameFile struct {
			ID          int64        `json:"id"`
			WhitePlayer string       `json:"white_player"`
			BlackPlayer string       `json:"black_player"`
			Status      string       `json:"status"`
			Winner      string       `json:"winner"`
			Result      string       `json:"result"`
			CreatedAt   string       `json:"created_at"`
			TimeControl int          `json:"time_control"`
			Moves       []MoveRecord `json:"moves"`
		}
		gf := GameFile{
			ID: game.ID, WhitePlayer: game.WhitePlayer, BlackPlayer: game.BlackPlayer,
			Status: game.Status, Winner: game.Winner, Result: result,
			CreatedAt: game.CreatedAt, TimeControl: game.TimeControl, Moves: game.Moves,
		}
		jsonBytes, _ := json.MarshalIndent(gf, "", "  ")
		os.WriteFile(fmt.Sprintf("%s/%d.json", cfg.Files.GamesDirectory, game.ID), jsonBytes, 0644)
	}

	files := ""
	if cfg.Files.SavePGN { files = "PGN" }
	if cfg.Files.SaveJSON {
		if files != "" { files += " + " }
		files += "JSON"
	}
	fmt.Printf("[PLIKI] Zapisano grę %d (%s)\n", game.ID, files)
}

// ─── Console output ───────────────────────────────────────────────────────────

func printGamesTable() {
	rows, err := db.Query(
		"SELECT id, white_player, black_player, status, winner, created_at FROM games ORDER BY id")
	if err != nil {
		return
	}
	defer rows.Close()
	fmt.Println()
	fmt.Println("┌────┬──────────────┬──────────────┬──────────────────────┬────────────┬──────────┐")
	fmt.Println("│ ID │ White        │ Black        │ Started              │ Status     │ Winner   │")
	fmt.Println("├────┼──────────────┼──────────────┼──────────────────────┼────────────┼──────────┤")
	hasRows := false
	for rows.Next() {
		hasRows = true
		var id int64
		var white, black, status, winner, createdAt string
		rows.Scan(&id, &white, &black, &status, &winner, &createdAt)
		if winner == "" {
			winner = "—"
		}
		fmt.Printf("│ %-2d │ %-12s │ %-12s │ %-20s │ %-10s │ %-8s │\n",
			id, white, black, createdAt, status, winner)
	}
	if !hasRows {
		fmt.Println("│                          (no games yet)                                       │")
	}
	fmt.Println("└────┴──────────────┴──────────────┴──────────────────────┴────────────┴──────────┘")
	fmt.Println()
}

// ─── Handlers ─────────────────────────────────────────────────────────────────

// POST /newgame — rozpoczyna nową grę
// Klient C++ wysyła: { "white_player":"...", "black_player":"...", "time_control_ms": 600000 }
// Legacy format:      { "white_player":"...", "black_player":"...", "time_control_sec": 600 }
func handleNewGame(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var payload struct {
		WhitePlayer    string `json:"white_player"`
		BlackPlayer    string `json:"black_player"`
		TimeControlMs  int    `json:"time_control_ms"`  // C++ client (milliseconds)
		TimeControlSec int    `json:"time_control_sec"` // legacy (seconds)
	}
	json.NewDecoder(r.Body).Decode(&payload)
	if payload.WhitePlayer == "" {
		payload.WhitePlayer = cfg.Chess.WhitePlayerDefault
	}
	if payload.BlackPlayer == "" {
		payload.BlackPlayer = cfg.Chess.BlackPlayerDefault
	}

	// Normalise: C++ sends ms, legacy sends sec
	initialMs := 0
	if payload.TimeControlMs > 0 {
		initialMs = payload.TimeControlMs
	} else if payload.TimeControlSec > 0 {
		initialMs = payload.TimeControlSec * 1000
	}
	timeControlSec := initialMs / 1000 // store as seconds in DB

	now := time.Now().Format("2006-01-02 15:04:05")

	mu.Lock()
	result, err := db.Exec(`
		INSERT INTO games
		    (white_player, black_player, status, winner, created_at,
		     time_control, white_time_ms, black_time_ms)
		VALUES (?, ?, 'ongoing', '', ?, ?, ?, ?)`,
		payload.WhitePlayer, payload.BlackPlayer, now,
		timeControlSec, initialMs, initialMs,
	)
	mu.Unlock()
	if err != nil {
		http.Error(w, "DB error: "+err.Error(), http.StatusInternalServerError)
		return
	}

	id, _ := result.LastInsertId()
	tcLabel := "unlimited"
	if initialMs > 0 {
		tcLabel = fmt.Sprintf("%d min/side", timeControlSec/60)
	}
	fmt.Printf("\n[NEW GAME] #%d | %s vs %s | Time: %s | %s\n",
		id, payload.WhitePlayer, payload.BlackPlayer, tcLabel, now)
	printGamesTable()

	broadcastUpdate(id)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{"game_id": id})
}

// POST /move — rejestruje ruch i aktualizuje zegarki
// Ciało: { "game_id": N, "move": "e2e4", "player": "White", "time_left_ms": N }
func handleMove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	// Klient C++ wysyła time_left_ms (tylko jedna strona).
	// Legacy format: white_time_ms + black_time_ms (obie strony).
	var payload struct {
		GameID      int64  `json:"game_id"`
		Move        string `json:"move"`
		Player      string `json:"player"`
		TimeLeftMs  int    `json:"time_left_ms"`  // C++ client
		WhiteTimeMs int    `json:"white_time_ms"` // legacy
		BlackTimeMs int    `json:"black_time_ms"` // legacy
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Bad request", http.StatusBadRequest)
		return
	}

	now := time.Now().Format("15:04:05")

	mu.Lock()

	// Expand time_left_ms into white/black by fetching the other side from DB
	if payload.TimeLeftMs > 0 {
		var curW, curB int
		db.QueryRow("SELECT white_time_ms, black_time_ms FROM games WHERE id = ?",
			payload.GameID).Scan(&curW, &curB)
		if payload.Player == "White" {
			payload.WhiteTimeMs = payload.TimeLeftMs
			payload.BlackTimeMs = curB
		} else {
			payload.BlackTimeMs = payload.TimeLeftMs
			payload.WhiteTimeMs = curW
		}
	}

	var moveNum int
	db.QueryRow("SELECT COUNT(*) FROM moves WHERE game_id = ?", payload.GameID).Scan(&moveNum)
	moveNum++
	_, err := db.Exec(
		"INSERT INTO moves (game_id, move_number, player, move, timestamp) VALUES (?, ?, ?, ?, ?)",
		payload.GameID, moveNum, payload.Player, payload.Move, now,
	)
	if err == nil && (payload.WhiteTimeMs > 0 || payload.BlackTimeMs > 0) {
		db.Exec("UPDATE games SET white_time_ms = ?, black_time_ms = ? WHERE id = ?",
			payload.WhiteTimeMs, payload.BlackTimeMs, payload.GameID)
	}
	mu.Unlock()

	if err != nil {
		http.Error(w, "DB error: "+err.Error(), http.StatusInternalServerError)
		return
	}

	fmt.Printf("[%s] Game %-3d | Move #%-3d | %-6s plays %s  (W:%dms B:%dms)\n",
		now, payload.GameID, moveNum, payload.Player, payload.Move,
		payload.WhiteTimeMs, payload.BlackTimeMs)

	broadcastUpdate(payload.GameID)

	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true,"move_number":%d}`, moveNum)
}

func handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var payload struct {
		GameID int64  `json:"game_id"`
		Status string `json:"status"`
		Winner string `json:"winner"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Bad request", http.StatusBadRequest)
		return
	}

	mu.Lock()
	db.Exec("UPDATE games SET status = ?, winner = ? WHERE id = ?",
		payload.Status, payload.Winner, payload.GameID)
	mu.Unlock()

	fmt.Printf("\n[GAME OVER] Game %d | %s | Winner: %s\n",
		payload.GameID, payload.Status, payload.Winner)
	printGamesTable()

	broadcastUpdate(payload.GameID)

	if game, err := getGame(payload.GameID); err == nil {
		saveGameFiles(game)
	}

	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// GET /state[?id=N] — zapytanie o stan gry
// Fallback REST — przeglądarka używa SSE, ale endpoint ten jest dla narzędzi zewnętrznych.
func handleState(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	idStr := r.URL.Query().Get("id")
	if idStr == "" {
		games, err := getAllGames()
		if err != nil {
			http.Error(w, "DB error", http.StatusInternalServerError)
			return
		}
		if games == nil {
			games = []*GameState{}
		}
		json.NewEncoder(w).Encode(games)
		return
	}

	var id int64
	fmt.Sscan(idStr, &id)
	game, err := getGame(id)
	if err != nil {
		http.Error(w, "Game not found", http.StatusNotFound)
		return
	}
	json.NewEncoder(w).Encode(game)
}

// GET /events — strumień Server-Sent Events
// Przeglądarka utrzymuje jedno długotrwałe połączenie zamiast pytać /state co sekundę.
//
// Wysyłane typy zdarzeń:
//
//	list  — tablica JSON wszystkich gier (podsumowanie bez ruchów) — przy każdej zmianie
//	game  — obiekt JSON jednej gry (pełne info + ruchy) — przy ruchu lub statusie
func handleEvents(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "SSE not supported by this server", http.StatusInternalServerError)
		return
	}

	// Send the current game list immediately so the sidebar shows right away.
	if games, err := getAllGames(); err == nil {
		if data, err := json.Marshal(games); err == nil {
			fmt.Fprintf(w, "event: list\ndata: %s\n\n", data)
			flusher.Flush()
		}
	}

	ch := sseHub.Subscribe()
	defer sseHub.Unsubscribe(ch)

	// Keep-alive comment — zapobiega zamykaniu połączenia przez proxy
	ticker := time.NewTicker(time.Duration(cfg.Viewer.KeepaliveIntervalSec) * time.Second)
	defer ticker.Stop()

	for {
		select {
		case msg, ok := <-ch:
			if !ok {
				return
			}
			w.Write(msg)
			flusher.Flush()
		case <-ticker.C:
			fmt.Fprintf(w, ": keepalive\n\n")
			flusher.Flush()
		case <-r.Context().Done():
			return
		}
	}
}

func handleViewer(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	// Wczytaj viewer.html z dysku jeśli istnieje
	if data, err := os.ReadFile(cfg.Viewer.ViewerFile); err == nil {
		w.Write(data)
		return
	}
	// Fallback: wbudowana minimalna strona
	fmt.Fprint(w, viewerFallback)
}

// ─── Autoryzacja admina ───────────────────────────────────────────────────────

// adminRandToken generuje kryptograficznie bezpieczny token sesji (base64url, 32 bajty).
func adminRandToken() string {
	b := make([]byte, 32)
	rand.Read(b)
	return base64.URLEncoding.EncodeToString(b)
}

// newAdminSession tworzy nową sesję i zwraca jej token.
func newAdminSession() string {
	token := adminRandToken()
	ttl := time.Duration(adminCreds.SessionTTLMin) * time.Minute
	if ttl == 0 {
		ttl = 60 * time.Minute
	}
	adminSessionsMu.Lock()
	adminSessions[token] = time.Now().Add(ttl)
	adminSessionsMu.Unlock()
	return token
}

// checkAdminSession sprawdza czy token sesji jest ważny (nie wygasł).
func checkAdminSession(token string) bool {
	if token == "" {
		return false
	}
	adminSessionsMu.Lock()
	defer adminSessionsMu.Unlock()
	exp, ok := adminSessions[token]
	if !ok || time.Now().After(exp) {
		delete(adminSessions, token)
		return false
	}
	return true
}

// adminToken wyciąga token sesji z ciasteczka lub nagłówka X-Admin-Token.
func adminToken(r *http.Request) string {
	if c, err := r.Cookie("adm"); err == nil {
		return c.Value
	}
	return r.Header.Get("X-Admin-Token")
}

// adminAPI — middleware JSON 401 dla nieautoryzowanych żądań API.
func adminAPI(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !checkAdminSession(adminToken(r)) {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusUnauthorized)
			fmt.Fprint(w, `{"ok":false,"error":"unauthorized"}`)
			return
		}
		next(w, r)
	}
}

// loadAdminCreds wczytuje admin.json lub generuje nowe hasło przy pierwszym uruchomieniu.
func loadAdminCreds() {
	data, err := os.ReadFile("admin.json")
	if err == nil && json.Unmarshal(data, &adminCreds) == nil && adminCreds.PasswordHash != "" {
		fmt.Println("✅ Dane administratora wczytane")
		return
	}
	// Wygeneruj nowe losowe hasło (12 znaków, CSPRNG)
	raw := make([]byte, 12)
	rand.Read(raw)
	const chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	pass := make([]byte, 12)
	for i, c := range raw {
		pass[i] = chars[int(c)%len(chars)]
	}
	password := string(pass)
	hash, _ := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	adminCreds = AdminCreds{
		Username:      "admin",
		PasswordHash:  string(hash),
		SessionTTLMin: 60,
	}
	out, _ := json.MarshalIndent(adminCreds, "", "  ")
	os.WriteFile("admin.json", out, 0600)
	fmt.Println("✅ Wygenerowano dane administratora (pierwsze uruchomienie):")
	fmt.Printf("   ┌──────────────────────────────────┐\n")
	fmt.Printf("   │  Użytkownik : %-19s│\n", adminCreds.Username)
	fmt.Printf("   │  Hasło      : %-19s│\n", password)
	fmt.Printf("   └──────────────────────────────────┘\n")
	fmt.Println("   Hash zapisany do admin.json — zaloguj się na /admin")
}

// ─── Handlery admina ─────────────────────────────────────────────────────────

// POST /admin/api/login — weryfikacja hasła bcrypt, ustawia ciasteczko sesji.
func handleAdminLogin(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		fmt.Fprint(w, `{"ok":false,"error":"method_not_allowed"}`)
		return
	}
	var creds struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&creds); err != nil {
		w.WriteHeader(http.StatusBadRequest)
		fmt.Fprint(w, `{"ok":false,"error":"invalid_body"}`)
		return
	}
	// Stały czas odpowiedzi (ochrona przed timing attack na nazwę użytkownika)
	hashToCheck := adminCreds.PasswordHash
	err := bcrypt.CompareHashAndPassword([]byte(hashToCheck), []byte(creds.Password))
	if creds.Username != adminCreds.Username || err != nil {
		w.WriteHeader(http.StatusUnauthorized)
		fmt.Fprint(w, `{"ok":false,"error":"nieprawidłowe dane logowania"}`)
		return
	}
	token := newAdminSession()
	ttl := time.Duration(adminCreds.SessionTTLMin) * time.Minute
	if ttl == 0 {
		ttl = 60 * time.Minute
	}
	http.SetCookie(w, &http.Cookie{
		Name:     "adm",
		Value:    token,
		Path:     "/",
		Expires:  time.Now().Add(ttl),
		HttpOnly: true,
		SameSite: http.SameSiteStrictMode,
	})
	fmt.Printf("[ADMIN] Zalogowano: %s\n", adminCreds.Username)
	fmt.Fprint(w, `{"ok":true}`)
}

// POST /admin/api/logout — niszczy sesję, czyści ciasteczko.
func handleAdminLogout(w http.ResponseWriter, r *http.Request) {
	token := adminToken(r)
	if token != "" {
		adminSessionsMu.Lock()
		delete(adminSessions, token)
		adminSessionsMu.Unlock()
	}
	http.SetCookie(w, &http.Cookie{Name: "adm", Path: "/", MaxAge: -1})
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprint(w, `{"ok":true}`)
}

// GET /admin — panel administracyjny (serwuje admin.html)
func handleAdminDashboard(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	if data, err := os.ReadFile("admin.html"); err == nil {
		w.Write(data)
		return
	}
	fmt.Fprint(w, adminFallback)
}

// GET /test — panel arbitra i diagnostyki.
func handleTestDashboard(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	if data, err := os.ReadFile("test.html"); err == nil {
		w.Write(data)
		return
	}
	http.Error(w, "test.html not found", http.StatusNotFound)
}

// ─── ENDPOINTY ARBITRA (Art. 6 i 7 Przepisów FIDE) ─────────────────────────

// POST /arbiter/adjust-clock
// Korekta czasu dowolnego zawodnika (Art. 6.10.2, 7.1).
// { "game_id": N, "white_time_ms": N, "black_time_ms": N, "reason": "..." }
func handleArbiterAdjustClock(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID      int64  `json:"game_id"`
		WhiteTimeMs *int   `json:"white_time_ms"`
		BlackTimeMs *int   `json:"black_time_ms"`
		Reason      string `json:"reason"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	if p.WhiteTimeMs != nil && p.BlackTimeMs != nil {
		db.Exec("UPDATE games SET white_time_ms=?, black_time_ms=? WHERE id=?",
			*p.WhiteTimeMs, *p.BlackTimeMs, p.GameID)
	} else if p.WhiteTimeMs != nil {
		db.Exec("UPDATE games SET white_time_ms=? WHERE id=?", *p.WhiteTimeMs, p.GameID)
	} else if p.BlackTimeMs != nil {
		db.Exec("UPDATE games SET black_time_ms=? WHERE id=?", *p.BlackTimeMs, p.GameID)
	}
	fmt.Printf("[ARBITER] AdjustClock game=%d reason=%q\n", p.GameID, p.Reason)
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/add-time
// Bonifikata czasu (Art. 7.5.5 — 2 min za nielegalne posunięcie przeciwnika).
// { "game_id": N, "player": "White"|"Black", "add_ms": N, "reason": "..." }
func handleArbiterAddTime(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID int64  `json:"game_id"`
		Player string `json:"player"`
		AddMs  int    `json:"add_ms"`
		Reason string `json:"reason"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	if p.Player == "White" {
		db.Exec("UPDATE games SET white_time_ms = white_time_ms + ? WHERE id = ?", p.AddMs, p.GameID)
	} else {
		db.Exec("UPDATE games SET black_time_ms = black_time_ms + ? WHERE id = ?", p.AddMs, p.GameID)
	}
	fmt.Printf("[ARBITER] AddTime game=%d player=%s +%dms reason=%q\n", p.GameID, p.Player, p.AddMs, p.Reason)
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/pause
// Zatrzymanie zegara przez sędziego (Art. 6.11.1).
// { "game_id": N, "reason": "..." }
// Implementacja: ustawia status na "paused" (nie-finalne), broadcast.
func handleArbiterPause(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID int64  `json:"game_id"`
		Reason string `json:"reason"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	db.Exec("UPDATE games SET status='paused' WHERE id=? AND status='ongoing'", p.GameID)
	fmt.Printf("[ARBITER] Pause game=%d reason=%q\n", p.GameID, p.Reason)
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/resume
// Wznowienie partii po zatrzymaniu (Art. 6.11.3).
// { "game_id": N }
func handleArbiterResume(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID int64 `json:"game_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	db.Exec("UPDATE games SET status='ongoing' WHERE id=? AND status='paused'", p.GameID)
	fmt.Printf("[ARBITER] Resume game=%d\n", p.GameID)
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/delete-move
// Usunięcie ostatniego ruchu (Art. 7.5.1 — przywrócenie pozycji po nieprawidłowym ruchu).
// { "game_id": N, "reason": "..." }
func handleArbiterDeleteMove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID int64  `json:"game_id"`
		Reason string `json:"reason"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	db.Exec(`DELETE FROM moves WHERE id = (
		SELECT id FROM moves WHERE game_id=? ORDER BY move_number DESC LIMIT 1
	)`, p.GameID)
	fmt.Printf("[ARBITER] DeleteLastMove game=%d reason=%q\n", p.GameID, p.Reason)
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/void
// Unieważnienie partii — reset ruchów i statusu (Art. 7.2.1).
// { "game_id": N, "reason": "..." }
func handleArbiterVoid(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID int64  `json:"game_id"`
		Reason string `json:"reason"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	db.Exec("DELETE FROM moves WHERE game_id=?", p.GameID)
	db.Exec("UPDATE games SET status='ongoing', winner='' WHERE id=?", p.GameID)
	fmt.Printf("[ARBITER] VoidGame game=%d reason=%q\n", p.GameID, p.Reason)
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/forfeit
// Porażka przez arbitra — kara za 2. nielegalny ruch lub spóźnienie (Art. 7.5.5, 6.7.1).
// { "game_id": N, "loser": "White"|"Black", "reason": "..." }
func handleArbiterForfeit(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID int64  `json:"game_id"`
		Loser  string `json:"loser"`
		Reason string `json:"reason"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	winner := "Black"
	if p.Loser == "Black" {
		winner = "White"
	}
	mu.Lock()
	defer mu.Unlock()
	db.Exec("UPDATE games SET status='forfeit', winner=? WHERE id=?", winner, p.GameID)
	fmt.Printf("[ARBITER] Forfeit game=%d loser=%s winner=%s reason=%q\n", p.GameID, p.Loser, winner, p.Reason)
	broadcastUpdate(p.GameID)
	if game, err := getGame(p.GameID); err == nil {
		saveGameFiles(game)
	}
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true,"winner":"%s"}`, winner)
}

// POST /arbiter/draw
// Orzeczenie remisu przez arbitra (Art. 6.9 — brak możliwości mata).
// { "game_id": N, "reason": "..." }
func handleArbiterDraw(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID int64  `json:"game_id"`
		Reason string `json:"reason"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	db.Exec("UPDATE games SET status='draw', winner='Draw' WHERE id=?", p.GameID)
	fmt.Printf("[ARBITER] Draw game=%d reason=%q\n", p.GameID, p.Reason)
	broadcastUpdate(p.GameID)
	if game, err := getGame(p.GameID); err == nil {
		saveGameFiles(game)
	}
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/set-players
// Zmiana danych zawodników (Art. 7.3 — błędne kolory przy < 10 ruchach).
// { "game_id": N, "white_player": "...", "black_player": "...", "swap": true/false }
func handleArbiterSetPlayers(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID      int64  `json:"game_id"`
		WhitePlayer string `json:"white_player"`
		BlackPlayer string `json:"black_player"`
		Swap        bool   `json:"swap"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	if p.Swap {
		// Zamień kolory: pobierz obecnych i odwróć
		var w2, b2 string
		db.QueryRow("SELECT white_player, black_player FROM games WHERE id=?", p.GameID).Scan(&w2, &b2)
		db.Exec("UPDATE games SET white_player=?, black_player=? WHERE id=?", b2, w2, p.GameID)
		fmt.Printf("[ARBITER] SwapColors game=%d: %s <-> %s\n", p.GameID, w2, b2)
	} else {
		if p.WhitePlayer != "" {
			db.Exec("UPDATE games SET white_player=? WHERE id=?", p.WhitePlayer, p.GameID)
		}
		if p.BlackPlayer != "" {
			db.Exec("UPDATE games SET black_player=? WHERE id=?", p.BlackPlayer, p.GameID)
		}
		fmt.Printf("[ARBITER] SetPlayers game=%d white=%q black=%q\n", p.GameID, p.WhitePlayer, p.BlackPlayer)
	}
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// POST /arbiter/set-timecontrol
// Zmiana kontroli czasu (wymiana zegara, Art. 6.10.1).
// { "game_id": N, "time_control_sec": N, "white_time_ms": N, "black_time_ms": N }
func handleArbiterSetTimeControl(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		jsonErr(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var p struct {
		GameID         int64 `json:"game_id"`
		TimeControlSec int   `json:"time_control_sec"`
		WhiteTimeMs    int   `json:"white_time_ms"`
		BlackTimeMs    int   `json:"black_time_ms"`
	}
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		jsonErr(w, "bad request", http.StatusBadRequest)
		return
	}
	mu.Lock()
	defer mu.Unlock()
	db.Exec("UPDATE games SET time_control=?, white_time_ms=?, black_time_ms=? WHERE id=?",
		p.TimeControlSec, p.WhiteTimeMs, p.BlackTimeMs, p.GameID)
	fmt.Printf("[ARBITER] SetTimeControl game=%d tc=%ds W=%dms B=%dms\n",
		p.GameID, p.TimeControlSec, p.WhiteTimeMs, p.BlackTimeMs)
	broadcastUpdate(p.GameID)
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

// GET /arbiter — serwuje panel arbitra.
func handleArbiterDashboard(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	if data, err := os.ReadFile("arbiter.html"); err == nil {
		w.Write(data)
		return
	}
	http.Error(w, "arbiter.html not found", http.StatusNotFound)
}

// jsonErr — pomocnik dla odpowiedzi błędu JSON.
func jsonErr(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	fmt.Fprintf(w, `{"ok":false,"error":%q}`, msg)
}

// GET /admin/api/stats — statystyki serwera (wymaga sesji).
func handleAdminAPIStats(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	var total, ongoing, totalMoves int
	db.QueryRow("SELECT COUNT(*) FROM games").Scan(&total)
	db.QueryRow("SELECT COUNT(*) FROM games WHERE status='ongoing'").Scan(&ongoing)
	db.QueryRow("SELECT COUNT(*) FROM moves").Scan(&totalMoves)
	adminSessionsMu.Lock()
	activeSessions := len(adminSessions)
	adminSessionsMu.Unlock()
	json.NewEncoder(w).Encode(map[string]interface{}{
		"ok":              true,
		"total_games":     total,
		"ongoing":         ongoing,
		"finished":        total - ongoing,
		"total_moves":     totalMoves,
		"active_sessions": activeSessions,
		"db_path":         cfg.Database.Path,
		"games_dir":       cfg.Files.GamesDirectory,
		"port":            cfg.Server.Port,
	})
}

// GET /admin/api/games — lista wszystkich gier (wymaga sesji).
func handleAdminAPIGames(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	games, err := getAllGames()
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprint(w, `{"ok":false,"error":"db_error"}`)
		return
	}
	if games == nil {
		games = []*GameState{}
	}
	json.NewEncoder(w).Encode(map[string]interface{}{"ok": true, "games": games})
}

// DELETE /admin/api/game?id=N — usuwa grę i jej ruchy (wymaga sesji).
func handleAdminAPIDeleteGame(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodDelete {
		w.WriteHeader(http.StatusMethodNotAllowed)
		fmt.Fprint(w, `{"ok":false,"error":"method_not_allowed"}`)
		return
	}
	idStr := r.URL.Query().Get("id")
	if idStr == "" {
		w.WriteHeader(http.StatusBadRequest)
		fmt.Fprint(w, `{"ok":false,"error":"missing_id"}`)
		return
	}
	var id int64
	fmt.Sscan(idStr, &id)
	mu.Lock()
	db.Exec("DELETE FROM moves WHERE game_id = ?", id)
	res, err := db.Exec("DELETE FROM games WHERE id = ?", id)
	mu.Unlock()
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprint(w, `{"ok":false,"error":"db_error"}`)
		return
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		w.WriteHeader(http.StatusNotFound)
		fmt.Fprint(w, `{"ok":false,"error":"not_found"}`)
		return
	}
	fmt.Printf("[ADMIN] Usunięto grę #%d\n", id)
	broadcastUpdate(0)
	fmt.Fprint(w, `{"ok":true}`)
}

// POST /admin/api/change-password — zmiana hasła admina (wymaga sesji + starego hasła).
func handleAdminAPIChangePassword(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		w.WriteHeader(http.StatusBadRequest)
		fmt.Fprint(w, `{"ok":false,"error":"invalid_body"}`)
		return
	}
	if err := bcrypt.CompareHashAndPassword([]byte(adminCreds.PasswordHash), []byte(body.OldPassword)); err != nil {
		w.WriteHeader(http.StatusUnauthorized)
		fmt.Fprint(w, `{"ok":false,"error":"złe aktualne hasło"}`)
		return
	}
	if len(body.NewPassword) < 8 {
		w.WriteHeader(http.StatusBadRequest)
		fmt.Fprint(w, `{"ok":false,"error":"hasło za krótkie (min. 8 znaków)"}`)
		return
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(body.NewPassword), bcrypt.DefaultCost)
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		return
	}
	adminCreds.PasswordHash = string(hash)
	out, _ := json.MarshalIndent(adminCreds, "", "  ")
	os.WriteFile("admin.json", out, 0600)
	// Unieważnij wszystkie aktywne sesje po zmianie hasła
	adminSessionsMu.Lock()
	adminSessions = map[string]time.Time{}
	adminSessionsMu.Unlock()
	fmt.Println("[ADMIN] Hasło zmienione, wszystkie sesje unieważnione")
	fmt.Fprint(w, `{"ok":true}`)
}

// ─── Funkcje pomocnicze ───────────────────────────────────────────────────────

func getLocalIP() string {
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		return "unknown"
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).IP.String()
}

// ─── Punkt wejścia ───────────────────────────────────────────────────────────

func main() {
	fmt.Println("♟ Ładowanie konfiguracji...")
	fmt.Println()

	// Wczytaj config.json
	configData, err := os.ReadFile("config.json")
	if err != nil {
		fmt.Println("❌ Błąd: nie znaleziono config.json")
		return
	}
	if err := json.Unmarshal(configData, &cfg); err != nil {
		fmt.Println("❌ Błąd: nie można sparsować config.json")
		fmt.Println("   ", err)
		return
	}
	fmt.Println("✅ Konfiguracja wczytana")

	// Inicjalizuj bazę danych
	if err := initDB(); err != nil {
		fmt.Println("❌ Błąd inicjalizacji bazy danych:", err)
		return
	}
	defer db.Close()
	fmt.Println("✅ Baza danych gotowa")

	// Utwórz katalog dla gier
	if err := os.MkdirAll(cfg.Files.GamesDirectory, 0755); err != nil {
		fmt.Println("❌ Błąd tworzenia katalogu gier:", err)
		return
	}
	fmt.Println("✅ Katalog gier gotów")

	ip := getLocalIP()
	portStr := fmt.Sprintf("%d", cfg.Server.Port)

	// Wczytaj lub wygeneruj dane administratora
	loadAdminCreds()

	// Zarejestruj handlery HTTP
	http.HandleFunc("/newgame", handleNewGame)
	http.HandleFunc("/move", handleMove)
	http.HandleFunc("/status", handleStatus)
	http.HandleFunc("/state", handleState)
	if cfg.Viewer.EnableSSE {
		http.HandleFunc("/events", handleEvents)
	}
	http.HandleFunc("/", handleViewer)

	// Endpointy panelu administracyjnego
	http.HandleFunc("/admin", handleAdminDashboard)
	http.HandleFunc("/test", handleTestDashboard)
	http.HandleFunc("/admin/api/login", handleAdminLogin)
	http.HandleFunc("/admin/api/logout", handleAdminLogout)
	http.HandleFunc("/admin/api/stats", adminAPI(handleAdminAPIStats))
	http.HandleFunc("/admin/api/games", adminAPI(handleAdminAPIGames))
	http.HandleFunc("/admin/api/game", adminAPI(handleAdminAPIDeleteGame))
	http.HandleFunc("/admin/api/change-password", adminAPI(handleAdminAPIChangePassword))

	// Endpointy panelu arbitra (FIDE Art. 6 & 7)
	http.HandleFunc("/arbiter", handleArbiterDashboard)
	http.HandleFunc("/arbiter/adjust-clock", handleArbiterAdjustClock)
	http.HandleFunc("/arbiter/add-time", handleArbiterAddTime)
	http.HandleFunc("/arbiter/pause", handleArbiterPause)
	http.HandleFunc("/arbiter/resume", handleArbiterResume)
	http.HandleFunc("/arbiter/delete-move", handleArbiterDeleteMove)
	http.HandleFunc("/arbiter/void", handleArbiterVoid)
	http.HandleFunc("/arbiter/forfeit", handleArbiterForfeit)
	http.HandleFunc("/arbiter/draw", handleArbiterDraw)
	http.HandleFunc("/arbiter/set-players", handleArbiterSetPlayers)
	http.HandleFunc("/arbiter/set-timecontrol", handleArbiterSetTimeControl)
	fmt.Println("✅ Endpointy HTTP zarejestrowane (w tym /admin, /arbiter)")

	fmt.Println()
	fmt.Println("╔═══════════════════════════════════════════════════════════════╗")
	fmt.Println("║              ♟ Chess Relay Server — działający ♟             ║")
	fmt.Println("╠═══════════════════════════════════════════════════════════════╣")
	fmt.Printf("║  Adres IP          : %-48s║\n", ip)
	fmt.Printf("║  Port              : %-48d║\n", cfg.Server.Port)
	fmt.Printf("║  Przeglądarka      : http://%s:%s%-36s║\n", ip, portStr, "")
	fmt.Printf("║  Baza danych       : %-48s║\n", cfg.Database.Path)
	fmt.Printf("║  Katalog gier      : %-48s║\n", cfg.Files.GamesDirectory)
	fmt.Printf("║  SSE (Server Push) : %-48v║\n", cfg.Viewer.EnableSSE)
	fmt.Printf("║  Zapis PGN         : %-48v║\n", cfg.Files.SavePGN)
	fmt.Printf("║  Zapis JSON        : %-48v║\n", cfg.Files.SaveJSON)
	fmt.Println("╚═══════════════════════════════════════════════════════════════╝")
	fmt.Println()
	fmt.Println("Gry w bazie:")
	printGamesTable()

	// Sprawdź dostępność serwera API TypeScript (port 9090) po starcie.
	// Uruchamiamy w tle — serwer Go musi najpierw zacząć nasłuchiwać.
	go func() {
		time.Sleep(800 * time.Millisecond)
		endpoints := []struct {
			name string
			url  string
		}{
			{"API /health", "http://127.0.0.1:9090/api/health"},
			{"API /clock/info (brak klucza → 400)", "http://127.0.0.1:9090/api/clock/info"},
		}
		fmt.Println()
		fmt.Println("─── Sprawdzenie API TS (port 9090) ───────────────────────────")
		client := &http.Client{Timeout: 3 * time.Second}
		allOk := true
		for _, ep := range endpoints {
			resp, err := client.Get(ep.url)
			if err != nil {
				fmt.Printf("  ❌ %-40s niedostępny (%v)\n", ep.name, err)
				allOk = false
				continue
			}
			resp.Body.Close()
			// /api/health → 200; /api/clock/info bez klucza → 400 (oczekiwane)
			ok := resp.StatusCode == 200 || resp.StatusCode == 400
			mark := "✅"
			if !ok {
				mark = "⚠️ "
				allOk = false
			}
			fmt.Printf("  %s %-40s HTTP %d\n", mark, ep.name, resp.StatusCode)
		}
		if allOk {
			fmt.Println("  ✅ API TypeScript: wszystkie endpointy odpowiadają")
		} else {
			fmt.Println("  ⚠️  Uruchom: cd api && npm run server")
		}
		fmt.Println("──────────────────────────────────────────────────────────────")
	}()

	if err := http.ListenAndServe(fmt.Sprintf(":%d", cfg.Server.Port), nil); err != nil {
		fmt.Println("❌ Błąd serwera:", err)
	}
}

// ─── Wbudowana przeglądarka ───────────────────────────────────────────────────

// adminFallback wyświetla się tylko gdy admin.html brakuje na dysku.
const adminFallback = `<!DOCTYPE html>
<html lang="pl"><head><meta charset="UTF-8"><title>Admin</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:system-ui,sans-serif;background:#0b0c0f;color:#e8edf5;
  display:flex;align-items:center;justify-content:center;height:100vh;}
.box{background:#14161c;border:1px solid #272a3a;border-radius:12px;
  padding:32px;text-align:center;max-width:360px;width:100%;}
h2{color:#c9a227;font-size:1.1rem;margin-bottom:8px;}
p{color:#474f60;font-size:.82rem;}
code{color:#c9a227;background:#0b0c0f;padding:2px 8px;border-radius:4px;}
</style></head>
<body><div class="box">
<h2>♟ Panel Administratora</h2>
<p>Umieść <code>admin.html</code> obok pliku serwera.</p>
</div></body></html>`

// viewerFallback wyświetla się tylko gdy viewer.html brakuje na dysku.
const viewerFallback = `<!DOCTYPE html>
<html lang="pl">
<head>
<meta charset="UTF-8">
<title>Chess Live — brak pliku</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
body{
  font-family:'Inter',system-ui,sans-serif;
  background:#0b0c0f;color:#e8edf5;
  display:flex;align-items:center;justify-content:center;
  height:100vh;flex-direction:column;gap:18px;
}
.icon{font-size:72px;opacity:.25;animation:float 4s ease-in-out infinite;}
@keyframes float{0%,100%{transform:translateY(0)}50%{transform:translateY(-10px)}}
h2{font-size:1.2rem;font-weight:700;color:#c9a227;}
p{color:#474f60;font-size:.85rem;line-height:1.6;text-align:center;}
code{
  display:inline-block;margin-top:4px;
  background:#14161c;border:1px solid #272a3a;
  border-radius:6px;padding:2px 10px;
  color:#c9a227;font-size:.82rem;font-family:monospace;
}
</style>
</head>
<body>
<div class="icon">♞</div>
<h2>Chess Live</h2>
<p>Umieść <code>viewer.html</code> w tym samym katalogu co serwer.<br>Serwer działa — endpointy API są aktywne.</p>
</body>
</html>`
