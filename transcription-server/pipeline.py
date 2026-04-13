"""
pipeline.py – Orchestriert Whisper → Pyannote → Align → LLM → Telegram.
"""

import asyncio
import logging
import os
import subprocess
import time

import transcriber
import diarizer
import aligner
import summarizer
import notifier
import config

logger = logging.getLogger(__name__)


def _session_name_from_path(wav_path: str) -> str:
    return os.path.basename(wav_path).replace("_audio.wav", "")


def _write_transcript_md(session_name: str, transcript: str,
                          detected_language: str, num_speakers: int) -> str:
    path = os.path.join(config.OUTPUT_DIR, f"{session_name}_transcript.md")

    # Sprecher aus dem Transkript extrahieren
    speakers = sorted({
        line.split("]")[1].split(":")[0].strip()
        for line in transcript.splitlines()
        if "]" in line and ":" in line
    })
    speaker_list = ", ".join(speakers) if speakers else "unbekannt"

    lang_display = detected_language.capitalize() if detected_language else "unbekannt"

    content = f"""# Transkript \u2013 {session_name}

- **Sprache:** {lang_display}
- **Sprecher:** {num_speakers} erkannt ({speaker_list})

## Verlauf

"""
    for line in transcript.splitlines():
        if line.strip():
            # [MM:SS] SPRECHER_X: Text → **[MM:SS] SPRECHER_X:** Text
            if "]" in line and ":" in line:
                ts_end = line.index("]") + 1
                rest   = line[ts_end:].strip()
                colon  = rest.index(":") + 1
                speaker_part = rest[:colon]
                text_part    = rest[colon:].strip()
                ts_part = line[:ts_end]
                content += f"**{ts_part} {speaker_part}** {text_part}\n\n"
            else:
                content += line + "\n\n"

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)

    logger.info(f"[PIPELINE] Transkript geschrieben: {path}")
    return path


def _write_summary_md(session_name: str, summary: str,
                       detected_language: str, num_speakers: int) -> str:
    path = os.path.join(config.OUTPUT_DIR, f"{session_name}_summary.md")
    lang_display = detected_language.capitalize() if detected_language else "unbekannt"

    content = f"""# Zusammenfassung \u2013 {session_name}

- **Sprache:** {lang_display}
- **Sprecher:** {num_speakers} erkannt

{summary}
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)

    logger.info(f"[PIPELINE] Zusammenfassung geschrieben: {path}")
    return path


def _convert_to_ogg(wav_path: str) -> str:
    """
    Konvertiert WAV → OGG/Opus mit ffmpeg.
    Löscht WAV nur wenn OGG > 0 Bytes.
    """
    ogg_path = wav_path.replace("_audio.wav", "_audio.ogg")
    cmd = [
        "ffmpeg", "-y", "-i", wav_path,
        "-c:a", "libopus", "-b:a", "32k",
        ogg_path
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=120)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.decode(errors="replace"))

        ogg_size = os.path.getsize(ogg_path) if os.path.exists(ogg_path) else 0
        if ogg_size == 0:
            raise RuntimeError("OGG-Datei ist leer")

        os.remove(wav_path)
        logger.info(f"[PIPELINE] OGG erstellt: {ogg_path} ({ogg_size} Bytes), WAV gelöscht")
        return ogg_path
    except Exception as e:
        logger.error(f"[PIPELINE] OGG-Konvertierung fehlgeschlagen: {e}")
        return wav_path  # WAV bleibt erhalten


def run_pipeline(wav_path: str):
    """
    Führt die vollständige Pipeline synchron aus.
    Wird vom asyncio-Worker in einem Thread-Executor aufgerufen.
    """
    session_name = _session_name_from_path(wav_path)
    start_time = time.time()
    logger.info(f"[PIPELINE] === Start: {session_name} ===")

    error_log_path = os.path.join(config.OUTPUT_DIR, "error.log")
    pipeline_error = None

    whisper_segments  = []
    detected_language = "unbekannt"
    speaker_turns     = []
    merged_transcript = ""
    summary           = ""

    # 1. Transkription
    try:
        whisper_segments, detected_language = transcriber.transcribe(wav_path)
    except Exception as e:
        msg = f"[{session_name}] Whisper-Fehler: {e}"
        logger.error(msg)
        with open(error_log_path, "a") as ef:
            ef.write(msg + "\n")
        pipeline_error = str(e)

    # 2. Diarization
    try:
        speaker_turns = diarizer.diarize(wav_path)
    except Exception as e:
        msg = f"[{session_name}] Pyannote-Fehler: {e}"
        logger.error(msg)
        with open(error_log_path, "a") as ef:
            ef.write(msg + "\n")
        if not pipeline_error:
            pipeline_error = str(e)

    # 3. Alignment
    try:
        merged_transcript = aligner.align(whisper_segments, speaker_turns)
    except Exception as e:
        msg = f"[{session_name}] Aligner-Fehler: {e}"
        logger.error(msg)
        with open(error_log_path, "a") as ef:
            ef.write(msg + "\n")
        if not pipeline_error:
            pipeline_error = str(e)

    # 4. Transkript-MD schreiben
    num_speakers = len({t["speaker"] for t in speaker_turns}) if speaker_turns else 0
    try:
        _write_transcript_md(session_name, merged_transcript, detected_language, num_speakers)
    except Exception as e:
        logger.error(f"[PIPELINE] Transkript-MD Fehler: {e}")

    # 5. Zusammenfassung
    try:
        summary = summarizer.summarize(merged_transcript)
    except Exception as e:
        msg = f"[{session_name}] Ollama-Fehler: {e}"
        logger.error(msg)
        with open(error_log_path, "a") as ef:
            ef.write(msg + "\n")
        if not pipeline_error:
            pipeline_error = str(e)

    # 6. Summary-MD schreiben
    try:
        _write_summary_md(session_name, summary, detected_language, num_speakers)
    except Exception as e:
        logger.error(f"[PIPELINE] Summary-MD Fehler: {e}")

    # 7+8. WAV → OGG
    try:
        _convert_to_ogg(wav_path)
    except Exception as e:
        logger.error(f"[PIPELINE] OGG-Fehler: {e}")

    # 9. Telegram
    notifier.send(session_name, error=pipeline_error)

    # 10. Laufzeit-Log
    elapsed = time.time() - start_time
    wav_size = os.path.getsize(wav_path) if os.path.exists(wav_path) else 0
    logger.info(
        f"[PIPELINE] === Ende: {session_name} | "
        f"Laufzeit: {elapsed:.1f}s | "
        f"Sprache: {detected_language} | "
        f"Sprecher: {num_speakers} | "
        f"Audio: {wav_size} Bytes ==="
    )


async def pipeline_worker(queue: asyncio.Queue):
    """
    Asyncio-Task, der Jobs aus der Queue sequenziell abarbeitet.
    Führt run_pipeline in einem Thread-Executor aus (blockiert nicht den Event-Loop).
    """
    loop = asyncio.get_event_loop()
    while True:
        wav_path = await queue.get()
        logger.info(f"[WORKER] Verarbeite: {wav_path}")
        try:
            await loop.run_in_executor(None, run_pipeline, wav_path)
        except Exception as e:
            logger.error(f"[WORKER] Unerwarteter Fehler: {e}")
        finally:
            queue.task_done()
