# Plik do Serwera

from flask import Flask, request, jsonify
from flask_socketio import SocketIO, join_room
import chess
import chess.pgn
from datetime import datetime
from pathlib import Path

app = Flask(__name__)
socketio = SocketIO(app, cors_allowed_origins="*")

games = {}

@app.route("/game/start", methods=["POST"])
def start_game():
    game_id = len(games) + 1
    games[game_id] = {
        "board": chess.Board(),
        "status": "running"
    }

    socketio.emit("game_started", {"game_id": game_id})
    return jsonify({"game_id": game_id})


@app.route("/game/<int:game_id>/move", methods=["POST"])
def move(game_id):
    game = games.get(game_id)
    if not game:
        return {"error": "No game"}, 404

    board = game["board"]
    move = chess.Move.from_uci(request.json["uci"])

    if move not in board.legal_moves:
        return {"error": "Illegal move"}, 400

    board.push(move)

    socketio.emit(
        "move",
        {
            "game_id": game_id,
            "uci": request.json["uci"],
            "fen": board.fen()
        },
        room=f"game_{game_id}"
    )

    return {"ok": True}


@app.route("/game/<int:game_id>/end", methods=["POST"])
def end_game(game_id):
    game = games.get(game_id)
    if not game:
        return {"error": "No game"}, 404

    board = game["board"]
    game["status"] = "ended"

    Path("games").mkdir(exist_ok=True)

    pgn = chess.pgn.Game.from_board(board)
    pgn.headers["Date"] = datetime.now().strftime("%Y.%m.%d")

    with open(f"games/game_{game_id}.pgn", "w") as f:
        print(pgn, file=f)

    socketio.emit("game_ended", {"game_id": game_id})
    return {"ok": True}


@socketio.on("join")
def join(data):
    join_room(f"game_{data['game_id']}")

if __name__ == "__main__":
    socketio.run(app, host="0.0.0.0", port=5000)
