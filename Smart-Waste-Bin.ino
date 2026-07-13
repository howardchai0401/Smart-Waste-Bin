#include <WiFi.h>
#include <PubSubClient.h>
#include "HX711.h"
/*
-------------------------------------------------------
Project : Smart Waste Bin (ETP)
Board   : ESP32 DevKitC
Sensors :
- HX711 + 4 Load Cells  (Weight)
- HC-SR04                (Fill Level)
- MQ135                  (Air Quality)

MQTT   : Enabled — publishes JSON every 10 seconds
Broker : 10.152.10.53:1883
-------------------------------------------------------
*/

//
// ===============================
// WiFi
// ===============================
//

const char* ssid = "Howard's Galaxy S24+";
const char* password = "HowardChai1234";

//
// ===============================
// MQTT
// ===============================
//

const char* mqtt_server = "10.152.10.53";
const int mqtt_port = 1883;

const char* mqtt_topic = "smartbin/data";

WiFiClient espClient;
PubSubClient client(espClient);

//
// ===============================
// HX711
// ===============================
//

#define HX_DT   4
#define HX_SCK  2

HX711 scale;

float calibration_factor = 22500.0;

// Raw weight measured by HX711
float weight = 0.0;

// Stable weight used for display and MQTT
float displayedWeight = 0.0;

// Update only when additional weight exceeds 20 g
const float UPDATE_THRESHOLD = 0.02;

// Detect rubbish removal when weight decreases by more than 500 g
const float RESET_THRESHOLD = 0.30;

// Values below 200 g are considered an empty bin
const float EMPTY_WEIGHT_THRESHOLD = 0.20;
//
// ===============================
// HC-SR04
// ===============================
//

#define TRIG_PIN 5
#define ECHO_PIN 18

const float EMPTY_DISTANCE = 28.0;
const float FULL_DISTANCE  = 2.0;

float distance = 0;
int fillLevel = 0;

//
// ===============================
// MQ135
// ===============================
//

#define MQ135_PIN 34

int odorValue = 0;
String odorStatus = "";

const int MQ135_SAMPLES = 20;

//
// ===============================
// Timing
// ===============================
//

unsigned long lastPublish = 0;

// ===============================
// Immediate Update Settings
// ===============================

// Last values printed to Serial Monitor
float lastOutputWeight = 0.0;
int lastOutputFillLevel = 0;
int lastOutputOdor = 0;

// Significant-change thresholds
const int FILL_CHANGE_THRESHOLD = 10;  // 10%
const int ODOR_CHANGE_THRESHOLD = 100; // 100 ADC units

//
// ===============================
// WiFi
// ===============================
//

void connectWiFi()
{
    Serial.println();
    Serial.println("================================");
    Serial.println("Searching for Wi-Fi...");
    Serial.println("================================");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    delay(1000);

    Serial.println();
    Serial.println("Connecting to Wi-Fi...");

    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(500);
    }

    Serial.println();
    Serial.println("Wi-Fi Connected!");

    Serial.println();
    Serial.println("================================");
    Serial.println("Wi-Fi Connected Successfully!");
    Serial.println("================================");

    Serial.print("SSID           : ");
    Serial.println(WiFi.SSID());

    Serial.print("IP Address     : ");
    Serial.println(WiFi.localIP());

    Serial.print("Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    Serial.println("================================");
}

//
// ===============================
// MQTT
// ===============================
//

void reconnectMQTT()
{
    while (!client.connected())
    {
        Serial.print("Connecting MQTT...");

        if (client.connect("ESP32_SmartBin"))
        {
            Serial.println("Connected");
        }
        else
        {
            Serial.print("Failed : ");
            Serial.print("rc=");
            Serial.println(client.state());

            delay(3000);
        }
    }
}

//
// ===============================
// HX711
// ===============================
//

void readWeight()
{
    float w = scale.get_units(10);

    if (w < 0)
    {
        w = 0;
    }

    // Treat residual mechanical load below 200 g as zero
    if (w < EMPTY_WEIGHT_THRESHOLD)
    {
        w = 0;
    }

    if (w > 10.0)
    {
        w = 10.0;
    }

    weight = w;

    // New rubbish added
    if (weight > displayedWeight + UPDATE_THRESHOLD)
    {
        displayedWeight = weight;
    }

    // Rubbish removed
    else if ((displayedWeight - weight) > RESET_THRESHOLD)
    {
        displayedWeight = weight;
    }

    // Ignore small decreases
}

//
// ===============================
// Ultrasonic
// ===============================
//

void readFillLevel()
{
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(5);

    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);

    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);

    if(duration == 0)
    {
        Serial.println("Ultrasonic Timeout");
        return;
    }

    distance = duration * 0.0343 / 2.0;

    fillLevel = map(distance,
                    EMPTY_DISTANCE,
                    FULL_DISTANCE,
                    0,
                    100);

    fillLevel = constrain(fillLevel,0,100);
}

//
// ===============================
// MQ135
// ===============================
//
// MQ135 requires warm-up before readings become stable.
void readOdor()
{
    long total = 0;

    // Average multiple readings
    for (int i = 0; i < MQ135_SAMPLES; i++)
    {
        total += analogRead(MQ135_PIN);
        delay(20);
    }

    odorValue = total / MQ135_SAMPLES;

    // Air quality classification
    if (odorValue < 280)
    {
        odorStatus = "Clean Air";
    }
    else if (odorValue < 600)
    {
        odorStatus = "Moderate Air Quality";
    }
    else
    {
        odorStatus = "Poor Air Quality / Strong Gas";
    }
}

//
// ===============================
// Publish JSON
// ===============================
//

void publishData()
{
    // =====================================
    // Human-readable output
    // =====================================

    Serial.println();
    Serial.println("================================");
    Serial.println(" SMART WASTE BIN SENSOR DATA");
    Serial.println("================================");

    Serial.print("Weight      : ");
    Serial.print(displayedWeight, 3);
    Serial.println(" kg");

    Serial.print("Fill Level  : ");
    Serial.print(fillLevel);
    Serial.println(" %");

    /* Odor raw value — uncomment for debugging
    Serial.print("Odor Value  : ");
    Serial.println(odorValue);
    */

    Serial.print("Air Quality : ");
    Serial.println(odorStatus);

    // =====================================
    // JSON Output
    // =====================================

    String json = "{";

    json += "\"weight\":";
    json += String(displayedWeight, 3);

    json += ",";

    json += "\"fill_level\":";
    json += String(fillLevel);

    json += ",";

    json += "\"odor\":";
    json += String(odorValue);

    json += "}";

    Serial.println();
    Serial.println("JSON Payload:");
    Serial.println(json);

    Serial.println("===========================================");

    // Publish via MQTT (only when connected)
    if (client.connected())
    {
        client.publish(mqtt_topic, json.c_str());
    }
    else
    {
        Serial.println("MQTT disconnected — data not sent");
    }
}

//
// ===============================
// Setup
// ===============================
//

void setup()
{
    Serial.begin(115200);
    delay(3000);

    Serial.println();
    Serial.println("================================");
    Serial.println(" Smart Waste Bin System");
    Serial.println("================================");

    // ===============================
    // HX711 Initialization
    // ===============================

    Serial.println();
    Serial.println("================================");
    Serial.println("Initializing Weight Sensor...");
    Serial.println("Please remove all rubbish from the bin.");
    Serial.println("Calibrating in progress...");
    Serial.println("================================");
    delay(2000);

    scale.begin(HX_DT, HX_SCK);

    // 15-second countdown
    for (int i = 15; i > 0; i--)
    {
        Serial.print("Taring in ");
        Serial.print(i);
        Serial.println(" second(s)...");
        delay(1000);
    }

    scale.set_scale(calibration_factor);

    Serial.println();
    Serial.println("Taring weight sensor...");
    scale.tare(100);

    Serial.println("Weight sensor is ready.");
    Serial.println("================================");
    delay(1000);

    // Ultrasonic
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // MQ135
    pinMode(MQ135_PIN, INPUT);

    Serial.println();
    Serial.println("================================");
    Serial.println("MQ-135 Air Quality Sensor");
    Serial.println("Warming up sensor...");
    Serial.println("Please wait 30 seconds.");
    Serial.println("================================");

    delay(30000);

    // Wi-Fi
    connectWiFi();

    client.setServer(mqtt_server, mqtt_port);

    // Attempt first MQTT connection
    reconnectMQTT();

    Serial.println();
    Serial.println("Smart Bin Ready");
    Serial.println("Waiting for sensor readings...");
    Serial.println("================================");
}

//
// ===============================
// Loop
// ===============================
//

void loop()
{
    // =====================================
    // MQTT — keep connection alive
    // =====================================

    // client.loop() processes incoming messages and sends
    // keepalive pings. Must be called regularly or the
    // broker will disconnect you within ~15 seconds.
    if (!client.loop())
    {
        // Connection lost — reconnect
        reconnectMQTT();
    }

    if (!client.connected())
    {
        reconnectMQTT();
    }

    // =====================================
    // Read all sensors
    // =====================================

    readWeight();
    readFillLevel();
    readOdor();

    // Check whether any sensor has changed significantly
    bool weightChanged =
        abs(displayedWeight - lastOutputWeight) >= UPDATE_THRESHOLD;

    bool fillChanged =
        abs(fillLevel - lastOutputFillLevel) >= FILL_CHANGE_THRESHOLD;

    bool odorChanged =
        abs(odorValue - lastOutputOdor) >= ODOR_CHANGE_THRESHOLD;


    // =====================================
    // Immediate sensor update
    // =====================================

    if (weightChanged || fillChanged || odorChanged)
    {
        Serial.println();
        Serial.println("================================");
        Serial.println(" SENSOR CHANGE DETECTED");
        Serial.println("================================");

        Serial.print("Weight      : ");
        Serial.print(displayedWeight, 3);
        Serial.println(" kg");

        Serial.print("Fill Level  : ");
        Serial.print(fillLevel);
        Serial.println(" %");

        Serial.print("Air Quality : ");
        Serial.println(odorStatus);

        Serial.println("================================");

        // Save the latest displayed values
        lastOutputWeight = displayedWeight;
        lastOutputFillLevel = fillLevel;
        lastOutputOdor = odorValue;
    }


    // =====================================
    // Full sensor output every 10 seconds
    // =====================================

    if (millis() - lastPublish >= 10000)
    {
        lastPublish = millis();

        publishData();

        // Synchronize previous values
        lastOutputWeight = displayedWeight;
        lastOutputFillLevel = fillLevel;
        lastOutputOdor = odorValue;
    }
}
