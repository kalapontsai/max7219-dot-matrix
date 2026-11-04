# MAX7219 四位 8x8 點陣走馬燈

ESP32-C3 + MAX7219 四位 8x8 共陽點陣模組，跑馬燈顯示數字 **0 ~ 9**（共 10 個），**由右至左**捲動。

## 檔案

| 檔案 | 說明 |
|---|---|
| `max7219-four-digit-marquee.ino` | 主程式（Arduino sketch） |
| `README.md` | 本文件 |

## 硬體需求

- ESP32-C3 開發板
- MAX7219 四位 8x8 點陣模組（4 片 8x8 面板 daisy-chain 串接，單色）
- 杜邦線

## 硬體接線

| MAX7219 模組 | ESP32-C3 | 備註 |
|---|---|---|
| VCC | 5V | 請確認模組規格是否可吃 3V3 |
| GND | GND | |
| DIN (MOSI) | **GPIO 6** | VSPI MOSI |
| CLK (SCLK) | **GPIO 5** | VSPI SCK |
| CS (LOAD) | **GPIO 7** | VSPI SS |
| DOUT | 下一片模組的 DIN | daisy-chain 串接 |

### 腳位選擇說明

ESP32-C3 的 strapping pin 為 **GPIO 2 / GPIO 8 / GPIO 9**，GPIO 0 為 download mode pin。
這四個 pin 都不能拿來接 CS / CLK。本程式採用 VSPI 預設腳位（5/6/7），全部避開 strapping pin。

若你的開發板把 SPI 腳位重新映射了，改 sketch 開頭的這三行：

```cpp
static const uint8_t PIN_CLK = 5;   // VSPI SCLK
static const uint8_t PIN_DIN = 6;   // VSPI MOSI
static const uint8_t PIN_CS  = 7;   // VSPI SS / LOAD
```

## 串接順序（重要，會影響捲動方向）

MAX7219 是 daisy-chain 架構，**位址 0 = 離 Arduino 最遠的那一片**，位址 3 = 最近 Arduino。

因此「畫面由左至右」的實體面板順序是：

```
畫面左                      畫面右
[面板3][面板2][面板1][面板0]
```

程式已把這層對應處理好，所以你只要把模組的 DOUT 接到下一片的 DIN，
**方向由左往右接**即可。如果你的排線是反方向接（DOUT 接到最左側），
捲動方向會相反，把 sketch 裡的這行改成 `true`：

```cpp
static const bool REVERSE_PANEL_ORDER = false;   // 改成 true
```

## 極性說明（8x8 共陽點陣）

MAX7219 是 **current-sink** 驅動晶片：LED 陽極接 VCC、陰極接到 MAX7219 的 segment 腳，
所以 **送 1 = 亮、0 = 熄**。市售「共陽點陣」模組的接法相同，本程式採 1 = 亮。

若你的面板亮暗相反，把 sketch 這兩個常數對調：

```cpp
static const uint8_t LED_ON  = 0x01;   // 改成 0x00
static const uint8_t LED_OFF = 0x00;   // 改成 0x01
```

## 程式原理

採用「離屏畫布 + 視窗 blit」：

| 項目 | 數值 | 說明 |
|---|---|---|
| canvas 寬度 | 90 欄 | 完整存放數字帶 0~9 |
| view 視窗寬度 | 32 欄 | 4 片面板 = 4 × 8 |
| 每個數字 | 8 欄字型 | 中間夾 1 欄空白，共 9 欄/數字 |
| 總影格數 | 91 | windowX 從 0 遞增到 90 |

視窗右移 → 內容在螢幕上左移 → 數字由右至左捲動，最後整條帶從左側滑出畫面，
下一輪重新從右側進入，形成循環。

### 可調參數

```cpp
static const uint8_t BRIGHTNESS = 8;        // 亮度 0~15
static const uint8_t SCROLL_STEP = 1;       // 每幀移動幾欄（越大越快）
static const uint16_t FRAME_DELAY_MS = 60;  // 每幀停留時間（越小越快）
```

## 函式庫

使用 `MAX7219` 函式庫（arduino-libraries），安裝指令：

```bash
arduino-cli lib install "MAX7219"
```

## 編譯 / 燒錄

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3 --warnings all \
  --output-dir /tmp/espout /tmp/espbuild/max7219-four-digit-marquee

arduino-cli upload --fqbn esp32:esp32:esp32c3 \
  -p /dev/ttyACM0 /tmp/espbuild/max7219-four-digit-marquee
```

注意：`arduino-cli` 要求 sketch 主檔名與資料夾同名，所以要先把 .ino 複製到
一個與檔名同名的資料夾（Arduino IDE 會自動處理這個限制）。

## 實機待驗項目

編譯成功只代表能對到函式庫標頭建置，**不代表面板一定會亮**。以下需要在實機上確認：

1. **四片面板是否都被點亮** — 串接順序錯誤或某片 CS 沒接好，會有面板全暗
2. **捲動方向是否為右至左** — 若是反的，設 `REVERSE_PANEL_ORDER = true`
3. **數字是否直立、有無左右鏡射** — 取決於模組安裝方向；鏡射可在 `pushCanvasToPanels()` 內把欄位順序反轉
4. **亮度是否適中** — 預設 8/15，若有 bloom 現象請調低 `BRIGHTNESS`
