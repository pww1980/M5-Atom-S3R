"""
diarizer.py – pyannote.audio Wrapper.
Pipeline wird einmalig geladen.
"""

import logging
from pyannote.audio import Pipeline
import config

logger = logging.getLogger(__name__)

_pipeline = None


def load_pipeline():
    global _pipeline
    if _pipeline is None:
        logger.info("[PYANNOTE] Lade Diarization-Pipeline ...")
        _pipeline = Pipeline.from_pretrained(
            "pyannote/speaker-diarization-3.1",
            token=config.PYANNOTE_TOKEN,   # use_auth_token entfernt in pyannote ≥ 3.x
        )
        logger.info("[PYANNOTE] Pipeline geladen")


def diarize(wav_path: str) -> list[dict]:
    """
    Führt Sprecherdiarisierung durch.
    Rückgabe: [{start, end, speaker}, ...]
    """
    load_pipeline()
    logger.info(f"[PYANNOTE] Diarisiere: {wav_path}")

    kwargs = {"min_speakers": config.PYANNOTE_MIN_SPEAKERS}
    if config.PYANNOTE_MAX_SPEAKERS is not None:
        kwargs["max_speakers"] = config.PYANNOTE_MAX_SPEAKERS

    diarization = _pipeline(wav_path, **kwargs)

    turns = []
    for turn, _, speaker in diarization.itertracks(yield_label=True):
        turns.append({
            "start":   turn.start,
            "end":     turn.end,
            "speaker": speaker,  # z.B. "SPEAKER_00"
        })

    # Speaker-IDs normalisieren: SPEAKER_00 → SPEAKER_A etc.
    unique_speakers = sorted({t["speaker"] for t in turns})
    labels = [chr(ord('A') + i) for i in range(len(unique_speakers))]
    speaker_map = {s: f"SPRECHER_{labels[i]}" for i, s in enumerate(unique_speakers)}

    for t in turns:
        t["speaker"] = speaker_map[t["speaker"]]

    logger.info(f"[PYANNOTE] {len(unique_speakers)} Sprecher erkannt: {list(speaker_map.values())}")
    return turns
