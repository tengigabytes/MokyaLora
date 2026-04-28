# 10 — G-1 Status Bar

## 角色定位

Status Bar 是**全螢幕第一個被掃視到的元素**。其職責是在不打斷任務的前提下回答幾個常駐疑問：

1. 現在幾點？
2. 我的無線電還活著嗎？
3. 我能定位嗎？
4. 我電還夠嗎？
5. 系統現在處於什麼狀態？
6. 有沒有未讀訊息？
7. 有沒有警告？

每個問題都「掃一眼就要知道」，因此 **Status Bar 圖示優先、文字次之**——眼睛辨色比辨字快。

## 設計約束

- **高度**：16px（一行 Unifont 西文）
- **寬度**：320px = 40 半形字位 = 20 全形字位
- **字型**：Unifont 16×16 / 8×16
- **永久顯示**，無法被 App 覆蓋（除少數全屏狀態如 SOS 啟動）

## 元素清單（9 項）

```
09:41 ▲▼ ⚠ ●Mesh:7 ●GPS ✉3 ▣87% Op
```

| # | 元素 | 字位 | 顯示 | 說明 |
|---|---|---|---|---|
| 1 | 時間 | 5 | 永久 | `HH:MM`，12/24h 制依設定 |
| 2 | TX/RX 動作燈 | 2 | 永久 | `▲`=TX 橙 / `▼`=RX 綠 |
| 3 | 警告燈 | 1 | 條件 | `⚠` 黃，無警告時不繪製 |
| 4 | 鄰居節點 | 7 | 永久 | `●Mesh:7` |
| 5 | GPS 狀態 | 4 | 永久 | `●GPS` |
| 6 | 未讀訊息 | 3 | 條件 | `✉N`，N=0 不繪製 |
| 7 | 電量 | 5 | 永久 | `▣87%` |
| 8 | 模式 | 2 | 永久 | `Op` / `注` / `EN` / `Ab`（view 層 `ABC→Ab` mapping） |

字位驗算（最大狀態）：5+1+2+1+1+1+7+1+4+1+3+1+5+1+2 = 36 字位 ✓ 剩 4 字位空白用作元素間 padding

## 元素規格細節

### 1. 時間（5 字位）

```
09:41
```

- 12h 制：`9:41a` / `1:30p`（仍 5 字位）
- 24h 制：`09:41`
- 主色 `#E6EDF3`
- 整點短閃 1 次（可選，預設關）
- RTC 漂移 >5 分鐘且無 GPS 同步：顯示 `09:41?`，主色 + 紅色 `?`

### 2. TX/RX 動作燈（2 字位）

```
▲▼
```

- `▲` TX：橙 `#FFA657` 顯示 100ms，再淡出至灰 `#30363D`
- `▼` RX：綠 `#39D353` 顯示 100ms，再淡出至灰
- 不動作時兩個都顯示為灰色（保留位置不跳動）
- 動畫：硬切「亮 → 暗」，無漸變，省 GPU

兩個獨立符號的設計：使用者要能區分「我發出去了」與「對方在說話」。

> **實作依賴**：⚠️ 目前 IPC 無 `IPC_MSG_TX_DONE / IPC_MSG_RX_DONE` 通道，Core 1 無法即時得知 LoRa TX/RX 邊緣事件。
> 候選實作路徑：(a) 新增 `IPC_MSG_RADIO_EVENT` 由 Core 0 LoRa callback 觸發推送；(b) 退而求其次以 cascade FromRadio 流量間接偵測 RX（無法區分 ACK/payload）；(c) TX 燈改由「`IPC_CMD_SEND_TEXT` 送出 → 收 `Routing(ACK)` 或 `QueueStatus`」之間亮起作為 proxy。**規劃決定前，TX/RX 動作燈不可實裝**。

### 3. 警告燈（1 字位，條件顯示）

```
⚠
```

- 無警告時不繪製（位置留空）
- 有警告時顯示 `⚠` 黃色 `#F1E05A`
- 1 Hz 慢閃（不像 SOS 那麼急）
- 多警告時固定顯示 `⚠`，按 FUNC 進詳情面板看完整清單

警告類型：
- GPS 失效 >5 分鐘
- LoRa TX 異常（連續 N 次發送失敗）
- 感測器讀值異常
- PSRAM/Flash 容量警告
- Mesh 完全孤立
- 韌體版本與網路不相容
- 設定衝突

### 4. 鄰居節點（7 字位）

```
●Mesh:7
```

- `●` 圖示色：
  - 綠 = 鄰居 ≥1 且 LastHeard <5m
  - 黃 = 鄰居 ≥1 且 LastHeard 5-15m
  - 紅 = 鄰居 ≥1 且 LastHeard >15m
  - 灰 = 鄰居 = 0（孤立節點）
- N >99 顯示 `99+`
- 鄰居數定義：過去 1 小時內聽到任何封包的節點數（與 Meshtastic NodeDB 一致）

### 5. GPS 狀態（4 字位）

```
●GPS
```

| 狀態 | 顯示 | 顏色 |
|---|---|---|
| 3D Fix | `●GPS` | 綠 |
| 2D Fix | `●GPS` | 黃 |
| 搜尋中 | `◌GPS` | 灰 |
| 無定位/逾時 | `✕GPS` | 紅 |
| 模組停用 | `○GPS` | 暗灰 |

> **實作依賴**：⚠️ 目前 `IPC_MSG_DEVICE_STATUS` 只有 `gps_sats`（衛星數），**無 `gps_fix_mode` / `hdop`**，Core 1 無法區分 3D vs 2D Fix。
> 需新增：(a) 擴充 `IPC_MSG_DEVICE_STATUS` 加入 `gps_fix_mode` (0=none, 1=2D, 2=3D) 與 `hdop_x10`；或 (b) Core 1 直接走 Teseo driver（已 ✅）取狀態，不依賴 cascade。
> 過渡期可只區分「有衛星數 ≥4 → ●GPS 綠 / 否則 ◌GPS 灰」兩態。

### 6. 未讀訊息（3 字位，條件顯示）

```
✉3
```

- N = 所有對話 unread_count 總和
- N = 0：不繪製（位置留空）
- N ≥ 1：`✉N` 主色綠 `#39D353`
- N >9：`✉9+`
- 收到新訊息時：`✉` 橙色 `#FFA657` 閃 3 次（每次 200ms）後恢復綠
- 在訊息對話內輸入時：隱藏（避免分心）

### 7. 電量（5 字位）

```
▣87%
```

| 圖示 | 含義 |
|---|---|
| `▣` | 放電中 |
| `⚡` | 充電中 |
| `▪` | 已滿且仍接電源 |
| `!` | 異常（過熱/過壓） |

| 顏色 | 條件 |
|---|---|
| 主色 `#E6EDF3` | >30% |
| 黃 `#F1E05A` | 15-30% |
| 紅 `#F85149` | 5-15% |
| 紅閃爍 | <5%（觸發極簡低電模式） |

### 8. 模式標籤（2 字位）

| 標籤 | 顏色 | 含義 |
|---|---|---|
| `Op` | 主色 `#E6EDF3` | Operation：D-pad 為焦點導航 |
| `注` | 橙 `#FFA657` | SmartZh 注音輸入 |
| `EN` | 橙 `#FFA657` | SmartEn 英文 multitap + 字典輔助 |
| `Ab` | 橙 `#FFA657` | Direct 純 multitap，無字典 |

> **註**：所有標籤統一 **2 字位寬**（`Op`/`注`(全形 1 = 2 半形)/`EN`/`Ab`），Status Bar 模式區寬度固定不抖動。
>
> **MIE 標籤對應**：MIE `mie_mode_label()` 實際輸出 `ABC`（3 字位，見 `firmware/mie/src/ime_display.cpp:23-30`），UI view 層需做 mapping `ABC → Ab` 後再渲染到 Status Bar，不直接拿 MIE 字串往 LVGL 送。其餘 `EN`、`注` 直接對齊。
>
> **Num 模式** ❌ MIE 不存在，數字輸入併入 SmartEn multitap。

**所有橙色標籤都意味「D-pad 行為已切換」**——這是 D-pad-only 裝置最關鍵的視覺確認。

App 名稱（IDLE/CHAT/MAP/CFG）**不顯示**——使用者看畫面內容就知道在哪個 App。

## 字位佈局（精確 X 座標）

| 元素 | 起始 X | 結束 X | 字位 |
|---|---|---|---|
| 時間 | 0 | 40 | 5 |
| 空白 | 40 | 48 | 1 |
| TX/RX | 48 | 64 | 2 |
| 警告燈 | 64 | 72 | 1（條件） |
| 空白 | 72 | 80 | 1 |
| 鄰居數 | 80 | 136 | 7 |
| 空白 | 136 | 144 | 1 |
| GPS | 144 | 176 | 4 |
| 空白 | 176 | 184 | 1 |
| 未讀 | 184 | 208 | 3（條件） |
| 空白 | 208 | 216 | 1 |
| 電量 | 216 | 256 | 5 |
| 空白 | 256 | 288 | 4 |
| 模式 | 288 | 304 | 2 |
| 留白 | 304 | 320 | 2 |

警告燈與未讀為條件顯示，但**位置永遠保留**（不繪製只是該位置留空），不擠壓其他元素。視覺穩定優先。

## 警告態覆蓋層

最高優先警告會**整條 Status Bar 變色覆蓋**正常元素：

### SOS 啟動

```
🚨 SOS 廣播中  下一次 23s   ●Mesh:5 ▣4%
```

- 整條紅底 `#8B1A1A`，1 Hz 閃爍

### 收到他人 SOS

```
⚠ 收到 SOS：阿明  距 12.4km  按 OK 查看
```

- 整條紅底 `#8B1A1A`，常亮（不閃爍，避免疲勞）

### 極低電量（<5%）

```
⚠ 電量不足  4%  請盡快充電
```

- 整條暗紅底 `#6E1A1A`

### 系統嚴重警告（黃級）

不覆蓋整條，只顯示 `⚠` 警告燈。

### 優先序

```
SOS 啟動 (整條覆蓋)
  > 收到 SOS (整條覆蓋)
  > 極低電量 <5% (整條覆蓋)
  > 系統警告 (僅 ⚠ 燈)
  > 正常 Status Bar
```

## FUNC 長按詳情面板

> **註**：目前 Phase 2 開發階段 FUNC 鍵尚無 view-router 消費端（按下無作用），L-1 九宮格實作完成後依本規劃改為「桌面 = 九宮格 / 其他頁 = 該頁定義」。本節以下描述為 L-1 完成後的目標行為。FUNC 長按 ≥2s 的 timer 需在 UI 層另實作（keypad driver 對功能鍵不做 long-press 計時）。

從任何頁面長按 FUNC ≥2 秒（短按是九宮格）彈出 Modal：

```
┌──────────────────────────┐
│ 系統狀態                  │
├──────────────────────────┤
│ 🕐 09:41:23  GMT+8        │
│    上次 GPS 同步 02:15 前  │
│                          │
│ 📡 TX 124 / RX 256        │
│    最近發送 00:12 前      │
│    最近接收 00:03 前      │
│                          │
│ 📻 LongFast               │
│    906.875 MHz / SF11     │
│    BW 250kHz / CR 4/5     │
│                          │
│ 🌐 鄰居 7 / 已知 23       │
│    最近聽到 00:02 前      │
│    Hop 平均 2.1           │
│                          │
│ 📍 3D Fix                 │
│    24.149°N 120.681°E     │
│    HDOP 0.8 衛星 9        │
│                          │
│ ✉ 未讀 3                  │
│    阿明(2) 登山隊(1)      │
│                          │
│ 🔋 87%  4.02V             │
│    -180mA  28.4°C         │
│    剩餘 ~11h24m           │
│                          │
│ 💼 IDLE                   │
│    運行 03:14:22          │
│    韌體 1.0.0+abc123      │
│                          │
│ ⚠ 系統警告 (1)            │
│    GPS 訊號弱             │
├──────────────────────────┤
│ ↑↓ 滾動  Back 關閉         │
└──────────────────────────┘
```

特性：
- 滾動式 Modal
- 警告區段優先在頂端
- 任何鍵（BACK / FUNC / OK）關閉
- 內容每秒重繪

## 互動行為

Status Bar 是**純顯示元素**，全域焦點不會落在它身上。

| 操作 | 行為 |
|---|---|
| FUNC 短按 | 桌面：呼出九宮格 / 其他頁：依該頁定義 |
| FUNC 長按 ≥2s | Status Bar 詳情面板（任何頁面） |
| FUNC 在 SOS 啟動中 | 無作用（鎖定） |

警告態下 Status Bar 變成可互動：

| 警告類型 | 互動 |
|---|---|
| SOS 啟動 | 不可互動（在 SOS App 內處理） |
| 收到 SOS | 按 OK 確認後恢復正常 |
| 極低電量 | 按 OK 暫時關閉警告 30 秒 |
| 系統警告（⚠ 燈） | 按 FUNC 進詳情面板看警告清單 |

## 動態更新時機

不每秒全條重繪，分群更新：

| 元素 | 更新頻率 | 觸發 |
|---|---|---|
| 時間 | 每分鐘 | 分鐘變更 |
| TX 燈 | 即時 | LoRa TX_DONE callback |
| RX 燈 | 即時 | LoRa RX_DONE callback |
| 警告燈 | 1 Hz 慢閃 | 警告 entry/exit |
| 鄰居數 | 每 30 秒 | NodeDB 掃描 |
| GPS | 每秒檢查、變化才繪 | GNSS event |
| 未讀數 | 即時 | 訊息 entry/clear |
| 電量 | 每 30 秒 | ADC 採樣 |
| 模式 | 即時 | 焦點/文字框 entry |

LVGL 局部 `lv_obj_invalidate()`，不重繪整條。

## 配色完整表

| 用途 | 顏色 | Hex |
|---|---|---|
| 背景 | 主背景 | `#0B0F14` |
| 主文字 | 主色 | `#E6EDF3` |
| 次文字 | 次色 | `#7D8590` |
| 不動作灰（TX/RX 暗態） | 暗灰 | `#30363D` |
| 良好狀態 | 綠 | `#39D353` |
| 注意狀態 | 黃 | `#F1E05A` |
| 警告狀態 | 紅 | `#F85149` |
| TX 動作 | 橙 | `#FFA657` |
| RX 動作 | 綠 | `#39D353` |
| TYPE 模式（注/EN/Ab） | 橙 | `#FFA657` |
| 警告燈 ⚠ | 黃 | `#F1E05A` |
| SOS 警告背景 | 深紅 | `#8B1A1A` |
| 極低電警告背景 | 暗紅 | `#6E1A1A` |
| 系統警告背景（不覆蓋） | — | 不變 |

## 實作備忘

### LVGL 結構

```c
typedef struct {
    lv_obj_t* container;
    lv_obj_t* time_label;
    lv_obj_t* tx_indicator;
    lv_obj_t* rx_indicator;
    lv_obj_t* warn_indicator;
    lv_obj_t* mesh_label;
    lv_obj_t* gps_indicator;
    lv_obj_t* gps_label;
    lv_obj_t* unread_label;
    lv_obj_t* battery_indicator;
    lv_obj_t* battery_label;
    lv_obj_t* mode_label;
} status_bar_t;

extern status_bar_t g_status_bar;

void status_bar_init(lv_obj_t* parent);
void status_bar_set_time(uint8_t h, uint8_t m);
void status_bar_pulse_tx(void);
void status_bar_pulse_rx(void);
void status_bar_set_warning(bool on);
void status_bar_set_neighbors(uint8_t count, uint16_t last_heard_sec);
void status_bar_set_gps(gps_state_t state);
void status_bar_set_unread(uint8_t count);
void status_bar_set_battery(uint8_t pct, batt_state_t state);
void status_bar_set_mode(input_mode_t mode);

// 警告態（覆蓋整條）
void status_bar_show_alert(alert_type_t type, const char* msg);
void status_bar_clear_alert(void);
```

### 全域 FUNC 長按監聽

```c
static uint32_t func_press_start = 0;

void on_func_press(void) {
    func_press_start = lv_tick_get();
}

void on_func_release(void) {
    uint32_t held = lv_tick_get() - func_press_start;
    if (held >= 2000) {
        show_status_detail_modal();
    } else {
        send_func_short_press_event();
    }
}
```

### 警告管理

```c
typedef enum {
    WARN_NONE,
    WARN_GPS_LOST,
    WARN_TX_FAIL,
    WARN_SENSOR_ERR,
    WARN_FLASH_FULL,
    WARN_MESH_ISOLATED,
    WARN_FW_INCOMPAT,
    WARN_CONFIG_CONFLICT,
    WARN_COUNT
} warn_type_t;

typedef struct {
    warn_type_t type;
    char description[64];
    uint32_t since_tick;
    bool active;
} warning_t;

extern warning_t g_warnings[WARN_COUNT];

bool any_warning_active(void) {
    for (int i = 0; i < WARN_COUNT; i++)
        if (g_warnings[i].active) return true;
    return false;
}
```

## 設定 App 對應

Status Bar 相關設定（落在 S-4 顯示）：

| 設定項 | 模板 | 預設值 |
|---|---|---|
| 12/24 小時制 | A 列舉 | 24h |
| 整點時間閃爍 | C 開關 | 關 |
| 警告燈閃爍 | C 開關 | 開 |
| Status Bar 詳情長按延時 | B 數值 | 2 秒（0.5-5 秒） |
| TX/RX 燈動畫 | C 開關 | 開 |

## 對其他頁面的影響

Status Bar 定下後，所有頁面繼承：

1. 每頁從 Y=17 開始繪製內容（Y=0~16 為 Status Bar）
2. App 層不需自繪 Status Bar，由全域常駐 widget 負責
3. 進入新文字框時，IME 元件統一呼叫 `status_bar_set_mode(IME_BOPOMOFO)` 等
4. 退出文字框時恢復 `status_bar_set_mode(IME_OP)`
5. 長按 FUNC 2s 是全域行為，App 不應攔截

---

最後更新：2026-04
版本：v2.1（模式區固定 2 字位：注/EN/Ab/Op，view 層做 MIE `ABC→Ab` mapping；移除不存在的 Num；GPS 與 TX/RX 動作燈標實作依賴）
