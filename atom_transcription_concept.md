# Implementierungskonzept: Atom Echo S3R – Transkriptions-Pipeline

## Ziel

Ein M5Stack Atom Echo S3R nimmt per Button-Druck Sprache auf und streamt sie über WiFi an einen lokalen Python-Server (Intel N100, 16 GB RAM). Der Server transkribiert die Aufnahme, führt Sprecherdiarisierung durch und erstellt eine strukturierte Zusammenfassung. Das Ergebnis wird per Telegram zugestellt.

---

## Systemübersicht

```
[Atom Echo S3R]  --WiFi/WebSocket-->  [Python-Server N100]
   ESP32-S3                               ├── Audio-Empfang (asyncio WebSocket)
   ES7210 Codec                           ├── faster-whisper (Transkription)
   GPIO 41 Button                         ├── pyannote.audio (Diarization)
   RGB LED                                ├── Alignment (Zeitstempel-Merge)
                                          ├── Ollama (Zusammenfassung)
                                          └── Telegram Bot (Notification)
```

---

## Teil 1: Firmware (ESP32-S3, Arduino-Framework)

### 1.1 Hardware-Konfiguration

| Komponente | Detail |
|---|---|
| Board | M5Stack Atom Echo S3R |
| MCU | ESP32-S3 |
| Audio-Codec | ES7210 (I2C-Adresse: 0x40) |
| Button | GPIO 41 (INPUT_PULLUP, LOW = gedrückt) |
| LED | RGB (adressierbar, einzelne LED) |
| Audio-Interface | I2S |
| Samplerate | 16.000 Hz |
| Bit-Tiefe | 16 Bit |
| Kanäle | Mono |

### 1.2 Audio-Feedback (Pieptöne)

Der Atom Echo S3R hat einen eingebauten Lautsprecher, angesteuert über I2S DAC (gleicher Bus wie Mikrofon, aber TX-Richtung). Pieptöne werden als kurze Sinuston-Bursts generiert (Frequenz ~1000 Hz, 16-bit PCM, 16 kHz Samplerate).

| Ereignis | Muster | Frequenz | Dauer je Ton |
|---|---|---|---|
| Boot – Server erreichbar | 2× kurz | 1000 Hz | 80ms Ton, 80ms Pause |
| Boot – Server nicht erreichbar | 5× kurz | 600 Hz | 80ms Ton, 80ms Pause |
| Aufnahme starten (Druck 1) | 1× kurz | 1000 Hz | 100ms |
| Aufnahme beenden (Druck 2) | 3× kurz | 1000 Hz | 80ms Ton, 60ms Pause |

**Server-Erreichbarkeitsprüfung beim Boot:**
TCP-Verbindungsversuch auf `SERVER_IP:8765`, Timeout 2 Sekunden. Kein vollständiger WebSocket-Handshake nötig — reiner TCP-Connect reicht als Erreichbarkeitstest.

**Wichtig:** I2S kann nicht gleichzeitig senden (Lautsprecher) und empfangen (Mikrofon). Pieptöne daher nur im IDLE- und PROCESSING-Zustand ausgeben — nie während einer laufenden Aufnahme. Der Ton beim Aufnahme-Stopp (3× piepsen) wird *nach* dem Schließen des I2S-RX-Streams ausgegeben.

### 1.3 Initialisierungsreihenfolge beim Boot

1. Serial starten (115200 Baud, nur für Debugging)
2. WiFi verbinden (SSID + Passwort als Konstanten definieren)
3. ES7210 per I2C initialisieren:
   - I2C auf Standard-Pins starten
   - ES7210 Reset-Sequenz ausführen
   - Samplerate 16 kHz, Mono, 16 Bit konfigurieren
   - Mikrofon-Gain setzen (empfohlen: 24–30 dB)
4. I2S-Interface initialisieren:
   - Mode: Master, RX
   - Sample Rate: 16.000 Hz
   - Bits per Sample: 16
   - Channel: Mono
   - DMA Buffer: 512 Samples, 8 Buffer
5. Button-Pin als INPUT_PULLUP konfigurieren
6. Server-Erreichbarkeit prüfen (TCP-Connect, Timeout 2s):
   - Erreichbar → 2× piepsen (1000 Hz), LED: IDLE (weiß, gedimmt)
   - Nicht erreichbar → 5× piepsen (600 Hz), LED: ERROR (blau blinkend), danach trotzdem IDLE

### 1.3 Zustandsmaschine

```
IDLE ──(Druck 1)──> RECORDING ──(Druck 2)──> PROCESSING ──(ACK)──> IDLE
  ^                                                                    |
  └──────────────────────────(langer Druck >3s)───────────────────────┘
```

| Zustand | LED-Farbe | LED-Verhalten | Aktion |
|---|---|---|---|
| IDLE | Weiß | Dauerhaft gedimmt (10%) | Wartet auf Button |
| RECORDING | Rot | Blinkt 500ms Intervall | Streamt Audio |
| PROCESSING | Gelb | Pulsiert (Fade in/out) | Wartet auf Server-ACK |
| DONE | Grün | 2 Sekunden an, dann IDLE | Fertig |
| ERROR | Blau | Schnelles Blinken (200ms) | Verbindungsfehler |

### 1.4 Button-Logik (Entprellung: 50ms)

```
Druck erkannt (LOW, nach Entprellung):
  - Zustand == IDLE:
      → WebSocket-Verbindung öffnen zu ws://SERVER_IP:8765
      → Bei Erfolg: 1× piepsen (1000 Hz), Zustand = RECORDING
      → Bei Fehler: Zustand = ERROR, 3s warten, zurück zu IDLE
  
  - Zustand == RECORDING:
      → I2S-RX stoppen
      → I2S-Puffer leeren (letzten Chunk senden)
      → WebSocket sauber schließen (close frame)
      → 3× piepsen (1000 Hz)
      → Zustand = PROCESSING

  - langer Druck (>3s), Zustand == RECORDING:
      → WebSocket hart trennen (kein close frame)
      → Zustand = IDLE
      → Keine Verarbeitung auf Server
```

### 1.5 Audio-Streaming (im RECORDING-Zustand)

- I2S-Buffer in 512-Sample-Chunks auslesen (= 512 × 2 Bytes = 1024 Bytes pro Chunk)
- Chunk direkt als Binärdaten über WebSocket senden (Binary Frame, kein Base64)
- Streaming kontinuierlich in der Loop, kein Delay

### 1.6 WLAN-Abbruch während der Aufnahme

Der Atom Echo S3R verfügt über 8 MB PSRAM. Bei Verbindungsverlust werden Chunks lokal gepuffert und nach Wiederverbindung nachgesendet.

**PSRAM-Limit:** Maximal 4 MB für den Audio-Puffer reservieren (50% des PSRAM), der Rest bleibt für Stack, Heap und I2S-DMA. Bei 16 kHz / 16 Bit Mono entspricht 4 MB ca. 2 Minuten Aufnahme. Ist der Puffer voll, werden älteste Chunks verworfen (Ringpuffer-Prinzip) — die Aufnahme läuft weiter, es gehen nur die ältesten Daten aus der Offline-Phase verloren.

**Ablauf bei Verbindungsverlust:**
1. WebSocket-Fehler erkannt → LED wechselt zu Blau pulsierend (Zustand: RECONNECTING)
2. I2S-Aufnahme läuft weiter
3. Chunks werden in PSRAM-Ringpuffer geschrieben (max. 4 MB)
4. WiFi-Reconnect-Versuch alle 2 Sekunden (max. 30 Versuche = 60 Sekunden)
5. Bei Erfolg: WebSocket neu aufbauen, PSRAM-Puffer zuerst senden, dann wieder live streamen, LED zurück zu Rot blinkend
6. Nach 60 Sekunden ohne Reconnect: Aufnahme abbrechen, 5× Fehler-Piep (600 Hz), LED ERROR (blau blinkend schnell), zurück zu IDLE — Server verwirft unvollständigen Puffer

**Neue Konfigurationskonstanten:**
```
PSRAM_BUFFER_MAX_BYTES      = 4 * 1024 * 1024   // 4 MB
WIFI_RECONNECT_INTERVAL_MS  = 2000
WIFI_RECONNECT_MAX_ATTEMPTS = 30
```

**Ergänzung Zustandsmaschine:**
```
RECORDING ──(WLAN-Verlust)──> RECONNECTING ──(Erfolg)──> RECORDING
                                           └──(Timeout)──> IDLE
```

| Zustand | LED-Farbe | LED-Verhalten | Aktion |
|---|---|---|---|
| RECONNECTING | Blau | Pulsiert langsam | Puffert in PSRAM, versucht Reconnect |

### 1.7 Abhängigkeiten (Arduino Libraries)

- `arduinoWebSockets` von Markus Sattler (WebSocket-Client)
- `Wire` (I2C für ES7210, built-in)
- `driver/i2s.h` (ESP-IDF I2S, über Arduino zugänglich)
- `WiFi.h` (built-in ESP32)
- `Adafruit_NeoPixel` oder `FastLED` (RGB LED)
- `esp_psram.h` (PSRAM-Zugriff, built-in ESP-IDF)

### 1.8 Konfigurationskonstanten (oben in der .ino-Datei)

```
WIFI_SSID                   = "..."
WIFI_PASSWORD               = "..."
SERVER_IP                   = "192.168.x.x"    // Feste IP des N100
SERVER_PORT                 = 8765
I2S_SAMPLE_RATE             = 16000
I2S_DMA_BUF_LEN            = 512
I2S_DMA_BUF_CNT            = 8
ES7210_I2C_ADDR             = 0x40
BUTTON_PIN                  = 41
BUTTON_DEBOUNCE             = 50               // ms
LONG_PRESS_MS               = 3000
MIC_GAIN_DB                 = 24
PSRAM_BUFFER_MAX_BYTES      = 4194304          // 4 MB
WIFI_RECONNECT_INTERVAL_MS  = 2000
WIFI_RECONNECT_MAX_ATTEMPTS = 30
```

---

## Teil 2: Server-Backend (Python, N100)

### 2.1 Projektstruktur

```
transcription-server/
├── main.py                  # Einstiegspunkt, startet WebSocket-Server
├── config.py                # Alle Konfigurationswerte
├── audio_receiver.py        # WebSocket-Handler, schreibt WAV
├── pipeline.py              # Orchestriert Whisper → Pyannote → Align → LLM
├── transcriber.py           # faster-whisper Wrapper
├── diarizer.py              # pyannote Wrapper
├── aligner.py               # Zeitstempel-Merge Whisper + Pyannote
├── summarizer.py            # Ollama API-Aufruf
├── notifier.py              # Telegram Bot
├── requirements.txt
└── output/                  # Ergebnisdateien (auto-erstellt)
```

### 2.2 Abhängigkeiten (requirements.txt)

```
faster-whisper
pyannote.audio
websockets
pydub
requests
torch
torchaudio
```

**Hinweis:** `pyannote.audio` benötigt PyTorch. Auf CPU (N100) reicht `torch` ohne CUDA. Installation einmalig dauerhaft (~5–10 min).

**Einmalig erforderlich:**
- HuggingFace-Account erstellen (kostenlos)
- Modell-Lizenz akzeptieren: `pyannote/speaker-diarization-3.1`
- HuggingFace-Token in `config.py` eintragen

### 2.3 Konfiguration (config.py)

```
WEBSOCKET_HOST         = "0.0.0.0"
WEBSOCKET_PORT         = 8765
AUDIO_SAMPLE_RATE      = 16000
AUDIO_CHANNELS         = 1
AUDIO_BIT_DEPTH        = 16

WHISPER_MODEL          = "medium"
WHISPER_LANGUAGE       = "de"
WHISPER_COMPUTE_TYPE   = "int8"      # Optimal für CPU

PYANNOTE_TOKEN         = "hf_..."    # HuggingFace Token
PYANNOTE_MIN_SPEAKERS  = 1
PYANNOTE_MAX_SPEAKERS  = None        # Flexibel/automatisch

OLLAMA_URL             = "http://localhost:11434/api/generate"
OLLAMA_MODEL           = "mistral"   # oder gemma3:4b

TELEGRAM_BOT_TOKEN     = "..."
TELEGRAM_CHAT_ID       = "..."

OUTPUT_DIR             = "./output"
```

### 2.4 audio_receiver.py – WebSocket-Server

**Verhalten:**
- Startet asyncio WebSocket-Server auf `0.0.0.0:8765`
- Pro eingehender Verbindung: neues Session-Objekt anlegen
- Eingehende Binärdaten (PCM-Chunks) in ByteArray puffern
- Bei Verbindungsschluss (close frame empfangen):
  1. WAV-Header prependen (44 Byte, Standard RIFF/WAVE)
  2. WAV-Datei schreiben nach `output/YYYY_MM_DD_HH_MM_audio.wav`
  3. Session-Pfad in die **Pipeline-Queue** einreihen (nicht direkt starten)
- Bei abgebrochener Verbindung (kein close frame): Puffer verwerfen, keine Verarbeitung

**Gleichzeitige Verbindungen:**
Mehrere Verbindungen werden gleichzeitig angenommen und gepuffert. Jede abgeschlossene Session wird sequenziell in die Queue eingereiht. Der Pipeline-Worker arbeitet Jobs strikt nacheinander ab — keine parallelen Whisper/Pyannote-Läufe. Dadurch kein Ressourcenkonflikt auf dem N100.

```
Session A fertig ──┐
Session B fertig ──┼──> Queue ──> Pipeline-Worker (sequenziell)
Session C fertig ──┘
```

**Queue-Implementierung:** `asyncio.Queue` — unbegrenzte Tiefe, FIFO. Der Worker läuft als eigener asyncio-Task parallel zum WebSocket-Server.

**WAV-Header-Parameter:**
- ChunkID: `RIFF`
- Format: `WAVE`
- Subchunk1ID: `fmt `
- AudioFormat: 1 (PCM)
- NumChannels: 1
- SampleRate: 16000
- BitsPerSample: 16
- ByteRate: 32000 (SampleRate × NumChannels × BitsPerSample/8)
- BlockAlign: 2
- Subchunk2ID: `data`
- Subchunk2Size: Länge der Rohdaten in Bytes

### 2.5 transcriber.py – Whisper-Wrapper

**Verhalten:**
- Modell beim ersten Aufruf laden und im Speicher halten (nicht bei jedem Aufruf neu laden)
- `faster_whisper.WhisperModel("medium", device="cpu", compute_type="int8")`
- `model.transcribe(wav_path, beam_size=5, vad_filter=True)` — **kein `language`-Parameter**
- Whisper erkennt die Sprache automatisch aus den ersten 30 Sekunden (zuverlässig für Deutsch/Englisch)
- Erkannte Sprache wird ins Log geschrieben und in die MD-Dateien aufgenommen
- Rückgabe: Liste von Segmenten, je Segment: `{start: float, end: float, text: str}`, plus `detected_language: str`
- VAD-Filter aktivieren (entfernt Stille, verbessert Qualität)

### 2.6 diarizer.py – Pyannote-Wrapper

**Verhalten:**
- Pipeline einmalig laden: `Pipeline.from_pretrained("pyannote/speaker-diarization-3.1", use_auth_token=TOKEN)`
- Aufruf mit: `pipeline(wav_path, min_speakers=1, max_speakers=None)`
- Rückgabe: Liste von Turns, je Turn: `{start: float, end: float, speaker: str}`
- Speaker-IDs normalisieren: `SPEAKER_00`, `SPEAKER_01`, etc.

### 2.7 aligner.py – Zeitstempel-Merge

**Algorithmus:**
Für jedes Whisper-Segment den dominierenden Sprecher bestimmen:

1. Alle Pyannote-Turns mit Zeitüberlapp zum Whisper-Segment finden
2. Überlapp-Dauer je Sprecher aufsummieren
3. Sprecher mit größtem Überlapp → zugewiesener Sprecher für dieses Segment
4. Aufeinanderfolgende Segmente desselben Sprechers zusammenführen

**Ausgabe-Format (transcript.txt):**
```
[00:00:03] SPRECHER_A: Text des ersten Segments hier.
[00:00:08] SPRECHER_B: Antwort des zweiten Sprechers.
[00:00:15] SPRECHER_A: Weiterer Text...
```

### 2.8 summarizer.py – Ollama-Wrapper

**Input:** Vollständiges Diarization-Transkript als String

**Prompt-Struktur (deutsch):**

```
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
[TRANSKRIPT]
```

**API-Aufruf:** POST an `http://localhost:11434/api/generate`, `stream: false`

**Rückgabe:** String (Zusammenfassung)

### 2.9 notifier.py – Telegram

**Verhalten nach Abschluss:**
- Eine einzelne Textnachricht senden: `"✅ Transkription fertig: 2025_04_13_14_30"`
- Falls Fehler in der Pipeline: `"❌ Fehler bei Job 2025_04_13_14_30: [Fehlermeldung]"`
- Kein Datei-Versand. Die MD-Dateien liegen lokal auf dem Server.

**API-Endpunkt:**
- `https://api.telegram.org/bot{TOKEN}/sendMessage` – ausschließlich Textnachricht

### 2.10 pipeline.py – Orchestrierung

**Sequenzieller Ablauf (kein Threading innerhalb der Pipeline):**

```
1. transcriber.transcribe(wav_path)        → whisper_segments, detected_language
2. diarizer.diarize(wav_path)              → speaker_turns
3. aligner.align(whisper_segments,         → merged_transcript (str)
                 speaker_turns)
4. YYYY_MM_DD_HH_MM_transcript.md schreiben (inkl. erkannte Sprache im Header)
5. summarizer.summarize(merged_transcript) → summary (str)
6. YYYY_MM_DD_HH_MM_summary.md schreiben
7. WAV → OGG konvertieren (ffmpeg via subprocess, Opus-Codec, 32 kbit/s)
8. WAV-Datei löschen
9. notifier.send(session_name)
10. Logging: Laufzeit, Dateigrößen, erkannte Sprache ausgeben
```

**OGG-Konvertierung (Schritt 7):**
- Tool: `ffmpeg` (muss auf dem Server installiert sein: `apt install ffmpeg`)
- Codec: Opus
- Bitrate: 32 kbit/s (ausreichend für Sprache, ca. 240 KB/min)
- Kommando: `ffmpeg -i audio.wav -c:a libopus -b:a 32k audio.ogg`
- WAV wird erst nach erfolgreich geschriebener OGG-Datei gelöscht

**Fehlerbehandlung:** Jeder Schritt in try/except. Bei Fehler: Schritt überspringen, Fehlermeldung in error.log, Telegram-Notification mit Fehlerdetail.

### 2.11 main.py – Einstiegspunkt

- Logging konfigurieren (INFO-Level, in console und `server.log`)
- `output/`-Verzeichnis erstellen falls nicht vorhanden
- Whisper-Modell vorladen beim Start (verhindert Verzögerung beim ersten Aufruf)
- WebSocket-Server starten, asyncio Event-Loop laufen lassen
- Graceful Shutdown bei SIGINT/SIGTERM

---

## Teil 3: Setup & Deployment

### 3.1 Server-Setup (einmalig)

```bash
# 1. Systempakete
apt install ffmpeg

# 2. Python-Umgebung
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# 3. Ollama installieren (falls noch nicht vorhanden)
curl -fsSL https://ollama.com/install.sh | sh
ollama pull mistral

# 4. HuggingFace: Einmalig einloggen und Token in config.py eintragen
#    Modell-Lizenz akzeptieren unter:
#    https://huggingface.co/pyannote/speaker-diarization-3.1

# 5. Server starten
python main.py
```

### 3.2 Autostart (systemd)

Service-Unit `/etc/systemd/system/transcription.service` erstellen:

```ini
[Unit]
Description=Atom Transcription Server
After=network.target

[Service]
User=<username>
WorkingDirectory=/pfad/zum/projekt
ExecStart=/pfad/zum/venv/bin/python main.py
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```bash
systemctl enable transcription
systemctl start transcription
```

### 3.3 Netzwerk

- N100 bekommt feste IP im Router (DHCP-Reservierung per MAC)
- Port 8765 muss im lokalen Netz erreichbar sein (kein externer Zugriff nötig)
- Atom Echo S3R und N100 im selben WLAN-Netz

---

## Teil 4: Output-Struktur

Pro Aufnahme-Session entstehen drei Dateien im `output/`-Verzeichnis:

```
output/
├── 2025_04_13_14_30_transcript.md   # Diarization-Transkript mit Timestamps
├── 2025_04_13_14_30_summary.md      # LLM-Zusammenfassung
└── 2025_04_13_14_30_audio.ogg       # Aufnahme (Opus, ~32 kbit/s)
```

**Dateiname-Schema:** `YYYY_MM_DD_HH_MM_[typ]` — Timestamp ist der Zeitpunkt des Aufnahme-Starts.

**MD-Struktur `_transcript.md`:**
```markdown
# Transkript – 2025_04_13_14_30

- **Sprache:** Deutsch
- **Sprecher:** 2 erkannt (SPRECHER_A, SPRECHER_B)

## Verlauf

**[00:00:03] SPRECHER_A:** Text des ersten Segments hier.

**[00:00:08] SPRECHER_B:** Antwort des zweiten Sprechers.

**[00:00:15] SPRECHER_A:** Weiterer Text...
```

**MD-Struktur `_summary.md`:**
```markdown
# Zusammenfassung – 2025_04_13_14_30

- **Sprache:** Deutsch
- **Sprecher:** 2 erkannt

## Kernaussagen je Sprecher

...

## Entscheidungen

...

## Offene Punkte / Fragen

...
```

---

## Teil 5: Bekannte Stolpersteine

| Problem | Ursache | Lösung |
|---|---|---|
| ES7210 liefert kein Audio | I2C-Initialisierung fehlt | Vor I2S starten, Reset-Sequenz einhalten |
| Pyannote lädt ewig | Erste Initialisierung dauert | Akzeptiert, nicht zeitkritisch |
| Leises/verzerrtes Audio | Gain zu niedrig/hoch | MIC_GAIN_DB anpassen (24–30 dB) |
| WebSocket-Verbindung schlägt fehl | Server nicht erreichbar | IP-Adresse und Port prüfen, Firewall |
| Sprecher werden vertauscht | Kurze Äußerungen | `min_speakers` erhöhen verbessert Stabilität |
| I2S RX/TX Konflikt | I2S ist half-duplex | Pieptöne nur außerhalb RECORDING-Zustand ausgeben |
| PSRAM nicht verfügbar | PSRAM im Build nicht aktiviert | In platformio.ini: `board_build.arduino.memory_type = qio_opi` |
| Falsche Spracherkennung | Kurze Aufnahme oder starker Akzent | Mindestaufnahme ~10 Sekunden für zuverlässige Erkennung |
| ffmpeg nicht gefunden | Nicht installiert | `apt install ffmpeg` auf dem Server |
| OGG-Datei leer | ffmpeg-Fehler | WAV erst löschen wenn OGG-Größe > 0 Bytes geprüft |

---

## Zusammenfassung der Designentscheidungen

| Entscheidung | Wahl | Begründung |
|---|---|---|
| Audio-Format | PCM 16-bit / 16 kHz Mono | Whisper-optimiert, geringer Datenstrom |
| Übertragungsprotokoll | WebSocket Binary | Kein HTTP-Overhead, sauberes EOF-Signal |
| WLAN-Abbruch | PSRAM-Ringpuffer (4 MB) | Kein Datenverlust, 50% PSRAM-Limit |
| Whisper-Modell | medium, int8, CPU | Gute Qualität, N100-tauglich |
| Spracherkennung | Whisper Auto-Detect | Unterstützt Deutsch + Englisch ohne Konfiguration |
| Diarization | pyannote 3.1, flexibel | State-of-the-art, kostenlos lokal |
| Gleichzeitige Jobs | asyncio.Queue (FIFO) | Kein Ressourcenkonflikt, kein Datenverlust |
| Audioarchiv | OGG/Opus 32 kbit/s | ~240 KB/min statt ~1,9 MB/min (WAV), WAV wird gelöscht |
| Zusammenfassung | Ollama (Mistral) lokal | Kein Cloud-Dependency, ausreichend Qualität |
| Notification | Telegram (OpenClaw) | Passt zur bestehenden Infrastruktur |
