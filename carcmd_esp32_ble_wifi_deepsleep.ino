#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>
#include "esp32-hal-cpu.h"
#include <Wire.h>
#include <RTClib.h>  // 引入 DS3231 RTC 庫

#define RELAY_PIN_A  5  // GPIO 5 鎖門
#define RELAY_PIN_B  4  // GPIO 4 發車
#define RELAY_PIN_C  15 // GPIO 15 開門
#define POWER_PIN 26    // GPIO 26 鑰匙的3.3v電源
#define RSSI_THRESHOLD -110  // RSSI 閾值
//#define RSSI_THRESHOLD -30 // RSSI 閾值
#define RSSI_SECOND_THRESHOLD -130  // 第二層 RSSI 閾值

String targetMacAddress = "20:22:05:26:00:8d";
RTC_DS3231 rtc;

const char* ssid = "opposky";
const char* password = "0988085";
const char* url = "http://www.url.com/carcmd/checkcarboot.php";

NimBLEScan* pBLEScan;
bool deviceDetected = false;  // 記錄設備是否被檢測到
bool bluetoothDeviceDetected = false;  // 標誌位，記錄是否檢測到藍牙設備
int  mvRssi;
bool BluetoothInRange = false;
int thisAct = 0;  // 本次 act
bool isDeepRest = false;

RTC_DATA_ATTR bool powerOn = true;  // 使用 RTC記憶體記錄是否為第一次上電，第一次上電不執行實際開鎖門動作
RTC_DATA_ATTR bool lastBluetoothDetected = false;  // 使用 RTC 記憶體保存上次藍牙檢測狀態
RTC_DATA_ATTR int sleepCounter = 0;  // 使用 RTC 記憶體保存深度睡眠次數
RTC_DATA_ATTR int preAct = 1;  // 使用 RTC 記憶體保存上次 act 0:fast 1:slow 2:open 3:lock
                               // 因為使用string在RTC處理麻煩所以用整數代碼
void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);  // 將核心速度設為80 MHz
  thisAct = preAct;
  // 初始化 I2C 和 RTC
  Wire.begin(21, 22); // SDA = 21, SCL = 22
  if (!rtc.begin()) {
    Serial.println("無法找到 RTC");
    while (1);
  }

  // 如果 RTC 斷電，設置時間
  if (rtc.lostPower()) {
    Serial.println("RTC 斷電，設置時間!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));  // 設置 RTC 為編譯時間
  }
  // 設置為輸出模式，初始狀態為低電平  
  pinMode(RELAY_PIN_C, OUTPUT);
  digitalWrite(RELAY_PIN_C, HIGH);
  pinMode(RELAY_PIN_A, OUTPUT);
  digitalWrite(RELAY_PIN_A, HIGH);
  pinMode(RELAY_PIN_B, OUTPUT);
  digitalWrite(RELAY_PIN_B, HIGH);
  pinMode(POWER_PIN, OUTPUT); //for key power
  digitalWrite(POWER_PIN, LOW);  // 初始狀態下關閉電源輸出
  
  // 檢查當前時間，如果在09:00到12:00之間，進入深度睡眠10秒
  // 顯示當前時間
  DateTime now = rtc.now();
  Serial.print("目前時間：");
  Serial.print(now.year(), DEC);
  Serial.print('/');
  Serial.print(now.month(), DEC);
  Serial.print('/');
  Serial.print(now.day(), DEC);
  Serial.print(" ");
  Serial.print(now.hour(), DEC);
  Serial.print(':');
  Serial.print(now.minute(), DEC);
  Serial.print(':');
  Serial.println(now.second(), DEC);
  // 獲取星期幾（0 是星期天，1 是星期一，以此類推）
  uint8_t dayOfWeek = now.dayOfTheWeek();
  // 檢查當前時間，並根據條件進入深度睡眠模式
  isDeepRest = false;
  if (dayOfWeek >= 1 && dayOfWeek <= 7) { // 週一到週日
    if (now.hour() >= 21 || now.hour() < 7) {
      isDeepRest = true;
    }
  }
  if (dayOfWeek >= 1 && dayOfWeek <= 4) { // 週一到週四
    if ((now.hour() >= 9 && now.hour() < 12) || (now.hour() >= 13 && now.hour() < 17)) {
      isDeepRest = true;
    }
  }
  if (dayOfWeek = 5) { // 週五
    if ((now.hour() >= 9 && now.hour() < 12)) {
      isDeepRest = true;
    }
  }
  //for test
  if (now.minute() > 20 && now.minute() < 30){
    isDeepRest = false;
  }
  // 檢查深度睡眠恢復的次數
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    sleepCounter++;
    Serial.println(sleepCounter);
  } else {
    sleepCounter = 0;  // 如果是第一次啟動或非計時器喚醒，則重置計數器
  }

  // 先掃描藍牙設備
  if (!isDeepRest){
    NimBLEDevice::init("");  // 使用 NimBLE 庫初始化
    NimBLEDevice::setPower(ESP_PWR_LVL_N14);  // 設置率級別

    pBLEScan = NimBLEDevice::getScan();  // 創建 NimBLE 掃描對象
    pBLEScan->setActiveScan(false);  // 設置為被動掃描模式
    pBLEScan->setInterval(100);  // 設置掃描間隔
    pBLEScan->setWindow(99);  // 設置掃描窗口，應該小於或等於間隔值
    
    mvRssi = -999;
    NimBLEScanResults foundDevices = pBLEScan->start(2, false);  // 開始掃描
    bool found = handleScanResults(foundDevices);
    pBLEScan->clearResults();  // 清除掃描結果
    //若上次動作為開門，但這次卻掃不到藍牙信標，需判斷是否因為藍牙信標突然消失所致，故要再檢查一次
    if (!found && preAct == 2){
      Serial.println("設備消失，第2次確認");
      NimBLEScanResults foundDevices = pBLEScan->start(1, false);  
      found = handleScanResults(foundDevices);
      pBLEScan->clearResults();  // 清除掃描結果  
    }
    if (!found && preAct == 2){
      Serial.println("設備消失，第3次確認");
      NimBLEScanResults foundDevices = pBLEScan->start(1, false);  
      found = handleScanResults(foundDevices);
      pBLEScan->clearResults();  // 清除掃描結果  
    }
    //若在藍牙範圍內但是不到觸發值，持續掃描一段時間
    if (preAct != 2 && BluetoothInRange && !found){
      for (int i = 0; i < 30; i++) {
        Serial.print("進入快速掃描");
        Serial.println(i);
        NimBLEScanResults foundDevices = pBLEScan->start(1, false);  
        found = handleScanResults(foundDevices);
        if (found){
          break;
        }
      }  
    }
    if (found) {
      bluetoothDeviceDetected = true;  // 檢測到藍牙設備，設置標誌位
      if (!deviceDetected && !lastBluetoothDetected) {
        if (!powerOn){
          digitalWrite(POWER_PIN, HIGH);
          delay(100);
          digitalWrite(RELAY_PIN_C, LOW);
          delay(1000);
          digitalWrite(RELAY_PIN_C, HIGH);
          digitalWrite(POWER_PIN, LOW);
        }
        Serial.println("開門(藍牙信標出現)");
        deviceDetected = true;
        BluetoothInRange = false;
        thisAct = 2;
      }
      lastBluetoothDetected = true;  // 更新 RTC 記憶體中的檢測狀態
    } else {
      if (lastBluetoothDetected) {
        if (mvRssi <= RSSI_SECOND_THRESHOLD) {
          if (!powerOn){
            digitalWrite(POWER_PIN, HIGH);
            delay(100);
            digitalWrite(RELAY_PIN_A, LOW);
            delay(1000);
            digitalWrite(RELAY_PIN_A, HIGH);
            digitalWrite(POWER_PIN, LOW);
          }
          Serial.println("鎖門(藍牙信標消失)");
          deviceDetected = false;
          bluetoothDeviceDetected = false;  // 設備消失，清除藍牙設備標誌位
          lastBluetoothDetected = false;  // 更新 RTC 記憶體中的檢測狀態
          BluetoothInRange = false;
          thisAct = 3;
        }
      }
    } 
  }
  
  powerOn = false; //用來判斷是否第一次上電的循環，所以跑到這就要讓他false
  // 判斷是否應該進行 Wi-Fi 操作
  if (sleepCounter >= 24) {
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
      
      Serial.println("斷開 WiFi, 進入深度睡眠模式...");
      thisAct = 1;
      WiFi.disconnect();  // 斷開 Wi-Fi 連接 
    } else {
      Serial.println("檢測到藍牙信標，跳過本次 WiFi 動作");
    }
  }

  // 進入深度睡眠模式
  if (thisAct == preAct && thisAct != 0 && preAct != 0){ //0=fast
    BluetoothInRange = false;
    Serial.println("動作相同 PASS");
  }
  Serial.print("thisAct=");
  Serial.print(thisAct);
  Serial.print(",preAct=");
  Serial.println(preAct);
  // if (thisAct == 0 && preAct == 0){ //0=fast
  //   BluetoothInRange = true;
  // }
  preAct = thisAct;
  // 深度睡眠7秒藍牙2秒   130 mWh
  // 深度睡眠5秒藍牙2秒   160 mWh
  // 深度睡眠5秒藍牙3秒   205 mWh
  // 深度睡眠4秒藍牙3秒   240 mWh
  // 深度睡眠3秒藍牙1.5秒 15次循環檢查一次wifi 160 mWh
  // 深度睡眠3秒藍牙2秒   15次循環檢查一次wifi  200mWh
  if (isDeepRest){
    Serial.println("進入深度睡眠模式(slow)...");
    esp_sleep_enable_timer_wakeup(10 * 1000000);  // x秒後喚醒
    esp_deep_sleep_start();  
  } else {
    Serial.println("進入深度睡眠模式(fast)...");
    esp_sleep_enable_timer_wakeup(1 * 1000000);  // x秒後喚醒
    esp_deep_sleep_start();  
  }
}

void loop() {
    // 空的主循環
}

bool handleScanResults(NimBLEScanResults& foundDevices) {
  int count = foundDevices.getCount();
  thisAct = 1; //1=slow
  BluetoothInRange = false;
  Serial.print("rssi=");
  for (int i = 0; i < count; i++) {
    NimBLEAdvertisedDevice device = foundDevices.getDevice(i);
    int rssi = device.getRSSI();  // 獲取 RSSI 值
    if (device.getAddress().toString() == std::string(targetMacAddress.c_str())) {
      Serial.println(rssi);
      mvRssi = rssi;
      BluetoothInRange = true;
      if (preAct == 2){//2=open
        thisAct = preAct; 
      }else{
        thisAct = 0;//0=fast
      }
      // Serial.print("blue sacn thisAct=");
      // Serial.println(thisAct);
      if (rssi > RSSI_THRESHOLD) {
        return true;  // 找到符合條件的設備
        pBLEScan->stop();  // 停止掃描
      }
    }
  }
  if (!BluetoothInRange){
    Serial.println("???");
  }
  
  return false;  // 沒有找到符合條件的設備
}

void connectToWiFi() {
  int iCount = 0;

  // 掃描 Wi-Fi 網路
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
    Serial.println("未找到目標 SSID，跳過本次 WiFi 動作...");
    return;
  }

  // 連接 Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    iCount++;
    if (iCount > 20) {
      Serial.println("停止連接");
      break;
    }
  }
  if (iCount <= 20) {
    Serial.println("WiFi 連接成功");
  } 
}

void triggerRelays() {
  Serial.println("引擎啟動");
  digitalWrite(POWER_PIN, HIGH);  // 打開3.3V電源輸出
  delay(1000);

  // 觸發繼電器 A 一秒
  digitalWrite(RELAY_PIN_A, LOW);
  delay(1000);
  digitalWrite(RELAY_PIN_A, HIGH);
  delay(500);

  // 觸發繼電器 B 4秒
  digitalWrite(RELAY_PIN_B, LOW);
  delay(4000);
  digitalWrite(RELAY_PIN_B, HIGH);

  send_line("BVB-7980 啟動中...");
  digitalWrite(POWER_PIN, LOW);  // 關閉3.3V電源輸出
  //delay(30000);
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
    Serial.println("HTTP 請求錯誤");
  }
  http.end();
}