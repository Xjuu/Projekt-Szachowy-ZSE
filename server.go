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

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	_ "modernc.org/sqlite"
)

// ─── SSE Hub ──────────────────────────────────────────────────────────────────
// Each connected browser gets one channel.  When the game state changes the
// server broadcasts a typed SSE event to every subscriber.

type Hub struct {
	mu      sync.Mutex
	clients map[chan []byte]struct{}
}

var sseHub = &Hub{clients: make(map[chan []byte]struct{})}

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

// Broadcast sends an SSE frame to every subscriber.
// Slow / full channels are silently dropped (non-blocking select).
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

// ─── Database ─────────────────────────────────────────────────────────────────

var (
	db *sql.DB
	mu sync.Mutex // protects all DB writes
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
	db, err = sql.Open("sqlite", "./chess.db")
	if err != nil {
		return err
	}
	db.Exec("PRAGMA journal_mode=WAL")

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
	os.MkdirAll("gry", 0755)

	result := "*"
	switch game.Winner {
	case "White":
		result = "1-0"
	case "Black":
		result = "0-1"
	case "Draw":
		result = "1/2-1/2"
	}

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
	os.WriteFile(fmt.Sprintf("gry/%d.pgn", game.ID), []byte(pgn.String()), 0644)

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
	os.WriteFile(fmt.Sprintf("gry/%d.json", game.ID), jsonBytes, 0644)
	fmt.Printf("[FILES] Saved gry/%d.pgn and gry/%d.json\n", game.ID, game.ID)
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

// POST /newgame
// C++ client sends: { "white_player":"...", "black_player":"...", "time_control_ms": 600000 }
// Legacy sends:     { "white_player":"...", "black_player":"...", "time_control_sec": 600 }
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
		payload.WhitePlayer = "White"
	}
	if payload.BlackPlayer == "" {
		payload.BlackPlayer = "Black"
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

// POST /move
// Body: { "game_id": N, "move": "e2e4", "player": "White",
//
//	"white_time_ms": N, "black_time_ms": N }
func handleMove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	// C++ client sends time_left_ms (one side only).
	// Legacy sends white_time_ms + black_time_ms (both sides).
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

// GET /state[?id=N]
// REST fallback — the browser viewer uses SSE, but this endpoint is kept for
// external tools / debugging.
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

// GET /events
// Server-Sent Events stream.  The browser keeps one long-lived connection here
// instead of polling /state every second.
//
// Event types sent:
//
//	list  — JSON array of all games (summary, no moves)  — fired on any change
//	game  — JSON object for a single game (full, with moves) — fired on move/status
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

	// Keep-alive comment every 30 s prevents proxies from closing idle connections.
	ticker := time.NewTicker(30 * time.Second)
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
	// Serve viewer.html from disk if it exists next to the binary.
	// This lets you edit the UI without recompiling the server.
	if data, err := os.ReadFile("viewer.html"); err == nil {
		w.Write(data)
		return
	}
	// Fallback: embedded minimal page
	fmt.Fprint(w, viewerFallback)
}

// ─── Utilities ────────────────────────────────────────────────────────────────

func getLocalIP() string {
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		return "unknown"
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).IP.String()
}

// ─── Entry point ──────────────────────────────────────────────────────────────

func main() {
	if err := initDB(); err != nil {
		fmt.Println("DB init error:", err)
		return
	}
	defer db.Close()
	os.MkdirAll("gry", 0755)

	ip := getLocalIP()
	port := "8080"

	http.HandleFunc("/newgame", handleNewGame)
	http.HandleFunc("/move", handleMove)
	http.HandleFunc("/status", handleStatus)
	http.HandleFunc("/state", handleState)
	http.HandleFunc("/events", handleEvents)
	http.HandleFunc("/", handleViewer)

	fmt.Println("╔══════════════════════════════════════╗")
	fmt.Println("║     Chess Relay Server — running     ║")
	fmt.Println("╠══════════════════════════════════════╣")
	fmt.Printf("║  Local IP  : %-24s║\n", ip)
	fmt.Printf("║  Port      : %-24s║\n", port)
	fmt.Printf("║  Viewer    : http://%s:%s  ║\n", ip, port)
	fmt.Printf("║  Database  : %-24s║\n", "./chess.db")
	fmt.Printf("║  Game files: %-24s║\n", "./gry/")
	fmt.Println("╚══════════════════════════════════════╝")
	fmt.Println()
	fmt.Println("Games in database:")
	printGamesTable()

	if err := http.ListenAndServe(":"+port, nil); err != nil {
		fmt.Println("Server error:", err)
	}
}

// ─── Embedded viewer ──────────────────────────────────────────────────────────

// viewerFallback is shown only when viewer.html is missing from disk.
// Put viewer.html in the same directory as the server binary to get the full UI.
const viewerFallback = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Chess Viewer — file missing</title>
<style>
  body { font-family: monospace; background: #161512; color: #e8e3d8;
         display: flex; align-items: center; justify-content: center;
         height: 100vh; margin: 0; flex-direction: column; gap: 14px; }
  h2  { color: #81b64c; }
  p   { color: #9e9a93; font-size: .9rem; }
  code{ color: #d4a84b; }
</style>
</head>
<body>
<h2>♟ Chess Viewer</h2>
<p>Place <code>viewer.html</code> in the same directory as this server binary.</p>
<p>The server is running — API endpoints are active.</p>
</body>
</html>`
