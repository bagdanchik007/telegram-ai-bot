# telegram-ai-bot

Telegram-Chatbot in modernem C++17, der Nachrichten an die **Anthropic Claude API**
(Messages API) weiterleitet – alternativ an jede **OpenAI-kompatible API**
(OpenAI, Ollama, vLLM, LM Studio, OpenRouter, …).

## Funktionen

- `/start` – Begrüßung, `/help` – Hilfe, `/clear` – Gesprächsverlauf löschen
- Jede normale Textnachricht wird an die KI geschickt, die Antwort kommt zurück
- Gesprächsgedächtnis pro Telegram-User (letzte 10 Nachrichten, nur im RAM)
- Long Polling (kein Webhook, keine öffentliche URL nötig)
- Fehlerbehandlung mit verständlichen Meldungen: fehlende Konfiguration,
  ungültiger API-Key, Rate Limit (inkl. `Retry-After`), überlastete API,
  Netzwerkprobleme, leere Antworten; automatische Wiederholung bei
  vorübergehenden Fehlern (2 Retries, Backoff)
- Lange Antworten werden automatisch auf mehrere Telegram-Nachrichten verteilt (Limit 4096 Zeichen)
- Sauberes Beenden mit `Ctrl+C` / `SIGTERM`
- Tokens und API-Keys ausschließlich über Umgebungsvariablen

## Projektstruktur

```
telegram-ai-bot/
├── CMakeLists.txt
├── .env.example          # Vorlage für die Umgebungsvariablen
├── .gitignore
├── README.md
├── include/
│   └── ai_client.hpp     # KI-Client: Konfiguration, Fehlertypen, Verlauf pro User
├── src/
│   ├── ai_client.cpp     # HTTP (libcurl) + JSON (nlohmann/json), Anthropic & OpenAI
│   └── main.cpp          # Telegram-Bot (tgbot-cpp), Befehle, Long-Polling-Schleife
└── build/                # entsteht beim Bauen (nicht im Git)
```

## Voraussetzungen

- Linux (getestet: Ubuntu 24.04, GCC 13, CMake 3.28), Compiler mit C++17
- CMake ≥ 3.16
- Bibliotheken: OpenSSL, libcurl, nlohmann/json (≥ 3.9), Boost (Header), **tgbot-cpp**
- Ein Bot-Token von [@BotFather](https://t.me/BotFather) und ein API-Key von
  [console.anthropic.com](https://console.anthropic.com) (oder einer OpenAI-kompatiblen API)

### Abhängigkeiten installieren (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    libssl-dev libcurl4-openssl-dev nlohmann-json3-dev libboost-system-dev
```

tgbot-cpp gibt es nicht als Distributionspaket – aus dem Quellcode bauen und installieren:

```bash
git clone https://github.com/reo7sp/tgbot-cpp.git
cd tgbot-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig
cd ..
```

> Getestet mit dem aktuellen Stand von tgbot-cpp (`master`). Liegt tgbot-cpp in einem
> eigenen Prefix, beim Konfigurieren `-DCMAKE_PREFIX_PATH=<Pfad>` angeben.

## Bauen

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Das Ergebnis ist `build/telegram-ai-bot`.

## Konfiguration

Der Bot liest **nur Umgebungsvariablen** (er parst keine `.env`-Datei selbst).

| Variable             | Pflicht | Beschreibung                                                   |
|----------------------|---------|----------------------------------------------------------------|
| `TELEGRAM_BOT_TOKEN` | ja      | Token von @BotFather                                           |
| `ANTHROPIC_API_KEY`  | ja\*    | Anthropic-API-Key (\*nicht nötig bei `AI_PROVIDER=openai`)     |
| `CLAUDE_MODEL`       | nein    | Modellname, Standard `claude-sonnet-5-5`                       |
| `MAX_TOKENS`         | nein    | Maximale Antwortlänge in Tokens, Standard `1024`               |
| `SYSTEM_PROMPT`      | nein    | System-Prompt (Standard: freundlicher deutschsprachiger Assistent) |
| `AI_PROVIDER`        | nein    | `anthropic` (Standard) oder `openai`                           |
| `OPENAI_API_KEY`     | bei `openai` | API-Key der OpenAI-kompatiblen API                        |
| `OPENAI_MODEL`       | nein    | Standard `gpt-4o-mini`                                         |
| `OPENAI_BASE_URL`    | nein    | Standard `https://api.openai.com/v1` (z. B. `http://localhost:11434/v1` für Ollama) |
| `OPENAI_TOKEN_FIELD` | nein    | `max_tokens` (Standard) oder `max_completion_tokens` für neuere OpenAI-Modelle |

## Starten

```bash
cp .env.example .env
nano .env                       # Token und API-Key eintragen

set -a; source .env; set +a     # Variablen in die Shell laden
./build/telegram-ai-bot
```

Oder ohne Datei:

```bash
TELEGRAM_BOT_TOKEN=123456:ABC... ANTHROPIC_API_KEY=sk-ant-... ./build/telegram-ai-bot
```

Dann den Bot in Telegram öffnen und `/start` senden. Beenden mit `Ctrl+C`.

### Als Dienst mit systemd (optional)

`/etc/systemd/system/telegram-ai-bot.service`:

```ini
[Unit]
Description=Telegram AI Bot
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/opt/telegram-ai-bot/telegram-ai-bot
EnvironmentFile=/etc/telegram-ai-bot.env
Restart=on-failure
RestartSec=5
DynamicUser=yes

[Install]
WantedBy=multi-user.target
```

`/etc/telegram-ai-bot.env` enthält die Variablen als `KEY=wert` (ohne `export`, Rechte `chmod 600`).
Danach: `sudo systemctl daemon-reload && sudo systemctl enable --now telegram-ai-bot`.

## Hinweise und Grenzen

- **Verlauf nur im RAM:** Nach einem Neustart ist das Gedächtnis leer.
- **Eine Nachricht nach der anderen:** `tgbot-cpp` verarbeitet Updates nacheinander.
  Dauert eine KI-Antwort lange, warten andere Nutzer solange. Für sehr viele gleichzeitige
  Nutzer wäre ein Thread-Pool nötig (der `AiClient` ist bereits thread-sicher).
- **Plain Text:** Antworten werden ohne Markdown-Formatierung gesendet, damit Telegram nie
  wegen ungültigem Markdown ablehnt.
- **Datenschutz:** Nachrichteninhalte werden nicht geloggt, nur Nutzer-ID und Länge.
- **Geheimnisse:** `.env` steht in `.gitignore`. Wurde ein Token versehentlich veröffentlicht,
  bei @BotFather mit `/revoke` neu erzeugen und den API-Key in der Anthropic-Console erneuern.
