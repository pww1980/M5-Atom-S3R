# Sprachaufnahme & Transkriptions-Pipeline

Sprachaufnahme per Knopfdruck, automatische Transkription mit Sprecherdiarisierung und LLM-Zusammenfassung, Benachrichtigung per Telegram.

```
[Aufnahmegerät] ──WiFi──► [Python-Server (z. B. N100)]
                               ├── faster-whisper  (Transkription)
                               ├── pyannote.audio  (Diarization)
                               ├── Ollama/Mistral  (Zusammenfassung)
                               └── Telegram Bot    (Benachrichtigung)
```

---

## Unterstützte Hardware

| Gerät | Firmware | Besonderheit |
|---|---|---|
| **M5Stack Cardputer** | `firmware-cardputer/` | Display, SD-Karte, WAV lokal + Upload |
| M5Stack Atom EchoS3R | `firmware/` | Kompakt, kein Display, nur WiFi-Upload |

**Empfehlung: Cardputer** – einfachere Bedienung dank Display und lokaler SD-Sicherung.

---

## Cardputer – Ablauf

```
Taste drücken
    │
    ▼
Aufnahme läuft (WAV direkt auf SD-Karte)
Display: roter Punkt + MM:SS Timer
    │
Taste drücken
    │
    ▼
"Senden?" – 10s Countdown auf Display
    │
    ├─ Taste drücken ──► Upload an Server ──► Transkription
    └─ Timeout (10s) ──► Nur SD, kein Upload
```

---

## Installation

### 1. PlatformIO

#### Weg A: VS Code (empfohlen)

1. [VS Code](https://code.visualstudio.com/) installieren
2. Extension **PlatformIO IDE** installieren (`Ctrl+Shift+X`)
3. VS Code neu starten

#### Weg B: CLI

```bash
pip install platformio
```

#### Linux: USB-Rechte (einmalig)

```bash
sudo usermod -aG dialout $USER
# Danach neu einloggen
```

---

### 2. Firmware flashen

```bash
git clone https://github.com/pww1980/M5-Atom-S3R.git
cd M5-Atom-S3R/firmware-cardputer

# Flashen
pio run -t upload

# Serieller Monitor (optional)
pio device monitor --baud 115200
```

> Beim ersten Build lädt PlatformIO alle Libraries automatisch herunter (~2–3 Minuten).

---

### 3. Ersteinrichtung (WLAN + Server-IP)

Beim ersten Start öffnet das Gerät einen eigenen WLAN-Hotspot:

1. Mit **`Cardputer-Transcription`** verbinden (kein Passwort)
2. Browser → **`192.168.4.1`**
3. Eintragen:
   - WLAN-Name (SSID) und Passwort
   - **Server-IP** (feste IP des Server-Rechners)
4. Speichern → Gerät verbindet sich automatisch

> Router-Tipp: Dem Server-Rechner eine feste IP per DHCP-Reservierung vergeben.

---

### 4. Server-Backend (Python)

#### 4a. Systempakete

```bash
sudo apt install ffmpeg
```

#### 4b. Python-Umgebung

```bash
cd transcription-server
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

#### 4c. Ollama

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama pull mistral
```

#### 4d. Konfiguration (`config.py`)

```python
PYANNOTE_TOKEN     = "hf_..."   # HuggingFace Token (siehe unten)
TELEGRAM_BOT_TOKEN = "..."      # Telegram Bot Token
TELEGRAM_CHAT_ID   = "..."      # Telegram Chat-ID
```

**HuggingFace-Token:**
1. Account anlegen: [huggingface.co](https://huggingface.co/)
2. Modell-Lizenz akzeptieren: [pyannote/speaker-diarization-3.1](https://huggingface.co/pyannote/speaker-diarization-3.1)
3. Token unter *Settings → Access Tokens* erstellen

#### 4e. Server starten

```bash
source venv/bin/activate
python main.py
```

#### 4f. Health-Check

```bash
python check_server.py
```

#### 4g. Autostart (systemd, optional)

```bash
sudo nano /etc/systemd/system/transcription.service
```

```ini
[Unit]
Description=Transcription Server
After=network.target

[Service]
User=<username>
WorkingDirectory=/pfad/zu/M5-Atom-S3R/transcription-server
ExecStart=/pfad/zu/venv/bin/python main.py
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable transcription
sudo systemctl start transcription
```

---

## Cardputer – Bedienung

### Display-Anzeigen

| Anzeige | Bedeutung |
|---|---|
| **Bereit** + Server-IP | Gerät einsatzbereit |
| Roter Punkt + **MM:SS** | Aufnahme läuft |
| **Senden?** + Countdown | Warten auf Bestätigung |
| **Uploading...** | Übertragung läuft |
| **Gesendet!** | Upload erfolgreich |
| **Fehler!** | Upload fehlgeschlagen (WAV auf SD gespeichert) |
| **Nur SD** | Timeout abgelaufen, kein Upload |

### Tastensteuerung

| Aktion | Zustand | Ergebnis |
|---|---|---|
| **OK** drücken | Bereit | Aufnahme starten |
| **OK** drücken | Aufnahme läuft | Aufnahme stoppen |
| **OK** drücken | "Senden?"-Bildschirm | Upload starten |
| Timeout (10s) | "Senden?"-Bildschirm | Nur SD, kein Upload |
| **OK** 5s halten | Bereit | WiFi-Einstellungen zurücksetzen |

### Piep-Signale

| Signal | Bedeutung |
|---|---|
| 2× kurz (1000 Hz) | Gerät bereit |
| 1× kurz (1000 Hz) | Aufnahme gestartet |
| 2× kurz (1000 Hz) | Aufnahme gestoppt |
| 3× kurz (1000 Hz) | Upload erfolgreich |
| 5× kurz (600 Hz) | Fehler (Upload / SD) |

---

## Verarbeitungs-Pipeline (Server)

```
WAV empfangen
      │
      ▼
faster-whisper  → Sprache-zu-Text
      │
      ▼
pyannote.audio  → Sprecherdiarisierung (Wer spricht wann?)
      │
      ▼
Alignment       → Whisper-Text + Sprecher zusammenführen
      │
      ▼
Ollama/Mistral  → Strukturierte Zusammenfassung
      │
      ▼
ffmpeg          → WAV → OGG/Opus (32 kbit/s)
      │
      ▼
Telegram        → "Transkription fertig"
```

### Ergebnis-Dateien

Pro Aufnahme entstehen drei Dateien in `transcription-server/output/`:

```
output/
├── 2025_04_13_14_30_transcript.md   # Transkript mit Sprecher-Timestamps
├── 2025_04_13_14_30_summary.md      # LLM-Zusammenfassung
└── 2025_04_13_14_30_audio.ogg       # Aufnahme (Opus, ~32 kbit/s)
```

---

## Remote-Debugging (kein Serial Monitor nötig)

Alle Geräte-Ereignisse erscheinen im Server-Log (`python main.py`):

```
[DEVICE] Cardputer Boot OK – SD: ja  WiFi: 192.168.50.37
[STATUS] Aufnahme gestartet  (Session: sess_12345)
[STATUS] Aufnahme beendet   (Session: sess_12345)
[HTTP]   sess_12345 Seg 0: 512,000 Bytes  [FINAL]
[DEVICE] Upload OK
```
