"""
main.py – Einstiegspunkt für den Transkriptions-Server (Schritt 1: WAV speichern).
"""

import asyncio
import logging
import os
import signal
import sys

import config
import http_receiver

# =============================================================================
# Logging
# =============================================================================
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("server.log", encoding="utf-8"),
    ],
)
logger = logging.getLogger(__name__)


# =============================================================================
# Hauptfunktion
# =============================================================================
async def main():
    os.makedirs(config.OUTPUT_DIR, exist_ok=True)
    logger.info(f"[MAIN] Output-Verzeichnis: {os.path.abspath(config.OUTPUT_DIR)}")

    loop = asyncio.get_event_loop()

    # Signal-Handler im asyncio-Kontext registrieren (Linux/macOS only).
    # Dadurch wird shutdown_event.set() sicher aus dem Event-Loop heraus aufgerufen,
    # ohne den httpd.shutdown()-Block zu treffen.
    shutdown_event = asyncio.Event()

    def _shutdown():
        logger.info("[MAIN] Signal empfangen – Server wird beendet")
        shutdown_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, _shutdown)

    # Event-Loop an http_receiver übergeben (für spätere Pipeline-Integration)
    http_receiver._event_loop = loop

    httpd = http_receiver.start_server()
    logger.info("[MAIN] Server läuft. Warte auf Verbindungen ...")

    await shutdown_event.wait()

    # server_close() schließt den Socket sofort.
    # Der Daemon-Thread (serve_forever) endet automatisch beim nächsten
    # select()-Timeout (≤ 0,5 s) – kein blockierendes shutdown() nötig.
    httpd.server_close()
    logger.info("[MAIN] Server beendet")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
