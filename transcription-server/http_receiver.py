"""
http_receiver.py – HTTP-Server, empfängt PCM-Segmente vom Gerät, schreibt WAV.

Protokoll (POST /upload):
  Header X-Session-Id  – eindeutige Session-ID vom Gerät
  Header X-Seq-Num     – Segmentnummer (0, 1, 2, …)
  Header X-Final       – "1" beim letzten Segment, sonst "0"
  Body                 – rohes PCM (16 kHz, 16 bit, mono)

Bei X-Final: 1 werden alle Segmente der Session in der richtigen Reihenfolge
zusammengefügt, ein WAV-Header vorangestellt und die Datei gespeichert.
"""

import asyncio
import logging
import os
import struct
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import config

logger = logging.getLogger(__name__)

# Wird von main.py gesetzt, sobald der Event-Loop läuft
pipeline_queue: asyncio.Queue = None
_event_loop: asyncio.AbstractEventLoop = None

# In-Memory-Speicher: {session_id: {seq_num: pcm_bytes}}
_sessions: dict = {}
_lock = threading.Lock()


# =============================================================================
# WAV-Header
# =============================================================================
def _build_wav_header(pcm_len: int) -> bytes:
    num_channels    = config.AUDIO_CHANNELS
    sample_rate     = config.AUDIO_SAMPLE_RATE
    bits_per_sample = config.AUDIO_BIT_DEPTH
    byte_rate       = sample_rate * num_channels * bits_per_sample // 8
    block_align     = num_channels * bits_per_sample // 8
    chunk_size      = 36 + pcm_len
    return struct.pack(
        '<4sI4s4sIHHIIHH4sI',
        b'RIFF', chunk_size, b'WAVE', b'fmt ',
        16, 1, num_channels, sample_rate, byte_rate, block_align, bits_per_sample,
        b'data', pcm_len,
    )


# =============================================================================
# HTTP-Handler
# =============================================================================
class _UploadHandler(BaseHTTPRequestHandler):

    def do_GET(self):
        if self.path != '/hello':
            self.send_error(404, 'Not Found')
            return
        device_ip = self.headers.get('X-Device-IP', self.client_address[0])
        logger.info(f"[HTTP] Gerät meldet sich: {device_ip}")
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(b'OK')

    def do_POST(self):
        if self.path != '/upload':
            self.send_error(404, 'Not Found')
            return

        session_id     = self.headers.get('X-Session-Id', 'unknown')
        seq_num        = int(self.headers.get('X-Seq-Num', '0'))
        is_final       = self.headers.get('X-Final', '0') == '1'
        content_length = int(self.headers.get('Content-Length', '0'))

        pcm = self.rfile.read(content_length)

        with _lock:
            if session_id not in _sessions:
                _sessions[session_id] = {}
            _sessions[session_id][seq_num] = pcm

        logger.info(
            f"[HTTP] {session_id} Seg {seq_num}: "
            f"{len(pcm):,} Bytes{'  [FINAL]' if is_final else ''}"
        )

        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(b'ACK')

        if is_final:
            threading.Thread(
                target=_finalize_session,
                args=(session_id,),
                daemon=True,
            ).start()

    # Standard-Zugriffslog unterdrücken – eigenes Logging in do_POST
    def log_message(self, fmt, *args):
        pass


# =============================================================================
# Finalisierung
# =============================================================================
def _finalize_session(session_id: str):
    with _lock:
        segments = _sessions.pop(session_id, {})

    if not segments:
        logger.warning(f"[HTTP] {session_id}: keine Segmente gespeichert")
        return

    all_pcm = b''.join(segments[k] for k in sorted(segments))

    wav_path = os.path.join(tempfile.gettempdir(), f"atom_{session_id}.wav")
    with open(wav_path, 'wb') as f:
        f.write(_build_wav_header(len(all_pcm)))
        f.write(all_pcm)

    duration_s = len(all_pcm) / (
        config.AUDIO_SAMPLE_RATE * config.AUDIO_CHANNELS * config.AUDIO_BIT_DEPTH // 8
    )
    logger.info(
        f"[HTTP] {session_id}: WAV gespeichert → {wav_path} "
        f"({duration_s:.1f}s, {len(segments)} Segment(e))"
    )

    if pipeline_queue is not None and _event_loop is not None:
        asyncio.run_coroutine_threadsafe(pipeline_queue.put(wav_path), _event_loop)


# =============================================================================
# Server starten (in eigenem Thread)
# =============================================================================
def start_server() -> HTTPServer:
    host = config.SERVER_HOST
    port = config.SERVER_PORT
    httpd = HTTPServer((host, port), _UploadHandler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    logger.info(f"[HTTP] Server läuft auf {host}:{port}")
    return httpd
