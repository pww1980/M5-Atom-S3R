"""
audio_receiver.py – WebSocket-Server, empfängt PCM-Audio, schreibt WAV.

Ende-Erkennung (in Priorität):
  1. "DONE"-Textframe vom Gerät  → ACK senden, Verbindung schließen
  2. Sauberer Close-Frame        → kein ACK möglich, Daten verarbeiten
  3. Idle-Timeout (5s kein Chunk) → Verbindung schließen, Daten verarbeiten
  4. Unerwarteter Abbruch        → Puffer verwerfen
"""

import asyncio
import logging
import struct
import os
import tempfile
from datetime import datetime

import websockets

import config

logger = logging.getLogger(__name__)

pipeline_queue: asyncio.Queue = None  # wird von main.py gesetzt

IDLE_TIMEOUT = 5.0   # Sekunden ohne Daten → Aufnahme gilt als beendet


def _build_wav_header(pcm_len: int) -> bytes:
    num_channels   = config.AUDIO_CHANNELS
    sample_rate    = config.AUDIO_SAMPLE_RATE
    bits_per_sample = config.AUDIO_BIT_DEPTH
    byte_rate      = sample_rate * num_channels * bits_per_sample // 8
    block_align    = num_channels * bits_per_sample // 8
    chunk_size     = 36 + pcm_len

    return struct.pack(
        '<4sI4s4sIHHIIHH4sI',
        b'RIFF',
        chunk_size,
        b'WAVE',
        b'fmt ',
        16,               # Subchunk1Size
        1,                # AudioFormat PCM
        num_channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
        b'data',
        pcm_len,
    )


async def handle_connection(websocket):
    session_name = datetime.now().strftime("%Y_%m_%d_%H_%M_%S")
    logger.info(f"[SESSION] Neue Verbindung: {session_name}")

    pcm_data   = bytearray()
    done_frame = False    # True = "DONE"-Frame, Verbindung noch offen → ACK möglich
    end_reason = "?"

    # -----------------------------------------------------------------------
    # Empfangsschleife mit Idle-Timeout
    # asyncio.wait_for() auf jeden einzelnen recv()-Aufruf:
    # → wenn 5s kein Chunk kommt, gilt Aufnahme als beendet
    # → robuster als "DONE"-Frame alleine (Timing-Probleme auf ESP32-Seite)
    # -----------------------------------------------------------------------
    try:
        while True:
            try:
                message = await asyncio.wait_for(
                    websocket.recv(), timeout=IDLE_TIMEOUT
                )
            except asyncio.TimeoutError:
                end_reason = f"Idle-Timeout ({IDLE_TIMEOUT:.0f}s)"
                logger.info(f"[SESSION] {session_name}: {end_reason} – Aufnahme beendet")
                break

            if isinstance(message, bytes):
                pcm_data.extend(message)

            elif isinstance(message, str) and message.strip() == "DONE":
                done_frame = True
                end_reason = "DONE-Frame"
                logger.info(f"[SESSION] {session_name}: DONE empfangen")
                break

            # Andere Textframes ignorieren

    except websockets.exceptions.ConnectionClosedOK:
        end_reason = "Close-Frame"
        logger.info(f"[SESSION] {session_name}: sauberer Close-Frame (kein ACK möglich)")

    except websockets.exceptions.ConnectionClosedError:
        logger.warning(f"[SESSION] {session_name}: unerwarteter Abbruch – verworfen")
        return

    except Exception as e:
        logger.error(f"[SESSION] {session_name}: Fehler: {e}")
        return

    # -----------------------------------------------------------------------
    # Leere Session verwerfen
    # -----------------------------------------------------------------------
    if len(pcm_data) == 0:
        logger.warning(f"[SESSION] {session_name}: kein Audio empfangen – verworfen")
        try:
            await websocket.close()
        except Exception:
            pass
        return

    # -----------------------------------------------------------------------
    # WAV in /tmp/ schreiben
    # -----------------------------------------------------------------------
    wav_path = os.path.join(tempfile.gettempdir(), f"atom_{session_name}.wav")

    with open(wav_path, 'wb') as f:
        f.write(_build_wav_header(len(pcm_data)))
        f.write(pcm_data)

    duration_s = len(pcm_data) / (config.AUDIO_SAMPLE_RATE
                                   * config.AUDIO_CHANNELS
                                   * config.AUDIO_BIT_DEPTH // 8)
    logger.info(
        f"[SESSION] {session_name}: WAV → {wav_path} "
        f"({len(pcm_data)} Bytes, {duration_s:.1f}s, Ende: {end_reason})"
    )

    # -----------------------------------------------------------------------
    # ACK – nur wenn Verbindung noch offen (DONE-Pfad oder nach Idle-Timeout)
    # -----------------------------------------------------------------------
    if done_frame or end_reason.startswith("Idle"):
        try:
            await websocket.send("ACK")
            await websocket.close()
        except Exception as e:
            logger.debug(f"[SESSION] ACK nicht gesendet: {e}")

    # -----------------------------------------------------------------------
    # In Pipeline-Queue einreihen
    # -----------------------------------------------------------------------
    await pipeline_queue.put(wav_path)
    logger.info(f"[SESSION] {session_name}: in Queue eingereiht")


async def start_server():
    logger.info(f"[WS] Server startet auf {config.WEBSOCKET_HOST}:{config.WEBSOCKET_PORT}")
    async with websockets.serve(
        handle_connection,
        config.WEBSOCKET_HOST,
        config.WEBSOCKET_PORT,
        max_size=None,          # kein Limit für Nachrichtengröße
        ping_interval=None,     # kein automatisches Ping
    ):
        await asyncio.Future()  # läuft ewig
