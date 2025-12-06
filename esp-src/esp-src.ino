#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

// --- NETWORK & STORAGE ---
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <Preferences.h>
#include <vector>
#include <PubSubClient.h> // MQTT Library
#include "secrets.h" 

// ==========================================
// 1. CONFIGURATION
// ==========================================
// --- MQTT SETTINGS ---
#define MQTT_BROKER         "broker.hivemq.com" 
#define MQTT_PORT           1883
#define MQTT_TOPIC_STATUS   "esp32/lock/status" // Topic 1: Real-time Status
#define MQTT_TOPIC_LOG      "esp32/lock/log"    // Topic 2: Database Log (JSON)

// --- HARDWARE SETTINGS ---
#define LOCK_PIN            16  
#define PIR_PIN             17  
#define STATUS_LED          2   
#define AUTO_LOCK_TIMEOUT_MS 15000 

// --- BLE SETTINGS ---
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_ID_UUID        "beb5483e-36e1-4688-b7f5-ea07361b26a8" 
#define CHAR_NONCE_UUID     "d29a73d5-1234-4567-8901-23456789abcd" 
#define CHAR_RESPONSE_UUID  "1c95d5e3-d8bc-4e31-9989-13e6184a44b9" 

const char* THIS_ROOM = "ROOM_404"; 

// ==========================================
// 2. DATA STRUCTURES
// ==========================================
struct User {
    String id;
    String key;
    String role;        
    String allowedRoom; 
};

std::vector<User> userDatabase; 

// ==========================================
// 3. GLOBAL OBJECTS
// ==========================================
BLEServer* pServer = NULL;
BLECharacteristic* pResponseChar = NULL;
bool deviceConnected = false;
bool idVerified = false;
int currentUserIndex = -1;

// Tracks who performed the action (for Logging)
String lastUnlockedBy = "SYSTEM"; 

QueueHandle_t doorQueue;
TimerHandle_t autoLockTimer; 
Preferences preferences; 

// MQTT Client
WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// 4. HELPER FUNCTIONS 
// ==========================================

String toHexString(uint8_t* data, size_t len) {
    String output = "";
    output.reserve(len * 2);
    const char hexChars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        output += hexChars[(data[i] >> 4) & 0xF];
        output += hexChars[data[i] & 0xF];
    }
    return output;
}

void calculateHMAC(String nonce, String key, uint8_t* output) {
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)nonce.c_str(), nonce.length());
    mbedtls_md_hmac_finish(&ctx, output);
    mbedtls_md_free(&ctx);
}

// ==========================================
// 5. DATABASE & WIFI FUNCTIONS
// ==========================================

void parseUserData(String jsonString) {
    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, jsonString);

    if (error) {
        Serial.print(F("JSON Parsing Error: ")); 
        Serial.println(error.f_str());
        return;
    }

    userDatabase.clear(); 

    JsonArray data = doc["data"];
    for (JsonObject elem : data) {
        User u;
        if (elem.containsKey("username")) u.id = elem["username"].as<String>();
        else if (elem.containsKey("userId")) u.id = elem["userId"].as<String>();
        
        if (elem.containsKey("key")) u.key = elem["key"].as<String>();
        else if (elem.containsKey("secretKey")) u.key = elem["secretKey"].as<String>();
        
        if (elem.containsKey("allowed_room")) u.allowedRoom = elem["allowed_room"].as<String>();
        else if (elem.containsKey("allowedRoom")) u.allowedRoom = elem["allowedRoom"].as<String>();

        u.role = elem["role"].as<String>();
        userDatabase.push_back(u);
    }
    Serial.print("User Database Updated. Total Users: "); 
    Serial.println(userDatabase.size());
}

void syncDatabase() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        Serial.print("Syncing DB from: "); 
        Serial.println(API_URL);

        http.begin(API_URL); 
        int httpResponseCode = http.GET();

        if (httpResponseCode > 0) {
            String payload = http.getString();
            Serial.print("HTTP Success. Payload Size: ");
            Serial.println(payload.length());
            
            parseUserData(payload);
            
            preferences.begin("lock_db", false); 
            preferences.putString("json_data", payload);
            preferences.end();
            Serial.println("Database Synced & Saved to Flash.");
        } 
        else {
            Serial.print("HTTP Error: ");
            Serial.println(httpResponseCode);
            Serial.print("Error Details: ");
            Serial.println(http.errorToString(httpResponseCode));
        }
        http.end();
    } else {
        Serial.println("Cannot Sync: WiFi Disconnected.");
    }
}

void loadOfflineDatabase() {
    Serial.println("Attempting to load Offline Database...");
    preferences.begin("lock_db", true); 
    String payload = preferences.getString("json_data", "");
    preferences.end();

    if (payload != "") {
        Serial.println("Offline Data Found.");
        parseUserData(payload);
    } else {
        Serial.println("No Offline Data Found in Flash.");
    }
}

// --- MQTT HELPER FUNCTIONS ---

// 1. Sends "LOCKED" or "UNLOCKED" (For UI Status)
void publishLiveStatus(String state) {
    if (!client.connected()) return;
    client.publish(MQTT_TOPIC_STATUS, state.c_str());
    // (Optional: add MQTT log here if you want, but sticking to your requested logs)
}

// 2. Sends Full JSON (For Database/Logs)
void publishAccessLog(String user, String room) {
    if (!client.connected()) return;
    
    JsonDocument doc;
    doc["userId"] = user;
    doc["room"]   = room;
    // Node-RED will add timestamp
    
    char buffer[200];
    serializeJson(doc, buffer);

    client.publish(MQTT_TOPIC_LOG, buffer);
}

// ==========================================
// 6. TASKS & TIMERS
// ==========================================

void autoLockCallback(TimerHandle_t xTimer) {
  Serial.println("[TIMER] Auto-lock triggered.");
  int closeCommand = 0; 
  xQueueSend(doorQueue, &closeCommand, 0); 
}

void doorTask(void * parameter) {
  int receivedCommand;
  bool isUnlocked = false; 
  unsigned long lastTimerReset = 0; 

  Serial.println("[TASK] Door Task Started.");

  for(;;) { 
    if (xQueueReceive(doorQueue, &receivedCommand, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (receivedCommand == 1) { 
        // --- UNLOCK EVENT ---
        Serial.println("[DOOR] Unlocking...");
        digitalWrite(LOCK_PIN, HIGH);
        digitalWrite(STATUS_LED, HIGH);
        isUnlocked = true;
        xTimerStart(autoLockTimer, 0); 
        if (deviceConnected && pResponseChar != NULL) {
            pResponseChar->notify();
        }
        
        // MQTT ACTIONS
        publishLiveStatus("UNLOCKED");
        publishAccessLog(lastUnlockedBy, THIS_ROOM);
      }
      else if (receivedCommand == 0) { 
        // --- LOCK EVENT ---
        Serial.println("[DOOR] Locking...");
        digitalWrite(LOCK_PIN, LOW);
        digitalWrite(STATUS_LED, LOW);
        isUnlocked = false;
        xTimerStop(autoLockTimer, 0);
        if (deviceConnected && pResponseChar != NULL) {
            pResponseChar->notify();
        }
        
        // MQTT ACTIONS
        publishLiveStatus("LOCKED");
        // No log needed for auto-lock usually
      }
    }
    
    if (isUnlocked && digitalRead(PIR_PIN) == HIGH) {
      if (millis() - lastTimerReset > 1000) {
        Serial.println("[PIR] Motion Detected - Timer Extended.");
        xTimerReset(autoLockTimer, 0); 
        lastTimerReset = millis();
      }
    }
  }
}

// ==========================================
// 7. BLE CALLBACKS
// ==========================================

class ServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("[BLE] Device Connected.");
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      idVerified = false;
      currentUserIndex = -1;
      lastUnlockedBy = "SYSTEM"; // Reset
      Serial.println("[BLE] Device Disconnected. Restarting Advertising...");
      BLEDevice::startAdvertising(); 
    }
};

class IDCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String rxValue = pCharacteristic->getValue().c_str();
        rxValue.trim();

        if (rxValue.length() > 0) {
            Serial.print("[BLE] ID Rx: "); Serial.println(rxValue);
            int cmd = 1; 

            if (rxValue == "OPEN") {
                if (idVerified && currentUserIndex != -1) {
                    User u = userDatabase[currentUserIndex];
                    
                    // SAVE USER FOR LOGGING
                    lastUnlockedBy = u.id;

                    if (u.role == "ADMIN") {
                        Serial.println("[ACCESS] Granted (Admin)");
                        xQueueSend(doorQueue, &cmd, 0); 
                    } 
                    else if (u.role == "LECTURER" && u.allowedRoom == THIS_ROOM) {
                         Serial.println("[ACCESS] Granted (Lecturer)");
                         xQueueSend(doorQueue, &cmd, 0); 
                    } 
                    else {
                         Serial.println("[ACCESS] Denied (Wrong Room)");
                         pResponseChar->setValue("DENIED_ROOM");
                         pResponseChar->notify();
                    }
                } else {
                    Serial.println("[ACCESS] Denied (Not Verified)");
                }
            }
            else {
                // Identity Lookup
                bool found = false;
                for(int i=0; i < userDatabase.size(); i++) {
                    if (userDatabase[i].id == rxValue) {
                        currentUserIndex = i;
                        idVerified = true;
                        found = true;
                        Serial.print("[AUTH] User Found: "); 
                        Serial.println(userDatabase[i].id);
                        break;
                    }
                }
                if(!found) {
                    idVerified = false;
                    currentUserIndex = -1;
                    Serial.println("[AUTH] Unknown User ID");
                }
            }
        }
    }
};

class NonceCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String nonce = pCharacteristic->getValue().c_str();
        nonce.trim();

        if (nonce.length() > 0) {
             Serial.print("[BLE] Nonce Rx: "); Serial.println(nonce);
             
             if (idVerified && currentUserIndex != -1) {
                String userKey = userDatabase[currentUserIndex].key;
                uint8_t hmacResult[32];
                calculateHMAC(nonce, userKey, hmacResult);
                String hexSignature = toHexString(hmacResult, 32);
                
                pResponseChar->setValue(hexSignature.c_str());
                pResponseChar->notify();
                Serial.print("[BLE] Sent Hash: "); Serial.println(hexSignature);
            } else {
                Serial.println("[BLE] Nonce Ignored: User not verified.");
            }
        }
    }
};

// ==========================================
// 8. SETUP & LOOP
// ==========================================

void reconnectMQTT() {
  while (!client.connected()) {
    // Silent background reconnection to avoid spamming logs
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("[MQTT] Connected");
      
      String currentStatus = (digitalRead(LOCK_PIN) == HIGH) ? "UNLOCKED" : "LOCKED";
      publishLiveStatus(currentStatus);
    } else {
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n--- ESP32 SMART LOCK STARTING ---");
  
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  digitalWrite(LOCK_PIN, LOW); 
  digitalWrite(STATUS_LED, LOW);

  // WiFi Setup
  WiFi.begin(WIFI_SSID, WIFI_PASS); 
  Serial.print("Connecting to WiFi");
  
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 10) {
      delay(500); // 500ms delay per dot
      Serial.print(".");
      retry++;
  }
  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi Connected! IP: ");
      Serial.println(WiFi.localIP());
      syncDatabase(); 
  } else {
      Serial.println("WiFi Timeout. Switching to Offline Mode.");
      loadOfflineDatabase(); 
      delay(1000);
  }

  // MQTT INIT
  client.setServer(MQTT_BROKER, MQTT_PORT);

  // RTOS Setup
  doorQueue = xQueueCreate(10, sizeof(int));
  autoLockTimer = xTimerCreate("AutoLock", pdMS_TO_TICKS(AUTO_LOCK_TIMEOUT_MS), pdFALSE, (void*)0, autoLockCallback);
  xTaskCreatePinnedToCore(doorTask, "DoorTask", 4096, NULL, 1, NULL, 1);

  // BLE Setup
  BLEDevice::init("ESP32_Smart_Lock"); 
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pIDChar = pService->createCharacteristic(CHAR_ID_UUID, BLECharacteristic::PROPERTY_WRITE);
  pIDChar->setCallbacks(new IDCallbacks());

  BLECharacteristic *pNonceChar = pService->createCharacteristic(CHAR_NONCE_UUID, BLECharacteristic::PROPERTY_WRITE);
  pNonceChar->setCallbacks(new NonceCallbacks());

  pResponseChar = pService->createCharacteristic(CHAR_RESPONSE_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  pResponseChar->addDescriptor(new BLE2902());

  pService->start();
  
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  BLEDevice::getAdvertising()->setScanResponse(true);
  BLEDevice::getAdvertising()->setMinPreferred(0x06);  
  BLEDevice::startAdvertising();
  
  Serial.println("System Running (Ready for BLE Connections)");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
      if (!client.connected()) {
          reconnectMQTT();
      }
      client.loop();
  }
  vTaskDelay(100 / portTICK_PERIOD_MS);
}