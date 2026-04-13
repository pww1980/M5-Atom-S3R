/**
 * Atom Echo S3R – Transkriptions-Pipeline Firmware
 *
 * Hardware:  M5Stack Atom VoiceS3R / Atom EchoS3R
 * MCU:       ESP32-S3
 * Codec:     ES8311  (via M5Unified)
 * Button:    GPIO 41
 * LED:       keine
 *
 * WiFi-Einrichtung: Beim ersten Start (oder nach Reset) öffnet das Gerät
 * einen WLAN-Hotspot "Atom-Transcription-Setup". Dort im Browser unter
 * 192.168.4.1 SSID, Passwort und Server-IP eintragen.
 *
 * Zustände: IDLE → RECORDING → PROCESSING → DONE / ERROR / RECONNECTING
 */

#include <M5Unified.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <esp_psram.h>

// =============================================================================
// Konstanten
// =============================================================================
#define SERVER_PORT                 8765

#define AUDIO_SAMPLE_RATE           16000
#define AUDIO_BUF_SAMPLES           512
#define AUDIO_BUF_BYTES             (AUDIO_BUF_SAMPLES * 2)

#define BUTTON_PIN                  41
#define BUTTON_DEBOUNCE_MS          50
#define LONG_PRESS_MS               3000
#define PORTAL_RESET_HOLD_MS        5000   // 5s Druck im IDLE → WiFi-Reset

#define PSRAM_BUFFER_MAX_BYTES      (4 * 1024 * 1024)
#define WIFI_RECONNECT_INTERVAL_MS  2000
#define WIFI_RECONNECT_MAX_ATTEMPTS 30

// =============================================================================
// Globaler Zustand
// =============================================================================
enum State { IDLE, RECORDING, RECONNECTING, PROCESSING, DONE, ERROR_STATE };
State currentState = IDLE;

char serverIP[40] = "192.168.x.x";   // wird aus NVS geladen oder per Portal gesetzt

Preferences prefs;

// =============================================================================
// Pieptöne
// =============================================================================
void beepPattern(uint16_t freqHz, uint32_t tonMs, uint32_t pauseMs, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        M5.Speaker.tone(freqHz, tonMs);
        delay(tonMs);
        M5.Speaker.stop();
        if (i < count - 1) delay(pauseMs);
    }
}

// =============================================================================
// WiFi-Setup (WiFiManager + benutzerdefinierter Parameter für Server-IP)
// =============================================================================
WiFiManager       wm;
char              savedIP[40];
bool              shouldSaveParams = false;
WiFiManagerParameter* paramServerIP = nullptr;

void saveParamsCallback() { shouldSaveParams = true; }

void setupWiFi(bool forcePortal = false) {
    // Server-IP aus NVS laden
    prefs.begin("atom", true);
    String ip = prefs.getString("server_ip", "192.168.x.x");
    prefs.end();
    ip.toCharArray(savedIP, sizeof(savedIP));

    // Eigenen Parameter für Server-IP zur Portal-Seite hinzufügen
    delete paramServerIP;
    paramServerIP = new WiFiManagerParameter("server_ip", "Server-IP (N100)", savedIP, 39);
    wm.addParameter(paramServerIP);
    wm.setSaveParamsCallback(saveParamsCallback);

    // Hotspot-Name und Timeout
    wm.setAPName("Atom-Transcription-Setup");
    wm.setConfigPortalTimeout(180);   // 3 Minuten, dann Neustart
    wm.setConnectTimeout(15);

    bool ok;
    if (forcePortal) {
        Serial.println("[WIFI] Portal manuell geöffnet");
        ok = wm.startConfigPortal("Atom-Transcription-Setup");
    } else {
        ok = wm.autoConnect("Atom-Transcription-Setup");
    }

    if (!ok) {
        Serial.println("[WIFI] Verbindung fehlgeschlagen – Neustart");
        beepPattern(600, 80, 80, 5);
        delay(1000);
        ESP.restart();
    }

    // Gespeicherte Parameter übernehmen
    if (shouldSaveParams) {
        const char* val = paramServerIP->getValue();
        prefs.begin("atom", false);
        prefs.putString("server_ip", val);
        prefs.end();
        strncpy(serverIP, val, sizeof(serverIP));
        Serial.printf("[WIFI] Server-IP gespeichert: %s\n", serverIP);
    } else {
        strncpy(serverIP, savedIP, sizeof(serverIP));
    }

    Serial.printf("[WIFI] Verbunden. Gerät-IP: %s  Server: %s\n",
                  WiFi.localIP().toString().c_str(), serverIP);
}

// =============================================================================
// Server-Erreichbarkeitstest (TCP-only)
// =============================================================================
bool serverReachable() {
    WiFiClient c;
    bool ok = c.connect(serverIP, SERVER_PORT, 2000);
    c.stop();
    return ok;
}

// =============================================================================
// PSRAM-Ringpuffer
// =============================================================================
uint8_t* psramBuf  = nullptr;
uint32_t psramHead = 0;
uint32_t psramTail = 0;
uint32_t psramUsed = 0;

void psramBufInit() {
    if (!psramFound()) { Serial.println("[PSRAM] Nicht verfügbar"); return; }
    psramBuf = (uint8_t*)ps_malloc(PSRAM_BUFFER_MAX_BYTES);
    if (psramBuf) Serial.printf("[PSRAM] %d MB reserviert\n",
                                 PSRAM_BUFFER_MAX_BYTES / (1024 * 1024));
    else          Serial.println("[PSRAM] Allokierung fehlgeschlagen");
}

void psramBufReset() { psramHead = psramTail = psramUsed = 0; }

void psramBufWrite(const uint8_t* data, uint32_t len) {
    if (!psramBuf) return;
    for (uint32_t i = 0; i < len; i++) {
        if (psramUsed >= PSRAM_BUFFER_MAX_BYTES) {
            psramTail = (psramTail + 1) % PSRAM_BUFFER_MAX_BYTES;
            psramUsed--;
        }
        psramBuf[psramHead] = data[i];
        psramHead = (psramHead + 1) % PSRAM_BUFFER_MAX_BYTES;
        psramUsed++;
    }
}

uint32_t psramBufRead(uint8_t* out, uint32_t maxLen) {
    if (!psramBuf) return 0;
    uint32_t n = (psramUsed < maxLen) ? psramUsed : maxLen;
    for (uint32_t i = 0; i < n; i++) {
        out[i]    = psramBuf[psramTail];
        psramTail = (psramTail + 1) % PSRAM_BUFFER_MAX_BYTES;
    }
    psramUsed -= n;
    return n;
}

// =============================================================================
// WebSocket
// =============================================================================
WebSocketsClient ws;
bool wsConnected   = false;
bool wsAckReceived = false;

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsConnected = true;
            Serial.println("[WS] Verbunden");
            break;
        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("[WS] Getrennt");
            if (currentState == RECORDING) currentState = RECONNECTING;
            break;
        case WStype_TEXT:
            if (length >= 3 && strncmp((char*)payload, "ACK", 3) == 0)
                wsAckReceived = true;
            break;
        default: break;
    }
}

bool wsConnect() {
    ws.begin(serverIP, SERVER_PORT, "/");
    ws.onEvent(wsEvent);
    ws.setReconnectInterval(0);
    unsigned long t = millis();
    while (!wsConnected && millis() - t < 3000) { ws.loop(); delay(10); }
    return wsConnected;
}

// =============================================================================
// Reconnect-Logik
// =============================================================================
int           reconnectAttempts   = 0;
unsigned long lastReconnectMillis = 0;
int16_t       audioBuf[AUDIO_BUF_SAMPLES];

void handleReconnecting() {
    // Aufnahme läuft weiter, Chunks in PSRAM puffern
    if (M5.Mic.record(audioBuf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false))
        psramBufWrite((uint8_t*)audioBuf, AUDIO_BUF_BYTES);

    unsigned long now = millis();
    if (now - lastReconnectMillis < WIFI_RECONNECT_INTERVAL_MS) return;
    lastReconnectMillis = now;
    reconnectAttempts++;
    Serial.printf("[RECONNECT] Versuch %d/%d\n", reconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS);

    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

    if (WiFi.status() == WL_CONNECTED && wsConnect()) {
        // PSRAM-Puffer senden, dann live weiterstreamen
        uint8_t tmp[AUDIO_BUF_BYTES];
        uint32_t n;
        while ((n = psramBufRead(tmp, AUDIO_BUF_BYTES)) > 0) {
            ws.sendBIN(tmp, n);
            ws.loop();
        }
        currentState      = RECORDING;
        reconnectAttempts = 0;
        Serial.println("[RECONNECT] Wiederverbunden");
        return;
    }

    if (reconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
        Serial.println("[RECONNECT] Timeout – Aufnahme abgebrochen");
        M5.Mic.end();
        M5.Speaker.begin();
        beepPattern(600, 80, 80, 5);
        currentState      = IDLE;
        reconnectAttempts = 0;
    }
}

// =============================================================================
// Setup
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(300);

    // M5Unified (verwaltet ES8311, I2S, I2C intern)
    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Speaker.setVolume(200);
    M5.Speaker.begin();

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    psramBufInit();

    // WiFi + Server-IP konfigurieren (blockiert bis verbunden)
    setupWiFi();

    // Server-Erreichbarkeit prüfen
    if (serverReachable()) {
        Serial.println("[BOOT] Server erreichbar");
        beepPattern(1000, 80, 80, 2);   // 2× kurz → OK
    } else {
        Serial.println("[BOOT] Server NICHT erreichbar");
        beepPattern(600, 80, 80, 5);    // 5× kurz → Fehler
    }

    currentState = IDLE;
    Serial.println("[BOOT] Bereit");
}

// =============================================================================
// Loop
// =============================================================================
bool          lastButtonState  = HIGH;
unsigned long pressStartMillis = 0;
bool          longPressHandled = false;

void startMic() {
    M5.Speaker.end();
    auto mic_cfg            = M5.Mic.config();
    mic_cfg.sample_rate     = AUDIO_SAMPLE_RATE;
    mic_cfg.stereo          = false;
    mic_cfg.dma_buf_count   = 8;
    mic_cfg.dma_buf_len     = AUDIO_BUF_SAMPLES;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();
}

void stopMic() {
    M5.Mic.end();
    M5.Speaker.begin();
}

void loop() {
    M5.update();
    ws.loop();

    bool btn = digitalRead(BUTTON_PIN);
    unsigned long now = millis();

    // Flanke gedrückt
    if (btn == LOW && lastButtonState == HIGH) {
        delay(BUTTON_DEBOUNCE_MS);
        if (digitalRead(BUTTON_PIN) == LOW) {
            pressStartMillis = millis();
            longPressHandled = false;
        }
    }

    // Langer Druck im IDLE (≥5s) → WiFi-Portal zurücksetzen
    if (btn == LOW && !longPressHandled && currentState == IDLE) {
        if (millis() - pressStartMillis >= PORTAL_RESET_HOLD_MS) {
            longPressHandled = true;
            Serial.println("[BTN] WiFi-Reset – Portal öffnet sich");
            beepPattern(1000, 200, 100, 2);
            wm.resetSettings();
            delay(500);
            ESP.restart();
        }
    }

    // Langer Druck im RECORDING (≥3s) → Aufnahme abbrechen
    if (btn == LOW && !longPressHandled && currentState == RECORDING) {
        if (millis() - pressStartMillis >= LONG_PRESS_MS) {
            longPressHandled = true;
            Serial.println("[BTN] Langer Druck – Aufnahme abgebrochen");
            stopMic();
            ws.disconnect();
            currentState = IDLE;
        }
    }

    // Kurzer Druck (Loslassen)
    if (btn == HIGH && lastButtonState == LOW && !longPressHandled) {

        if (currentState == IDLE) {
            Serial.println("[BTN] Start Aufnahme");
            // Erst WS verbinden, dann piepen (Speaker läuft noch), dann Mic starten
            if (wsConnect()) {
                beepPattern(1000, 100, 0, 1);   // 1× 1000Hz 100ms → Aufnahme startet
                startMic();                      // Speaker.end() + Mic.begin()
                psramBufReset();
                reconnectAttempts   = 0;
                lastReconnectMillis = 0;
                currentState        = RECORDING;
            } else {
                Serial.println("[BTN] WebSocket fehlgeschlagen");
                beepPattern(600, 80, 80, 5);    // 5× 600Hz → Verbindungsfehler
                currentState = IDLE;
            }

        } else if (currentState == RECORDING) {
            Serial.println("[BTN] Stopp Aufnahme");
            if (M5.Mic.record(audioBuf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false) && wsConnected)
                ws.sendBIN((uint8_t*)audioBuf, AUDIO_BUF_BYTES);
            stopMic();
            ws.disconnect();
            delay(100);
            beepPattern(1000, 80, 60, 3);   // 3× kurz → Aufnahme gestoppt
            wsAckReceived = false;
            currentState  = PROCESSING;
        }
    }
    lastButtonState = btn;

    // Audio streamen (RECORDING)
    if (currentState == RECORDING && wsConnected) {
        if (M5.Mic.record(audioBuf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false))
            ws.sendBIN((uint8_t*)audioBuf, AUDIO_BUF_BYTES);
    }

    // Puffern + Reconnect (RECONNECTING)
    if (currentState == RECONNECTING)
        handleReconnecting();

    // Warten auf Server-ACK (PROCESSING)
    // Kein weiterer Piep nötig – die 3× beim Stopp signalisieren bereits Ende der Aufnahme.
    // Der ACK bestätigt nur intern die erfolgreiche Übergabe an den Server.
    if (currentState == PROCESSING && wsAckReceived) {
        Serial.println("[PIPELINE] Server hat Job übernommen");
        currentState = IDLE;
    }
}
