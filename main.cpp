#include "chess-library/include/chess.hpp"
#include <iostream>
#include <list>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>

using namespace chess;
using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

// ─── Kontrola czasu ───────────────────────────────────────────────────────────

struct TimeControl {
    long long whiteMs = 0;
    long long blackMs = 0;
    bool      enabled = false;
};

std::string formatTime(long long ms) {
    if (ms < 0) ms = 0;
    long long secs = ms / 1000;
    long long mins = secs / 60;
    secs %= 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lld:%02lld", mins, secs);
    return std::string(buf);
}

void printClocks(const TimeControl& tc) {
    std::cout << "  +----------------------------+\n";
    std::cout << "  | Biali: " << formatTime(tc.whiteMs)
              << "   Czarni: " << formatTime(tc.blackMs) << " |\n";
    std::cout << "  +----------------------------+\n";
}

// ─── Siec ─────────────────────────────────────────────────────────────────────

std::string stripPort(const std::string& ip) {
    auto pos = ip.find(':');
    if (pos != std::string::npos) return ip.substr(0, pos);
    return ip;
}

std::string httpPost(const std::string& ip, int port,
                     const std::string& path, const std::string& body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv { 5, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
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
        << "Connection: close\r\n\r\n"
        << body;
    std::string reqStr = req.str();
    send(sock, reqStr.c_str(), reqStr.size(), 0);

    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        response += buf;
    }
    close(sock);
    return response;
}

std::string parseGameId(const std::string& response) {
    auto pos = response.find("\"game_id\"");
    if (pos == std::string::npos) return "";
    auto colon = response.find(':', pos);
    if (colon == std::string::npos) return "";
    size_t start = colon + 1;
    while (start < response.size() &&
           (response[start] == ' ' || response[start] == '\n' || response[start] == '\r'))
        start++;
    if (start >= response.size()) return "";
    if (response[start] == '"') {
        auto q2 = response.find('"', start + 1);
        if (q2 == std::string::npos) return "";
        return response.substr(start + 1, q2 - start - 1);
    }
    size_t end = start;
    while (end < response.size() && isdigit(response[end])) end++;
    return response.substr(start, end - start);
}

// Tworzy nowa gre na serwerze (z opcjonalna kontrola czasu)
std::string requestNewGame(const std::string& ip,
                           const std::string& whiteName,
                           const std::string& blackName,
                           long long timeControlMs) {
    std::ostringstream body;
    body << "{\"white_player\":\"" << whiteName << "\","
         << "\"black_player\":\"" << blackName << "\","
         << "\"time_control_ms\":" << timeControlMs << "}";
    std::cerr << "[debug] POST /newgame\n";
    std::string resp = httpPost(ip, 8080, "/newgame", body.str());
    std::cerr << "[debug] response: [" << resp << "]\n";
    std::string id = parseGameId(resp);
    std::cerr << "[debug] game_id: [" << id << "]\n";
    return id;
}

// Wysyla ruch z pozostalym czasem gracza po ruchu
void sendMove(const std::string& ip, const std::string& gameId,
              const std::string& move, const std::string& color,
              long long timeLeftMs) {
    std::ostringstream body;
    body << "{\"game_id\":" << gameId << ","
         << "\"move\":\"" << move << "\","
         << "\"player\":\"" << color << "\","
         << "\"time_left_ms\":" << timeLeftMs << "}";
    std::string resp = httpPost(ip, 8080, "/move", body.str());
    if (resp.empty())
        std::cerr << "[network] Ruch nie wyslany.\n";
    else if (resp.find("\"ok\":true") == std::string::npos)
        std::cerr << "[network] Blad serwera: " << resp.substr(0, 120) << "\n";
}

// Wysyla status koncowy gry
void sendStatus(const std::string& ip, const std::string& gameId,
                const std::string& status, const std::string& winner) {
    std::ostringstream body;
    body << "{\"game_id\":" << gameId << ","
         << "\"status\":\"" << status << "\","
         << "\"winner\":\"" << winner << "\"}";
    std::string resp = httpPost(ip, 8080, "/status", body.str());
    if (resp.empty())
        std::cerr << "[network] Status nie wyslany.\n";
}

// ─── Obsluga tury ─────────────────────────────────────────────────────────────
//
// Zwraca:
//   1  – poprawny ruch wykonany
//   0  – nieprawidlowy/nielegalny input, ponow probe
//  -1  – skonczyl sie czas (przegrana na czas)

int handleTurn(Board& board, const Movelist& moves,
               const std::string& playerName,
               const std::string& playerColor,
               const std::string& serverIp, const std::string& gameId,
               bool networked, std::list<std::string>& moveHistory,
               TimeControl& tc)
{
    long long& myTime = (playerColor == "White") ? tc.whiteMs : tc.blackMs;

    if (tc.enabled) {
        std::cout << "\n";
        printClocks(tc);
        if (myTime <= 0) {
            std::cout << "[" << playerColor << "] Czas minul!\n";
            return -1;
        }
    }

    std::cout << "[" << playerColor << "] " << playerName << " gra (UCI): ";
    std::cout.flush();

    // Mierzenie czasu ruchu
    auto t0 = Clock::now();
    std::string input;
    std::cin >> input;
    long long elapsed = std::chrono::duration_cast<Ms>(Clock::now() - t0).count();

    if (tc.enabled) {
        myTime -= elapsed;
        if (myTime <= 0) {
            myTime = 0;
            std::cout << "[" << playerColor << "] Skonczyl sie czas!\n";
            return -1;
        }
    }

    Move move;
    try {
        move = uci::uciToMove(board, input);
    } catch (...) {
        std::cout << "Nieprawidlowy format. Sprobuj ponownie.\n";
        return 0;
    }

    bool legal = false;
    for (auto m : moves)
        if (uci::moveToUci(m) == input) { legal = true; break; }

    if (!legal) {
        std::cout << "Nielegalny ruch. Sprobuj ponownie.\n";
        return 0;
    }

    board.makeMove(move);
    long long sendTime = tc.enabled ? myTime : 0;
    if (networked) sendMove(serverIp, gameId, input, playerColor, sendTime);
    moveHistory.push_back(input);
    return 1;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::string serverIp;
    std::cout << "Adres IP serwera (Enter = tryb lokalny): ";
    std::getline(std::cin, serverIp);
    serverIp = stripPort(serverIp);
    bool networked = !serverIp.empty();

    std::string whiteName = "White";
    std::string blackName = "Black";
    std::cout << "Nazwa gracza Bialego: ";
    std::getline(std::cin, whiteName);
    if (whiteName.empty()) whiteName = "White";
    std::cout << "Nazwa gracza Czarnego: ";
    std::getline(std::cin, blackName);
    if (blackName.empty()) blackName = "Black";

    // Konfiguracja kontroli czasu
    TimeControl tc;
    std::string tcChoice;
    std::cout << "Uzyc kontroli czasu? (t/n) [n]: ";
    std::getline(std::cin, tcChoice);
    if (!tcChoice.empty() && (tcChoice[0] == 't' || tcChoice[0] == 'T' ||
                               tcChoice[0] == 'y' || tcChoice[0] == 'Y')) {
        tc.enabled = true;
        std::cout << "Minuty na gracza [10]: ";
        std::string mStr;
        std::getline(std::cin, mStr);
        int minutes = mStr.empty() ? 10 : std::stoi(mStr);
        if (minutes < 1) minutes = 1;
        tc.whiteMs = tc.blackMs = (long long)minutes * 60 * 1000;
        std::cout << "Kontrola czasu: " << minutes << " min na gracza\n\n";
    }

    // Rejestracja gry na serwerze
    std::string gameId;
    if (networked) {
        long long tcMs = tc.enabled ? tc.whiteMs : 0;
        gameId = requestNewGame(serverIp, whiteName, blackName, tcMs);
        if (gameId.empty()) {
            std::cout << "Brak polaczenia z serwerem - tryb lokalny.\n\n";
            networked = false;
        } else {
            std::cout << "\n"
                      << "+=========================================+\n"
                      << "|             GRA ROZPOCZETA             |\n"
                      << "+=========================================+\n"
                      << "|  Numer gry : #" << gameId << "\n"
                      << "|  Biali     : " << whiteName << "\n"
                      << "|  Czarni    : " << blackName << "\n";
            if (tc.enabled)
                std::cout << "|  Czas      : " << formatTime(tc.whiteMs) << " na gracza\n";
            std::cout << "|  Widok     : http://" << serverIp << ":8080\n"
                      << "+=========================================+\n\n";
        }
    } else {
        std::cout << "Tryb lokalny (bez serwera).\n\n";
    }

    std::list<std::string> moveHistory;
    Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    std::string winner;

    // Glowna petla gry
    while (true) {
        Movelist moves;
        movegen::legalmoves(moves, board);

        bool        isWhite       = (board.sideToMove() == Color::WHITE);
        std::string playerName    = isWhite ? whiteName  : blackName;
        std::string playerColor   = isWhite ? "White"    : "Black";
        std::string opponentColor = isWhite ? "Black"    : "White";

        // Koniec gry (mat / pat)
        if (moves.size() == 0) {
            std::string status;
            if (board.inCheck()) {
                status = "checkmate";
                winner = opponentColor;
                std::cout << "\n*** Mat! "
                          << (isWhite ? blackName : whiteName)
                          << " (" << opponentColor << ") wygrywa! ***\n";
            } else {
                status = "stalemate";
                winner = "Draw";
                std::cout << "\n*** Pat! Remis. ***\n";
            }
            if (networked) sendStatus(serverIp, gameId, status, opponentColor);
            break;
        }

        // Tura gracza
        int result = handleTurn(board, moves, playerName, playerColor,
                                serverIp, gameId, networked, moveHistory, tc);

        if (result == -1) {
            // Przegrana na czas
            winner = opponentColor;
            std::cout << "\n*** " << playerName << " (" << playerColor
                      << ") przegral na czas!  "
                      << (isWhite ? blackName : whiteName)
                      << " (" << opponentColor << ") wygrywa! ***\n";
            if (tc.enabled) printClocks(tc);
            if (networked) sendStatus(serverIp, gameId, "timeout", opponentColor);
            break;
        }
        // result == 0 → petla ponowi probe automatycznie
    }

    // Podsumowanie partii
    std::cout << "\n--- Historia partii ---\n"
              << "Gra      : " << (gameId.empty() ? "lokalna" : "#" + gameId) << "\n"
              << "Biali    : " << whiteName << "\n"
              << "Czarni   : " << blackName << "\n"
              << "Wynik    : " << winner << "\n";
    if (tc.enabled) {
        std::cout << "Czas Biali  : " << formatTime(tc.whiteMs) << "\n"
                  << "Czas Czarni : " << formatTime(tc.blackMs) << "\n";
    }
    std::cout << "\n";
    int i = 1;
    for (const auto& m : moveHistory)
        std::cout << i++ << ". " << m << "\n";

    std::cout << "\nNacisnij Enter aby wyjsc...";
    std::cin.ignore();
    std::cin.get();
    return 0;
}
