/*
 * main.cpp – M5Stack Cardputer Aufnahmegerät
 *
 * Taste drücken → Aufnahme startet (WAV direkt auf SD-Karte)
 * Taste drücken → Aufnahme stoppt → Upload an Server → Transkription
 * Taste 5s halten (im Leerlauf) → WiFi-Reset
 *
 * Kein Ping-Pong-Buffer, keine Segmente, kein WiFi während der Aufnahme.
 * WAV bleibt als Backup auf der SD-Karte.
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
#define SERVER_PORT        8765
#define HTTP_UPLOAD_PATH   "/upload"

#define AUDIO_SAMPLE_RATE  16000
#define AUDIO_BUF_SAMPLES  512
#define AUDIO_BUF_BYTES    (AUDIO_BUF_SAMPLES * 2)
#define WAV_HEADER_BYTES   44

#define LONG_PRESS_MS      5000    // WiFi-Reset

// Cardputer SD-Karte SPI-Pins (laut M5Stack-Schaltplan)
#define SD_CS    12
#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14

// =============================================================================
// Zustandsmaschine
// =============================================================================
enum State { IDLE, RECORDING, UPLOADING };
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

static int16_t    micBuf[AUDIO_BUF_SAMPLES];
static uint32_t   btnDownAt     = 0;

// =============================================================================
// Display
// =============================================================================
void disp(const char* line1, const char* line2 = nullptr) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(4, 10);
    M5.Display.print(line1);
    if (line2) {
        M5.Display.setTextSize(1);
        M5.Display.setCursor(4, 48);
        M5.Display.print(line2);
    }
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
    struct __attribute__((packed)) WavHdr {
        char     riff[4]       = {'R','I','F','F'};
        uint32_t chunkSize;
        char     wave[4]       = {'W','A','V','E'};
        char     fmt[4]        = {'f','m','t',' '};
        uint32_t subchunk1     = 16;
        uint16_t audioFmt      = 1;    // PCM
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

    disp("WiFi...", "");

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
    if (!sdReady) { beep(600, 80, 3); disp("SD fehlt!"); return; }

    snprintf(sessionId, sizeof(sessionId), "sess_%lu", (unsigned long)millis());
    snprintf(wavPath,   sizeof(wavPath),   "/%s.wav",   sessionId);

    wavFile = SD.open(wavPath, FILE_WRITE);
    if (!wavFile) { beep(600, 80, 3); disp("SD Fehler"); return; }

    writeWavHeader(wavFile, 0);   // Platzhalter, wird am Ende aktualisiert
    bytesWritten = 0;

    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = AUDIO_SAMPLE_RATE;
    mic_cfg.stereo      = false;
    M5.Mic.config(mic_cfg);
    M5.Speaker.end();
    M5.Mic.begin();

    sendStatus("recording_start");
    beep(1000, 100);
    disp("REC", sessionId + 5);   // "sess_" überspringen
    currentState = RECORDING;
    Serial.printf("[REC] %s\n", wavPath);
}

// =============================================================================
// Aufnahme stoppen + hochladen
// =============================================================================
void finishRecording() {
    M5.Mic.end();
    delay(150);
    M5.Speaker.begin();

    writeWavHeader(wavFile, bytesWritten);
    wavFile.close();

    float dur = (float)bytesWritten / (AUDIO_SAMPLE_RATE * 2);
    Serial.printf("[REC] Stop: %lu B = %.1f s\n", (unsigned long)bytesWritten, dur);
    sendStatus("recording_stop");

    disp("Uploading...", wavPath + 1);
    currentState = UPLOADING;

    // PCM-Daten aus WAV streamen (Header überspringen)
    File f = SD.open(wavPath);
    bool ok = false;

    if (f && f.size() > WAV_HEADER_BYTES) {
        f.seek(WAV_HEADER_BYTES);
        uint32_t pcmSize = f.size() - WAV_HEADER_BYTES;

        char url[80];
        snprintf(url, sizeof(url), "http://%s:%d%s", serverIP, SERVER_PORT, HTTP_UPLOAD_PATH);

        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type",  "application/octet-stream");
        http.addHeader("X-Session-Id",  sessionId);
        http.addHeader("X-Seq-Num",     "0");
        http.addHeader("X-Final",       "1");
        http.setTimeout(60000);   // 60s für große Dateien

        int code = http.sendRequest("POST", &f, pcmSize);
        http.end();
        f.close();

        ok = (code == 200);
        Serial.printf("[HTTP] %d (%lu KB, %.1fs)\n",
                      code, (unsigned long)(pcmSize / 1024), dur);
    } else {
        if (f) f.close();
    }

    if (ok) {
        beep(1000, 80, 3, 60);
        char msg[32];
        snprintf(msg, sizeof(msg), "Fertig! %.0fs", dur);
        disp(msg, "WAV auf SD gespeichert");
        sendLog("Upload OK");
    } else {
        beep(600, 80, 5);
        disp("Upload fehler", "WAV auf SD gespeichert");
        sendLog("Upload FEHLER – WAV auf SD");
    }

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

    disp("Starte...");

    sdReady = setupSD();
    if (!sdReady) disp("SD fehlt!", "Karte einlegen");

    setupWiFi();

    char logMsg[64];
    snprintf(logMsg, sizeof(logMsg), "Cardputer Boot OK – SD: %s", sdReady ? "ja" : "nein");
    sendLog(logMsg);

    beep(1000, 80, 2);
    disp("Bereit", serverIP);
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
    }

    // --- Button (BtnA / OK-Taste) ---------------------------------------------
    if (M5.BtnA.wasPressed()) btnDownAt = millis();

    if (M5.BtnA.wasReleased()) {
        uint32_t held = millis() - btnDownAt;

        if (held >= LONG_PRESS_MS && currentState == IDLE) {
            disp("WiFi Reset...");
            beep(1000, 200, 2, 100);
            wm.resetSettings();
            delay(500);
            ESP.restart();

        } else if (currentState == IDLE) {
            beginRecording();

        } else if (currentState == RECORDING) {
            finishRecording();
        }
        // UPLOADING: Tastendruck ignorieren
    }
}
