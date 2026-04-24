# Szachy — System Zegara Szachowego

Kompletna dokumentacja systemu zdalnego zegara szachowego opartego na Raspberry Pi.

---

## Spis treści

1. [Architektura](#architektura)
2. [Wymagania](#wymagania)
3. [Instalacja](#instalacja)
4. [Konfiguracja](#konfiguracja)
5. [Pierwsze uruchomienie](#pierwsze-uruchomienie)
6. [Generowanie kluczy API](#generowanie-kluczy-api)
7. [Zarządzanie kluczami](#zarzadzanie-kluczami)
8. [Uruchamianie systemu](#uruchamianie-systemu)
9. [Endpointy API](#endpointy-api)
10. [Panel administratora](#panel-administratora)
11. [Przeglądarka partii](#przegladarka-partii)
12. [Testy](#testy)
13. [Rozwiązywanie problemów](#rozwiazywanie-problemow)

---

## Architektura

```
Telefon (Bluetooth)
        │  RFCOMM
        ▼
┌──────────────────┐
│   main.cpp       │  Raspberry Pi — odbiera ruchy, waliduje, relayuje
│   (C++/SDL2)     │
└──────┬───────────┘
       │ HTTP POST  (port 9090)
       ▼
┌──────────────────┐
│  api/server.ts   │  Brama API (Node.js/Express) — weryfikacja kluczy
│  (TypeScript)    │  Argon2id + blokada IP
└──────┬───────────┘
       │ HTTP POST  (port 8080)
       ▼
┌──────────────────┐         ┌──────────────────┐
│   server.go      │◄────────│ Przeglądarka SSE  │  viewer.html
│   (Go)           │  SSE    │ (dowolna przegl.) │
│   chess.db       │────────►│                  │
└──────┬───────────┘         └──────────────────┘
       │
       ▼
┌──────────────────┐
│  admin.html      │  Panel administracyjny (tylko localhost/auth)
└──────────────────┘
```

### Trzy warstwy bezpieczeństwa

| Warstwa | Ochrona |
|---------|---------|
| Klucz publiczny (`pk_…`) | Odczyt stanu gry |
| Klucz prywatny (`sk_…`) | Zapis ruchów, nowa gra, zakończenie |
| Hasło admina (bcrypt) | Panel administracyjny |

---

## Wymagania

### Serwer / Komputer główny
- **Go** ≥ 1.21
- **Node.js** ≥ 20 + **npm**
- System Linux / macOS (Windows: wymaga WSL2)

### Raspberry Pi (klient zegarowy)
- **Raspberry Pi** 3B+ lub nowszy
- Bluetooth wbudowany lub adapter USB
- **BlueZ** (stos Bluetooth Linuxa)
- **SDL2** + **SDL2_ttf** (opcjonalnie: UI na ekranie)
- Kompilator C++17 z obsługą pthreads

---

## Instalacja

### 1. Brama API (TypeScript)

```bash
cd api
npm install
```

### 2. Serwer Go

```bash
# Pobierz zależności
go mod download

# Skompiluj (opcjonalnie — można uruchamiać przez `go run`)
go build -o chess_server server.go
```

### 3. Klient Raspberry Pi (main.cpp)

```bash
# Zainstaluj biblioteki systemowe
sudo apt install libsdl2-dev libsdl2-ttf-dev libbluetooth-dev

# Skompiluj
g++ -std=c++17 -O2 main.cpp -o chess_pi \
    -lSDL2 -lSDL2_ttf -lbluetooth -lpthread
```

---

## Konfiguracja

### `config.json` — Konfiguracja serwera Go

```json
{
  "server": {
    "port": 8080,           // Port HTTP serwera Go
    "host": "0.0.0.0"       // Adres nasłuchu (0.0.0.0 = wszystkie interfejsy)
  },
  "database": {
    "path": "./chess.db",   // Ścieżka do bazy SQLite z partiami
    "wal_mode": true        // WAL = lepsza wydajność współbieżna
  },
  "chess": {
    "default_time_control_minutes": 5,    // Domyślna kontrola czasu
    "default_increment_seconds": 0,        // Increment (0 = brak)
    "white_player_default": "Biały",       // Domyślna nazwa gracza białymi
    "black_player_default": "Czarny"       // Domyślna nazwa gracza czarnymi
  },
  "files": {
    "games_directory": "./gry",  // Katalog zapisu PGN i JSON
    "save_pgn": true,            // Zapisuj pliki .pgn po zakończeniu gry
    "save_json": true            // Zapisuj pliki .json po zakończeniu gry
  },
  "viewer": {
    "viewer_file": "viewer.html",         // Ścieżka do pliku HTML viewera
    "enable_sse": true,                   // Włącz Server-Sent Events
    "keepalive_interval_seconds": 30      // Jak często wysyłać keepalive SSE
  }
}
```

---

### `clock.json` — Konfiguracja klienta Raspberry Pi

Plik tworzony po wygenerowaniu kluczy (patrz niżej). Umieść go obok pliku `chess_pi`.

```json
{
  "clock_code":  "CHS-XXXX-XXXX-XXXX",
  "public_key":  "pk_...",
  "private_key": "sk_...",
  "server":      "192.168.1.100:9090",
  "bt_channel":  1
}
```

| Pole | Opis |
|------|------|
| `clock_code` | Unikalny identyfikator zegara (z `npm run generate`) |
| `public_key` | Klucz do odczytu stanu gry |
| `private_key` | Klucz do wysyłania ruchów / zarządzania grą |
| `server` | Adres IP:port bramy API (TypeScript, domyślnie port 9090) |
| `bt_channel` | Kanał RFCOMM Bluetooth (1–30) |

> **Uwaga:** Parametry z `clock.json` można nadpisać argumentami wiersza poleceń.

---

### `admin.json` — Dane administratora (auto-generowany)

Plik tworzony automatycznie przy pierwszym uruchomieniu `server.go`. Zawiera:

```json
{
  "username": "admin",
  "password_hash": "$2a$10$...",   // bcrypt hash — NIE zmieniaj ręcznie
  "session_ttl_minutes": 60
}
```

> **Nigdy nie udostępniaj** tego pliku. Zawiera hash bcrypt hasła admina.  
> Jeśli chcesz zresetować hasło — usuń plik, uruchom serwer ponownie.

---

## Pierwsze uruchomienie

### Krok 1 — Uruchom serwer Go

```bash
./chess_server
# lub: go run server.go
```

Przy pierwszym uruchomieniu serwer wypisze:

```
♟ Ładowanie konfiguracji...
✅ Konfiguracja wczytana
✅ Baza danych gotowa
✅ Katalog gier gotów
✅ Dane administratora wygenerowane (pierwsze uruchomienie):
   ┌──────────────────────────────────┐
   │  Użytkownik : admin              │
   │  Hasło      : xKj4mPqR8vNz      │
   └──────────────────────────────────┘
   Hash zapisany do admin.json — zaloguj się na /admin
✅ Endpointy HTTP zarejestrowane (w tym /admin)
```

> **Zapisz hasło admina!** Jest wyświetlane tylko raz.  
> Jeśli je zgubisz — usuń `admin.json` i uruchom serwer ponownie.

### Krok 2 — Wygeneruj klucze API dla zegara

```bash
cd api
npm run generate -- "Zegar Sali A"
```

Wynik:

```
────────────────────────────────────────────────────────────────────────
  NEW CLOCK REGISTERED
────────────────────────────────────────────────────────────────────────
  clock code       : CHS-8KJ4-MPQR-XT3W
  label            : Zegar Sali A
  public API key   : pk_xBz9mK4nPqR7vJtY2wL0sD5hF1cE8aG...
  private API key  : sk_nQ3kR6pM8tX1vY9wZ4sJ7bF2hE5cA0...
────────────────────────────────────────────────────────────────────────
  ⚠  Save the keys NOW — they are shown only once.
```

> **Skopiuj wszystkie trzy wartości natychmiast!**  
> Baza danych przechowuje tylko hasze Argon2id — kluczy nie można odzyskać.

### Krok 3 — Uzupełnij `clock.json`

Skopiuj wygenerowane wartości do pliku `clock.json` na Raspberry Pi.

### Krok 4 — Uruchom bramę API

```bash
cd api
npm run server
# lub z nadpisaniem portów:
PORT=9090 GO_SERVER=http://127.0.0.1:8080 npm run server
```

### Krok 5 — Uruchom klienta na Raspberry Pi

```bash
./chess_pi
# lub z argumentami (nadpisują clock.json):
./chess_pi 192.168.1.100:9090 1 CHS-8KJ4-MPQR-XT3W sk_nQ3...
```

---

## Generowanie kluczy API

Każdy zegar szachowy ma trzy identyfikatory:

| Identyfikator | Format | Entropia | Zastosowanie |
|---------------|--------|----------|--------------|
| Kod zegara | `CHS-XXXX-XXXX-XXXX` | ~60 bitów | Lookup w bazie danych |
| Klucz publiczny | `pk_<43 znaki base64url>` | 256 bitów | Odczyt stanu gry |
| Klucz prywatny | `sk_<43 znaki base64url>` | 256 bitów | Zapis ruchów i zarządzanie |

```bash
# Generuj nowy zegar z etykietą
cd api
npm run generate -- "Nazwa Zegara"

# Generuj bez etykiety
npm run generate
```

### Jak działają klucze

1. Generowany jest losowy 32-bajtowy klucz (CSPRNG)
2. Klucz jest hashowany algorytmem **Argon2id** (19 MiB RAM, 2 iteracje)
3. W bazie `clocks.db` przechowywany jest **tylko hash** + kod zegara
4. Plaintext klucza jest wyświetlany **jednorazowo** w terminalu

### Format kodu zegara

```
CHS - 8KJ4 - MPQR - XT3W
 ^      ^      ^      ^
 |      |      |      |
 |   4 znaki Crockford base32 (każda grupa)
 |
 Prefiks stały
```

Alfabet Crockford base32 wyklucza myląco podobne znaki (I, L, O, U).

---

## Zarządzanie kluczami

### Lista wszystkich zegarów

```bash
cd api
npm run list
```

Wynik:

```
ID  Kod zegara            Etykieta        Ostatnie użycie     Aktywny
─────────────────────────────────────────────────────────────────────
1   CHS-8KJ4-MPQR-XT3W   Zegar Sali A   2025-04-20 14:32    ✅
2   CHS-2MN9-KRQT-WZ5X   Zegar Sali B   nigdy               ✅
3   CHS-7VPS-BHJD-CT4R   Stary zegar    2025-03-01 08:00    ❌ (odwołany)
```

### Weryfikacja klucza

```bash
cd api
npm run verify -- CHS-8KJ4-MPQR-XT3W pk_xBz9mK4n...
```

### Odwołanie klucza (unieważnienie)

```bash
cd api
npm run revoke -- CHS-8KJ4-MPQR-XT3W
```

> Po odwołaniu wszystkie żądania z tym kodem zegara zwrócą `401 Unauthorized`.  
> Operacja jest nieodwracalna — trzeba wygenerować nowy zegar.

---

## Uruchamianie systemu

### Kolejność uruchamiania

```
1. Serwer Go (chess_server / go run server.go)
2. Brama API (npm run server w katalogu api/)
3. Klient Pi (./chess_pi)
```

### Serwer Go

```bash
# Uruchomienie
go run server.go
# lub skompilowany:
./chess_server

# Zmiana lokalizacji config.json (domyślnie ./config.json)
# Plik musi być w katalogu roboczym
```

**Porty i ścieżki:**
- Nasłuch: `0.0.0.0:8080` (lub według `config.json`)
- Panel admina: `http://localhost:8080/admin`
- Przeglądarka: `http://localhost:8080/`
- API gier: `http://localhost:8080/state`, `/newgame`, `/move`, `/status`
- SSE: `http://localhost:8080/events`

### Brama API (TypeScript)

```bash
cd api
npm run server

# Zmienne środowiskowe:
PORT=9090                          # Port nasłuchu (domyślnie 9090)
GO_SERVER=http://127.0.0.1:8080    # URL serwera Go
CLOCKS_DB=./clocks.db              # Ścieżka do bazy kluczy
```

**Adresy:** `http://0.0.0.0:9090`

### Klient Raspberry Pi

```bash
# Używa clock.json z katalogu roboczego
./chess_pi

# Nadpisanie przez argumenty wiersza poleceń:
./chess_pi <serwer:port> <kanał_bt> <kod_zegara> <klucz_prywatny>

# Przykład:
./chess_pi 192.168.1.100:9090 1 CHS-8KJ4-MPQR-XT3W sk_nQ3kR6...
```

**Protokół Bluetooth (RFCOMM):**

```
HELLO                      → powitanie po połączeniu
NEWGAME|Biały|Czarny|5|3   → nowa gra (5 min, 3 sek increment)
MOVE|e2e4                  → ruch w notacji UCI
PAUSE                      → pauza zegara
RESUME                     → wznowienie
RESET                      → reset gry
ARBITER_STOP               → zakończenie przez arbitra
ERROR|czas_białych|czas_czarnych  → koniec czasu
BONUS|biały_ms|czarny_ms|czas_ms  → bonus czasu (bramka itp.)
QUIT                       → rozłącz
```

---

## Endpointy API

### Brama API (port 9090) — wymaga kluczy

Wszystkie żądania muszą zawierać:
- Nagłówek `X-Clock-Code: CHS-XXXX-XXXX-XXXX`
- Nagłówek `X-API-Key: pk_...` lub `sk_...`  
  (alternatywnie: `Authorization: Bearer <klucz>`)

#### `GET /api/health`
Sprawdzenie działania serwera. **Nie wymaga autentykacji.**

```bash
curl http://localhost:9090/api/health
```
```json
{ "ok": true, "service": "chess-clock-api" }
```

---

#### `GET /api/clock/info`
Informacje o zegarze. Wymaga klucza **publicznego lub prywatnego**.

```bash
curl http://localhost:9090/api/clock/info \
  -H "X-Clock-Code: CHS-8KJ4-MPQR-XT3W" \
  -H "X-API-Key: pk_xBz9mK4n..."
```
```json
{
  "ok": true,
  "clock_code": "CHS-8KJ4-MPQR-XT3W",
  "label": "Zegar Sali A",
  "scope": "public"
}
```

---

#### `GET /api/clock/state`
Stan bieżącej gry. Wymaga klucza **publicznego lub prywatnego**.

```bash
curl http://localhost:9090/api/clock/state \
  -H "X-Clock-Code: CHS-8KJ4-MPQR-XT3W" \
  -H "X-API-Key: pk_..."

# Stan konkretnej gry:
curl "http://localhost:9090/api/clock/state?id=3" ...
```
```json
[
  {
    "id": 1,
    "white_player": "Alicja",
    "black_player": "Bartek",
    "status": "ongoing",
    "winner": "",
    "created_at": "2025-04-20 14:00:00",
    "time_control": 300,
    "white_time_ms": 287500,
    "black_time_ms": 294000,
    "moves": [
      { "move_number": 1, "player": "White", "move": "e2e4", "timestamp": "14:00:05" }
    ]
  }
]
```

---

#### `POST /api/clock/newgame`
Nowa gra. Wymaga klucza **prywatnego**.

```bash
curl -X POST http://localhost:9090/api/clock/newgame \
  -H "Content-Type: application/json" \
  -H "X-Clock-Code: CHS-8KJ4-MPQR-XT3W" \
  -H "X-API-Key: sk_..." \
  -d '{
    "white_player": "Alicja",
    "black_player": "Bartek",
    "time_control_ms": 300000
  }'
```
```json
{ "game_id": 7 }
```

| Pole | Typ | Opis |
|------|-----|------|
| `white_player` | string | Imię gracza białymi (opcjonalne) |
| `black_player` | string | Imię gracza czarnymi (opcjonalne) |
| `time_control_ms` | number | Czas na grę w milisekundach |
| `time_control_sec` | number | Czas na grę w sekundach (legacy) |

---

#### `POST /api/clock/move`
Rejestracja ruchu. Wymaga klucza **prywatnego**.

```bash
curl -X POST http://localhost:9090/api/clock/move \
  -H "Content-Type: application/json" \
  -H "X-Clock-Code: CHS-8KJ4-MPQR-XT3W" \
  -H "X-API-Key: sk_..." \
  -d '{
    "game_id": 7,
    "move": "e2e4",
    "player": "White",
    "time_left_ms": 287500
  }'
```
```json
{ "ok": true, "move_number": 1 }
```

| Pole | Typ | Opis |
|------|-----|------|
| `game_id` | number | ID gry z `/newgame` |
| `move` | string | Ruch w notacji UCI (`e2e4`, `g1f3`) |
| `player` | string | `"White"` lub `"Black"` |
| `time_left_ms` | number | Pozostały czas gracza który ruszył |
| `white_time_ms` | number | (legacy) Czas białych bezpośrednio |
| `black_time_ms` | number | (legacy) Czas czarnych bezpośrednio |

---

#### `POST /api/clock/status`
Zakończenie gry. Wymaga klucza **prywatnego**.

```bash
curl -X POST http://localhost:9090/api/clock/status \
  -H "Content-Type: application/json" \
  -H "X-Clock-Code: CHS-8KJ4-MPQR-XT3W" \
  -H "X-API-Key: sk_..." \
  -d '{
    "game_id": 7,
    "status": "checkmate",
    "winner": "White"
  }'
```
```json
{ "ok": true }
```

| `status` | Znaczenie |
|----------|-----------|
| `checkmate` | Mat |
| `stalemate` | Pat / remis |
| `timeout` | Przekroczenie czasu |
| `draw` | Remis przez zgodę |
| `resign` | Poddanie się |

---

### Kody błędów autentykacji

| Kod HTTP | `error` | Znaczenie |
|----------|---------|-----------|
| 400 | `missing_clock_code` | Brak nagłówka `X-Clock-Code` |
| 400 | `missing_api_key` | Brak nagłówka `X-API-Key` |
| 400 | `invalid_key_prefix` | Klucz nie zaczyna się od `pk_` ani `sk_` |
| 401 | `unknown_or_revoked_clock` | Nieznany lub odwołany kod zegara |
| 401 | `bad_api_key` | Nieprawidłowy klucz (zły hash) |
| 403 | `private_key_required` | Endpoint wymaga `sk_`, podano `pk_` |
| 429 | `too_many_auth_failures` | Zablokowany IP (5 lub 20 minut) |

---

### Blokada IP (Rate Limiting)

Brama API blokuje adresy IP po nieudanych próbach autentykacji:

```
Pierwsza nieudana próba  → blokada na  5 minut
Każda kolejna próba      → blokada na 20 minut
```

Przy blokadzie odpowiedź zawiera:

```json
{
  "ok": false,
  "error": "too_many_auth_failures",
  "retry_after_seconds": 287
}
```

---

### Serwer Go (port 8080) — endpointy wewnętrzne

> Te endpointy są dostępne bezpośrednio (bez autentykacji kluczami).  
> W produkcji **nie eksponuj portu 8080** poza sieć lokalną — cały ruch zewnętrzny powinien przechodzić przez bramę TypeScript (9090).

| Metoda | Ścieżka | Opis |
|--------|---------|------|
| `GET` | `/` | Przeglądarka partii (viewer.html) |
| `GET` | `/state` | Lista gier lub `?id=N` dla jednej |
| `GET` | `/events` | SSE — push aktualizacji do przeglądarki |
| `POST` | `/newgame` | Nowa gra |
| `POST` | `/move` | Rejestracja ruchu |
| `POST` | `/status` | Zakończenie gry |
| `GET` | `/admin` | Panel administratora |
| `POST` | `/admin/api/login` | Logowanie admina (JSON) |
| `POST` | `/admin/api/logout` | Wylogowanie admina |
| `GET` | `/admin/api/stats` | Statystyki (wymaga sesji) |
| `GET` | `/admin/api/games` | Lista gier (wymaga sesji) |
| `DELETE` | `/admin/api/game?id=N` | Usuń grę (wymaga sesji) |
| `POST` | `/admin/api/change-password` | Zmień hasło admina (wymaga sesji) |

---

## Panel administratora

Panel admina dostępny pod adresem: `http://<adres_serwera>:8080/admin`

### Logowanie

1. Otwórz `http://localhost:8080/admin` w przeglądarce
2. Wpisz `admin` jako nazwę użytkownika
3. Wpisz hasło wyświetlone przy pierwszym uruchomieniu serwera
4. Kliknij **Zaloguj**

Sesja jest ważna przez 60 minut (konfigurowalnie w `admin.json`).

### Funkcje panelu

**Pulpit:**
- Karty statystyk: łączna liczba partii, trwające, zakończone, łączna liczba ruchów
- Informacje o serwerze: port, ścieżka do bazy, katalog gier
- Liczba aktywnych sesji admina
- Link do przeglądarki partii

**Partie:**
- Tabela wszystkich partii z detalami
- Status każdej partii (trwająca / mat / pat / timeout)
- Przycisk **Usuń** — usuwa grę i wszystkie jej ruchy (nieodwracalne)
- Automatyczne odświeżanie co 15 sekund

**Ustawienia:**
- Zmiana hasła administratora
  - Wymaga podania aktualnego hasła
  - Minimalna długość nowego hasła: 8 znaków
  - Po zmianie wszystkie aktywne sesje są unieważniane

### Bezpieczeństwo sesji

- Sesje przechowywane w pamięci RAM (znikają po restarcie serwera)
- Ciasteczko sesji: `HttpOnly`, `SameSite=Strict`
- Hasło hashowane algorytmem **bcrypt** (koszt: `DefaultCost = 10`)
- Timing-safe porównanie: sprawdzanie hasła trwa tyle samo niezależnie od nazwy użytkownika

### API panelu admina (JSON)

Wszystkie endpointy `/admin/api/*` (poza `/login`) wymagają ważnej sesji — albo ciasteczka `adm`, albo nagłówka `X-Admin-Token`.

```bash
# Logowanie
curl -c cookies.txt -X POST http://localhost:8080/admin/api/login \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"twoje_haslo"}'

# Statystyki
curl -b cookies.txt http://localhost:8080/admin/api/stats

# Lista gier
curl -b cookies.txt http://localhost:8080/admin/api/games

# Usuń grę #3
curl -b cookies.txt -X DELETE "http://localhost:8080/admin/api/game?id=3"

# Zmień hasło
curl -b cookies.txt -X POST http://localhost:8080/admin/api/change-password \
  -H "Content-Type: application/json" \
  -d '{"old_password":"stare","new_password":"nowe_haslo"}'

# Wyloguj
curl -b cookies.txt -X POST http://localhost:8080/admin/api/logout
```

---

## Przeglądarka partii

Przeglądarka dostępna pod adresem: `http://<adres_serwera>:8080/`

### Funkcje

- **Lista partii** — lewy panel, automatycznie odświeżany przez SSE
- **Szachownica** — renderowana w CSS/JS z Unicode figurami szachowymi
- **Zegary** — żywy odliczanie, złoty gdy aktywny, czerwony gdy < 30 sekund
- **Lista ruchów** — klikalne, można nawigować po historii partii
- **Status połączenia** — zielona kropka `live` lub czerwona `rozłączono`

### Nawigacja po partii

| Klawisz / Przycisk | Akcja |
|-------------------|-------|
| `←` / `▶` | Poprzedni ruch |
| `→` / `▶` | Następny ruch |
| `Home` / `⏮` | Pozycja startowa |
| `End` / `⏭` | Ostatni ruch |
| `L` | Skocz do ostatniego ruchu (live) |
| `● Live` | Wróć do śledzenia na żywo |

### Server-Sent Events (SSE)

Przeglądarka utrzymuje jedno trwałe połączenie HTTP zamiast odpytywać serwer:

```
event: list
data: [{...gra 1...}, {...gra 2...}]

event: game
data: {"id": 7, "moves": [...], ...}

: keepalive
```

Zdarzenie `list` — pełna lista gier (bez ruchów) wysyłana przy każdej zmianie.  
Zdarzenie `game` — pełne dane jednej gry wraz z ruchami.

---

## Testy

### Uruchomienie testów serwera Go

```bash
go test -v server_test.go server.go
```

### Lista testów (69 testów)

| Kategoria | Liczba | Co testuje |
|-----------|--------|-----------|
| `initDB` | 3 | Tworzenie tabel i kolumn |
| `getGame` / `getAllGames` | 3 | Odczyt z bazy |
| `handleNewGame` | 5 | Tworzenie gier, domyślni gracze, kontrola czasu |
| `handleMove` | 6 | Ruchy, czas, kaskadowe aktualizacje, błędy |
| `handleStatus` | 5 | Mat, pat, timeout, walidacja wejścia |
| `handleState` | 5 | Lista, jedna gra, 404, CORS |
| `handleViewer` | 3 | Fallback, plik, Content-Type |
| `handleEvents` | 2 | SSE: lista początkowa, nagłówki |
| `saveGameFiles` | 6 | PGN (wyniki 1-0 / 0-1 / ½-½), JSON, oba formaty |
| `broadcastUpdate` | 3 | SSE broadcast (bez/1/kilku subskrybentów) |
| Admin: `loadAdminCreds` | 2 | Auto-generowanie i wczytywanie |
| Admin: sesje | 4 | Tworzenie, wygaśnięcie, pusty/nieznany token |
| Admin: `handleAdminLogin` | 5 | OK, złe hasło, zła nazwa, zła metoda, złe JSON |
| Admin: `handleAdminLogout` | 2 | Niszczenie sesji, brak sesji |
| Admin: middleware | 1 | 401 bez tokenu |
| Admin: `handleAdminAPIStats` | 2 | Statystyki, info serwera |
| Admin: `handleAdminAPIGames` | 2 | Lista, pusta lista |
| Admin: `handleAdminAPIDeleteGame` | 5 | OK, cascade ruchów, brak ID, 404, zła metoda |
| Admin: `changePassword` | 3 | OK, złe hasło, za krótkie |
| Admin: `handleAdminDashboard` | 2 | Fallback, plik |

### Uruchomienie z filtrem

```bash
# Tylko testy admina
go test -v -run TestAdmin server_test.go server.go

# Tylko testy handlerów HTTP
go test -v -run TestHandle server_test.go server.go

# Konkretny test
go test -v -run TestHandleMove_KilkaRuchow server_test.go server.go
```

---

## Rozwiązywanie problemów

### Serwer Go nie startuje

```
❌ Błąd: nie znaleziono config.json
```
→ Upewnij się, że `config.json` jest w katalogu roboczym serwera.

```
❌ Błąd inicjalizacji bazy danych: ...
```
→ Sprawdź uprawnienia do katalogu z bazą (`chess.db`).

---

### Brama API zwraca błąd 401

```json
{ "error": "unknown_or_revoked_clock" }
```
→ Kod zegara nie istnieje w `clocks.db` lub został odwołany. Uruchom `npm run list`.

```json
{ "error": "bad_api_key" }
```
→ Klucz jest błędny lub pochodzi z innego zegara. Weryfikacja: `npm run verify`.

---

### IP zablokowane (429)

```json
{ "error": "too_many_auth_failures", "retry_after_seconds": 287 }
```
→ Poczekaj na minięcie czasu blokady lub zrestartuj bramę API (blokady są w pamięci RAM).

---

### Przeglądarka SSE nie działa

- Sprawdź, czy w `config.json` jest `"enable_sse": true`
- Sprawdź, czy proxy (nginx/Apache) przepuszcza `text/event-stream` bez buforowania
- Dodaj nagłówek `X-Accel-Buffering: no` jeśli używasz nginx

---

### Hasło admina zagubione

```bash
# Usuń plik z hashem — przy następnym starcie zostanie wygenerowane nowe
rm admin.json
# Uruchom serwer i zapisz wydrukowane hasło
go run server.go
```

---

### Bluetooth nie wykrywa telefonu (Raspberry Pi)

```bash
# Sprawdź status
sudo systemctl status bluetooth

# Uruchom tryb wykrywalności
sudo hciconfig hci0 piscan

# Sparuj ręcznie
bluetoothctl
> scan on
> pair <MAC_telefonu>
> trust <MAC_telefonu>
```

---

## Struktura plików

```
clawd/
├── server.go           # Serwer Go — gry, SSE, panel admina
├── server_test.go      # Testy (69 testów)
├── config.json         # Konfiguracja serwera Go
├── admin.json          # Hash hasła admina (auto-generowany)
├── admin.html          # Panel administracyjny
├── viewer.html         # Przeglądarka partii
├── clock.json          # Konfiguracja klienta Pi (utwórz ręcznie)
├── main.cpp            # Klient Raspberry Pi (C++)
├── chess.db            # Baza SQLite z partiami (auto-tworzona)
├── gry/                # Eksportowane pliki PGN i JSON
├── go.mod / go.sum     # Zależności Go
└── api/
    ├── package.json
    ├── src/
    │   ├── server.ts   # Brama API Express
    │   ├── auth.ts     # Weryfikacja kluczy Argon2id
    │   ├── crypto.ts   # Generowanie kluczy i kodów
    │   ├── db.ts       # SQLite — tabela kluczy
    │   ├── ratelimit.ts# Blokada IP
    │   ├── generate.ts # Skrypt: npm run generate
    │   ├── list.ts     # Skrypt: npm run list
    │   ├── revoke.ts   # Skrypt: npm run revoke
    │   └── verify.ts   # Skrypt: npm run verify
    └── clocks.db       # Baza SQLite z kluczami API (auto-tworzona)
```
