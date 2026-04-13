#!/usr/bin/env python3
"""
check_server.py – Prüft ob alle Komponenten des Transkriptions-Servers
bereit sind. Kann jederzeit ausgeführt werden, auch während der Server läuft.

Verwendung:
    python check_server.py
    python check_server.py --host 192.168.1.100  # anderen Host prüfen
"""

import argparse
import asyncio
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

import requests
import websockets

import config

# =============================================================================
# Ausgabe-Helpers
# =============================================================================
GREEN  = "\033[92m"
RED    = "\033[91m"
YELLOW = "\033[93m"
RESET  = "\033[0m"
BOLD   = "\033[1m"

def ok(msg):    print(f"  {GREEN}✓{RESET}  {msg}")
def fail(msg):  print(f"  {RED}✗{RESET}  {msg}")
def warn(msg):  print(f"  {YELLOW}!{RESET}  {msg}")
def header(msg): print(f"\n{BOLD}{msg}{RESET}")

# =============================================================================
# Einzelne Checks
# =============================================================================
results: list[tuple[str, bool]] = []

def check(name: str, passed: bool, detail: str = ""):
    results.append((name, passed))
    if passed:
        ok(f"{name}" + (f"  ({detail})" if detail else ""))
    else:
        fail(f"{name}" + (f"  → {detail}" if detail else ""))
    return passed


# --- 1. TCP-Erreichbarkeit ---------------------------------------------------
def check_tcp(host: str, port: int) -> bool:
    header("1  WebSocket-Server")
    try:
        s = socket.create_connection((host, port), timeout=3)
        s.close()
        return check("TCP-Verbindung", True, f"{host}:{port}")
    except OSError as e:
        return check("TCP-Verbindung", False, str(e))


# --- 2. WebSocket-Handshake + ACK -------------------------------------------
async def _ws_roundtrip(host: str, port: int) -> tuple[bool, bool]:
    """Verbindet per WebSocket, sendet PCM-Stille + DONE-Frame, erwartet ACK.

    Das Gerät sendet DONE (kein WebSocket-Close-Frame) – erst danach schickt
    der Server ACK und schließt die Verbindung.  Der Close-Frame-Pfad sendet
    bewusst kein ACK (Verbindung ist bereits halb geschlossen).
    """
    uri = f"ws://{host}:{port}"
    hs_ok  = False
    ack_ok = False
    try:
        async with websockets.connect(uri, open_timeout=5) as ws:
            hs_ok = True
            # 512 Samples × 2 Bytes Stille = 1 KB (entspricht einem echten Chunk)
            silence = b"\x00" * 1024
            await ws.send(silence)
            # DONE senden → Server schreibt WAV, sendet ACK, schließt dann selbst
            await ws.send("DONE")
            # ACK mit kurzem Timeout abwarten
            try:
                async with asyncio.timeout(8):
                    async for msg in ws:
                        if isinstance(msg, str) and msg.startswith("ACK"):
                            ack_ok = True
                            break
            except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
                pass
    except Exception as e:
        return hs_ok, ack_ok
    return hs_ok, ack_ok

def check_websocket(host: str, port: int) -> bool:
    hs_ok, ack_ok = asyncio.run(_ws_roundtrip(host, port))
    check("WebSocket-Handshake",   hs_ok,  "Verbindung aufgebaut"  if hs_ok  else "Handshake fehlgeschlagen")
    check("Server-ACK nach DONE",  ack_ok, "ACK empfangen"         if ack_ok else "kein ACK innerhalb 8 s")
    return hs_ok and ack_ok


# --- 3. Ollama ---------------------------------------------------------------
def check_ollama() -> bool:
    header("2  Ollama")
    base = config.OLLAMA_URL.rsplit("/api/", 1)[0]
    try:
        r = requests.get(base, timeout=5)
        running = check("Ollama läuft", r.status_code == 200, f"HTTP {r.status_code}")
    except requests.exceptions.ConnectionError as e:
        return check("Ollama läuft", False, "nicht erreichbar – läuft Ollama?")

    if not running:
        return False

    # Modell vorhanden?
    try:
        r = requests.get(f"{base}/api/tags", timeout=5)
        models = [m["name"] for m in r.json().get("models", [])]
        model_name = config.OLLAMA_MODEL
        found = any(model_name in m for m in models)
        check(
            f"Modell '{model_name}' vorhanden",
            found,
            ", ".join(models) if models else "keine Modelle gefunden",
        )
        return running and found
    except Exception as e:
        warn(f"Modell-Prüfung fehlgeschlagen: {e}")
        return running


# --- 4. ffmpeg ---------------------------------------------------------------
def check_ffmpeg() -> bool:
    header("3  ffmpeg")
    path = shutil.which("ffmpeg")
    if not path:
        return check("ffmpeg installiert", False, "nicht im PATH – sudo apt install ffmpeg")
    try:
        r = subprocess.run(["ffmpeg", "-version"], capture_output=True, timeout=5)
        ver = r.stdout.decode().splitlines()[0] if r.stdout else "?"
        return check("ffmpeg installiert", True, ver)
    except Exception as e:
        return check("ffmpeg installiert", False, str(e))


# --- 5. Konfiguration --------------------------------------------------------
def check_config() -> bool:
    header("4  Konfiguration")
    all_ok = True
    placeholders = {"hf_...", "...", ""}

    def cfg_check(label, value):
        nonlocal all_ok
        ok_flag = value not in placeholders
        check(label, ok_flag, value[:12] + "…" if ok_flag else "Platzhalter – bitte eintragen")
        all_ok = all_ok and ok_flag

    cfg_check("PYANNOTE_TOKEN",      config.PYANNOTE_TOKEN)
    cfg_check("TELEGRAM_BOT_TOKEN",  config.TELEGRAM_BOT_TOKEN)
    cfg_check("TELEGRAM_CHAT_ID",    config.TELEGRAM_CHAT_ID)
    return all_ok


# --- 6. Output-Verzeichnis ---------------------------------------------------
def check_output_dir() -> bool:
    header("5  Output-Verzeichnis")
    d = config.OUTPUT_DIR
    exists = os.path.isdir(d)
    if not exists:
        try:
            os.makedirs(d)
            exists = True
        except OSError:
            pass
    if not check("Verzeichnis vorhanden", exists, os.path.abspath(d)):
        return False

    testfile = os.path.join(d, ".write_test")
    try:
        with open(testfile, "w") as f:
            f.write("test")
        os.remove(testfile)
        return check("Schreibzugriff", True)
    except OSError as e:
        return check("Schreibzugriff", False, str(e))


# --- 7. Python-Pakete --------------------------------------------------------
def check_packages() -> bool:
    header("6  Python-Pakete")
    packages = {
        "faster_whisper": "faster-whisper",
        "pyannote.audio": "pyannote.audio",
        "websockets":     "websockets",
        "pydub":          "pydub",
        "requests":       "requests",
        "torch":          "torch",
    }
    all_ok = True
    for module, pip_name in packages.items():
        try:
            __import__(module)
            check(pip_name, True)
        except ImportError:
            check(pip_name, False, f"pip install {pip_name}")
            all_ok = False
    return all_ok


# =============================================================================
# Zusammenfassung
# =============================================================================
def print_summary():
    passed = sum(1 for _, r in results if r)
    total  = len(results)
    print(f"\n{'─' * 45}")
    print(f"{BOLD}Ergebnis: {passed}/{total} Checks bestanden{RESET}")
    failed = [name for name, r in results if not r]
    if failed:
        print(f"{RED}Fehlgeschlagen:{RESET}")
        for name in failed:
            print(f"  • {name}")
        print()
        return False
    print(f"{GREEN}Alle Checks bestanden – Server ist bereit.{RESET}\n")
    return True


# =============================================================================
# Main
# =============================================================================
def main():
    parser = argparse.ArgumentParser(description="Atom Transcription Server – Health Check")
    parser.add_argument("--host", default=config.WEBSOCKET_HOST.replace("0.0.0.0", "127.0.0.1"),
                        help="WebSocket-Host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=config.WEBSOCKET_PORT,
                        help=f"WebSocket-Port (default: {config.WEBSOCKET_PORT})")
    parser.add_argument("--skip-ws", action="store_true",
                        help="WebSocket-Test überspringen (wenn Server nicht läuft)")
    args = parser.parse_args()

    print(f"{BOLD}=== Atom Transcription Server – Health Check ==={RESET}")
    print(f"Host: {args.host}:{args.port}  |  {time.strftime('%Y-%m-%d %H:%M:%S')}")

    check_packages()
    check_config()
    check_output_dir()
    check_ffmpeg()
    check_ollama()

    if not args.skip_ws:
        tcp_ok = check_tcp(args.host, args.port)
        if tcp_ok:
            check_websocket(args.host, args.port)
        else:
            warn("WebSocket-Test übersprungen (TCP nicht erreichbar)")
            warn("Server starten mit: python main.py")
            results.append(("WebSocket-Handshake",   False))
            results.append(("Server-ACK nach Close",  False))
    else:
        warn("WebSocket-Test übersprungen (--skip-ws)")

    success = print_summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
