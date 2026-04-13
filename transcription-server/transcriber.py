"""
transcriber.py – faster-whisper Wrapper.
Modell wird einmalig geladen und im Speicher gehalten.
"""

import logging
from faster_whisper import WhisperModel
import config

logger = logging.getLogger(__name__)

_model: WhisperModel = None


def load_model():
    global _model
    if _model is None:
        logger.info(f"[WHISPER] Lade Modell '{config.WHISPER_MODEL}' ...")
        _model = WhisperModel(
            config.WHISPER_MODEL,
            device="cpu",
            compute_type=config.WHISPER_COMPUTE_TYPE,
        )
        logger.info("[WHISPER] Modell geladen")


def transcribe(wav_path: str) -> tuple[list[dict], str]:
    """
    Transkribiert die WAV-Datei.
    Rückgabe: (segments, detected_language)
    segments: [{start, end, text}, ...]
    """
    load_model()
    logger.info(f"[WHISPER] Transkribiere: {wav_path}")

    segments_iter, info = _model.transcribe(
        wav_path,
        beam_size=5,
        vad_filter=True,
        # kein language-Parameter → automatische Erkennung
    )

    detected_language = info.language
    logger.info(f"[WHISPER] Erkannte Sprache: {detected_language} "
                f"(Wahrscheinlichkeit: {info.language_probability:.2f})")

    segments = []
    for seg in segments_iter:
        segments.append({
            "start": seg.start,
            "end":   seg.end,
            "text":  seg.text.strip(),
        })

    logger.info(f"[WHISPER] {len(segments)} Segmente erkannt")
    return segments, detected_language
