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
# Graceful Shutdown
# =============================================================================
shutdown_event = asyncio.Event()


def _handle_signal(signum, frame):
    logger.info(f"[MAIN] Signal {signum} – Server wird beendet")
    shutdown_event.set()


# =============================================================================
# Hauptfunktion
# =============================================================================
async def main():
    os.makedirs(config.OUTPUT_DIR, exist_ok=True)
    logger.info(f"[MAIN] Output-Verzeichnis: {os.path.abspath(config.OUTPUT_DIR)}")

    # Event-Loop an http_receiver übergeben (für spätere Pipeline-Integration)
    http_receiver._event_loop = asyncio.get_event_loop()

    httpd = http_receiver.start_server()
    logger.info("[MAIN] Server läuft. Warte auf Verbindungen ...")

    await shutdown_event.wait()

    httpd.shutdown()
    logger.info("[MAIN] Server beendet")


if __name__ == "__main__":
    signal.signal(signal.SIGINT,  _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
