#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>
#include "esp32-hal-cpu.h"

#define RELAY_PIN_A  5  // GPIO 5 鎖門
#define RELAY_PIN_B  4  // GPIO 4 發車
#define RELAY_PIN_C  15 // GPIO 15 開門
#define POWER_PIN 25    // GPIO 25
#define RSSI_THRESHOLD -90  // RSSI 閾值
#define RSSI_SECOND_THRESHOLD -110  // 第二層 RSSI 閾值
#define TIMEOUT 3000  // 超時時間3秒（3000毫秒）

String targetMacAddress = "20:22:05:26:00:8d";

const char* ssid = "opposky";
const char* password = "0988085240";
const char* url = "http://www.inskychen.com/carcmd/checkcarboot.php";

NimBLEScan* pBLEScan;
// unsigned long lastDetectedTime = 0;  // 上次檢測到目標設備的時間戳
bool deviceDetected = false;  // 記錄設備是否被檢測到
bool bluetoothDeviceDetected = false;  // 標誌位，記錄是否檢測到藍牙設備
int mvRssi;
bool BluetoothInRange = false;
int thisAct = 0;  // 本次 act

RTC_DATA_ATTR bool lastBluetoothDetected = false;  // 使用 RTC 記憶體保存上次藍牙檢測狀態
RTC_DATA_ATTR int sleepCounter = 0;  // 使用 RTC 記憶體保存深度睡眠次數
//RTC_DATA_ATTR int lastRssi = -999;  // 使用 RTC 記憶體最後一次信號值
RTC_DATA_ATTR int preAct = 1;  // 使用 RTC 記憶體上次 act 0:fast 1:slow 2:open 3:lock

void setup() {
    Serial.begin(115200);
    setCpuFrequencyMhz(80);  // 將核心速度設為80 MHz
    
    thisAct = preAct;
    // Serial.print("blue sacn 0 preAct=");
    // Serial.println(preAct);
    
    NimBLEDevice::init("");  // 使用 NimBLE 庫初始化
    NimBLEDevice::setPower(ESP_PWR_LVL_N0);  // 設置較低的功率級別

    pBLEScan = NimBLEDevice::getScan();  // 創建 NimBLE 掃描對象
    pBLEScan->setActiveScan(false);  // 設置為被動掃描模式
    pBLEScan->setInterval(100);  // 設置掃描間隔
    pBLEScan->setWindow(99);  // 設置掃描窗口，應該小於或等於間隔值

    // 設置為輸出模式，初始狀態為低電平
    pinMode(RELAY_PIN_C, OUTPUT);
    pinMode(RELAY_PIN_A, OUTPUT);
    pinMode(RELAY_PIN_B, OUTPUT);
    digitalWrite(RELAY_PIN_C, HIGH);
    digitalWrite(RELAY_PIN_A, HIGH);
    digitalWrite(RELAY_PIN_B, HIGH);

    pinMode(POWER_PIN, OUTPUT);
    digitalWrite(POWER_PIN, LOW);  // 初始狀態下關閉電源輸出

    // 檢查深度睡眠恢復的次數
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        sleepCounter++;
        Serial.println(sleepCounter);
    } else {
        sleepCounter = 0;  // 如果是第一次啟動或非計時器喚醒，則重置計數器
    }

    // 先掃描藍牙設備
    
    mvRssi = -999;
    NimBLEScanResults foundDevices = pBLEScan->start(2, false);  // 開始掃描
    bool found = handleScanResults(foundDevices);
    pBLEScan->clearResults();  // 清除掃描結果

    if (found) {
        bluetoothDeviceDetected = true;  // 檢測到藍牙設備，設置標誌位
        // lastDetectedTime = millis();  // 更新最後檢測到目標設備的時間戳
        if (!deviceDetected && !lastBluetoothDetected) {
            digitalWrite(POWER_PIN, HIGH);
            delay(100);
            Serial.println("開門1(藍牙信標出現）");
            digitalWrite(RELAY_PIN_C, LOW);
            delay(1000);
            digitalWrite(RELAY_PIN_C, HIGH);
            digitalWrite(POWER_PIN, LOW);
            deviceDetected = true;
            BluetoothInRange = false;
            thisAct = 2;
        }
        lastBluetoothDetected = true;  // 更新 RTC 記憶體中的檢測狀態
    } else {
        if (lastBluetoothDetected) {
            if (mvRssi <= RSSI_SECOND_THRESHOLD) {
                digitalWrite(POWER_PIN, HIGH);
                delay(100);
                Serial.println("鎖門1（藍牙信標消失）");
                digitalWrite(RELAY_PIN_A, LOW);
                delay(1000);
                digitalWrite(RELAY_PIN_A, HIGH);
                digitalWrite(POWER_PIN, LOW);
                deviceDetected = false;
                bluetoothDeviceDetected = false;  // 設備消失，清除藍牙設備標誌位
                lastBluetoothDetected = false;  // 更新 RTC 記憶體中的檢測狀態
                BluetoothInRange = false;
                thisAct = 3;
            }
        }
    }

    // 判斷是否應該進行 Wi-Fi 操作
    if (sleepCounter >= 8) {
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
            Serial.println("斷開 WiFi, 進入深度睡眠模式...");
            thisAct = 1; 
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
    if (thisAct == 0 && preAct == 0){ //0=fast
      BluetoothInRange = true;
    }
    preAct = thisAct;
    if (BluetoothInRange){
      Serial.println("進入深度睡眠模式(fast)...");
      esp_sleep_enable_timer_wakeup(0.1 * 1000000);  // 0.1秒後喚醒
      esp_deep_sleep_start();
    }else{
      Serial.println("進入深度睡眠模式(slow)...");
      esp_sleep_enable_timer_wakeup(7 * 1000000);  // 8秒後喚醒
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
    for (int i = 0; i < count; i++) {
        NimBLEAdvertisedDevice device = foundDevices.getDevice(i);
        int rssi = device.getRSSI();  // 獲取 RSSI 值
        if (device.getAddress().toString() == std::string(targetMacAddress.c_str())) {
            Serial.print("rssi=");
            Serial.println(rssi);
            mvRssi = rssi;
            //lastRssi = rssi;
            BluetoothInRange = true;
            if (preAct == 2){//2=open
              thisAct = preAct; 
            }else{
              thisAct = 0;//0=fast
            }
            
            // Serial.print("blue sacn thisAct=");
            // Serial.println(thisAct);
            //pBLEScan->stop();  // 停止掃描
            
        }
        if (device.getAddress().toString() == std::string(targetMacAddress.c_str()) && rssi > RSSI_THRESHOLD) {
            return true;  // 找到符合條件的設備
        }
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
        delay(1500);
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
    delay(1500);
    digitalWrite(RELAY_PIN_A, HIGH);
    delay(500);

    // 觸發繼電器 B 4秒
    digitalWrite(RELAY_PIN_B, LOW);
    delay(4000);
    digitalWrite(RELAY_PIN_B, HIGH);

    send_line("BVB-7980 啟動中...");
    digitalWrite(POWER_PIN, LOW);  // 關閉3.3V電源輸出
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
        Serial.println("HTTP 請求錯誤");
    }

    http.end();
}