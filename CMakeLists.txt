cmake_minimum_required(VERSION 3.16)

project(telegram-ai-bot
  VERSION 1.0.0
  DESCRIPTION "Telegram-Chatbot mit KI-Anbindung (Anthropic Claude / OpenAI-kompatibel)"
  LANGUAGES CXX)

# --- C++-Standard ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build-Typ" FORCE)
endif()

# Neue CMake-Versionen: Boost über BoostConfig.cmake statt des alten FindBoost.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# --- Abhängigkeiten ----------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.9 REQUIRED)
find_package(Boost 1.65 REQUIRED)

# tgbot-cpp: aktuelle Versionen installieren eine TgBotConfig.cmake (-> TgBot::TgBot).
find_package(TgBot QUIET CONFIG)
if(NOT TARGET TgBot::TgBot)
  # Fallback für ältere tgbot-cpp-Installationen ohne CMake-Config.
  find_path(TGBOT_INCLUDE_DIR tgbot/tgbot.h)
  find_library(TGBOT_LIBRARY NAMES TgBot)
  if(NOT TGBOT_INCLUDE_DIR OR NOT TGBOT_LIBRARY)
    message(FATAL_ERROR
      "tgbot-cpp wurde nicht gefunden. Bitte aus https://github.com/reo7sp/tgbot-cpp "
      "bauen und installieren (siehe README.md) oder -DCMAKE_PREFIX_PATH=<Installationspfad> setzen.")
  endif()
  add_library(TgBot::TgBot UNKNOWN IMPORTED)
  set_target_properties(TgBot::TgBot PROPERTIES
    IMPORTED_LOCATION "${TGBOT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${TGBOT_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "Boost::headers;CURL::libcurl;OpenSSL::SSL;OpenSSL::Crypto;Threads::Threads")
endif()

# --- Warnungen ---------------------------------------------------------------
add_library(project_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  target_compile_options(project_warnings INTERFACE /W4)
endif()

# --- KI-Client (Bibliothek) --------------------------------------------------
add_library(ai_client STATIC src/ai_client.cpp)
target_include_directories(ai_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ai_client PUBLIC cxx_std_17)
target_link_libraries(ai_client
  PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    Threads::Threads
    project_warnings)

# --- Bot (ausführbare Datei) -------------------------------------------------
add_executable(telegram-ai-bot src/main.cpp)
target_link_libraries(telegram-ai-bot
  PRIVATE
    ai_client
    TgBot::TgBot
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    project_warnings)

install(TARGETS telegram-ai-bot RUNTIME DESTINATION bin)
