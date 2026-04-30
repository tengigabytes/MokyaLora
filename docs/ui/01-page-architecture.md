# 01 — 62 頁面完整架構

本文件盤點 MokyaLora UI 的所有頁面與全域元件。每頁分配代號，作為後續細部規劃的索引。

## 對應 Core 1 view 實作狀態（2026-04-30 快照）

下表彙整本架構規劃中各頁面在目前 Core 1 view-router 內的實作對應，作為規劃 ↔ 韌體進度的追蹤介面。

| 頁面代號 | Core 1 view（檔案/變數） | 狀態 | 備註 |
|---|---|---|---|
| L-0 桌面 | `boot_home_view` | ✅ | 5 telemetry + 3 messages + 1 event 對齊 spec（commit `80f7a55`） |
| L-1 九宮格 | `launcher_view` | ✅ 部分 | FUNC 鍵走 view-router 預設行為；spec-named 9 格（commit `188b2a8` / `ee3a72e`），其中 Map / Power 兩格仍 `VIEW_ID_COUNT`（disabled placeholder），待 D-1 / 電源 App 上線 |
| keypad 測試 | `keypad_view` | ✅ debug | 鍵盤診斷用 |
| G-1 Status Bar | `global/status_bar` | ✅ | 全域常駐 widget；TX/RX activity pulse（commit `7e5cd1b`） |
| G-2 Hint Bar | `global/hint_bar` | ✅ | 由 view descriptor `.hints` 驅動（commit `538b614`） |
| G-3 IME | `ime_view` | ✅ | 注音 / EN / Ab 三模式（MIE v4 + LRU）；Mode A inline overlay 給 A-2（commit `47cb07c`）。MIE 無獨立 Num 模式 |
| G-4 全域 Modal | `global/global_alert` | ✅ | DM toast + low-battery alert orchestrator（commit `2bbab46` + `7852fe6` no-battery guard） |
| A-1 對話列表 | `chat_list_view` | ✅ | DM store 後端（commit `0ceb082`、`b0094c3`） |
| A-2 對話詳情 | `conversation_view` | ✅ | 對話流 + Mode A inline IME overlay（commit `0ceb082`、`47cb07c`） |
| A-2 送出 | `messages/messages_send.{c,h}` | ✅ | OK 鍵讀 `ime_view_text()` → cascade `phoneapi_encode_text_packet`；TX 狀態追蹤走 `messages/messages_tx_status` + `messages/dm_store` |
| A-3 訊息詳情 modal | `message_detail_view` | ✅ | FUNC long-press 觸發（commit `a04d909`） |
| A-4 罐頭訊息 | `canned_view` + `canned_messages` | ✅ | LEFT 在 conversation_view 觸發（commit `97a47b3` + `b2f0a95`） |
| B-1 頻道清單 | `channels_view` | ✅ | 8 entries（commit `76d4d75`） |
| B-2 頻道編輯 | `channel_edit_view` | ✅ 部分 | 3 可寫欄（name / module_position_precision / muted）+ 3 唯讀顯示欄（PSK 摘要 / role / channel id），commit `76d4d75`。PSK / role 改為可寫、uplink / downlink 欄位仍待補 |
| B-3 加入頻道 | `channel_add_view` | ✅ | 從 channels_view OK 在空 slot 進入；name + role + 32 B random PSK，`AdminMessage.set_channel` (field 33) self-admin 發送，AdminModule 寫 channelFile（dev-Sblzm e530d61）|
| B-4 分享頻道 | `channel_share_view` + `util/channel_share_url` + `util/base64_url` | ✅ | URL `meshtastic.org/e/#<base64(ChannelSet)>` 文字 + `lv_qrcode` 144×144 顯示；OpenCV 端到端 decode 驗證通（dev-Sblzm 5a/5b commits）|
| C-1 節點清單 | `nodes_view` | ✅ | cascade `phoneapi_cache` 來源（refactor `d9ebf55`） |
| C-2 節點詳情 | `node_detail_view` | ✅ | 含 last traceroute + position reply 渲染（commit `d9ebf55` + `a65af97`） |
| C-3 節點操作 | `node_ops_view` | ✅ | 7 OPs：DM / ALIAS / FAVORITE / IGNORE / TRACEROUTE / REQUEST_POS / REMOTE_ADMIN（commit `d9ebf55`、`f9270b6`、`6b385eb`、`58f61f6`、`89786ef`） |
| C-3 sub OP_REMOTE_ADMIN | `remote_admin_view` | ✅ | 5 actions sub-menu：Reboot / Shutdown / FactoryReset(Cfg) / FactoryReset(Dev) / NodeDB Reset（T2.5，commit `89786ef`） |
| C-4 我的節點 | `my_node_view` | ✅ | commit `d9ebf55` |
| D-1 主地圖 | `map_view` | ✅ 部分 | 向量 PPI 雷達盤：3 距離環 + 本機 `+` + N 標 + peer 點（短名稱 + SNR 著色），LEFT/RIGHT 7 檔尺度（100 m..100 km），SET 切 layer mask（nodes / all / me-only），UP/DOWN 走訪 peer cursor、OK 鎖定進 D-6（dev-Sblzm） |
| D-2 圖層切換 | `map_view` 子模式 | ✅ 部分 | D-1 內 SET 鍵循環；航跡/航點未實作（ALL 暫等同 NODES） |
| D-3~D-5 航點 | ⏳ | 未實作 | 航點 CRUD + persist 待 LittleFS 整合 |
| D-6 航點導航 | `map_nav_view` | ✅ 部分 | 鎖定 peer 後顯示大方位字（8 方位 + 度數）+ 距離 + ETA + 速度；C-3 OP_NAVIGATE 入口；BACK 回 D-1（dev-Sblzm） |
| F-1 本機遙測 | `telemetry_view` (TELE_PAGE_F1) | ✅ | channel_util / air_util_tx 從 cascade DeviceMetrics 解碼（已存在）+ 自身 NodeInfo 取出顯示（dev-Sblzm 57bc816）|
| F-2 環境感測 | `telemetry_view` (TELE_PAGE_F2) | ✅ | 氣壓/三軸磁/各 sensor 溫度；Rev A 無濕度感測器 |
| F-3 鄰居資訊 | `telemetry_view` (TELE_PAGE_F3) | ✅ | NEIGHBORINFO_APP (PortNum 71) cascade decoder + `phoneapi_neighbors_t` per-node cache + Nbrs column；live broadcast 驗證受限於 Meshtastic 4hr min 間隔（dev-Sblzm 9b38f1b）|
| F-4 歷史曲線 | `telemetry_view` (TELE_PAGE_F4) + `metrics/history` | ✅ 部分 | 電量 + 訊號雙 chart（256 點 × 30 s = 2 hr 8 min 視窗），ring 在 SRAM `.bss` 1.5 KB；空中時間佔比 chart 留 placeholder 待 TELEMETRY_APP self-decode；persist 跨 boot 不在 v1（T2.6） |
| T-0 工具主選單 | `tools_view` | ✅ | spec-named 入口（commit `0ceb082`、`ee3a72e`） |
| T-1 Traceroute | `traceroute_view` | ✅ | commit `3fbf664` |
| T-2 Range Test | `range_test_view` + `messages/range_test_log` | ✅ | RANGE_TEST_APP (PortNum 66) cascade decoder + per-peer hit/seq/SNR/RSSI ring（cap 7）+ 模組狀態 header；live broadcast 驗證待 RF（dev-Sblzm 5ee4a07）|
| T-3 訊號頻譜 | ⏳ | 未實作 | SX1262 RSSI 掃描需 cascade decoder |
| T-4 封包嗅探 | ⏳ | 未實作 | |
| T-5 LoRa 自我測試 | ⏳ | 未實作 | |
| T-6 GNSS 衛星圖 | `gnss_sky_view` | ✅ | 仰角 / 方位 / SNR（commit `3fbf664`） |
| T-7 配對碼顯示 | ⏳ | 未實作 | |
| T-8 firmware info | `firmware_info_view` | ✅ | versions / hashes（commit `3fbf664`） |
| S-0 設定主頁 | `settings/settings_app_view` | ✅ | tree walker + breadcrumb（commit `ba55cf9`） |
| S-1~S-6, S-8~S-12 | `settings/settings_tree` + `settings/settings_keys` | ✅ 部分 | 19 groups / 115 keys（B3 + T2.4.2：Device / LoRa / Position / Power / Display / Channel / Owner / Security + Telemetry / Neighbor / RangeTest / DetectSnsr / CannedMsg / Ambient / Paxcounter / StoreForward / Serial / ExtNotif / RemoteHW），cascade `FR_TAG_MODULE_CONFIG` walk-down decoder + cache 已上（commit `9cf76a4`） |
| S-7 模組索引 | `modules_index_view` | ✅ | 10-page module sub-page index + deep-link（commit `a6c4a46`） |
| S-X 模板 A/B/C/D | `template_enum/number/text/toggle` | ✅ | enum (`828a253`)、toggle (`ba55cf9`)、number (`9f8b032`)、text (`62b3091`) |
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
| B-4 | 分享頻道 | URL + 144×144 QR 已實作（LVGL 內建 `lv_qrcode` / Nayuki qrcodegen-c）|

> **B-4 實作摘要（2026-04-30 dev-Sblzm）**：Pre-req `phoneapi_channel_t` 加 `psk[32]` 進 cache（decoder 從 skip-bytes 改成 memcpy；privacy 接受單機 SWD 可見）。Encoder 在 `firmware/core1/src/util/channel_share_url.{c,h}` —— 手寫 ChannelSet protobuf + URL-safe base64 (`base64_url.{c,h}`)。QR 渲染靠 LVGL `LV_USE_QRCODE=1` 拉進 vendored qrcodegen，`lv_qrcode_update()` 把 URL 編進 144×144 I1 (1 bpp) buffer。驗證：URL 結構與 `meshtastic --info` Primary URL 各欄位逐一比對 9/9 PASS；QR 透過 SWD dump I1 buffer → PNG → cv2.QRCodeDetector decode → byte-for-byte 等於 URL。

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

最後更新：2026-04-30
版本：v1.2（D-Map v1 / F-1 / F-3 / T-2 / B-3 / B-4 全部從 ⏳ 升 ✅；
Core 1 view 對照表同步 dev-Sblzm 5/5 phase commits + audit 抓出的
B-3 set_channel field tag 8→33 修正 + B-4 cv2 端到端 QR decode 驗證；
頁數 v1.1=62 維持不變，僅狀態欄位更新）

v1.1（2026-04-29）：移除 Audio/MQTT/Network/BT 模組；S 進階 S-12~S-15
順移；B-3 移除 QR；新增 Core 1 view 對照表；補各 App 實作落差註記；
頁數 67 → 62。
