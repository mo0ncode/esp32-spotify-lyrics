#pragma once
#include <stdint.h>

// First upload with true to check the LCD and all three buttons without Wi-Fi.
// Release the buttons during the first second after every reset.
constexpr bool HARDWARE_TEST = false;

constexpr uint8_t LCD_SDA = 21;
constexpr uint8_t LCD_SCL = 22;
constexpr uint8_t PREVIOUS_PIN = 25;
constexpr uint8_t PLAY_PAUSE_PIN = 26;
constexpr uint8_t NEXT_PIN = 27;

// Timed lyrics advance locally between Spotify checks. Raise this value if
// your developer account quota requires fewer calls. External changes appear
// on the next check; successful local controls trigger an earlier check.
constexpr uint32_t SPOTIFY_POLL_MS = 15000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
constexpr uint32_t SCROLL_STEP_MS = 450;
constexpr int32_t LYRIC_ADJUST_MS = 0; // Positive values show lyrics earlier.
