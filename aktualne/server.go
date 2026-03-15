package main

import (
	"encoding/json"
	"fmt"
	"math/rand"
	"net"
	"net/http"
	"sync"
	"time"
)

type MoveRecord struct {
	Move      string `json:"move"`
	Player    string `json:"player"`
	Timestamp string `json:"timestamp"`
	MoveNum   int    `json:"move_number"`
}

type GameState struct {
	ID        string       `json:"id"`
	Moves     []MoveRecord `json:"moves"`
	Status    string       `json:"status"`
	Winner    string       `json:"winner"`
	CreatedAt string       `json:"created_at"`
}

var (
	games = make(map[string]*GameState)
	mu    sync.RWMutex
)

func generateID() string {
	const chars = "abcdefghijklmnopqrstuvwxyz0123456789"
	b := make([]byte, 8)
	for i := range b {
		b[i] = chars[rand.Intn(len(chars))]
	}
	return string(b)
}

func printGamesTable() {
	fmt.Println()
	fmt.Println("┌──────────┬─────────────────────┬───────────┬─────────┬────────┐")
	fmt.Println("│ Game ID  │ Started             │ Moves     │ Status  │ Winner │")
	fmt.Println("├──────────┼─────────────────────┼───────────┼─────────┼────────┤")
	if len(games) == 0 {
		fmt.Println("│ (no games yet)                                               │")
	}
	for _, g := range games {
		winner := g.Winner
		if winner == "" {
			winner = "—"
		}
		fmt.Printf("│ %-8s │ %-19s │ %-9d │ %-7s │ %-6s │\n",
			g.ID, g.CreatedAt, len(g.Moves), g.Status, winner)
	}
	fmt.Println("└──────────┴─────────────────────┴───────────┴─────────┴────────┘")
	fmt.Println()
}

func handleNewGame(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	id := generateID()
	now := time.Now().Format("2006-01-02 15:04:05")

	mu.Lock()
	games[id] = &GameState{ID: id, Status: "ongoing", CreatedAt: now}
	mu.Unlock()

	fmt.Printf("\n[NEW GAME] ID: %s  started at %s\n", id, now)
	mu.RLock()
	printGamesTable()
	mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"game_id": id})
}

func handleMove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var payload struct {
		GameID string `json:"game_id"`
		Move   string `json:"move"`
		Player string `json:"player"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Bad request", http.StatusBadRequest)
		return
	}

	mu.Lock()
	game, ok := games[payload.GameID]
	if !ok {
		mu.Unlock()
		http.Error(w, "Game not found", http.StatusNotFound)
		return
	}
	record := MoveRecord{
		Move:      payload.Move,
		Player:    payload.Player,
		Timestamp: time.Now().Format("15:04:05"),
		MoveNum:   len(game.Moves) + 1,
	}
	game.Moves = append(game.Moves, record)
	mu.Unlock()

	fmt.Printf("[%s] Game %-8s | Move #%-3d | %-5s plays %s\n",
		record.Timestamp, payload.GameID, record.MoveNum, record.Player, record.Move)

	w.WriteHeader(http.StatusOK)
}

func handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var payload struct {
		GameID string `json:"game_id"`
		Status string `json:"status"`
		Winner string `json:"winner"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Bad request", http.StatusBadRequest)
		return
	}

	mu.Lock()
	game, ok := games[payload.GameID]
	if ok {
		game.Status = payload.Status
		game.Winner = payload.Winner
	}
	mu.Unlock()

	fmt.Printf("\n[GAME OVER] Game %s | %s | Winner: %s\n",
		payload.GameID, payload.Status, payload.Winner)
	mu.RLock()
	printGamesTable()
	mu.RUnlock()

	w.WriteHeader(http.StatusOK)
}

func handleState(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")

	mu.RLock()
	defer mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	if id == "" {
		list := make([]*GameState, 0, len(games))
		for _, g := range games {
			list = append(list, g)
		}
		json.NewEncoder(w).Encode(list)
		return
	}

	game, ok := games[id]
	if !ok {
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
	rand.Seed(time.Now().UnixNano())
	ip := getLocalIP()
	port := "9090"

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
	fmt.Println("╚══════════════════════════════════════╝")
	fmt.Println()

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
    body { font-family: monospace; background: #1e1e1e; color: #d4d4d4; display: flex; height: 100vh; }

    /* LEFT PANEL — game list */
    #sidebar {
      width: 260px; min-width: 260px;
      background: #252526;
      border-right: 1px solid #3c3c3c;
      display: flex; flex-direction: column;
    }
    #sidebar h2 { padding: 1rem; color: #569cd6; border-bottom: 1px solid #3c3c3c; }
    #gameList { overflow-y: auto; flex: 1; }
    .game-item {
      padding: 0.75rem 1rem;
      cursor: pointer;
      border-bottom: 1px solid #2d2d2d;
      transition: background 0.15s;
    }
    .game-item:hover { background: #2a2d2e; }
    .game-item.active { background: #094771; }
    .game-item .gid { color: #9cdcfe; font-weight: bold; }
    .game-item .gmeta { font-size: 0.8rem; color: #888; margin-top: 2px; }
    .badge {
      display: inline-block; font-size: 0.7rem; padding: 1px 6px;
      border-radius: 3px; margin-left: 6px; vertical-align: middle;
    }
    .badge.ongoing  { background: #0e4e0e; color: #4ec9b0; }
    .badge.checkmate{ background: #4e0e0e; color: #f48771; }
    .badge.stalemate{ background: #3e3e0e; color: #dcdcaa; }

    /* RIGHT PANEL — game detail */
    #main { flex: 1; display: flex; flex-direction: column; padding: 1.5rem; overflow-y: auto; }
    #main h1 { color: #569cd6; margin-bottom: 0.5rem; }
    #gameInfo { margin-bottom: 1rem; }
    .status-line { font-size: 1rem; color: #4ec9b0; }
    .winner-line { font-size: 1.1rem; color: #ffd700; font-weight: bold; margin-top: 4px; }

    table { border-collapse: collapse; width: 100%; max-width: 480px; margin-top: 0.5rem; }
    th, td { border: 1px solid #3c3c3c; padding: 7px 14px; text-align: left; }
    th { background: #252526; color: #9cdcfe; }
    tr:nth-child(even) { background: #252526; }
    .white { color: #dcdcaa; }
    .black { color: #ce9178; }

    #placeholder { color: #555; margin-top: 4rem; font-size: 1.1rem; }
  </style>
</head>
<body>
  <div id="sidebar">
    <h2>♟ Games</h2>
    <div id="gameList"></div>
  </div>
  <div id="main">
    <div id="placeholder">← Select a game to view moves</div>
    <div id="gameDetail" style="display:none">
      <h1 id="gameTitle">Game</h1>
      <div id="gameInfo">
        <div class="status-line" id="statusLine"></div>
        <div class="winner-line" id="winnerLine"></div>
      </div>
      <table>
        <thead><tr><th>#</th><th>Time</th><th>Player</th><th>Move</th></tr></thead>
        <tbody id="moveTable"></tbody>
      </table>
    </div>
  </div>

  <script>
    let selectedId = null;
    let allGames = [];

    function selectGame(id) {
      selectedId = id;
      document.querySelectorAll('.game-item').forEach(el => el.classList.remove('active'));
      const el = document.querySelector('[data-id="' + id + '"]');
      if (el) el.classList.add('active');
      renderDetail();
    }

    function renderSidebar() {
      const list = document.getElementById('gameList');
      list.innerHTML = '';
      [...allGames].reverse().forEach(g => {
        const div = document.createElement('div');
        div.className = 'game-item' + (g.id === selectedId ? ' active' : '');
        div.dataset.id = g.id;
        const badgeClass = g.status === 'ongoing' ? 'ongoing' : g.status === 'checkmate' ? 'checkmate' : 'stalemate';
        div.innerHTML =
          '<div class="gid">' + g.id + '<span class="badge ' + badgeClass + '">' + g.status + '</span></div>' +
          '<div class="gmeta">' + (g.moves ? g.moves.length : 0) + ' moves · ' + (g.created_at || '') + '</div>';
        div.onclick = () => selectGame(g.id);
        list.appendChild(div);
      });
    }

    function renderDetail() {
      if (!selectedId) return;
      const game = allGames.find(g => g.id === selectedId);
      if (!game) return;

      document.getElementById('placeholder').style.display = 'none';
      document.getElementById('gameDetail').style.display = 'block';
      document.getElementById('gameTitle').textContent = 'Game ' + game.id;
      document.getElementById('statusLine').textContent = 'Status: ' + game.status;
      document.getElementById('winnerLine').textContent = game.winner ? 'Winner: ' + game.winner : '';

      const tbody = document.getElementById('moveTable');
      tbody.innerHTML = '';
      (game.moves || []).forEach(m => {
        const cls = m.player === 'White' ? 'white' : 'black';
        tbody.innerHTML +=
          '<tr><td>' + m.move_number + '</td><td>' + m.timestamp +
          '</td><td class="' + cls + '">' + m.player + '</td><td>' + m.move + '</td></tr>';
      });
    }

    async function refresh() {
      try {
        const res = await fetch('/state');
        const games = await res.json();
        if (!games) return;

        // merge — fetch full state for selected game to get latest moves
        if (selectedId) {
          const idx = games.findIndex(g => g.id === selectedId);
          if (idx !== -1) {
            const detail = await fetch('/state?id=' + selectedId);
            games[idx] = await detail.json();
          }
        }

        allGames = games;
        renderSidebar();
        renderDetail();
      } catch(e) {}
    }

    refresh();
    setInterval(refresh, 1500);
  </script>
</body>
</html>`
