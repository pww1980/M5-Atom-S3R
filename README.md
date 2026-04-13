# Atom Echo S3R – Transkriptions-Pipeline

Sprachaufnahme per Knopfdruck auf dem M5Stack Atom VoiceS3R, automatische Transkription mit Sprecherdiarisierung und LLM-Zusammenfassung, Benachrichtigung per Telegram.

```
[Atom VoiceS3R] --WiFi/WebSocket--> [Python-Server (N100)]
   ESP32-S3                              ├── faster-whisper  (Transkription)
   ES8311 Codec                          ├── pyannote.audio  (Diarization)
   GPIO 41 Button                        ├── Ollama/Mistral  (Zusammenfassung)
                                         └── Telegram Bot    (Benachrichtigung)
```

---

## Implementierung / Installation

### Voraussetzungen

| Komponente | Anforderung |
|---|---|
| Hardware | M5Stack Atom VoiceS3R (Atom EchoS3R) |
| IDE | [PlatformIO](https://platformio.org/) (VS Code Extension oder CLI) |
| Server | Linux-Rechner im lokalen Netz (z. B. Intel N100), Python 3.10+ |
| Accounts | [HuggingFace](https://huggingface.co/) (kostenlos, für pyannote) |

---

### 1. PlatformIO installieren

PlatformIO läuft auf Windows und Linux gleichermaßen gut. Nimm die Umgebung, die du bereits nutzt – wenn du den N100-Server sowieso unter Linux betreibst, kannst du auch dort flashen.

#### Weg 1: VS Code + Extension (empfohlen)

1. **VS Code** installieren: [code.visualstudio.com](https://code.visualstudio.com/)
2. In VS Code: `Ctrl+Shift+X` → nach **PlatformIO IDE** suchen → installieren
3. VS Code neu starten → PlatformIO-Icon erscheint in der linken Leiste

#### Weg 2: CLI (ohne VS Code)

```bash
pip install platformio
```

#### Linux: USB-Rechte einmalig setzen

Damit PlatformIO ohne `sudo` auf den USB-Port zugreifen darf:

```bash
sudo usermod -aG dialout $USER
# Danach neu einloggen (oder: newgrp dialout)
```

---

### 2. Firmware (ESP32-S3)

```bash
# Repository klonen
git clone https://github.com/pww1980/M5-Atom-S3R.git
cd M5-Atom-S3R/firmware

# Flashen via PlatformIO
pio run -t upload

# Seriellen Monitor öffnen (optional, für Debugging)
pio device monitor --baud 115200
```

> Beim ersten Build lädt PlatformIO automatisch alle Libraries (`M5Unified`, `WebSockets`, `WiFiManager`) und die ESP32-S3-Toolchain herunter – das dauert einmalig ca. 2–3 Minuten.

Die Firmware trägt sich selbst ins WLAN ein – keine Zugangsdaten im Code nötig (siehe Abschnitt **Ersteinrichtung** unter Bedienung).

> **Technischer Hinweis:** Die Firmware nutzt beide Kerne des ESP32-S3. Der Audio-Capture läuft als dedizierter FreeRTOS-Task auf Core 0 und liest kontinuierlich vom I2S-DMA. Der WebSocket-Versand läuft auf Core 1 (Arduino-Loop). Kommunikation zwischen den Tasks erfolgt über eine FreeRTOS-Queue mit Buffer-Pool (16 × 1 KB im SRAM) – das verhindert Audioglitches auch bei kurzen WLAN-Verzögerungen.

---

### 3. Server-Backend (Python)

#### 3a. Systempakete

```bash
sudo apt install ffmpeg
```

#### 3b. Python-Umgebung

```bash
cd transcription-server
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

> Die erste Installation dauert ca. 5–10 Minuten (PyTorch + pyannote).

#### 3c. Ollama installieren

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama pull mistral
```

#### 3d. Konfiguration (`config.py`)

```python
PYANNOTE_TOKEN    = "hf_..."   # HuggingFace Token (s. unten)
TELEGRAM_BOT_TOKEN = "..."     # Telegram Bot Token
TELEGRAM_CHAT_ID   = "..."     # Telegram Chat-ID
```

**HuggingFace-Token erstellen:**
1. Account anlegen auf [huggingface.co](https://huggingface.co/)
2. Modell-Lizenz akzeptieren: [pyannote/speaker-diarization-3.1](https://huggingface.co/pyannote/speaker-diarization-3.1)
3. Token unter *Settings → Access Tokens* erstellen und in `config.py` eintragen

#### 3e. Server starten

```bash
source venv/bin/activate
python main.py
```

#### 3f. Autostart (systemd, optional)

```bash
sudo nano /etc/systemd/system/transcription.service
```

```ini
[Unit]
Description=Atom Transcription Server
After=network.target

[Service]
User=<dein-username>
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

#### 3g. Netzwerk

- Den Server-Rechner im Router eine **feste IP** vergeben (DHCP-Reservierung per MAC).
- Port `8765` muss im lokalen Netz erreichbar sein – kein externer Zugriff erforderlich.
- Atom VoiceS3R und Server müssen im gleichen WLAN sein.

---

### Ergebnis-Dateien

Pro Aufnahme entstehen drei Dateien in `transcription-server/output/`:

```
output/
├── 2025_04_13_14_30_transcript.md   # Transkript mit Sprecher-Timestamps
├── 2025_04_13_14_30_summary.md      # LLM-Zusammenfassung
└── 2025_04_13_14_30_audio.ogg       # Aufnahme (Opus, ~32 kbit/s)
```

---

## Bedienung

### Ersteinrichtung (WLAN + Server-IP)

Beim allerersten Start – oder nach einem WiFi-Reset – öffnet das Gerät einen eigenen WLAN-Hotspot:

1. Mit dem Smartphone oder Laptop mit dem WLAN **`Atom-Transcription-Setup`** verbinden (kein Passwort)
2. Browser öffnen → **`192.168.4.1`** aufrufen
3. Im Portal eintragen:
   - WLAN-Name (SSID) und Passwort
   - **Server-IP** (feste IP des N100-Rechners)
4. Auf **Speichern** klicken → Gerät verbindet sich und startet normal

Die Zugangsdaten werden dauerhaft gespeichert und beim nächsten Start automatisch verwendet.

---

### Aufnahme

| Aktion | Beschreibung |
|---|---|
| **1× kurz drücken** (im Ruhezustand) | Aufnahme starten |
| **1× kurz drücken** (während Aufnahme) | Aufnahme beenden und verarbeiten |
| **3 Sekunden halten** (während Aufnahme) | Aufnahme abbrechen (kein Upload) |
| **5 Sekunden halten** (im Ruhezustand) | WiFi-Einstellungen zurücksetzen |

---

### Piep-Signale

| Signal | Bedeutung |
|---|---|
| 2× kurz (1000 Hz) | Gerät bereit, Server erreichbar |
| 5× kurz (600 Hz) | Server nicht erreichbar |
| 1× kurz (1000 Hz) | Aufnahme läuft |
| 3× kurz (1000 Hz) | Aufnahme beendet, Verarbeitung läuft |
| 5× kurz (600 Hz) | Verbindungsfehler / Timeout |
| 2× lang (1000 Hz) | WiFi-Reset wird durchgeführt |

---

### Verarbeitungs-Pipeline (automatisch nach Aufnahme)

```
Aufnahme beendet
      │
      ▼
faster-whisper      → Sprache-zu-Text (automatische Spracherkennung)
      │
      ▼
pyannote.audio      → Sprecherdiarisierung (Wer spricht wann?)
      │
      ▼
Alignment           → Whisper-Text + Sprecher zusammenführen
      │
      ▼
Ollama (Mistral)    → Strukturierte Zusammenfassung erstellen
      │
      ▼
ffmpeg              → WAV → OGG/Opus (32 kbit/s)
      │
      ▼
Telegram            → "✅ Transkription fertig: 2025_04_13_14_30"
```

---

### WLAN-Unterbrechung während der Aufnahme

Bricht die WLAN-Verbindung während einer Aufnahme ab, läuft die Aufnahme weiter. Das Gerät puffert die Audiodaten lokal im PSRAM (bis zu 4 MB ≈ 2 Minuten) und sendet sie nach Wiederverbindung nach. Nach 60 Sekunden ohne Verbindung wird die Aufnahme automatisch abgebrochen (5× Fehler-Piep).
