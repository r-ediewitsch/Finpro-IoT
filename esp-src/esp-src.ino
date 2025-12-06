#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

// --- OLED LIBRARIES ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==========================================
// 1. HARDWARE CONFIGURATION
// ==========================================
#define LOCK_PIN            16  // From OLED Code
#define PIR_PIN             17  // From OLED Code
#define STATUS_LED          2   // Built-in LED
#define AUTO_LOCK_TIMEOUT_MS 15000 

// OLED Settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C 
#define I2C_SDA 21          
#define I2C_SCL 22          

// ==========================================
// 2. BLE CONFIGURATION
// ==========================================
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_ID_UUID        "beb5483e-36e1-4688-b7f5-ea07361b26a8" 
#define CHAR_NONCE_UUID     "d29a73d5-1234-4567-8901-23456789abcd" 
#define CHAR_RESPONSE_UUID  "1c95d5e3-d8bc-4e31-9989-13e6184a44b9" 

// Identity of THIS Lock
const char* THIS_ROOM = "ROOM_404"; 

// ==========================================
// 3. USER DATABASE
// ==========================================
struct User {
    String id;
    String key;
    String role;        
    String allowedRoom; 
};

User userDatabase[] = {
    // ID          Key            Role        Allowed Room
    {"ADM_001",    "masterkey",   "ADMIN",    "ALL"},      
    {"LEC_A",      "mathpass",    "LECTURER", "ROOM_303"}, 
    {"LEC_B",      "englishpass", "LECTURER", "ROOM_404"}  
};
int userCount = sizeof(userDatabase) / sizeof(userDatabase[0]);

// ==========================================
// 4. GLOBAL OBJECTS
// ==========================================
// BLE Globals
BLEServer* pServer = NULL;
BLECharacteristic* pResponseChar = NULL; // Used for Hash AND Status updates
bool deviceConnected = false;
bool idVerified = false;
int currentUserIndex = -1;

// OLED & RTOS Globals
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
QueueHandle_t doorQueue;
TimerHandle_t autoLockTimer; 

// ==========================================
// 5. HELPER FUNCTIONS (Logic & Display)
// ==========================================

void updateDisplay(String status) {
  display.clearDisplay();
  
  // Room Name
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 10); 
  display.println(THIS_ROOM);

  // Status
  display.setTextSize(2);
  display.setCursor(10, 40); 
  display.println(status);
  display.display(); 
}

// Convert bytes to Hex String
String toHexString(uint8_t* data, size_t len) {
    String output = "";
    char buff[3];
    for (size_t i = 0; i < len; i++) {
        sprintf(buff, "%02x", data[i]);
        output += buff;
    }
    return output;
}

// Calculate HMAC-SHA256
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

// TRIGGER FUNCTION: Sends command to the Task (Non-blocking)
void triggerDoorUnlock() {
    int cmd = 1; // 1 = OPEN
    xQueueSend(doorQueue, &cmd, 0);
}

// ==========================================
// 6. TASKS & TIMERS (The "Muscle")
// ==========================================

// Timer Callback: Runs when 15 seconds passes
void autoLockCallback(TimerHandle_t xTimer) {
  Serial.println("TIMER EXPIRED: Auto-locking...");
  int closeCommand = 0; 
  xQueueSend(doorQueue, &closeCommand, 0); 
}

// Main Door Task: Handles OLED, Relay, and PIR
void doorTask(void * parameter) {
  int receivedCommand;
  bool isUnlocked = false; 
  unsigned long lastTimerReset = 0; 

  updateDisplay("LOCKED"); // Initial State

  for(;;) { 
    // Wait for command from BLE or Timer
    if (xQueueReceive(doorQueue, &receivedCommand, pdMS_TO_TICKS(100)) == pdTRUE) {
      
      // --- COMMAND: OPEN ---
      if (receivedCommand == 1) { 
        Serial.println("TASK: Unlocking Hardware");
        digitalWrite(LOCK_PIN, HIGH);
        digitalWrite(STATUS_LED, HIGH);
        isUnlocked = true;
        xTimerStart(autoLockTimer, 0); // Start Auto-lock
        updateDisplay("UNLOCKED");
        
        // Notify App if connected
        if (deviceConnected && pResponseChar != NULL) {
            pResponseChar->setValue("UNLOCKED");
            pResponseChar->notify();
        }
      }
      // --- COMMAND: CLOSE ---
      else if (receivedCommand == 0) { 
        Serial.println("TASK: Locking Hardware");
        digitalWrite(LOCK_PIN, LOW);
        digitalWrite(STATUS_LED, LOW);
        isUnlocked = false;
        xTimerStop(autoLockTimer, 0);
        updateDisplay("LOCKED");
        
        // Notify App if connected
        if (deviceConnected && pResponseChar != NULL) {
            pResponseChar->setValue("LOCKED");
            pResponseChar->notify();
        }
      }
    }

    // --- PIR MOTION EXTENSION ---
    // If door is unlocked AND motion detected -> Reset Timer
    if (isUnlocked && digitalRead(PIR_PIN) == HIGH) {
      if (millis() - lastTimerReset > 1000) {
        Serial.println("Motion Detected: Extending Unlock Timer");
        xTimerReset(autoLockTimer, 0); 
        lastTimerReset = millis();
      }
    }
  }
}

// ==========================================
// 7. BLE CALLBACKS (The "Brain")
// ==========================================

class ServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("Device Connected");
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      idVerified = false;
      currentUserIndex = -1;
      Serial.println("Device Disconnected");
      BLEDevice::startAdvertising(); 
    }
};

class IDCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String rxValue = pCharacteristic->getValue().c_str();

        // CLEANUP
        while (rxValue.length() > 0 && 
              (rxValue[rxValue.length()-1] == 0 || 
               rxValue[rxValue.length()-1] == '\n' || 
               rxValue[rxValue.length()-1] == '\r' ||
               rxValue[rxValue.length()-1] == ' ')) {
            rxValue.remove(rxValue.length()-1);
        }

        if (rxValue.length() > 0) {
            Serial.print("Rx ID: ["); Serial.print(rxValue); Serial.println("]");

            // --- COMMAND: OPEN ---
            if (rxValue == "OPEN") {
                if (idVerified && currentUserIndex != -1) {
                    String role = userDatabase[currentUserIndex].role;
                    String targetRoom = userDatabase[currentUserIndex].allowedRoom;
                    
                    if (role == "ADMIN") {
                        Serial.println("Access: GRANTED (Admin)");
                        triggerDoorUnlock(); // Calls the Queue/Task
                    } 
                    else if (role == "LECTURER" && targetRoom == THIS_ROOM) {
                        Serial.println("Access: GRANTED (Lecturer)");
                        triggerDoorUnlock(); // Calls the Queue/Task
                    } 
                    else {
                         Serial.println("Access: DENIED (Wrong Room)");
                         // Notify App of specific failure
                         pResponseChar->setValue("DENIED_ROOM");
                         pResponseChar->notify();
                    }
                } else {
                    Serial.println("Error: Not Authenticated.");
                }
            }
            // --- IDENTIFICATION: USER ID ---
            else {
                bool found = false;
                for(int i=0; i < userCount; i++) {
                    if (userDatabase[i].id == rxValue) {
                        currentUserIndex = i;
                        idVerified = true;
                        found = true;
                        Serial.print("User Identified: "); Serial.println(userDatabase[i].id);
                        break;
                    }
                }
                if(!found) {
                    idVerified = false;
                    currentUserIndex = -1;
                    Serial.println("Unknown User ID.");
                }
            }
        }
    }
};

class NonceCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String nonce = pCharacteristic->getValue().c_str();

        // CLEANUP
        while (nonce.length() > 0 && 
              (nonce[nonce.length()-1] == 0 || 
               nonce[nonce.length()-1] == '\n' || 
               nonce[nonce.length()-1] == '\r' ||
               nonce[nonce.length()-1] == ' ')) {
            nonce.remove(nonce.length()-1);
        }

        if (nonce.length() > 0) {
            Serial.print("Nonce: "); Serial.println(nonce);

            if(idVerified && currentUserIndex != -1) {
                String userKey = userDatabase[currentUserIndex].key;
                uint8_t hmacResult[32];
                calculateHMAC(nonce, userKey, hmacResult);
                String hexSignature = toHexString(hmacResult, 32);
                
                // Send Hash
                pResponseChar->setValue(hexSignature.c_str());
                pResponseChar->notify();
                Serial.print("Sent Hash: "); Serial.println(hexSignature);
            } else {
                Serial.println("Ignored Nonce: User not verified.");
            }
        }
    }
};

// ==========================================
// 8. SETUP & LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  
  // --- GPIO SETUP ---
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  digitalWrite(LOCK_PIN, LOW); // Locked on boot
  digitalWrite(STATUS_LED, LOW);

  // --- I2C / OLED SETUP ---
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); 
  delay(100);

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed. Check wiring."));
    for(;;); 
  }
  display.clearDisplay();
  display.display();

  // --- RTOS SETUP ---
  doorQueue = xQueueCreate(10, sizeof(int));
  autoLockTimer = xTimerCreate("AutoLock", pdMS_TO_TICKS(AUTO_LOCK_TIMEOUT_MS), pdFALSE, (void*)0, autoLockCallback);
  // Start the Door Task on Core 1
  xTaskCreatePinnedToCore(doorTask, "DoorTask", 4096, NULL, 1, NULL, 1);

  // --- BLE SETUP ---
  BLEDevice::init("ESP32_Smart_Lock"); // Combined Name
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // ID Characteristic
  BLECharacteristic *pIDChar = pService->createCharacteristic(CHAR_ID_UUID, BLECharacteristic::PROPERTY_WRITE);
  pIDChar->setCallbacks(new IDCallbacks());

  // Nonce Characteristic
  BLECharacteristic *pNonceChar = pService->createCharacteristic(CHAR_NONCE_UUID, BLECharacteristic::PROPERTY_WRITE);
  pNonceChar->setCallbacks(new NonceCallbacks());

  // Response Characteristic
  pResponseChar = pService->createCharacteristic(CHAR_RESPONSE_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  pResponseChar->addDescriptor(new BLE2902());

  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  
  BLEDevice::startAdvertising();
  
  Serial.println("System Running: Secure OLED Lock");
}

void loop() {
  // Main loop is empty because FreeRTOS Task handles the door
  vTaskDelay(2000 / portTICK_PERIOD_MS);
}