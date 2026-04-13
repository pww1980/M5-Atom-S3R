/*
 * main.cpp – M5Stack Cardputer Aufnahmegerät
 *
 * Ablauf:
 *   Taste drücken  → Aufnahme startet (WAV direkt auf SD-Karte)
 *   Taste drücken  → Aufnahme stoppt  → "Senden?" mit 10s-Countdown
 *   Taste drücken  → Upload an Server → Transkription
 *   Timeout (10s)  → Nur auf SD speichern, kein Upload
 *   Taste 5s halten (im Leerlauf) → WiFi-Reset
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>

// =============================================================================
// Konstanten
// =============================================================================
#define SERVER_PORT         8765
#define HTTP_UPLOAD_PATH    "/upload"

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BUF_SAMPLES   512
#define AUDIO_BUF_BYTES     (AUDIO_BUF_SAMPLES * 2)
#define WAV_HEADER_BYTES    44

#define LONG_PRESS_MS       5000   // WiFi-Reset
#define CONFIRM_TIMEOUT_MS  10000  // Sekunden bis Auto-Abbruch nach Stopp

// Cardputer SD-Karte SPI-Pins
#define SD_CS    12
#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14

// Farben
#define CLR_BG      TFT_BLACK
#define CLR_WHITE   TFT_WHITE
#define CLR_RED     TFT_RED
#define CLR_GREEN   TFT_GREEN
#define CLR_YELLOW  TFT_YELLOW
#define CLR_GRAY    0x7BEF

// =============================================================================
// Zustandsmaschine
// =============================================================================
enum State { IDLE, RECORDING, CONFIRM, UPLOADING };
static State currentState = IDLE;

// =============================================================================
// Globale Variablen
// =============================================================================
static char       serverIP[40]  = "192.168.x.x";
static Preferences prefs;
static WiFiManager wm;
static SPIClass   sdSPI(HSPI);
static bool       sdReady       = false;

static char       wavPath[48]   = "";
static char       sessionId[32] = "";
static File       wavFile;
static uint32_t   bytesWritten  = 0;
static uint32_t   recStartMs    = 0;
static uint32_t   confirmStartMs = 0;

static int16_t    micBuf[AUDIO_BUF_SAMPLES];
static uint32_t   btnDownAt     = 0;

// =============================================================================
// Display-Hilfsfunktionen
// =============================================================================
static uint32_t lastDisplayMs = 0;

void dispIdle() {
    M5.Display.fillScreen(CLR_BG);
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(10, 15);
    M5.Display.print("Bereit");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(CLR_GRAY, CLR_BG);
    M5.Display.setCursor(10, 65);
    M5.Display.printf("Server: %s", serverIP);
    M5.Display.setCursor(10, 80);
    M5.Display.print("SD: ");
    M5.Display.print(sdReady ? "OK" : "fehlt!");
    M5.Display.setTextColor(CLR_GREEN, CLR_BG);
    M5.Display.setCursor(10, 100);
    M5.Display.print("[Taste] Aufnahme starten");
}

void dispRecording(uint32_t elapsedSec) {
    uint32_t mm = elapsedSec / 60;
    uint32_t ss = elapsedSec % 60;
    M5.Display.fillScreen(CLR_BG);
    // Roter REC-Indikator
    M5.Display.fillCircle(20, 20, 10, CLR_RED);
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(36, 12);
    M5.Display.print("REC");
    // Großer Timer
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);
    M5.Display.setCursor(30, 50);
    M5.Display.printf("%02lu:%02lu", mm, ss);
    // Hinweis unten
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(CLR_GRAY, CLR_BG);
    M5.Display.setCursor(10, 118);
    M5.Display.print("[Taste] Aufnahme stoppen");
}

void dispConfirm(uint32_t remainingSec) {
    M5.Display.fillScreen(CLR_BG);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(CLR_YELLOW, CLR_BG);
    M5.Display.setCursor(10, 10);
    M5.Display.print("Senden?");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);
    M5.Display.setCursor(10, 42);
    M5.Display.printf("Datei: %s", wavPath + 1);
    M5.Display.setCursor(10, 58);
    M5.Display.printf("Groesse: %lu KB", (unsigned long)(bytesWritten / 1024));
    // Countdown
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(CLR_YELLOW, CLR_BG);
    M5.Display.setCursor(10, 80);
    M5.Display.printf("Timeout: %lus", (unsigned long)remainingSec);
    // Hinweis
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(CLR_GREEN, CLR_BG);
    M5.Display.setCursor(10, 118);
    M5.Display.print("[Taste]=Senden  [Timeout]=Nur SD");
}

void dispUploading(uint32_t pcmKB) {
    M5.Display.fillScreen(CLR_BG);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);
    M5.Display.setCursor(10, 15);
    M5.Display.print("Uploading...");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(CLR_GRAY, CLR_BG);
    M5.Display.setCursor(10, 50);
    M5.Display.printf("%lu KB -> %s", pcmKB, serverIP);
}

void dispResult(bool ok, float dur) {
    M5.Display.fillScreen(CLR_BG);
    M5.Display.setTextSize(2);
    if (ok) {
        M5.Display.setTextColor(CLR_GREEN, CLR_BG);
        M5.Display.setCursor(10, 15);
        M5.Display.print("Gesendet!");
    } else {
        M5.Display.setTextColor(CLR_RED, CLR_BG);
        M5.Display.setCursor(10, 15);
        M5.Display.print("Fehler!");
    }
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);
    M5.Display.setCursor(10, 50);
    M5.Display.printf("Dauer: %.0f s", dur);
    M5.Display.setCursor(10, 65);
    M5.Display.printf("WAV: %s", wavPath + 1);
    M5.Display.setTextColor(CLR_GRAY, CLR_BG);
    M5.Display.setCursor(10, 95);
    M5.Display.print("Datei auf SD gespeichert");
}

// =============================================================================
// Beep
// =============================================================================
void beep(uint16_t hz, uint32_t ms, uint8_t count = 1, uint32_t pause = 80) {
    for (uint8_t i = 0; i < count; ++i) {
        M5.Speaker.tone(hz, ms);
        delay(ms);
        M5.Speaker.stop();
        if (i + 1 < count) delay(pause);
    }
}

// =============================================================================
// HTTP-Hilfsfunktionen
// =============================================================================
void sendLog(const char* msg) {
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/log", serverIP, SERVER_PORT);
    HTTPClient http;
    http.begin(url);
    http.addHeader("X-Message", msg);
    http.setTimeout(2000);
    http.GET();
    http.end();
}

void sendStatus(const char* event) {
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/status", serverIP, SERVER_PORT);
    HTTPClient http;
    http.begin(url);
    http.addHeader("X-Event",      event);
    http.addHeader("X-Session-Id", sessionId);
    http.setTimeout(3000);
    http.GET();
    http.end();
}

// =============================================================================
// WAV-Header (44 Bytes, little-endian)
// =============================================================================
void writeWavHeader(File& f, uint32_t pcmBytes) {
    struct __attribute__((packed)) {
        char     riff[4]       = {'R','I','F','F'};
        uint32_t chunkSize;
        char     wave[4]       = {'W','A','V','E'};
        char     fmt[4]        = {'f','m','t',' '};
        uint32_t subchunk1     = 16;
        uint16_t audioFmt      = 1;
        uint16_t channels      = 1;
        uint32_t sampleRate    = AUDIO_SAMPLE_RATE;
        uint32_t byteRate      = AUDIO_SAMPLE_RATE * 2;
        uint16_t blockAlign    = 2;
        uint16_t bitsPerSample = 16;
        char     data[4]       = {'d','a','t','a'};
        uint32_t dataSize;
    } hdr;
    hdr.chunkSize = 36 + pcmBytes;
    hdr.dataSize  = pcmBytes;
    f.seek(0);
    f.write((uint8_t*)&hdr, sizeof(hdr));
}

// =============================================================================
// WiFi-Setup
// =============================================================================
static bool shouldSaveParams         = false;
static WiFiManagerParameter* paramIP = nullptr;

void saveParamsCallback() { shouldSaveParams = true; }

void setupWiFi(bool forcePortal = false) {
    shouldSaveParams = false;
    prefs.begin("atom", true);
    String ip = prefs.getString("server_ip", "192.168.x.x");
    prefs.end();
    ip.toCharArray(serverIP, sizeof(serverIP));

    delete paramIP;
    paramIP = new WiFiManagerParameter("server_ip", "Server-IP", serverIP, 39);
    wm.addParameter(paramIP);
    wm.setSaveParamsCallback(saveParamsCallback);
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(15);

    M5.Display.fillScreen(CLR_BG);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);
    M5.Display.setCursor(10, 30);
    M5.Display.print("WiFi...");

    bool ok = forcePortal
        ? wm.startConfigPortal("Cardputer-Transcription")
        : wm.autoConnect("Cardputer-Transcription");

    if (!ok) { beep(600, 80, 5); delay(1000); ESP.restart(); }

    if (shouldSaveParams) {
        const char* val = paramIP->getValue();
        prefs.begin("atom", false);
        prefs.putString("server_ip", val);
        prefs.end();
        strncpy(serverIP, val, sizeof(serverIP));
        serverIP[sizeof(serverIP) - 1] = '\0';
    }
    Serial.printf("[WiFi] %s  Server: %s\n",
                  WiFi.localIP().toString().c_str(), serverIP);
}

// =============================================================================
// SD-Init
// =============================================================================
bool setupSD() {
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI)) {
        Serial.println("[SD] Init fehlgeschlagen");
        return false;
    }
    uint64_t freeMB = (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024);
    Serial.printf("[SD] OK – frei: %llu MB\n", freeMB);
    return true;
}

// =============================================================================
// Aufnahme starten
// =============================================================================
void beginRecording() {
    if (!sdReady) {
        beep(600, 80, 3);
        M5.Display.fillScreen(CLR_BG);
        M5.Display.setTextColor(CLR_RED, CLR_BG);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(10, 30);
        M5.Display.print("SD fehlt!");
        return;
    }

    snprintf(sessionId, sizeof(sessionId), "sess_%lu", (unsigned long)millis());
    snprintf(wavPath,   sizeof(wavPath),   "/%s.wav",   sessionId);

    wavFile = SD.open(wavPath, FILE_WRITE);
    if (!wavFile) {
        beep(600, 80, 3);
        M5.Display.fillScreen(CLR_BG);
        M5.Display.setTextColor(CLR_RED, CLR_BG);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(10, 30);
        M5.Display.print("SD Fehler");
        return;
    }

    writeWavHeader(wavFile, 0);
    bytesWritten = 0;
    recStartMs   = millis();

    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = AUDIO_SAMPLE_RATE;
    mic_cfg.stereo      = false;
    M5.Mic.config(mic_cfg);
    M5.Speaker.end();
    M5.Mic.begin();

    sendStatus("recording_start");
    beep(1000, 100);
    dispRecording(0);
    lastDisplayMs = millis();
    currentState  = RECORDING;
    Serial.printf("[REC] Start: %s\n", wavPath);
}

// =============================================================================
// Aufnahme stoppen (→ CONFIRM)
// =============================================================================
void stopRecording() {
    M5.Mic.end();
    delay(150);
    M5.Speaker.begin();

    writeWavHeader(wavFile, bytesWritten);
    wavFile.close();

    float dur = (float)bytesWritten / (AUDIO_SAMPLE_RATE * 2);
    Serial.printf("[REC] Stop: %lu B = %.1f s\n", (unsigned long)bytesWritten, dur);
    sendStatus("recording_stop");

    beep(1000, 80, 2, 80);
    confirmStartMs = millis();
    dispConfirm(CONFIRM_TIMEOUT_MS / 1000);
    lastDisplayMs = millis();
    currentState  = CONFIRM;
}

// =============================================================================
// Upload durchführen
// =============================================================================
void doUpload() {
    File f = SD.open(wavPath);
    if (!f || f.size() <= WAV_HEADER_BYTES) {
        if (f) f.close();
        beep(600, 80, 5);
        return;
    }

    f.seek(WAV_HEADER_BYTES);
    uint32_t pcmSize = f.size() - WAV_HEADER_BYTES;
    float    dur     = (float)pcmSize / (AUDIO_SAMPLE_RATE * 2);

    dispUploading(pcmSize / 1024);

    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d%s", serverIP, SERVER_PORT, HTTP_UPLOAD_PATH);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type",  "application/octet-stream");
    http.addHeader("X-Session-Id",  sessionId);
    http.addHeader("X-Seq-Num",     "0");
    http.addHeader("X-Final",       "1");
    http.setTimeout(60000);

    int code = http.sendRequest("POST", &f, pcmSize);
    http.end();
    f.close();

    bool ok = (code == 200);
    Serial.printf("[HTTP] %d (%lu KB, %.1fs)\n", code, (unsigned long)(pcmSize / 1024), dur);

    if (ok) {
        beep(1000, 80, 3, 60);
        sendLog("Upload OK");
    } else {
        beep(600, 80, 5);
        sendLog("Upload FEHLER");
    }

    dispResult(ok, dur);
    delay(2000);
    dispIdle();
    currentState = IDLE;
}

// =============================================================================
// Setup
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);

    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Speaker.setVolume(200);
    M5.Speaker.begin();
    M5.Display.setTextColor(CLR_WHITE, CLR_BG);

    M5.Display.fillScreen(CLR_BG);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 30);
    M5.Display.print("Starte...");

    sdReady = setupSD();
    setupWiFi();

    char logMsg[64];
    snprintf(logMsg, sizeof(logMsg), "Cardputer Boot OK – SD: %s  WiFi: %s",
             sdReady ? "ja" : "nein",
             WiFi.localIP().toString().c_str());
    sendLog(logMsg);

    beep(1000, 80, 2);
    dispIdle();
    Serial.println("[BOOT] Bereit");
}

// =============================================================================
// Loop
// =============================================================================
void loop() {
    M5.update();

    // --- Mikrofon → SD (RECORDING) --------------------------------------------
    if (currentState == RECORDING) {
        M5.Mic.record(micBuf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false);
        wavFile.write((uint8_t*)micBuf, AUDIO_BUF_BYTES);
        bytesWritten += AUDIO_BUF_BYTES;

        // Display-Update jede Sekunde
        uint32_t now = millis();
        if (now - lastDisplayMs >= 1000) {
            dispRecording((now - recStartMs) / 1000);
            lastDisplayMs = now;
        }
    }

    // --- Sende-Bestätigung (CONFIRM) ------------------------------------------
    if (currentState == CONFIRM) {
        uint32_t elapsed   = millis() - confirmStartMs;
        uint32_t remaining = (elapsed < CONFIRM_TIMEOUT_MS)
                             ? (CONFIRM_TIMEOUT_MS - elapsed + 999) / 1000 : 0;

        // Countdown auf Display aktualisieren
        if (millis() - lastDisplayMs >= 500) {
            dispConfirm(remaining);
            lastDisplayMs = millis();
        }

        // Timeout → Nur SD, kein Upload
        if (elapsed >= CONFIRM_TIMEOUT_MS) {
            beep(600, 80, 1);
            float dur = (float)bytesWritten / (AUDIO_SAMPLE_RATE * 2);
            dispResult(false, dur);
            M5.Display.setTextColor(CLR_WHITE, CLR_BG);
            M5.Display.setCursor(10, 15);
            M5.Display.setTextSize(2);
            M5.Display.print("Nur SD");
            sendLog("Aufnahme nur auf SD gespeichert");
            delay(2000);
            dispIdle();
            currentState = IDLE;
            return;
        }
    }

    // --- Button (BtnA / OK-Taste) ---------------------------------------------
    if (M5.BtnA.wasPressed()) btnDownAt = millis();

    if (M5.BtnA.wasReleased()) {
        uint32_t held = millis() - btnDownAt;

        if (held >= LONG_PRESS_MS && currentState == IDLE) {
            M5.Display.fillScreen(CLR_BG);
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(CLR_YELLOW, CLR_BG);
            M5.Display.setCursor(10, 30);
            M5.Display.print("WiFi Reset...");
            beep(1000, 200, 2, 100);
            wm.resetSettings();
            delay(500);
            ESP.restart();

        } else if (currentState == IDLE) {
            beginRecording();

        } else if (currentState == RECORDING) {
            stopRecording();

        } else if (currentState == CONFIRM) {
            doUpload();
        }
        // UPLOADING: ignorieren
    }
}
