#include "chess-library/include/chess.hpp"
#include <iostream>
#include <list>
#include <string>
#include <sstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

using namespace chess;

std::string stripPort(const std::string& ip) {
    auto pos = ip.find(':');
    if (pos != std::string::npos)
        return ip.substr(0, pos);
    return ip;
}

std::string httpPost(const std::string& ip, int port, const std::string& path, const std::string& body) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { WSACleanup(); return ""; }

    DWORD tv = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(sock); WSACleanup();
        std::cerr << "[network] connect() failed, WSAError: " << WSAGetLastError() << "\n";
        return "";
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.0\r\n"
        << "Host: " << ip << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    std::string reqStr = req.str();
    send(sock, reqStr.c_str(), (int)reqStr.size(), 0);

    // sygnalizuj serwerowi koniec wysyłania
    shutdown(sock, SD_SEND);

    std::string response;
    char buf[512];
    int n;
    while ((n = recv(sock, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = '\0';
        response += buf;
    }

    closesocket(sock);
    WSACleanup();
    return response;
}

std::string parseGameId(const std::string& response) {
    auto pos = response.find("\"game_id\"");
    if (pos == std::string::npos) return "";
    auto colon = response.find(':', pos);
    if (colon == std::string::npos) return "";
    auto q1 = response.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = response.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return response.substr(q1 + 1, q2 - q1 - 1);
}

std::string requestNewGame(const std::string& ip) {
    std::string resp = httpPost(ip, 8080, "/newgame", "{}");
    return parseGameId(resp);
}

void sendMove(const std::string& ip, const std::string& gameId,
              const std::string& move, const std::string& player) {
    std::string body = "{\"game_id\":\"" + gameId + "\","
                       "\"move\":\"" + move + "\","
                       "\"player\":\"" + player + "\"}";
    if (httpPost(ip, 8080, "/move", body).empty())
        std::cerr << "[network] Failed to send move.\n";
}

void sendStatus(const std::string& ip, const std::string& gameId,
                const std::string& status, const std::string& winner) {
    std::string body = "{\"game_id\":\"" + gameId + "\","
                       "\"status\":\"" + status + "\","
                       "\"winner\":\"" + winner + "\"}";
    httpPost(ip, 8080, "/status", body);
}

bool handleTurn(Board& board, const Movelist& moves,
                const std::string& playerName,
                const std::string& serverIp, const std::string& gameId,
                bool networked, std::list<std::string>& moveHistory)
{
    std::cout << playerName << " to move\n";
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
    if (networked) sendMove(serverIp, gameId, input, playerName);
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

    if (networked) {
        gameId = requestNewGame(serverIp);
        if (gameId.empty()) {
            std::cout << "Could not reach server - running in local-only mode.\n\n";
            networked = false;
        } else {
            std::cout << "\n";
            std::cout << "╔══════════════════════════════════════╗\n";
            std::cout << "║           GAME STARTED               ║\n";
            std::cout << "╠══════════════════════════════════════╣\n";
            std::cout << "║  Game ID : " << gameId << "                   ║\n";
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

        std::string playerName = (board.sideToMove() == Color::WHITE) ? "White" : "Black";
        std::string opponent   = (board.sideToMove() == Color::WHITE) ? "Black" : "White";

        if (moves.size() == 0) {
            std::string status;
            if (board.inCheck()) {
                status = "checkmate";
                winner = opponent;
                std::cout << "\n*** Checkmate! " << winner << " wins! ***\n";
            } else {
                status = "stalemate";
                winner = "Draw";
                std::cout << "\n*** Stalemate! It's a draw. ***\n";
            }
            if (networked) sendStatus(serverIp, gameId, status, winner);
            break;
        }

        while (!handleTurn(board, moves, playerName, serverIp, gameId, networked, moveHistory))
            ;
    }

    std::cout << "\n--- Game history ---\n";
    std::cout << "Game ID : " << (gameId.empty() ? "local" : gameId) << "\n";
    std::cout << "Result  : " << (winner.empty() ? "unknown" : winner) << "\n\n";
    int i = 1;
    for (const auto& m : moveHistory)
        std::cout << i++ << ". " << m << "\n";

    std::cout << "\nPress Enter to exit...";
    std::cin.ignore();
    std::cin.get();

    return 0;
}
