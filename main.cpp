#include "chess-library/include/chess.hpp"
#include <iostream>
#include <list>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace chess;

// Usuwa port z adresu IP (np. "127.0.0.1:8080" -> "127.0.0.1")
std::string stripPort(const std::string& ip) {
    auto pos = ip.find(':');
    if (pos != std::string::npos)
        return ip.substr(0, pos);
    return ip;
}

// Wysyła prosty request HTTP POST przez sockety
// ip      - adres serwera
// port    - port (np. 8080)
// path    - endpoint (np. "/move")
// body    - dane JSON
// Zwraca cały response jako string
std::string httpPost(const std::string& ip, int port, const std::string& path, const std::string& body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv { 5, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        std::cerr << "[network] connect() failed\n";
        return "";
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << ip << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    std::string reqStr = req.str();
    send(sock, reqStr.c_str(), reqStr.size(), 0);

    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = '\0';
        response += buf;
    }

    close(sock);
    return response;
}

// Parsuje game_id z odpowiedzi JSON (bardzo "na surowo")
std::string parseGameId(const std::string& response) {
    auto pos = response.find("\"game_id\"");
    if (pos == std::string::npos) return "";
    auto colon = response.find(':', pos);
    if (colon == std::string::npos) return "";
    size_t start = colon + 1;
    while (start < response.size() && (response[start] == ' ' || response[start] == '\n' || response[start] == '\r'))
        start++;
    if (start >= response.size()) return "";
    if (response[start] == '"') {
        auto q2 = response.find('"', start + 1);
        if (q2 == std::string::npos) return "";
        return response.substr(start + 1, q2 - start - 1);
    } else {
        size_t end = start;
        while (end < response.size() && isdigit(response[end])) end++;
        return response.substr(start, end - start);
    }
}

// Tworzy nową grę na serwerze
std::string requestNewGame(const std::string& ip,
                           const std::string& whiteName,
                           const std::string& blackName) {
    std::string body = "{\"white_player\":\"" + whiteName + "\","
                        "\"black_player\":\"" + blackName + "\"}";
    std::cerr << "[debug] POST /newgame to " << ip << ":8080\n";
    std::string resp = httpPost(ip, 8080, "/newgame", body);
    std::cerr << "[debug] raw response: [" << resp << "]\n";
    std::string id = parseGameId(resp);
    std::cerr << "[debug] parsed game_id: [" << id << "]\n";
    return id;
}

// Wysyła ruch do serwera
void sendMove(const std::string& ip, const std::string& gameId,
              const std::string& move, const std::string& color) {
    std::string body = "{\"game_id\":" + gameId + ","
                       "\"move\":\"" + move + "\","
                       "\"player\":\"" + color + "\"}";
    std::string resp = httpPost(ip, 8080, "/move", body);
    if (resp.empty())
        std::cerr << "[network] Move not sent.\n";
    else if (resp.find("\"ok\":true") == std::string::npos)
        std::cerr << "[network] Server error: " << resp.substr(0, 120) << "\n";
}

// Wysyła status gry (checkmate/stalemate)
void sendStatus(const std::string& ip, const std::string& gameId,
                const std::string& status, const std::string& winner) {
    std::string body = "{\"game_id\":" + gameId + ","
                       "\"status\":\"" + status + "\","
                       "\"winner\":\"" + winner + "\"}";
    std::string resp = httpPost(ip, 8080, "/status", body);
    if (resp.empty())
        std::cerr << "[network] Status not sent.\n";
}

// Obsługa ruchu Gracza
bool handleTurn(Board& board, const Movelist& moves,
                const std::string& playerName,
                const std::string& playerColor,
                const std::string& serverIp, const std::string& gameId,
                bool networked, std::list<std::string>& moveHistory)
{
    std::cout << "[" << playerColor << "] " << playerName << " to move\n";
    std::cout << "Enter move in UCI format (e.g. e2e4): ";

    std::string input;
    std::cin >> input;

    Move move;
    try {
        move = uci::uciToMove(board, input);
    } catch (...) {
        std::cout << "Invalid format. Try again.\n";
        return false;
    }

    bool legal = false;
    for (auto m : moves)
        if (uci::moveToUci(m) == input) { legal = true; break; }

    if (!legal) {
        std::cout << "Illegal move. Try again.\n";
        return false;
    }

    board.makeMove(move);
    if (networked) sendMove(serverIp, gameId, input, playerColor);
    moveHistory.push_back(input);
    return true;
}

int main() {
    std::string serverIp;
    std::cout << "Enter relay server IP (or press Enter to skip networking): ";
    std::getline(std::cin, serverIp);
    serverIp = stripPort(serverIp);

    bool networked = !serverIp.empty();
    std::string gameId;
    std::string whiteName = "White";
    std::string blackName = "Black";

    std::cout << "White player name: ";
    std::getline(std::cin, whiteName);
    if (whiteName.empty()) whiteName = "White";

    std::cout << "Black player name: ";
    std::getline(std::cin, blackName);
    if (blackName.empty()) blackName = "Black";

    if (networked) {
        gameId = requestNewGame(serverIp, whiteName, blackName);
        if (gameId.empty()) {
            std::cout << "Could not reach server - running in local-only mode.\n\n";
            networked = false;
        } else {
            std::cout << "\n";
            std::cout << "╔══════════════════════════════════════╗\n";
            std::cout << "║           GAME STARTED               ║\n";
            std::cout << "╠══════════════════════════════════════╣\n";
            std::cout << "║  Game ID : #" << gameId << "                    ║\n";
            std::cout << "║  White   : " << whiteName << "\n";
            std::cout << "║  Black   : " << blackName << "\n";
            std::cout << "║  Watch   : http://" << serverIp << ":8080  ║\n";
            std::cout << "╚══════════════════════════════════════╝\n\n";
        }
    } else {
        std::cout << "Running in local-only mode.\n\n";
    }

    std::list<std::string> moveHistory;
    Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    std::string winner;

    while (true) {
        Movelist moves;
        movegen::legalmoves(moves, board);

        bool isWhite          = (board.sideToMove() == Color::WHITE);
        std::string playerName    = isWhite ? whiteName    : blackName;
        std::string playerColor   = isWhite ? "White"      : "Black";
        std::string opponentColor = isWhite ? "Black"      : "White";

        if (moves.size() == 0) {
            std::string status;
            if (board.inCheck()) {
                status = "checkmate";
                winner = opponentColor;
                std::cout << "\n*** Checkmate! "
                          << (isWhite ? blackName : whiteName)
                          << " (" << opponentColor << ") wins! ***\n";
            } else {
                status = "stalemate";
                winner = "Draw";
                std::cout << "\n*** Stalemate! It's a draw. ***\n";
            }
            if (networked) sendStatus(serverIp, gameId, status, opponentColor);
            break;
        }

        while (!handleTurn(board, moves, playerName, playerColor,
                           serverIp, gameId, networked, moveHistory))
            ;
    }

    std::cout << "\n--- Game history ---\n";
    std::cout << "Game ID : " << (gameId.empty() ? "local" : "#" + gameId) << "\n";
    std::cout << "White   : " << whiteName << "\n";
    std::cout << "Black   : " << blackName << "\n";
    std::cout << "Result  : " << winner << "\n\n";
    int i = 1;
    for (const auto& m : moveHistory)
        std::cout << i++ << ". " << m << "\n";

    std::cout << "\nPress Enter to exit...";
    std::cin.ignore();
    std::cin.get();
    return 0;
}
