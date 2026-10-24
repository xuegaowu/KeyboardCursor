## 编译

```powershell
g++ -std=c++11 -O2 -s -mwindows -static -pthread -fexec-charset=GBK KeyboardCursor.cpp -o KeyboardCursor.exe -lwinmm -lgdi32 -luser32 -lshell32
```

# 这个介绍过期了


# 🖱️ 光标控制程序 / Cursor Control Program

一个轻量的键盘驱动鼠标控制工具：绑定按键即可移动光标、点击和滚动滚轮，并通过组合键一键启用/禁用控制。

A lightweight keyboard-driven mouse controller. Bind keys to move the cursor, click, and scroll — then toggle control on and off with a hotkey.

---

## 📌 1. 启动程序 / Start the Program

编译并运行可执行文件。首次启动时，程序会自动创建 `save/` 文件夹（若不存在），并加载默认配置文件 `save0.ini`。

Compile and run the executable. On first launch, the program automatically creates a `save/` folder (if missing) and loads the default configuration file `save0.ini`.

---

## 📌 2. 按键绑定方法 / How to Bind Keys

-  点击要绑定的按键按钮（如 **上**、**左键**、**滚轮上移**）。
-  系统提示 **"按任意键..."**，此时按下任意键（如 `E`）。
-  绑定的按键会显示在按钮上，即可使用。

-  Click the key button you want to bind (e.g., **Up**, **Left Click**, **Scroll Up**).
-  The system prompts **"Press any key..."** — press any key (e.g., `E`).
-  The bound key is now displayed on the button and ready to use.

---

## 📌 3. 启用 / 禁用光标控制 / Enable & Disable Cursor Control

按 **组合键1（默认 `Ctrl`）** + **组合键2（默认 `Shift`）** 切换光标控制的启用状态。窗口标题显示当前状态：`[启用]` / `[禁用]`。

Press **Key 1 (default `Ctrl`)** + **Key 2 (default `Shift`)** to toggle cursor control. The window title shows the current state: `[Enable]` / `[Disable]`.

---

## 📌 4. 鼠标控制方式 / How to Control the Mouse

| 按键 | 功能 |
|------|------|
| `E` | 光标上移 |
| `D` | 光标下移 |
| `S` | 光标左移 |
| `F` | 光标右移 |
| `J` | 左键按下 |
| `K` | 右键按下 |
| `M` | 中键按下 |
| `O` | 滚轮上移 |
| `L` | 滚轮下移 |

| Key | Action |
|-----|--------|
| `E` | Move cursor up |
| `D` | Move cursor down |
| `S` | Move cursor left |
| `F` | Move cursor right |
| `J` | Press left mouse button |
| `K` | Press right mouse button |
| `M` | Press middle mouse button |
| `O` | Scroll wheel up |
| `L` | Scroll wheel down |

> 上表为默认按键，任意按键均可重新绑定（见 §2）。 / The keys above are defaults; any key can be rebound (see §2).

---

## 📌 5. 滚轮设置 / Wheel Settings

| 设置项 | 控件 ID | 默认值 |
|--------|---------|--------|
| 滚轮间隔 | `ID_EDIT_WHEEL_INTERVAL` | 80 ms |
| 滚轮速度 | `ID_EDIT_WHEEL_SPEED` | 1 |
| 滚轮加速倍率 | `WHEEL_SPEED_MULTIPLIER`（配合 `Shift`） | 3 |

- **滚轮方向按键**：设为 `O` / `L`（上 / 下）。
- **滚轮间隔**：每次滚动之间的延迟（默认 `80 ms`）。
- **滚轮速度**：每次按键的滚动步数（默认 `1`）。
- **滚轮加速**：按住 `Shift` 时，滚动速度乘以 `WHEEL_SPEED_MULTIPLIER`（默认 `3`）。

| Setting | Item ID | Default |
|---------|---------|---------|
| Scroll interval | `ID_EDIT_WHEEL_INTERVAL` | 80 ms |
| Scroll speed | `ID_EDIT_WHEEL_SPEED` | 1 |
| Scroll speed multiplier | `WHEEL_SPEED_MULTIPLIER` (with `Shift`) | 3 |

- **Scroll keys**: assign `O` / `L` to scroll up / down.
- **Scroll interval**: delay between scroll steps (default `80 ms`).
- **Scroll speed**: number of scroll steps per key press (default `1`).
- **Scroll acceleration**: hold `Shift` to multiply scroll speed by `WHEEL_SPEED_MULTIPLIER` (default `3`).

---

## 📌 6. 保存 / 加载设置 / Save & Load Settings

- **保存按钮** — 将当前配置保存为 `saveX.ini`（如 `save1.ini`）。若选择 `+`，程序会自动创建新的存档文件。
- **加载按钮** — 从下拉列表中选择存档，读取配置并应用。

- **Save button** — Saves the current configuration to `saveX.ini` (e.g., `save1.ini`). Selecting `+` creates a new save file automatically.
- **Load button** — Select an entry from the dropdown list to load and apply that configuration.

---

## 📌 7. 重置设置 / Reset All Settings

点击 **重置** 按钮，**或** 按 `Esc` + `0`（仅当程序窗口处于前台时）。所有按键将恢复为默认值（如 `E`、`J`、`Ctrl`、`Shift` 等）。

Click the **Reset** button, **or** press `Esc` + `0` (only when the window is in the foreground). All keybindings revert to their default values (e.g., `E`, `J`, `Ctrl`, `Shift`, etc.).

---

## 📌 8. 最小化到托盘（可选）/ Minimize to Tray (Optional)

勾选 **最小化到托盘** 选项。关闭程序窗口时，程序会隐藏到系统托盘而不退出。

> ⚠️ **注意：** 目前不支持托盘交互 —— 隐藏后尚无法从托盘还原窗口。此为已知限制，后续版本可扩展。

Check the **Minimize to Tray** option. When you close the window, the program hides in the system tray instead of exiting.

> ⚠️ **Note:** Tray interaction is not currently supported — the window cannot yet be restored from the tray. This is a known limitation and can be extended in future versions.

---

## 📌 9. 快速总结 / Quick Summary

- 运行程序。
- 点击某个按键按钮（如 **上**、**左键**、**滚轮上移**），然后按任意键（如 `E`）完成绑定。
- 按 `Ctrl` + `Shift` 启用 / 禁用光标控制。
- 按对应按键即可移动鼠标、点击或滚动。

- Run the program.
- Click a key button (e.g., **Up**, **Left Click**, **Scroll Up**), then press any key (e.g., `E`).
- Press `Ctrl` + `Shift` to enable / disable cursor control.
- Press the corresponding keys to move the mouse, click, or scroll.
