# Chess Clock Pi — Instrukcje

## Jak to działa (ogólny schemat)

```
 [Telefon z app]
       |
   Bluetooth RFCOMM (SPP)
       |
  [Raspberry Pi]  ←──────── main.cpp
       |
   HTTP (JSON)
       |
  [Go server]    ←──────── server.go
       |
    Browser      ←──────── viewer.html
```

1. **Raspberry Pi** uruchamia `chess_pi` — wyświetla zegar szachowy (SDL2), serwer Bluetooth i kod QR.
2. **Telefon** paruje się przez Bluetooth i łączy przez RFCOMM. Kod QR ułatwia odczyt adresu MAC.
3. **Pi** odbiera komendy z telefonu (ruchy, starty gry, pauzy), waliduje je biblioteką szachową i zapisuje.
4. **Pi** wysyła każdy ruch i wyniki do **serwera Go** przez HTTP.
5. **Serwer Go** trzyma historię gry w SQLite i pcha aktualizacje do przeglądarki przez SSE.
6. **Przeglądarka** na `http://<ip_serwera>:8080` pokazuje partię na żywo.

---

## Wymagania sprzętowe

| Element | Co |
|---|---|
| Raspberry Pi 3/4/5 | lub dowolny Linux z Bluetooth |
| Wyświetlacz | ekran dotykowy lub monitor HDMI |
| Adapter Bluetooth | wbudowany w Pi 3/4/5 wystarczy |

---

## Instalacja zależności (Raspberry Pi OS / Debian / Ubuntu)

```bash
# Aktualizacja systemu
sudo apt update && sudo apt upgrade -y

# SDL2 — grafika
sudo apt install -y libsdl2-dev libsdl2-ttf-dev

# BlueZ — Bluetooth
sudo apt install -y bluez libbluetooth-dev

# libqrencode — generowanie QR
sudo apt install -y libqrencode-dev

# Go — do uruchomienia serwera
sudo apt install -y golang

# Narzędzia budowania
sudo apt install -y build-essential git
```

---

## Pobranie projektu

```bash
git clone <url-repozytorium> Szachy
cd Szachy/clawd
```

Upewnij się że folder `chess-library/include/chess.hpp` istnieje (jest dołączona do repozytorium).

---

## Kompilacja

```bash
# Skompiluj klienta Pi
make

# Skompiluj serwer Go (opcjonalnie, jeśli chcesz relay)
make server
```

Po sukcesie będą pliki:
- `./chess_pi`       — program na Pi
- `./chess_server`   — serwer Go (opcjonalny)

---

## Uruchomienie

### Bez serwera (tylko lokalnie)

```bash
sudo ./chess_pi
```

> `sudo` jest wymagane do otwarcia gniazda Bluetooth RFCOMM.

### Z serwerem Go (pełny tryb)

**Terminal 1 — serwer (może być na innej maszynie):**

```bash
./chess_server
# Serwer słucha na :8080
```

**Terminal 2 — Pi:**

```bash
sudo ./chess_pi 192.168.1.100:8080 1
#                ^^^^^^^^^^^^^^^^^  ^
#                IP:port serwera    kanał Bluetooth RFCOMM
```

Parametry:
- `192.168.1.100:8080` — adres serwera Go (opcjonalny, bez = tryb lokalny)
- `1` — numer kanału RFCOMM (domyślnie 1)

---

## Parowanie telefonu z Pi

### Krok 1: Włącz Bluetooth i ustaw Pi jako wykrywalny

```bash
sudo bluetoothctl
power on
discoverable on
pairable on
agent on
default-agent
```

Zostaw to okno otwarte — pojawi się tu prośba o potwierdzenie PIN.

### Krok 2: Sparuj telefon

Na telefonie (Android lub iOS):
1. Otwórz **Ustawienia → Bluetooth**.
2. Wyszukaj urządzenia — powinno pojawić się `raspberrypi` lub `chess-clock-pi`.
3. Stuknij i potwierdź PIN (zazwyczaj `0000` lub `1234`).
4. W terminalu Pi wpisz `yes` aby zaakceptować parowanie.

### Krok 3: Skanowanie kodu QR (odczyt adresu MAC)

Na ekranie Pi wyświetli się kod QR kodujący:

```
chessclock://XX:XX:XX:XX:XX:XX/1?server=192.168.1.100:8080
```

Zeskanuj go dowolną aplikacją (aparat telefonu, Google Lens). Zobaczysz MAC adres i numer kanału — potrzebne do połączenia.

---

## Połączenie przez Bluetooth (telefon → Pi)

### Zalecana aplikacja

**Android:** *Serial Bluetooth Terminal* (darmowa, Google Play)

1. Otwórz aplikację → Menu → Devices.
2. Wybierz sparowane `raspberrypi`.
3. Połącz.
4. Możesz teraz wysyłać komendy.

**iOS:** *Bluetooth Serial* lub *BLE Terminal* (szukaj "RFCOMM Serial" — iOS ma ograniczenia BT Classic).

> Uwaga: iOS domyślnie nie obsługuje klasycznego Bluetooth SPP. Zalecamy urządzenie Android lub komputer z laptopem jako klientem.

---

## Protokół Bluetooth — komendy

Komendy wysyłane z telefonu do Pi (tekst, zakończony `\n`):

| Komenda | Opis | Przykład |
|---|---|---|
| `HELLO` | Test połączenia | `HELLO` |
| `NEWGAME\|Biali\|Czarni\|minuty\|inkrement_s` | Nowa gra | `NEWGAME\|Kowalski\|Nowak\|5\|0` |
| `MOVE\|<uci>` | Wyślij ruch w formacie UCI | `MOVE\|e2e4` |
| `PAUSE` | Pauza zegara | `PAUSE` |
| `RESUME` | Wznowienie po pauzie | `RESUME` |
| `RESET` | Reset gry (te same ustawienia) | `RESET` |
| `ARBITER_STOP` | Arbiter zatrzymuje zegar | `ARBITER_STOP` |
| `ARBITER_RESUME` | Arbiter wznawia po zatrzymaniu | `ARBITER_RESUME` |
| `ERROR\|white` | Rejestracja błędu gracza (2 = przegrana) | `ERROR\|black` |
| `BONUS\|white\|<ms>` | Arbiter dodaje czas (w ms) | `BONUS\|white\|120000` |
| `QUIT` | Zakończ sesję | `QUIT` |

### Odpowiedzi Pi do telefonu

| Odpowiedź | Znaczenie |
|---|---|
| `HELLO\|chess_clock_pi` | Pi żyje |
| `OK\|newgame\|<id>` | Gra zarejestrowana, `id` = numer gry na serwerze |
| `MOVE_ACCEPTED\|<uci>` | Ruch zaakceptowany |
| `ERR\|illegal_move\|e2e5` | Ruch nielegalny |
| `ERR\|bad_format\|...` | Nieznany format ruchu |
| `CLOCK\|<ms_biali>\|<ms_czarni>\|<aktywny>\|<stan>` | Stan zegarów co 500ms |
| `GAME_OVER\|<winner>\|<reason>` | Koniec gry (`checkmate`, `stalemate`, `timeout`) |

### Przykładowa sesja

```
→  HELLO
←  HELLO|chess_clock_pi
→  NEWGAME|Kowalski|Nowak|5|0
←  OK|newgame|42
→  MOVE|e2e4
←  MOVE_ACCEPTED|e2e4
←  CLOCK|298341|300000|black|3
→  MOVE|e7e5
←  MOVE_ACCEPTED|e7e5
...
←  GAME_OVER|White|checkmate
```

---

## Obsługa ekranu Pi (SDL2)

Ekran Pi działa równolegle z Bluetooth. Możesz go używać bez telefonu.

| Ekran | Co widać |
|---|---|
| **QR** | Kod QR + adres MAC + status BT. Pojawia się na starcie. |
| **Setup** | Ustawienie czasu i inkrementu, przycisk START. |
| **Gra** | Dwa zegary (Biały/Czarny), przyciski PAUZA, RESET, ARBITER. |
| **Arbiter** | Przyciski błędów i bonusów dla obu graczy. |
| **Pomoc** | Lista skrótów klawiszowych. |

### Klawiatura (kiedy Pi jest podłączone do klawiatury)

| Klawisz | Akcja |
|---|---|
| `SPACJA` | Pauza / Wznów |
| `R` | Reset zegara |
| `H` | Pomoc |
| `ESC` | Wróć do Setup / zamknij |
| `A` | Arbiter: Stop |
| `Q` | Arbiter: Wznów |
| `1` / `2` | Błąd Białego / Czarnego |
| `3` / `4` | +2 min Białemu / Czarnemu |

### Mysz / dotyk

- Klikasz lewą lub prawą połowę ekranu podczas gry → przełączasz zegar (jak naciśnięcie przycisku na zegarze szachowym).
- Przyciski na dole — PAUZA, RESET, ARBITER.
- `X` w prawym górnym rogu — zamknij.

---

## Oglądanie gry w przeglądarce

Jeśli serwer Go jest uruchomiony:

```
http://<ip_serwera>:8080
```

Strona pokazuje listę partii. Kliknij na partię, żeby zobaczyć na żywo aktualizowane ruchy, zegary i historię.

---

## Struktura plików projektu

```
clawd/
├── main.cpp            ← Program Pi (ten plik)
├── Makefile            ← Kompilacja
├── server.go           ← Serwer Go (relay + baza danych)
├── viewer.html         ← Przeglądarka partii (wbudowana w serwer)
├── chess.db            ← Baza SQLite (tworzona automatycznie)
├── chess-library/      ← Biblioteka szachowa (header-only C++)
│   └── include/
│       └── chess.hpp
├── gry/                ← Przykładowe partie (JSON/PGN)
└── github/             ← Oryginalny kod zegara w C (źródło portu)
    ├── Makefile
    └── src/
        ├── main.c
        ├── clock.c / clock.h
        ├── ui.c / ui.h
        └── app.h
```

---

## Rozwiązywanie problemów

### "bluetooth socket(): Permission denied"
Uruchom z `sudo`:
```bash
sudo ./chess_pi
```

Lub dodaj uprawnienia do gniazd Bluetooth dla użytkownika:
```bash
sudo setcap 'cap_net_raw,cap_net_admin+eip' ./chess_pi
```

### "TTF_OpenFont failed"
Zainstaluj czcionki DejaVu:
```bash
sudo apt install fonts-dejavu-core
```

### Telefon nie widzi Pi w skanowaniu BT
```bash
sudo bluetoothctl
power on
discoverable on
```

### Serwer Go: "no such table: games"
Usuń plik `chess.db` — zostanie odtworzony:
```bash
rm chess.db && ./chess_server
```

### Pi nie łączy się z serwerem Go
Sprawdź czy serwer działa i czy Pi ma dostęp sieciowy:
```bash
curl -s http://192.168.1.100:8080/state
```

---

## Automatyczne uruchomienie po starcie (opcjonalnie)

Stwórz plik serwisu systemd `/etc/systemd/system/chess-pi.service`:

```ini
[Unit]
Description=Chess Clock Pi
After=bluetooth.target network.target

[Service]
Type=simple
User=root
WorkingDirectory=/home/pi/clawd
ExecStart=/home/pi/clawd/chess_pi 192.168.1.100:8080 1
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Włącz:
```bash
sudo systemctl enable chess-pi
sudo systemctl start chess-pi
```
