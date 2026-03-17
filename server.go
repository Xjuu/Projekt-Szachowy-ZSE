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

var (
	db *sql.DB
	mu sync.Mutex
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
}

func initDB() error {
	var err error
	db, err = sql.Open("sqlite", "./chess.db")
	if err != nil {
		return err
	}
	db.Exec("PRAGMA journal_mode=WAL")
	_, err = db.Exec(`CREATE TABLE IF NOT EXISTS games (
		id           INTEGER PRIMARY KEY AUTOINCREMENT,
		white_player TEXT    NOT NULL DEFAULT 'White',
		black_player TEXT    NOT NULL DEFAULT 'Black',
		status       TEXT    NOT NULL DEFAULT 'ongoing',
		winner       TEXT    NOT NULL DEFAULT '',
		created_at   TEXT    NOT NULL
	)`)
	if err != nil {
		return err
	}
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
	row := db.QueryRow(
		"SELECT id, white_player, black_player, status, winner, created_at FROM games WHERE id = ?", id)
	g := &GameState{}
	if err := row.Scan(&g.ID, &g.WhitePlayer, &g.BlackPlayer, &g.Status, &g.Winner, &g.CreatedAt); err != nil {
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
	rows, err := db.Query(
		"SELECT id, white_player, black_player, status, winner, created_at FROM games ORDER BY id")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var games []*GameState
	for rows.Next() {
		g := &GameState{Moves: []MoveRecord{}}
		rows.Scan(&g.ID, &g.WhitePlayer, &g.BlackPlayer, &g.Status, &g.Winner, &g.CreatedAt)
		games = append(games, g)
	}
	return games, nil
}

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
		Moves       []MoveRecord `json:"moves"`
	}
	gf := GameFile{
		ID: game.ID, WhitePlayer: game.WhitePlayer, BlackPlayer: game.BlackPlayer,
		Status: game.Status, Winner: game.Winner, Result: result,
		CreatedAt: game.CreatedAt, Moves: game.Moves,
	}
	jsonBytes, _ := json.MarshalIndent(gf, "", "  ")
	os.WriteFile(fmt.Sprintf("gry/%d.json", game.ID), jsonBytes, 0644)
	fmt.Printf("[FILES] Saved gry/%d.pgn and gry/%d.json\n", game.ID, game.ID)
}

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

func handleNewGame(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var payload struct {
		WhitePlayer string `json:"white_player"`
		BlackPlayer string `json:"black_player"`
	}
	json.NewDecoder(r.Body).Decode(&payload)
	if payload.WhitePlayer == "" {
		payload.WhitePlayer = "White"
	}
	if payload.BlackPlayer == "" {
		payload.BlackPlayer = "Black"
	}
	now := time.Now().Format("2006-01-02 15:04:05")
	mu.Lock()
	result, err := db.Exec(
		"INSERT INTO games (white_player, black_player, status, winner, created_at) VALUES (?, ?, 'ongoing', '', ?)",
		payload.WhitePlayer, payload.BlackPlayer, now,
	)
	mu.Unlock()
	if err != nil {
		http.Error(w, "DB error: "+err.Error(), http.StatusInternalServerError)
		return
	}
	id, _ := result.LastInsertId()
	fmt.Printf("\n[NEW GAME] ID: %d | White: %s | Black: %s | %s\n",
		id, payload.WhitePlayer, payload.BlackPlayer, now)
	printGamesTable()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{"game_id": id})
}

func handleMove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var payload struct {
		GameID int64  `json:"game_id"`
		Move   string `json:"move"`
		Player string `json:"player"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Bad request", http.StatusBadRequest)
		return
	}
	now := time.Now().Format("15:04:05")
	mu.Lock()
	var moveNum int
	db.QueryRow("SELECT COUNT(*) FROM moves WHERE game_id = ?", payload.GameID).Scan(&moveNum)
	moveNum++
	_, err := db.Exec(
		"INSERT INTO moves (game_id, move_number, player, move, timestamp) VALUES (?, ?, ?, ?, ?)",
		payload.GameID, moveNum, payload.Player, payload.Move, now,
	)
	mu.Unlock()
	if err != nil {
		http.Error(w, "DB error: "+err.Error(), http.StatusInternalServerError)
		return
	}
	fmt.Printf("[%s] Game %-3d | Move #%-3d | %-6s plays %s\n",
		now, payload.GameID, moveNum, payload.Player, payload.Move)
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
	game, err := getGame(payload.GameID)
	if err == nil {
		saveGameFiles(game)
	}
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"ok":true}`)
}

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

func handleViewer(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html")
	fmt.Fprint(w, viewerHTML)
}

func getLocalIP() string {
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		return "unknown"
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).IP.String()
}

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

const viewerHTML = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>Chess Game Viewer</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: monospace; background: #1e1e1e; color: #d4d4d4; display: flex; height: 100vh; overflow: hidden; }

    /* SIDEBAR */
    #sidebar { width: 260px; min-width: 260px; background: #252526; border-right: 1px solid #3c3c3c; display: flex; flex-direction: column; }
    #sidebar h2 { padding: 1rem; color: #569cd6; border-bottom: 1px solid #3c3c3c; font-size: 1rem; }
    #gameList { overflow-y: auto; flex: 1; }
    .game-item { padding: 0.7rem 1rem; cursor: pointer; border-bottom: 1px solid #2d2d2d; transition: background 0.15s; }
    .game-item:hover { background: #2a2d2e; }
    .game-item.active { background: #094771; }
    .game-item .gid { color: #9cdcfe; font-weight: bold; font-size: 0.9rem; }
    .game-item .players { font-size: 0.8rem; color: #dcdcaa; margin-top: 2px; }
    .game-item .gmeta { font-size: 0.72rem; color: #888; margin-top: 2px; }
    .badge { display: inline-block; font-size: 0.65rem; padding: 1px 5px; border-radius: 3px; margin-left: 5px; vertical-align: middle; }
    .badge.ongoing   { background: #0e4e0e; color: #4ec9b0; }
    .badge.checkmate { background: #4e0e0e; color: #f48771; }
    .badge.stalemate { background: #3e3e0e; color: #dcdcaa; }

    /* MAIN */
    #main { flex: 1; display: flex; flex-direction: column; overflow: hidden; }
    #placeholder { color: #555; margin: auto; font-size: 1.1rem; }
    #gameDetail { display: none; flex-direction: column; height: 100%; overflow: hidden; }

    /* HEADER */
    #gameHeader { padding: 0.8rem 1.2rem 0.6rem; border-bottom: 1px solid #3c3c3c; flex-shrink: 0; }
    #gameHeader h1 { color: #569cd6; font-size: 1.05rem; }
    .players-line { font-size: 0.88rem; margin-top: 4px; }
    .players-line .w { color: #dcdcaa; }
    .players-line .b { color: #ce9178; }
    .status-line { font-size: 0.82rem; color: #4ec9b0; margin-top: 3px; }
    .winner-line { font-size: 0.88rem; color: #ffd700; font-weight: bold; margin-top: 2px; }

    /* BODY */
    #gameBody { display: flex; flex: 1; overflow: hidden; padding: 0.8rem 1rem; gap: 1.2rem; align-items: flex-start; }

    /* BOARD SECTION */
    #boardSection { display: flex; flex-direction: column; align-items: center; gap: 0.5rem; flex-shrink: 0; }
    #boardWrapper { display: flex; align-items: flex-start; }
    #rankLabels { display: flex; flex-direction: column; width: 16px; margin-right: 3px; }
    #rankLabels span { height: 52px; display: flex; align-items: center; justify-content: center; font-size: 0.68rem; color: #888; }
    #boardCol { display: flex; flex-direction: column; }
    #chessBoard { display: grid; grid-template-columns: repeat(8, 52px); grid-template-rows: repeat(8, 52px); border: 2px solid #555; }
    #fileLabels { display: flex; margin-top: 3px; }
    #fileLabels span { width: 52px; text-align: center; font-size: 0.68rem; color: #888; }

    .square { width: 52px; height: 52px; display: flex; align-items: center; justify-content: center; font-size: 2rem; position: relative; }
    .square.light { background: #f0d9b5; }
    .square.dark  { background: #b58863; }
    .square.hl    { box-shadow: inset 0 0 0 4px rgba(255,215,0,0.75); }
    .piece-w { color: #fff; text-shadow: 0 0 3px #000, 0 0 2px #000; line-height: 1; }
    .piece-b { color: #1a1a1a; text-shadow: 0 0 2px rgba(255,255,255,0.5); line-height: 1; }

    /* CONTROLS */
    #boardControls { display: flex; align-items: center; gap: 5px; flex-wrap: wrap; justify-content: center; width: 416px; }
    #boardControls button { background: #3c3c3c; color: #d4d4d4; border: 1px solid #555; padding: 4px 10px; cursor: pointer; border-radius: 3px; font-size: 0.82rem; }
    #boardControls button:hover { background: #505050; }
    #boardControls button:disabled { opacity: 0.35; cursor: default; }
    #boardControls button.live-on { background: #0e4e0e; color: #4ec9b0; border-color: #4ec9b0; }
    #moveLabel { font-size: 0.8rem; color: #aaa; min-width: 80px; text-align: center; }

    /* MOVE LIST */
    #moveSection { flex: 1; overflow-y: auto; min-width: 0; }
    #moveSection table { border-collapse: collapse; width: 100%; }
    #moveSection th, #moveSection td { border: 1px solid #3c3c3c; padding: 5px 10px; text-align: left; font-size: 0.82rem; }
    #moveSection th { background: #252526; color: #9cdcfe; position: sticky; top: 0; z-index: 1; }
    #moveSection tbody tr { cursor: pointer; }
    #moveSection tbody tr:hover { background: #2a2d2e !important; }
    #moveSection tbody tr:nth-child(even) { background: #252526; }
    #moveSection tbody tr.cur { background: #094771 !important; }
    .white { color: #dcdcaa; }
    .black { color: #ce9178; }
  </style>
</head>
<body>
  <div id="sidebar">
    <h2>&#9822; Chess Games</h2>
    <div id="gameList"></div>
  </div>
  <div id="main">
    <div id="placeholder">&#8592; Select a game from the list</div>
    <div id="gameDetail">
      <div id="gameHeader">
        <h1 id="gameTitle"></h1>
        <div class="players-line">
          <span class="w">&#9812; <span id="whiteName"></span></span>
          &nbsp;vs&nbsp;
          <span class="b">&#9818; <span id="blackName"></span></span>
        </div>
        <div class="status-line" id="statusLine"></div>
        <div class="winner-line" id="winnerLine"></div>
      </div>
      <div id="gameBody">
        <div id="boardSection">
          <div id="boardWrapper">
            <div id="rankLabels">
              <span>8</span><span>7</span><span>6</span><span>5</span>
              <span>4</span><span>3</span><span>2</span><span>1</span>
            </div>
            <div id="boardCol">
              <div id="chessBoard"></div>
              <div id="fileLabels">
                <span>a</span><span>b</span><span>c</span><span>d</span>
                <span>e</span><span>f</span><span>g</span><span>h</span>
              </div>
            </div>
          </div>
          <div id="boardControls">
            <button id="btnFirst" onclick="goTo(0)">&#9198;</button>
            <button id="btnPrev"  onclick="goTo(curIdx-1)">&#9664;</button>
            <span   id="moveLabel">0 / 0</span>
            <button id="btnNext"  onclick="goTo(curIdx+1)">&#9654;</button>
            <button id="btnLast"  onclick="goTo(states.length-1)">&#9197;</button>
            <button id="btnLive"  onclick="setLive(true)">&#128308; Live</button>
          </div>
        </div>
        <div id="moveSection">
          <table>
            <thead><tr><th>#</th><th>Time</th><th>Player</th><th>Move (UCI)</th></tr></thead>
            <tbody id="moveTable"></tbody>
          </table>
        </div>
      </div>
    </div>
  </div>

<script>
var PIECES = {
  'K':'\u2654','Q':'\u2655','R':'\u2656','B':'\u2657','N':'\u2658','P':'\u2659',
  'k':'\u265a','q':'\u265b','r':'\u265c','b':'\u265d','n':'\u265e','p':'\u265f'
};
var INIT_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR';

var selectedId = null;
var allGames   = [];
var curGame    = null;
var states     = [];   // board state at each half-move (states[0]=initial)
var curIdx     = 0;
var isLive     = true;

function parseFEN(fen) {
  var b = [];
  for (var i=0;i<8;i++) b.push([null,null,null,null,null,null,null,null]);
  var ranks = fen.split(' ')[0].split('/');
  for (var r=0;r<8;r++) {
    var c=0;
    for (var i=0;i<ranks[r].length;i++) {
      var ch=ranks[r][i];
      if (ch>='1'&&ch<='8') { c+=parseInt(ch); }
      else { b[r][c]=ch; c++; }
    }
  }
  return b;
}

function rc(sq) {
  return [8-parseInt(sq[1]), sq.charCodeAt(0)-97];
}

function applyUCI(board, uci) {
  var b = board.map(function(row){return row.slice();});
  var f=rc(uci.substring(0,2)), t=rc(uci.substring(2,4));
  var fr=f[0],fc=f[1],tr=t[0],tc=t[1];
  var promo = uci.length>=5 ? uci[4] : null;
  var piece = b[fr][fc];
  if (!piece) return b;

  // en passant
  if ((piece==='P'||piece==='p') && fc!==tc && !b[tr][tc]) b[fr][tc]=null;

  // castling white
  if (piece==='K'&&fr===7&&fc===4) {
    if (tc===6){b[7][5]='R';b[7][7]=null;}
    else if(tc===2){b[7][3]='R';b[7][0]=null;}
  }
  // castling black
  if (piece==='k'&&fr===0&&fc===4) {
    if (tc===6){b[0][5]='r';b[0][7]=null;}
    else if(tc===2){b[0][3]='r';b[0][0]=null;}
  }

  b[tr][tc] = promo ? (piece===piece.toUpperCase()?promo.toUpperCase():promo.toLowerCase()) : piece;
  b[fr][fc] = null;
  return b;
}

function buildStates(moves) {
  var arr = [parseFEN(INIT_FEN)];
  for (var i=0;i<moves.length;i++) arr.push(applyUCI(arr[arr.length-1], moves[i].move));
  return arr;
}

function renderBoard(board, fromSq, toSq) {
  var cont = document.getElementById('chessBoard');
  cont.innerHTML = '';
  for (var r=0;r<8;r++) {
    for (var c=0;c<8;c++) {
      var div = document.createElement('div');
      div.className = 'square ' + ((r+c)%2===0?'light':'dark');
      var sqName = String.fromCharCode(97+c)+(8-r);
      if (sqName===fromSq||sqName===toSq) div.classList.add('hl');
      var piece = board[r][c];
      if (piece) {
        var span = document.createElement('span');
        span.textContent = PIECES[piece]||'';
        span.className   = piece===piece.toUpperCase() ? 'piece-w' : 'piece-b';
        div.appendChild(span);
      }
      cont.appendChild(div);
    }
  }
}

function goTo(idx) {
  if (!states.length) return;
  idx = Math.max(0, Math.min(idx, states.length-1));
  curIdx = idx;
  isLive = (idx === states.length-1);
  document.getElementById('btnLive').className = isLive ? 'live-on' : '';

  var fromSq='', toSq='';
  if (idx>0 && curGame && curGame.moves && curGame.moves[idx-1]) {
    var uci = curGame.moves[idx-1].move;
    fromSq = uci.substring(0,2);
    toSq   = uci.substring(2,4);
  }
  renderBoard(states[idx], fromSq, toSq);

  var total = states.length-1;
  document.getElementById('moveLabel').textContent = idx + ' / ' + total;
  document.getElementById('btnPrev').disabled  = (idx===0);
  document.getElementById('btnFirst').disabled = (idx===0);
  document.getElementById('btnNext').disabled  = (idx===total);
  document.getElementById('btnLast').disabled  = (idx===total);

  // highlight row in table
  document.querySelectorAll('#moveTable tr').forEach(function(row){ row.classList.remove('cur'); });
  if (idx>0) {
    var row = document.querySelector('#moveTable tr[data-i="'+(idx-1)+'"]');
    if (row) { row.classList.add('cur'); row.scrollIntoView({block:'nearest'}); }
  }
}

function setLive(v) {
  isLive = v;
  if (isLive) goTo(states.length-1);
  document.getElementById('btnLive').className = isLive ? 'live-on' : '';
}

function selectGame(id) {
  selectedId = id;
  isLive = true;
  curGame = null;
  document.querySelectorAll('.game-item').forEach(function(el){ el.classList.remove('active'); });
  var el = document.querySelector('[data-id="'+id+'"]');
  if (el) el.classList.add('active');
}

function renderSidebar() {
  var list = document.getElementById('gameList');
  var st = list.scrollTop;
  list.innerHTML = '';
  allGames.slice().reverse().forEach(function(g) {
    var div = document.createElement('div');
    div.className = 'game-item'+(g.id===selectedId?' active':'');
    div.dataset.id = g.id;
    var bc = g.status==='ongoing'?'ongoing':g.status==='checkmate'?'checkmate':'stalemate';
    div.innerHTML =
      '<div class="gid">Game #'+g.id+'<span class="badge '+bc+'">'+g.status+'</span></div>'+
      '<div class="players">'+(g.white_player||'White')+' vs '+(g.black_player||'Black')+'</div>'+
      '<div class="gmeta">'+(g.created_at||'')+'</div>';
    div.onclick = function(){ selectGame(g.id); };
    list.appendChild(div);
  });
  list.scrollTop = st;
}

function renderDetail(game) {
  document.getElementById('placeholder').style.display = 'none';
  document.getElementById('gameDetail').style.display  = 'flex';

  document.getElementById('gameTitle').textContent  = 'Game #'+game.id;
  document.getElementById('whiteName').textContent  = game.white_player||'White';
  document.getElementById('blackName').textContent  = game.black_player||'Black';
  document.getElementById('statusLine').textContent = 'Status: '+game.status;
  document.getElementById('winnerLine').textContent = game.winner ? 'Winner: '+game.winner : '';
  document.getElementById('btnLive').style.display  = game.status==='ongoing' ? '' : 'none';

  var prevLen = curGame ? (curGame.moves?curGame.moves.length:0) : -1;
  var currLen = game.moves ? game.moves.length : 0;
  var newGame = !curGame || curGame.id !== game.id;

  curGame = game;

  if (newGame) {
    states = buildStates(game.moves||[]);
    curIdx = states.length-1;
    isLive = true;
  } else if (currLen !== prevLen) {
    states = buildStates(game.moves||[]);
    if (isLive) curIdx = states.length-1;
  }

  document.getElementById('btnLive').className = isLive ? 'live-on' : '';

  // rebuild move table
  var tbody = document.getElementById('moveTable');
  tbody.innerHTML = '';
  (game.moves||[]).forEach(function(m, i) {
    var cls = m.player==='White'?'white':'black';
    var tr  = document.createElement('tr');
    tr.dataset.i = i;
    if (i===curIdx-1) tr.classList.add('cur');
    tr.innerHTML = '<td>'+m.move_number+'</td><td>'+m.timestamp+'</td>'+
                   '<td class="'+cls+'">'+m.player+'</td><td>'+m.move+'</td>';
    tr.onclick = (function(idx){ return function(){ goTo(idx+1); isLive=false; }; })(i);
    tbody.appendChild(tr);
  });

  goTo(curIdx);
}

async function refresh() {
  try {
    var res   = await fetch('/state');
    var games = await res.json();
    if (!games) return;
    allGames = games;
    renderSidebar();

    if (selectedId !== null) {
      var dr = await fetch('/state?id='+selectedId);
      if (dr.ok) renderDetail(await dr.json());
    }
  } catch(e) {}
}

// keyboard navigation
document.addEventListener('keydown', function(e) {
  if (e.key==='ArrowLeft')  goTo(curIdx-1);
  if (e.key==='ArrowRight') goTo(curIdx+1);
  if (e.key==='Home')       goTo(0);
  if (e.key==='End')        goTo(states.length-1);
});

refresh();
setInterval(refresh, 1500);
</script>
</body>
</html>`