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

#define I2C_SDA    6
#define I2C_SCL    7
#define DHTPIN     4
#define MQ_PIN     0
#define DHTTYPE    DHT22
#define SIM900_TX  19
#define SIM900_RX  18

#define ADC_SATURATED  2100

// Sane bounds for persisted R0 and threshold — scaled for the divided range.
#define R0_MIN           50.0f
#define R0_MAX         2000.0f
#define THRESHOLD_MIN   100
#define THRESHOLD_MAX  2090

HardwareSerial sim900(1);
DHT dht(DHTPIN, DHTTYPE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHAR_GAS_UUID       "12345678-1234-1234-1234-123456789ab1"
#define CHAR_TEMP_UUID      "12345678-1234-1234-1234-123456789ab2"
#define CHAR_HUM_UUID       "12345678-1234-1234-1234-123456789ab3"
#define CHAR_ALERT_UUID     "12345678-1234-1234-1234-123456789ab4"
#define CHAR_THRESHOLD_UUID "12345678-1234-1234-1234-123456789ab5"
#define CHAR_CALIBRATE_UUID "12345678-1234-1234-1234-123456789ab6"
#define CHAR_R0_UUID        "12345678-1234-1234-1234-123456789ab7"
#define CHAR_PHONE_UUID     "12345678-1234-1234-1234-123456789ab8"

// Rolling average — 10 samples
#define N_SMOOTH 10
int  smoothBuf[N_SMOOTH] = {};
int  smoothIdx = 0;
long smoothSum = 0;

void preFillBuffer(int v) {
  for (int i = 0; i < N_SMOOTH; i++) smoothBuf[i] = v;
  smoothSum = (long)v * N_SMOOTH;
  smoothIdx = 0;
}

void pushSample(int v) {
  smoothSum -= smoothBuf[smoothIdx];
  smoothBuf[smoothIdx] = v;
  smoothSum += v;
  smoothIdx = (smoothIdx + 1) % N_SMOOTH;
}

int getSmoothed() { return (int)(smoothSum / N_SMOOTH); }

// State
float  R0 = 10.0f;
int    mqThreshold = 1200;
float  temp = 0, hum = 0;
float  lastGoodTemp = 0, lastGoodHum = 0;
int    gasValue = 0;
bool   saturated = false;
bool   alertActive = false;
bool   deviceConnected = false;
bool   doCalibration = false;
String targetNumber = "";
unsigned long lastSmsTime = 0;
const  unsigned long smsInterval = 30000;

BLECharacteristic *pCharGas, *pCharTemp, *pCharHum, *pCharAlert, *pCharR0;

void updateR0Char() {
  pCharR0->setValue(String((int)R0).c_str());
  if (deviceConnected) pCharR0->notify();
}

// Retries up to 3 times; falls back to last good values on persistent failure.
void readDHT() {
  for (int i = 0; i < 3; i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      temp = lastGoodTemp = t;
      hum  = lastGoodHum  = h;
      return;
    }
    delay(100);
    esp_task_wdt_reset();
  }
  temp = lastGoodTemp;
  hum  = lastGoodHum;
  Serial.println("[DHT] fail — using last good values");
}

// Refuses calibration if alert is active to prevent setting R0 in polluted air.
void runAutoCalibrate() {
  if (alertActive) {
    Serial.println("[CAL] refused — alert active");
    if (deviceConnected) {
      pCharR0->setValue("ERR_DIRTY");
      pCharR0->notify();
      delay(200);
      updateR0Char();
    }
    return;
  }

  long sum = 0;
  for (int i = 0; i < 20; i++) {
    esp_task_wdt_reset();
    sum += analogRead(MQ_PIN);
    delay(10);
  }
  float newR0 = (float)(sum / 20);

  if (newR0 < R0_MIN || newR0 > R0_MAX) {
    Serial.printf("[CAL] result %.0f out of range — aborted\n", newR0);
    return;
  }

  R0 = newR0;
  prefs.putFloat("r0", R0);
  updateR0Char();
  Serial.printf("[CAL] new R0 = %.0f\n", R0);
}

void sendSMS(String message) {
  if (targetNumber.length() < 5) return;
  esp_task_wdt_reset();
  sim900.println("AT");         delay(1000);
  sim900.println("AT+CMGF=1"); delay(1000);
  sim900.print("AT+CMGS=\""); sim900.print(targetNumber); sim900.println("\"");
  delay(2000);
  sim900.print(message);
  delay(1000);
  sim900.write(26);
  for (int i = 0; i < 20; i++) {
    esp_task_wdt_reset(); delay(500);
    while (sim900.available()) Serial.write(sim900.read());
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*)    { deviceConnected = true; }
  void onDisconnect(BLEServer*) { deviceConnected = false; BLEDevice::startAdvertising(); }
};

class PhoneCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pC) {
    targetNumber = pC->getValue().c_str();
    prefs.putString("phone", targetNumber);
  }
};

class ThresholdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pC) {
    int val = atoi(pC->getValue().c_str());
    if (val >= THRESHOLD_MIN && val <= THRESHOLD_MAX) {
      mqThreshold = val;
      prefs.putInt("threshold", mqThreshold);
    } else {
      Serial.printf("[THR] rejected out-of-range value: %d\n", val);
    }
  }
};

// Accepts: "CAL", "R0+NNN", "R0-NNN"
class CalCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pC) {
    String cmd = String(pC->getValue().c_str());
    cmd.trim();

    if (cmd == "CAL") {
      doCalibration = true;
    } else if (cmd.startsWith("R0+")) {
      float delta = cmd.substring(3).toFloat();
      if (delta > 0 && (R0 + delta) <= R0_MAX) {
        R0 += delta;
        prefs.putFloat("r0", R0);
        updateR0Char();
      }
    } else if (cmd.startsWith("R0-")) {
      float delta = cmd.substring(3).toFloat();
      if (delta > 0 && (R0 - delta) >= R0_MIN) {
        R0 -= delta;
        prefs.putFloat("r0", R0);
        updateR0Char();
      }
    }
  }
};

void setup() {
  Serial.begin(115200);
  sim900.begin(9600, SERIAL_8N1, SIM900_RX, SIM900_TX);

  analogSetPinAttenuation(MQ_PIN, ADC_11db);

  prefs.begin("monitor", false);
  targetNumber = prefs.getString("phone", "");

  float storedR0  = prefs.getFloat("r0",        10.0f);
  int   storedThr = prefs.getInt("threshold",   1200);
  R0          = constrain(storedR0,  R0_MIN,        R0_MAX);
  mqThreshold = constrain(storedThr, THRESHOLD_MIN, THRESHOLD_MAX);
  if (storedR0  != R0)          prefs.putFloat("r0",       R0);
  if (storedThr != mqThreshold) prefs.putInt("threshold",  mqThreshold);

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = 15000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_reconfigure(&wdt_config);
  esp_task_wdt_add(NULL);

  Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.begin();
  dht.begin();

  for (int s = 30; s > 0; s--) {
    esp_task_wdt_reset();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(2, 12, "AmmoniaMonitor");
    u8g2.drawHLine(0, 15, 128);
    u8g2.drawStr(2, 30, "Warming up MQ...");
    u8g2.setCursor(2, 44);
    u8g2.print("Ready in: "); u8g2.print(s); u8g2.print(" s");
    u8g2.sendBuffer();
    delay(1000);
  }

  preFillBuffer(analogRead(MQ_PIN));

  BLEDevice::init("AmmoniaMonitor");
  BLEServer*  pServer  = BLEDevice::createServer();
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

  pCharR0 = pService->createCharacteristic(
    CHAR_R0_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharR0->addDescriptor(new BLE2902());
  pCharR0->setValue(String((int)R0).c_str());

  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  BLEDevice::startAdvertising();

  Serial.printf("[BOOT] R0=%.0f threshold=%d phone=%s\n", R0, mqThreshold, targetNumber.c_str());
}

void loop() {
  esp_task_wdt_reset();

  if (doCalibration) {
    doCalibration = false;
    runAutoCalibrate();
  }

  pushSample(analogRead(MQ_PIN));
  gasValue  = getSmoothed();
  saturated = (gasValue >= ADC_SATURATED);

  readDHT();

  alertActive = saturated || (gasValue > mqThreshold);

  // OLED
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(2, 10, deviceConnected ? "APP CONNECTED" : "WAITING BLE...");
  u8g2.drawHLine(0, 13, 128);

  if (saturated) {
    u8g2.drawStr(2, 27, "!! ADC SATURATED !!");
    u8g2.drawStr(2, 39, "CHECK SENSOR/WIRING");
  } else if (alertActive) {
    u8g2.drawStr(2, 27, "!! HIGH NH3 ALERT !!");
    u8g2.setCursor(2, 39);
    u8g2.print("VAL:"); u8g2.print(gasValue);
    u8g2.print(" THR:"); u8g2.print(mqThreshold);
  } else {
    u8g2.setCursor(2, 27);
    u8g2.print("NH3: "); u8g2.print(gasValue);
    u8g2.setCursor(2, 39);
    u8g2.print("THR: "); u8g2.print(mqThreshold);
  }

  u8g2.setCursor(2, 50);
  u8g2.print("R0:"); u8g2.print((int)R0);
  u8g2.print(" T:"); u8g2.print(temp, 1); u8g2.print("C");
  u8g2.setCursor(2, 62);
  u8g2.print("H:"); u8g2.print(hum, 1);
  u8g2.print("% "); u8g2.print(targetNumber != "" ? targetNumber : "NO NUM");
  u8g2.sendBuffer();

  if (deviceConnected) {
    pCharGas->setValue(saturated ? "SAT" : String(gasValue).c_str());
    pCharGas->notify();
    pCharTemp->setValue(String(temp, 1).c_str());  pCharTemp->notify();
    pCharHum->setValue(String(hum, 1).c_str());    pCharHum->notify();
    pCharAlert->setValue(alertActive ? "1" : "0"); pCharAlert->notify();
  }

  if (alertActive) {
    if (millis() - lastSmsTime >= smsInterval) {
      String msg = saturated
        ? "CRITICAL: Ammonia sensor SATURATED — check wiring or ventilate immediately!"
        : "CRITICAL: High Ammonia! Level: " + String(gasValue) + " | Threshold: " + String(mqThreshold);
      sendSMS(msg);
      lastSmsTime = millis();
    }
  } else if (gasValue < (mqThreshold - 300)) {
    lastSmsTime = 0;
  }

  delay(1000);
}

