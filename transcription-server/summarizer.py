"""
summarizer.py – Ollama API Wrapper für LLM-Zusammenfassung.
"""

import logging
import requests
import config

logger = logging.getLogger(__name__)

PROMPT_TEMPLATE = """\
Du bist ein präziser Assistent für Gesprächsanalyse.

Analysiere das folgende Transkript und erstelle eine strukturierte Zusammenfassung in drei Abschnitten:

1. KERNAUSSAGEN JE SPRECHER
   - Liste die wichtigsten Aussagen pro Sprecher stichpunktartig auf.

2. ENTSCHEIDUNGEN
   - Liste alle im Gespräch getroffenen Entscheidungen oder Vereinbarungen auf.
   - Falls keine: "Keine expliziten Entscheidungen."

3. OFFENE PUNKTE / FRAGEN
   - Liste ungeklärte Fragen oder Aufgaben auf.
   - Falls keine: "Keine offenen Punkte."

Transkript:
{transcript}
"""


def summarize(transcript: str) -> str:
    """
    Sendet das Transkript an Ollama und gibt die Zusammenfassung zurück.
    """
    prompt = PROMPT_TEMPLATE.format(transcript=transcript)
    payload = {
        "model":  config.OLLAMA_MODEL,
        "prompt": prompt,
        "stream": False,
    }

    logger.info(f"[OLLAMA] Sende Anfrage an {config.OLLAMA_URL} ...")
    response = requests.post(config.OLLAMA_URL, json=payload, timeout=300)
    response.raise_for_status()

    data = response.json()
    summary = data.get("response", "").strip()
    logger.info(f"[OLLAMA] Zusammenfassung erhalten ({len(summary)} Zeichen)")
    return summary
