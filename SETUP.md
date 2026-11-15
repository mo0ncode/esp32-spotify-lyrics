# Spotify lyrics display

Firmware for the pictured classic ESP32-WROOM-32 development board, a 20x4
HD44780-compatible LCD with PCF8574 I2C backpack, and three powered button modules.

## Wiring

Follow [the wiring diagram](docs/wiring.png). The LCD uses the pictured 8-channel discrete-transistor converter, labelled A0–A7 and B0–B7.

| Device connection | ESP32 / converter connection |
|---|---|
| Converter VCCA | ESP32 3V3 |
| Converter VCCB and LCD VCC | Verified USB-derived 5 V supply |
| Converter GND, LCD GND, all button GND | ESP32 GND |
| ESP32 D21 | Converter A0 |
| Converter B0 | LCD SDA |
| ESP32 D22 | Converter A1 |
| Converter B1 | LCD SCL |
| All button VCC | ESP32 3V3 |
| Previous button OUT | ESP32 D25 |
| Play/pause button OUT | ESP32 D26 |
| Next button OUT | ESP32 D27 |

If the converter numbers its channels from zero, choose any two matching pairs.
The firmware does not depend on which converter channel numbers are used.
This converter operates automatically and has no OE pin. Match the channel numbers on both sides: A0 to B0 and A1 to B1.
Verify VIN is approximately 5 V relative to GND when the ESP32 is powered over USB
before using VIN for LCD/converter power. Disconnect USB while changing wiring.

## Install the Arduino dependencies

1. Install Arduino IDE.
2. Add this Additional Boards Manager URL in Settings/Preferences:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Install **esp32 by Espressif Systems**, version **3.3.7 or a compatible newer
   3.3.x release**. This sketch uses that core's built-in TLS certificate bundle.
4. In Library Manager install **ArduinoJson by Benoit Blanchon**, version **7.x**,
   and **hd44780 by Bill Perry**. Accept required dependencies.
5. Open `SpotifyLyrics.ino`, select **ESP32 Dev Module**, and select the USB port.
   Keep `firmware.cpp`, `firmware.h`, `config.h`, and `credentials.h` in the same
   `SpotifyLyrics` folder. Arduino IDE compiles `firmware.cpp` automatically.

## First check the display and buttons

1. Set `HARDWARE_TEST = true` in `config.h`.
2. Upload. Open Serial Monitor at **115200 baud**.
3. Release all buttons during the first second after a restart. The program
   samples the idle level, allowing either active-HIGH or active-LOW modules.
4. Press each button. Its name should appear on the LCD and Serial Monitor.
5. If the LCD initializes but text is invisible, gently adjust the blue contrast
   trimmer. If initialization fails, run the library example at
   **File > Examples > hd44780 > ioClass > hd44780_I2Cexp > I2CexpDiag**.
   This library detects the backpack address and pin mapping.

## Create the Spotify app and complete login

You need Spotify Premium and a Spotify developer app with Web API enabled.

1. Open https://developer.spotify.com/dashboard and create/select your app.
2. Register this exact redirect URI in the app settings:
   `http://127.0.0.1:8888/callback`
3. Copy the app's **Client ID**. This implementation uses Authorization Code with
   PKCE. The Client Secret is not used.
4. Ensure the Spotify account used for login is allowed in your development app's
   **Users and Access / User Management** settings.
5. Run the included `spotify_login.py` on your computer with Python 3. From the repository root, run:

   ```sh
   python3 spotify_login.py
   ```

   The helper creates the ignored `SpotifyLyrics/credentials.h` locally. For a hardware-only test before login, copy `credentials.example.h` to `credentials.h` first.
6. Enter the Client ID, your 2.4 GHz Wi-Fi name/password, and your email or project
   URL. LRCLIB requires a client contact in its User-Agent header.
7. Complete the login in the browser. The script saves `credentials.h` inside the
   sketch folder. The terminal does not print your token or password.
8. Set `HARDWARE_TEST = false` in `config.h` and upload again.
9. Open Spotify on your phone/computer/speaker and start a song. The ESP32 controls
   that active Spotify player. It also needs access to internet time via NTP for
   HTTPS certificate validation.

After setup, the computer does not need to stay connected. Power the ESP32 by USB.
The ESP32 refreshes access tokens automatically and stores rotated refresh tokens
in its flash preferences. To change Spotify accounts, run the login helper again
and upload the new credentials. Keep `credentials.h` private.

## Display and playback behavior

- Row 1 shows the title with `> ` for playing or `||` for paused.
- Row 2 shows the first artist.
- Rows 3 and 4 show the current lyric line, scrolling when needed.
- Previous, play/pause and next control the active Spotify device.
- Serial Monitor also accepts `b`, `p`, and `n` for those controls.
- Spotify is polled every 15 seconds by default. A local playback timer advances
  lyrics between requests. Commands made on another device are detected on the
  next poll. Successful local controls schedule an earlier check.
- Timed lyrics are fetched from LRCLIB once per new track. Network/server errors
  are retried after a delay. No matching timed lyrics produces an explicit message.
- HTTPS runs in a background task so the button debounce and LCD drawing continue
  during network requests. A queued control can still wait for the current HTTP
  request to complete. Transport-failed skip commands are not retried automatically.
- `429` responses honor Retry-After. Development quota errors pause Spotify calls
  for an hour if the server gives no duration. Actual quota-reset times may differ.
  Change `SPOTIFY_POLL_MS` if your account needs fewer requests.
- `403` usually requires checking Premium status, account access, granted scopes,
  or playback restrictions. `404` on a command usually means no active player.
- A response too large for the configured 48 KiB body limit is rejected.

## Limits and adjustments

The firmware compiles successfully for ESP32 Dev Module with Arduino-ESP32
3.3.12 and the installed ArduinoJson/hd44780 libraries. The user has confirmed
working lyrics on hardware. The latest playback-control fix adds Content-Length: 0
to empty PUT/POST requests and prints each command's HTTP result and Spotify error
message to Serial Monitor. The user has confirmed that playback controls work.
Playback commands now accept all HTTP 2xx success responses, including 200 and
204, and display Playing, Paused, Previous track, or Next track on the LCD.
The builder has confirmed working lyrics, buttons, playback controls, and action messages on the device.
The login helper passed syntax, command-line help, and credential-string checks.

The 2004A LCD has a limited character ROM. This version renders printable ASCII
and replaces unsupported Unicode characters with `?`. Full Chinese, Tamil,
Arabic, and other Unicode scripts need a suitable graphical display/font.

LRCLIB coverage and song-version matching vary. An instrumental track or a record
with only untimed lyrics is reported explicitly. The parser supports ordinary
line-timed `[mm:ss.xx]` LRC, including multiple timestamps on one line. It caps at
300 entries and 240 ASCII characters per entry. LRC metadata offsets and word-level
karaoke timing are not applied. Use `LYRIC_ADJUST_MS` for a manual timing shift.

Network delay and periodic polling mean lyric alignment is approximate. If Spotify
state becomes stale for three polling intervals, the screen reports it instead of
continuing to present lyrics as synchronized.

## Primary documentation

- https://developer.spotify.com/documentation/web-api/tutorials/code-pkce-flow
- https://developer.spotify.com/documentation/web-api/tutorials/refreshing-tokens
- https://developer.spotify.com/documentation/web-api/reference/get-the-users-currently-playing-track
- https://developer.spotify.com/documentation/web-api/concepts/quota-modes
- https://lrclib.net/docs
- https://github.com/duinoWitchery/hd44780
- https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html
