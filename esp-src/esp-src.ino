#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

// --- OLED LIBRARIES ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- CONFIGURATION ---
#define LOCK_PIN 16 
#define PIR_PIN  17 
#define AUTO_LOCK_TIMEOUT_MS 15000 

// OLED Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C // Check Serial Monitor if this is wrong
#define I2C_SDA 21          // Explicitly define SDA pin
#define I2C_SCL 22          // Explicitly define SCL pin

// UUIDs
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// --- GLOBALS ---
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
QueueHandle_t doorQueue;
TimerHandle_t autoLockTimer; 

// Create Display Object
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- HELPER: UPDATE SCREEN ---
void updateDisplay(String status) {
  display.clearDisplay();
  
  // Draw Fixed Room Name "S202"
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(40, 10); // Roughly centered X=40, Y=10
  display.println("S202");

  // Draw Status (LOCKED / UNLOCKED)
  display.setTextSize(2);
  display.setCursor(10, 40); // X=10, Y=40
  display.println(status);

  display.display(); 
}

// --- TIMER CALLBACK ---
void autoLockCallback(TimerHandle_t xTimer) {
  Serial.println("TIMER EXPIRED");
  int closeCommand = 0; 
  xQueueSend(doorQueue, &closeCommand, 0); 
}

// --- DOOR TASK ---
void doorTask(void * parameter) {
  int receivedCommand;
  bool isUnlocked = false; 
  unsigned long lastTimerReset = 0; 

  // Initial Screen
  updateDisplay("LOCKED");

  for(;;) { 
    if (xQueueReceive(doorQueue, &receivedCommand, pdMS_TO_TICKS(100)) == pdTRUE) {
      
      if (receivedCommand == 1) { // OPEN
        Serial.println("CMD: OPEN");
        digitalWrite(LOCK_PIN, HIGH);
        isUnlocked = true;
        xTimerStart(autoLockTimer, 0);
        
        // UPDATE OLED: Show UNLOCKED
        updateDisplay("UNLOCKED");

        if (deviceConnected) {
           pCharacteristic->setValue("Unlocked");
           pCharacteristic->notify();
        }
      }
      else if (receivedCommand == 0) { // CLOSE
        Serial.println("CMD: CLOSE");
        digitalWrite(LOCK_PIN, LOW);
        isUnlocked = false;
        xTimerStop(autoLockTimer, 0);
        
        // UPDATE OLED: Show LOCKED
        updateDisplay("LOCKED");

        if (deviceConnected) {
           pCharacteristic->setValue("Locked");
           pCharacteristic->notify();
        }
      }
    }

    // CHECK PIR
    // Note: We extend the timer logic here but do NOT update the display text
    if (isUnlocked && digitalRead(PIR_PIN) == HIGH) {
      if (millis() - lastTimerReset > 1000) {
        Serial.println("Motion: Timer Reset");
        xTimerReset(autoLockTimer, 0); 
        lastTimerReset = millis();
      }
    }
  }
}

// --- CALLBACKS ---
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("Device Connected");
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("Device Disconnected");
      BLEDevice::startAdvertising(); 
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String value = pCharacteristic->getValue();
      if (value.length() > 0) {
        int commandToSend = -1;
        if (value == "open" || value == "1") commandToSend = 1;
        else if (value == "close" || value == "0") commandToSend = 0;

        if (commandToSend != -1) {
           xQueueSend(doorQueue, &commandToSend, 0); 
        }
      }
    }
};

void setup() {
  Serial.begin(115200);
  
  pinMode(LOCK_PIN, OUTPUT);
  digitalWrite(LOCK_PIN, LOW); 
  pinMode(PIR_PIN, INPUT); 

  // --- I2C INITIALIZATION & DIAGNOSTICS ---
  // 1. Explicitly initialize Wire with the correct pins
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); // FIX: Lower I2C clock to 100kHz for stability
  delay(100); // Give display time to power up

  // 2. Run a quick scan to confirm the screen is connected
  Serial.println("Scanning for I2C devices...");
  byte error, address;
  int nDevices = 0;
  for(address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address<16) Serial.print("0");
      Serial.print(address,HEX);
      Serial.println("  !");
      nDevices++;
    }
  }
  if (nDevices == 0) {
    Serial.println("No I2C devices found. CHECK WIRING (SDA=21, SCL=22)!");
  } else {
    Serial.println("I2C Scan complete.");
  }
  // ----------------------------------------

  // --- OLED SETUP ---
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed. Check Address/Wiring."));
    for(;;); 
  }
  display.clearDisplay();
  display.display();

  doorQueue = xQueueCreate(10, sizeof(int));
  autoLockTimer = xTimerCreate("AutoLock", pdMS_TO_TICKS(AUTO_LOCK_TIMEOUT_MS), pdFALSE, (void*)0, autoLockCallback);

  xTaskCreatePinnedToCore(doorTask, "DoorTask", 4096, NULL, 1, NULL, 1);

  BLEDevice::init("ESP32_Classroom");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->setValue("Locked");
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  
  BLEDevice::startAdvertising();
  
  Serial.println("System Running...");
}

void loop() {
  vTaskDelay(2000 / portTICK_PERIOD_MS);
}