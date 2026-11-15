#include "firmware.h"
#include "config.h"
#include "credentials.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <algorithm>
#include <cstring>
#include <time.h>
#include <utility>
#include <vector>
#include <esp_timer.h>

// Root-certificate bundle supplied by the official Arduino-ESP32 3.3.x core.
// A fresh TLS client is used for each request, with certificate validation on.
extern const uint8_t caBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t caBundleEnd[] asm("_binary_x509_crt_bundle_end");

enum class Command : uint8_t { Previous, Toggle, Next };

struct Lyric {
  uint32_t atMs;
  String text;
};

struct ScreenState {
  String title;
  String artist;
  String lyricMessage = "Start Spotify";
  String notice = "Starting...";
  uint64_t noticeUntil = 0;
  uint64_t sampledAt = 0;
  uint32_t progressMs = 0;
  uint32_t durationMs = 0;
  bool playing = false;
  std::vector<Lyric> lyrics;
};

struct Button {
  uint8_t pin;
  Command command;
  const char *name;
  int idle = LOW;
  int raw = LOW;
  int stable = LOW;
  uint32_t changedAt = 0;
};

struct Response {
  int code = -1;
  String body;
  uint32_t retrySeconds = 0;
};

// Stops a large/unknown-length response from exhausting the WROOM's RAM.
class LimitedBody : public Stream {
public:
  String text;
  static constexpr size_t limit = 49152;
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t *data, size_t length) override {
    if (length > limit - text.length()) return 0;
    return text.concat(reinterpret_cast<const char *>(data), length) ? length : 0;
  }
};

hd44780_I2Cexp lcd; // Detects the I2C address and PCF8574 pin mapping.
ScreenState screen;
SemaphoreHandle_t screenMutex;
QueueHandle_t commands;
Preferences preferences;
Button buttons[] = {
  {PREVIOUS_PIN, Command::Previous, "Previous"},
  {PLAY_PAUSE_PIN, Command::Toggle, "Play/pause"},
  {NEXT_PIN, Command::Next, "Next"},
};
String lastRows[4];
String accessToken;
String refreshToken;
String trackId;
String trackTitle;
String trackArtist;
String trackAlbum;
uint32_t trackDuration = 0;
uint64_t accessExpiresAt = 0;
uint64_t spotifyBlockedUntil = 0;
uint64_t lyricsBlockedUntil = 0;
uint64_t nextPollAt = 0;
uint64_t nextLyricsAt = 0;
bool lyricsPending = false;
bool loginInvalid = false;
bool workerPlaying = false;

uint64_t nowMs() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

void notice(const String &message, uint32_t duration = 3000) {
  xSemaphoreTake(screenMutex, portMAX_DELAY);
  screen.notice = message;
  screen.noticeUntil = nowMs() + duration;
  xSemaphoreGive(screenMutex);
  Serial.println(message);
}

String urlEncode(const String &input) {
  const char *hex = "0123456789ABCDEF";
  String output;
  output.reserve(input.length() * 2);
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(input[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      output += static_cast<char>(c);
    } else {
      output += '%';
      output += hex[c >> 4];
      output += hex[c & 15];
    }
  }
  return output;
}

// A character LCD has no Unicode font. Keep printable ASCII; represent each
// unsupported UTF-8 character once instead of sending garbled bytes to the LCD.
String lcdText(const String &input) {
  String output;
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(input[i]);
    if (c >= 32 && c <= 126) output += static_cast<char>(c);
    else if (c < 128) output += ' ';
    else if ((c & 0xC0) != 0x80) output += '?';
  }
  return output;
}

String windowOf(String text, size_t width, uint64_t step) {
  if (text.length() <= width) {
    while (text.length() < width) text += ' ';
    return text;
  }
  text += "   ";
  size_t offset = step % text.length();
  String doubled = text + text;
  return doubled.substring(offset, offset + width);
}

Response request(const char *method, const String &url,
                 const String &body = "", bool authorized = false,
                 const char *contentType = nullptr) {
  Response result;
  WiFiClientSecure tls;
  tls.setCACertBundle(caBundleStart, caBundleEnd - caBundleStart);
  tls.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(tls, url)) return result;
  const char *headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("User-Agent", LRCLIB_USER_AGENT);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  if (authorized) http.addHeader("Authorization", "Bearer " + accessToken);
  if (contentType) http.addHeader("Content-Type", contentType);
  // HTTPClient omits Content-Length for empty payloads. Spotify's player
  // endpoints can reject bodyless PUT/POST requests without an explicit zero.
  if (body.isEmpty() && (strcmp(method, "PUT") == 0 || strcmp(method, "POST") == 0)) {
    http.addHeader("Content-Length", "0");
  }
  result.code = http.sendRequest(method, body);
  if (result.code > 0) {
    long wait = http.header("Retry-After").toInt();
    result.retrySeconds = wait > 0 ? static_cast<uint32_t>(wait) : 0;
    if (result.code != 204) {
      LimitedBody payload;
      int received = http.writeToStream(&payload);
      if (received < 0) result.code = -2;
      else result.body = std::move(payload.text);
    }
  }
  http.end();
  return result;
}

uint32_t retryDelay(const Response &response, uint32_t fallbackSeconds) {
  // Never retry earlier than Retry-After. Clamp only against millis overflow.
  uint64_t seconds = response.retrySeconds ? response.retrySeconds : fallbackSeconds;
  return static_cast<uint32_t>(std::min<uint64_t>(seconds * 1000, 4000000000ULL));
}

bool ensureToken() {
  if (loginInvalid || nowMs() < spotifyBlockedUntil) return false;
  if (accessToken.length() && nowMs() < accessExpiresAt) return true;
  String body = "grant_type=refresh_token&client_id=" + urlEncode(SPOTIFY_CLIENT_ID) +
                "&refresh_token=" + urlEncode(refreshToken);
  Response response = request("POST", "https://accounts.spotify.com/api/token", body,
                              false, "application/x-www-form-urlencoded");
  if (response.code == 429) {
    spotifyBlockedUntil = nowMs() + retryDelay(response, 60);
    notice("Spotify rate limit");
    return false;
  }
  if (response.code != 200) {
    loginInvalid = response.code == 400 || response.code == 401;
    notice(loginInvalid ? "Run login again" : "Token network error", 10000);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response.body) || !doc["access_token"].is<const char *>()) {
    notice("Token parse error");
    return false;
  }
  accessToken = doc["access_token"].as<String>();
  uint32_t seconds = doc["expires_in"] | 3600U;
  accessExpiresAt = nowMs() + static_cast<uint64_t>(seconds > 60 ? seconds - 60 : seconds) * 1000;
  String replacement = doc["refresh_token"] | "";
  if (replacement.length() && replacement != refreshToken) {
    refreshToken = replacement;
    preferences.putString("refresh", refreshToken);
  }
  return true;
}

Response spotify(const char *method, const String &path) {
  Response response;
  if (!ensureToken()) return response;
  response = request(method, "https://api.spotify.com/v1" + path, "", true);
  // A rejected token did not execute the operation, so retry it once with a new token.
  if (response.code == 401) {
    accessToken = "";
    if (ensureToken()) response = request(method, "https://api.spotify.com/v1" + path, "", true);
  }
  if (response.code == 429) {
    bool quota = response.body.indexOf("QUOTA_EXCEEDED") >= 0;
    spotifyBlockedUntil = nowMs() + retryDelay(response, quota ? 3600 : 60);
    notice(quota ? "Spotify quota limit" : "Spotify rate limit", 10000);
  }
  return response;
}

std::vector<Lyric> parseLyrics(const String &lrc) {
  std::vector<Lyric> result;
  int start = 0;
  while (start < static_cast<int>(lrc.length()) && result.size() < 300) {
    int end = lrc.indexOf('\n', start);
    if (end < 0) end = lrc.length();
    String line = lrc.substring(start, end);
    start = end + 1;
    std::vector<uint32_t> times;
    int pos = 0;
    while (pos < static_cast<int>(line.length()) && line[pos] == '[') {
      int close = line.indexOf(']', pos);
      if (close < 0) break;
      String stamp = line.substring(pos + 1, close);
      int minutes = 0;
      float seconds = 0;
      if (sscanf(stamp.c_str(), "%d:%f", &minutes, &seconds) == 2 &&
          minutes >= 0 && seconds >= 0 && seconds < 60) {
        times.push_back(static_cast<uint32_t>(minutes * 60000 + seconds * 1000 + 0.5f));
      }
      pos = close + 1;
    }
    String text = line.substring(pos);
    text.trim();
    text = lcdText(text);
    if (text.length() > 240) text = text.substring(0, 240);
    for (uint32_t stamp : times) {
      if (result.size() == 300) break;
      result.push_back({stamp, text});
    }
  }
  std::stable_sort(result.begin(), result.end(), [](const Lyric &a, const Lyric &b) {
    return a.atMs < b.atMs;
  });
  return result;
}

void fetchLyrics() {
  if (!lyricsPending || nowMs() < nextLyricsAt || nowMs() < lyricsBlockedUntil) return;
  String url = "https://lrclib.net/api/get?track_name=" + urlEncode(trackTitle) +
               "&artist_name=" + urlEncode(trackArtist) + "&album_name=" + urlEncode(trackAlbum) +
               "&duration=" + String((trackDuration + 500) / 1000);
  Response response = request("GET", url);
  if (response.code == 429) {
    uint32_t wait = retryDelay(response, 60);
    lyricsBlockedUntil = nowMs() + wait + (esp_random() % 1000);
    return;
  }
  if (response.code < 0 || response.code >= 500) {
    nextLyricsAt = nowMs() + 60000;
    return;
  }
  lyricsPending = false;
  std::vector<Lyric> parsed;
  String message = "Lyrics unavailable";
  if (response.code == 200) {
    JsonDocument filter;
    filter["syncedLyrics"] = true;
    filter["instrumental"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, response.body, DeserializationOption::Filter(filter))) {
      if (doc["instrumental"] | false) message = "Instrumental";
      else {
        parsed = parseLyrics(doc["syncedLyrics"] | "");
        message = parsed.empty() ? "No timed lyrics" : "Waiting for lyrics";
      }
    }
  }
  xSemaphoreTake(screenMutex, portMAX_DELAY);
  screen.lyrics = std::move(parsed);
  screen.lyricMessage = message;
  xSemaphoreGive(screenMutex);
}

bool pollPlayback() {
  Response response = spotify("GET", "/me/player/currently-playing");
  if (response.code == 204) {
    trackId = "";
    lyricsPending = false;
    workerPlaying = false;
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    screen.title = "";
    screen.artist = "";
    screen.playing = false;
    screen.lyrics.clear();
    screen.lyricMessage = "Open Spotify & play";
    xSemaphoreGive(screenMutex);
    return true;
  }
  if (response.code != 200) {
    if (response.code == 403) notice("Check Premium/access", 8000);
    else if (response.code != 429 && !loginInvalid && nowMs() >= spotifyBlockedUntil)
      notice("Spotify unavailable");
    return false;
  }
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["currently_playing_type"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["name"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, response.body, DeserializationOption::Filter(filter))) {
    notice("Playback parse error");
    return false;
  }
  String kind = doc["currently_playing_type"] | "unknown";
  if (kind != "track" || doc["item"].isNull() || doc["progress_ms"].isNull()) {
    trackId = "";
    lyricsPending = false;
    workerPlaying = doc["is_playing"] | false;
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    screen.title = "Spotify";
    screen.artist = "";
    screen.playing = workerPlaying;
    screen.sampledAt = nowMs();
    screen.lyrics.clear();
    screen.lyricMessage = "No track lyrics";
    xSemaphoreGive(screenMutex);
    return true;
  }
  String id = doc["item"]["id"] | "";
  String title = doc["item"]["name"] | "Unknown track";
  String artist = doc["item"]["artists"][0]["name"] | "Unknown artist";
  // A local track may have no Spotify ID.
  if (!id.length()) id = title + "|" + artist;
  bool changed = id != trackId;
  workerPlaying = doc["is_playing"] | false;
  trackId = id;
  trackTitle = title;
  trackArtist = artist;
  trackAlbum = doc["item"]["album"]["name"] | "";
  trackDuration = doc["item"]["duration_ms"] | 0U;
  xSemaphoreTake(screenMutex, portMAX_DELAY);
  screen.title = lcdText(title);
  screen.artist = lcdText(artist);
  screen.progressMs = doc["progress_ms"] | 0U;
  screen.durationMs = trackDuration;
  screen.sampledAt = nowMs();
  screen.playing = workerPlaying;
  if (changed) {
    screen.lyrics.clear();
    screen.lyricMessage = "Loading lyrics...";
  }
  xSemaphoreGive(screenMutex);
  if (changed) {
    lyricsPending = true;
    nextLyricsAt = nowMs();
  }
  return true;
}

void sendControl(Command command) {
  if (nowMs() < spotifyBlockedUntil) {
    notice("Spotify limit: wait");
    return;
  }
  // Get the actual play state before choosing play vs pause.
  if (command == Command::Toggle && !pollPlayback()) return;
  const char *method = command == Command::Toggle ? "PUT" : "POST";
  String path;
  if (command == Command::Previous) path = "/me/player/previous";
  else if (command == Command::Next) path = "/me/player/next";
  else path = workerPlaying ? "/me/player/pause" : "/me/player/play";
  Response response = spotify(method, path);
  Serial.printf("Spotify control %s %s -> HTTP %d\n", method, path.c_str(), response.code);
  const bool succeeded = response.code >= 200 && response.code < 300;
  if (response.code > 0 && !succeeded && !response.body.isEmpty()) {
    JsonDocument errorDoc;
    if (!deserializeJson(errorDoc, response.body)) {
      // Print only API error fields, never Authorization headers or tokens.
      if (errorDoc["error"].is<JsonObject>()) {
        String message = errorDoc["error"]["message"] | "";
        String reason = errorDoc["error"]["reason"] | "";
        Serial.printf("Spotify message: %s\nSpotify reason: %s\n",
                      message.c_str(), reason.c_str());
      } else if (errorDoc["error"].is<const char *>()) {
        Serial.printf("Spotify error: %s\n", errorDoc["error"].as<const char *>());
      }
    }
  }
  if (succeeded) {
    if (command == Command::Toggle) {
      workerPlaying = !workerPlaying;
      xSemaphoreTake(screenMutex, portMAX_DELAY);
      uint64_t elapsed = nowMs() - screen.sampledAt;
      if (screen.playing) screen.progressMs += static_cast<uint32_t>(elapsed);
      screen.sampledAt = nowMs();
      screen.playing = workerPlaying;
      xSemaphoreGive(screenMutex);
    }
    const char *action = "Next track";
    if (command == Command::Previous) action = "Previous track";
    if (command == Command::Toggle) action = workerPlaying ? "Playing" : "Paused";
    notice(action, 1000);
    nextPollAt = nowMs() + 800;
  } else if (response.code == 404) notice("Open Spotify & play");
  else if (response.code == 403) notice("Control HTTP 403");
  else if (response.code > 0 && response.code != 429)
    notice("Control HTTP " + String(response.code));
  else if (response.code < 0 && !loginInvalid && nowMs() >= spotifyBlockedUntil)
    notice("Control network err");
  // Transport failures are not retried: a skip may already have reached Spotify.
}

void networkTask(void *) {
  preferences.begin("spotlyrics", false);
  String seed = SPOTIFY_REFRESH_TOKEN;
  if (preferences.getString("seed") != seed) {
    preferences.putString("seed", seed);
    preferences.putString("refresh", seed);
  }
  refreshToken = preferences.getString("refresh", seed);
  if (String(WIFI_SSID).startsWith("YOUR_") || seed.startsWith("YOUR_")) {
    notice("Set credentials.h", 86400000);
    vTaskDelete(nullptr);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  notice("Connecting Wi-Fi...", 20000);
  bool clockStarted = false;
  uint64_t lastReconnect = nowMs();
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (nowMs() - lastReconnect >= 15000) {
        WiFi.reconnect();
        lastReconnect = nowMs();
        notice("Check Wi-Fi", 5000);
      }
      // Do not execute stale button presses after Wi-Fi comes back.
      xQueueReset(commands);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!clockStarted) {
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      clockStarted = true;
      notice("Setting clock...", 5000);
    }
    if (time(nullptr) < 1700000000) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (loginInvalid) {
      xQueueReset(commands);
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    Command command;
    if (xQueueReceive(commands, &command, 0) == pdTRUE) sendControl(command);
    if (nowMs() >= nextPollAt && nowMs() >= spotifyBlockedUntil) {
      // Schedule from request completion, including failures, to avoid tight retries.
      pollPlayback();
      nextPollAt = nowMs() + SPOTIFY_POLL_MS;
    }
    fetchLyrics();
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void handlePress(Command command, const char *name) {
  Serial.printf("Button: %s\n", name);
  if (HARDWARE_TEST) notice(String(name) + " pressed", 2000);
  else if (xQueueSend(commands, &command, 0) != pdTRUE) notice("Please wait...");
}

void readButtons() {
  for (Button &button : buttons) {
    int raw = digitalRead(button.pin);
    if (raw != button.raw) {
      button.raw = raw;
      button.changedAt = millis();
    }
    if (raw != button.stable && millis() - button.changedAt >= BUTTON_DEBOUNCE_MS) {
      button.stable = raw;
      if (raw != button.idle) handlePress(button.command, button.name);
    }
  }
}

void drawScreen() {
  static uint64_t lastDraw = 0;
  static uint64_t lyricStartedAt = 0;
  static String previousLyric;
  if (nowMs() - lastDraw < 100) return;
  lastDraw = nowMs();
  uint64_t step = nowMs() / SCROLL_STEP_MS;
  String rows[4];
  xSemaphoreTake(screenMutex, portMAX_DELAY);
  rows[0] = screen.title.length()
      ? String(screen.playing ? "> " : "||") + windowOf(screen.title, 18, step)
      : "Spotify lyrics";
  rows[1] = windowOf(screen.artist, 20, step);
  String lyric = screen.lyricMessage;
  if (!screen.lyrics.empty()) {
    uint64_t elapsed = nowMs() - screen.sampledAt;
    uint64_t position = screen.progressMs + (screen.playing ? elapsed : 0);
    if (screen.durationMs && position > screen.durationMs) position = screen.durationMs;
    int64_t adjusted = static_cast<int64_t>(position) + LYRIC_ADJUST_MS;
    for (const Lyric &line : screen.lyrics) {
      if (static_cast<int64_t>(line.atMs) > adjusted) break;
      lyric = line.text;
    }
    if (elapsed > static_cast<uint64_t>(SPOTIFY_POLL_MS) * 3) lyric = "Playback sync stale";
  }
  if (nowMs() < screen.noticeUntil) lyric = screen.notice;
  if (lyric != previousLyric) {
    previousLyric = lyric;
    lyricStartedAt = nowMs();
  }
  String block = windowOf(lyric, 40, (nowMs() - lyricStartedAt) / SCROLL_STEP_MS);
  rows[2] = block.substring(0, 20);
  rows[3] = block.substring(20, 40);
  xSemaphoreGive(screenMutex);
  for (uint8_t row = 0; row < 4; ++row) {
    rows[row] = windowOf(rows[row], 20, 0);
    if (rows[row] != lastRows[row]) {
      lcd.setCursor(0, row);
      lcd.print(rows[row]);
      lastRows[row] = rows[row];
    }
  }
}

void appSetup() {
  Serial.begin(115200);
  screenMutex = xSemaphoreCreateMutex();
  commands = xQueueCreate(4, sizeof(Command));
  if (!screenMutex || !commands) {
    Serial.println("Cannot allocate task resources");
    while (true) delay(1000);
  }
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  int status = lcd.begin(20, 4);
  if (status != 0) {
    Serial.printf("LCD initialization failed (%d). Run hd44780 I2CexpDiag.\n", status);
    while (true) delay(1000);
  }
  lcd.backlight();
  lcd.print("Release all buttons");
  for (Button &button : buttons) pinMode(button.pin, INPUT);
  delay(1000);
  for (Button &button : buttons) {
    button.idle = digitalRead(button.pin);
    button.raw = button.idle;
    button.stable = button.idle;
    button.changedAt = millis();
    Serial.printf("%s: idle=%d, pressed=%d\n", button.name, button.idle, !button.idle);
  }
  lcd.clear();
  if (HARDWARE_TEST) {
    notice("Press each button", 86400000);
    screen.artist = "Hardware test";
    screen.lyricMessage = "Press each button";
  } else if (xTaskCreate(networkTask, "spotify", 16384, nullptr, 1, nullptr) != pdPASS) {
    notice("Network task failed", 86400000);
  }
}

void appLoop() {
  readButtons();
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'b') handlePress(Command::Previous, "Previous");
    if (c == 'p') handlePress(Command::Toggle, "Play/pause");
    if (c == 'n') handlePress(Command::Next, "Next");
  }
  drawScreen();
  delay(5);
}
