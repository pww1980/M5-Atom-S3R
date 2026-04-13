"""
aligner.py – Zeitstempel-Merge Whisper-Segmente + Pyannote-Turns.
"""

import logging
from collections import defaultdict

logger = logging.getLogger(__name__)


def _format_ts(seconds: float) -> str:
    m = int(seconds) // 60
    s = int(seconds) % 60
    return f"{m:02d}:{s:02d}"


def align(whisper_segments: list[dict], speaker_turns: list[dict]) -> str:
    """
    Weist jedem Whisper-Segment den dominierenden Sprecher zu.
    Rückgabe: Formatiertes Transkript als String.
    """
    result_lines = []
    prev_speaker = None
    merged_text = ""
    merged_start = 0.0

    for seg in whisper_segments:
        seg_start = seg["start"]
        seg_end   = seg["end"]
        seg_text  = seg["text"]

        # Überlapp je Sprecher berechnen
        overlap_per_speaker = defaultdict(float)
        for turn in speaker_turns:
            overlap_start = max(seg_start, turn["start"])
            overlap_end   = min(seg_end,   turn["end"])
            if overlap_end > overlap_start:
                overlap_per_speaker[turn["speaker"]] += overlap_end - overlap_start

        if overlap_per_speaker:
            speaker = max(overlap_per_speaker, key=overlap_per_speaker.get)
        else:
            speaker = "SPRECHER_?"

        # Aufeinanderfolgende Segmente desselben Sprechers zusammenführen
        if speaker == prev_speaker:
            merged_text += " " + seg_text
        else:
            if prev_speaker is not None:
                ts = _format_ts(merged_start)
                result_lines.append(f"[{ts}] {prev_speaker}: {merged_text.strip()}")
            prev_speaker  = speaker
            merged_text   = seg_text
            merged_start  = seg_start

    # Letztes Segment ausgeben
    if prev_speaker is not None:
        ts = _format_ts(merged_start)
        result_lines.append(f"[{ts}] {prev_speaker}: {merged_text.strip()}")

    transcript = "\n".join(result_lines)
    logger.info(f"[ALIGN] {len(result_lines)} Zeilen im Transkript")
    return transcript
