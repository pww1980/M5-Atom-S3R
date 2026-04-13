"""
audio_receiver.py – WebSocket-Server, empfängt PCM-Audio, schreibt WAV.
"""

import asyncio
import logging
import struct
import os
from datetime import datetime

import websockets

import config

logger = logging.getLogger(__name__)

pipeline_queue: asyncio.Queue = None  # wird von main.py gesetzt


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
    session_name = datetime.now().strftime("%Y_%m_%d_%H_%M")
    logger.info(f"[SESSION] Neue Verbindung: {session_name}")

    pcm_data = bytearray()
    clean_close = False

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                pcm_data.extend(message)
            # Textframes ignorieren
    except websockets.exceptions.ConnectionClosedOK:
        clean_close = True
    except websockets.exceptions.ConnectionClosedError:
        clean_close = False
    except Exception as e:
        logger.error(f"[SESSION] Fehler: {e}")
        clean_close = False

    if not clean_close or len(pcm_data) == 0:
        logger.warning(f"[SESSION] {session_name} abgebrochen oder leer – verworfen")
        return

    # WAV schreiben
    os.makedirs(config.OUTPUT_DIR, exist_ok=True)
    wav_path = os.path.join(config.OUTPUT_DIR, f"{session_name}_audio.wav")

    with open(wav_path, 'wb') as f:
        f.write(_build_wav_header(len(pcm_data)))
        f.write(pcm_data)

    logger.info(f"[SESSION] WAV geschrieben: {wav_path} ({len(pcm_data)} Bytes PCM)")

    # ACK zurück an Device
    try:
        await websocket.send("ACK")
    except Exception:
        pass

    # In Pipeline-Queue einreihen
    await pipeline_queue.put(wav_path)
    logger.info(f"[SESSION] In Queue eingereiht: {wav_path}")


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
