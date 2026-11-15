#!/usr/bin/env python3
"""One-time Spotify PKCE login. Uses Python's standard library only."""

import argparse
import base64
import getpass
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-currently-playing user-modify-playback-state"


def cpp_string(value):
    # Literal UTF-8 works for non-ASCII Wi-Fi names as well as ordinary text.
    return json.dumps(value, ensure_ascii=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent / "SpotifyLyrics" / "credentials.h",
        help="Arduino credentials header to create or replace",
    )
    args = parser.parse_args()
    print("Register this exact redirect URI in your Spotify app settings:")
    print(REDIRECT_URI)
    print("This script opens Spotify login and saves credentials locally.")
    client_id = input("Spotify Client ID: ").strip()
    wifi_ssid = input("2.4 GHz Wi-Fi name: ").strip()
    wifi_password = getpass.getpass("Wi-Fi password (hidden): ")
    contact = input("Your email or project URL (for LRCLIB client identification): ").strip()
    if not client_id or not wifi_ssid or not contact:
        raise ValueError("Client ID, Wi-Fi name and contact are required.")
    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(32)
    outcome = {}

    class Callback(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path != "/callback":
                self.send_error(404)
                return
            query = urllib.parse.parse_qs(url.query)
            received_state = query.get("state", [""])[0]
            if not secrets.compare_digest(received_state, state):
                self.send_error(400, "Login state mismatch. Restart the login script.")
                return
            if "error" in query:
                outcome["error"] = query["error"][0]
            elif query.get("code"):
                outcome["code"] = query["code"][0]
            else:
                self.send_error(400, "Missing authorization code")
                return
            message = b"Spotify login received. Return to Terminal to finish setup."
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(message)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(message)

        def log_message(self, *_):
            # The callback URL contains an authorization code. Keep it out of logs.
            pass

    params = {
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "state": state,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
    }
    authorize_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(params)
    with http.server.HTTPServer(("127.0.0.1", 8888), Callback) as server:
        server.timeout = 1
        print("Opening Spotify. Finish login within three minutes.")
        if not webbrowser.open(authorize_url):
            print("Open this URL in your browser:")
            print(authorize_url)
        deadline = time.monotonic() + 180
        while not outcome and time.monotonic() < deadline:
            server.handle_request()
    if not outcome:
        raise TimeoutError("Login timed out. Run this script again.")
    if outcome.get("error"):
        raise ValueError("Spotify login was not approved: " + outcome["error"])

    data = urllib.parse.urlencode({
        "client_id": client_id,
        "grant_type": "authorization_code",
        "code": outcome["code"],
        "redirect_uri": REDIRECT_URI,
        "code_verifier": verifier,
    }).encode()
    request = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            token = json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(
            f"Spotify token exchange failed (HTTP {error.code}). "
            "Check the Client ID and registered redirect URI, then try again."
        ) from None
    refresh_token = token.get("refresh_token")
    if not refresh_token:
        raise RuntimeError("Spotify returned no refresh token. Restart login.")

    values = {
        "WIFI_SSID": wifi_ssid,
        "WIFI_PASSWORD": wifi_password,
        "SPOTIFY_CLIENT_ID": client_id,
        "SPOTIFY_REFRESH_TOKEN": refresh_token,
        "LRCLIB_USER_AGENT": f"ESP32Lyrics/1.0 ({contact})",
    }
    contents = "#pragma once\n\n// Private credentials generated by spotify_login.py.\n"
    contents += "\n".join(f"constexpr char {name}[] = {cpp_string(value)};" for name, value in values.items())
    contents += "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
        output.write(contents)
    os.chmod(args.output, 0o600)
    print(f"Saved credentials to {args.output}")
    print("Open SpotifyLyrics.ino in Arduino IDE, then upload to ESP32 Dev Module.")
    print("Keep credentials.h private. It contains your Wi-Fi password and refresh token.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, TimeoutError, RuntimeError) as error:
        print(f"Setup stopped: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nLogin cancelled.", file=sys.stderr)
        sys.exit(1)
