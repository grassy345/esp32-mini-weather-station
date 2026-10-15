#include <DFRobot_RainfallSensor.h>
#include <NetworkClientSecure.h>
#include <time.h>
#include <MQTT.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <SensirionI2cSht3x.h>

// WiFi credentials
const char *ssid         = "YOUR_WIFI_SSID";
const char *wifiPassword = "YOUR_WIFI_PASSWORD";

// HiveMQ credentials
const char *mqttBroker   = "YOUR_MQTT_BROKER";
const char *mqttUsername = "YOUR_MQTT_USERNAME";
const char *mqttPassword = "YOUR_MQTT_PASSWORD";
const int   mqttPort     = 8883;

// MQTT topic
const char *topicStatus  = "rainfall/status";

// Telegram bot credentials
const char *telegramToken  = "YOUR_TELEGRAM_BOT_TOKEN";
const char *telegramChatId = "YOUR_TELEGRAM_CHAT_ID";

// Client objects
NetworkClientSecure secureClient;
MQTTClient          mqttClient(512);
DFRobot_RainfallSensor_UART Sensor(&Serial2);

// BMP280 and SHT30 sensor objects
Adafruit_BMP280 bmp;
SensirionI2cSht3x sensor;

// Rainfall tracking
float         lastRainfall  = 0;
unsigned long lastCheckTime = 0;

// -------------------------------------------------------
// getIntensityLabel() — returns a human-readable label
// -------------------------------------------------------
const char* getIntensityLabel(float rate)
{
  if (rate <= 0.0f)
    return "No Rain";
  if (rate <= 2.5f)
    return "Light Rain";
  if (rate <= 7.5f)
    return "Moderate Rain";
  if (rate <= 50.0f)
    return "Heavy Rain";
  return "Violent Rain";
}

// -------------------------------------------------------
// sendTelegramAlert() — sends a rain alert to the Telegram
// group. Returns true on success, false after 3 retries.
// Uses a local NetworkClientSecure (not the MQTT one).
// -------------------------------------------------------
bool sendTelegramAlert(float rate)
{
  NetworkClientSecure tg;
  tg.setInsecure();
  tg.setHandshakeTimeout(5000);

  time_t nowTime;
  time(&nowTime);
  struct tm *ti = localtime(&nowTime);
  char timeStr[24];
  strftime(timeStr, sizeof(timeStr), "%d %b %Y, %I:%M %p", ti);

  char msg[200];
  snprintf(msg, sizeof(msg),
    "🌧️ %s: %.2f mm/hr\n%s",
    getIntensityLabel(rate), rate, timeStr);

  String body = "{\"chat_id\":\"";
  body += telegramChatId;
  body += "\",\"text\":\"";
  body += msg;
  body += "\"}";

  for (int attempt = 0; attempt < 3; attempt++)
  {
    if (tg.connect("api.telegram.org", 443, 5000))
    {
      tg.println("POST /bot" + String(telegramToken) + "/sendMessage HTTP/1.1");
      tg.println("Host: api.telegram.org");
      tg.println("Content-Type: application/json");
      tg.println("Content-Length: " + String(body.length()));
      tg.println("Connection: close");
      tg.println();
      tg.print(body);
      tg.stop();
      return true;
    }
    tg.stop();
    if (attempt < 2)
      delay(1000);
  }

  return false;
}

// -------------------------------------------------------
// publishStatus() — builds JSON and publishes to MQTT
// -------------------------------------------------------
void publishStatus(float rate, float pressure, float temperature, float humidity)
{
  time_t nowTime;
  time(&nowTime);
  unsigned long long timestampMs = (unsigned long long)nowTime * 1000ULL;

  char rateStr[10];
  dtostrf(rate, 4, 2, rateStr);
  char pressureStr[12];
  if (isnan(pressure))
    strcpy(pressureStr, "null");
  else
    dtostrf(pressure, 8, 2, pressureStr);
  char tempStr[8];
  if (isnan(temperature))
    strcpy(tempStr, "null");
  else
    dtostrf(temperature, 5, 2, tempStr);
  char humStr[8];
  if (isnan(humidity))
    strcpy(humStr, "null");
  else
    dtostrf(humidity, 5, 2, humStr);

  char json[256];
  snprintf(json, sizeof(json),
    "{\"rate\":%s,\"pressure\":%s,\"temperature\":%s,\"humidity\":%s,\"timestamp\":%llu}",
    rateStr, pressureStr, tempStr, humStr, timestampMs);

  mqttClient.publish(topicStatus, json, true, 1);
}

// -------------------------------------------------------
// ensureWiFi() — reconnects WiFi if dropped
// -------------------------------------------------------
void ensureWiFi()
{
  if(WiFi.status() == WL_CONNECTED)
	  return;

  digitalWrite(2, LOW); // WiFi died
  digitalWrite(18, LOW); // So did MQTT service

  while(WiFi.status() != WL_CONNECTED)
  {
    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid, wifiPassword);
    
    int attempts = 0;
    // Wait up to 10 seconds for connection to succeed
    while(WiFi.status() != WL_CONNECTED && attempts < 50)
    {
      delay(200);
      digitalWrite(2, !digitalRead(2));
      attempts++;
    }
    
    // If still not connected, wait 2 seconds before re-triggering WiFi.begin()
    if(WiFi.status() != WL_CONNECTED)
      delay(2000);
  }

  digitalWrite(2, HIGH); // WiFi came back on
}

// -------------------------------------------------------
// reconnectMQTT()
// -------------------------------------------------------
void reconnectMQTT()
{
  while (!mqttClient.connected())
  {
    ensureWiFi();

    if (mqttClient.connect("ESP32Client", mqttUsername, mqttPassword))
      digitalWrite(18, HIGH); // MQTT service established
    else
    {
      for(int i = 0; i < 50; i++)
      {
        if (i % 10 == 0)
          digitalWrite(18, !digitalRead(18));
        delay(100);
      }
      digitalWrite(18, LOW); // MQTT service was disconnected
    }
  }
}

// -------------------------------------------------------
// setup()
// -------------------------------------------------------
void setup()
{
  ////////////////////////////////////////////////////////
  // WiFi setup //
  ////////////////////////////////////////////////////////
  
  pinMode(2, OUTPUT); // For WiFi indicator (Default blue LED)
  digitalWrite(2, LOW);  
  WiFi.setHostname("ESP32");
  WiFi.useStaticBuffers(true);
  while(WiFi.status() != WL_CONNECTED)
  {
    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid, wifiPassword);
    
    int attempts = 0;
    // Wait up to 10 seconds for connection to succeed
    while(WiFi.status() != WL_CONNECTED && attempts < 50)
    {
      delay(200);
      digitalWrite(2, !digitalRead(2));
      attempts++;
    }
    
    // If still not connected, wait 2 seconds before re-triggering WiFi.begin()
    if(WiFi.status() != WL_CONNECTED)
      delay(2000);
  }  
  digitalWrite(2, HIGH); // WiFi connected
  
  ////////////////////////////////////////////////////////
  // NTP Time Synchronization //
  ////////////////////////////////////////////////////////
  
  pinMode(32, OUTPUT); // External Blue LED for Time-sync indication
  digitalWrite(32, LOW);
  // Configure NTP
  configTime(19800, 0, "pool.ntp.org");
  time_t nowTime = time(nullptr);
  while(nowTime < 100000)
  {
    delay(100);
    nowTime = time(nullptr);
	digitalWrite(32, !digitalRead(32));
  }
  digitalWrite(32, HIGH); // NTP time synchronized
  
  ////////////////////////////////////////////////////////
  // Rainfall rate measuring sensor setup //
  ////////////////////////////////////////////////////////
  
  pinMode(25, OUTPUT); // Red LED for sensor status indication
  digitalWrite(25, LOW);
  Serial2.begin(9600, SERIAL_8N1, 16, 17, false, 20000UL, 120);
  delay(2000);
  while(!Sensor.begin())
  {
    delay(200);
    digitalWrite(25, !digitalRead(25));
  }
  digitalWrite(25, HIGH); // Rainfall sensor was setup successfully
  
  ////////////////////////////////////////////////////////
  // BMP280 Sensor startup //
  ////////////////////////////////////////////////////////
  
  pinMode(27, OUTPUT); // Yellow LED for BMP280 sensor status indication
  digitalWrite(27, LOW);
  if (bmp.begin(0x76, 0x58))
  {
	/* Default settings from datasheet. */
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED,     /* Operating Mode. */
                    Adafruit_BMP280::SAMPLING_X1,     /* Temp. oversampling */
                    Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                    Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                    Adafruit_BMP280::STANDBY_MS_1000); /* Standby time. */
    digitalWrite(27, HIGH); // BMP280 sensor setup successfully indicated by Yellow LED  
  }
  // else Yellow LED stays LOW
  
  ////////////////////////////////////////////////////////
  // SHT30 Sensor startup //
  ////////////////////////////////////////////////////////
				  
  pinMode(12, OUTPUT); // Green LED for SHT30 sensor status indication
  digitalWrite(12, LOW);
  sensor.begin(Wire, 0x44);
  sensor.stopMeasurement();
  delay(1);
  sensor.softReset();
  delay(100);
  uint16_t statusReg = 0u;
  if(sensor.readStatusRegister(statusReg) == NO_ERROR)
    digitalWrite(12, HIGH); // SHT30 operating successfully
  // else: Green LED stays LOW — sensor failure indicated
  
  ////////////////////////////////////////////////////////
  // MQTT Service setup //
  ////////////////////////////////////////////////////////

  pinMode(18, OUTPUT); // Green LED for MQTT service indication
  digitalWrite(18, LOW);
  secureClient.setInsecure();
  mqttClient.begin(mqttBroker, mqttPort, secureClient);
  reconnectMQTT();
  
  ////////////////////////////////////////////////////////

  lastCheckTime = millis();
}

// -------------------------------------------------------
// loop()
// -------------------------------------------------------
void loop()
{
  // Keep MQTT alive
  if(!mqttClient.connected())
  {
    digitalWrite(18, LOW); // MQTT service was disconnected
    reconnectMQTT();
  }
  mqttClient.loop();

  unsigned long now = millis();

  if(now - lastCheckTime >= 600000)
  {
    float currentRainfall = Sensor.getRainfall(24);

    if(now - lastCheckTime > 1800000)
    {
      lastRainfall = currentRainfall;
      lastCheckTime = now;
      return;
    }
    
    float rate = max(0.0f, (currentRainfall - lastRainfall) * 6);
    float pressure = NAN;
    if(bmp.takeForcedMeasurement())
        pressure = bmp.readPressure() / 100.0f;

    float temperature;
    float humidity;
	  static int16_t error;
    error = sensor.measureSingleShot(REPEATABILITY_MEDIUM, false, temperature, humidity);
    if(error != NO_ERROR)
	  {
      temperature = NAN;
      humidity = NAN;
    }

    publishStatus(rate, pressure, temperature, humidity);

    if (rate > 0.0f)
      sendTelegramAlert(rate);

    lastRainfall  = currentRainfall;
    lastCheckTime = now;
  }
  delay(10);
}