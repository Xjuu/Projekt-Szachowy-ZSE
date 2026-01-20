import requests
import chess
import time

# ADRES SERWERA
SERVER_URL = "http://192.168.1.100:5000"

def start_game():
    """Rozpoczyna nowa gre na serwerze"""
    try:
        r = requests.post(f"{SERVER_URL}/game/start")
        r.raise_for_status()
        data = r.json()
        print(f"[INFO] Połączono z serwerem! Nowa gra rozpoczęta. game_id = {data['game_id']}")
        return data['game_id']
    except requests.exceptions.RequestException as e:
        print("[BŁĄD] Nie udało się połączyć z serwerem:", e)
        return None

def make_move(game_id, move_uci):
    """Wysyla ruch do serwera i zwraca odpowiedzź"""
    try:
        r = requests.post(f"{SERVER_URL}/game/{game_id}/move", json={"uci": move_uci})
        r.raise_for_status()
        return r.json()
    except requests.exceptions.RequestException as e:
        print("[BŁĄD] Problem z wysłaniem ruchu:", e)
        return None

def main():
    print("Łączenie z serwerem...")
    game_id = start_game()
    if not game_id:
        return

    board = chess.Board()

    while not board.is_game_over():
        print("\nAktualna pozycja:")
        print(board)
        print(f"Ruch {'Białego' if board.turn == chess.WHITE else 'Czarnego'}")

        move = input("Podaj ruch w formacie UCI (np. e2e4): ").strip()
        result = make_move(game_id, move)

        if result:
            if "fen" in result:
                board.set_fen(result["fen"])
                print("[INFO] Ruch zaakceptowany.")
            else:
                print("[BŁĄD] Serwer odrzucił ruch:", result)
        else:
            print("[BŁĄD] Nie udało się przesłać ruchu, spróbuj ponownie.")

    print("\n=== KONIEC GRY ===")
    print(board)
    if board.is_checkmate():
        winner = "Białe" if board.turn == chess.BLACK else "Czarne"
        print(f"Szach-mat! Wygrywa {winner}!")
    elif board.is_stalemate() or board.is_insufficient_material():
        print("Remis!")
    else:
        print("Gra zakończona.")

if __name__ == "__main__":
    main()

# Pierdol sie, ja wole po angielsku dawac wszystko jak programuje
