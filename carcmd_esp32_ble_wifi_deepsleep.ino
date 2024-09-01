#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>
#include "esp32-hal-cpu.h"

#define RELAY_PIN_A  5  // GPIO 5 锁门
#define RELAY_PIN_B  4  // GPIO 4 发车
#define RELAY_PIN_C  15 // GPIO 15 开门
#define POWER_PIN 25    // GPIO 25
#define RSSI_THRESHOLD -90  // RSSI 阈值
#define RSSI_SECOND_THRESHOLD -110  // 第二层RSSI阈值
#define TIMEOUT 3000  // 超时时间3秒（3000毫秒）

String targetMacAddress = "20:22:05:26:00:8d";

const char* ssid = "opposky";
const char* password = "0988085240";
const char* url = "http://www.inskychen.com/carcmd/checkcarboot.php";

NimBLEScan* pBLEScan;
unsigned long lastDetectedTime = 0;  // 上次检测到目标设备的时间戳
bool deviceDetected = false;  // 记录设备是否被检测到
bool bluetoothDeviceDetected = false;  // 标志位，记录是否检测到蓝牙设备
int mvRssi;
bool BluetoothInRange = false;
int thisAct = 0;  // 本次act

RTC_DATA_ATTR bool lastBluetoothDetected = false;  // 使用RTC内存保存上次蓝牙检测状态
RTC_DATA_ATTR int sleepCounter = 0;  // 使用RTC内存保存深度睡眠次数
//RTC_DATA_ATTR int lastRssi = -999;  // 使用RTC内存最後一次訊號值
RTC_DATA_ATTR int preAct = 1;  // 使用RTC内存上次act 0:fast 1:slow 2:open 3:lock


void setup() {
    Serial.begin(115200);
    setCpuFrequencyMhz(80);  // 将核心速度设为80 MHz
    
    thisAct = preAct;
    // Serial.print("blue sacn 0 preAct=");
    // Serial.println(preAct);
    
    NimBLEDevice::init("");  // 使用NimBLE库初始化
    NimBLEDevice::setPower(ESP_PWR_LVL_N2);  // 设置较低的功率级别

    pBLEScan = NimBLEDevice::getScan();  // 创建NimBLE扫描对象
    pBLEScan->setActiveScan(false);  // 设置为被动扫描模式
    pBLEScan->setInterval(100);  // 设置扫描间隔
    pBLEScan->setWindow(99);  // 设置扫描窗口，应该小于或等于间隔值

    // 设置为输出模式，初始状态为低电平
    pinMode(RELAY_PIN_C, OUTPUT);
    pinMode(RELAY_PIN_A, OUTPUT);
    pinMode(RELAY_PIN_B, OUTPUT);
    digitalWrite(RELAY_PIN_C, HIGH);
    digitalWrite(RELAY_PIN_A, HIGH);
    digitalWrite(RELAY_PIN_B, HIGH);

    pinMode(POWER_PIN, OUTPUT);
    digitalWrite(POWER_PIN, LOW);  // 初始状态下关闭电源输出

    // 检查深度睡眠恢复的次数
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        sleepCounter++;
        Serial.println(sleepCounter);
    } else {
        sleepCounter = 0;  // 如果是第一次启动或非计时器唤醒，则重置计数器
    }

    // 先扫描蓝牙设备
    
    mvRssi = -999;
    NimBLEScanResults foundDevices = pBLEScan->start(2, false);  // 开始扫描
    bool found = handleScanResults(foundDevices);
    pBLEScan->clearResults();  // 清除扫描结果

    if (found) {
        bluetoothDeviceDetected = true;  // 检测到蓝牙设备，设置标志位
        lastDetectedTime = millis();  // 更新最后检测到目标设备的时间戳
        if (!deviceDetected && !lastBluetoothDetected) {
            digitalWrite(POWER_PIN, HIGH);
            delay(1000);
            Serial.println("开门1(蓝牙信标出现）");
            digitalWrite(RELAY_PIN_C, LOW);
            delay(1000);
            digitalWrite(RELAY_PIN_C, HIGH);
            digitalWrite(POWER_PIN, LOW);
            deviceDetected = true;
            BluetoothInRange = false;
            thisAct = 2;
        }
        lastBluetoothDetected = true;  // 更新RTC内存中的检测状态
    } else {
        if (lastBluetoothDetected) {
            if (mvRssi <= RSSI_SECOND_THRESHOLD) {
                digitalWrite(POWER_PIN, HIGH);
                delay(1000);
                Serial.println("锁门1（蓝牙信标消失）");
                digitalWrite(RELAY_PIN_A, LOW);
                delay(1000);
                digitalWrite(RELAY_PIN_A, HIGH);
                digitalWrite(POWER_PIN, LOW);
                deviceDetected = false;
                bluetoothDeviceDetected = false;  // 设备消失，清除蓝牙设备标志位
                lastBluetoothDetected = false;  // 更新RTC内存中的检测状态
                BluetoothInRange = false;
                thisAct = 3;
            }
        }
    }

    // 判断是否应该进行 Wi-Fi 操作
    if (sleepCounter >= 8) {
        sleepCounter = 0;  // 重置计数器
        if (!lastBluetoothDetected) {
            connectToWiFi();

            if (WiFi.status() == WL_CONNECTED) {
                HTTPClient http;
                WiFiClient client;

                // 使用 WiFiClient 和 URL 初始化 HTTPClient
                http.begin(client, url);

                int httpCode = http.GET();  // 发送请求并获取响应代码

                if (httpCode > 0) {
                    String payload = http.getString();  // 获取网页内容

                    // 检查 "boot" 标签是否为 "enable"
                    if (payload.indexOf("<boot>boot</boot>") != -1) {
                        triggerRelays();
                    } else {
                        Serial.println("Boot is not enabled.");
                    }
                } else {
                    Serial.printf("GET failed, error: %s\n", http.errorToString(httpCode).c_str());
                }
                http.end();  // 结束 HTTP 请求
            }

            WiFi.disconnect();  // 断开 Wi-Fi 连接
            Serial.println("断开WiFi, 进入深度睡眠模式...");
            thisAct = 1; 
        } else {
            Serial.println("检测到蓝牙信标，跳过本次WIFI动作");
        }
    }

    // 进入深度睡眠模式
    if (thisAct == preAct && thisAct != 0 && preAct != 0){ //0=fast
      BluetoothInRange = false;
      Serial.println("動作相同PASS");
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
      Serial.println("进入深度睡眠模式(fast)...");
      esp_sleep_enable_timer_wakeup(0.1 * 1000000);  // 0.1秒后唤醒
      esp_deep_sleep_start();
    }else{
      Serial.println("进入深度睡眠模式(slow)...");
      esp_sleep_enable_timer_wakeup(10 * 1000000);  // 3秒后唤醒
      esp_deep_sleep_start();
    }
}

void loop() {
    // 空的主循环
}

bool handleScanResults(NimBLEScanResults& foundDevices) {
    int count = foundDevices.getCount();
    thisAct = 1; //1=slow
    BluetoothInRange = false;
    for (int i = 0; i < count; i++) {
        NimBLEAdvertisedDevice device = foundDevices.getDevice(i);
        int rssi = device.getRSSI();  // 获取 RSSI 值
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
            //pBLEScan->stop();  // 停止扫描
            
        }
        if (device.getAddress().toString() == std::string(targetMacAddress.c_str()) && rssi > RSSI_THRESHOLD) {
            return true;  // 找到符合条件的设备
        }
    }
    return false;  // 没有找到符合条件的设备
}

void connectToWiFi() {
    int iCount = 0;

    // 扫描 Wi-Fi 网络
    int n = WiFi.scanNetworks();
    bool ssidFound = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == ssid) {
            ssidFound = true;
            break;
        }
    }

    // 如果未找到目标 SSID，等待一段时间然后再重试
    if (!ssidFound) {
        Serial.println("未找到目标 SSID，跳过本次WIFI动作...");
        return;
    }

    // 连接 Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1500);
        Serial.print(".");
        iCount++;
        if (iCount > 20) {
            Serial.println("停止连接");
            break;
        }
    }
    if (iCount <= 20) {
        Serial.println("WiFi connected");
    } 
}

void triggerRelays() {
    Serial.println("Engine start");
    digitalWrite(POWER_PIN, HIGH);  // 打开3.3V电源输出
    delay(1000);

    // 触发继电器 A 一秒
    digitalWrite(RELAY_PIN_A, LOW);
    delay(1500);
    digitalWrite(RELAY_PIN_A, HIGH);
    delay(500);

    // 触发继电器 B 4秒
    digitalWrite(RELAY_PIN_B, LOW);
    delay(4000);
    digitalWrite(RELAY_PIN_B, HIGH);

    send_line("BVB-7980 启动中...");
    digitalWrite(POWER_PIN, LOW);  // 关闭3.3V电源输出
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