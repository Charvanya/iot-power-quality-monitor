// ============================================================
//  ESP32-2  —  RELAY CONTROL NODE
//  IoT-Based Real-Time Power Quality Monitoring System
//
//  Receives P1, P2, SPIKE from the Sensor Node over UART.
//  Runs 6 selectable load-control modes and enforces a
//  spike-protection SAFE MODE that overrides everything
//  else the instant a spike is detected.
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>

// ================= RELAYS =================
int relayPins[] = {5, 19};   // Relay1 = Load1 (GPIO5), Relay2 = Load2 (GPIO19)

// ================= UART (from Sensor Node) =================
HardwareSerial SerialUART(2);
// Relay Node UART RX = GPIO16 <- Sensor Node UART TX (GPIO17)
// Relay Node UART TX = GPIO17 -> Sensor Node UART RX (GPIO16)

// ================= WIFI =================
// TODO: set your own WiFi credentials before flashing
const char* SSID     = "YOUR_WIFI_SSID";
const char* PASSWORD = "YOUR_WIFI_PASSWORD";

// ================= MQTT =================
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_CLIENT = "Relay_Control_Node_01";

// ================= GLOBAL STATE =================
bool spikeDetected = false;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ============================================================
//  RELAY HELPERS
// ============================================================
void relayON(int r)  { digitalWrite(relayPins[r], HIGH); }
void relayOFF(int r) { digitalWrite(relayPins[r], LOW);  }

// ============================================================
//  CHECK INCOMING UART FOR A SPIKE FLAG
//  Expected format from Sensor Node: P1,P2,SPIKE
// ============================================================
void checkForSpike() {
    if (SerialUART.available()) {
        String data = SerialUART.readStringUntil('\n');
        data.trim();
        Serial.println("UART RECEIVED -> " + data);

        int c1 = data.indexOf(',');
        int c2 = data.indexOf(',', c1 + 1);

        if (c2 > 0) {
            int spike = data.substring(c2 + 1).toInt();
            if (spike == 1) spikeDetected = true;
        }
    }
}

// ============================================================
//  SPIKE PROTECTION — HIGHEST PRIORITY IN THE SYSTEM
//  Immediately cuts both relays and holds in SAFE MODE
//  until the Sensor Node reports the spike has cleared.
// ============================================================
void emergencyShutdown() {
    Serial.println("\n[!] SPIKE DETECTED -> SYSTEM SHUTDOWN");

    relayOFF(0);
    relayOFF(1);

    while (true) {
        Serial.println("SAFE MODE: waiting for spike to clear...");

        if (SerialUART.available()) {
            String data = SerialUART.readStringUntil('\n');
            data.trim();
            Serial.println("UART CHECK -> " + data);

            int c1 = data.indexOf(',');
            int c2 = data.indexOf(',', c1 + 1);

            if (c2 > 0) {
                int spike = data.substring(c2 + 1).toInt();
                if (spike == 0) {
                    Serial.println("[OK] SPIKE CLEARED -> SYSTEM RECOVERING");
                    spikeDetected = false;
                    return;   // exit safe mode, resume normal operation
                }
            }
        }
        delay(500);
    }
}

// ============================================================
//  WIFI / MQTT
// ============================================================
void connectWiFi() {
    WiFi.begin(SSID, PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected");
}

void connectMQTT() {
    while (!mqttClient.connected()) {
        if (mqttClient.connect(MQTT_CLIENT)) {
            Serial.println("MQTT Connected");
        } else {
            delay(2000);
        }
    }
}

// ============================================================
//  NON-BLOCKING SERIAL INT READ (for mode selection menu)
// ============================================================
int readIntNonBlocking() {
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() > 0) return input.toInt();
    }
    return -1;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);

    for (int i = 0; i < 2; i++) {
        pinMode(relayPins[i], OUTPUT);
        relayOFF(i);
    }

    SerialUART.begin(115200, SERIAL_8N1, 16, 17);   // RX=16, TX=17

    connectWiFi();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    connectMQTT();

    Serial.println("System Started -> NORMAL MODE");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();

    // Spike check always runs first — highest priority
    checkForSpike();
    if (spikeDetected) emergencyShutdown();

    Serial.println("\n========= NORMAL MODE =========");
    Serial.println("Select Mode:");
    Serial.println("1 -> Manual");
    Serial.println("2 -> Auto");
    Serial.println("3 -> Priority");
    Serial.println("4 -> Break");
    Serial.println("5 -> IoT Control");
    Serial.println("6 -> Peak Load");
    Serial.println("================================");

    checkForSpike();

    int method = readIntNonBlocking();
    if (method == -1) {
        delay(1000);
        return;
    }

    // ===== 1: MANUAL — both loads ON for a user-given time =====
    if (method == 1) {
        Serial.println("\n=== MANUAL MODE ===");
        while (!Serial.available());
        int t = Serial.parseInt();

        relayON(0);
        relayON(1);

        unsigned long start = millis();
        while (millis() - start < (unsigned long)t * 1000) {
            checkForSpike();
            if (spikeDetected) emergencyShutdown();
            delay(10);
        }
        relayOFF(0);
        relayOFF(1);
    }

    // ===== 2: AUTO — 15s cycles, user confirms to continue =====
    else if (method == 2) {
        relayON(0);
        relayON(1);
        bool run = true;

        while (run) {
            unsigned long start = millis();
            while (millis() - start < 15000) {
                checkForSpike();
                if (spikeDetected) emergencyShutdown();
                delay(10);
            }
            Serial.println("Continue? (1 = YES, 0 = NO)");
            while (!Serial.available());
            int choice = Serial.parseInt();
            if (choice != 1) run = false;
        }
        relayOFF(0);
        relayOFF(1);
    }

    // ===== 3: PRIORITY — run loads sequentially in chosen order =====
    else if (method == 3) {
        while (!Serial.available()); int first  = Serial.parseInt();
        while (!Serial.available()); int second = Serial.parseInt();
        while (!Serial.available()); int t1     = Serial.parseInt();
        while (!Serial.available()); int t2     = Serial.parseInt();
        first--; second--;

        relayON(first);
        unsigned long start1 = millis();
        while (millis() - start1 < (unsigned long)t1 * 1000) {
            checkForSpike();
            if (spikeDetected) emergencyShutdown();
            delay(10);
        }
        relayOFF(first);

        relayON(second);
        unsigned long start2 = millis();
        while (millis() - start2 < (unsigned long)t2 * 1000) {
            checkForSpike();
            if (spikeDetected) emergencyShutdown();
            delay(10);
        }
        relayOFF(second);
    }

    // ===== 4: BREAK — run, pause, resume =====
    else if (method == 4) {
        while (!Serial.available());
        int t = Serial.parseInt();
        int half = t / 2;

        relayON(0);
        relayON(1);
        unsigned long start1 = millis();
        while (millis() - start1 < (unsigned long)half * 1000) {
            checkForSpike();
            if (spikeDetected) emergencyShutdown();
            delay(10);
        }
        relayOFF(0);
        relayOFF(1);

        delay(5000);   // break interval

        relayON(0);
        relayON(1);
        unsigned long start2 = millis();
        while (millis() - start2 < (unsigned long)half * 1000) {
            checkForSpike();
            if (spikeDetected) emergencyShutdown();
            delay(10);
        }
        relayOFF(0);
        relayOFF(1);
    }

    // ===== 5: IOT CONTROL — direct relay ON/OFF via commands =====
    else if (method == 5) {
        bool run = true;
        while (run) {
            int cmd = readIntNonBlocking();
            checkForSpike();
            if (spikeDetected) emergencyShutdown();

            if      (cmd == 1) relayON(0);
            else if (cmd == 2) relayOFF(0);
            else if (cmd == 3) relayON(1);
            else if (cmd == 4) relayOFF(1);
            else if (cmd == 0) run = false;
        }
    }

    // ===== 6: PEAK LOAD — cut the heavier load if over threshold =====
    else if (method == 6) {
        while (!Serial.available());
        float Pth = Serial.parseFloat();

        while (!SerialUART.available());
        String data = SerialUART.readStringUntil('\n');
        int idx = data.indexOf(',');
        float P1 = data.substring(0, idx).toFloat();
        float P2 = data.substring(idx + 1).toFloat();

        float total = P1 + P2;
        if (total > Pth) {
            if (P1 > P2) { relayOFF(0); relayON(1); }
            else         { relayOFF(1); relayON(0); }
        } else {
            relayON(0);
            relayON(1);
        }

        delay(10000);
        relayOFF(0);
        relayOFF(1);
    }
}
Add relay node firmware
