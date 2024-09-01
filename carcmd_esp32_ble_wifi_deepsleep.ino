//
// 1.BLE掃描與設備檢測：
//   程式使用ESP32的BLE功能掃描附近的藍牙設備，並根據RSSI（信號強度指標）來判斷目標設備是否在範圍內。
//   當檢測到目標設備（根據MAC地址和RSSI判斷）時，控制指定的GPIO針腳進行動作。
//   如果設備在上次掃描中存在，而這次掃描中消失，則會觸發另一個動作。
// 2.Wi-Fi連接與HTTP請求：
//   系統每次喚醒後，如果沒有檢測到藍牙設備，則會嘗試連接指定的Wi-Fi網路並發送HTTP請求到指定的伺服器。
//   如果收到伺服器的回應指示啟動操作，系統將會觸發繼電器以啟動設備（例如汽車）。
// 3.深度睡眠管理：
//   系統利用深度睡眠來節省電能。每次喚醒後，系統會進行BLE掃描和Wi-Fi連接操作，然後再次進入深度睡眠。
//   深度睡眠時間設定為2秒，每次喚醒後檢查特定狀態以決定下一步操作。
// 4.狀態保存：
//   程式使用RTC內存保存上次藍牙檢測狀態和深度睡眠次數，以便在ESP32喚醒後可以繼續判斷是否需要進行Wi-Fi操作或觸發動作。
#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <esp_sleep.h>
#include "esp32-hal-cpu.h"

#define RELAY_PIN_A  5  // GPIO 5 鎖門
#define RELAY_PIN_B  4 // GPIO 4 發車
#define RELAY_PIN_C  15  // GPIO 12 開門
#define POWER_PIN 25  // GPIO 25
#define RSSI_THRESHOLD -90  // RSSI 閾值
#define RSSI_SECOND_THRESHOLD -110  // 第二層RSSI閾值
#define TIMEOUT 3000  // 超時時間3秒（3000毫秒）
// 定義藍牙設備的MAC地址
// String targetMacAddress = "df:65:fd:1f:21:22";
String targetMacAddress = "20:22:05:26:00:8d";

const char* ssid = "opposky";
const char* password = "0988085240";
const char* url = "http://www.inskychen.com/carcmd/checkcarboot.php";

BLEScan* pBLEScan;
unsigned long lastDetectedTime = 0;  // 上次檢測到目標設備的時間戳
bool deviceDetected = false;  // 記錄設備是否被檢測到
bool bluetoothDeviceDetected = false;  // 標誌位，記錄是否檢測到藍牙設備
int mvRssi;

// int sleepCounter = 0;  // 記錄深度睡眠次數

RTC_DATA_ATTR bool lastBluetoothDetected = false;  // 使用RTC內存保存上次藍牙檢測狀態
RTC_DATA_ATTR int sleepCounter = 0;  // 使用RTC內存保存深度睡眠次數

void setup() {
  Serial.begin(115200);
  // 將核心速度設置為80 MHz
  setCpuFrequencyMhz(80);

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); // 創建新的掃描對象
  pBLEScan->setActiveScan(false);   // 設置為主動掃描模式
  pBLEScan->setInterval(100);      // 設置掃描間隔
  pBLEScan->setWindow(99);         // 設置掃描窗口，應該小於或等於間隔值

  // 設置為輸出模式，初始狀態為低電平
  pinMode(RELAY_PIN_C, OUTPUT);
  pinMode(RELAY_PIN_A, OUTPUT);
  pinMode(RELAY_PIN_B, OUTPUT);
  digitalWrite(RELAY_PIN_C, HIGH);
  digitalWrite(RELAY_PIN_A, HIGH);
  digitalWrite(RELAY_PIN_B, HIGH);
  
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, LOW);  // 初始状态下关闭电源输出
  
  // 檢查深度睡眠恢復的次數
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    sleepCounter++;
    Serial.println(sleepCounter);
  } else {
    sleepCounter = 0;  // 如果是第一次啟動或非計時器喚醒，則重置計數器
  }
  
  // 先掃描藍牙設備
  mvRssi = -999;
  BLEScanResults* foundDevices = pBLEScan->start(2, false);
  bool found = handleScanResults(*foundDevices);
  pBLEScan->clearResults();

  if (found) {
    bluetoothDeviceDetected = true;  // 檢測到藍牙設備，設置標誌位
    lastDetectedTime = millis();  // 更新最後檢測到目標設備的時間戳
    if (!deviceDetected && !lastBluetoothDetected) {
      // 如果之前未檢測到設備，現在檢測到了，且上次未檢測到設備
      // 打开3.3V电源输出
      digitalWrite(POWER_PIN, HIGH);
      delay(1000);
      Serial.println("開門1(藍牙信標出現）");
      digitalWrite(RELAY_PIN_C, LOW);
      delay(1000);
      digitalWrite(RELAY_PIN_C, HIGH);
      digitalWrite(POWER_PIN, LOW);
      deviceDetected = true;
      // Serial.println("開門2");

    }
    lastBluetoothDetected = true;  // 更新RTC內存中的檢測狀態
  } else {
    if (lastBluetoothDetected) {
      // 如果上次檢測到設備，且這次沒有檢測到，則進行第二次檢查
      // BLEScanResults* verifyDevices = pBLEScan->start(1, false);
      // bool verify = verifyScanResults(*verifyDevices);
      // pBLEScan->clearResults();
      // Serial.println("check1");
      // Serial.println(mvRssi);
      // Serial.println(RSSI_SECOND_THRESHOLD);
      
      if (mvRssi <= RSSI_SECOND_THRESHOLD){
      // if (!verify) {
        // 如果第二次檢查確實沒檢測到設備，觸發未檢測到藍牙設備的動作
        // 打开3.3V电源输出
        digitalWrite(POWER_PIN, HIGH);
        delay(1000);
        Serial.println("鎖門1（藍牙信標消失）");
        digitalWrite(RELAY_PIN_A, LOW);
        delay(1000);
        digitalWrite(RELAY_PIN_A, HIGH);
        // 關閉3.3V电源输出
        digitalWrite(POWER_PIN, LOW);
        deviceDetected = false;
        bluetoothDeviceDetected = false;  // 設備消失，清除藍牙設備標誌位
        lastBluetoothDetected = false;  // 更新RTC內存中的檢測狀態
        // Serial.println("鎖門2");
      }
    }
  }

  // 判斷是否應該進行 Wi-Fi 操作
  if (sleepCounter >= 10) {
    sleepCounter = 0;  // 重置計數器
    if (!lastBluetoothDetected) {
      connectToWiFi();

      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        WiFiClient client;

        // 使用 WiFiClient 和 URL 初始化 HTTPClient
        http.begin(client, url);

        int httpCode = http.GET();  // 發送請求並獲取響應代碼

        if (httpCode > 0) {
          String payload = http.getString();  // 獲取網頁內容

          // 檢查 "boot" 標籤是否為 "enable"
          if (payload.indexOf("<boot>boot</boot>") != -1) {
            triggerRelays();
          } else {
            Serial.println("Boot is not enabled.");
          }
        } else {
          Serial.printf("GET failed, error: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();  // 結束 HTTP 請求
      }

      WiFi.disconnect();  // 斷開 Wi-Fi 連接
      Serial.println("斷開WiFi, 進入深度睡眠模式...");
    } else {
      Serial.println("偵測到藍牙信標，跳過本次WIFI動作");
    }
  }
  
  // 進入深度睡眠模式
  // Serial.println("進入深度睡眠模式...");
  esp_sleep_enable_timer_wakeup(3 * 1000000);  // 3秒後喚醒
  esp_deep_sleep_start();
}

void loop() {
  // 空的主循環
}

bool handleScanResults(BLEScanResults& foundDevices) {
  int count = foundDevices.getCount();
  for (int i = 0; i < count; i++) {
    BLEAdvertisedDevice device = foundDevices.getDevice(i);
    int rssi = device.getRSSI(); // 獲取 RSSI 值
    if (device.getAddress().toString() == targetMacAddress ) {
      // Serial.print("rssi:");
      // Serial.println(rssi);
      mvRssi = rssi;
    }
    if (device.getAddress().toString() == targetMacAddress && rssi > RSSI_THRESHOLD) {
      return true;  // 找到符合條件的設備
    }
    // if (device.getAddress().toString() == targetMacAddress && rssi <= RSSI_SECOND_THRESHOLD) {
    //   return false;  // 找到符合條件的設備
    // }
  }
  return false;  // 沒有找到符合條件的設備
}

void connectToWiFi() {
  int iCount = 0;

  // 掃描 Wi-Fi 網絡
  int n = WiFi.scanNetworks();
  bool ssidFound = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      ssidFound = true;
      break;
    }
  }

  // 如果未找到目標 SSID，等待一段時間然後再重試
  if (!ssidFound) {
    Serial.println("未找到目標 SSID，跳過本次WIFI動作...");
    return;
  }

  // 連接 Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1500);
    Serial.print(".");
    iCount++;
    if (iCount > 20) {
      Serial.println("停止連接");
      break;
    }
  }
  if (iCount <= 20) {
    Serial.println("WiFi connected");
  } 
}

void triggerRelays() {
  Serial.println("Engine start");
  // 打开3.3V电源输出
  digitalWrite(POWER_PIN, HIGH);
  delay(1000);

  // 觸發繼電器 A 一秒
  digitalWrite(RELAY_PIN_A, LOW);
  delay(1500);
  digitalWrite(RELAY_PIN_A, HIGH);
  delay(500);

  // 觸發繼電器 B 4秒
  digitalWrite(RELAY_PIN_B, LOW);
  delay(4000);
  digitalWrite(RELAY_PIN_B, HIGH);

  send_line("BVB-7980 啟動中...");
  // 關閉3.3V电源输出
  digitalWrite(POWER_PIN, LOW);
  delay(30000);
}

void send_line(String msg) {
  WiFiClient client;
  HTTPClient http;

  String encodedString = base64::encode(msg);
  String url = "http://www.inskychen.com/sendtoline_car.php?msg=" + encodedString;
  http.begin(client, url);

  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println(httpCode);
    Serial.println(payload);
  } else {
    Serial.println("Error on HTTP request");
  }

  http.end();
}

bool verifyScanResults(BLEScanResults& foundDevices) {
  int count = foundDevices.getCount();
  for (int i = 0; i < count; i++) {
    BLEAdvertisedDevice device = foundDevices.getDevice(i);
    int rssi = device.getRSSI(); // 獲取 RSSI 值
    if (device.getAddress().toString() == targetMacAddress && rssi > RSSI_SECOND_THRESHOLD) {
      return true;  // RSSI 小於 -75，但大於 -90，視為設備仍然存在
    }
  }
  return false;  // RSSI 小於 -90，視為設備消失
}