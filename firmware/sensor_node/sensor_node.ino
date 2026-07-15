// ============================================================
//  ESP32-1  —  DUAL-LINE POWER SENSOR NODE
//  IoT-Based Real-Time Power Quality Monitoring System
//
//  Reads voltage & current on two lines, calculates power,
//  detects spikes via a push-button simulation, and sends
//  data two ways:
//    1) MQTT  → Node-RED / React dashboard (cloud monitoring)
//    2) UART  → Relay Control Node (fast local communication)
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ================= WIFI =================
// TODO: set your own WiFi credentials before flashing
const char* SSID     = "YOUR_WIFI_SSID";
const char* PASSWORD = "YOUR_WIFI_PASSWORD";

// ================= MQTT =================
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_CLIENT = "Sensor_Node_01";

const char* TOPIC_LINE1 = "factory/line1/power/monitor";
const char* TOPIC_LINE2 = "factory/line2/power/monitor";

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ================= UART (to Relay Node) =================
HardwareSerial SerialUART(1);
#define TXD1 17   // Sensor Node UART TX  -> Relay Node UART RX (GPIO16)
#define RXD1 16   // Sensor Node UART RX  <- Relay Node UART TX (GPIO17)

// ================= PINS =================
// Line 1
#define L1_CURRENT_PIN   32
#define L1_VOLTAGE_PIN   33
#define L1_BUTTON_PIN    21   // spike-simulation push button

// Line 2
#define L2_CURRENT_PIN   34
#define L2_VOLTAGE_PIN   35
#define L2_BUTTON_PIN    21   // NOTE: shares GPIO21 with L1 button in this build;
                              // wire a second GPIO here if independent triggering is needed

// ================= SENSOR CONSTANTS =================
const float ADC_REF        = 3.3;
const int   ADC_RES        = 4095;
const float SENSITIVITY    = 0.185;   // ACS712 sensitivity (V/A)
const float CAL_FACTOR     = 675.0;   // Voltage sensor calibration factor
const float CURRENT_FACTOR = 0.53;    // Current sensor calibration factor
const int   RMS_SAMPLES    = 800;

// ================= CALIBRATION OFFSETS =================
float l1Offset = 2048;
float l2Offset = 2048;

// ================= TIMING =================
unsigned long lastPublishTime = 0;
const unsigned long PUBLISH_INTERVAL_MS = 300;   // < 2s real-time update target

// ============================================================
//  WIFI
// ============================================================
void connectWiFi() {
    WiFi.begin(SSID, PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected");
}

// ============================================================
//  MQTT
// ============================================================
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
//  CURRENT SENSOR ZERO-OFFSET CALIBRATION
//  Run with no load connected at boot.
// ============================================================
float calibrateCurrent(int pin, const char* name) {
    Serial.print("[CAL] ");
    Serial.println(name);
    delay(2000);

    long sum = 0;
    for (int i = 0; i < 2000; i++) {
        sum += analogRead(pin);
        delayMicroseconds(200);
    }
    return sum / 2000.0;
}

// ============================================================
//  RMS VOLTAGE
// ============================================================
float readVoltage(int pin) {
    long offsetSum = 0;
    for (int i = 0; i < RMS_SAMPLES; i++) {
        offsetSum += analogRead(pin);
        delayMicroseconds(120);
    }
    float offset = offsetSum / (float)RMS_SAMPLES;

    float sumSq = 0;
    for (int i = 0; i < RMS_SAMPLES; i++) {
        float raw = analogRead(pin) - offset;
        float v   = (raw / ADC_RES) * ADC_REF;
        sumSq += v * v;
        delayMicroseconds(120);
    }

    float vrms = sqrt(sumSq / RMS_SAMPLES);
    return vrms * CAL_FACTOR;
}

// ============================================================
//  RMS CURRENT
// ============================================================
float readCurrent(int pin, float offset) {
    float sumSq = 0;
    for (int i = 0; i < RMS_SAMPLES; i++) {
        float raw = analogRead(pin) - offset;
        float v   = (raw / ADC_RES) * ADC_REF;
        float c   = v / SENSITIVITY;
        sumSq += c * c;
        delayMicroseconds(120);
    }

    float rms     = sqrt(sumSq / RMS_SAMPLES);
    float current  = rms * CURRENT_FACTOR - 0.03;   // small offset correction
    if (current < 0) current = 0;                   // clamp noise floor
    return current;
}

// ============================================================
//  PUBLISH LINE DATA TO MQTT
// ============================================================
void publishData(int id, float voltage, float current, float power,
                  bool fault, bool spike, const char* topic) {

    StaticJsonDocument<256> doc;
    doc["lineId"]  = id;
    doc["voltage"] = voltage;
    doc["current"] = current;
    doc["power"]   = power;
    doc["fault"]   = fault;
    doc["spike"]   = spike;

    char payload[256];
    serializeJson(doc, payload);

    mqttClient.publish(topic, payload);
    Serial.println("MQTT: " + String(payload));
}

// ============================================================
//  SEND COMBINED POWER + SPIKE STATE TO RELAY NODE OVER UART
//  Format: P1,P2,SPIKE   e.g.  300.00,250.00,1
// ============================================================
void sendUARTCombined(float p1, float p2, bool spike) {
    SerialUART.print(p1, 2);
    SerialUART.print(",");
    SerialUART.print(p2, 2);
    SerialUART.print(",");
    SerialUART.println(spike ? 1 : 0);

    Serial.print("UART SENT: ");
    Serial.print(p1);
    Serial.print(",");
    Serial.print(p2);
    Serial.print(",");
    Serial.println(spike);
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    SerialUART.begin(115200, SERIAL_8N1, RXD1, TXD1);

    pinMode(L1_BUTTON_PIN, INPUT_PULLUP);
    pinMode(L2_BUTTON_PIN, INPUT_PULLUP);
    analogReadResolution(12);

    l1Offset = calibrateCurrent(L1_CURRENT_PIN, "Line-1 Current");
    l2Offset = calibrateCurrent(L2_CURRENT_PIN, "Line-2 Current");

    connectWiFi();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    connectMQTT();
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();

    if (millis() - lastPublishTime > PUBLISH_INTERVAL_MS) {
        lastPublishTime = millis();

        float v1 = readVoltage(L1_VOLTAGE_PIN);
        float c1 = readCurrent(L1_CURRENT_PIN, l1Offset);
        float p1 = v1 * c1;
        bool spike1 = digitalRead(L1_BUTTON_PIN) == LOW;
        bool fault1 = (v1 > 270 || v1 < 180 || c1 > 1.5 || spike1);

        float v2 = readVoltage(L2_VOLTAGE_PIN);
        float c2 = readCurrent(L2_CURRENT_PIN, l2Offset);
        float p2 = v2 * c2;
        bool spike2 = digitalRead(L2_BUTTON_PIN) == LOW;
        bool fault2 = (v2 > 270 || v2 < 180 || c2 > 1.5 || spike2);

        publishData(1, v1, c1, p1, fault1, spike1, TOPIC_LINE1);
        publishData(2, v2, c2, p2, fault2, spike2, TOPIC_LINE2);

        bool spikeGlobal = spike1 || spike2;
        sendUARTCombined(p1, p2, spikeGlobal);
    }
}
Add sensor node firmware
