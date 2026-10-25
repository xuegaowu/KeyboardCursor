# 多语言改造设计（v1）

目标：UI 支持多语言，先做 **简体中文 + English**，结构上为"以后加十几种"留好路。

## 0. 已定决策

| # | 问题 | 决定 |
|---|---|---|
| 1 | 切换语言是否立即生效 | **重启生效**。选完弹窗询问，同意则保存并自动重开，拒绝则语言不变 |
| 2 | 首次做几种语言 | 简体中文 + English |
| 3 | 语言包形式 | 先**内置**（编译进 exe）；外部文件（B 方案）后面单独讨论 |
| 4 | 布局 | **测宽自动排**，坐标不再写死 |
| 5 | 窗口尺寸 | 允许加宽；宽度按内容自动算 |
| 6 | 英文文案 | 紧凑写法 + 悬停提示（tooltip）解释含义 |
| 7 | 存档槽英文名 | `Saves` |
| 8 | 老存档 | **不兼容**，不迁移 |
| 9 | 数据目录 | `data\`（无前导点、不设隐藏属性） |

## 1. 目录结构

```
KeyboardCursor.exe
data\                     ← 所有外部文件都放这里
  config.ini              ← 程序级配置（语言等），不属于任何存档槽
  save\                   ← 存档槽
    save0.ini             ← "下次启动恢复的状态"
    save1.ini ...
  lang\                   ← 以后放语言包（B 方案），现在为空
```

对应改动：

| 位置 | 旧 | 新 |
|---|---|---|
| `GetSaveDir()` | `exe\save` | `exe\data\save` |
| `GetLocalSaveDir()` | `%LOCALAPPDATA%\KeyboardCursor\save` | `%LOCALAPPDATA%\KeyboardCursor\data\save` |
| 目录创建 | `CreateDirectoryA` 只能建一层 | 新写 `MakeDirs()` 逐级建，`data\save` 两层一次搞定 |

`data` **不加隐藏属性**：Windows 下以点开头的目录本来就不自动隐藏，用普通名字更好找。

**不迁移旧存档**：新版只认 `data\save\`，旧的 `exe\save\` 直接忽略（旧文件留在原地不删，用户自己处理）。

## 2. 语言怎么存、怎么切

### 2.1 存放位置

```ini
; data\config.ini
[UI]
language=zh-CN
```

放在 `config.ini` 而不是 `save0.ini`，原因：语言是**程序级 UI 设置**，不属于任何存档槽。否则加载 `save3.ini` 会把界面语言也一起换掉，很怪。

### 2.2 启动顺序（关键约束）

语言必须在**创建窗口之前**确定，因为控件标题在建窗时就要用对应语言的文字：

```
WinMain
  ├─ ResolveSaveDir()          决定 data\save 是否可写（失败→用户目录→只读）
  ├─ LoadLanguage()            读 data\config.ini，选定字符串表
  ├─ CreateWindowExA(...)      WM_CREATE 里用 T(...) 建控件 + 测宽排版
  └─ ShowWindow()
```

### 2.3 切换流程

```
用户在下拉框选了另一种语言
   │
   ├─ 和当前语言相同 → 什么都不做
   │
   └─ 不同 → MessageBox「切换语言需要重新启动程序才会生效。现在重启吗？」
        ├─ 否 → 下拉框选回当前语言，不写盘、不改内存，完全无副作用
        └─ 是 → 写 data\config.ini
                 → g_restartRequested = true
                 → PostMessage(WM_CLOSE)   走现有退出流程（有未保存修改会先问）
                      ├─ 用户在"是否保存"里选取消 → 清 g_restartRequested，留在窗口
                      └─ 正常退出 → WM_DESTROY 里先 Unhook + 停控制线程（现有逻辑）
                                   → CreateProcessA(自己的 exe)
                                   → 新实例启动，此时旧实例已卸钩，不会双重注入
```

**为什么在 WM_DESTROY 里启动新进程**：那时钩子已经卸掉、控制线程已经停了，新实例不会和旧实例同时注入鼠标键盘（否则会有几毫秒的双倍速度）。

## 3. 字符串表结构

```cpp
enum StrId {
    S_APP_TITLE, S_TITLE_ON, ...
    S_COUNT
};

// 每种语言一张表，下标就是 StrId。
// 用 ID 而不是"按中文原文查表"：同一个词在不同语境下英文不同
// （"开关"作分组名 vs "开关键"），按原文查迟早冲突。
static const char* const kZh[S_COUNT] = { ... };
static const char* const kEn[S_COUNT] = { ... };

static const char* const* g_lang = kZh;   // 当前语言表
inline const char* T(StrId id) { return g_lang[id]; }
```

外部文件（B 方案）以后接在 `LoadLanguage()` 里：读 `data\lang\xx.ini` 填进一张可写表，缺项回退到 `kZh`。**结构上现在就留好**，不实现。

## 4. 测宽布局

### 4.1 原则

- 所有 x 坐标由"前一个控件的实测宽度"推出，不再写死
- 标签**右对齐**到输入框左边（`SS_RIGHT`），这样中文短、德语长都整齐
- 同组内同类控件统一宽度（取该组里最长的那条文字），避免参差不齐

### 4.2 测量函数

```cpp
// 用控件当前的字体量文字宽度。必须用 DEFAULT_GUI_FONT 之类的实际字体，
// 不能用固定字宽估算 —— 中英混排、不同 DPI 都会算错。
int TextWidth(HWND hwnd, const char* s);
```

### 4.3 各区域算法

| 区域 | 算法 |
|---|---|
| 左栏标签 | `labelW = max(8 条标签的宽) + 8`；输入框 `x = sx + labelW + 6` |
| 右栏标签 | `labelW = max(11 条标签的宽) + 8` |
| 右栏按钮 | `btnW = max(76, TextWidth("Backspace") + 16)`，按键名最长可能是 Backspace |
| 复选框 | `w = TextWidth + 28`（勾选框占 20） |
| 单选按钮 | `w = TextWidth + 28` |
| 底部按钮 | `w = max(80, TextWidth + 24)`，从右边界往左排 |
| 状态行 | 通栏 `wideW - 24` |

### 4.4 窗口宽度自适应

建窗时先用一个临时尺寸，`WM_CREATE` 里排完版算出**内容需要的客户区宽度**，再：

```
需要的宽度 = max(colW*2 + 8 + 2*M, 顶行总宽, 底部总宽)
上限        = 屏幕工作区宽 * 0.9      // 防止德语把窗口撑出屏幕
调整窗口    = SetWindowPos（此时窗口还没 ShowWindow，不会闪）
```

这样中文 640、英文 700、德文 900 都能自动适配，不用每次改常量。

## 5. 完整字符串清单（75 条）

> 最终提取时可能拆/并几条，以代码为准。

### 5.1 窗口与托盘（6）

| ID | 中文 | English |
|---|---|---|
| S_APP_TITLE | 光标控制 | Keyboard Cursor |
| S_TITLE_ON | [启用] | [On] |
| S_TITLE_OFF | [禁用] | [Off] |
| S_TRAY_SHOW | 显示窗口 | Show window |
| S_TRAY_EXIT | 退出 | Exit |
| S_ERR_HOOK | 键盘钩子安装失败！ | Failed to install the keyboard hook. |

### 5.2 顶部开关组（3 + 语言下拉）

| ID | 中文 | English |
|---|---|---|
| S_GRP_SWITCH | 开关 | Switches |
| S_CHK_INTERCEPT | 启用按键拦截 | Intercept keys |
| S_CHK_TRAY | 关闭时最小化到托盘 | Minimize to tray |
| S_CHK_AUTOSTART | 开机自启动 | Auto start |

语言下拉项本身**不翻译**（各语言用母语显示自己）：
`CN 简体中文` / `EN English`

### 5.3 速度组（9）

| ID | 中文 | English | tooltip（中 / 英） |
|---|---|---|---|
| S_GRP_SPEED | 速度 | Speed | — |
| S_LBL_SPEED | 初始速度: | Start speed: | 按下方向键第一拍移动的像素数 / Pixels moved on the first tick |
| S_LBL_ACCEL | 加速度: | Acceleration: | 每拍增加的速度，0 = 不加速 / Speed added per tick, 0 = no acceleration |
| S_LBL_MAXSPEED | 速度上限: | Max speed: | 加速到此为止；小于初速时改作下限 / Acceleration stops here; below start speed it becomes the floor |
| S_LBL_MUL | 加速倍率: | Boost factor: | 按住加速键时速度乘这个值，可小于 1 / Multiplier while the boost key is held |
| S_LBL_WIVL | 滚轮间隔(ms): | Wheel interval(ms): | 两次滚轮事件的最小间隔 / Minimum gap between wheel events |
| S_LBL_WSPD | 滚轮速度: | Wheel speed: | 一次滚动走几行 / Lines per wheel event |
| S_LBL_WMUL | 滚轮加速: | Wheel boost: | 按住滚轮加速键时乘这个值，上限 3000 行 / Multiplier while the wheel-boost key is held, capped at 3000 |
| S_LBL_TICK | 刷新间隔(ms): | Refresh(ms): | 控制线程多久醒一次；速度单位是"每拍" / How often the control loop wakes; speed is per tick |

### 5.4 按键映射组（13）

| ID | 中文 | English |
|---|---|---|
| S_GRP_KEYS | 按键映射 | Key mapping |
| S_LBL_UP | 上: | Up: |
| S_LBL_DOWN | 下: | Down: |
| S_LBL_LEFT | 左: | Left: |
| S_LBL_RIGHT | 右: | Right: |
| S_LBL_LCLICK | 左键: | L-Click: |
| S_LBL_RCLICK | 右键: | R-Click: |
| S_LBL_MCLICK | 中键: | M-Click: |
| S_LBL_WUP | 上滚: | Wheel up: |
| S_LBL_WDOWN | 下滚: | Wheel down: |
| S_LBL_BOOST | 加速: | Boost: |
| S_LBL_WBOOST | 滚轮加速: | Wheel boost: |
| S_NONE | 无 | (none) |
| S_WAITING_KEY | 按任意键... | Press a key... |

### 5.5 组合键组（6）

| ID | 中文 | English |
|---|---|---|
| S_GRP_COMBO | 开关组合键（1~4 个键同时按下） | Combo key (hold 1-4 keys) |
| S_CLEAR_ALL | 清空全部 | Clear all |
| S_RADIO_TOGGLE | 按下切换（按一下开、再按一下关） | Toggle (press to switch) |
| S_RADIO_HOLD | 按住生效（按下开、松开关） | Hold (press = on, release = off) |
| S_HOTKEY_HINT | 重置快捷键：Esc + 0 | Reset hotkey: Esc + 0 |

### 5.6 存档组（6）

| ID | 中文 | English |
|---|---|---|
| S_GRP_SAVE | 存档 | Saves |
| S_BTN_SAVE | 保存 | Save |
| S_BTN_LOAD | 加载 | Load |
| S_BTN_APPLY | 应用 | Apply |
| S_BTN_RESET | 重置 | Reset |
| S_BTN_EXIT | 退出程序 | Exit |

（下拉框里的 `+` 和 `save1..N` 是文件名，不翻译。）

### 5.7 状态行（13）

| ID | 中文 | English |
|---|---|---|
| S_ST_LOAD_FAILED_BAK | 存档读取失败，已使用默认值；原文件将在下次保存前备份为 .bak。 | Failed to read saves; defaults loaded. The old file will be backed up as .bak. |
| S_ST_READONLY | 只读模式：程序目录不可写且未启用回退，设置不会保存。 | Read-only: the program folder is not writable and fallback is disabled. |
| S_ST_DIR_FALLBACK | 存档目录（程序目录不可写，已改用用户目录）： | Save folder (program folder not writable, using user folder): |
| S_ST_DIR | 存档目录： | Save folder: |
| S_ST_APPLIED_RO | 已应用（只读模式，未写入磁盘） | Applied (read-only, not written to disk) |
| S_ST_APPLIED_SAVE0 | 已应用，并写入默认存档 save0 | Applied and written to save0 |
| S_ST_APPLIED_FAIL | 已应用，但写入 save0 失败 | Applied, but writing save0 failed |
| S_ST_SAVED | 已保存： | Saved: |
| S_ST_SAVE_FAIL | 保存失败：无法写入存档目录。 | Save failed: the save folder is not writable. |
| S_ST_LOADED | 已加载： | Loaded: |
| S_ST_LOADED_FAIL_SAVE0 | 已加载：（但写入 save0 失败） | Loaded: (but writing save0 failed) |
| S_ST_NEW_SAVE | 已新建存档： | New save created: |
| S_ST_RESET | 已重置为出厂默认并立即生效（未保存） | Reset to defaults and applied (not saved) |

### 5.8 消息框（14 文案 + 5 标题）

| ID | 中文 | English |
|---|---|---|
| S_MB_DIR_PROMPT | 程序所在目录不可写…是否改为保存到： | The program folder is not writable. Save to: |
| S_MB_DIR_PROMPT_TAIL | 选择“否”将以只读模式继续运行… | Choosing No runs in read-only mode… |
| S_MB_AUTOSTART_FAIL | 无法修改开机自启动设置（注册表写入失败）。 | Failed to change auto-start (registry write failed). |
| S_MB_READONLY_SAVE | 当前为只读模式，设置无法保存。 | Read-only mode: settings cannot be saved. |
| S_MB_SAVE_FAIL | 保存失败：无法写入存档目录。 | Save failed: the save folder is not writable. |
| S_MB_NO_SAVE_FILE | 存档文件不存在。 | The save file does not exist. |
| S_MB_LOAD_FAIL | 存档读取失败，已使用默认值。 | Failed to read the save; defaults loaded. |
| S_MB_READONLY_NEW | 只读模式下无法新建存档。 | Cannot create a save in read-only mode. |
| S_MB_SAVE_FULL | 存档编号已用尽（save1~save999）。 | No free save slot (save1-save999). |
| S_MB_NEW_FAIL | 新建存档失败：无法写入存档目录。 | Failed to create the save: the folder is not writable. |
| S_MB_KEY_CONFLICT | 该键已被另一种用途占用，请换一个键。 | This key is used by another function. Pick another one. |
| S_MB_QUIT_SAVE | 设置已修改但还没保存，退出前保存吗？ | Settings changed but not saved. Save before exiting? |
| S_MB_QUIT_SAVE_FAIL | 保存失败，已取消退出。请检查存档目录后重试。 | Save failed; exit cancelled. Check the save folder. |
| S_MB_RESTART | 切换语言需要重新启动程序才会生效。现在重启吗？ | The language change takes effect after a restart. Restart now? |
| S_TIP_TITLE | 提示 | Info |
| S_ERR_TITLE | 错误 | Error |
| S_CONFLICT_TITLE | 按键冲突 | Key conflict |
| S_QUIT_TITLE | 退出 | Exit |
| S_DIR_TITLE | 存档目录 | Save folder |
| S_LANG_TITLE | 语言 | Language |

### 5.9 键名（4 + 1）

`S_KEY_LEFT / S_KEY_RIGHT / S_KEY_UP / S_KEY_DOWN`（左方向 / Left …）、`S_NONE` 复用。
`Space` `Enter` `Tab` `Esc` `Backspace` `PageUp` 等本来就是英文，保持不变。

## 6. 改动清单

| # | 内容 | 位置 | 规模 |
|---|---|---|---|
| 1 | `StrId` + 两张表 + `T()` | 文件头 | ~110 行 |
| 2 | 75 处字面量替换 | 全文 | 机械替换 |
| 3 | `LoadLanguage()` + `config.ini` 读写 | 存档模块旁 | ~40 行 |
| 4 | 目录改 `data\`（含 `SHCreateDirectoryExA`、回退路径） | `GetSaveDir` 一带 | ~20 行 |
| 5 | `TextWidth()` + 各区域测宽排版 | `WM_CREATE` | **~220 行**（主体） |
| 6 | 窗口宽度自适应 | `WM_CREATE` + `WinMain` | ~30 行 |
| 7 | 语言下拉框 + 切换/重启流程 | `WM_CREATE` + `WM_COMMAND` | ~70 行 |
| 8 | tooltip（8 个数值框） | `WM_CREATE` + 语言切换 | ~50 行 |
| 9 | 重启：`g_restartRequested` + `CreateProcess` | `WM_DESTROY` | ~25 行 |

合计约 **560 行**，`KeyboardCursor.cpp` 从 2810 涨到约 3350 行。

## 7. 风险

| 风险 | 说明 | 对策 |
|---|---|---|
| GBK 装不下的字符 | `-fexec-charset=GBK`，德语 `ä ö ü ß`、俄语西里尔在 GBK 里**有**（区 0xA1~ 无拼音符号区），但西班牙语 `ñ`、波兰语 `ł` **没有** → 编译直接报错 | 内置表阶段只做中英；等 B 方案（外部 UTF-8 + 运行时转码）再扩 |
| 德语后窗口过宽 | 测宽虽自适应，但 900px 窗口难看 | 上限设屏幕工作区 90%，超出则标签省略号 `SS_ENDELLIPSIS` |
| tooltip 多语言 | 语言重启才变，切换语言后 tooltip 自动是新语言，无需额外处理 | 建窗时按当前语言填一次 |
| 键名超长 | 用户把键设成 `Backspace` | 按钮宽度按最长键名预留 |
| 重启失败 | `CreateProcess` 失败（文件被占用等） | 弹窗提示"请手动重新打开"，然后正常退出 |

## 8. 明确不做

- 不做运行时热切换（重启生效）
- 不做旧存档迁移
- 不做外部语言包（B 方案，另议）
- 不改数字输入的 `.` 规则（翻译不涉及小数点）
- 不翻译存档文件名、路径、ini 键名
