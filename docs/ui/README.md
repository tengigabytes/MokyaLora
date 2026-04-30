# MokyaLora UI/UX 規格文件

本資料夾收錄 MokyaLora 韌體的 UI/UX 設計規格。所有頁面、元件、互動行為都以此為實作依歸。

## 設計目標

MokyaLora 是一台**單機獨立操作完整 Meshtastic 所有功能**的 LoRa 手持機。UI 設計核心約束：

- **顯示**：2.4" TFT，320×240，全 Unifont 16×16 點陣字
- **輸入**：5-way D-pad（▲▼◀▶OK）+ 5×5 注音半鍵盤 + 功能鍵群（FUNC、BACK、SET、DEL、MODE、TAB、SPACE 等）+ Power + 音量鍵
- **無觸控**
- **核心需求**：本機可完整操作 Meshtastic 全部功能與設定，包含遠端管理其他節點
- **語系**：嚴格使用臺灣正體中文 + 英文

## 設計憲章

| 原則 | 內容 |
|---|---|
| 焦點唯一 | 全系統任一時刻僅有一個橙色焦點框 |
| 三層上限 | Launcher 桌面 → App → Detail/Modal；設定樹例外（用麵包屑+折疊壓縮深度感） |
| 模式即 App | Meshtastic 功能群以九個獨立 App 承載，避免單一 App 內 Tab 爆炸 |
| 文字輸入是子模式 | D-pad 在 IME 中改變語意，由 IME 接管直到 BACK 退出 |
| 全域反射 | Status Bar 永駐，Hint Bar 僅在子模式顯示 |
| 對齊 Meshtastic | 設定項命名與層級對齊官方 protobuf |

## 文件結構

```
docs/ui/
├── README.md                          # 本檔，總索引
├── 00-design-charter.md               # 設計憲章與全域規則
├── 01-page-architecture.md            # 62 頁面完整架構 + 實作狀態追蹤
├── 10-status-bar.md                   # G-1 Status Bar（全域元件）
├── 12-ime.md                          # G-3 IME 套皮（兩種編輯模式）
├── 20-launcher-home.md                # L-0 桌面/Home
└── 50-settings-leaf-templates.md      # S-X 設定葉節點四模板
```

## 文件 vs 實作 兩軸進度

本資料夾追蹤兩條彼此獨立的進度線：
- **詳細設計文件**（本表 ✅ / ⏳）：是否寫了專屬的線框圖 + 元件規格 + 鍵位 + LVGL 結構文件
- **實作進度**：是否已落地到 Core 1 firmware（在 `01-page-architecture.md` 表格內逐頁追）

絕大多數頁面是先在 `01-page-architecture.md` 寫一行描述就直接實作，**沒回填獨立詳細 spec**。

### 詳細設計文件進度（本資料夾）

| 編號 | 名稱 | 設計 spec | 實作 |
|---|---|---|---|
| - | 設計憲章 (00) | ✅ v1.0 | n/a |
| - | 62 頁架構 (01) | ✅ v1.5 持續更新 | n/a |
| L-0 | 桌面 (20) | ✅ v4 | ✅ |
| G-1 | Status Bar (10) | ✅ v2 | ✅ |
| G-3 | IME 套皮 (12) | ✅ v2.0（模式 B：SET 送出 / BACK 存草稿 / OK 換行 已收斂）| ✅ |
| S-X | 設定葉節點四模板 (50) | ✅ v1.0 | ✅ |
| L-1 | 九宮格功能表 | ⏳ 詳細 spec 待寫 | ✅ |
| A-1 / A-2 | 對話列表 / 詳情 | ⏳ 詳細 spec 待寫 | ✅ |
| A-3 / A-4、B-*、C-*、D-1/2/6、F-1~F-4、T-0~T-8、S-0/S-7、S-7.1~S-7.10 | 其餘 ~50 個已實作頁面 | ⏳ 詳細 spec 待寫 | ✅ |
| D-3~D-5 / Z-1~Z-3 | 航點 / SOS | ⏳ 詳細 spec 待寫 | ⏳ 未實作 |

**摘要**：詳細 spec 6 / 62 頁；實作 48 ✅ / 2 ✅ 部分 / 6 ⏳ / 62 總頁（77% 完整、3% 部分、10% 未實作；其餘為全域元件與模組子頁不算入單頁）。L1 sweep 後（2026-05-01）。詳細實作狀態見 `01-page-architecture.md`。

## 全域配色（深色主題）

| Token | Hex | 用途 |
|---|---|---|
| `bg_primary` | `#0B0F14` | 主背景 |
| `bg_secondary` | `#161C24` | 次背景（卡片、文字框內、內容區框） |
| `bg_preedit` | `#2A2018` | Preedit 背景塊（深橙低飽和） |
| `text_primary` | `#E6EDF3` | 主文字 |
| `text_secondary` | `#7D8590` | 次文字（提示、單位、麵包屑） |
| `text_preedit` | `#FFA657` | Preedit 文字 |
| `accent_focus` | `#FFA657` | 焦點橙 / 候選字選中 / 游標 / IME 模式 |
| `accent_success` | `#39D353` | 主色綠（成功、未讀計數、3D Fix、TX 完成） |
| `border_focus` | `#FFA657` | 焦點態邊框 |
| `border_normal` | `#30363D` | 一般邊框、不動作灰 |
| `warn_yellow` | `#F1E05A` | 注意、2D Fix、警告燈、字數接近上限 |
| `warn_red` | `#F85149` | 嚴重、無 GPS、低電、SOS、字數超限 |
| `alert_bg_critical` | `#8B1A1A` | SOS 警告背景 |
| `alert_bg_warning` | `#6E1A1A` | 極低電警告背景 |

### 配色記憶

| 視覺 | 含義 |
|---|---|
| 主色白 | 預設、正常、Op 態 |
| 綠 | 良好、TX 完成、未讀計數、3D Fix |
| 黃 | 注意、有警告、2D Fix、電量警告 |
| 紅 | 嚴重、無 GPS、低電、SOS |
| 橙 | TX 動作、IME 輸入態、焦點（D-pad 行為已變） |
| 灰 | 不動作、停用、無資料 |

## 字型策略

**全程使用 Unifont 16×16 點陣字**：

- CJK：16×16 點陣
- 西文：8×16 點陣（半形）
- 行高：16px 字 + 上下 2-3px padding（總行高 20-22px）

選擇 Unifont 的理由：
1. 涵蓋全 Unicode BMP（不會缺字）
2. 點陣字實機可讀性穩定（無縮放糊化）
3. 字型管理單純（單一字型檔，BIG5 子集約 460KB flash）
4. 開源（GPL/OFL）

## 全域實體鍵位

依 PCB 設計實際佈局（**邏輯分群**；底層為 6×6 掃描矩陣，見 `firmware/core1/src/keypad/keymap_matrix.h` 與 `00-design-charter.md` §鍵位全域慣例）：

```
功能鍵群（左上）：    FUNC、BACK
D-pad（中央）：       ▲▼◀▶ OK
功能鍵群（右上）：    SET、DEL
音量鍵（最右）：      VOL+、VOL-（⏳ 用途待定，無揚聲器）
主鍵盤 R0-R3（5×4）： 20 個多功能鍵（注音/英文/數字 multitap）
功能鍵 R4：          MODE、TAB、SPACE、`，SYM`、`。.?`
電源：               Power（獨立 GPIO；driver ⏳ 未實作）
```

## 鍵位語意對照（全域慣例）

| 鍵 | Op 態（焦點導航） | IME 態（文字輸入） |
|---|---|---|
| D-pad ▲▼◀▶ | 焦點移動 / 滾動 | 候選字翻頁/游標 / 無候選時游標 |
| OK | 進入 / 確認 | 模式 A：確認 / 模式 B：候選字 commit / multitap commit / idle 換行 |
| BACK | 上一層 / 取消 | 清 Preedit / 退出 / 模式 B 存草稿 |
| BACK 長按 | 鎖屏（⏳ 需 UI 層 long-press timer） | 同 |
| FUNC 短按 | 桌面：呼出九宮格 / 其他：定義 | 無作用 |
| FUNC 長按 ≥2s | Status Bar 詳情面板 | Status Bar 詳情面板 |
| SET | 進當前 App 子設定 | 模式 A：（不在 IME 內，由 App 自處）/ 模式 B：**送出 / 套用**（v2.0 收斂）|
| MODE | （定義中） | 切 IME（注→EN→Ab，三模式循環） |
| MODE 長按 | （定義中） | ⏳ CapsLock 未實作（MIE 無 case-lock） |
| TAB | 焦點群組切換 | 焦點群組切換 |
| SPACE | 焦點群組切換 / 滾動 | 注：一聲/空白；EN：commit+空格；Ab：空格 |
| DEL | （無對應） | 刪 Preedit / 刪前字 |
| Power 短按 | 螢幕關（⏳ Power button driver 未實作） | 同 |
| Power 長按 5s | 啟動 SOS（⏳ 同上 + IPC_CMD_SEND_SOS 未定義） | 同 |
| VOL+ / VOL- | ⏳ 待重新定義（無揚聲器） | 同 |

## 規劃方法論

每份規格依以下結構撰寫：

1. **角色定位**：這個元件/頁面解決什麼問題
2. **線框圖**：ASCII art 繪示版面
3. **元素規格**：每個元素的字級、顏色、行為
4. **互動行為**：完整鍵位對照表
5. **設定 App 對應**：相關設定項
6. **實作備忘**：LVGL 結構與 API 雛形

## 參考資源

- [Meshtastic 官方文件](https://meshtastic.org/docs/)
- [Meshtastic protobuf 定義](https://github.com/meshtastic/protobufs)
- [GNU Unifont](https://unifoundry.com/unifont/)
- [LVGL v9 文件](https://docs.lvgl.io/master/)

---

最後更新：2026-04-30
規劃中文件代號：`vN.0`（v1.0 = 第一次定稿，v2.0 = 第一次重大修訂）

**2026-04-30 修訂**：
- 規劃進度表改寫成「設計 spec / 實作」兩軸（先前混在一起易誤判）
- 文件結構樹「67 頁面」→「62 頁面」（v1.1 移除 Audio/MQTT/Network/BT 後沒同步）
- G-3 IME 收斂為 v2.0：模式 B 送出鍵改為 SET，OK 保留為換行（與舊規劃 OK 長按送出方案相反）
