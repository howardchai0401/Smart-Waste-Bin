// ────────────────────────────────────────────────────────────────
//  SmartBin ESP32 node — reference firmware
//
//  Publishes sensor telemetry to the SmartBin backend broker.
//
//  Matches the backend contract in:
//    - backend/app/schemas.py::MqttSensorPayload
//    - backend/mosquitto/mosquitto.conf  (TLS on 8883, auth required)
//    - backend/mosquitto/acl             (device may only write to
//                                         smartbin/<username>/telemetry)
//
//  Board       : ESP32 DevKit
//  Sensors     : HX711 + 4 load cells, HC-SR04, MQ135, optional VBAT
//  MQTT client : knolleary/PubSubClient  (works over TLS via
//                WiFiClientSecure — CA cert baked in config.h)
//  JSON        : bblanchon/ArduinoJson
//
//  Why we swapped fields:
//    weight     → weight_kg
//    fill_level → fill_percentage (+ raw distance_cm)
//    odor       → gas_adc (+ human air_quality tag)
//    (new)      → bin_id  (needed to route to a Bin row)
//    (new)      → battery_voltage (drives low-battery anomalies)
//
//  Sensor calibration sourced from Smart Waste Bin.txt:
//    - HX711 stable-weight hysteresis filter
//    - MQ135 20-sample averaging with air-quality classification
//    - HC-SR04 map/constrain with ultrasonic timeout guard
//
//  Change log vs final code.txt:
//    - WiFi & MQTT unchanged (still loaded from config.h)
//    - Sensors & calibration replaced with Smart Waste Bin versions
//    - NO changes to MQTT auth, TLS, or broker settings
// ────────────────────────────────────────────────────────────────

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "HX711.h"

#include "config.h"

// ── MQTT topic derived from DEVICE_ID (set in config.h) ──────────
static const char* MQTT_TOPIC = "smartbin/" DEVICE_ID "/telemetry";
static const char* MQTT_CLIENT_ID = "esp32-" DEVICE_ID;

// ── Network + MQTT clients ──────────────────────────────────────
#if MQTT_USE_TLS
WiFiClientSecure netClient;
#else
WiFiClient netClient;
#endif
PubSubClient mqtt(netClient);

// ── Sensors ─────────────────────────────────────────────────────
HX711 scale;

// ────────────────────────────────────────────────────────────────
//  HX711 — stable-weight hysteresis (from Smart Waste Bin.txt)
// ────────────────────────────────────────────────────────────────

// Note: HX_CALIBRATION comes from config.h (22500.0f)

float weight_raw = 0.0f;    // raw instantaneous reading
float weight_kg  = 0.0f;    // stable weight used for display & MQTT

// Update only when additional weight exceeds 20 g
const float UPDATE_THRESHOLD = 0.02f;

// Detect rubbish removal when weight decreases by more than 300 g
const float RESET_THRESHOLD = 0.30f;

// Values below 200 g are considered an empty bin
const float EMPTY_WEIGHT_THRESHOLD = 0.20f;

// ────────────────────────────────────────────────────────────────
//  HC-SR04 — ultrasonic fill level (from Smart Waste Bin.txt)
// ────────────────────────────────────────────────────────────────

// Note: EMPTY_DISTANCE_CM (28.0) and FULL_DISTANCE_CM (2.0)
//       come from config.h

float distance_cm     = 0.0f;
float fill_percentage = 0.0f;

// ────────────────────────────────────────────────────────────────
//  MQ135 — multi-sample averaged gas reading (from Smart Waste Bin.txt)
// ────────────────────────────────────────────────────────────────

#define MQ135_SAMPLES 20

int   gas_adc         = 0;
float battery_voltage = 0.0f;

// ── State ───────────────────────────────────────────────────────
unsigned long last_publish_ms          = 0;
unsigned long last_reconnect_attempt_ms = 0;

// ────────────────────────────────────────────────────────────────
//  Wi-Fi (unchanged from final code.txt — config from config.h)
// ────────────────────────────────────────────────────────────────

static void wifi_begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[wifi] connecting to %s", WIFI_SSID);

  // Block up to 20 s on initial connect. Auto-reconnect handles
  // drops in the background after that.
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[wifi] initial connect failed — will retry in background");
  }
}

// ────────────────────────────────────────────────────────────────
//  MQTT (non-blocking reconnect — unchanged from final code.txt)
//  Uses DEVICE_ID + MQTT_PASS from config.h for auth.
// ────────────────────────────────────────────────────────────────

static bool mqtt_try_reconnect() {
  if (mqtt.connected()) return true;

  // Rate-limit attempts to once every 5 s so the sensor bus keeps running.
  unsigned long now = millis();
  if (now - last_reconnect_attempt_ms < 5000) return false;
  last_reconnect_attempt_ms = now;

  Serial.printf("[mqtt] connecting to %s:%d as user=%s ... ",
                MQTT_HOST, MQTT_PORT, DEVICE_ID);
  bool ok = mqtt.connect(MQTT_CLIENT_ID, DEVICE_ID, MQTT_PASS);
  if (ok) {
    Serial.println("connected");
  } else {
    Serial.printf("failed (state=%d)\n", mqtt.state());
  }
  return ok;
}

// ────────────────────────────────────────────────────────────────
//  HX711 — load cell (from Smart Waste Bin.txt calibration)
// ────────────────────────────────────────────────────────────────

static void read_weight() {
  if (!scale.is_ready()) return;

  // Average 10 HX711 readings
  float w = scale.get_units(10);

  // Prevent negative readings
  if (w < 0.0f) {
    w = 0.0f;
  }

  // Treat residual mechanical load below 200 g as zero
  if (w < EMPTY_WEIGHT_THRESHOLD) {
    w = 0.0f;
  }

  // Maximum supported bin weight is 10 kg
  if (w > 10.0f) {
    w = 10.0f;
  }

  // Store the current raw measurement
  weight_raw = w;

  // ==========================================
  // Stable Weight Filtering
  // ==========================================

  // New rubbish added:
  // Update only when weight increases by over 20 g
  if (weight_raw > weight_kg + UPDATE_THRESHOLD) {
    weight_kg = weight_raw;
  }

  // Rubbish removed:
  // Accept the lower reading when weight decreases
  // by more than 300 g
  else if ((weight_kg - weight_raw) > RESET_THRESHOLD) {
    weight_kg = weight_raw;
  }

  // Ignore smaller decreases caused by:
  // load-cell drift, PVC foam deformation,
  // mounting stress or mechanical relaxation
}

// ────────────────────────────────────────────────────────────────
//  HC-SR04 — ultrasonic (from Smart Waste Bin.txt calibration)
// ────────────────────────────────────────────────────────────────

static void read_ultrasonic() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) {
    Serial.println("[ultrasonic] timeout");
    return;
  }
  distance_cm = duration * 0.0343f / 2.0f;

  // Map ToF distance to 0–100 % using map() + constrain()
  // (from Smart Waste Bin.txt)
  int fill = map(distance_cm,
                 EMPTY_DISTANCE_CM,
                 FULL_DISTANCE_CM,
                 0,
                 100);
  fill = constrain(fill, 0, 100);
  fill_percentage = (float)fill;
}

// ────────────────────────────────────────────────────────────────
//  MQ135 — gas sensor (from Smart Waste Bin.txt calibration)
// ────────────────────────────────────────────────────────────────

static void read_gas() {
  long total = 0;

  // Average MQ135_SAMPLES readings — MQ135 needs warm-up before
  // readings become stable.
  for (int i = 0; i < MQ135_SAMPLES; i++) {
    total += analogRead(MQ135_PIN);
    delay(20);
  }

  gas_adc = total / MQ135_SAMPLES;
}

static void read_battery() {
#if BATTERY_ENABLED
  // Multi-sample average to smooth ADC noise.
  const int N = 16;
  uint32_t acc = 0;
  for (int i = 0; i < N; i++) acc += analogRead(BAT_PIN);
  float raw = (float)acc / N;
  battery_voltage = (raw / BAT_ADC_MAX) * BAT_ADC_REF_V * BAT_DIVIDER_RATIO;
#else
  battery_voltage = 3.3f;   // sensible default; backend treats as "OK"
#endif
}

// ────────────────────────────────────────────────────────────────
//  Human-readable tags matching the backend's optional fields
// ────────────────────────────────────────────────────────────────

static const char* bin_status_from_fill(float f) {
  if (f >= 80.0f) return "CRITICAL";
  if (f >= 50.0f) return "MEDIUM";
  return "LOW";
}

static const char* air_quality_from_gas(int adc) {
  // MQ135 air quality classification (from Smart Waste Bin.txt)
  if (adc < 280)       return "Clean Air";
  if (adc < 600)       return "Moderate Air Quality";
  return "Poor Air Quality / Strong Gas";
}

// ────────────────────────────────────────────────────────────────
//  Publish
// ────────────────────────────────────────────────────────────────

static void publish_payload() {
  // Human-readable output (from Smart Waste Bin.txt)
  Serial.println();
  Serial.println("================================");
  Serial.println(" SMART WASTE BIN SENSOR DATA");
  Serial.println("================================");

  Serial.print("Bin ID      : ");
  Serial.println(DEVICE_ID);

  Serial.print("Weight      : ");
  Serial.print(weight_kg, 3);
  Serial.println(" kg");

  Serial.print("Fill Level  : ");
  Serial.print(fill_percentage, 1);
  Serial.println(" %");

  Serial.print("Air Quality : ");
  Serial.println(air_quality_from_gas(gas_adc));

  // Build the JSON that backend/app/schemas.py::MqttSensorPayload expects.
  StaticJsonDocument<256> doc;
  doc["bin_id"]          = DEVICE_ID;
  doc["distance_cm"]     = round(distance_cm * 10) / 10.0;
  doc["fill_percentage"] = round(fill_percentage * 10) / 10.0;
  doc["bin_status"]      = bin_status_from_fill(fill_percentage);
  doc["gas_adc"]         = gas_adc;
  doc["air_quality"]     = air_quality_from_gas(gas_adc);
  doc["weight_kg"]       = round(weight_kg * 1000) / 1000.0;
  doc["battery_voltage"] = round(battery_voltage * 100) / 100.0;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));

  Serial.println();
  Serial.println("JSON Payload:");
  Serial.println(buf);
  Serial.println("===========================================");

  // PubSubClient only supports QoS 0 publish.
  if (!mqtt.publish(MQTT_TOPIC, (const uint8_t*)buf, n, /*retained=*/false)) {
    Serial.printf("[mqtt] publish failed (state=%d)\n", mqtt.state());
  }
}

// ────────────────────────────────────────────────────────────────
//  Setup — calibration from Smart Waste Bin.txt
// ────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("── SmartBin node booting ──");
  Serial.printf("device_id=%s topic=%s\n", DEVICE_ID, MQTT_TOPIC);

  // ── HX711 Calibration (from Smart Waste Bin.txt) ──────────────
  Serial.println();
  Serial.println("================================");
  Serial.println("Initializing Weight Sensor...");
  Serial.println("Please remove all rubbish from the bin.");
  Serial.println("Calibrating in progress...");
  Serial.println("================================");
  delay(2000);

  scale.begin(HX_DT, HX_SCK);

  // 15-second countdown before tare
  for (int i = 15; i > 0; i--) {
    Serial.print("Taring in ");
    Serial.print(i);
    Serial.println(" second(s)...");
    delay(1000);
  }

  scale.set_scale(HX_CALIBRATION);

  Serial.println();
  Serial.println("Taring weight sensor...");
  scale.tare(100);   // 100 samples for tare

  Serial.println("Weight sensor is ready.");
  Serial.println("================================");

  // ── Ultrasonic ────────────────────────────────────────────────
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // ── MQ135 — 30-second warm-up (from Smart Waste Bin.txt) ──────
  pinMode(MQ135_PIN, INPUT);
  Serial.println();
  Serial.println("================================");
  Serial.println("MQ-135 Air Quality Sensor");
  Serial.println("Warming up sensor...");
  Serial.println("Please wait 30 seconds.");
  Serial.println("================================");
  delay(30000);

#if BATTERY_ENABLED
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_PIN, ADC_11db);
#endif

  // ── Wi-Fi ─────────────────────────────────────────────────────
  wifi_begin();

  // ── MQTT ──────────────────────────────────────────────────────
#if MQTT_USE_TLS
  netClient.setCACert(CA_CERT);
  // Uncomment during first-boot troubleshooting only:
  // netClient.setInsecure();
#endif

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(384);           // room for our JSON payload
  mqtt.setKeepAlive(45);
  // First MQTT connection attempt
  mqtt_try_reconnect();

  Serial.println();
  Serial.println("[boot] Smart Bin Ready");
  Serial.println("[boot] Waiting for sensor readings...");
  Serial.println("================================");
}

// ────────────────────────────────────────────────────────────────
//  Loop
// ────────────────────────────────────────────────────────────────

void loop() {
  // Keep MQTT alive; retry connection in the background if dropped.
  // client.loop() must be called regularly — reconnects if it returns false.
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.loop() || !mqtt.connected()) {
      mqtt_try_reconnect();
    }
  }

  // Sensors run every iteration — no blocking on network state.
  read_weight();
  read_ultrasonic();
  read_gas();
  read_battery();

  unsigned long now = millis();
  if (now - last_publish_ms >= PUBLISH_INTERVAL_MS) {
    last_publish_ms = now;
    if (mqtt.connected()) {
      publish_payload();
    } else {
      // Print locally so operators can debug during commissioning.
      Serial.printf(
        "[local] fill=%.1f%% raw=%.1fcm w=%.2fkg gas=%d bat=%.2fV\n",
        fill_percentage, distance_cm, weight_kg, gas_adc, battery_voltage);
    }
  }

  delay(50);
}
