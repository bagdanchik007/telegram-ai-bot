// main.cpp
//
// Telegram-KI-Bot mit tgbot-cpp (Long Polling) und Anthropic Claude API.
//
// Ablauf:
//   1. Konfiguration aus Umgebungsvariablen lesen (TELEGRAM_BOT_TOKEN, ANTHROPIC_API_KEY, ...)
//   2. Bot starten, Webhook entfernen, Long Polling in einer Schleife
//   3. Jede Nachricht: Befehl (/start, /help, /clear) oder Chat-Anfrage an die KI

#include <tgbot/tgbot.h>

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "ai_client.hpp"

namespace {

// Telegram erlaubt maximal 4096 Zeichen pro Nachricht. Wir bleiben (in Bytes
// gerechnet) darunter, damit auch Umlaute/Emojis sicher passen.
constexpr std::size_t kTelegramChunkBytes = 4000;

// Wird vom Signal-Handler gesetzt (Ctrl+C / SIGTERM) -> sauberes Beenden.
volatile std::sig_atomic_t g_stopRequested = 0;
void onSignal(int) { g_stopRequested = 1; }

// ------------------------------- Hilfsfunktionen ----------------------------

void logLine(const char* level, const std::string& message) {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
  std::ostream& out = (std::string(level) == "INFO") ? std::cout : std::cerr;
  out << stamp << " [" << level << "] " << message << std::endl;
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string readEnv(const char* name) {
  const char* value = std::getenv(name);
  if (!value) return {};
  std::string s(value);
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

// tgbot-cpp liefert optionale Felder je nach Version als std::string oder
// std::optional<std::string>. Dieser Helfer funktioniert mit beiden.
template <typename T>
std::string textOf(const T& value) {
  if constexpr (std::is_convertible_v<const T&, std::string>) {
    return value;
  } else {
    return value ? std::string(*value) : std::string();
  }
}

// Zerlegt lange Antworten in Telegram-taugliche Stücke. Getrennt wird
// bevorzugt an Zeilenumbrüchen, dann an Leerzeichen – und nie mitten in einem
// UTF-8-Zeichen.
std::vector<std::string> splitForTelegram(const std::string& text,
                                          std::size_t limit = kTelegramChunkBytes) {
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = std::min(pos + limit, text.size());
    if (end < text.size()) {
      std::size_t cut = text.rfind('\n', end);
      if (cut == std::string::npos || cut <= pos + limit / 2) cut = text.rfind(' ', end);
      if (cut != std::string::npos && cut > pos + limit / 2) {
        end = cut;
      } else {
        // Auf Beginn eines UTF-8-Zeichens zurückgehen (Fortsetzungsbytes: 10xxxxxx).
        while (end > pos && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        if (end == pos) end = std::min(pos + limit, text.size());
      }
    }
    std::string part = text.substr(pos, end - pos);
    pos = end;
    while (pos < text.size() && (text[pos] == '\n' || text[pos] == ' ')) ++pos;

    const auto lastNonSpace = part.find_last_not_of(" \n\t\r");
    if (lastNonSpace == std::string::npos) continue;  // nur Leerraum
    part.erase(lastNonSpace + 1);
    parts.push_back(std::move(part));
  }
  return parts;
}

struct ParsedCommand {
  std::string name;    // z. B. "start" (klein geschrieben)
  std::string target;  // Teil nach '@' bei "/start@MeinBot" (klein geschrieben)
};

bool parseCommand(const std::string& text, ParsedCommand& out) {
  if (text.empty() || text[0] != '/') return false;
  const std::size_t tokenEnd = text.find_first_of(" \t\r\n");
  std::string token =
      text.substr(1, tokenEnd == std::string::npos ? std::string::npos : tokenEnd - 1);
  const std::size_t at = token.find('@');
  if (at != std::string::npos) {
    out.target = toLower(token.substr(at + 1));
    token.resize(at);
  }
  out.name = toLower(token);
  return true;
}

// ------------------------------- Bot-Logik ----------------------------------

class BotApp {
 public:
  BotApp(TgBot::Bot& bot, aibot::AiClient& ai, std::string botUsername)
      : bot_(bot), ai_(ai), botUsername_(toLower(std::move(botUsername))) {}

  // Einstiegspunkt für jede eingehende Nachricht. Wirft nie nach außen.
  void onMessage(const TgBot::Message::Ptr& message) noexcept {
    try {
      handle(message);
    } catch (const std::exception& e) {
      logLine("ERROR", std::string("Fehler beim Verarbeiten einer Nachricht: ") + e.what());
    } catch (...) {
      logLine("ERROR", "Unbekannter Fehler beim Verarbeiten einer Nachricht.");
    }
  }

 private:
  void handle(const TgBot::Message::Ptr& message) {
    if (!message || !message->chat) return;

    const std::string text = textOf(message->text);
    if (text.empty()) return;  // Fotos, Sticker, ... werden ignoriert

    const std::int64_t chatId = message->chat->id;
    const std::int64_t userId = message->from ? message->from->id : chatId;

    ParsedCommand command;
    if (parseCommand(text, command)) {
      // "/start@AndererBot" in Gruppen: nicht für uns bestimmt.
      if (!command.target.empty() && command.target != botUsername_) return;
      handleCommand(command, message, chatId, userId);
      return;
    }
    handleChat(text, chatId, userId);
  }

  void handleCommand(const ParsedCommand& command, const TgBot::Message::Ptr& message,
                     std::int64_t chatId, std::int64_t userId) {
    if (command.name == "start") {
      const std::string name = message->from ? message->from->firstName : std::string();
      sendText(chatId, "👋 Hallo" + (name.empty() ? std::string() : " " + name) +
                           "! Ich bin ein KI-Assistent.\n\n"
                           "Schreib mir einfach eine Nachricht, und ich antworte dir mit Hilfe "
                           "von KI. Mit /help siehst du alle Befehle.");
    } else if (command.name == "help") {
      sendText(chatId, "🤖 Hilfe\n\n"
                       "/start – Begrüßung anzeigen\n"
                       "/help – Diese Hilfe anzeigen\n"
                       "/clear – Gesprächsverlauf löschen\n\n"
                       "Jede andere Textnachricht wird an die KI geschickt. Ich merke mir die "
                       "letzten " +
                           std::to_string(ai_.config().maxHistory) +
                           " Nachrichten, damit ich den Zusammenhang behalte.");
    } else if (command.name == "clear") {
      ai_.clearHistory(userId);
      sendText(chatId, "🧹 Gesprächsverlauf gelöscht. Wir fangen von vorn an!");
    } else {
      sendText(chatId, "❓ Unbekannter Befehl. Mit /help siehst du, was ich kann.");
    }
  }

  void handleChat(const std::string& text, std::int64_t chatId, std::int64_t userId) {
    logLine("INFO", "Nachricht von user=" + std::to_string(userId) +
                        " chat=" + std::to_string(chatId) +
                        " (" + std::to_string(text.size()) + " Bytes)");
    sendTyping(chatId);

    std::string reply;
    try {
      reply = ai_.sendMessage(userId, text);
    } catch (const aibot::AiError& e) {
      // Technische Details ins Log, freundliche Meldung an den Nutzer.
      logLine("WARN", std::string("KI-Fehler (HTTP ") + std::to_string(e.httpStatus()) +
                          "): " + e.what());
      sendText(chatId, e.userMessage());
      return;
    } catch (const std::exception& e) {
      logLine("ERROR", std::string("Unerwarteter Fehler bei der KI-Anfrage: ") + e.what());
      sendText(chatId, "⚠️ Es ist ein unerwarteter Fehler aufgetreten. Bitte versuche es erneut.");
      return;
    }
    sendText(chatId, reply);
  }

  void sendTyping(std::int64_t chatId) {
    try {
      bot_.getApi().sendChatAction(chatId, "typing");
    } catch (const std::exception&) {
      // "Tippt ..." ist nur Kosmetik – Fehler hier ignorieren wir bewusst.
    }
  }

  void sendText(std::int64_t chatId, const std::string& text) {
    for (const auto& part : splitForTelegram(text)) {
      bot_.getApi().sendMessage(chatId, part);
    }
  }

  TgBot::Bot& bot_;
  aibot::AiClient& ai_;
  std::string botUsername_;
};

// libcurl global initialisieren/aufräumen (RAII).
struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

// Wartet bis zu `seconds` Sekunden, bricht aber bei Stop-Signal sofort ab.
void interruptibleSleep(int seconds) {
  for (int i = 0; i < seconds * 10 && !g_stopRequested; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace

int main() {
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  // --- Konfiguration prüfen (Fehler früh und verständlich melden) ----------
  const std::string telegramToken = readEnv("TELEGRAM_BOT_TOKEN");
  if (telegramToken.empty()) {
    logLine("ERROR",
            "Umgebungsvariable TELEGRAM_BOT_TOKEN ist nicht gesetzt. Token bei @BotFather "
            "erstellen und z. B. mit 'export TELEGRAM_BOT_TOKEN=...' setzen (siehe .env.example).");
    return 1;
  }

  aibot::AiConfig config;
  try {
    config = aibot::AiConfig::fromEnv();
  } catch (const aibot::ConfigError& e) {
    logLine("ERROR", std::string(e.what()) + " (siehe .env.example)");
    return 1;
  }

  CurlGlobal curlGlobal;
  aibot::AiClient ai(config);

  try {
    TgBot::Bot bot(telegramToken);

    // Verbindung/Token testen und Webhook entfernen (sonst funktioniert Long Polling nicht).
    std::string username;
    try {
      username = textOf(bot.getApi().getMe()->username);
      bot.getApi().deleteWebhook();
    } catch (const std::exception& e) {
      logLine("ERROR", std::string("Telegram-Verbindung fehlgeschlagen (Token ungültig oder "
                                   "kein Netzwerk?): ") + e.what());
      return 1;
    }

    BotApp app(bot, ai, username);
    bot.getEvents().onAnyMessage(
        [&app](TgBot::Message::Ptr message) { app.onMessage(message); });

    logLine("INFO", "Bot @" + username + " läuft (Modell: " + config.model + ", Provider: " +
                        (config.provider == aibot::Provider::Anthropic ? "Anthropic" : "OpenAI") +
                        "). Beenden mit Ctrl+C.");

    // --- Long-Polling-Schleife ---------------------------------------------
    // Ein Durchlauf wartet bis zu 10 s auf neue Updates; danach prüfen wir das Stop-Flag.
    TgBot::TgLongPoll longPoll(bot, 100, 10);
    int consecutiveErrors = 0;
    while (!g_stopRequested) {
      try {
        longPoll.start();
        consecutiveErrors = 0;
      } catch (const std::exception& e) {
        // Netzwerkausfall, Telegram-Fehler, Konflikt mit zweiter Instanz usw.
        ++consecutiveErrors;
        const int waitSeconds = std::min(30, 1 << std::min(consecutiveErrors, 5));
        logLine("WARN", std::string("Long Polling Fehler: ") + e.what() + " – neuer Versuch in " +
                            std::to_string(waitSeconds) + " s");
        interruptibleSleep(waitSeconds);
      }
    }
  } catch (const std::exception& e) {
    logLine("ERROR", std::string("Fataler Fehler: ") + e.what());
    return 1;
  }

  logLine("INFO", "Bot wurde sauber beendet.");
  return 0;
}
