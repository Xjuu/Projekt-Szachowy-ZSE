// server_test.go — kompleksowe testy wszystkich handlerów i funkcji serwera
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"golang.org/x/crypto/bcrypt"
)

// ─── Helpery testowe ──────────────────────────────────────────────────────────

// resetDB inicjalizuje świeżą bazę danych w pamięci przed każdym testem.
func resetDB(t *testing.T) {
	t.Helper()
	cfg = Config{
		Database: DatabaseConfig{Path: ":memory:", WALMode: false},
		Chess: ChessConfig{
			DefaultTimeControlMinutes: 5,
			WhitePlayerDefault:        "Biały",
			BlackPlayerDefault:        "Czarny",
		},
		Files: FilesConfig{
			GamesDirectory: t.TempDir(),
			SavePGN:        true,
			SaveJSON:       true,
		},
		Viewer: ViewerConfig{
			ViewerFile:           "nieistniejący_viewer.html",
			EnableSSE:            true,
			KeepaliveIntervalSec: 30,
		},
	}
	if db != nil {
		db.Close()
		db = nil
	}
	if err := initDB(); err != nil {
		t.Fatalf("initDB: %v", err)
	}
	t.Cleanup(func() {
		if db != nil {
			db.Close()
			db = nil
		}
	})
}

// postJSON wysyła żądanie POST z ciałem JSON do podanego handlera.
func postJSON(t *testing.T, handler http.HandlerFunc, body interface{}) *httptest.ResponseRecorder {
	t.Helper()
	b, _ := json.Marshal(body)
	req := httptest.NewRequest(http.MethodPost, "/", bytes.NewReader(b))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()
	handler(rr, req)
	return rr
}

// getReq wysyła żądanie GET do podanego handlera z daną ścieżką.
func getReq(handler http.HandlerFunc, path string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(http.MethodGet, path, nil)
	rr := httptest.NewRecorder()
	handler(rr, req)
	return rr
}

// createTestGame tworzy grę testową i zwraca jej ID.
func createTestGame(t *testing.T, white, black string) int64 {
	t.Helper()
	rr := postJSON(t, handleNewGame, map[string]interface{}{
		"white_player": white,
		"black_player": black,
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("createTestGame: status %d", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	return int64(res["game_id"].(float64))
}

// setupAdmin ustawia dane testowego admina i czyści sesje.
func setupAdmin(t *testing.T) {
	t.Helper()
	hash, _ := bcrypt.GenerateFromPassword([]byte("testHaslo123"), bcrypt.MinCost)
	adminCreds = AdminCreds{
		Username:      "admin",
		PasswordHash:  string(hash),
		SessionTTLMin: 60,
	}
	adminSessionsMu.Lock()
	adminSessions = map[string]time.Time{}
	adminSessionsMu.Unlock()
	t.Cleanup(func() {
		adminSessionsMu.Lock()
		adminSessions = map[string]time.Time{}
		adminSessionsMu.Unlock()
	})
}

// loginAdmin loguje się jako admin i zwraca token sesji.
func loginAdmin(t *testing.T) string {
	t.Helper()
	rr := postJSON(t, handleAdminLogin, map[string]string{
		"username": "admin",
		"password": "testHaslo123",
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("loginAdmin: status %d — %s", rr.Code, rr.Body.String())
	}
	for _, c := range rr.Result().Cookies() {
		if c.Name == "adm" {
			return c.Value
		}
	}
	t.Fatal("loginAdmin: brak ciasteczka 'adm'")
	return ""
}

// reqWithToken dodaje ciasteczko sesji admina do żądania.
func reqWithToken(method, path, token string) *http.Request {
	req := httptest.NewRequest(method, path, nil)
	req.AddCookie(&http.Cookie{Name: "adm", Value: token})
	return req
}

// ─── initDB ───────────────────────────────────────────────────────────────────

func TestInitDB_TabelaGames(t *testing.T) {
	resetDB(t)
	var name string
	err := db.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='games'").Scan(&name)
	if err != nil || name != "games" {
		t.Fatal("tabela 'games' nie istnieje po initDB")
	}
}

func TestInitDB_TabelaMoves(t *testing.T) {
	resetDB(t)
	var name string
	err := db.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='moves'").Scan(&name)
	if err != nil || name != "moves" {
		t.Fatal("tabela 'moves' nie istnieje po initDB")
	}
}

func TestInitDB_KolumnyGames(t *testing.T) {
	resetDB(t)
	// Upewnij się, że kolumny zegarów istnieją
	_, err := db.Exec("INSERT INTO games (white_player,black_player,status,winner,created_at,time_control,white_time_ms,black_time_ms) VALUES ('A','B','ongoing','','2025-01-01',300,300000,300000)")
	if err != nil {
		t.Fatalf("brakuje kolumn zegarów: %v", err)
	}
}

// ─── getGame / getAllGames ─────────────────────────────────────────────────────

func TestGetGame_NieznanaGra(t *testing.T) {
	resetDB(t)
	_, err := getGame(9999)
	if err == nil {
		t.Fatal("oczekiwano błędu dla nieistniejącej gry")
	}
}

func TestGetAllGames_Pusta(t *testing.T) {
	resetDB(t)
	games, err := getAllGames()
	if err != nil {
		t.Fatalf("getAllGames: %v", err)
	}
	if len(games) != 0 {
		t.Fatalf("oczekiwano 0 gier, got %d", len(games))
	}
}

func TestGetAllGames_KilkaGier(t *testing.T) {
	resetDB(t)
	createTestGame(t, "A", "B")
	createTestGame(t, "C", "D")
	games, err := getAllGames()
	if err != nil {
		t.Fatalf("getAllGames: %v", err)
	}
	if len(games) != 2 {
		t.Fatalf("oczekiwano 2 gry, got %d", len(games))
	}
}

// ─── handleNewGame ────────────────────────────────────────────────────────────

func TestHandleNewGame_OK(t *testing.T) {
	resetDB(t)
	rr := postJSON(t, handleNewGame, map[string]interface{}{
		"white_player": "Alicja",
		"black_player": "Bartek",
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d, oczekiwano 200", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	if res["game_id"] == nil {
		t.Fatal("brak pola game_id w odpowiedzi")
	}
}

func TestHandleNewGame_DomyślniGracze(t *testing.T) {
	resetDB(t)
	rr := postJSON(t, handleNewGame, map[string]interface{}{})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	id := int64(res["game_id"].(float64))
	game, _ := getGame(id)
	if game.WhitePlayer != "Biały" {
		t.Errorf("white_player = %q, oczekiwano %q", game.WhitePlayer, "Biały")
	}
	if game.BlackPlayer != "Czarny" {
		t.Errorf("black_player = %q, oczekiwano %q", game.BlackPlayer, "Czarny")
	}
}

func TestHandleNewGame_KontrolaaCzasuMs(t *testing.T) {
	resetDB(t)
	rr := postJSON(t, handleNewGame, map[string]interface{}{
		"white_player":    "Alicja",
		"black_player":    "Bartek",
		"time_control_ms": 300000,
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	id := int64(res["game_id"].(float64))
	game, _ := getGame(id)
	if game.TimeControl != 300 {
		t.Errorf("time_control = %d s, oczekiwano 300", game.TimeControl)
	}
	if game.WhiteTimeMs != 300000 || game.BlackTimeMs != 300000 {
		t.Errorf("white/black_time_ms = %d/%d, oczekiwano 300000", game.WhiteTimeMs, game.BlackTimeMs)
	}
}

func TestHandleNewGame_KontrolaCzasuSek(t *testing.T) {
	resetDB(t)
	rr := postJSON(t, handleNewGame, map[string]interface{}{
		"time_control_sec": 600,
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	id := int64(res["game_id"].(float64))
	game, _ := getGame(id)
	if game.TimeControl != 600 {
		t.Errorf("time_control = %d, oczekiwano 600", game.TimeControl)
	}
}

func TestHandleNewGame_ZłaMetoda(t *testing.T) {
	resetDB(t)
	req := httptest.NewRequest(http.MethodGet, "/newgame", nil)
	rr := httptest.NewRecorder()
	handleNewGame(rr, req)
	if rr.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status %d, oczekiwano 405", rr.Code)
	}
}

// ─── handleMove ───────────────────────────────────────────────────────────────

func TestHandleMove_OK(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "Alicja", "Bartek")
	rr := postJSON(t, handleMove, map[string]interface{}{
		"game_id": id,
		"move":    "e2e4",
		"player":  "White",
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	if res["move_number"].(float64) != 1 {
		t.Errorf("move_number = %v, oczekiwano 1", res["move_number"])
	}
}

func TestHandleMove_TimeLeftMs(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "Alicja", "Bartek")
	rr := postJSON(t, handleMove, map[string]interface{}{
		"game_id":      id,
		"move":         "e2e4",
		"player":       "White",
		"time_left_ms": 280000,
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	game, _ := getGame(id)
	if game.WhiteTimeMs != 280000 {
		t.Errorf("white_time_ms = %d, oczekiwano 280000", game.WhiteTimeMs)
	}
}

func TestHandleMove_TimeLeftMs_Czarny(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "A", "B")
	postJSON(t, handleMove, map[string]interface{}{"game_id": id, "move": "e2e4", "player": "White"})
	postJSON(t, handleMove, map[string]interface{}{
		"game_id":      id,
		"move":         "e7e5",
		"player":       "Black",
		"time_left_ms": 295000,
	})
	game, _ := getGame(id)
	if game.BlackTimeMs != 295000 {
		t.Errorf("black_time_ms = %d, oczekiwano 295000", game.BlackTimeMs)
	}
}

func TestHandleMove_KilkaRuchow(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "Alicja", "Bartek")
	ruchy := []struct{ move, player string }{
		{"e2e4", "White"}, {"e7e5", "Black"}, {"g1f3", "White"}, {"b8c6", "Black"},
	}
	for i, m := range ruchy {
		rr := postJSON(t, handleMove, map[string]interface{}{
			"game_id": id, "move": m.move, "player": m.player,
		})
		if rr.Code != http.StatusOK {
			t.Fatalf("ruch %d: status %d", i+1, rr.Code)
		}
	}
	game, _ := getGame(id)
	if len(game.Moves) != 4 {
		t.Errorf("liczba ruchów = %d, oczekiwano 4", len(game.Moves))
	}
}

func TestHandleMove_ZłeJSON(t *testing.T) {
	resetDB(t)
	req := httptest.NewRequest(http.MethodPost, "/move", strings.NewReader("nie-json!!!"))
	rr := httptest.NewRecorder()
	handleMove(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status %d, oczekiwano 400", rr.Code)
	}
}

func TestHandleMove_ZłaMetoda(t *testing.T) {
	resetDB(t)
	req := httptest.NewRequest(http.MethodGet, "/move", nil)
	rr := httptest.NewRecorder()
	handleMove(rr, req)
	if rr.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status %d, oczekiwano 405", rr.Code)
	}
}

// ─── handleStatus ─────────────────────────────────────────────────────────────

func TestHandleStatus_Mat(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "Alicja", "Bartek")
	rr := postJSON(t, handleStatus, map[string]interface{}{
		"game_id": id, "status": "checkmate", "winner": "White",
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	game, _ := getGame(id)
	if game.Status != "checkmate" {
		t.Errorf("status = %q, oczekiwano checkmate", game.Status)
	}
	if game.Winner != "White" {
		t.Errorf("winner = %q, oczekiwano White", game.Winner)
	}
}

func TestHandleStatus_Pat(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "A", "B")
	rr := postJSON(t, handleStatus, map[string]interface{}{
		"game_id": id, "status": "stalemate", "winner": "",
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	game, _ := getGame(id)
	if game.Status != "stalemate" {
		t.Errorf("status = %q, oczekiwano stalemate", game.Status)
	}
}

func TestHandleStatus_Timeout(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "A", "B")
	rr := postJSON(t, handleStatus, map[string]interface{}{
		"game_id": id, "status": "timeout", "winner": "Black",
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	game, _ := getGame(id)
	if game.Winner != "Black" {
		t.Errorf("winner = %q, oczekiwano Black", game.Winner)
	}
}

func TestHandleStatus_ZłaMetoda(t *testing.T) {
	resetDB(t)
	req := httptest.NewRequest(http.MethodGet, "/status", nil)
	rr := httptest.NewRecorder()
	handleStatus(rr, req)
	if rr.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status %d, oczekiwano 405", rr.Code)
	}
}

func TestHandleStatus_ZłeJSON(t *testing.T) {
	resetDB(t)
	req := httptest.NewRequest(http.MethodPost, "/status", strings.NewReader("zepsuty json"))
	rr := httptest.NewRecorder()
	handleStatus(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status %d, oczekiwano 400", rr.Code)
	}
}

// ─── handleState ──────────────────────────────────────────────────────────────

func TestHandleState_PustaLista(t *testing.T) {
	resetDB(t)
	rr := getReq(handleState, "/state")
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var games []GameState
	json.NewDecoder(rr.Body).Decode(&games)
	if len(games) != 0 {
		t.Errorf("oczekiwano 0 gier, got %d", len(games))
	}
}

func TestHandleState_Lista(t *testing.T) {
	resetDB(t)
	createTestGame(t, "Alicja", "Bartek")
	createTestGame(t, "Celina", "Darek")
	rr := getReq(handleState, "/state")
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var games []GameState
	json.NewDecoder(rr.Body).Decode(&games)
	if len(games) != 2 {
		t.Errorf("oczekiwano 2 gry, got %d", len(games))
	}
}

func TestHandleState_JednaGra(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "Alicja", "Bartek")
	req := httptest.NewRequest(http.MethodGet, fmt.Sprintf("/state?id=%d", id), nil)
	rr := httptest.NewRecorder()
	handleState(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var game GameState
	json.NewDecoder(rr.Body).Decode(&game)
	if game.ID != id {
		t.Errorf("id = %d, oczekiwano %d", game.ID, id)
	}
	if game.WhitePlayer != "Alicja" {
		t.Errorf("white_player = %q", game.WhitePlayer)
	}
}

func TestHandleState_NieznanaGra(t *testing.T) {
	resetDB(t)
	req := httptest.NewRequest(http.MethodGet, "/state?id=9999", nil)
	rr := httptest.NewRecorder()
	handleState(rr, req)
	if rr.Code != http.StatusNotFound {
		t.Fatalf("status %d, oczekiwano 404", rr.Code)
	}
}

func TestHandleState_NaglowekCORS(t *testing.T) {
	resetDB(t)
	rr := getReq(handleState, "/state")
	if rr.Header().Get("Access-Control-Allow-Origin") != "*" {
		t.Error("brak nagłówka CORS Access-Control-Allow-Origin: *")
	}
}

// ─── handleViewer ─────────────────────────────────────────────────────────────

func TestHandleViewer_Fallback(t *testing.T) {
	cfg.Viewer.ViewerFile = "plik_który_nie_istnieje.html"
	rr := getReq(handleViewer, "/")
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	if !strings.Contains(rr.Body.String(), "Chess") {
		t.Error("brak słowa 'Chess' w fallbacku viewera")
	}
}

func TestHandleViewer_ZPliku(t *testing.T) {
	tmp := t.TempDir()
	path := tmp + "/test_viewer.html"
	os.WriteFile(path, []byte("<html><body>testowa strona szachów</body></html>"), 0644)
	cfg.Viewer.ViewerFile = path
	rr := getReq(handleViewer, "/")
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	if !strings.Contains(rr.Body.String(), "testowa strona szachów") {
		t.Error("brak treści pliku viewer.html")
	}
}

func TestHandleViewer_NaglowekContentType(t *testing.T) {
	cfg.Viewer.ViewerFile = "brak.html"
	rr := getReq(handleViewer, "/")
	ct := rr.Header().Get("Content-Type")
	if !strings.Contains(ct, "text/html") {
		t.Errorf("Content-Type = %q, oczekiwano text/html", ct)
	}
}

// ─── handleEvents (SSE) ───────────────────────────────────────────────────────

func TestHandleEvents_PoczątkowaLista(t *testing.T) {
	resetDB(t)
	cfg.Viewer.KeepaliveIntervalSec = 60
	createTestGame(t, "Alicja", "Bartek")

	ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
	defer cancel()
	req := httptest.NewRequest(http.MethodGet, "/events", nil).WithContext(ctx)
	rr := httptest.NewRecorder()
	handleEvents(rr, req)

	body := rr.Body.String()
	if !strings.Contains(body, "event: list") {
		t.Errorf("brak 'event: list' w strumieniu SSE:\n%s", body)
	}
	if !strings.Contains(body, "Alicja") {
		t.Error("brak danych gracza w liście SSE")
	}
}

func TestHandleEvents_NaglowkiSSE(t *testing.T) {
	resetDB(t)
	cfg.Viewer.KeepaliveIntervalSec = 60

	ctx, cancel := context.WithTimeout(context.Background(), 150*time.Millisecond)
	defer cancel()
	req := httptest.NewRequest(http.MethodGet, "/events", nil).WithContext(ctx)
	rr := httptest.NewRecorder()
	handleEvents(rr, req)

	if ct := rr.Header().Get("Content-Type"); !strings.Contains(ct, "text/event-stream") {
		t.Errorf("Content-Type = %q, oczekiwano text/event-stream", ct)
	}
}

// ─── saveGameFiles ────────────────────────────────────────────────────────────

func TestSaveGameFiles_PGN_WynikBiale(t *testing.T) {
	tmp := t.TempDir()
	cfg.Files.GamesDirectory = tmp
	cfg.Files.SavePGN = true
	cfg.Files.SaveJSON = false

	game := &GameState{
		ID: 10, WhitePlayer: "Alicja", BlackPlayer: "Bartek",
		Status: "checkmate", Winner: "White",
		CreatedAt: "2025-01-01 12:00:00", TimeControl: 300,
		Moves: []MoveRecord{
			{Move: "e2e4", Player: "White", MoveNum: 1, Timestamp: "12:00:01"},
			{Move: "e7e5", Player: "Black", MoveNum: 2, Timestamp: "12:00:02"},
		},
	}
	saveGameFiles(game)

	data, err := os.ReadFile(fmt.Sprintf("%s/10.pgn", tmp))
	if err != nil {
		t.Fatalf("plik PGN nie zapisany: %v", err)
	}
	pgn := string(data)
	if !strings.Contains(pgn, `[White "Alicja"]`) {
		t.Error("brak tagu [White] w PGN")
	}
	if !strings.Contains(pgn, `[Black "Bartek"]`) {
		t.Error("brak tagu [Black] w PGN")
	}
	if !strings.Contains(pgn, "1-0") {
		t.Error("brak wyniku 1-0 w PGN")
	}
	if !strings.Contains(pgn, "e2e4") {
		t.Error("brak ruchu e2e4 w PGN")
	}
	if !strings.Contains(pgn, "[TimeControl") {
		t.Error("brak tagu [TimeControl] w PGN")
	}
}

func TestSaveGameFiles_PGN_WynikCzarne(t *testing.T) {
	tmp := t.TempDir()
	cfg.Files.GamesDirectory = tmp
	cfg.Files.SavePGN = true
	cfg.Files.SaveJSON = false

	game := &GameState{
		ID: 11, WhitePlayer: "A", BlackPlayer: "B",
		Status: "checkmate", Winner: "Black", Moves: []MoveRecord{},
	}
	saveGameFiles(game)

	data, _ := os.ReadFile(fmt.Sprintf("%s/11.pgn", tmp))
	if !strings.Contains(string(data), "0-1") {
		t.Error("brak wyniku 0-1 w PGN")
	}
}

func TestSaveGameFiles_PGN_Remis(t *testing.T) {
	tmp := t.TempDir()
	cfg.Files.GamesDirectory = tmp
	cfg.Files.SavePGN = true
	cfg.Files.SaveJSON = false

	game := &GameState{
		ID: 12, WhitePlayer: "A", BlackPlayer: "B",
		Status: "stalemate", Winner: "Draw", Moves: []MoveRecord{},
	}
	saveGameFiles(game)

	data, _ := os.ReadFile(fmt.Sprintf("%s/12.pgn", tmp))
	if !strings.Contains(string(data), "1/2-1/2") {
		t.Error("brak wyniku 1/2-1/2 w PGN")
	}
}

func TestSaveGameFiles_JSON(t *testing.T) {
	tmp := t.TempDir()
	cfg.Files.GamesDirectory = tmp
	cfg.Files.SavePGN = false
	cfg.Files.SaveJSON = true

	game := &GameState{
		ID: 20, WhitePlayer: "Celina", BlackPlayer: "Darek",
		Status: "stalemate", Winner: "Draw", Moves: []MoveRecord{},
	}
	saveGameFiles(game)

	data, err := os.ReadFile(fmt.Sprintf("%s/20.json", tmp))
	if err != nil {
		t.Fatalf("plik JSON nie zapisany: %v", err)
	}
	var out map[string]interface{}
	if err := json.Unmarshal(data, &out); err != nil {
		t.Fatalf("nieprawidłowy JSON: %v", err)
	}
	if out["white_player"] != "Celina" {
		t.Errorf("white_player = %v", out["white_player"])
	}
	if out["result"] != "1/2-1/2" {
		t.Errorf("result = %v, oczekiwano 1/2-1/2", out["result"])
	}
}

func TestSaveGameFiles_ObaFormaty(t *testing.T) {
	tmp := t.TempDir()
	cfg.Files.GamesDirectory = tmp
	cfg.Files.SavePGN = true
	cfg.Files.SaveJSON = true

	game := &GameState{
		ID: 30, WhitePlayer: "E", BlackPlayer: "F",
		Status: "checkmate", Winner: "White", Moves: []MoveRecord{},
	}
	saveGameFiles(game)

	if _, err := os.ReadFile(tmp + "/30.pgn"); err != nil {
		t.Error("brak pliku PGN przy SavePGN=true")
	}
	if _, err := os.ReadFile(tmp + "/30.json"); err != nil {
		t.Error("brak pliku JSON przy SaveJSON=true")
	}
}

func TestSaveGameFiles_PGNWyłączony(t *testing.T) {
	tmp := t.TempDir()
	cfg.Files.GamesDirectory = tmp
	cfg.Files.SavePGN = false
	cfg.Files.SaveJSON = false

	game := &GameState{ID: 99, Moves: []MoveRecord{}}
	saveGameFiles(game)

	if _, err := os.ReadFile(tmp + "/99.pgn"); err == nil {
		t.Error("plik PGN zapisany mimo SavePGN=false")
	}
}

// ─── broadcastUpdate ──────────────────────────────────────────────────────────

func TestBroadcastUpdate_OtrzymujeWiadomość(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "Alicja", "Bartek")

	ch := sseHub.Subscribe()
	defer sseHub.Unsubscribe(ch)

	broadcastUpdate(id)

	select {
	case msg := <-ch:
		s := string(msg)
		if !strings.Contains(s, "event: list") && !strings.Contains(s, "event: game") {
			t.Errorf("nieoczekiwana wiadomość SSE: %s", s)
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("timeout — brak wiadomości SSE")
	}
}

func TestBroadcastUpdate_BezSubskrybentów(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "A", "B")
	// Nie powinno panikować bez subskrybentów
	broadcastUpdate(id)
}

func TestBroadcastUpdate_KilkuSubskrybentów(t *testing.T) {
	resetDB(t)
	id := createTestGame(t, "A", "B")

	ch1 := sseHub.Subscribe()
	ch2 := sseHub.Subscribe()
	defer sseHub.Unsubscribe(ch1)
	defer sseHub.Unsubscribe(ch2)

	broadcastUpdate(id)

	for i, ch := range []chan []byte{ch1, ch2} {
		select {
		case <-ch:
		case <-time.After(500 * time.Millisecond):
			t.Fatalf("subskrybent %d nie otrzymał wiadomości", i+1)
		}
	}
}

// ─── Admin — loadAdminCreds ───────────────────────────────────────────────────

func TestLoadAdminCreds_TworzePliWBrakuJSON(t *testing.T) {
	orig := adminCreds
	adminSessions = map[string]time.Time{}
	t.Cleanup(func() { adminCreds = orig })

	tmp := t.TempDir()
	oldDir, _ := os.Getwd()
	os.Chdir(tmp)
	defer os.Chdir(oldDir)

	loadAdminCreds()

	if adminCreds.Username == "" {
		t.Error("username pusty po loadAdminCreds")
	}
	if adminCreds.PasswordHash == "" {
		t.Error("password_hash pusty po loadAdminCreds")
	}
	if _, err := os.ReadFile("admin.json"); err != nil {
		t.Error("plik admin.json nie został utworzony")
	}
}

func TestLoadAdminCreds_WczytujePlikJSON(t *testing.T) {
	hash, _ := bcrypt.GenerateFromPassword([]byte("mojeHaslo"), bcrypt.MinCost)
	creds := AdminCreds{Username: "szef", PasswordHash: string(hash), SessionTTLMin: 30}
	data, _ := json.MarshalIndent(creds, "", "  ")

	tmp := t.TempDir()
	os.WriteFile(tmp+"/admin.json", data, 0600)

	oldDir, _ := os.Getwd()
	os.Chdir(tmp)
	defer os.Chdir(oldDir)

	orig := adminCreds
	t.Cleanup(func() { adminCreds = orig })

	loadAdminCreds()

	if adminCreds.Username != "szef" {
		t.Errorf("username = %q, oczekiwano 'szef'", adminCreds.Username)
	}
	if adminCreds.SessionTTLMin != 30 {
		t.Errorf("session_ttl_minutes = %d, oczekiwano 30", adminCreds.SessionTTLMin)
	}
}

// ─── Admin — sesje ────────────────────────────────────────────────────────────

func TestAdminSession_Tworzenie(t *testing.T) {
	setupAdmin(t)
	token := newAdminSession()
	if token == "" {
		t.Fatal("token sesji jest pusty")
	}
	if !checkAdminSession(token) {
		t.Fatal("nowa sesja powinna być ważna")
	}
}

func TestAdminSession_Wygaśnięcie(t *testing.T) {
	setupAdmin(t)
	token := newAdminSession()
	// Ręcznie wygaś sesję
	adminSessionsMu.Lock()
	adminSessions[token] = time.Now().Add(-time.Minute)
	adminSessionsMu.Unlock()
	if checkAdminSession(token) {
		t.Fatal("wygasła sesja powinna być odrzucona")
	}
}

func TestAdminSession_PustyToken(t *testing.T) {
	if checkAdminSession("") {
		t.Fatal("pusty token nie powinien być ważny")
	}
}

func TestAdminSession_NieistniejącyToken(t *testing.T) {
	setupAdmin(t)
	if checkAdminSession("totalnieFałszywyToken_xyz") {
		t.Fatal("nieznany token nie powinien być ważny")
	}
}

// ─── Admin — handleAdminLogin ─────────────────────────────────────────────────

func TestAdminLogin_OK(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	rr := postJSON(t, handleAdminLogin, map[string]string{
		"username": "admin", "password": "testHaslo123",
	})
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d, oczekiwano 200 — %s", rr.Code, rr.Body.String())
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	if res["ok"] != true {
		t.Error("oczekiwano ok:true")
	}
	// Sprawdź ciasteczko sesji
	var mamCiasteczko bool
	for _, c := range rr.Result().Cookies() {
		if c.Name == "adm" && c.Value != "" {
			mamCiasteczko = true
		}
	}
	if !mamCiasteczko {
		t.Error("brak ciasteczka 'adm' po udanym logowaniu")
	}
}

func TestAdminLogin_ZłeHasło(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	rr := postJSON(t, handleAdminLogin, map[string]string{
		"username": "admin", "password": "złeHasło",
	})
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status %d, oczekiwano 401", rr.Code)
	}
}

func TestAdminLogin_ZłaNazwaUżytkownika(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	rr := postJSON(t, handleAdminLogin, map[string]string{
		"username": "haker", "password": "testHaslo123",
	})
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status %d, oczekiwano 401", rr.Code)
	}
}

func TestAdminLogin_ZłaMetoda(t *testing.T) {
	setupAdmin(t)
	rr := getReq(handleAdminLogin, "/admin/api/login")
	if rr.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status %d, oczekiwano 405", rr.Code)
	}
}

func TestAdminLogin_ZłeJSON(t *testing.T) {
	setupAdmin(t)
	req := httptest.NewRequest(http.MethodPost, "/admin/api/login", strings.NewReader("nie json"))
	rr := httptest.NewRecorder()
	handleAdminLogin(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status %d, oczekiwano 400", rr.Code)
	}
}

// ─── Admin — handleAdminLogout ────────────────────────────────────────────────

func TestAdminLogout_NiszczySesjęi(t *testing.T) {
	setupAdmin(t)
	token := newAdminSession()
	if !checkAdminSession(token) {
		t.Fatal("sesja powinna być aktywna przed wylogowaniem")
	}

	req := httptest.NewRequest(http.MethodPost, "/admin/api/logout", nil)
	req.AddCookie(&http.Cookie{Name: "adm", Value: token})
	rr := httptest.NewRecorder()
	handleAdminLogout(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	if checkAdminSession(token) {
		t.Fatal("sesja powinna być zniszczona po wylogowaniu")
	}
}

func TestAdminLogout_BezSesji(t *testing.T) {
	// Nie powinno panikować bez ciasteczka
	req := httptest.NewRequest(http.MethodPost, "/admin/api/logout", nil)
	rr := httptest.NewRecorder()
	handleAdminLogout(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
}

// ─── Admin — middleware adminAPI ──────────────────────────────────────────────

func TestAdminAPI_Middleware_Nieautoryzowany(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/admin/api/stats", nil)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIStats)(rr, req)
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status %d, oczekiwano 401", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	if res["ok"] != false {
		t.Error("oczekiwano ok:false")
	}
}

// ─── Admin — handleAdminAPIStats ──────────────────────────────────────────────

func TestAdminAPIStats_Statystyki(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	createTestGame(t, "A", "B")
	createTestGame(t, "C", "D")
	// Zakończ jedną grę
	id := createTestGame(t, "E", "F")
	postJSON(t, handleStatus, map[string]interface{}{
		"game_id": id, "status": "checkmate", "winner": "White",
	})

	req := reqWithToken(http.MethodGet, "/admin/api/stats", token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIStats)(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	if res["total_games"].(float64) != 3 {
		t.Errorf("total_games = %v, oczekiwano 3", res["total_games"])
	}
	if res["ongoing"].(float64) != 2 {
		t.Errorf("ongoing = %v, oczekiwano 2", res["ongoing"])
	}
	if res["finished"].(float64) != 1 {
		t.Errorf("finished = %v, oczekiwano 1", res["finished"])
	}
}

func TestAdminAPIStats_ZawieraInfoSerwera(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)
	cfg.Server.Port = 8080
	cfg.Database.Path = ":memory:"

	req := reqWithToken(http.MethodGet, "/admin/api/stats", token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIStats)(rr, req)

	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	if res["port"] == nil {
		t.Error("brak pola 'port' w statystykach")
	}
	if res["db_path"] == nil {
		t.Error("brak pola 'db_path' w statystykach")
	}
}

// ─── Admin — handleAdminAPIGames ──────────────────────────────────────────────

func TestAdminAPIGames_Lista(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	createTestGame(t, "Alicja", "Bartek")
	createTestGame(t, "Celina", "Darek")

	req := reqWithToken(http.MethodGet, "/admin/api/games", token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIGames)(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	if res["ok"] != true {
		t.Error("oczekiwano ok:true")
	}
	games := res["games"].([]interface{})
	if len(games) != 2 {
		t.Errorf("oczekiwano 2 gry, got %d", len(games))
	}
}

func TestAdminAPIGames_PustaLista(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	req := reqWithToken(http.MethodGet, "/admin/api/games", token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIGames)(rr, req)

	var res map[string]interface{}
	json.NewDecoder(rr.Body).Decode(&res)
	games := res["games"].([]interface{})
	if len(games) != 0 {
		t.Errorf("oczekiwano 0 gier, got %d", len(games))
	}
}

// ─── Admin — handleAdminAPIDeleteGame ─────────────────────────────────────────

func TestAdminAPIDeleteGame_OK(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	id := createTestGame(t, "Alicja", "Bartek")

	req := reqWithToken(http.MethodDelete, fmt.Sprintf("/admin/api/game?id=%d", id), token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIDeleteGame)(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status %d — %s", rr.Code, rr.Body.String())
	}
	if _, err := getGame(id); err == nil {
		t.Fatal("gra powinna być usunięta z bazy")
	}
}

func TestAdminAPIDeleteGame_UsuwaMovesCascade(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	id := createTestGame(t, "A", "B")
	postJSON(t, handleMove, map[string]interface{}{"game_id": id, "move": "e2e4", "player": "White"})

	req := reqWithToken(http.MethodDelete, fmt.Sprintf("/admin/api/game?id=%d", id), token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIDeleteGame)(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}

	var count int
	db.QueryRow("SELECT COUNT(*) FROM moves WHERE game_id = ?", id).Scan(&count)
	if count != 0 {
		t.Errorf("pozostało %d ruchów po usunięciu gry", count)
	}
}

func TestAdminAPIDeleteGame_BrakID(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	req := reqWithToken(http.MethodDelete, "/admin/api/game", token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIDeleteGame)(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status %d, oczekiwano 400", rr.Code)
	}
}

func TestAdminAPIDeleteGame_NieistniejaGra(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	req := reqWithToken(http.MethodDelete, "/admin/api/game?id=9999", token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIDeleteGame)(rr, req)

	if rr.Code != http.StatusNotFound {
		t.Fatalf("status %d, oczekiwano 404", rr.Code)
	}
}

func TestAdminAPIDeleteGame_ZłaMetoda(t *testing.T) {
	setupAdmin(t)
	token := loginAdmin(t)

	req := reqWithToken(http.MethodGet, "/admin/api/game?id=1", token)
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIDeleteGame)(rr, req)

	if rr.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status %d, oczekiwano 405", rr.Code)
	}
}

// ─── Admin — handleAdminAPIChangePassword ─────────────────────────────────────

func TestAdminAPIChangePassword_OK(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	req := httptest.NewRequest(http.MethodPost, "/admin/api/change-password",
		bytes.NewReader(mustJSON(map[string]string{
			"old_password": "testHaslo123",
			"new_password": "noweHaslo456",
		})))
	req.Header.Set("Content-Type", "application/json")
	req.AddCookie(&http.Cookie{Name: "adm", Value: token})
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIChangePassword)(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status %d — %s", rr.Code, rr.Body.String())
	}
	// Stary token powinien być unieważniony
	if checkAdminSession(token) {
		t.Error("stary token powinien być unieważniony po zmianie hasła")
	}
	// Nowe hasło powinno działać
	if err := bcrypt.CompareHashAndPassword([]byte(adminCreds.PasswordHash), []byte("noweHaslo456")); err != nil {
		t.Error("nowe hasło nie zostało poprawnie zapisane")
	}
}

func TestAdminAPIChangePassword_ZłeStareHasło(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	req := httptest.NewRequest(http.MethodPost, "/admin/api/change-password",
		bytes.NewReader(mustJSON(map[string]string{
			"old_password": "błędneStare",
			"new_password": "noweHaslo456",
		})))
	req.Header.Set("Content-Type", "application/json")
	req.AddCookie(&http.Cookie{Name: "adm", Value: token})
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIChangePassword)(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status %d, oczekiwano 401", rr.Code)
	}
}

func TestAdminAPIChangePassword_ZaKrótkieHasło(t *testing.T) {
	resetDB(t)
	setupAdmin(t)
	token := loginAdmin(t)

	req := httptest.NewRequest(http.MethodPost, "/admin/api/change-password",
		bytes.NewReader(mustJSON(map[string]string{
			"old_password": "testHaslo123",
			"new_password": "abc",
		})))
	req.Header.Set("Content-Type", "application/json")
	req.AddCookie(&http.Cookie{Name: "adm", Value: token})
	rr := httptest.NewRecorder()
	adminAPI(handleAdminAPIChangePassword)(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status %d, oczekiwano 400", rr.Code)
	}
}

// ─── Admin — handleAdminDashboard ─────────────────────────────────────────────

func TestAdminDashboard_SerwisFallback(t *testing.T) {
	rr := getReq(handleAdminDashboard, "/admin")
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	if !strings.Contains(rr.Body.String(), "Admin") {
		t.Error("brak słowa 'Admin' w fallbacku panelu")
	}
}

func TestAdminDashboard_SerwisZPliku(t *testing.T) {
	tmp := t.TempDir()
	path := tmp + "/admin.html"
	os.WriteFile(path, []byte("<html><body>panel testowy</body></html>"), 0644)

	// Tymczasowo zmień katalog roboczy
	oldDir, _ := os.Getwd()
	os.Chdir(tmp)
	defer os.Chdir(oldDir)

	rr := getReq(handleAdminDashboard, "/admin")
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d", rr.Code)
	}
	if !strings.Contains(rr.Body.String(), "panel testowy") {
		t.Error("brak treści pliku admin.html")
	}
}

// ─── Helper ───────────────────────────────────────────────────────────────────

func mustJSON(v interface{}) []byte {
	b, _ := json.Marshal(v)
	return b
}
