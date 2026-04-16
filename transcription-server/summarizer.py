"""
summarizer.py – Ollama API Wrapper für strukturierte LLM-Zusammenfassung.

Unterstützt Thinking-Modelle (qwen3, deepseek-r1 etc.):
  - Setzt "think": false im Request (Ollama ≥ 0.7)
  - Strippt <think>...</think> als Fallback
  - Robust JSON-Extraktion mit drei Fallback-Stufen
"""

import json
import logging
import re
import requests
import config

logger = logging.getLogger(__name__)

PROMPT_TEMPLATE = """\
Du bist ein präziser Assistent für Gesprächsanalyse.
Antworte NUR mit einem JSON-Objekt – kein Markdown, keine Erklärungen, kein Text davor oder danach.

Analysiere das folgende Transkript:

{{
  "zusammenfassung": "Kurze Zusammenfassung des Gesprächs (2-4 Sätze)",
  "themen":          ["Hauptthema 1", "Hauptthema 2"],
  "action_items":    ["Aufgabe 1 (Zuständig: X)", "Aufgabe 2"],
  "stichworte":      ["Stichwort1", "Stichwort2"],
  "stimmung":        "positiv|neutral|negativ|gemischt",
  "sprache":         "Deutsch"
}}

Transkript:
{transcript}
"""


# =============================================================================
# Hilfsfunktionen
# =============================================================================

def _strip_thinking(text: str) -> str:
    """Entfernt <think>…</think>-Blöcke (Thinking-Modelle wie qwen3, deepseek-r1)."""
    return re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL).strip()


def _extract_json(text: str) -> dict:
    """
    Versucht JSON aus dem Text zu extrahieren – drei Stufen:
      1. Direktes json.loads()
      2. JSON-Block aus Markdown-Fence (```json … ```)
      3. Erstes { bis letztes } im Text
    """
    # Stufe 1: direkt
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass

    # Stufe 2: Markdown-Fence
    match = re.search(r'```(?:json)?\s*([\s\S]*?)\s*```', text)
    if match:
        try:
            return json.loads(match.group(1))
        except json.JSONDecodeError:
            pass

    # Stufe 3: erstes { … letztes }
    start = text.find('{')
    end   = text.rfind('}')
    if start != -1 and end > start:
        try:
            return json.loads(text[start:end + 1])
        except json.JSONDecodeError:
            logger.warning("[Ollama] JSON-Parse fehlgeschlagen (alle Stufen erschöpft)")

    return {}


def _format_markdown(data: dict) -> str:
    """Wandelt das geparste JSON in formatiertes Markdown um."""
    parts = []

    if s := data.get("zusammenfassung"):
        parts.append(f"## Zusammenfassung\n\n{s}")

    if themen := data.get("themen"):
        parts.append("## Themen\n\n" + "\n".join(f"- {t}" for t in themen))

    if items := data.get("action_items"):
        parts.append("## Action Items\n\n" + "\n".join(f"- [ ] {i}" for i in items))

    if sw := data.get("stichworte"):
        parts.append("## Stichworte\n\n" + "  ".join(f"`{s}`" for s in sw))

    meta = []
    if stimmung := data.get("stimmung"):
        meta.append(f"**Stimmung:** {stimmung}")
    if sprache := data.get("sprache"):
        meta.append(f"**Sprache:** {sprache}")
    if meta:
        parts.append("## Metadaten\n\n" + "  |  ".join(meta))

    return "\n\n".join(parts)


# =============================================================================
# Hauptfunktion
# =============================================================================

def summarize(transcript: str) -> str:
    """
    Sendet das Transkript an Ollama und gibt formatiertes Markdown zurück.
    Bei leerem Transkript wird ein Hinweis zurückgegeben.
    """
    if not transcript.strip():
        return "_Kein Transkriptinhalt vorhanden._"

    prompt = PROMPT_TEMPLATE.format(transcript=transcript)
    payload = {
        "model":  config.OLLAMA_MODEL,
        "prompt": prompt,
        "stream": False,
        "think":  False,   # Thinking-Modus deaktivieren (Ollama ≥ 0.7, qwen3 etc.)
    }

    logger.info(f"[Ollama] Sende Anfrage an Modell '{config.OLLAMA_MODEL}'...")
    response = requests.post(config.OLLAMA_URL, json=payload, timeout=300)
    response.raise_for_status()

    raw = response.json().get("response", "").strip()

    # <think>-Tags entfernen (Fallback für ältere Ollama-Versionen)
    cleaned = _strip_thinking(raw)

    data = _extract_json(cleaned)

    if not data:
        logger.warning("[Ollama] Kein JSON erkannt – Rohantwort wird genutzt")
        return cleaned or raw

    result = _format_markdown(data)
    logger.info(f"[Ollama] Zusammenfassung erstellt ({len(result)} Zeichen)")
    return result
