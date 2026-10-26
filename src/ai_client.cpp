// ai_client.cpp
//
// Implementierung des KI-Clients (Anthropic Messages API + OpenAI-kompatibel).

#include "ai_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

namespace aibot {

namespace {

using json = nlohmann::json;

constexpr const char* kDefaultAnthropicModel = "claude-sonnet-5-5";
constexpr const char* kDefaultAnthropicUrl = "https://api.anthropic.com";
constexpr const char* kAnthropicVersion = "2023-06-01";
constexpr const char* kDefaultOpenAiModel = "gpt-4o-mini";
constexpr const char* kDefaultOpenAiUrl = "https://api.openai.com/v1";
constexpr const char* kDefaultSystemPrompt =
    "Du bist ein hilfreicher, freundlicher KI-Assistent in einem Telegram-Chat. "
    "Antworte in der Sprache des Nutzers, klar und möglichst knapp. "
    "Verwende reinen Text ohne Markdown-Formatierung.";

// Längere Wartezeiten (z. B. Retry-After: 60) blockieren den Bot-Thread zu
// lange. Dann geben wir den Fehler lieber direkt an den Nutzer weiter.
constexpr int kMaxRetryWaitSeconds = 10;

// ----------------------------- Hilfsfunktionen -----------------------------

std::string trim(std::string s) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Liest eine Umgebungsvariable (getrimmt). Leerer String, wenn nicht gesetzt.
std::string envOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value ? trim(value) : std::string();
}

std::string envOrDefault(const char* name, const char* fallback) {
  std::string value = envOrEmpty(name);
  return value.empty() ? std::string(fallback) : value;
}

std::string requireEnv(const char* name) {
  std::string value = envOrEmpty(name);
  if (value.empty()) {
    throw ConfigError(std::string("Umgebungsvariable ") + name + " ist nicht gesetzt.");
  }
  return value;
}

int parsePositiveInt(const std::string& value, const char* name) {
  std::size_t pos = 0;
  int number = 0;
  try {
    number = std::stoi(value, &pos);
  } catch (const std::exception&) {
    pos = 0;
  }
  if (pos == 0 || pos != value.size() || number <= 0) {
    throw ConfigError(std::string(name) + " muss eine positive ganze Zahl sein (aktuell: \"" +
                      value + "\").");
  }
  return number;
}

std::string truncate(const std::string& s, std::size_t maxLen) {
  return s.size() <= maxLen ? s : s.substr(0, maxLen) + "...";
}

// Holt die Fehlermeldung aus {"error": {"message": "..."}} (Anthropic & OpenAI).
std::string extractApiMessage(const std::string& body) {
  const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (j.is_object()) {
    const auto errIt = j.find("error");
    if (errIt != j.end()) {
      if (errIt->is_object()) {
        const auto msgIt = errIt->find("message");
        if (msgIt != errIt->end() && msgIt->is_string()) return msgIt->get<std::string>();
      } else if (errIt->is_string()) {
        return errIt->get<std::string>();
      }
    }
  }
  return truncate(body, 300);
}

ErrorKind kindForStatus(long status) {
  if (status == 401 || status == 403) return ErrorKind::Authentication;
  if (status == 429) return ErrorKind::RateLimit;
  if (status == 529 || status == 503) return ErrorKind::Overloaded;  // 529 = Anthropic "overloaded"
  if (status >= 500) return ErrorKind::Server;
  return ErrorKind::BadRequest;
}

// ------------------------------ libcurl-RAII -------------------------------

struct CurlEasy {
  CURL* handle = curl_easy_init();
  CurlEasy() = default;
  CurlEasy(const CurlEasy&) = delete;
  CurlEasy& operator=(const CurlEasy&) = delete;
  ~CurlEasy() {
    if (handle) curl_easy_cleanup(handle);
  }
};

struct CurlSlist {
  curl_slist* list = nullptr;
  CurlSlist() = default;
  CurlSlist(const CurlSlist&) = delete;
  CurlSlist& operator=(const CurlSlist&) = delete;
  ~CurlSlist() { curl_slist_free_all(list); }
  void append(const std::string& line) { list = curl_slist_append(list, line.c_str()); }
};

std::size_t writeCallback(char* data, std::size_t size, std::size_t count, void* userdata) {
  static_cast<std::string*>(userdata)->append(data, size * count);
  return size * count;
}

// Liest den "Retry-After"-Header (in Sekunden) aus der Antwort.
std::size_t headerCallback(char* buffer, std::size_t size, std::size_t count, void* userdata) {
  const std::size_t length = size * count;
  static const std::string kName = "retry-after:";
  if (length > kName.size()) {
    const std::string line(buffer, length);
    if (toLower(line.substr(0, kName.size())) == kName) {
      const long seconds = std::strtol(line.c_str() + kName.size(), nullptr, 10);
      if (seconds > 0 && seconds < 86400) *static_cast<int*>(userdata) = static_cast<int>(seconds);
    }
  }
  return length;
}

}  // namespace

// ---------------------------------------------------------------------------
// AiConfig
// ---------------------------------------------------------------------------

AiConfig AiConfig::fromEnv() {
  AiConfig cfg;

  const std::string provider = toLower(envOrDefault("AI_PROVIDER", "anthropic"));
  if (provider == "anthropic" || provider == "claude") {
    cfg.provider = Provider::Anthropic;
    cfg.apiKey = requireEnv("ANTHROPIC_API_KEY");
    cfg.model = envOrDefault("CLAUDE_MODEL", kDefaultAnthropicModel);
    cfg.baseUrl = envOrDefault("ANTHROPIC_BASE_URL", kDefaultAnthropicUrl);
  } else if (provider == "openai") {
    cfg.provider = Provider::OpenAI;
    cfg.apiKey = requireEnv("OPENAI_API_KEY");
    cfg.model = envOrDefault("OPENAI_MODEL", kDefaultOpenAiModel);
    cfg.baseUrl = envOrDefault("OPENAI_BASE_URL", kDefaultOpenAiUrl);
    // Neuere OpenAI-Modelle verlangen "max_completion_tokens" statt "max_tokens".
    cfg.openaiTokenField = envOrDefault("OPENAI_TOKEN_FIELD", "max_tokens");
  } else {
    throw ConfigError("AI_PROVIDER muss \"anthropic\" oder \"openai\" sein (aktuell: \"" +
                      provider + "\").");
  }

  while (!cfg.baseUrl.empty() && cfg.baseUrl.back() == '/') cfg.baseUrl.pop_back();

  const std::string maxTokens = envOrEmpty("MAX_TOKENS");
  if (!maxTokens.empty()) cfg.maxTokens = parsePositiveInt(maxTokens, "MAX_TOKENS");

  cfg.systemPrompt = envOrDefault("SYSTEM_PROMPT", kDefaultSystemPrompt);
  return cfg;
}

// ---------------------------------------------------------------------------
// AiError
// ---------------------------------------------------------------------------

std::string AiError::userMessage() const {
  switch (kind_) {
    case ErrorKind::Network:
      return "🌐 Ich kann die KI gerade nicht erreichen (Netzwerkproblem). "
             "Bitte versuche es in ein paar Augenblicken noch einmal.";
    case ErrorKind::Authentication:
      return "🔑 Die Anmeldung bei der KI-API ist fehlgeschlagen. "
             "Der API-Key des Bot-Betreibers ist vermutlich ungültig oder abgelaufen.";
    case ErrorKind::RateLimit: {
      std::string msg = "⏳ Es gibt gerade zu viele Anfragen an die KI (Rate Limit).";
      if (retryAfterSeconds_ > 0) {
        msg += " Bitte versuche es in etwa " + std::to_string(retryAfterSeconds_) +
               " Sekunden erneut.";
      } else {
        msg += " Bitte warte einen Moment und versuche es dann erneut.";
      }
      return msg;
    }
    case ErrorKind::Overloaded:
      return "🚧 Die KI ist gerade überlastet. Bitte versuche es gleich noch einmal.";
    case ErrorKind::BadRequest:
      return "⚠️ Die KI-API hat die Anfrage abgelehnt. Falls das häufiger passiert, "
             "setze das Gespräch mit /clear zurück oder informiere den Bot-Betreiber "
             "(mögliche Ursache: Guthaben aufgebraucht oder falscher Modellname).";
    case ErrorKind::Server:
      return "🛠️ Bei der KI-API ist ein interner Fehler aufgetreten. "
             "Bitte versuche es später erneut.";
    case ErrorKind::InvalidResponse:
      return "⚠️ Die Antwort der KI konnte nicht gelesen werden. Bitte versuche es erneut.";
    case ErrorKind::EmptyResponse:
      return "🤔 Die KI hat keine Antwort geliefert. Bitte formuliere deine Frage um "
             "oder versuche es erneut.";
  }
  return "⚠️ Unbekannter Fehler bei der KI-Anfrage.";
}

// ---------------------------------------------------------------------------
// AiClient – öffentliche API
// ---------------------------------------------------------------------------

AiClient::AiClient(AiConfig config) : config_(std::move(config)) {}

std::string AiClient::sendMessage(std::int64_t userId, const std::string& userText) {
  // 1) Nutzernachricht in den Verlauf legen und eine Kopie ziehen. Der Mutex
  //    wird NICHT während des HTTP-Requests gehalten.
  std::vector<ChatMessage> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = histories_[userId];
    history.push_back({"user", userText});
    trimHistory(history, config_.maxHistory);
    snapshot.assign(history.begin(), history.end());
  }

  // 2) KI fragen. Bei einem Fehler Nutzernachricht wieder entfernen.
  std::string reply;
  try {
    reply = completeWithRetry(snapshot);
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histories_.find(userId);
    if (it != histories_.end() && !it->second.empty() && it->second.back().role == "user") {
      it->second.pop_back();
    }
    throw;
  }

  // 3) Antwort speichern.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = histories_[userId];
    history.push_back({"assistant", reply});
    trimHistory(history, config_.maxHistory);
  }
  return reply;
}

void AiClient::clearHistory(std::int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  histories_.erase(userId);
}

std::size_t AiClient::historySize(std::int64_t userId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = histories_.find(userId);
  return it == histories_.end() ? 0 : it->second.size();
}

// ---------------------------------------------------------------------------
// AiClient – intern
// ---------------------------------------------------------------------------

void AiClient::trimHistory(std::deque<ChatMessage>& history, std::size_t maxMessages) {
  while (history.size() > maxMessages) history.pop_front();
  // Die Anthropic-API verlangt, dass das Gespräch mit "user" beginnt.
  while (!history.empty() && history.front().role != "user") history.pop_front();
}

std::string AiClient::completeWithRetry(const std::vector<ChatMessage>& messages) const {
  for (int attempt = 0;; ++attempt) {
    try {
      return config_.provider == Provider::Anthropic ? completeAnthropic(messages)
                                                     : completeOpenAI(messages);
    } catch (const AiError& e) {
      const bool retryable = e.kind() == ErrorKind::Network || e.kind() == ErrorKind::RateLimit ||
                             e.kind() == ErrorKind::Overloaded || e.kind() == ErrorKind::Server;
      if (!retryable || attempt >= config_.maxRetries) throw;

      // Wartezeit: Retry-After des Servers, sonst exponentielles Backoff (2 s, 4 s, ...).
      const int waitSeconds = e.retryAfterSeconds() > 0 ? e.retryAfterSeconds() : 2 << attempt;
      if (waitSeconds > kMaxRetryWaitSeconds) throw;
      std::this_thread::sleep_for(std::chrono::seconds(waitSeconds));
    }
  }
}

std::string AiClient::completeAnthropic(const std::vector<ChatMessage>& messages) const {
  // Request-Body laut Anthropic Messages API:
  //   { "model", "max_tokens", "system", "messages": [{ "role", "content" }] }
  json body = {{"model", config_.model},
               {"max_tokens", config_.maxTokens},
               {"messages", json::array()}};
  if (!config_.systemPrompt.empty()) body["system"] = config_.systemPrompt;
  for (const auto& m : messages) {
    body["messages"].push_back(json{{"role", m.role}, {"content", m.content}});
  }

  // error_handler_t::replace: ungültiges UTF-8 führt nicht zu einer Exception.
  const std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);

  const std::string response = httpPost(config_.baseUrl + "/v1/messages",
                                        {"x-api-key: " + config_.apiKey,
                                         std::string("anthropic-version: ") + kAnthropicVersion,
                                         "content-type: application/json"},
                                        payload);

  // Antwort: { "content": [ { "type": "text", "text": "..." }, ... ], ... }
  std::string text;
  try {
    const json j = json::parse(response);
    const auto it = j.find("content");
    if (it != j.end() && it->is_array()) {
      for (const auto& block : *it) {
        if (block.is_object() && block.value("type", std::string()) == "text") {
          text += block.value("text", std::string());
        }
      }
    }
  } catch (const json::exception& e) {
    throw AiError(ErrorKind::InvalidResponse,
                  std::string("Antwort der Anthropic-API nicht lesbar: ") + e.what());
  }
  if (trim(text).empty()) {
    throw AiError(ErrorKind::EmptyResponse, "Anthropic-API lieferte keinen Text.");
  }
  return text;
}

std::string AiClient::completeOpenAI(const std::vector<ChatMessage>& messages) const {
  // Request-Body laut OpenAI Chat Completions: System-Prompt ist die erste Nachricht.
  json body = {{"model", config_.model}, {"messages", json::array()}};
  body[config_.openaiTokenField] = config_.maxTokens;
  if (!config_.systemPrompt.empty()) {
    body["messages"].push_back(json{{"role", "system"}, {"content", config_.systemPrompt}});
  }
  for (const auto& m : messages) {
    body["messages"].push_back(json{{"role", m.role}, {"content", m.content}});
  }
  const std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);

  const std::string response =
      httpPost(config_.baseUrl + "/chat/completions",
               {"Authorization: Bearer " + config_.apiKey, "content-type: application/json"},
               payload);

  // Antwort: { "choices": [ { "message": { "content": "..." } } ] }
  std::string text;
  try {
    const json j = json::parse(response);
    const auto& choices = j.at("choices");
    if (choices.is_array() && !choices.empty()) {
      const auto& content = choices[0].at("message").at("content");
      if (content.is_string()) text = content.get<std::string>();
    }
  } catch (const json::exception& e) {
    throw AiError(ErrorKind::InvalidResponse,
                  std::string("Antwort der OpenAI-API nicht lesbar: ") + e.what());
  }
  if (trim(text).empty()) {
    throw AiError(ErrorKind::EmptyResponse, "OpenAI-API lieferte keinen Text.");
  }
  return text;
}

std::string AiClient::httpPost(const std::string& url, const std::vector<std::string>& headers,
                               const std::string& body) const {
  CurlEasy curl;
  if (!curl.handle) throw AiError(ErrorKind::Network, "curl_easy_init fehlgeschlagen.");

  CurlSlist headerList;
  for (const auto& h : headers) headerList.append(h);

  std::string response;
  int retryAfter = 0;
  char errorBuffer[CURL_ERROR_SIZE] = {0};

  curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
  curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headerList.list);
  curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, headerCallback);
  curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &retryAfter);
  curl_easy_setopt(curl.handle, CURLOPT_ERRORBUFFER, errorBuffer);
  curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT, config_.timeoutSeconds);
  curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L);  // wichtig in Multithread-Programmen
  curl_easy_setopt(curl.handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl.handle, CURLOPT_USERAGENT, "telegram-ai-bot/1.0");

  const CURLcode rc = curl_easy_perform(curl.handle);
  if (rc != CURLE_OK) {
    const std::string detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc);
    throw AiError(ErrorKind::Network, "HTTP-Request fehlgeschlagen: " + detail);
  }

  long status = 0;
  curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);
  if (status >= 200 && status < 300) return response;

  throw AiError(kindForStatus(status),
                "HTTP " + std::to_string(status) + ": " + extractApiMessage(response), status,
                retryAfter);
}

}  // namespace aibot
