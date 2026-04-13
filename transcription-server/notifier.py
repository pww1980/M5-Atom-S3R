"""
notifier.py – Telegram-Benachrichtigung per Bot.
"""

import logging
import requests
import config

logger = logging.getLogger(__name__)


def send(session_name: str, error: str = None):
    """
    Sendet eine Textnachricht via Telegram.
    Bei Fehler: error != None.
    """
    if error:
        text = f"\u274c Fehler bei Job {session_name}: {error}"
    else:
        text = f"\u2705 Transkription fertig: {session_name}"

    url = f"https://api.telegram.org/bot{config.TELEGRAM_BOT_TOKEN}/sendMessage"
    payload = {
        "chat_id": config.TELEGRAM_CHAT_ID,
        "text":    text,
    }

    try:
        response = requests.post(url, json=payload, timeout=10)
        response.raise_for_status()
        logger.info(f"[TELEGRAM] Nachricht gesendet: {text}")
    except Exception as e:
        logger.error(f"[TELEGRAM] Fehler beim Senden: {e}")
