"""
main.py – Einstiegspunkt für den Transkriptions-Server.
"""

import asyncio
import logging
import os
import signal
import sys

import config
import audio_receiver
import pipeline
import transcriber

# =============================================================================
# Logging konfigurieren
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

# Unterdrücke harmlose "connection closed before HTTP request" Traces vom
# websockets-Library – passiert wenn der ESP32 intern eine TCP-Verbindung
# abbricht und sofort neu verbindet (normales Retry-Verhalten).
class _WsHandshakeFilter(logging.Filter):
    _SUPPRESSED = (
        "did not receive a valid HTTP request",
        "connection closed while reading HTTP request line",
    )
    def filter(self, record: logging.LogRecord) -> bool:
        msg = record.getMessage()
        return not any(s in msg for s in self._SUPPRESSED)

logging.getLogger("websockets.server").addFilter(_WsHandshakeFilter())
logging.getLogger("websockets.asyncio.server").addFilter(_WsHandshakeFilter())


# =============================================================================
# Graceful Shutdown
# =============================================================================
shutdown_event = asyncio.Event()


def _handle_signal(signum, frame):
    logger.info(f"[MAIN] Signal {signum} empfangen – Server wird beendet")
    shutdown_event.set()


# =============================================================================
# Hauptfunktion
# =============================================================================
async def main():
    # Output-Verzeichnis erstellen
    os.makedirs(config.OUTPUT_DIR, exist_ok=True)
    logger.info(f"[MAIN] Output-Verzeichnis: {os.path.abspath(config.OUTPUT_DIR)}")

    # Whisper-Modell vorladen (verhindert Verzögerung beim ersten Aufruf)
    logger.info("[MAIN] Lade Whisper-Modell vor ...")
    loop = asyncio.get_event_loop()
    await loop.run_in_executor(None, transcriber.load_model)
    logger.info("[MAIN] Whisper-Modell bereit")

    # Pipeline-Queue erstellen und an audio_receiver übergeben
    queue: asyncio.Queue = asyncio.Queue()
    audio_receiver.pipeline_queue = queue

    # Pipeline-Worker als asyncio-Task starten
    worker_task = asyncio.create_task(pipeline.pipeline_worker(queue))

    # WebSocket-Server starten (läuft als weiterer Task)
    server_task = asyncio.create_task(audio_receiver.start_server())

    logger.info("[MAIN] Server läuft. Warte auf Verbindungen ...")

    # Warten bis Shutdown-Signal
    await shutdown_event.wait()

    logger.info("[MAIN] Beende Server ...")
    worker_task.cancel()
    server_task.cancel()

    try:
        await worker_task
    except asyncio.CancelledError:
        pass
    try:
        await server_task
    except asyncio.CancelledError:
        pass

    logger.info("[MAIN] Server beendet")


if __name__ == "__main__":
    signal.signal(signal.SIGINT,  _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
