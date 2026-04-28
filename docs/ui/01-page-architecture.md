# 01 — 62 頁面完整架構

本文件盤點 MokyaLora UI 的所有頁面與全域元件。每頁分配代號，作為後續細部規劃的索引。

## 對應 Core 1 view 實作狀態（2026-04 快照）

下表彙整本架構規劃中各頁面在目前 Core 1 view-router 內的實作對應，作為規劃 ↔ 韌體進度的追蹤介面。

| 頁面代號 | Core 1 view（檔案/變數） | 狀態 | 備註 |
|---|---|---|---|
| L-0 桌面 | ⏳ | 未實作 | 目前以 `rf_debug_view` 作為 boot view（debug 暫行） |
| L-1 九宮格 | ⏳ | 未實作 | FUNC 鍵目前**未消費** `key_event`（不是 cycle）；待 view-router 加 FUNC handler |
| keypad 測試 | `keypad_view` | ✅ debug | 鍵盤診斷用 |
| G-1 Status Bar | ⏳ | 未實作 | 全域常駐 widget 待建立 |
| G-2 Hint Bar | ⏳ | 未實作 | |
| G-3 IME | `ime_view` | ✅ | 注音 / EN / Ab 三模式皆已實作（MIE v4 + LRU；MIE 內部 `ABC` 由 view 層 mapping 為 `Ab`）；MIE 無獨立 Num 模式 |
| G-4 全域 Modal | ⏳ | 未實作 | |
| A-2 對話詳情 | `messages_view` | ✅ 部分 | 8-entry inbox + UP/DOWN 切換 + OK 回覆；對話流預覽未實作 |
| A-2 送出 | `messages_send.{c,h}` | ✅ | OK 鍵讀 `ime_view_text()` → cascade `phoneapi_encode_text_packet` |
| A-1/A-3/A-4 | ⏳ | 未實作 | |
| B-1~B-4 頻道 | ⏳ | 未實作 | |
| C-1 節點清單 | `nodes_view` | ✅ 部分 | cascade `phoneapi_cache` 來源 |
| C-2~C-4 | ⏳ | 未實作 | |
| D-1~D-6 地圖 | ⏳ | 未實作 | |
| F-1~F-4 遙測 | ⏳ | 未實作 | |
| T-1~T-8 工具 | ⏳ | 未實作 | 部分 debug view 可作雛形（`rf_debug_view`） |
| S-0~S-12 設定 | `settings_view` | ✅ 部分 | B3 已覆蓋 88 keys / 15 groups（含 ModuleConfig 4 群） |
| S-X 模板 A/B/C/D | ⏳ | 未實作 | settings_view 目前未走四模板抽象，每項自繪 |
| Z-1~Z-3 SOS | ⏳ | 未實作 | 前置：power button driver、極簡低電狀態機 |
| 字型 | `mie_font.{c,h}` + `font_test_view` | ✅ | MIEF v1 / Unifont sm 16 |

> **更新慣例**：每次有 Core 1 view 完成或重命名時，本表需同步更新。狀態欄使用 ✅ 完成 / ✅ 部分 / 🔄 進行中 / ⏳ 未實作。


## 頁面總計

| 區塊 | 頁數 |
|---|---|
| Launcher | 1 |
| 全域元件 | 4 |
| 訊息 App | 4 |
| 頻道 App | 4 |
| 節點 App | 4 |
| 地圖 App（整合航點） | 6 |
| 遙測 App | 4 |
| 工具 App | 9（含主選單） |
| 設定 App 主頁 + 二級頁 | 1 + 12 |
| 設定模組子頁 | 10 |
| SOS App | 3 |
| **頁面合計** | **62 頁** |
| 葉節點設定項 | 約 70+，全部走四種模板 |

## L · Launcher 層

| 編號 | 頁面 | 說明 |
|---|---|---|
| L-0 | 桌面（Home/Dashboard） | 開機後主畫面，狀態儀表 + 通知中樞 |
| L-1 | 九宮格功能表 | Func 鍵呼出，9 個 App 入口 |

## G · 全域元件

| 編號 | 元件 | 說明 |
|---|---|---|
| G-1 | Status Bar | 16px 頂端列：時間 / TX·RX / 警告燈 / 鄰居 / GPS / 未讀 / 電量 / 模式 |
| G-2 | Hint Bar | 16px 底端列：動態顯示當前焦點可用按鍵 |
| G-3 | IME 子模式 | 文字輸入子模式（兩種編輯模式：A 簡易、B 全螢幕） |
| G-4 | 全域 Modal | 來訊提示、SOS 警報、低電警告、二次確認對話框、草稿恢復 |

## A · 訊息 App

| 編號 | 頁面 | 說明 |
|---|---|---|
| A-1 | 對話列表 | DM 與頻道對話混合，依最近活動排序 |
| A-2 | 對話詳情 | 訊息流 + 輸入區 |
| A-3 | 訊息詳情 | 長按某則訊息：原始 packet、SNR/RSSI、跳數、ACK、Traceroute |
| A-4 | 預設訊息（Canned） | 快發訊息列表（5-way 裝置的關鍵功能） |

> **實作落差**：cascade `phoneapi_msgs_ring` 容量目前 = **4**（單條訊息級別 ring，非完整對話歷史），不足以支撐 A-1 對話列表 / A-2 訊息流長期保留；需新增 Core 1 端對話 ring buffer（建議每對話保留最後 N 條，PSRAM 持久化）。
> ACK 狀態目前由 `messages_view` 自行追蹤 `s_pending_tx_*` + 解析 cascade `Routing(ACK)` / `QueueStatus`，**無全域 TX 追蹤層**；A-3 訊息詳情 ACK 顯示需以此為基礎擴充。

## B · 頻道 App

| 編號 | 頁面 | 說明 |
|---|---|---|
| B-1 | 頻道列表 | Primary + Secondary 0-7，含啟用狀態 |
| B-2 | 頻道編輯 | 名稱、PSK、Role、Uplink/Downlink、Position Precision |
| B-3 | 加入頻道 | 手動輸入 / 接收 admin packet |
| B-4 | 分享頻道 | 顯示 URL（給對方手機掃）⚠️ QR 渲染 codec 尚未實作 |

> **實作落差（B-4）**：Meshtastic channel-share URL 編碼（base64 of ChannelSettings protobuf）需在 Core 1 加 codec；目前 cascade phoneapi 只暴露 `IPC_CFG_CHANNEL_*` 讀取，無 URL 產生器。QR 渲染需引入 QR encoder（如 `qrcodegen`，~5 KB code）。

## C · 節點 App

| 編號 | 頁面 | 說明 |
|---|---|---|
| C-1 | 節點清單 | 排序：SNR / 最近 / 距離 / 名稱 |
| C-2 | 節點詳情 | Long/Short Name、Node ID、HW、Role、SNR/RSSI、距離方位、電量、座標、Public Key |
| C-3 | 節點操作選單 | DM、別名、收藏、忽略、Traceroute、Request Position、Remote Admin |
| C-4 | 我的節點 | 編輯本機 Long/Short Name、Role、Licensed |

## D · 地圖 App（整合航點）

依設計決策，地圖 App 整合航點功能（不獨立）。

| 編號 | 頁面 | 說明 |
|---|---|---|
| D-1 | 主地圖 | 向量極座標式：本機為中心，其他節點為點，距離環 |
| D-2 | 圖層切換 | 節點 / 航跡 / 航點 / 全顯 |
| D-3 | 航點清單 | 自建 + 收到的，可篩選 |
| D-4 | 航點詳情 | 座標、名稱、icon、過期、廣播者 |
| D-5 | 新增航點 | 用當前位置 / 手動輸入座標 |
| D-6 | 航點導航 | 鎖定航點：方位箭頭 + 距離 + 預估到達 |

## F · 遙測 App

| 編號 | 頁面 | 說明 |
|---|---|---|
| F-1 | 本機遙測 | 電量、電壓、運行時間、空中時間佔比、Channel utilisation |
| F-2 | 環境感測 | 溫濕度、氣壓、空氣品質（若有感測器） |
| F-3 | 鄰居資訊 | NeighborInfo：每鄰居的 SNR/RSSI 矩陣 |
| F-4 | 歷史曲線 | 電量、訊號、空中時間趨勢圖 |

## T · 工具 App

| 編號 | 頁面 | 說明 |
|---|---|---|
| T-0 | 工具主選單 | 集合入口 |
| T-1 | Traceroute | 發起與結果顯示 |
| T-2 | Range Test | 模組控制與結果 |
| T-3 | 訊號頻譜 | SX1262 RSSI 掃描視覺化 |
| T-4 | 封包嗅探 | 原始封包流 + hex dump |
| T-5 | LoRa 自我測試 | Loopback、TX/RX 健康檢查 |
| T-6 | GNSS 衛星圖 | 仰角/方位/SNR 條 |
| T-7 | 配對碼顯示 | Admin Channel 配對用 |
| T-8 | 韌體資訊 | 版本、Build hash、檢查更新 |

## S · 設定 App（樹狀，葉節點全部走四模板）

### S-0 · 設定主頁

頂端顯示：本機暱稱 / 角色 / 區域 / Preset / TX 功率
分組：常用 / 通訊 / 進階（折疊）

> **B3 IPC 設定覆蓋現況（2026-04 校對）**：cascade IPC_CFG_* 約 60+ key 已就緒（Device / LoRa / Position / Display / Power / Security / Owner / Channel / Telemetry / NeighborInfo / RangeTest / DetectionSensor / CannedMessage / AmbientLighting / Paxcounter）。**仍缺**：Store & Forward (S-7.4)、External Notification (S-7.2)、Serial (S-7.9)、Remote Hardware (S-7.10) 四個 ModuleConfig 群組；對應子頁實作前需先補 IPC_CFG_* 與 cascade decoder。

### S 常用區

| 編號 | 二級頁 | 葉節點數量（含進階折疊） |
|---|---|---|
| S-1 | 無線電 | 5 常用 + 8 進階 |
| S-2 | 裝置 | 6 常用 + 6 進階 |
| S-3 | 位置 | 6 常用 + 4 進階 |
| S-4 | 顯示 | 13 常用 + 3 進階 |
| S-5 | 電源 | 5 常用 + 5 進階 |

### S 通訊區

| 編號 | 二級頁 | 內容 |
|---|---|---|
| S-6 | 加密與金鑰 | Public/Private Key、Admin Key×3、Is Managed、PKC Disabled、Admin Channel |
| S-7 | 模組 | 12 個模組各一頁（見下） |
| S-8 | 遠端管理 | 選擇目標節點 → 鏡像本機所有設定項 |

### S-7 模組子頁（10 頁）

| 編號 | 模組頁 | 說明 |
|---|---|---|
| S-7.1 | Canned Message | 訊息列表、輸入模式、目標頻道 |
| S-7.2 | External Notification | GPIO、亮燈/蜂鳴、持續時間 |
| S-7.3 | Range Test | 啟用、間隔、儲存 |
| S-7.4 | Store & Forward | 啟用、Heartbeat、記錄筆數 |
| S-7.5 | Telemetry | Device/Environment/Air Quality 廣播間隔 |
| S-7.6 | Detection Sensor | GPIO、觸發邏輯、最小間隔 |
| S-7.7 | Paxcounter | 啟用、間隔、計數（⚠️ 本機無 WiFi/BT radio，實際無法掃描；保留設定 UI 供 Meshtastic 設定鏡像之用） |
| S-7.8 | Neighbor Info | 啟用、間隔、跨度 |
| S-7.9 | Serial | Baud、模式、GPIO |
| S-7.10 | Remote Hardware | GPIO 控制清單 |

> **已移除**：原 S-7.11 Audio（無揚聲器/麥克風硬體）、原 S-7.12 MQTT（無 WiFi/Ethernet 硬體，無上行通道）。

### S 進階區（預設折疊）

| 編號 | 二級頁 | 說明 |
|---|---|---|
| S-9 | 匯入匯出 | 設定包、SD 備份還原、URL |
| S-10 | 開發者選項 | Serial Console、Debug Log、原始封包視窗 |
| S-11 | 危險區 | 重啟、Shutdown、Factory Reset（二次確認） |
| S-12 | 版本資訊 | 韌體、Protocol、Build、HW Rev、空中時間、開機時間 |

> **已移除**：原 S-9 音訊（無揚聲器硬體）、原 S-10 網路（無 WiFi/Ethernet 硬體）、原 S-11 藍牙（無 BT 硬體）。S-12~S-15 順移為 S-9~S-12。原 S-12 匯入匯出的「QR 設定包」改為純 URL/SD 備份（無相機）。

### S-X · 葉節點四種模板（共用元件）

| 模板 | 用途 | 數量估計 |
|---|---|---|
| 模板 A | 列舉選一 | ~30 項 |
| 模板 B | 數值輸入 | ~25 項 |
| 模板 C | 開關 | ~20 項 |
| 模板 D | 文字輸入 | ~10 項 |

詳見 `50-settings-leaf-templates.md`。

## Z · SOS App

**前置條件**（規劃時須先完成）：
- Power button event handler（GPIO 中斷 + 5 秒長按 timer），目前 keypad driver 不含電源鍵。
- 極簡低電模式狀態機（與 L-0 桌面共用），目前未實作。
- SOS broadcast IPC command — cascade 目前無對應路由，需新增 `IPC_CMD_SEND_SOS` 或沿用 `IPC_CMD_SEND_TEXT` 走 broadcast channel + 特定 portnum。

| 編號 | 頁面 | 說明 |
|---|---|---|
| Z-1 | SOS 待機 | 編輯求救文字、預覽廣播內容 |
| Z-2 | SOS 啟動中 | 紅色全屏：已廣播次數、收到回應、剩餘電量 |
| Z-3 | SOS 設定 | 廣播間隔、目標頻道、附加感測器資料 |

## 細部規劃建議順序

依使用頻率與設計風險排序：

1. ✅ **G-1 Status Bar** + **G-2 Hint Bar**（先定全域骨架）
2. ✅ **G-3 IME 子模式**（影響所有文字輸入頁）
3. ✅ **L-0 桌面**（外觀基調）
4. ✅ **S-X 設定樹四模板**（一次定下，80+ 葉節點受益）
5. ⏳ **L-1 九宮格功能表**
6. ⏳ **A-1 對話列表** → **A-2 對話詳情** → **A-4 Canned**（最高頻使用）
7. ⏳ **C-1 節點清單** → **C-2 節點詳情**（第二高頻）
8. ⏳ **B-1 頻道列表** → **B-2 頻道編輯**
9. ⏳ **D-1 主地圖**（最特殊的視覺）
10. ⏳ 其他二級設定頁
11. ⏳ **Z-1 ~ Z-3 SOS**
12. ⏳ **F**、**T** 系列收尾

## 進度標記慣例

- ✅ 已完成定稿（有獨立規格文件）
- ⏳ 待規劃
- 🔄 規劃中
- ❌ 暫不規劃 / 已從架構移除

---

最後更新：2026-04-29
版本：v1.1（移除 Audio/MQTT/Network/BT 模組；S 進階 S-12~S-15 順移；B-3 移除 QR；新增 Core 1 view 對照表；補各 App 實作落差註記；頁數 67 → 62）
