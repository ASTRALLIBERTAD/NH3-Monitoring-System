#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include "esp_task_wdt.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

#define I2C_SDA 6
#define I2C_SCL 7
#define DHTPIN  4
#define MQ_PIN  0
#define DHTTYPE DHT22
#define SIM900_TX 19
#define SIM900_RX 18

HardwareSerial sim900(1);
DHT dht(DHTPIN, DHTTYPE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;

#define SERVICE_UUID           "12345678-1234-1234-1234-123456789abc"
#define CHAR_GAS_UUID          "12345678-1234-1234-1234-123456789ab1"
#define CHAR_TEMP_UUID         "12345678-1234-1234-1234-123456789ab2"
#define CHAR_HUM_UUID          "12345678-1234-1234-1234-123456789ab3"
#define CHAR_ALERT_UUID        "12345678-1234-1234-1234-123456789ab4"
#define CHAR_THRESHOLD_UUID    "12345678-1234-1234-1234-123456789ab5"
#define CHAR_CALIBRATE_UUID    "12345678-1234-1234-1234-123456789ab6"
#define CHAR_R0_UUID           "12345678-1234-1234-1234-123456789ab7"
#define CHAR_PHONE_UUID        "12345678-1234-1234-1234-123456789ab8"

float R0 = 10.0f;
int mqThreshold = 2500;
float temp = 0, hum = 0;
int gasValue = 0;
bool alertActive = false;
bool deviceConnected = false;
bool doCalibration = false;
String targetNumber = "";
unsigned long  lastSmsTime = 0;
const unsigned long smsInterval = 30000;

BLECharacteristic *pCharGas, *pCharTemp, *pCharHum, *pCharAlert, *pCharR0;

void sendSMS(String message) {
  if (targetNumber.length() < 5) return;
  Serial.println("--- SMS ATTEMPT ---");
  esp_task_wdt_reset();
  
  sim900.println("AT"); 
  delay(1000);
  sim900.println("AT+CMGF=1"); 
  delay(1000);

  sim900.print("AT+CMGS=\"");
  sim900.print(targetNumber);
  sim900.println("\"");
  
  delay(2000); 
  
  sim900.print(message);
  delay(1000);
  sim900.write(26); 
  
  for(int i=0; i<20; i++) { 
    esp_task_wdt_reset(); 
    delay(500); 
    while(sim900.available()) Serial.write(sim900.read());
  }
  Serial.println("\n--- ATTEMPT END ---");
}

// void callSMS() {
//   if (targetNumber.length() < 5) return;
//   Serial.println("--- CALL ATTEMPT ---");

//   sim900.print("ATD"); 
//   sim900.print(targetNumber);
//   sim900.println(";");

//   for(int i=0; i<40; i++) { 
//     esp_task_wdt_reset(); 
//     delay(500); 
//   }
  
//   sim900.println("ATH"); 
//   delay(100);
  
//   for(int i=0; i<20; i++) { 
//     esp_task_wdt_reset(); 
//     delay(500); 
//     while(sim900.available()) Serial.write(sim900.read());
//   }

//   Serial.println("\n--- ATTEMPT END ---");
// }

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pS) { deviceConnected = true; }
  void onDisconnect(BLEServer* pS) { deviceConnected = false; BLEDevice::startAdvertising(); }
};

class PhoneCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pC) {
    targetNumber = pC->getValue().c_str();
    prefs.putString("phone", targetNumber);
  }
};

class ThresholdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pC) {
    int val = pC->getValue().c_str() ? atoi(pC->getValue().c_str()) : 0;
    if (val > 0) {
      mqThreshold = val;
      prefs.putInt("threshold", mqThreshold);
    }
  }
};

class CalCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pC) { if (pC->getValue() == "CAL") doCalibration = true; }
};

void setup() {
  Serial.begin(115200);
  sim900.begin(9600, SERIAL_8N1, SIM900_RX, SIM900_TX);
  prefs.begin("monitor", false);
  targetNumber = prefs.getString("phone", "");
  mqThreshold  = prefs.getInt("threshold", 2500);
  
  esp_task_wdt_config_t wdt_config = { 
    .timeout_ms = 15000, 
    .idle_core_mask = 0, 
    .trigger_panic = true 
  };
  esp_task_wdt_reconfigure(&wdt_config);
  esp_task_wdt_add(NULL);

  Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.begin();
  dht.begin();

  BLEDevice::init("AmmoniaMonitor");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* pService = pServer->createService(SERVICE_UUID);

  pCharGas = pService->createCharacteristic(CHAR_GAS_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pCharGas->addDescriptor(new BLE2902());
  pCharTemp = pService->createCharacteristic(CHAR_TEMP_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pCharTemp->addDescriptor(new BLE2902());
  pCharHum = pService->createCharacteristic(CHAR_HUM_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pCharHum->addDescriptor(new BLE2902());
  pCharAlert = pService->createCharacteristic(CHAR_ALERT_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pCharAlert->addDescriptor(new BLE2902());

  BLECharacteristic* pThreshold = pService->createCharacteristic(CHAR_THRESHOLD_UUID, BLECharacteristic::PROPERTY_WRITE);
  pThreshold->setCallbacks(new ThresholdCallbacks());
  BLECharacteristic* pPhone = pService->createCharacteristic(CHAR_PHONE_UUID, BLECharacteristic::PROPERTY_WRITE);
  pPhone->setCallbacks(new PhoneCallbacks());
  BLECharacteristic* pCal = pService->createCharacteristic(CHAR_CALIBRATE_UUID, BLECharacteristic::PROPERTY_WRITE);
  pCal->setCallbacks(new CalCallbacks());
  pCharR0 = pService->createCharacteristic(CHAR_R0_UUID, BLECharacteristic::PROPERTY_READ);

  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  BLEDevice::startAdvertising();
}

void loop() {
  esp_task_wdt_reset();
  gasValue = analogRead(MQ_PIN);
  temp = dht.readTemperature();
  hum = dht.readHumidity();
  
  if (isnan(temp)) temp = 0; 
  if (isnan(hum)) hum = 0;
  
  alertActive = (gasValue > mqThreshold);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(2, 10, deviceConnected ? "APP CONNECTED" : "WAITING BLE...");
  u8g2.drawHLine(0, 13, 128);
  
  if (alertActive) {
    u8g2.drawStr(2, 30, "ALERT: HIGH NH3!");
    u8g2.setCursor(2, 42); u8g2.print("VALUE: "); u8g2.print(gasValue);
  } else {
    u8g2.setCursor(2, 30); u8g2.print("NH3: "); u8g2.print(gasValue);
  }
  
  u8g2.setCursor(2, 52); u8g2.print("T: "); u8g2.print(temp,1); u8g2.print("C H: "); u8g2.print(hum,1); u8g2.print("%");
  u8g2.setCursor(2, 62); u8g2.print("NUM: "); u8g2.print(targetNumber != "" ? targetNumber : "NONE");
  u8g2.sendBuffer();

  if (deviceConnected) {
    pCharGas->setValue(String(gasValue).c_str()); pCharGas->notify();
    pCharTemp->setValue(String(temp, 1).c_str()); pCharTemp->notify();
    pCharHum->setValue(String(hum, 1).c_str()); pCharHum->notify();
    pCharAlert->setValue(alertActive ? "1" : "0"); pCharAlert->notify();
  }

  if (alertActive) {
    if (millis() - lastSmsTime >= smsInterval) {
      sendSMS("CRITICAL: High Ammonia! Level: " + String(gasValue));
      
      // callSMS();
      
      lastSmsTime = millis();
    }
  } else if (!alertActive && gasValue < (mqThreshold - 300)) {
    lastSmsTime = 0;
  }
  
  delay(1000);
}
