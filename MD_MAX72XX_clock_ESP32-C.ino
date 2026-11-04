#include <WiFi.h>
#include <WiFiServer.h>
#include <HTTPClient.h>
#include <MD_MAX72xx.h>
#include <time.h>

#define DEBUG 1

#if DEBUG
#define PRINT(s, v) { Serial.print(F(s)); Serial.print(v); }
#define PRINTS(s)   { Serial.print(F(s)); }
#else
#define PRINT(s, v)
#define PRINTS(s)
#endif

// ==========================================
// 硬體與模組設定
// ==========================================
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4   // 4 塊 8x8 點陣，總顯示寬度 32 像素

// ESP32-C3 Super Mini 腳位定義
#define CLK_PIN   4  // SCK[cite: 2, 3]
#define DATA_PIN  6  // DIN (MOSI)[cite: 2, 3]
#define CS_PIN    7  // SS[cite: 2, 3]

MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// WiFi 預設連線帳密
char sta_ssid[32] = "SAMPO_STAFF";
char sta_password[64] = "066018688";

// AP 模式設定
const char* ap_ssid = "ESP32-C3-Setup";
const char* ap_password = "";

enum WiFiModeState { MODE_CONNECTING, MODE_STA, MODE_AP };
WiFiModeState currentMode = MODE_CONNECTING;

// NTP 設定 (台灣 GMT+8)
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;

// 亮度與速度控制
uint8_t dayBrightness = 7;
uint8_t nightBrightness = 0;
uint8_t currentAppliedBrightness = 255;

// 夜間時段設定 (小時 0~23)
int nightStartHour = 22;
int nightEndHour = 6;
bool isNightTime = false;

uint32_t driftSpeed = 1500;    // 飄移速度 (毫秒)

// WiFi Server
WiFiServer server(80);

// 狀態計時變數
const uint32_t STA_TIMEOUT = 10000;
uint32_t connectTimer = 0;
uint32_t lastDriftTime = 0;

// 門禁狀態監控相關
char doorStatusUrl[128] = "http://192.168.1.100:8080/status";
bool doorIsOpen = false;
uint32_t lastDoorCheckTime = 0;
const uint32_t DOOR_CHECK_INTERVAL = 60000; // 每一分鐘檢查一次

// 飄移相關座標變數 (限制在 32x8 範圍內)
int posX = 0;
int posY = 0;
int dirX = 1;
int dirY = 0; // 上下不晃出螢幕
int textWidth = 27; // HH:MM 寬度約 27 像素

// ==========================================
// 網頁 UI
// ==========================================
String getAPWebPage()
{
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
  "<title>ESP32 AP 模式設定</title>"
  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "<style>"
  "body{font-family:sans-serif;padding:20px;max-width:400px;margin:auto;background:#f4f4f4;}"
  ".card{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}"
  "input{width:100%;padding:10px;box-sizing:border-box;margin:8px 0;border:1px solid #ccc;border-radius:4px;}"
  "button{width:100%;padding:12px;background:#28a745;color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer;}"
  "</style></head><body><div class='card'>"
  "<h2>WiFi 設定 (AP 模式)</h2>"
  "<p style='color:red;'>連線預設 WiFi 失敗，請設定網路：</p>"
  "<form action='/connect' method='GET'>"
  "<label>SSID:</label><input type='text' name='ssid' required>"
  "<label>Password:</label><input type='password' name='pass'>"
  "<div style='margin:15px 0;'>"
  "<label>飄移速度: <span id='spv'>" + String(driftSpeed) + "</span> ms</label>"
  "<input type='range' name='spd' min='1000' max='5000' step='100' value='" + String(driftSpeed) + "' oninput='document.getElementById(\"spv\").innerText=this.value'>"
  "</div>"
  "<button type='submit'>儲存並連線</button>"
  "</form></div></body></html>";
  return html;
}

String getSTAWebPage()
{
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
  "<title>ESP32 控制台</title>"
  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "<style>"
  "body{font-family:sans-serif;padding:20px;max-width:450px;margin:auto;background:#f9f9f9;}"
  ".card{background:#fff;padding:20px;border-radius:10px;box-shadow:0 2px 6px rgba(0,0,0,0.1);margin-bottom:15px;}"
  "input[type=range]{width:100%;margin-top:8px;}"
  "input[type=text]{width:100%;padding:8px;box-sizing:border-box;margin-top:6px;border:1px solid #ccc;border-radius:4px;}"
  "input[type=number]{width:70px;padding:5px;border:1px solid #ccc;border-radius:4px;}"
  "button{width:100%;padding:10px;background:#007BFF;color:#fff;border:none;border-radius:5px;cursor:pointer;margin-top:10px;font-size:15px;}"
  ".row{display:flex;justify-content:space-between;align-items:center;font-weight:bold;margin-top:15px;}"
  ".badge{padding:3px 8px;border-radius:12px;font-size:12px;color:#fff;background:#6c757d;}"
  ".active-mode{background:#28a745;}"
  ".door-open{background:#dc3545;}"
  "</style>"
  "<script>"
  "function setVal(type, val){"
  "  var xhr = new XMLHttpRequest();"
  "  xhr.open('GET', '/set?' + type + '=' + val + '&nc=' + Math.random(), true);"
  "  xhr.send();"
  "}"
  "function saveNight(){"
  "  var s = document.getElementById('n_start').value;"
  "  var e = document.getElementById('n_end').value;"
  "  var xhr = new XMLHttpRequest();"
  "  xhr.open('GET', '/setNight?start=' + s + '&end=' + e + '&nc=' + Math.random(), true);"
  "  xhr.send();"
  "}"
  "function saveDoorUrl(){"
  "  var u = encodeURIComponent(document.getElementById('door_url').value);"
  "  var xhr = new XMLHttpRequest();"
  "  xhr.open('GET', '/setDoor?url=' + u + '&nc=' + Math.random(), true);"
  "  xhr.send();"
  "}"
  "</script></head><body>"
  "<div class='card'>"
  "<h2>點陣時鐘控制 (STA 模式)</h2>"
  "<p>已連線 IP: " + WiFi.localIP().toString() + " <span class='badge " + (isNightTime ? "active-mode" : "") + "'>" + (isNightTime ? "夜間模式" : "白天模式") + "</span></p>"
  
  "<div class='row'><span>白天亮度 (0-15):</span><span id='b_val'>" + String(dayBrightness) + "</span></div>"
  "<input type='range' min='0' max='15' value='" + String(dayBrightness) + "' onchange='document.getElementById(\"b_val\").innerText=this.value; setVal(\"DAY_BRI\", this.value)'>"
  
  "<div class='row'><span>飄移速度 (ms):</span><span id='s_val'>" + String(driftSpeed) + " ms</span></div>"
  "<input type='range' min='1000' max='5000' step='100' value='" + String(driftSpeed) + "' onchange='document.getElementById(\"s_val\").innerText=this.value+\" ms\"; setVal(\"SPD\", this.value)'>"
  "</div>"

  "<div class='card'>"
  "<h3>🚪 門禁狀態監控網址</h3>"
  "<p>目前門狀態: <span class='badge " + (doorIsOpen ? "door-open" : "active-mode") + "'>" + (doorIsOpen ? "門開 (OPEN)" : "門關 (CLOSED)") + "</span></p>"
  "<label>狀態查詢網址 (每分鐘輪詢一次):</label>"
  "<input type='text' id='door_url' value='" + String(doorStatusUrl) + "'>"
  "<button onclick='saveDoorUrl()'>儲存網址並立即查詢</button>"
  "</div>"

  "<div class='card'>"
  "<h3>🌙 夜間時段設定</h3>"
  "<div class='row'><span>開始時間 (時):</span><input type='number' id='n_start' min='0' max='23' value='" + String(nightStartHour) + "'> 時</div>"
  "<div class='row'><span>結束時間 (時):</span><input type='number' id='n_end' min='0' max='23' value='" + String(nightEndHour) + "'> 時</div>"
  "<div class='row'><span>夜間亮度 (0-15):</span><span id='nb_val'>" + String(nightBrightness) + "</span></div>"
  "<input type='range' min='0' max='15' value='" + String(nightBrightness) + "' onchange='document.getElementById(\"nb_val\").innerText=this.value; setVal(\"NIGHT_BRI\", this.value)'>"
  "<button onclick='saveNight()'>更新夜間時段</button>"
  "</div>"
  "</body></html>";
  return html;
}

// ==========================================
// 點陣圖形字型繪製 (緊湊字距 + 邊緣警示)
// ==========================================
int drawCompactChar(char c, int offsetX, int offsetY)
{
  uint8_t charBuf[8];
  uint8_t charWidth = mx.getChar(c, sizeof(charBuf), charBuf);
  
  // 找出該字元實際使用的寬度（去除字型緩衝區右側全空白的直列）
  int actualWidth = charWidth;
  while (actualWidth > 1 && charBuf[actualWidth - 1] == 0) {
    actualWidth--;
  }
  
  for (int col = 0; col < actualWidth; col++)
  {
    int screenCol = offsetX + col;
    if (screenCol >= 0 && screenCol < 32)
    {
      uint8_t colPattern = charBuf[col];
      for (int row = 0; row < 8; row++)
      {
        int screenRow = offsetY + row;
        if (screenRow >= 0 && screenRow < 8)
        {
          if ((colPattern >> (7 - row)) & 0x01)
          {
            mx.setPoint(screenRow, screenCol, true);
          }
        }
      }
    }
  }
  return actualWidth;
}

void renderTimeString(const char* str, int ox, int oy)
{
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  mx.clear();
  
  // 1. 繪製 HH:MM 時間字串 (字元間隔縮短為 1 像素)
  int currX = ox;
  while (*str)
  {
    int w = drawCompactChar(*str, currX, oy);
    currX += w + 1; // 間距縮小為 1 像素
    str++;
  }
  textWidth = currX - ox;

  // 2. 當門開啟 (door == "open") 時，最左 (第0欄) 與最右 (第31欄) 一整行全亮
  if (doorIsOpen)
  {
    for (int r = 0; r < 8; r++)
    {
      mx.setPoint(r, 0, true);   // 最左行全亮
      mx.setPoint(r, 31, true);  // 最右行全亮
    }
  }
  
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

// 依據時間自動切換白天/夜間亮度
void updateBrightnessMode(int currentHour)
{
  bool night;
  if (nightStartHour > nightEndHour)
  {
    night = (currentHour >= nightStartHour || currentHour < nightEndHour);
  }
  else
  {
    night = (currentHour >= nightStartHour && currentHour < nightEndHour);
  }
  isNightTime = night;

  uint8_t targetBri = isNightTime ? nightBrightness : dayBrightness;
  if (currentAppliedBrightness != targetBri)
  {
    currentAppliedBrightness = targetBri;
    mx.control(MD_MAX72XX::INTENSITY, currentAppliedBrightness);
  }
}

// 飄移更新邏輯 (限制在 32 像素可見範圍內)
void updateDriftDisplay()
{
  if (millis() - lastDriftTime < driftSpeed) return;
  lastDriftTime = millis();

  char timeBuf[16];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50))
  {
    updateBrightnessMode(timeinfo.tm_hour);
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo); // 格式改為 hh:mm
  }
  else
  {
    if (currentMode == MODE_AP)
    {
      strcpy(timeBuf, "AP");
    }
    else
    {
      strcpy(timeBuf, "--:--");
    }
  }

  // 若門開啟，最左與最右邊緣各佔 1 欄，飄移有效寬度縮為 1~30
  int minLimitX = doorIsOpen ? 1 : 0;
  int maxLimitX = (doorIsOpen ? 31 : 32) - textWidth;

  if (maxLimitX < minLimitX) maxLimitX = minLimitX;

  // 限制左右飄移不跑出可視區
  posX += dirX;
  if (posX >= maxLimitX) { posX = maxLimitX; dirX = -1; }
  if (posX <= minLimitX) { posX = minLimitX; dirX = 1; }

  posY = 0; // 高度固定填滿 8 點不上下移出畫面

  renderTimeString(timeBuf, posX, posY);
}

// ==========================================
// 門禁 API 輪詢處理 (每分鐘一次)
// ==========================================
void checkDoorStatus()
{
  if (currentMode != MODE_STA || strlen(doorStatusUrl) < 7) return;

  if (millis() - lastDoorCheckTime >= DOOR_CHECK_INTERVAL || lastDoorCheckTime == 0)
  {
    lastDoorCheckTime = millis();
    PRINTS("\n[HTTP] 正在檢查門禁狀態: ");
    PRINTS(doorStatusUrl);

    HTTPClient http;
    http.begin(doorStatusUrl);
    http.setTimeout(3000); // 3 秒超時
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
      String payload = http.getString();
      PRINT("\n[HTTP] 回傳內容: ", payload);

      // 解析回傳 JSON 是否包含 open
      if (payload.indexOf("\"door\"") >= 0 && payload.indexOf("\"open\"") >= 0)
      {
        doorIsOpen = true;
        PRINTS("\n[Door] 門狀態: OPEN (警告燈已開啟)");
      }
      else
      {
        doorIsOpen = false;
        PRINTS("\n[Door] 門狀態: CLOSED");
      }
    }
    else
    {
      PRINT("\n[HTTP] 請求失敗，錯誤碼: ", httpCode);
    }
    http.end();
  }
}

// ==========================================
// 網路與請求處理
// ==========================================
void startAPMode()
{
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  currentMode = MODE_AP;
  mx.control(MD_MAX72XX::INTENSITY, dayBrightness);
  PRINTS("\n[WiFi] 連線失敗，啟動 AP 模式！");
}

void checkWiFi()
{
  if (currentMode == MODE_CONNECTING)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      currentMode = MODE_STA;
      PRINTS("\n[WiFi] STA 連線成功！");
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      lastDoorCheckTime = 0; // 連線成功後立刻發起一次查詢
    }
    else if (millis() - connectTimer >= STA_TIMEOUT)
    {
      startAPMode();
    }
  }
}

void handleClientRequests()
{
  WiFiClient client = server.accept();
  if (!client) return;

  uint32_t startWait = millis();
  while (!client.available() && millis() - startWait < 500) delay(1);

  if (client.available())
  {
    String req = client.readStringUntil('\r');
    client.readStringUntil('\n');

    // 1. AP 模式連線設定
    if (currentMode == MODE_AP && req.indexOf("GET /connect") >= 0)
    {
      int sIndex = req.indexOf("ssid=");
      int pIndex = req.indexOf("&pass=");
      int spIndex = req.indexOf("&spd=");

      if (sIndex > 0 && pIndex > 0)
      {
        String new_ssid = req.substring(sIndex + 5, pIndex);
        String new_pass = (spIndex > 0) ? req.substring(pIndex + 6, spIndex) : req.substring(pIndex + 6, req.indexOf(" ", pIndex));
        
        if (spIndex > 0) {
          int sEnd = req.indexOf(" ", spIndex);
          driftSpeed = req.substring(spIndex + 5, sEnd).toInt();
        }

        new_ssid.replace('+', ' ');
        new_pass.replace('+', ' ');
        new_ssid.toCharArray(sta_ssid, sizeof(sta_ssid));
        new_pass.toCharArray(sta_password, sizeof(sta_password));

        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n設定已儲存，正在重新連線...");
        delay(100);
        client.stop();

        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(sta_ssid, sta_password);
        connectTimer = millis();
        currentMode = MODE_CONNECTING;
        return;
      }
    }
    // 2. STA 模式即時調整滑桿
    else if (req.indexOf("GET /set?") >= 0)
    {
      int dbIdx = req.indexOf("DAY_BRI=");
      if (dbIdx >= 0) {
        dayBrightness = req.substring(dbIdx + 8, req.indexOf("&", dbIdx)).toInt();
        currentAppliedBrightness = 255;
      }
      int nbIdx = req.indexOf("NIGHT_BRI=");
      if (nbIdx >= 0) {
        nightBrightness = req.substring(nbIdx + 10, req.indexOf("&", nbIdx)).toInt();
        currentAppliedBrightness = 255;
      }
      int sIdx = req.indexOf("SPD=");
      if (sIdx >= 0) {
        driftSpeed = req.substring(sIdx + 4, req.indexOf("&", sIdx)).toInt();
      }
      client.print("HTTP/1.1 200 OK\r\n\r\nOK");
      client.stop();
      return;
    }
    // 3. STA 模式夜間時段表單更新
    else if (req.indexOf("GET /setNight?") >= 0)
    {
      int sIdx = req.indexOf("start=");
      int eIdx = req.indexOf("&end=");
      if (sIdx >= 0 && eIdx >= 0) {
        nightStartHour = req.substring(sIdx + 6, eIdx).toInt();
        nightEndHour = req.substring(eIdx + 5, req.indexOf("&", eIdx + 5)).toInt();
        currentAppliedBrightness = 255;
      }
      client.print("HTTP/1.1 200 OK\r\n\r\nOK");
      client.stop();
      return;
    }
    // 4. STA 模式設定門禁網址
    else if (req.indexOf("GET /setDoor?url=") >= 0)
    {
      int uIdx = req.indexOf("url=");
      int endIdx = req.indexOf("&", uIdx);
      if (endIdx < 0) endIdx = req.indexOf(" ", uIdx);
      String uStr = req.substring(uIdx + 4, endIdx);
      
      // 簡易解碼 URL
      uStr.replace("%3A", ":");
      uStr.replace("%2F", "/");
      uStr.toCharArray(doorStatusUrl, sizeof(doorStatusUrl));
      
      lastDoorCheckTime = 0; // 儲存後立即強制觸發一次輪詢
      client.print("HTTP/1.1 200 OK\r\n\r\nOK");
      client.stop();
      return;
    }

    // 回應主要網頁
    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n");
    if (currentMode == MODE_AP) {
      client.print(getAPWebPage());
    } else {
      client.print(getSTAWebPage());
    }
  }
  delay(1);
  client.stop();
}

void setup(void)
{
  Serial.begin(115200);
  delay(500);
  PRINTS("\n=== ESP32-C3 LED 矩陣時鐘啟動 ===");

  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, dayBrightness);
  mx.clear();

  WiFi.mode(WIFI_STA);
  WiFi.begin(sta_ssid, sta_password);
  connectTimer = millis();
  currentMode = MODE_CONNECTING;

  server.begin();
}

void loop(void)
{
  checkWiFi();
  handleClientRequests();
  checkDoorStatus();
  updateDriftDisplay();
}