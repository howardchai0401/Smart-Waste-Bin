#include <WiFi.h>
#include <PubSubClient.h>
#include "HX711.h"
/*
-------------------------------------------------------
Project : Smart Waste Bin (ETP)
Board   : ESP32 DevKitC
Sensors :
- HX711 + 4 Load Cells
- HC-SR04
- MQ135

Current Version:
Sensor testing only.
MQTT publish temporarily disabled until broker details
are provided.
-------------------------------------------------------
*/

// ====================================================
// MQTT publish is temporarily disabled.
// Broker information will be added after the team
// leader provides the MQTT server details.
// ====================================================

//
// ===============================
// WiFi
// ===============================
//

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

//
// ===============================
// MQTT
// ===============================
//

const char* mqtt_server = "YOUR_BROKER_IP";
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

float weight = 0;

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

//
// ===============================
// Timing
// ===============================
//

unsigned long lastPublish = 0;

//
// ===============================
// WiFi
// ===============================
//

void connectWiFi()
{
    Serial.print("Connecting WiFi");

    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi Connected");
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
    float w = scale.get_units(100);

    if (w < 0)
        w = 0;

    if (w < 0.02)
        w = 0;

    if (w > 10)
        w = 10;

    weight = w;
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
    odorValue = analogRead(MQ135_PIN);
}

//
// ===============================
// Publish JSON
// ===============================
//

void publishData()
{
    String json = "{";

    json += "\"weight\":";
    json += String(weight,3);

    json += ",";

    json += "\"fill_level\":";
    json += String(fillLevel);

    json += ",";

    json += "\"odor\":";
    json += String(odorValue);

    json += "}";

    Serial.println("========================");
    Serial.println(json);
    Serial.println("========================");

    // MQTT disabled for sensor testing
    // client.publish(mqtt_topic, json.c_str());
}

//
// ===============================
// Setup
// ===============================
//

void setup()
{
    Serial.begin(115200);

    //
    // HX711
    //

    scale.begin(HX_DT,HX_SCK);

    delay(15000);

    scale.set_scale(calibration_factor);

    scale.tare(100);

    //
    // Ultrasonic
    //

    pinMode(TRIG_PIN,OUTPUT);
    pinMode(ECHO_PIN,INPUT);

    //
    // MQ135
    //

    pinMode(MQ135_PIN,INPUT);

    //
    // WiFi
    //

    connectWiFi();

    client.setServer(mqtt_server,mqtt_port);

    Serial.println();
    Serial.println("Smart Bin Ready");
}

//
// ===============================
// Loop
// ===============================
//

/*void loop()
{
    if (!client.connected())
        reconnectMQTT();

    client.loop();

    readWeight();

    readFillLevel();

    readOdor();

    if (millis() - lastPublish >= 10000)
    {
        lastPublish = millis();

        publishData();
    }
}*/

void loop()
{
    // Read all sensors
    readWeight();

    readFillLevel();

    readOdor();

    if (millis() - lastPublish >= 10000)
    {
        lastPublish = millis();

        publishData();
    }
}
