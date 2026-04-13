# Testplan – Atom Echo S3R Transkriptions-Pipeline

## Übersicht

| Phase | Bereich | Hardware nötig |
|---|---|---|
| T1 | Server-Backend (Unit) | Nein |
| T2 | Firmware – Boot & WiFi | Ja |
| T3 | Firmware – Button & Pieptöne | Ja |
| T4 | Firmware – Audio & WebSocket | Ja + Server |
| T5 | Firmware – WLAN-Abbruch | Ja + Server |
| T6 | Server – Pipeline | Ja + Server |
| T7 | End-to-End | Ja + Server |

---

## T1 – Server-Backend (ohne Hardware)

### T1.1 WAV-Header
**Vorbereitung:** Rohe PCM-Datei erzeugen  
```bash
# 1 Sekunde Stille als PCM
python3 -c "import sys; sys.stdout.buffer.write(b'\x00\x00' * 16000)" > test.pcm
```
**Schritte:**
1. `audio_receiver.py` direkt importieren, `_build_wav_header(32000)` aufrufen
2. Ergebnis mit `ffprobe` oder `python wave` prüfen

**Erwartetes Ergebnis:**
- Header ist 44 Bytes
- SampleRate=16000, Channels=1, BitsPerSample=16

---

### T1.2 Aligner
**Schritte:**
```python
from aligner import align
segs  = [{"start": 0.0, "end": 3.0, "text": "Hallo Welt"},
          {"start": 3.1, "end": 6.0, "text": "Wie geht es"}]
turns = [{"start": 0.0, "end": 3.5, "speaker": "SPRECHER_A"},
          {"start": 3.5, "end": 6.0, "speaker": "SPRECHER_B"}]
print(align(segs, turns))
```
**Erwartetes Ergebnis:**
```
[00:00] SPRECHER_A: Hallo Welt
[00:03] SPRECHER_B: Wie geht es
```

---

### T1.3 WebSocket-Server (TCP-Erreichbarkeit)
**Schritte:**
1. `python main.py` starten
2. `nc -zv localhost 8765` ausführen

**Erwartetes Ergebnis:** `Connection to localhost 8765 port [tcp/*] succeeded`

---

### T1.4 Pipeline-Queue (sequenziell)
**Schritte:**
1. Server starten
2. Zwei WAV-Dateien direkt in die Queue legen:
```python
import asyncio
from audio_receiver import pipeline_queue
asyncio.run(pipeline_queue.put("output/test1.wav"))
asyncio.run(pipeline_queue.put("output/test2.wav"))
```
3. Log beobachten

**Erwartetes Ergebnis:**
- Job 1 startet, Job 2 wartet
- Job 2 startet erst nach Abschluss von Job 1
- Keine parallelen Whisper-Läufe

---

### T1.5 Telegram-Notifier
**Schritte:**
```python
from notifier import send
send("2025_01_01_12_00")               # Erfolg
send("2025_01_01_12_00", "Testfehler") # Fehler
```
**Erwartetes Ergebnis:**
- Telegram erhält: `✅ Transkription fertig: 2025_01_01_12_00`
- Telegram erhält: `❌ Fehler bei Job 2025_01_01_12_00: Testfehler`

---

### T1.6 OGG-Konvertierung
**Schritte:**
1. Test-WAV erzeugen: `ffmpeg -f lavfi -i sine=frequency=440:duration=5 test.wav`
2. In `pipeline.py`: `_convert_to_ogg("test.wav")` aufrufen

**Erwartetes Ergebnis:**
- `test.ogg` existiert und ist > 0 Bytes
- `test.wav` ist gelöscht
- Bitrate ~32 kbit/s (`ffprobe test.ogg`)

---

## T2 – Firmware: Boot & WiFi

### T2.1 Ersteinrichtung (kein WLAN gespeichert)
**Schritte:**
1. Atom flashen (frisch, kein WLAN gespeichert)
2. Mit Smartphone nach WLAN `Atom-Transcription-Setup` suchen

**Erwartetes Ergebnis:**
- Hotspot erscheint innerhalb von 10 Sekunden
- Browser öffnet `192.168.4.1` automatisch oder manuell
- Formular zeigt Felder: SSID, Passwort, Server-IP

---

### T2.2 WLAN-Konfiguration speichern
**Schritte:**
1. Im Portal SSID, Passwort, Server-IP eintragen → Speichern
2. Atom startet neu

**Erwartetes Ergebnis:**
- Atom verbindet sich mit dem eingetragenen WLAN
- Serieller Monitor zeigt: `[WIFI] Verbunden. IP: ... Server: ...`

---

### T2.3 Boot – Server erreichbar
**Voraussetzung:** Server läuft, Atom im gleichen WLAN  
**Erwartetes Ergebnis:** 2× kurzer Piep (1000 Hz)

---

### T2.4 Boot – Server nicht erreichbar
**Voraussetzung:** Server gestoppt  
**Erwartetes Ergebnis:** 5× kurzer Piep (600 Hz)

---

### T2.5 WiFi-Reset (langer Druck)
**Schritte:**
1. Button 5 Sekunden gedrückt halten (im Ruhezustand)

**Erwartetes Ergebnis:**
- 2× langer Piep (1000 Hz)
- Atom startet neu
- Hotspot `Atom-Transcription-Setup` öffnet sich

---

## T3 – Firmware: Button & Pieptöne

### T3.1 Start-Piep
**Schritte:** Kurz drücken (Ruhezustand, Server erreichbar)  
**Erwartetes Ergebnis:** 1× Piep 1000 Hz, 100 ms

### T3.2 Stopp-Piep
**Schritte:** Erneut kurz drücken (während Aufnahme)  
**Erwartetes Ergebnis:** 3× Piep 1000 Hz, 80 ms Ton / 60 ms Pause

### T3.3 Abbruch (langer Druck)
**Schritte:** Während Aufnahme Button 3 Sekunden halten  
**Erwartetes Ergebnis:**
- Kein Piep
- Zurück in Ruhezustand
- Server erhält **kein** close-Frame, verwirft Puffer

### T3.4 Entprellung
**Schritte:** Button schnell mehrfach antippen  
**Erwartetes Ergebnis:** Keine Doppelauslösung, Zustandswechsel nur einmal

---

## T4 – Firmware: Audio & WebSocket

### T4.1 WebSocket-Verbindungsaufbau
**Voraussetzung:** Server läuft  
**Schritte:** Kurz drücken  
**Erwartetes Ergebnis:**
- Serieller Monitor: `[WS] Verbunden`
- Server-Log: `[SESSION] Neue Verbindung: YYYY_MM_DD_HH_MM`

---

### T4.2 Audiodaten ankommen
**Schritte:**
1. Aufnahme starten, 10 Sekunden sprechen, stoppen
2. Erzeugte WAV-Datei auf dem Server prüfen

**Erwartetes Ergebnis:**
- WAV-Datei in `output/` vorhanden
- Dauer ~10 s (`ffprobe YYYY_..._audio.wav`)
- Audio hörbar und verständlich (`aplay` oder Media Player)

---

### T4.3 Chunk-Größe
**Schritte:** Im Seriellen Monitor Chunk-Logs prüfen  
**Erwartetes Ergebnis:** Chunks à 1024 Bytes (512 Samples × 2 Bytes)

---

### T4.4 Sauberer Verbindungsschluss
**Schritte:** Aufnahme normal beenden  
**Erwartetes Ergebnis:**
- Server-Log: Session wird in Queue eingereiht (close-Frame empfangen)
- Kein `[SESSION] abgebrochen`

---

## T5 – Firmware: WLAN-Abbruch & Reconnect

### T5.1 Reconnect nach kurzer Unterbrechung
**Schritte:**
1. Aufnahme starten, sprechen
2. WLAN-Router kurz aus- und einschalten (~10 s)
3. Aufnahme weitersprechen, normal stoppen

**Erwartetes Ergebnis:**
- Serieller Monitor: `[RECONNECT] Versuch 1/30` erscheint
- Nach Reconnect: `[RECONNECT] Wiederverbunden`
- WAV auf dem Server enthält Audiodaten aus beiden Phasen (PSRAM-Puffer wurde nachgesendet)

---

### T5.2 PSRAM-Puffer läuft nicht voll
**Schritte:** Aufnahme starten, WLAN 30 Sekunden trennen  
**Erwartetes Ergebnis:**
- Serieller Monitor zeigt keine `Ringpuffer voll`-Meldung
- Nach Reconnect: vollständige Aufnahme

---

### T5.3 Reconnect-Timeout
**Schritte:**
1. Aufnahme starten
2. WLAN dauerhaft trennen (Router aus)
3. 60 Sekunden warten

**Erwartetes Ergebnis:**
- Nach 30 Versuchen: 5× Fehler-Piep (600 Hz)
- Zurück in Ruhezustand
- Kein Absturz

---

## T6 – Server: Pipeline-Qualität

### T6.1 Whisper – Spracherkennung
**Testdatei:** Klare deutsche Aufnahme, 30 Sekunden  
**Erwartetes Ergebnis:**
- `_transcript.md` enthält lesbaren deutschen Text
- Log zeigt: `Erkannte Sprache: de`
- Wortfehlerrate subjektiv < 10 %

---

### T6.2 Whisper – Spracherkennung Englisch
**Testdatei:** Klare englische Aufnahme, 30 Sekunden  
**Erwartetes Ergebnis:**
- Log zeigt: `Erkannte Sprache: en`
- Korrekte englische Transkription

---

### T6.3 Diarization – 2 Sprecher
**Testdatei:** Aufnahme mit 2 deutlich unterschiedlichen Stimmen  
**Erwartetes Ergebnis:**
- `_transcript.md` enthält `SPRECHER_A` und `SPRECHER_B`
- Sprecherwechsel erfolgen an plausiblen Stellen

---

### T6.4 Zusammenfassung
**Erwartetes Ergebnis:**
- `_summary.md` enthält alle drei Abschnitte: Kernaussagen, Entscheidungen, Offene Punkte
- Sprache und Sprecheranzahl im Header korrekt

---

### T6.5 Fehlerbehandlung – Ollama nicht erreichbar
**Schritte:** Ollama stoppen, Aufnahme verarbeiten lassen  
**Erwartetes Ergebnis:**
- `error.log` enthält Eintrag mit Fehlermeldung
- `_transcript.md` ist trotzdem geschrieben
- Telegram erhält Fehlernachricht: `❌ Fehler bei Job ...`
- Server läuft weiter (kein Absturz)

---

## T7 – End-to-End

### T7.1 Vollständiger Durchlauf
**Schritte:**
1. Atom und Server starten
2. 2-minütiges Gespräch mit 2 Personen aufnehmen
3. Aufnahme stoppen

**Erwartetes Ergebnis:**
- Atom: 3× Stopp-Piep
- Server verarbeitet automatisch
- In `output/` entstehen: `_transcript.md`, `_summary.md`, `_audio.ogg`
- Telegram-Nachricht kommt an
- WAV-Datei ist gelöscht

---

### T7.2 Mehrere Aufnahmen in Folge
**Schritte:** 3 Aufnahmen direkt hintereinander starten  
**Erwartetes Ergebnis:**
- Alle 3 Jobs werden sequenziell verarbeitet (keine parallelen Läufe)
- Alle 3 Telegram-Nachrichten kommen an
- Keine Ressourcenkonflikte (Speicher, CPU)

---

### T7.3 Lange Aufnahme (10 Minuten)
**Erwartetes Ergebnis:**
- Kein PSRAM-Overflow
- WAV-Datei ~19 MB, OGG ~2,4 MB
- Vollständige Transkription ohne Abbruch

---

## Checkliste vor Inbetriebnahme

- [ ] `config.py`: `PYANNOTE_TOKEN`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID` eingetragen
- [ ] HuggingFace-Modell-Lizenz akzeptiert (`pyannote/speaker-diarization-3.1`)
- [ ] Ollama läuft: `ollama list` zeigt `mistral`
- [ ] `ffmpeg` installiert: `ffmpeg -version`
- [ ] Server hat feste IP (DHCP-Reservierung im Router)
- [ ] Port 8765 im lokalen Netz erreichbar
- [ ] Atom und Server im gleichen WLAN
- [ ] Erster Boot: WiFi-Portal ausgefüllt, Server-IP eingetragen
- [ ] T2.3 bestanden: 2× Piep beim Boot
