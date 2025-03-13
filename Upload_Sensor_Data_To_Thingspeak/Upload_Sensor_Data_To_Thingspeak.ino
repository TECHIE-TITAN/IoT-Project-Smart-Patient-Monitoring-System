#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <DHT.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <spo2_algorithm.h>

const char* ssid = "YOUR_WIFI_SSID";                  // Check how to integrate with user wifi
const char* password = "YOUR_WIFI_PASSWORD";

const char* thingspeak_server = "api.thingspeak.com"; // This remains constant as there is only one server
const String master_api_key = "YOUR_MASTER_API_KEY";  // ThingSpeak account API key

#define DHT_PIN 4         // DHT22 data pin
#define DHT_TYPE DHT22    // DHT sensor type
#define SAMPLING_RATE 50  // Number of samples to collect for HR and SpO2 measurement

// Sensor objects
DHT dht(DHT_PIN, DHT_TYPE);
MAX30105 particleSensor;

// Variables for sensor readings
float temperature = 0;
float humidity = 0;
float heartRate = 0;
float spo2 = 0;

// Patient data
String patientID = "";
String channelID = "";
String writeAPIKey = "";

// For storing persistent data
Preferences preferences;

// For MAX30102 algorithm
uint32_t irBuffer[100]; // infrared LED sensor data
uint32_t redBuffer[100]; // red LED sensor data
int32_t bufferLength = 100; // data length
int32_t spo2Value; // SPO2 value
int8_t validSPO2; // indicator to show if the SPO2 calculation is valid
int32_t heartRateValue; // heart rate value
int8_t validHeartRate; // indicator to show if the heart rate calculation is valid

void setup() {
  Serial.begin(115200);
  Serial.println("\nPatient Monitoring System Starting...");
  
  // Initialize sensors
  dht.begin();
  
  // Initialize MAX30102 sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 sensor not found!");
    while (1);
  }
  
  // Configure MAX30102 for reading
  byte ledBrightness = 60; // Options: 0=Off to 255=50mA
  byte sampleAverage = 4; // Options: 1, 2, 4, 8, 16, 32
  byte ledMode = 2; // Options: 1=Red only, 2=Red+IR, 3=Red+IR+Green
  byte sampleRate = 100; // Options: 50, 100, 200, 400, 800, 1000, 1600, 3200
  int pulseWidth = 411; // Options: 69, 118, 215, 411
  int adcRange = 4096; // Options: 2048, 4096, 8192, 16384
  
  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
  
  // Connect to WiFi
  connectToWiFi();
  
  // Check if we have stored patient data
  preferences.begin("patient", false);
  patientID = preferences.getString("id", "");
  channelID = preferences.getString("channel", "");
  writeAPIKey = preferences.getString("apikey", "");
  
  if (patientID == "" || channelID == "" || writeAPIKey == "") {
    Serial.println("No patient data found. Creating new patient...");
    setupNewPatient();
  } else {
    Serial.println("Patient data loaded:");
    Serial.println("Patient ID: " + patientID);
    Serial.println("Channel ID: " + channelID);
    Serial.println("API Key: " + writeAPIKey);
  }
  
  Serial.println("Setup complete. Monitoring started.");
}

void loop() {
  // Read sensor data
  readSensorData();
  
  // Print readings to serial monitor
  printSensorData();
  
  // Send data to ThingSpeak
  sendDataToThingSpeak();
  
  // Wait before next reading
  delay(15000); // 15 seconds
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Failed to connect to WiFi. Will retry later.");
  }
}

void generateUniqueID(char* buffer, size_t size) {
  // Generate a unique 8-character alphanumeric ID
  const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  
  // Use part of the MAC address and current time for seed
  uint8_t mac[6];
  WiFi.macAddress(mac);
  unsigned long seed = millis() + mac[5] + (mac[4] << 8) + (mac[3] << 16);
  randomSeed(seed);
  
  // Generate the ID
  for (size_t i = 0; i < size - 1; i++) {
    int index = random(0, strlen(charset));
    buffer[i] = charset[index];
  }
  buffer[size - 1] = '\0'; // Null terminator
}

bool createThingSpeakChannel(const String& patientID, String& outChannelID, String& outWriteAPIKey) {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }
  }
  
  HTTPClient http;
  
  // ThingSpeak API endpoint for creating a channel
  String url = "https://api.thingspeak.com/channels.json";
  
  // Create channel with fields for temperature, humidity, heart rate, and SpO2
  String postData = "api_key=" + master_api_key;
  postData += "&name=Patient_" + patientID;
  postData += "&field1=Temperature";
  postData += "&field2=Humidity";
  postData += "&field3=HeartRate";
  postData += "&field4=SpO2";
  postData += "&public=false";  // Make channel private
  
  http.begin(url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  
  int httpResponseCode = http.POST(postData);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("ThingSpeak Channel Created:");
    
    // Parse the JSON response to get channel ID and API key
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error) {
      outChannelID = doc["id"].as<String>();
      outWriteAPIKey = doc["api_keys"][0]["api_key"].as<String>();
      
      Serial.print("Channel ID: ");
      Serial.println(outChannelID);
      Serial.print("Write API Key: ");
      Serial.println(outWriteAPIKey);
      
      http.end();
      return true;
    } else {
      Serial.println("JSON parsing failed");
    }
  } else {
    Serial.print("HTTP Error on channel creation: ");
    Serial.println(httpResponseCode);
  }
  
  http.end();
  return false;
}

void setupNewPatient() {
  // Generate a unique patient ID
  char idBuffer[9];  // 8 chars + null terminator
  generateUniqueID(idBuffer, sizeof(idBuffer));
  patientID = String(idBuffer);
  
  Serial.print("Generated patient ID: ");
  Serial.println(patientID);
  
  // Create a ThingSpeak channel for this patient
  if (createThingSpeakChannel(patientID, channelID, writeAPIKey)) {
    // Save the data to persistent storage
    preferences.putString("id", patientID);
    preferences.putString("channel", channelID);
    preferences.putString("apikey", writeAPIKey);
    
    Serial.println("New patient registered successfully!");
  } else {
    Serial.println("Failed to create ThingSpeak channel. Will retry on next boot.");
  }
}

/* void readSensorData(){
  humidity = 100;
  temperature = 32;
  heartRate = 78;
  spo2 = 98;
} */

void readSensorData() {
  // Read temperature and humidity from DHT22
  humidity = dht.readHumidity();
  temperature = dht.readTemperature();
  
  // Check if DHT22 reading succeeded
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Failed to read from DHT sensor!");
    temperature = 0;
    humidity = 0;
  }
  
  // Read MAX30102 for heart rate and SpO2
  bufferLength = 100; // buffer length of 100 stores 4 seconds of samples running at 25sps
  
  // Read the first 100 samples, determine signal range
  for (byte i = 0; i < bufferLength; i++) {
    while (particleSensor.available() == false) // Do we have new data?
      particleSensor.check(); // Check the sensor for new data
    
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample(); // We're finished with this sample so move to next sample
  }
  
  // Calculate heart rate and SpO2 after first 100 samples (first 4 seconds of samples)
  maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2Value, &validSPO2, &heartRateValue, &validHeartRate);
  
  // If valid readings were obtained, update the values
  if (validHeartRate == 1) {
    heartRate = heartRateValue;
  } else {
    heartRate = 0; // Invalid reading
  }
  
  if (validSPO2 == 1) {
    spo2 = spo2Value;
  } else {
    spo2 = 0; // Invalid reading
  }
}

void printSensorData() {
  Serial.println("-----------------------------------");
  Serial.println("Patient: " + patientID);
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" °C");
  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");
  Serial.print("Heart Rate: ");
  Serial.print(heartRate);
  Serial.println(" BPM");
  Serial.print("SpO2: ");
  Serial.print(spo2);
  Serial.println("%");
}

void sendDataToThingSpeak() {
  // Check if we have channel information
  if (channelID == "" || writeAPIKey == "") {
    Serial.println("No channel information available. Cannot send data.");
    return;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Cannot connect to WiFi. Data not sent.");
      return;
    }
  }
  
  HTTPClient http;
  
  // Prepare the data string
  String data = "api_key=" + writeAPIKey;
  data += "&field1=" + String(temperature);
  data += "&field2=" + String(humidity);
  data += "&field3=" + String(heartRate);
  data += "&field4=" + String(spo2);
  
  // Prepare the URL
  String url = "http://api.thingspeak.com/update";
  
  // Send the request
  http.begin(url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  
  int httpResponseCode = http.POST(data);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Data sent to ThingSpeak:");
    Serial.println("HTTP Response code: " + String(httpResponseCode));
    Serial.println("Entry ID: " + response);
  } else {
    Serial.println("Error on sending data: " + String(httpResponseCode));
  }
  
  http.end();
}
