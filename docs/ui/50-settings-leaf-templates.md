# 50 — S-X 設定葉節點四模板

## 角色定位

設定樹有 80+ 葉節點（Region、Modem Preset、TX 功率、Long Name、Hop Limit…），但所有葉節點的編輯介面**只用 4 種模板**。使用者學會這 4 種模板後，所有設定項操作就熟練了。

| 代號 | 名稱 | 用途 | 範例 | 數量 |
|---|---|---|---|---|
| **A** | 列舉選一 | 從固定選項中選一個 | Region、Preset、Role | ~30 項 |
| **B** | 數值輸入 | 輸入一個數值 | TX 功率、Hop Limit、間隔秒數 | ~25 項 |
| **C** | 開關 | True/False 二選一 | TX 啟用、Smart Position | ~20 項 |
| **D** | 文字輸入 | 自由文字字串 | Long Name、PSK、Admin Channel Key | ~5 項 |

## 共通骨架

四種模板共用：

```
┌────────────────────────────────────────┐
│ STATUS BAR                             │ 16
├────────────────────────────────────────┤
│ ⚙ 麵包屑：a › b › c                    │ 20
│ 設定項名稱                              │ 20
│                                        │
│ ┌────────────────────────────────────┐ │
│ │                                    │ │
│ │     模板特異區（A/B/C/D 不同）       │ │ 動態高度
│ │                                    │ │
│ └────────────────────────────────────┘ │
│                                        │
│ 說明文字                                │ 18×N
│ ......                                 │
│                                        │
└────────────────────────────────────────┘
（無 Hint Bar，整個設定 App 不顯示 Hint Bar）
```

## 共通元素規格

| 元素 | 字級 | 顏色 |
|---|---|---|
| 麵包屑 | 14px | `#7D8590` |
| 標題 | 16px Unifont | `#E6EDF3` |
| 主文字（選項、值） | 16px | `#E6EDF3` |
| 次文字（提示、單位） | 14px | `#7D8590` |
| 焦點 ▶ | 16px | `#FFA657` |
| 大字數值（模板 B） | 32px Unifont ×2 | `#FFA657` |
| 警告文字 | 14px | `#F1E05A` 黃 / `#F85149` 紅 |
| 內容區框背景 | — | `#161C24` |

### 麵包屑規格

```
⚙ 無線電 › Modem Preset
⚙ 無線電 › 進階 › 頻率覆寫
⚙ 模組 › Canned Message › 訊息列表 › #3
⚙ 設定 ›… › Telemetry › Device 廣播間隔   ← 過長截斷
```

| 規則 | 內容 |
|---|---|
| 第 1 段 | 永遠是 `⚙` |
| 分隔符 | `›` |
| 最大顯示深度 | 4 段（含 `⚙` 與末段） |
| 超過 4 段 | 中段壓縮為 `…` |
| 顏色 | 全部次色 `#7D8590` |
| 長度 | 最多 36 半形字位（=288px），超過末段截斷 |
| 字級 | 14px |

### 說明文字規格

| 規則 | 內容 |
|---|---|
| 字級 | 14px Unifont CJK + 12px 西文 |
| 顏色 | 次色 `#7D8590` |
| 行高 | 18px |
| 最多顯示行數 | 4 行 |
| 自動 word wrap | 是 |
| 對齊 | 左對齊，4px padding |

## 模板 A：列舉選一

### 線框圖

```
┌────────────────────────────────────────┐
│ STATUS BAR                             │ 16
├────────────────────────────────────────┤
│ ⚙ 無線電 › Modem Preset                │ 20
│ Modem Preset                           │ 20
│                                        │ 4
│ ┌────────────────────────────────────┐ │
│ │ ○ ShortFast                        │ │ 22
│ │ ○ ShortSlow                        │ │ 22
│ │ ●▶LongFast      （當前）           │ │ 22  焦點 + 當前
│ │ ○ LongModerate                     │ │ 22
│ │ ○ LongSlow                         │ │ 22
│ │ ↓ 還有 1 項                        │ │ 14  滾動提示
│ └────────────────────────────────────┘ │
│                                        │ 4
│ 影響傳輸速度與距離。變更後本機與所有同網    │ 18
│ 節點需一致才能通訊。                     │ 18
└────────────────────────────────────────┘
                       16+20×2+4+22×5+14+4+18×2 = 224 ✓
```

### 元素規格

| 元素 | 規格 |
|---|---|
| **選項標記** | `●` 主色（當前已套用）/ `○` 灰（未套用） |
| **焦點標記** | `▶` 橙色 `#FFA657`，疊加在 highlight 行 |
| **選項列高** | 22px |
| **選項文字** | 主色 16px |
| **括號註釋** | 次色 14px，靠右排（如「最快、最近」、「當前」） |
| **每頁顯示行數** | 5 行（焦點所在置中） |
| **滾動** | 列表外圈不動，內容捲動，焦點永遠在中央附近 |
| **滾動提示** | 列表頂/底顯示 `↑ 還有 N 項` / `↓ 還有 N 項` |

### 互動行為

| 鍵 | 行為 |
|---|---|
| **D-pad ▲▼** | 移動 `▶` 焦點 |
| **D-pad ◀▶** | 無作用 |
| **OK 短按** | 一般項：套用並回上層 / 危險項：進二次確認 |
| **BACK** | 取消，回上層 |
| **MODE** | 無作用 |
| **注音/字母鍵** | 無作用（純列表選擇） |

### 視覺狀態

**焦點 = 當前已套用值**：

```
●▶LongFast      （當前）
```

OK 按下時不執行任何動作（已是當前值），但**仍離開頁面**。

**焦點 ≠ 當前值**：

```
○▶ShortFast
```

OK 按下時設定變更為 ShortFast，立即返回上層。

### 危險項二次確認

危險項清單：

**Reboot 觸發**：Region、Modem Preset、Role、主頻道 PSK、Factory Reset、Firmware Update
**斷聯風險**：TX 啟用→停用、Hop Limit=0、Frequency Override
**隱私敏感**：Position 廣播切換、Position Precision 降級、Admin Channel 啟用

二次確認版面：

```
┌────────────────────────────────────────┐
│ ⚙ 無線電 › Modem Preset                │ 20
│ Modem Preset                           │ 20
│                                        │ 4
│ ┌────────────────────────────────────┐ │
│ │ ○ LongFast      （當前）           │ │ 22
│ │ ○ LongModerate                     │ │ 22
│ │ ●▶VeryLongSlow                    │ │ 22  焦點移到危險選項
│ │ ↑ 已是最後一項                      │ │ 14
│ └────────────────────────────────────┘ │
│                                        │ 4
│ ⚠ 變更 Preset 將與目前網路斷聯          │ 18
│   且觸發本機重啟。                       │ 18
│                                        │ 4
│      [取消]      [▶確認變更]            │ 24
└────────────────────────────────────────┘
                              16+20×2+4+22×3+14+4+18×2+4+24 = 184 ✓
```

**互動**：
- 焦點先在選項列表
- 按 OK 不直接套用，焦點跳到下方按鈕區
- D-pad ◀▶ 在「取消」「確認變更」之間切換
- 按 OK 在「確認變更」上 → 套用並回上層
- 按 OK 在「取消」上 → 焦點跳回選項列表
- 按 BACK → 直接回上層不套用

## 模板 B：數值輸入

### 線框圖

```
┌────────────────────────────────────────┐
│ STATUS BAR                             │ 16
├────────────────────────────────────────┤
│ ⚙ 無線電 › TX 功率                     │ 20
│ TX 功率                                 │ 20
│                                        │ 8
│ ┌────────────────────────────────────┐ │
│ │                                    │ │ 16
│ │            ▶22◀  dBm               │ │ 40 大字 ×2
│ │                                    │ │ 16
│ │     範圍：0 - 22 dBm               │ │ 18
│ │     當前：22 dBm                   │ │ 18
│ │                                    │ │ 8
│ └────────────────────────────────────┘ │
│                                        │ 4
│ 發射功率上限。法規上限與 Region 設定相關。│ 18
│ 越高傳得越遠但耗電越多。                  │ 18
└────────────────────────────────────────┘
                              16+20×2+8+(16+40+16+18×2+8)+4+18×2 = 224 ✓
```

### 元素規格

| 元素 | 規格 |
|---|---|
| **大字數值** | 32px 高度區、橙色 `#FFA657`、Unifont 16px ×2 倍放大 |
| **左右箭頭** | `▶22◀` 暗示「可調整」，主色 |
| **單位** | 數值右側，主色 16px（如 `dBm`、`秒`、`m`、`%`） |
| **範圍提示** | 18px 一行，次色 |
| **當前值** | 18px 一行，次色（變更前的值） |

### 互動行為

| 鍵 | 行為 |
|---|---|
| **D-pad ▲** | +1 |
| **D-pad ▼** | -1 |
| **D-pad ▶** | +10（粗調） |
| **D-pad ◀** | -10（粗調） |
| **D-pad 長按** | 連續變化，3 秒後步進變 ±100 |
| **數字鍵 0-9** | 直接輸入覆蓋當前值 |
| **DEL** | 刪除最後一位 |
| **OK** | 套用，回上層 |
| **BACK** | 取消，回上層 |

### 數字鍵直接輸入邏輯

進入頁面 `dirty = false`：

| 操作 | 結果 |
|---|---|
| 第一次按數字鍵（dirty=false） | 清空 buffer，寫入該數字，dirty=true |
| 後續數字鍵（dirty=true） | append 到 buffer |
| D-pad 步進 | 以當前 buffer 值步進，dirty=true |
| DEL | 刪除最後一位 |
| 超範圍 | 紅色警告，OK 阻止套用 |

### 數值類型細分

| 類型 | 範例 | 顯示 |
|---|---|---|
| 整數 | TX 功率（0-22） | `22 dBm` |
| 浮點 | 頻率（906.875） | `906.875 MHz` |
| 時間 | 廣播間隔（300 秒） | `5 分鐘` 或 `300 秒` |
| 百分比 | 低電門檻（5%） | `5 %` |
| 負值 | 海拔修正（-5 m） | `-5 m` |
| 十六進位 | 頻率時隙（0x0F） | `0x0F` |

D-pad 行為對所有類型一致，但**步進單位**可由設定項定義（例如頻率 ±0.025 MHz、時間 ±60 秒）。

### 時間值人性化顯示

```
┌────────────────────────────────────┐
│             ▶5分00◀                │  ← 5 分鐘
│      範圍：30 秒 - 60 分鐘          │
│      當前：5 分鐘                  │
└────────────────────────────────────┘
```

D-pad ▲▼ 按秒數步進，但顯示「分:秒」格式。

## 模板 C：開關

### 線框圖

```
┌────────────────────────────────────────┐
│ STATUS BAR                             │ 16
├────────────────────────────────────────┤
│ ⚙ 無線電 › TX 啟用                     │ 20
│ TX 啟用                                 │ 20
│                                        │ 16
│ ┌────────────────────────────────────┐ │
│ │                                    │ │ 24
│ │     啟用 ────●──── 停用            │ │ 24
│ │                                    │ │ 24
│ │     當前：啟用                      │ │ 18
│ │                                    │ │ 12
│ └────────────────────────────────────┘ │
│                                        │ 8
│ 關閉後本機僅收不發（純監聽模式）。        │ 18
│ 此模式適合在頻段擁擠時減少自身佔用，但    │ 18
│ 你將無法回應他人訊息。                   │ 18
└────────────────────────────────────────┘
                              16+20×2+16+(24×3+18+12)+8+18×3 = 224 ✓
```

### 元素規格

滑桿在左邊 = 啟用 / 滑桿在右邊 = 停用。按 D-pad ◀▶ 滑桿即時左右移動 + 「當前：xxx」即時更新，OK 套用。

### 互動行為

| 鍵 | 行為 |
|---|---|
| **D-pad ◀** | 切到左側選項 |
| **D-pad ▶** | 切到右側選項 |
| **D-pad ▲▼** | 無作用 |
| **OK** | 套用並回上層 |
| **BACK** | 取消，回上層 |

### 「啟用/停用」之外的二值選項

| 設定項 | 左 | 右 |
|---|---|---|
| TX 啟用 | 啟用 | 停用 |
| Smart Position | 啟用 | 停用 |
| Licensed | 是 | 否 |
| Is Managed | 是 | 否 |
| Compass Heading | 真北 | 磁北 |
| 12/24 小時制 | 12 | 24 |
| 高度單位 | 公尺 | 英尺 |
| 距離單位 | 公里 | 英哩 |

依設定項聲明，左右按鈕的文字可自訂。**每個項目視覺位置固定**——使用者一旦習慣某設定項是「左 = 真北」，不會混淆。

## 模板 D：文字輸入

### 線框圖

```
┌────────────────────────────────────────┐
│ STATUS BAR（注，橙）                    │ 16
├────────────────────────────────────────┤
│ ⚙ 裝置 › Long Name                     │ 20
│ Long Name                              │ 20
│                                        │ 8
│ ┌────────────────────────────────────┐ │
│ │ MokyaLora-洛克▒ㄎㄜˋ▒▌      14/39 │ │ 24
│ └────────────────────────────────────┘ │
│                                        │ 8
│ 節點長名稱，最多 39 位元組。將在 mesh    │ 18
│ 網路上廣播，建議使用個人代號方便其他     │ 18
│ 節點識別。                              │ 18
│                                        │ 動態
├────────────────────────────────────────┤
│ ►刻 克 課 客 嗑 哭 苛 渴 ‹1/3›         │ 18 IME Bar（條件）
└────────────────────────────────────────┘
```

### 規格

模板 D **直接複用 G-3 IME 模式 A 簡易編輯**，不重新發明。詳見 `12-ime.md`。

唯一差異：模板 D 在文字輸入區下方仍有「說明文字」，是設定頁的元件。

### 互動行為

繼承 G-3 模式 A 簡易編輯：

| 鍵 | 行為 |
|---|---|
| 注音/字母鍵 | 輸入 |
| **OK 短按** | 套用並回上層 |
| **BACK** | 取消（不套用），回上層 |
| **D-pad** | 候選字 / 游標 |
| **MODE** | IME 切換 |

### 文字輸入驗證

某些欄位有格式要求：

| 欄位 | 驗證 | 失敗處理 |
|---|---|---|
| PSK | hex 32 位元組 | 紅色提示「PSK 格式錯誤」，OK 不套用 |
| Admin Channel Key | hex 32 位元組 | 紅色提示「Key 格式錯誤」 |
| Long Name | 1-39 byte UTF-8 | 字數計數變紅 |
| Short Name | 1-4 byte UTF-8 | 字數計數變紅 |

驗證失敗時：字數計數變紅、下方加紅色警告訊息、OK 按下不套用（焦點留在文字框）。

## 套用流程（無 Toast）

```
1. 使用者按 OK
2. 危險項 → 進二次確認；一般項 → 直接執行 step 3
3. App 端驗證
   ├ 失敗 → 顯示錯誤、留在編輯頁、焦點回內容區
   └ 成功 ↓
4. 寫入 config（背景）
5. 立即返回上一層（無 toast）
6. 上一層列表顯示新值
7. 若需 reboot → 0.5 秒後觸發
```

無視覺確認 toast——OK 按下即離開、不顯示任何提示。回到上一層列表時，列表上的當前值已更新——這就是回饋本身。

## App 端 widget API

```c
// 共通 IME 模式（對齊 MIE：firmware/mie/include/mie/ime_logic.h）
typedef enum {
    IME_OP,
    IME_BOPOMOFO,   // 注（SmartZh）
    IME_EN_DICT,    // EN（SmartEn）
    IME_DIRECT,     // Ab（UI 顯示）— MIE 內部 Direct，標籤 `ABC` 由 view 層 mapping 為 `Ab`
    // IME_NUM 不存在；數字欄位由 view 層 bypass MIE
} ime_mode_t;

// 模板 A：列舉選一
typedef struct {
    const char* name;
    const char* hint;        // 括號註釋（如「最快、最近」）
    int value;
    bool is_dangerous;       // 是否觸發二次確認
} enum_option_t;

typedef struct {
    const char** breadcrumb;
    int breadcrumb_depth;
    const char* title;
    const char* description;
    const enum_option_t* options;
    int option_count;
    int current_value;
    void (*on_apply)(int new_value);
    void (*on_cancel)(void);
} enum_setting_config_t;

void show_enum_setting(const enum_setting_config_t* cfg);

// 模板 B：數值輸入
typedef struct {
    const char** breadcrumb;
    int breadcrumb_depth;
    const char* title;
    const char* description;
    int min, max;
    int step_small;
    int step_large;
    const char* unit;
    const char* (*format_value)(int value);  // 自訂顯示格式
    int current_value;
    void (*on_apply)(int new_value);
    void (*on_cancel)(void);
} number_setting_config_t;

void show_number_setting(const number_setting_config_t* cfg);

// 模板 C：開關
typedef struct {
    const char** breadcrumb;
    int breadcrumb_depth;
    const char* title;
    const char* description;
    const char* label_left;
    const char* label_right;
    bool current_value;      // false=左、true=右
    void (*on_apply)(bool new_value);
    void (*on_cancel)(void);
} toggle_setting_config_t;

void show_toggle_setting(const toggle_setting_config_t* cfg);

// 模板 D：文字輸入
typedef struct {
    const char** breadcrumb;
    int breadcrumb_depth;
    const char* title;
    const char* description;
    char* buffer;
    int max_length;
    ime_mode_t default_ime;
    bool dict_enabled;
    bool (*validate)(const char* text);
    const char* (*get_error_message)(const char* text);
    void (*on_apply)(const char* text);
    void (*on_cancel)(void);
} text_setting_config_t;

void show_text_setting(const text_setting_config_t* cfg);
```

## 設定項聲明（資料模型範例）

每個設定項在程式中聲明，模板自動套用：

```c
// 範例 1：Modem Preset（模板 A）
const enum_option_t modem_preset_options[] = {
    {"ShortTurbo", "最快、最近", 0, false},
    {"ShortFast", NULL, 1, false},
    {"ShortSlow", NULL, 2, false},
    {"LongFast", NULL, 3, false},
    {"LongModerate", NULL, 4, false},
    {"LongSlow", NULL, 5, false},
    {"VeryLongSlow", "最慢、最遠", 6, true},  // 危險
};

const enum_setting_config_t modem_preset_setting = {
    .breadcrumb = (const char*[]){"無線電", "Modem Preset"},
    .breadcrumb_depth = 2,
    .title = "Modem Preset",
    .description = "影響傳輸速度與距離的取捨。變更後本機與所有同網節點需一致才能通訊。新設定立即套用，會觸發本機重啟。",
    .options = modem_preset_options,
    .option_count = 7,
    .current_value = get_current_preset(),
    .on_apply = apply_modem_preset,
    .on_cancel = NULL,
};

// 範例 2：TX 功率（模板 B）
const number_setting_config_t tx_power_setting = {
    .breadcrumb = (const char*[]){"無線電", "TX 功率"},
    .breadcrumb_depth = 2,
    .title = "TX 功率",
    .description = "發射功率上限。法規上限與 Region 設定相關。",
    .min = 0,
    .max = 22,
    .step_small = 1,
    .step_large = 5,
    .unit = "dBm",
    .format_value = NULL,
    .current_value = get_current_tx_power(),
    .on_apply = apply_tx_power,
    .on_cancel = NULL,
};

// 範例 3：TX 啟用（模板 C）
const toggle_setting_config_t tx_enable_setting = {
    .breadcrumb = (const char*[]){"無線電", "TX 啟用"},
    .breadcrumb_depth = 2,
    .title = "TX 啟用",
    .description = "關閉後本機僅收不發（純監聽模式）。",
    .label_left = "啟用",
    .label_right = "停用",
    .current_value = !get_tx_enabled(),
    .on_apply = apply_tx_enable,
    .on_cancel = NULL,
};

// 範例 4：Long Name（模板 D）
const text_setting_config_t long_name_setting = {
    .breadcrumb = (const char*[]){"裝置", "Long Name"},
    .breadcrumb_depth = 2,
    .title = "Long Name",
    .description = "節點長名稱，最多 39 位元組 UTF-8。將在 mesh 網路上廣播。",
    .buffer = long_name_buffer,
    .max_length = 39,
    .default_ime = IME_BOPOMOFO,
    .dict_enabled = true,
    .validate = validate_long_name,
    .get_error_message = long_name_error,
    .on_apply = apply_long_name,
    .on_cancel = NULL,
};
```

## 對 80+ 設定項的開發影響

四模板定下後，新增設定項只需：

1. 寫一個 config 結構（資料）
2. 在某個二級設定頁的列表中加一行入口
3. 寫 `on_apply` callback（實際寫入 Meshtastic config）

**不需要重新設計頁面**。整個設定 App 維護工作量比逐頁設計減少 90%。

## 配色完整表

| 用途 | Token | Hex |
|---|---|---|
| 背景 | bg_primary | `#0B0F14` |
| 內容區框 | bg_secondary | `#161C24` |
| 主文字（標題、選項） | text_primary | `#E6EDF3` |
| 次文字（麵包屑、說明、單位） | text_secondary | `#7D8590` |
| 焦點邊框 | accent_focus | `#FFA657` |
| 焦點 ▶ 標記 | accent_focus | `#FFA657` |
| 當前已套用值 ● | text_primary | `#E6EDF3` |
| 未套用值 ○ | text_secondary | `#7D8590` |
| 大字數值（模板 B） | accent_focus | `#FFA657` |
| 警告紅 | warn_red | `#F85149` |
| 警告黃 | warn_yellow | `#F1E05A` |

---

最後更新：2026-04-29
版本：v1.1（IME enum 對齊 MIE Direct；移除 MQTT/Wi-Fi/Audio 範例；驗證表改用 Admin Channel Key + Short Name）
