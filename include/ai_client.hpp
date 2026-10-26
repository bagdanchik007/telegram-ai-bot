// ai_client.hpp
//
// Schlanker KI-Client für den Telegram-Bot.
//  * Spricht die Anthropic Messages API an (Standard)
//  * oder eine OpenAI-kompatible Chat-Completions-API (AI_PROVIDER=openai)
//  * verwaltet den Gesprächsverlauf pro Telegram-User (letzte N Nachrichten)
//
// HTTP: libcurl, JSON: nlohmann/json (beides nur in ai_client.cpp benutzt).

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace aibot {

// ---------------------------------------------------------------------------
// Konfiguration
// ---------------------------------------------------------------------------

enum class Provider { Anthropic, OpenAI };

// Wird geworfen, wenn die Konfiguration (Umgebungsvariablen) unvollständig ist.
class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct AiConfig {
  Provider provider = Provider::Anthropic;
  std::string apiKey;
  std::string model;
  std::string baseUrl;       // ohne abschließenden Slash
  std::string systemPrompt;
  std::string openaiTokenField = "max_tokens";  // nur OpenAI-Modus (s. fromEnv)
  int maxTokens = 1024;
  std::size_t maxHistory = 10;  // Anzahl gespeicherter Nachrichten pro User
  long timeoutSeconds = 60;     // Gesamt-Timeout pro HTTP-Request
  int maxRetries = 2;           // Wiederholungen bei 429/529/5xx/Netzwerkfehlern

  // Liest alle Werte aus Umgebungsvariablen. Wirft ConfigError bei Fehlern.
  static AiConfig fromEnv();
};

// ---------------------------------------------------------------------------
// Fehler
// ---------------------------------------------------------------------------

enum class ErrorKind {
  Network,          // DNS, Verbindung, Timeout ...
  Authentication,   // 401 / 403 – falscher oder fehlender API-Key
  RateLimit,        // 429
  Overloaded,       // 529 (Anthropic) bzw. 503 – Dienst überlastet
  BadRequest,       // 4xx – Anfrage abgelehnt (z. B. leeres Guthaben)
  Server,           // 5xx
  InvalidResponse,  // Antwort nicht lesbar / unerwartetes Format
  EmptyResponse     // KI hat keinen Text geliefert
};

class AiError : public std::runtime_error {
 public:
  AiError(ErrorKind kind, const std::string& technicalMessage, long httpStatus = 0,
          int retryAfterSeconds = 0)
      : std::runtime_error(technicalMessage),
        kind_(kind),
        httpStatus_(httpStatus),
        retryAfterSeconds_(retryAfterSeconds) {}

  ErrorKind kind() const noexcept { return kind_; }
  long httpStatus() const noexcept { return httpStatus_; }
  int retryAfterSeconds() const noexcept { return retryAfterSeconds_; }

  // Freundliche, deutschsprachige Fehlermeldung für den Telegram-Chat.
  // (Technische Details stehen in what() und gehören nur ins Log.)
  std::string userMessage() const;

 private:
  ErrorKind kind_;
  long httpStatus_;
  int retryAfterSeconds_;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

class AiClient {
 public:
  explicit AiClient(AiConfig config);

  // Hängt die Nutzernachricht an den Verlauf des Users an, fragt die KI und
  // speichert die Antwort. Bei einem Fehler wird die Nutzernachricht wieder
  // aus dem Verlauf entfernt (kein "halber" Verlauf) und AiError geworfen.
  std::string sendMessage(std::int64_t userId, const std::string& userText);

  // Löscht den Verlauf eines Users.
  void clearHistory(std::int64_t userId);

  // Anzahl der aktuell gespeicherten Nachrichten eines Users.
  std::size_t historySize(std::int64_t userId) const;

  const AiConfig& config() const noexcept { return config_; }

 private:
  struct ChatMessage {
    std::string role;  // "user" oder "assistant"
    std::string content;
  };

  std::string completeWithRetry(const std::vector<ChatMessage>& messages) const;
  std::string completeAnthropic(const std::vector<ChatMessage>& messages) const;
  std::string completeOpenAI(const std::vector<ChatMessage>& messages) const;

  // Führt einen HTTP-POST aus und liefert den Body bei 2xx zurück.
  // Wirft AiError bei Netzwerk- und HTTP-Fehlern.
  std::string httpPost(const std::string& url, const std::vector<std::string>& headers,
                       const std::string& body) const;

  // Begrenzt den Verlauf auf maxMessages und stellt sicher, dass er mit einer
  // User-Nachricht beginnt (Anthropic verlangt das).
  static void trimHistory(std::deque<ChatMessage>& history, std::size_t maxMessages);

  AiConfig config_;
  mutable std::mutex mutex_;
  std::map<std::int64_t, std::deque<ChatMessage>> histories_;
};

}  // namespace aibot
