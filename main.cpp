// ============================================================
// 光标控制 - 键盘转鼠标映射工具
// ------------------------------------------------------------
// 核心思路：
//   1. 用 WH_KEYBOARD_LL 低级键盘钩子捕获按键，同时"拦截"映射键，
//      使其不再透传给前台窗口（游戏/软件收不到）。
//   2. 钩子回调只把按键"状态"写入 std::atomic 变量（不阻塞、不加锁）。
//   3. 独立线程按固定节拍轮询这些原子变量，做决策并调用 SendInput/
//      SetCursorPos 驱动鼠标。
//   4. 用组合键（1~4 个键同时按下）作为"总开关"，边沿触发切换启用/禁用。
//   5. 存档体系：save1..N.ini 是用户可选的存档槽；save0.ini 是"下次启动要恢复
//      的状态"。保存、应用、加载都会更新 save0，启动时读它。
//   6. 存档目录不可写时不再静默失败：可回退到用户目录，或降级为只读模式，
//      并在窗口底部状态行如实显示当前状态。
//   7. 草稿模型：界面上所有改动先进草稿，点"应用"才推进内存真正生效，
//      点"保存"才落盘；退出时若有未保存的修改会询问是/否/取消。
// ============================================================

// 包含 Windows API 头文件
#include <windows.h>
// 线程库（用于独立线程监听键盘）
#include <thread>
// 原子操作库（线程安全标志）
#include <atomic>
// 时间库（用于控制循环间隔）
#include <chrono>
// 字符串库
#include <string>
// 向量库（存档文件列表）
#include <vector>
// 算法库（排序）
#include <algorithm>
// 标准 C 库：sprintf_s / strcpy_s / atoi / strcmp / getenv 都声明在这里。
// 之前靠 <windows.h> 等头文件的间接包含，换编译器或换包含顺序就会报错。
#include <cstdio>
#include <cstdlib>
#include <cstring>
// 数学库：fabs 用来给速度上限做绝对值比较
#include <cmath>
// 多媒体库：仅用于老系统兜底时的 timeBeginPeriod(1)。
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
// 注册表 API（RegOpenKeyExA / RegSetValueExA）在 advapi32 里。
// MSVC 需要这行；MinGW 默认就链接 advapi32，写了也会被忽略。
#pragma comment(lib, "advapi32.lib")

// ==================== 编译器兼容层 ====================
// MSVC 的 _s 安全函数在 MinGW/GCC 下没有对应声明。这里用宏把调用映射到标准 C 函数，
// 让同一份源码在两种编译器下都能编过。
// 所有调用点的缓冲区都是数组，所以 sizeof 能正确取到容量。
#ifdef __GNUC__
#define sprintf_s(buf, ...) snprintf((buf), sizeof(buf), __VA_ARGS__)
#define strcpy_s(dst, src)  lstrcpynA((dst), (src), (int)sizeof(dst))
#endif

using namespace std::chrono;

// 控制线程的刷新间隔（毫秒），界面上可改（"刷新间隔(ms)"）。
// 取值原则：略高于目标显示器刷新周期。6ms ≈ 167Hz，覆盖 144/165Hz。
// 注意：MOVE_SPEED 的单位是"每 tick 像素数"，改这个值会等比改变手感。
constexpr int DEFAULT_TICK_MS = 6;   // 出厂默认，草稿和 ini 缺省值共用这一处
std::atomic<int> TICK_MS(DEFAULT_TICK_MS);

// ==================== 数值输入范围 ====================
// 八项数值的合法范围只在这里定义一次。
// 以前"界面校验"（ReadEditsIntoDraft）和"读档钳制"（LoadFromFile）各写一份
// 硬编码数字，改一处忘另一处就会出现"应用时接受了、重启后却被截断"的假象。
// 两处语义仍然不同（界面是回退到上一个有效值，读档是截断到边界），
// 但边界本身共用这一份。
namespace Limit {
    // 速度类四项：单位是 px/拍，允许小数和负值（负 = 反向）
    constexpr double SPEED_LO = -1000.0, SPEED_HI = 1000.0;   // 初始速度
    constexpr double ACCEL_LO =  -500.0, ACCEL_HI =  500.0;   // 加速度
    constexpr double MAXSP_LO =     0.0, MAXSP_HI = 2000.0;   // 速度上限（大小，不接受负值）
    constexpr double MUL_LO   = -1000.0, MUL_HI   = 1000.0;   // 加速倍率

    // 滚轮与节拍三项：整数
    constexpr int WIVL_LO = 1, WIVL_HI = 10000;   // 滚轮间隔（ms）
    constexpr int WSPD_LO = 1, WSPD_HI =   260;   // 滚轮速度（档/次）
    constexpr int WMUL_LO = 1, WMUL_HI =   260;   // 滚轮加速（倍）
    constexpr int TICK_LO = 1, TICK_HI =   300;   // 刷新间隔（ms）
}

// ==================== 控件 ID ====================
// 每个子控件在窗口内的唯一标识。数字随意但须保证不重复，
// 因为 WM_COMMAND 通过 LOWORD(wParam) 判断是哪个控件发出的通知。
#define ID_EDIT_SPEED           3001   // 初始速度
#define ID_EDIT_SPEED_MUL       3002   // 加速倍率
#define ID_EDIT_WHEEL_INTERVAL  3003   // 滚轮间隔
#define ID_EDIT_WHEEL_SPEED     3004   // 滚轮速度
#define ID_EDIT_WHEEL_MUL       3005   // 滚轮加速
#define ID_EDIT_TICK            3006   // 刷新间隔（毫秒）
#define ID_EDIT_ACCEL           3007   // 加速度（px/tick²）
#define ID_EDIT_MAXSPEED        3008   // 速度上限（px/tick）
#define ID_BTN_KEY_UP           3011   // 上按键
#define ID_BTN_KEY_DOWN         3012   // 下按键
#define ID_BTN_KEY_LEFT         3013   // 左按键
#define ID_BTN_KEY_RIGHT        3014   // 右按键
#define ID_BTN_KEY_LCLICK       3015   // 左键
#define ID_BTN_KEY_RCLICK       3016   // 右键
#define ID_BTN_KEY_MCLICK       3017   // 中键
#define ID_BTN_KEY_WHEELUP      3018   // 上滚
#define ID_BTN_KEY_WHEELDOWN    3019   // 下滚
#define ID_BTN_KEY_BOOST        3028   // 加速键（按住时乘加速倍率）
#define ID_BTN_KEY_WHEELBOOST   3029   // 滚轮加速键（按住时滚轮乘滚轮加速）
#define ID_BTN_SWITCH1          3021   // 开关键1
#define ID_BTN_SWITCH2          3022   // 开关键2
#define ID_BTN_SWITCH3          3023   // 开关键3
#define ID_BTN_SWITCH4          3024   // 开关键4
#define ID_BTN_CLEAR_SWITCH     3025   // 清空开关键
#define ID_RADIO_SWITCH_TOGGLE  3026   // 组合键模式：按下切换
#define ID_RADIO_SWITCH_HOLD    3027   // 组合键模式：按住生效
#define ID_CHECK_INTERCEPT      3031   // 启用按键拦截
#define ID_CHECK_TRAY           3032   // 最小化到托盘
#define ID_CHECK_AUTOSTART      3033   // 开机自启动（写 HKCU 的 Run 键，不需要管理员）
#define ID_BTN_APPLY            3041   // 应用
#define ID_BTN_RESET            3042   // 重置
#define ID_BTN_EXIT             3043   // 退出程序（无视"最小化到托盘"）

// 存档控件 ID
#define ID_COMBO_SAVE           3061   // 存档下拉框
#define ID_BTN_SAVE             3062   // 保存
#define ID_BTN_LOAD             3063   // 加载
#define ID_STATIC_STATUS        3051   // 底部状态提示（存档目录 / 只读 / 读取失败）

// 自定义消息
// WM_APP 是 Windows 留给"应用程序私用消息"的起点，
// 从这里往上加偏移即可避免与系统消息冲突。
#define WM_RESET (WM_APP + 2)          // 重置通知
#define WM_NEW_SAVE (WM_APP + 3)       // 新建存档通知
#define WM_BIND_KEY (WM_APP + 4)       // 按键捕获通知

// ==================== 可调变量 ====================
// 这些值既被 UI 线程修改，也被控制线程读取，因此用 atomic。
// （本例中都是 int，读写本身是原子的，但使用 atomic 可以保证
//   修改立刻对其它线程可见，且不对齐问题被规避。）
std::atomic<double> MOVE_SPEED(1.5);      // 初始速度：起步时每帧移动的像素数（允许小数）

// 加速参数：按住方向键时速度从 MOVE_SPEED 起步，每个 tick 增加 MOVE_ACCEL，
// 涨到 MOVE_MAX_SPEED 封顶；松开或反向则重新从起步速度开始。
std::atomic<double> MOVE_ACCEL(0.02);     // 每 tick 增加多少（px/tick²），0 = 不加速
std::atomic<double> MOVE_MAX_SPEED(5.0);  // 速度上限（px/tick）
std::atomic<double> SPEED_MULTIPLIER(3.0); // 按住加速键时的速度倍率，允许小数；<1 即减速
std::atomic<int> WHEEL_INTERVAL(80);      // 两次滚轮事件的最小间隔（ms）
std::atomic<int> WHEEL_SPEED(1);          // 每次滚动的"档位"数
std::atomic<int> WHEEL_SPEED_MULTIPLIER(3); // 按住加速键时的滚轮倍率

// ==================== 滚轮的三层限制 ====================
// 滚轮档位是"一次事件要滚多少行"，放开到几百档之后光靠"范围"挡不住
// 目标程序被压垮，于是分成三层：单条消息 / 单次事件 / 每秒总量。

// L1：一条 WM_MOUSEWHEEL 能带的最大档位。
// 消息里的累计量是 16 位有符号数（GET_WHEEL_DELTA_WPARAM 就是
// (short)HIWORD(wParam)），上限 32767，即单条最多 273 档；
// 超过这个数会被当成负数，表现为"反向滚、幅度还不对"。
// 取 200 档（24000）留足余量。
constexpr int NOTCH_PER_INPUT = 200;

// L2：单次滚轮事件的档位总量上限。
// 速度 260 × 加速 260 = 67600 档，拆成 338 条消息一次发出去，
// 目标程序要在那一帧处理 67600 行滚动 —— 等于卡死。
// 3000 档 = 15 条消息，一帧内可以承受；想再大就必须接受"卡一下"。
constexpr int MAX_NOTCH_PER_EVENT = 3000;

// L3：全局平均速率上限（档/秒）。
// L2 只管一次事件，挡不住"事件发生得很密"：刷新间隔 1 + 滚轮间隔 1 时
// 每秒上千次事件，每次 3000 档 → 每秒上百万档，目标程序冻住几秒。
// 60000 档/秒 ≈ 每秒 500 次标准滚轮，这个量级下程序会明显掉帧，
// 但不会永久无响应 —— 这是有意选在"放开"和"保险"之间的值。
constexpr int MAX_NOTCH_PER_SECOND = 60000;

// 按键映射（均为虚拟键码 VK_*，字母直接用其 ASCII 值）
// 用 atomic 是为了让捕获阶段（钩子线程）写入、控制线程读取时看不到半新半旧的值。
std::atomic<int> keyUp('E');         // 上移
std::atomic<int> keyDown('D');       // 下移
std::atomic<int> keyLeft('S');       // 左移
std::atomic<int> keyRight('F');      // 右移
std::atomic<int> keyLClick('J');     // 左键
std::atomic<int> keyRClick('K');     // 右键
std::atomic<int> keyMClick('M');     // 中键
std::atomic<int> keyWheelUp('O');    // 滚轮上
std::atomic<int> keyWheelDown('L');  // 滚轮下
std::atomic<int> keyBoost(VK_SHIFT); // 加速键：按住时移动/滚轮乘加速倍率

// 开关组合键（0 表示未设置）
// 有效成员必须"全部同时按下"才视为触发。
// 0 是哨兵值：表示该槽位不参与判定，等价于"空位不要求"。
std::atomic<int> switchKey1(VK_CAPITAL);
std::atomic<int> switchKey2(0);
std::atomic<int> switchKey3(0);
std::atomic<int> switchKey4(0);

// 组合键的生效方式：
//   0 = 切换（按一下开、再按一下关）
//   1 = 按住（按下时开、松开即关）
std::atomic<int> switchHoldMode(0);

// 功能开关
std::atomic<bool> isRunning(true);            // 控制线程的运行标志，退出时置 false
std::atomic<bool> isEnabled(false);           // 总开关：启动时默认"禁用"
std::atomic<bool> isMinimizeToTray(false);    // 关闭窗口时是否最小化到托盘
std::atomic<bool> interceptEnabled(true);     // 是否拦截映射键（关掉后不再吞键）

// ==================== 按键状态 ====================
// 钩子线程写入、控制线程读取。名字保持了历史含义（keyE/keyD...），
// 实际含义是"映射功能是否触发"，而不是"物理 E 键是否按下"。
std::atomic<bool> keyE(false), keyS(false), keyD(false), keyF(false);
std::atomic<bool> keyJ(false), keyK(false);
std::atomic<bool> keyO(false), keyL(false), keyM(false);
// 加速键当前是否按下。具体是哪个键由 keyBoost 决定，默认 Shift。
// 名字不再叫 keyShift，因为现在已经可以改成任意键。
std::atomic<bool> keyBoostDown(false);

// 滚轮加速键：和上面的加速键是**两个独立角色**。
// 默认值同样是 Shift，所以升级后的手感不变；但用户可以把它改到别的键上，
// 实现"Shift 只加速移动、滚轮另有一个加速键"。
std::atomic<int>  keyWheelBoost(VK_SHIFT);
std::atomic<bool> wheelBoostDown(false);

// CapsLock 被设成组合键后会被钩子吞掉，系统就不再生效、也不再更新它的状态，
// 所以由钩子自己记一份按下/松开，供 IsKeyDown 判定组合键使用。
std::atomic<bool> capsDown(false);

// ==================== 按键心跳 ====================
// 每个"由钩子自己维护状态"的键，记录最后一次 keydown 的时间。
// 按住键时系统会持续产生重复 keydown，时间戳会不断刷新；
// 一旦标记还是"按下"、时间戳却很久没更新，就说明 keyup 丢了
// （典型场景：切到任务管理器这类高权限窗口，UIPI 让钩子收不到事件），
// 看门狗据此把状态强制复位，避免光标一直走。
enum KeySlot {
    SLOT_UP, SLOT_DOWN, SLOT_LEFT, SLOT_RIGHT,
    SLOT_LCLICK, SLOT_RCLICK, SLOT_MCLICK,
    SLOT_WHEELUP, SLOT_WHEELDOWN, SLOT_BOOST, SLOT_WHEELBOOST, SLOT_CAPS,
    SLOT_COUNT
};
std::atomic<DWORD> g_lastKeyDown[SLOT_COUNT];

// 启动时清零：atomic 的默认构造不保证初始化值，必须显式写一遍。
void InitKeyTimers() {
    for (int i = 0; i < SLOT_COUNT; i++) g_lastKeyDown[i] = 0;
}

// 鼠标三键"当前是否处于按下状态"，用于幂等判断（同状态不重发）。
// 与控制线程配合，避免每帧重复发送 DOWN/UP 事件。
std::atomic<bool> mouseLeftDown(false);
std::atomic<bool> mouseRightDown(false);
std::atomic<bool> mouseMiddleDown(false);

// ==================== 全局句柄 ====================
HHOOK g_hKeyboardHook = NULL;   // 低级键盘钩子句柄，退出时 Unhook
HWND g_hMainWnd = NULL;         // 主窗口句柄，钩子里需要 PostMessage 会用到
NOTIFYICONDATAA g_nid = {};     // 托盘图标的数据结构

// 按键捕获状态
// 非 0 表示"正在等待用户按下一个键"，此时钩子不设置状态、不拦截
// 而是直接 PostMessage WM_BIND_KEY 给主线程去更新映射。
std::atomic<int> waitingButtonId(0);   // 正在等待按键的按钮 ID（0=无）

// 存档相关
std::string g_saveDir;                  // save/ 目录路径
std::vector<std::string> g_saveFiles;   // 存档文件名列表（不含路径，不含扩展名）
                                        // 注意：不包含 save0（它是隐式自动档）。
bool g_saveWritable = true;             // false = 只读模式：不写盘、也不谎报成功
bool g_saveDirFallback = false;         // true = 已回退到 %LOCALAPPDATA% 下的目录
bool g_backupBeforeSave = false;        // 读档失败过：下次保存前先备份原文件

// ==================== 草稿配置 ====================
// 界面上所有可改的项先写进这里的草稿，只有点"应用"/"保存"才推进内存里的
// 原子量（真正生效），"保存"再落盘。
// 这样"没点应用就不生效、没点保存就不落盘"的语义才成立。
struct ConfigDraft {
    double moveSpeed;                                               // 初始速度（可带小数）
    double moveAccel;                                               // 加速度（px/tick²）
    double moveMaxSpeed;                                            // 速度上限（px/tick）
    double speedMul;                                                // 加速倍率（可带小数）
    int wheelInterval, wheelSpeed, wheelMul;                        // 滚轮相关的 3 个数值
    int tickMs;                                                     // 刷新间隔（毫秒）
    int keyUp, keyDown, keyLeft, keyRight;                          // 4 个移动键
    int keyLClick, keyRClick, keyMClick;                            // 3 个鼠标键
    int keyWheelUp, keyWheelDown;                                   // 2 个滚轮键
    int boostKey;                                                   // 加速键（移动）
    int wheelBoostKey;                                              // 滚轮加速键
    int switch1, switch2, switch3, switch4;                         // 开关组合键
    int switchMode;                                                 // 组合键模式：0=切换，1=按住
    bool intercept;                                                 // 启用按键拦截
    bool tray;                                                      // 关闭时最小化到托盘
};

ConfigDraft g_draft = {};        // 界面当前展示的草稿值
bool g_dirty = false;            // 草稿和磁盘不一致（有没保存的修改）
bool g_updatingUI = false;       // 正在程序化刷新控件，用来屏蔽误触发的 EN_CHANGE
bool g_shuttingDown = false;     // 系统正在关机/注销，跳过"是否保存"的询问

// ==================== 按键名称转换 ====================
// 把虚拟键码翻译成 UI 上显示的文字。
// 常见键手写名字（保证中文界面统一），其余交给系统翻译，最后兜底显示键码。
std::string GetKeyName(int vk) {
    if (vk == 0) return "无";
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    switch (vk) {
        case VK_CONTROL: return "Ctrl";
        case VK_SHIFT: return "Shift";
        case VK_MENU: return "Alt";
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_TAB: return "Tab";
        case VK_ESCAPE: return "Esc";
        case VK_BACK: return "Backspace";
        case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_LEFT: return "左方向";
        case VK_RIGHT: return "右方向";
        case VK_UP: return "上方向";
        case VK_DOWN: return "下方向";
        case VK_F1: return "F1";
        case VK_F2: return "F2";
        case VK_F3: return "F3";
        case VK_F4: return "F4";
        case VK_F5: return "F5";
        case VK_F6: return "F6";
        case VK_F7: return "F7";
        case VK_F8: return "F8";
        case VK_F9: return "F9";
        case VK_F10: return "F10";
        case VK_F11: return "F11";
        case VK_F12: return "F12";
        // 锁定键：以前落到 default 全显示 "Key"，用户根本分不清。
        case VK_CAPITAL: return "CapsLock";
        case VK_NUMLOCK: return "NumLock";
        case VK_SCROLL: return "ScrollLock";
    }

    // 其余键交给系统翻译：小键盘、OEM 键、F13+ 都能拿到本地化名称。
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc != 0) {
        LONG keyParam = (LONG)(sc << 16);
        // 方向键、Home/End 等是"扩展键"，不带扩展位会被查成小键盘上的同名键。
        switch (vk) {
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
                keyParam |= (1 << 24);
                break;
        }
        char name[64];
        if (GetKeyNameTextA(keyParam, name, 64) > 0) return name;
    }

    // 实在查不到就显示键码，总比一堆重名 "Key" 强。
    char hex[16];
    sprintf_s(hex, "Key%02X", vk);
    return hex;
}

// 更新按钮文字
// 只更新单个按钮；调用者通常在知道具体改了哪个映射后调用。
void UpdateButtonText(HWND hwnd, int btnId, int vk) {
    std::string name = GetKeyName(vk);
    SetDlgItemTextA(hwnd, btnId, name.c_str());
}

// 更新所有按键按钮文字
// 读的是**草稿**而不是内存里的生效值：界面上要显示用户刚改、但还没点"应用"的映射。
// 在"重置""清空""加载存档""按键捕获完成"等场合调用。
void UpdateAllButtonText(HWND hwnd) {
    UpdateButtonText(hwnd, ID_BTN_KEY_UP, g_draft.keyUp);
    UpdateButtonText(hwnd, ID_BTN_KEY_DOWN, g_draft.keyDown);
    UpdateButtonText(hwnd, ID_BTN_KEY_LEFT, g_draft.keyLeft);
    UpdateButtonText(hwnd, ID_BTN_KEY_RIGHT, g_draft.keyRight);
    UpdateButtonText(hwnd, ID_BTN_KEY_LCLICK, g_draft.keyLClick);
    UpdateButtonText(hwnd, ID_BTN_KEY_RCLICK, g_draft.keyRClick);
    UpdateButtonText(hwnd, ID_BTN_KEY_MCLICK, g_draft.keyMClick);
    UpdateButtonText(hwnd, ID_BTN_KEY_WHEELUP, g_draft.keyWheelUp);
    UpdateButtonText(hwnd, ID_BTN_KEY_WHEELDOWN, g_draft.keyWheelDown);
    UpdateButtonText(hwnd, ID_BTN_KEY_BOOST, g_draft.boostKey);
    UpdateButtonText(hwnd, ID_BTN_KEY_WHEELBOOST, g_draft.wheelBoostKey);
    UpdateButtonText(hwnd, ID_BTN_SWITCH1, g_draft.switch1);
    UpdateButtonText(hwnd, ID_BTN_SWITCH2, g_draft.switch2);
    UpdateButtonText(hwnd, ID_BTN_SWITCH3, g_draft.switch3);
    UpdateButtonText(hwnd, ID_BTN_SWITCH4, g_draft.switch4);
}

// 底部状态行：正常时显示存档路径，异常时显示原因。
// 放在窗口底部固定一行，避免再往标题栏塞信息。
void SetStatus(HWND hwnd, const std::string& text) {
    SetDlgItemTextA(hwnd, ID_STATIC_STATUS, text.c_str());
}

// ==================== 存档路径 ====================
// 获取 exe 所在目录
std::string GetExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string exePath(buf);
    size_t pos = exePath.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string(".") : exePath.substr(0, pos);
}

// 获取 save/ 目录路径
// 固定策略：与 exe 同目录下的 save/ 子目录，与 exe 是否在系统盘无关。
// 这样拷到 U 盘也能"带着存档走"。
std::string GetSaveDir() {
    return GetExeDir() + "\\save";
}

// 确保目录存在且真的能写
// CreateDirectory 成功不等于可写（只读介质、组策略、杀毒软件都可能拦），
// 所以实际写一个临时文件来验证，写完立刻删掉。
bool EnsureDirWritable(const std::string& dir) {
    if (!CreateDirectoryA(dir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    std::string probe = dir + "\\.write_test";
    HANDLE h = CreateFileA(probe.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileA(probe.c_str());
    return true;
}

// 用户目录下的备选存档路径：%LOCALAPPDATA%\KeyboardCursor\save
// 放在用户配置目录里，普通权限即可写，不受 Program Files 的只读限制。
std::string GetLocalSaveDir() {
    // 用 getenv 而不是 MSVC 专有的 _dupenv_s，这样 GCC / MSVC 都能编。
    // getenv 返回的是 CRT 内部缓冲区，调用方不能也不该 free。
    const char* p = getenv("LOCALAPPDATA");
    std::string base = (p != nullptr) ? p : "";
    if (base.empty()) return GetSaveDir();   // 极端情况退回原目录
    return base + "\\KeyboardCursor\\save";
}

// 决定最终使用的存档目录
//   1) exe\save 可写        → 正常模式
//   2) 不可写，用户同意回退  → %LOCALAPPDATA% 下的目录
//   3) 不可写，用户不同意    → 只读模式（程序照常运行，只是不写盘）
// 这一步必须在创建窗口之前完成：WM_CREATE 里就要扫描目录、读 save0.ini，
// 否则会有一段"目录还没定"的时间处于无保护状态。
void ResolveSaveDir() {
    std::string exeSave = GetSaveDir();
    if (EnsureDirWritable(exeSave)) {
        g_saveDir = exeSave;
        g_saveWritable = true;
        return;
    }

    std::string localSave = GetLocalSaveDir();
    std::string msg = "程序所在目录不可写，无法在 exe 旁保存存档。\n\n"
                      "是否改为保存到：\n" + localSave + "\n\n"
                      "选择“否”将以只读模式继续运行：\n"
                      "可以正常使用映射，但设置不会被保存。";
    if (MessageBoxA(NULL, msg.c_str(), "存档目录",
                    MB_YESNO | MB_ICONQUESTION) == IDYES
        && EnsureDirWritable(localSave)) {
        g_saveDir = localSave;
        g_saveWritable = true;
        g_saveDirFallback = true;
    } else {
        // 用户拒绝回退，或回退目标也写不了：降级为只读，不退出程序。
        g_saveDir = exeSave;
        g_saveWritable = false;
    }
}

// 检查文件是否存在
// 只算"普通文件"，目录视为不存在——因为本函数用于判断存档文件，
// 若同名目录存在，绝不能把它当成存档。
bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// 创建空存档文件
// 只在文件不存在时写默认值；存在则不动。用于首次运行时初始化，
// 后续 SaveToFile 会覆盖式写入——因为 INI 写入是"设置键值"而不是"删除文件"。
void CreateSaveFile(const std::string& name) {
    if (!g_saveWritable) return;   // 只读模式下不碰磁盘

    std::string path = g_saveDir + "\\" + name + ".ini";
    if (!FileExists(path)) {
        // 写入默认设置
        // 与程序内初始值保持一致，避免"加载后突变"。
        char buf[32];
        // 初始速度带小数，直接写字符串，格式和 SaveToFile 保持一致
        WritePrivateProfileStringA("Speed", "MOVE_SPEED", "1.5", path.c_str());
        // 加速度和上限是后加的，新档也写上默认值，免得靠读档时的缺省兜底
        WritePrivateProfileStringA("Speed", "MOVE_ACCEL", "0.02", path.c_str());
        WritePrivateProfileStringA("Speed", "MOVE_MAX_SPEED", "5", path.c_str());
        sprintf_s(buf, "%d", 3);  WritePrivateProfileStringA("Speed", "SPEED_MULTIPLIER", buf, path.c_str());
        sprintf_s(buf, "%d", 80); WritePrivateProfileStringA("Speed", "WHEEL_INTERVAL", buf, path.c_str());
        sprintf_s(buf, "%d", 1);  WritePrivateProfileStringA("Speed", "WHEEL_SPEED", buf, path.c_str());
        sprintf_s(buf, "%d", 3);  WritePrivateProfileStringA("Speed", "WHEEL_SPEED_MULTIPLIER", buf, path.c_str());
        WritePrivateProfileStringA("Keys", "keyUp", "69", path.c_str());
        WritePrivateProfileStringA("Keys", "keyDown", "68", path.c_str());
        WritePrivateProfileStringA("Keys", "keyLeft", "83", path.c_str());
        WritePrivateProfileStringA("Keys", "keyRight", "70", path.c_str());
        WritePrivateProfileStringA("Keys", "keyLClick", "74", path.c_str());
        WritePrivateProfileStringA("Keys", "keyRClick", "75", path.c_str());
        WritePrivateProfileStringA("Keys", "keyMClick", "77", path.c_str());
        WritePrivateProfileStringA("Keys", "keyWheelUp", "79", path.c_str());
        WritePrivateProfileStringA("Keys", "keyWheelDown", "76", path.c_str());
        WritePrivateProfileStringA("Keys", "boostKey", "16", path.c_str());
        // 滚轮加速键也是后加的：新档直接写默认值，免得靠读档缺省兜底。
        WritePrivateProfileStringA("Keys", "wheelBoostKey", "16", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey1", "20", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey2", "0", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey3", "0", path.c_str());
        WritePrivateProfileStringA("Switch", "switchKey4", "0", path.c_str());
        WritePrivateProfileStringA("Options", "interceptEnabled", "1", path.c_str());
        WritePrivateProfileStringA("Options", "isMinimizeToTray", "0", path.c_str());
    }
}

// 扫描 save/ 目录，填入 g_saveFiles
// 只收集 *.ini 文件（排除 save0.ini）；并以文件名排序保证下拉框顺序稳定。
void ScanSaveDir() {
    g_saveFiles.clear();

    std::string pattern = g_saveDir + "\\*.ini";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string fileName = findData.cFileName;
            // 去掉 .ini 后缀
            size_t pos = fileName.find_last_of('.');
            if (pos != std::string::npos) {
                fileName = fileName.substr(0, pos);
            }
            // 排除 save0
            // save0 是隐式自动档，不展示给用户，避免误保存。
            if (fileName != "save0") {
                g_saveFiles.push_back(fileName);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }

    // 排序
    // 简单字典序：save1, save10, save11 ... save2。
    // 未做自然排序，但足够用，不影响功能。
    std::sort(g_saveFiles.begin(), g_saveFiles.end());
}

// 确保 save/ 目录和三个默认存档存在
// CreateDirectory 在目录已存在时必然失败，忽略返回值即可；CreateSaveFile
// 内部也判存在，所以"补建默认档"可以无条件做。
// 旧实现只在目录刚创建时才建默认档，用户误删 save*.ini 后下拉框就只剩 "+"。
void EnsureSaveDir() {
    if (!g_saveWritable) return;   // 只读模式下不碰磁盘

    CreateDirectoryA(g_saveDir.c_str(), NULL);
    CreateSaveFile("save1");
    CreateSaveFile("save2");
    CreateSaveFile("save3");
}

// 把存档列表填入下拉框
// 每次刷新都会先 CB_RESETCONTENT，然后在末尾追加 "+"，
// "+" 作为"新建存档"的入口（在 ID_COMBO_SAVE 的 CBN_SELCHANGE 中被识别）。
void FillComboBox(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
    if (!hCombo) return;

    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);

    for (size_t i = 0; i < g_saveFiles.size(); i++) {
        SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)g_saveFiles[i].c_str());
    }

    // 最后加 "+"
    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"+");

    // 默认选中第一个
    SendMessageA(hCombo, CB_SETCURSEL, 0, 0);
}

// ==================== 草稿 / 应用 ====================
// 出厂默认值。重置按钮、以及"启动时没有存档"的初始草稿都用它。
void DraftDefaults() {
    g_draft.moveSpeed     = 1.5;
    g_draft.moveAccel     = 0.02;
    g_draft.moveMaxSpeed  = 5.0;
    g_draft.speedMul      = 3.0;
    g_draft.wheelInterval = 80;
    g_draft.wheelSpeed    = 1;
    g_draft.wheelMul      = 3;
    g_draft.tickMs        = DEFAULT_TICK_MS;

    g_draft.keyUp        = 'E';
    g_draft.keyDown      = 'D';
    g_draft.keyLeft      = 'S';
    g_draft.keyRight     = 'F';
    g_draft.keyLClick    = 'J';
    g_draft.keyRClick    = 'K';
    g_draft.keyMClick    = 'M';
    g_draft.keyWheelUp   = 'O';
    g_draft.keyWheelDown = 'L';
    g_draft.boostKey     = VK_SHIFT;
    // 滚轮加速键默认也是 Shift：与旧版本行为一致（当时 Shift 同时加速移动和滚轮）。
    // 想区分开的话，把它改成别的键即可。
    g_draft.wheelBoostKey = VK_SHIFT;

    g_draft.switch1 = VK_CAPITAL;
    g_draft.switch2 = 0;
    g_draft.switch3 = 0;
    g_draft.switch4 = 0;
    g_draft.switchMode = 0;   // 默认"按下切换"，和旧版本行为一致

    g_draft.intercept = true;
    g_draft.tray      = false;
}

// 把内存里"已生效"的值抄进草稿。
// 启动、读档之后必须调一次，否则草稿和实际状态会脱节。
void DraftFromApplied() {
    g_draft.moveSpeed     = MOVE_SPEED.load();
    g_draft.moveAccel     = MOVE_ACCEL.load();
    g_draft.moveMaxSpeed  = MOVE_MAX_SPEED.load();
    g_draft.speedMul      = SPEED_MULTIPLIER.load();
    g_draft.wheelInterval = WHEEL_INTERVAL.load();
    g_draft.wheelSpeed    = WHEEL_SPEED.load();
    g_draft.wheelMul      = WHEEL_SPEED_MULTIPLIER.load();
    g_draft.tickMs        = TICK_MS.load();

    g_draft.keyUp        = keyUp.load();
    g_draft.keyDown      = keyDown.load();
    g_draft.keyLeft      = keyLeft.load();
    g_draft.keyRight     = keyRight.load();
    g_draft.keyLClick    = keyLClick.load();
    g_draft.keyRClick    = keyRClick.load();
    g_draft.keyMClick    = keyMClick.load();
    g_draft.keyWheelUp   = keyWheelUp.load();
    g_draft.keyWheelDown = keyWheelDown.load();
    g_draft.boostKey     = keyBoost.load();
    g_draft.wheelBoostKey = keyWheelBoost.load();

    g_draft.switch1 = switchKey1.load();
    g_draft.switch2 = switchKey2.load();
    g_draft.switch3 = switchKey3.load();
    g_draft.switch4 = switchKey4.load();
    g_draft.switchMode = switchHoldMode.load();

    g_draft.intercept = interceptEnabled.load();
    g_draft.tray      = isMinimizeToTray.load();
}

// 把草稿推进内存，真正生效。
// 只改内存、不写盘 —— 写盘是 SaveToFile 的事。
// 钩子线程读的是这些原子量，所以"点应用"之后行为才会变。
void ApplyDraft() {
    MOVE_SPEED             = g_draft.moveSpeed;
    MOVE_ACCEL             = g_draft.moveAccel;
    MOVE_MAX_SPEED         = g_draft.moveMaxSpeed;
    SPEED_MULTIPLIER       = g_draft.speedMul;
    WHEEL_INTERVAL         = g_draft.wheelInterval;
    WHEEL_SPEED            = g_draft.wheelSpeed;
    WHEEL_SPEED_MULTIPLIER = g_draft.wheelMul;
    TICK_MS                = g_draft.tickMs;

    keyUp        = g_draft.keyUp;
    keyDown      = g_draft.keyDown;
    keyLeft      = g_draft.keyLeft;
    keyRight     = g_draft.keyRight;
    keyLClick    = g_draft.keyLClick;
    keyRClick    = g_draft.keyRClick;
    keyMClick    = g_draft.keyMClick;
    keyWheelUp   = g_draft.keyWheelUp;
    keyWheelDown = g_draft.keyWheelDown;
    keyBoost     = g_draft.boostKey;
    keyWheelBoost = g_draft.wheelBoostKey;

    switchKey1 = g_draft.switch1;
    switchKey2 = g_draft.switch2;
    switchKey3 = g_draft.switch3;
    switchKey4 = g_draft.switch4;
    switchHoldMode = g_draft.switchMode;

    interceptEnabled = g_draft.intercept;
    isMinimizeToTray = g_draft.tray;
}

// 把速度格式化成紧凑字符串：2 而不是 2.000000，2.5 而不是 2.500000。
// 保存、界面显示共用这一套格式，保证"写进去的字符串"和"读回来的"完全一致。
std::string FormatSpeed(double v) {
    char buf[32];
    sprintf_s(buf, "%.6f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s[s.size() - 1] == '0') s.erase(s.size() - 1);
        if (!s.empty() && s[s.size() - 1] == '.') s.erase(s.size() - 1);
    }
    return s;
}

// 把各个数值框的内容读进草稿。
// 带范围钳制：空、非数字、越界一律忽略，并把编辑框改回草稿里的有效值，
// 避免出现"框里显示空、实际保存的是旧值"这种对不上的情况。
void ReadEditsIntoDraft(HWND hwnd) {
    g_updatingUI = true;   // 下面回写编辑框会触发 EN_CHANGE，别把它当成用户改动

    char buf[32];
    int v;

    // 初始速度：符号决定方向（负 = 反向），0 = 静止起步。
    // 单位是"每拍像素数"，所以实际速度还受刷新间隔影响（见下方 tickMs 注释）。
    GetDlgItemTextA(hwnd, ID_EDIT_SPEED, buf, 32);
    double spd = atof(buf);
    if (spd >= Limit::SPEED_LO && spd <= Limit::SPEED_HI) g_draft.moveSpeed = spd;
    else SetDlgItemTextA(hwnd, ID_EDIT_SPEED, FormatSpeed(g_draft.moveSpeed).c_str());

    // 加速度：负值 = 减速（减到 0 停住，不会自己反向）。
    GetDlgItemTextA(hwnd, ID_EDIT_ACCEL, buf, 32);
    double acc = atof(buf);
    if (acc >= Limit::ACCEL_LO && acc <= Limit::ACCEL_HI) g_draft.moveAccel = acc;
    else SetDlgItemTextA(hwnd, ID_EDIT_ACCEL, FormatSpeed(g_draft.moveAccel).c_str());

    // 速度上限：不小于初速时是天花板，小于初速时当下限用
    // （速度不会低于它，上界改成初速）——旧配置的手感因此得以保留。
    // 它表示"大小"，所以不接受负值。
    GetDlgItemTextA(hwnd, ID_EDIT_MAXSPEED, buf, 32);
    double mx = atof(buf);
    if (mx >= Limit::MAXSP_LO && mx <= Limit::MAXSP_HI) g_draft.moveMaxSpeed = mx;
    else SetDlgItemTextA(hwnd, ID_EDIT_MAXSPEED, FormatSpeed(g_draft.moveMaxSpeed).c_str());

    // 加速倍率：负值 = 按住时翻转方向；0 = 刹车。
    // 它同时参与初速和上限的计算，cap 用 |倍率|，所以负值才是"反向"而不是"反向且变慢"。
    GetDlgItemTextA(hwnd, ID_EDIT_SPEED_MUL, buf, 32);
    double mulV = atof(buf);
    if (mulV >= Limit::MUL_LO && mulV <= Limit::MUL_HI) g_draft.speedMul = mulV;
    else SetDlgItemTextA(hwnd, ID_EDIT_SPEED_MUL, FormatSpeed(g_draft.speedMul).c_str());

    // 下面四项是整数：用 atoi 解析（小数会被截断），越界回退到草稿里的原值。
    GetDlgItemTextA(hwnd, ID_EDIT_WHEEL_INTERVAL, buf, 32);
    v = atoi(buf);
    if (v >= Limit::WIVL_LO && v <= Limit::WIVL_HI) g_draft.wheelInterval = v;
    else SetDlgItemInt(hwnd, ID_EDIT_WHEEL_INTERVAL, g_draft.wheelInterval, FALSE);

    // 注意：滚轮速度/加速填得再大，单次实际发出的档位仍受
    // MAX_NOTCH_PER_EVENT(3000) 截断，所以"速度 × 加速 > 3000"时
    // 多出来的部分不会生效（速度 ≥ 12 时，加速填满 260 就会被截断）。
    GetDlgItemTextA(hwnd, ID_EDIT_WHEEL_SPEED, buf, 32);
    v = atoi(buf);
    if (v >= Limit::WSPD_LO && v <= Limit::WSPD_HI) g_draft.wheelSpeed = v;
    else SetDlgItemInt(hwnd, ID_EDIT_WHEEL_SPEED, g_draft.wheelSpeed, FALSE);

    GetDlgItemTextA(hwnd, ID_EDIT_WHEEL_MUL, buf, 32);
    v = atoi(buf);
    if (v >= Limit::WMUL_LO && v <= Limit::WMUL_HI) g_draft.wheelMul = v;
    else SetDlgItemInt(hwnd, ID_EDIT_WHEEL_MUL, g_draft.wheelMul, FALSE);

    // 刷新间隔：决定控制线程多久醒一次。
    // 1ms 已经低于多数显示器的刷新周期，再快只是白唤醒 CPU；
    // 300ms 则光标会明显拖影。越界一律回退到草稿里的原值。
    // 提醒：初始速度的单位是 px/拍，把这里从 6 改到 1，
    // 同一个速度值的实际速度会变成 6 倍，调参时要一起看。
    GetDlgItemTextA(hwnd, ID_EDIT_TICK, buf, 32);
    v = atoi(buf);
    if (v >= Limit::TICK_LO && v <= Limit::TICK_HI) g_draft.tickMs = v;
    else SetDlgItemInt(hwnd, ID_EDIT_TICK, g_draft.tickMs, FALSE);

    g_updatingUI = false;
}

// 用草稿刷新整个界面：编辑框、两个复选框、9 个映射按钮。
// g_updatingUI 屏蔽 SetDlgItemInt 触发的 EN_CHANGE，
// 否则"程序自己刷新"会被当成"用户改了内容"，把脏标记错误地设上。
void RefreshUIFromDraft(HWND hwnd) {
    g_updatingUI = true;

    // 速度相关四项可带小数，不能用 SetDlgItemInt
    SetDlgItemTextA(hwnd, ID_EDIT_SPEED, FormatSpeed(g_draft.moveSpeed).c_str());
    SetDlgItemTextA(hwnd, ID_EDIT_ACCEL, FormatSpeed(g_draft.moveAccel).c_str());
    SetDlgItemTextA(hwnd, ID_EDIT_MAXSPEED, FormatSpeed(g_draft.moveMaxSpeed).c_str());
    SetDlgItemTextA(hwnd, ID_EDIT_SPEED_MUL, FormatSpeed(g_draft.speedMul).c_str());
    SetDlgItemInt(hwnd, ID_EDIT_WHEEL_INTERVAL, g_draft.wheelInterval, FALSE);
    SetDlgItemInt(hwnd, ID_EDIT_WHEEL_SPEED, g_draft.wheelSpeed, FALSE);
    SetDlgItemInt(hwnd, ID_EDIT_WHEEL_MUL, g_draft.wheelMul, FALSE);
    SetDlgItemInt(hwnd, ID_EDIT_TICK, g_draft.tickMs, FALSE);

    CheckDlgButton(hwnd, ID_CHECK_INTERCEPT,
                   g_draft.intercept ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, ID_CHECK_TRAY,
                   g_draft.tray ? BST_CHECKED : BST_UNCHECKED);

    // 两个单选按钮必须成组刷新：CheckRadioButton 会把组内其它按钮取消勾选。
    // 注意它只发 BM_SETCHECK，不会回发 WM_COMMAND，所以不会把界面刷新当成用户操作。
    CheckRadioButton(hwnd, ID_RADIO_SWITCH_TOGGLE, ID_RADIO_SWITCH_HOLD,
                     g_draft.switchMode == 0 ? ID_RADIO_SWITCH_TOGGLE : ID_RADIO_SWITCH_HOLD);

    UpdateAllButtonText(hwnd);

    g_updatingUI = false;
}

// 取下拉框当前选中的存档名；选的是 "+" 或者根本没选，返回空串。
std::string GetSelectedSlot(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return "";

    char name[256] = {};
    SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);
    if (strcmp(name, "+") == 0) return "";
    return name;
}

// ==================== 存档读写 ====================
// 把当前设置写到指定文件
// 每个配置项独立 WritePrivateProfileStringA，属于"按键值覆盖"，
// 不会清空未写到的键——因此不需要先删除现有文件。
// 返回值：true 表示写入后能读回，false 表示没写成（只读模式 / 磁盘问题）。
bool SaveToFile(const std::string& name) {
    if (!g_saveWritable) return false;   // 只读模式：不写、也不谎报成功

    std::string path = g_saveDir + "\\" + name + ".ini";

    // 之前读档失败过一次：先把原文件备份，避免把可能还有救的数据直接覆盖。
    if (g_backupBeforeSave) {
        std::string bak = path + ".bak";
        CopyFileA(path.c_str(), bak.c_str(), FALSE);   // 备份失败不阻断保存
        g_backupBeforeSave = false;
    }

    char buf[32];

    // 初始速度可能带小数，用 FormatSpeed 统一格式；
    // speedStr 留到函数末尾做读回校验。
    std::string speedStr = FormatSpeed(MOVE_SPEED.load());
    WritePrivateProfileStringA("Speed", "MOVE_SPEED", speedStr.c_str(), path.c_str());

    // 加速度和上限同样可能带小数
    WritePrivateProfileStringA("Speed", "MOVE_ACCEL",
                               FormatSpeed(MOVE_ACCEL.load()).c_str(), path.c_str());
    WritePrivateProfileStringA("Speed", "MOVE_MAX_SPEED",
                               FormatSpeed(MOVE_MAX_SPEED.load()).c_str(), path.c_str());
    WritePrivateProfileStringA("Speed", "SPEED_MULTIPLIER",
                               FormatSpeed(SPEED_MULTIPLIER.load()).c_str(), path.c_str());
    sprintf_s(buf, "%d", WHEEL_INTERVAL.load());
    WritePrivateProfileStringA("Speed", "WHEEL_INTERVAL", buf, path.c_str());
    sprintf_s(buf, "%d", WHEEL_SPEED.load());
    WritePrivateProfileStringA("Speed", "WHEEL_SPEED", buf, path.c_str());
    sprintf_s(buf, "%d", WHEEL_SPEED_MULTIPLIER.load());
    WritePrivateProfileStringA("Speed", "WHEEL_SPEED_MULTIPLIER", buf, path.c_str());

    // 刷新间隔：也放在 [Speed] 段，因为界面上它在速度组里。
    sprintf_s(buf, "%d", TICK_MS.load());
    WritePrivateProfileStringA("Speed", "tickMs", buf, path.c_str());

    // 按键映射：直接存 VK 码数值，读回时按需解释，不依赖键名表。
    sprintf_s(buf, "%d", keyUp.load());
    WritePrivateProfileStringA("Keys", "keyUp", buf, path.c_str());
    sprintf_s(buf, "%d", keyDown.load());
    WritePrivateProfileStringA("Keys", "keyDown", buf, path.c_str());
    sprintf_s(buf, "%d", keyLeft.load());
    WritePrivateProfileStringA("Keys", "keyLeft", buf, path.c_str());
    sprintf_s(buf, "%d", keyRight.load());
    WritePrivateProfileStringA("Keys", "keyRight", buf, path.c_str());
    sprintf_s(buf, "%d", keyLClick.load());
    WritePrivateProfileStringA("Keys", "keyLClick", buf, path.c_str());
    sprintf_s(buf, "%d", keyRClick.load());
    WritePrivateProfileStringA("Keys", "keyRClick", buf, path.c_str());
    sprintf_s(buf, "%d", keyMClick.load());
    WritePrivateProfileStringA("Keys", "keyMClick", buf, path.c_str());
    sprintf_s(buf, "%d", keyWheelUp.load());
    WritePrivateProfileStringA("Keys", "keyWheelUp", buf, path.c_str());
    sprintf_s(buf, "%d", keyWheelDown.load());
    WritePrivateProfileStringA("Keys", "keyWheelDown", buf, path.c_str());
    sprintf_s(buf, "%d", keyBoost.load());
    WritePrivateProfileStringA("Keys", "boostKey", buf, path.c_str());
    sprintf_s(buf, "%d", keyWheelBoost.load());
    WritePrivateProfileStringA("Keys", "wheelBoostKey", buf, path.c_str());

    // 组合键：0 表示该槽位空，读回时按同样规则解释。
    sprintf_s(buf, "%d", switchKey1.load());
    WritePrivateProfileStringA("Switch", "switchKey1", buf, path.c_str());
    sprintf_s(buf, "%d", switchKey2.load());
    WritePrivateProfileStringA("Switch", "switchKey2", buf, path.c_str());
    sprintf_s(buf, "%d", switchKey3.load());
    WritePrivateProfileStringA("Switch", "switchKey3", buf, path.c_str());
    sprintf_s(buf, "%d", switchKey4.load());
    WritePrivateProfileStringA("Switch", "switchKey4", buf, path.c_str());

    // 组合键模式：0=切换，1=按住。
    // 旧存档没有这个键，读的时候默认 0，行为和老版本一致。
    sprintf_s(buf, "%d", switchHoldMode.load());
    WritePrivateProfileStringA("Switch", "holdMode", buf, path.c_str());

    // 布尔值用 "1"/"0" 存，读时用 !=0 判断，对旧存档也兼容。
    WritePrivateProfileStringA("Options", "interceptEnabled",
        interceptEnabled.load() ? "1" : "0", path.c_str());
    WritePrivateProfileStringA("Options", "isMinimizeToTray",
        isMinimizeToTray.load() ? "1" : "0", path.c_str());

    // 抽样复核：API 返回成功不代表真写进去了（磁盘满、被占用等）。
    // 比的是"写进去的字符串"和"读回来的字符串"，避开浮点相等的坑。
    char verify[32] = {};
    GetPrivateProfileStringA("Speed", "MOVE_SPEED", "", verify, 32, path.c_str());
    return speedStr == verify;
}

// 读档结果
//   LOAD_OK      正常读取
//   LOAD_MISSING 文件不存在（首次运行，静默用默认值，不打扰用户）
//   LOAD_FAILED  文件存在但读不出内容（损坏 / 被占用 / 没权限）
enum LoadResult { LOAD_OK, LOAD_MISSING, LOAD_FAILED };

// ini 里的整数没有任何范围保证：可能被手改成 0、负数或超大值。
// GetPrivateProfileIntA 的返回值是 UINT，负数字符串会被解释成大数，
// 所以读出来必须自己钳一遍，不能直接塞进 atomic。
static int ClampIniInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 从指定文件读设置
// 读完后会自动同步到 UI 控件，因此调用方不需要再手动刷新界面。
LoadResult LoadFromFile(const std::string& name) {
    std::string path = g_saveDir + "\\" + name + ".ini";

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LOAD_MISSING;

    // 用"能不能读到已知键"判断文件是否可用；读不到就当损坏处理。
    char probe[32] = {};
    if (GetPrivateProfileStringA("Speed", "MOVE_SPEED", "", probe, 32, path.c_str()) == 0) {
        g_backupBeforeSave = true;   // 下次保存前先备份，别直接覆盖
        return LOAD_FAILED;
    }

    // 缺省值使用程序的"出厂默认"，这样即便存档缺失/损坏也能保持可用。
    // 这里的越界处理是"截断到边界"（不是回退）：读档时没有"上一个有效值"可用。
    // 边界与界面校验共用 Limit 里的一份定义，避免两边对不上。

    // 初始速度：允许负数（方向反转）和小数。
    char speedBuf[32] = {};
    GetPrivateProfileStringA("Speed", "MOVE_SPEED", "1.5", speedBuf, 32, path.c_str());
    double spd = atof(speedBuf);
    if (spd < Limit::SPEED_LO) spd = Limit::SPEED_LO;
    if (spd > Limit::SPEED_HI) spd = Limit::SPEED_HI;
    MOVE_SPEED = spd;

    // 加速度：缺省 0.02。负值 = 减速。
    char accBuf[32] = {};
    GetPrivateProfileStringA("Speed", "MOVE_ACCEL", "0.02", accBuf, 32, path.c_str());
    double accV = atof(accBuf);
    if (accV < Limit::ACCEL_LO) accV = Limit::ACCEL_LO;
    if (accV > Limit::ACCEL_HI) accV = Limit::ACCEL_HI;
    MOVE_ACCEL = accV;

    // 速度上限：缺省 5。小于初速时当下限用，不小于时当天花板。
    char maxBuf[32] = {};
    GetPrivateProfileStringA("Speed", "MOVE_MAX_SPEED", "5", maxBuf, 32, path.c_str());
    double maxV = atof(maxBuf);
    if (maxV < Limit::MAXSP_LO) maxV = Limit::MAXSP_LO;
    if (maxV > Limit::MAXSP_HI) maxV = Limit::MAXSP_HI;
    MOVE_MAX_SPEED = maxV;

    // 加速倍率：允许小数和负值。负值 = 按住时翻转方向。
    char mulBuf[32] = {};
    GetPrivateProfileStringA("Speed", "SPEED_MULTIPLIER", "3", mulBuf, 32, path.c_str());
    double mulV = atof(mulBuf);
    if (mulV < Limit::MUL_LO) mulV = Limit::MUL_LO;
    if (mulV > Limit::MUL_HI) mulV = Limit::MUL_HI;
    SPEED_MULTIPLIER = mulV;

    // 滚轮三项以前完全没有钳制，ini 被改成 0 时 WHEEL_INTERVAL=0 会让
    // "距上次超过 0ms"恒成立 —— 每个 tick 都发一次滚轮。补上。
    WHEEL_INTERVAL = ClampIniInt(
        (int)GetPrivateProfileIntA("Speed", "WHEEL_INTERVAL", 80, path.c_str()),
        Limit::WIVL_LO, Limit::WIVL_HI);
    WHEEL_SPEED = ClampIniInt(
        (int)GetPrivateProfileIntA("Speed", "WHEEL_SPEED", 1, path.c_str()),
        Limit::WSPD_LO, Limit::WSPD_HI);
    WHEEL_SPEED_MULTIPLIER = ClampIniInt(
        (int)GetPrivateProfileIntA("Speed", "WHEEL_SPEED_MULTIPLIER", 3, path.c_str()),
        Limit::WMUL_LO, Limit::WMUL_HI);

    // 刷新间隔必须在这里就钳住：如果 ini 被手改成 0，
    // Wait(0) 会退化成忙等死循环，把 CPU 拉满。
    TICK_MS = ClampIniInt(
        (int)GetPrivateProfileIntA("Speed", "tickMs", DEFAULT_TICK_MS, path.c_str()),
        Limit::TICK_LO, Limit::TICK_HI);

    keyUp = GetPrivateProfileIntA("Keys", "keyUp", 69, path.c_str());
    keyDown = GetPrivateProfileIntA("Keys", "keyDown", 68, path.c_str());
    keyLeft = GetPrivateProfileIntA("Keys", "keyLeft", 83, path.c_str());
    keyRight = GetPrivateProfileIntA("Keys", "keyRight", 70, path.c_str());
    keyLClick = GetPrivateProfileIntA("Keys", "keyLClick", 74, path.c_str());
    keyRClick = GetPrivateProfileIntA("Keys", "keyRClick", 75, path.c_str());
    keyMClick = GetPrivateProfileIntA("Keys", "keyMClick", 77, path.c_str());
    keyWheelUp = GetPrivateProfileIntA("Keys", "keyWheelUp", 79, path.c_str());
    keyWheelDown = GetPrivateProfileIntA("Keys", "keyWheelDown", 76, path.c_str());
    // 加速键：老存档没有这个键时默认 Shift(16)，行为和旧版本一致
    keyBoost = GetPrivateProfileIntA("Keys", "boostKey", 16, path.c_str());
    // 滚轮加速键：旧存档同样没有，缺省 16 —— 这样"按 Shift 加速滚轮"的老手感不变。
    keyWheelBoost = GetPrivateProfileIntA("Keys", "wheelBoostKey", 16, path.c_str());

    switchKey1 = GetPrivateProfileIntA("Switch", "switchKey1", 20, path.c_str());
    switchKey2 = GetPrivateProfileIntA("Switch", "switchKey2", 0, path.c_str());
    switchKey3 = GetPrivateProfileIntA("Switch", "switchKey3", 0, path.c_str());
    switchKey4 = GetPrivateProfileIntA("Switch", "switchKey4", 0, path.c_str());
    switchHoldMode = GetPrivateProfileIntA("Switch", "holdMode", 0, path.c_str());

    interceptEnabled = GetPrivateProfileIntA("Options", "interceptEnabled", 1, path.c_str()) != 0;
    isMinimizeToTray = GetPrivateProfileIntA("Options", "isMinimizeToTray", 0, path.c_str()) != 0;

    // 读进来的是"已生效"的值，草稿要同步一份，界面再按草稿刷新。
    // 只走 RefreshUIFromDraft 这一条路，保证界面文字和草稿永远一致。
    DraftFromApplied();
    RefreshUIFromDraft(g_hMainWnd);
    g_dirty = false;   // 刚读进来，内存和文件是一致的

    return LOAD_OK;
}

// 判断某个键是否属于当前四个开关槽位。
// 槽位为 0 表示空位，不参与判定；vk 为 0 时直接返回 false。
bool IsSwitchMember(int vk) {
    return vk != 0 &&
           (vk == switchKey1.load() || vk == switchKey2.load() ||
            vk == switchKey3.load() || vk == switchKey4.load());
}

// ==================== 判断是否需要拦截 ====================
bool ShouldIntercept(DWORD vkCode) {
    // CapsLock 参与组合键、或充当任一种加速键时必须吞掉：不吞的话按一下就把
    // 大小写翻转了。这里特意不受 isEnabled 影响 —— 否则"禁用"状态下按它
    // 仍会切大小写；但仍然尊重用户手动关掉"启用按键拦截"的选择。
    // 加速键的状态是钩子自己记的，吞掉不影响它工作。
    if (vkCode == VK_CAPITAL &&
        (IsSwitchMember((int)vkCode) || vkCode == (DWORD)keyBoost.load() ||
         vkCode == (DWORD)keyWheelBoost.load()))
        return interceptEnabled.load();

    // 总开关未启用、或用户手动关闭拦截时，一律放行。
    if (!isEnabled || !interceptEnabled) return false;

    // 其余组合键成员不拦截，保证 GetAsyncKeyState 能读到按键状态
    // 否则 Ctrl/Shift 这类键被吞掉，系统层面永远"没按下"，组合键形同失效。
    if (IsSwitchMember((int)vkCode)) return false;

    // 映射键：处于启用+拦截状态时吞掉，防止按键透传到前台窗口。
    return vkCode == keyUp.load() || vkCode == keyDown.load() ||
           vkCode == keyLeft.load() || vkCode == keyRight.load() ||
           vkCode == keyLClick.load() || vkCode == keyRClick.load() ||
           vkCode == keyMClick.load() || vkCode == keyWheelUp.load() ||
           vkCode == keyWheelDown.load();
}

// ==================== 记录按键状态 ====================
// 由钩子线程调用，把"某个物理键按下/松开"翻译成"某个功能触发/停止"。
// 名字里的 keyE / keyD 是老命名，实际含义是"上移功能触发"/"下移功能触发"。
// 按下时顺手记录时间戳，供看门狗判断这个键的 keyup 是不是丢了。
void SetKeyState(DWORD vkCode, bool down) {
    const DWORD now = down ? GetTickCount() : 0;

    if (vkCode == keyUp.load()) {
        keyE = down;
        if (down) g_lastKeyDown[SLOT_UP] = now;
    } else if (vkCode == keyDown.load()) {
        keyD = down;
        if (down) g_lastKeyDown[SLOT_DOWN] = now;
    } else if (vkCode == keyLeft.load()) {
        keyS = down;
        if (down) g_lastKeyDown[SLOT_LEFT] = now;
    } else if (vkCode == keyRight.load()) {
        keyF = down;
        if (down) g_lastKeyDown[SLOT_RIGHT] = now;
    } else if (vkCode == keyLClick.load()) {
        keyJ = down;
        if (down) g_lastKeyDown[SLOT_LCLICK] = now;
    } else if (vkCode == keyRClick.load()) {
        keyK = down;
        if (down) g_lastKeyDown[SLOT_RCLICK] = now;
    } else if (vkCode == keyWheelUp.load()) {
        keyO = down;
        if (down) g_lastKeyDown[SLOT_WHEELUP] = now;
    } else if (vkCode == keyWheelDown.load()) {
        keyL = down;
        if (down) g_lastKeyDown[SLOT_WHEELDOWN] = now;
    } else if (vkCode == keyMClick.load()) {
        keyM = down;
        if (down) g_lastKeyDown[SLOT_MCLICK] = now;
    }
    // 加速键只被"加速倍率"逻辑读取，键位可配置（默认 Shift）。
    else if (vkCode == keyBoost.load()) {
        keyBoostDown = down;
        if (down) g_lastKeyDown[SLOT_BOOST] = now;
    }

    // 滚轮加速键**单独一个 if**，不能并入上面的 else-if 链：
    // 默认两个加速键都是 Shift，写成 else-if 的话 Shift 会先命中"加速键"分支，
    // 滚轮加速就永远不会被置位 —— 默认配置下滚轮加速直接失效。
    // 两者相等时这里会各记一份状态，效果正是"一个键同时加速移动和滚轮"。
    if (vkCode == keyWheelBoost.load()) {
        wheelBoostDown = down;
        if (down) g_lastKeyDown[SLOT_WHEELBOOST] = now;
    }
}

// ==================== 键盘钩子 ====================
// WH_KEYBOARD_LL 的回调。注意：
//   1. 该回调在"安装钩子的线程"上被调用（也就是主线程的消息泵里），
//      因此这里不能做耗时操作；
//   2. 返回非 0 表示吞掉该事件，返回 CallNextHookEx(...) 结果表示继续传递。
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        // 把左右物理键码归一化为通用键码
        // 因为 GetAsyncKeyState(VK_SHIFT) 只认通用码；如果不归一，
        // 用户按右 Shift 时组合键判定会失效。
        DWORD vk = p->vkCode;
        switch (vk) {
            case VK_LCONTROL: case VK_RCONTROL: vk = VK_CONTROL; break;
            case VK_LSHIFT:   case VK_RSHIFT:   vk = VK_SHIFT;   break;
            case VK_LMENU:    case VK_RMENU:    vk = VK_MENU;    break;
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // 捕获模式：把按键交给主线程处理，本轮直接吞掉
            // 用 PostMessage 而不是 SetDlgItemText 等，是因为：
            //   1) 我们走在钩子回调里，此时别的线程正在等待输入；
            //   2) 修改 UI 只能在主线程做，钩子本身也在主线程，但
            //      为了不打断钩子链条，还是把"改映射"推迟到消息泵里更稳。
            int btn = waitingButtonId.load();
            if (btn != 0){
                PostMessageA(g_hMainWnd, WM_BIND_KEY, (WPARAM)vk, (LPARAM)btn);
                return 1;
            }

            // 隐藏彩蛋：窗口在前台时 Esc+0 重置
            // 用 GetForegroundWindow 判断，避免游戏里误触。
            if (vk == '0' && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && GetForegroundWindow() == g_hMainWnd) {
                PostMessageA(g_hMainWnd, WM_RESET, 0, 0);
            }

            SetKeyState(vk, true);
            // 先记 CapsLock 的按下状态，再决定要不要吞 ——
            // 一旦被吞，系统就不会再更新它，只能靠这里自己维护。
            if (vk == VK_CAPITAL) {
                capsDown = true;
                g_lastKeyDown[SLOT_CAPS] = GetTickCount();   // 喂看门狗
            }
            if (ShouldIntercept(vk)) return 1;
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            // 松开也要同步状态 + 视情况拦截，否则被吞的键会一直"按着"。
            SetKeyState(vk, false);
            if (vk == VK_CAPITAL) capsDown = false;
            if (ShouldIntercept(vk)) return 1;
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// ==================== 鼠标操作 ====================
// 以下 Down/Up 都用 exchange 做"状态转换"，只有真正变化时才注入事件。
// 若不这样，控制线程按 tick 一轮，按住左键时每秒会发约 200 个 LEFT DOWN，
// 系统会把这些当作连击/重复拖拽，行为完全错乱。
void MouseLeftDown() {
    if (mouseLeftDown.exchange(true)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseLeftUp() {
    if (!mouseLeftDown.exchange(false)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseRightDown() {
    if (mouseRightDown.exchange(true)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseRightUp() {
    if (!mouseRightDown.exchange(false)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseMiddleDown() {
    if (mouseMiddleDown.exchange(true)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseMiddleUp() {
    if (!mouseMiddleDown.exchange(false)) return;
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
    SendInput(1, &input, sizeof(INPUT));
}

// 一次滚轮事件要发出的"档位"数（1 档 = 标准滚轮一格 = WHEEL_DELTA）。
// 正数向上滚、负数向下滚。调用方负责把数量压到 MAX_NOTCH_PER_EVENT 以内。
//
// 为什么要拆成多条 INPUT：一条 WM_MOUSEWHEEL 的累计量在消息里是
// 16 位有符号数（GET_WHEEL_DELTA_WPARAM 展开就是 (short)HIWORD(wParam)），
// 上限 32767。超过这个数会被当成负数 —— 表现为"方向反了、幅度还不对"。
// 所以按 NOTCH_PER_INPUT(200 档 = 24000) 切块，整组用一次 SendInput 发出去。
void MouseWheelNotches(int notches) {
    if (notches == 0) return;

    const int dir = (notches > 0) ? 1 : -1;
    int left = (notches > 0) ? notches : -notches;

    // 数组长度按"最坏情况"给：3000 / 200 + 1 = 16。
    // 必须整体清零——INPUT 里有 dx/dy/dwExtraInfo 等字段，
    // 留着栈上的垃圾值可能让 SendInput 拒绝整批事件。
    INPUT inputs[MAX_NOTCH_PER_EVENT / NOTCH_PER_INPUT + 1] = {};

    int n = 0;
    const int cap = (int)(sizeof(inputs) / sizeof(inputs[0]));
    while (left > 0 && n < cap) {
        int chunk = (left > NOTCH_PER_INPUT) ? NOTCH_PER_INPUT : left;
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputs[n].mi.mouseData = (DWORD)(dir * chunk * WHEEL_DELTA);
        left -= chunk;
        n++;
    }

    if (n > 0) SendInput((UINT)n, inputs, sizeof(INPUT));
}

// 按 L3 速率上限折算：发出 notches 档之后要冷却多少毫秒。
// 向上取整 —— 宁可少发一点，也别超发。
static int NotchCooldownMs(int notches) {
    if (notches <= 0) return 0;
    return (notches * 1000 + MAX_NOTCH_PER_SECOND - 1) / MAX_NOTCH_PER_SECOND;
}

// 使用"绝对坐标"模式：0~65535 映射到整个虚拟桌面（多显示器时包含所有屏）。
// 旧实现只按主屏 SM_CXSCREEN 换算、且没加 MOUSEEVENTF_VIRTUALDESK，
// 光标在副屏或主屏左侧/上方（负坐标）时会偏。
void MouseMoveTo(int x, int y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return;   // 拿不到桌面尺寸就放弃，别算出除零

    // 夹到虚拟桌面范围内，避免越界坐标被系统截断到边上。
    if (x < vx) x = vx;
    if (x > vx + vw - 1) x = vx + vw - 1;
    if (y < vy) y = vy;
    if (y > vy + vh - 1) y = vy + vh - 1;

    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)(((LONGLONG)(x - vx) * 65535LL) / (vw - 1));
    input.mi.dy = (LONG)(((LONGLONG)(y - vy) * 65535LL) / (vh - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &input, sizeof(INPUT));
}


// ==================== 开关检测 ====================

// 判断指定键是否按下（左右分键合并判断）
// 组合键成员一般是"放行"的，所以直接读 GetAsyncKeyState 最可靠；
// 但 CapsLock 是个例外：它被拦截后系统收不到，只能读钩子里自维护的 capsDown。
bool IsKeyDown(int vk) {
    if (vk == VK_CAPITAL) return capsDown.load();

    switch (vk) {
        case VK_SHIFT:   return ((GetAsyncKeyState(VK_LSHIFT)   | GetAsyncKeyState(VK_RSHIFT))   & 0x8000) != 0;
        case VK_CONTROL: return ((GetAsyncKeyState(VK_LCONTROL) | GetAsyncKeyState(VK_RCONTROL)) & 0x8000) != 0;
        case VK_MENU:    return ((GetAsyncKeyState(VK_LMENU)    | GetAsyncKeyState(VK_RMENU))    & 0x8000) != 0;
        default:         return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
}

// 判定组合键是否"同时全部按下"。
// 空位（vk==0）视为"不要求"。全空则返回 false，避免误触发。
bool IsSwitchPressed() {
    int k1 = switchKey1.load();
    int k2 = switchKey2.load();
    int k3 = switchKey3.load();
    int k4 = switchKey4.load();

    if (k1 == 0 && k2 == 0 && k3 == 0 && k4 == 0) return false;

    // 空位直接置真，等价于"条件满足、无需检测"。
    bool p1 = (k1 == 0) || IsKeyDown(k1);
    bool p2 = (k2 == 0) || IsKeyDown(k2);
    bool p3 = (k3 == 0) || IsKeyDown(k3);
    bool p4 = (k4 == 0) || IsKeyDown(k4);

    return p1 && p2 && p3 && p4;
}

// ==================== 高精度节拍器 ====================
// 需求：轮询频率略高于显示器刷新率（144/165Hz），但不想为它牺牲功耗。
// 做法：用高精度可等待定时器。等待期间线程真正阻塞（CPU 占用 0%），
// 也不像 timeBeginPeriod 那样把全系统的定时器精度拉高。
// 老系统（Win10 1803 之前）创建失败时退回普通定时器，
// 由调用方补 timeBeginPeriod(1) 兜底。
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

class TickTimer {
public:
    TickTimer() {
        h = CreateWaitableTimerExA(NULL, NULL,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (h == NULL) {
            h = CreateWaitableTimerA(NULL, FALSE, NULL);
            coarse = true;
        }
    }

    ~TickTimer() {
        if (h != NULL) CloseHandle(h);
    }

    // 至少等待 ms 毫秒。定时器用"相对时间"（负值），单位 100ns。
    void Wait(int ms) {
        if (h != NULL) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)ms * 10000LL;
            if (SetWaitableTimer(h, &due, 0, NULL, NULL, FALSE)) {
                // 加一点兜底超时，避免定时器异常时线程永久卡住。
                WaitForSingleObject(h, (DWORD)ms + 50);
                return;
            }
        }
        std::this_thread::sleep_for(milliseconds(ms));
    }

    bool coarse = false;   // true = 退回了普通定时器，需要 timeBeginPeriod 兜底

private:
    HANDLE h = NULL;
};

// 更新标题栏的启用/禁用提示。
// 控制线程里直接调 SetWindowTextA 是安全的：窗口文本的修改会被转发到
// 创建窗口的线程执行，不会跨线程碰 UI 对象。
void UpdateTitleState() {
    HWND hwnd = FindWindowA("MyClass", NULL);
    if (hwnd != NULL) {
        std::string title = "光标控制 ";
        title += isEnabled.load() ? "[启用]" : "[禁用]";
        SetWindowTextA(hwnd, title.c_str());
    }
}

// 这些键按住时不会产生自动重复，不能靠"多久没收到 keydown"来判断松开。
bool IsNonRepeatingKey(int vk) {
    switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LWIN: case VK_RWIN:
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        case VK_SNAPSHOT: case VK_PAUSE:
            return true;
        default:
            return false;
    }
}

// 判断某个"标记为按下"的键，实际上是不是已经松开了。
//   - 没被钩子拦截的键：系统状态是真实的，直接问 GetAsyncKeyState。
//   - 被拦截的键：系统里读不到，只能靠"最后一次 keydown"的时间戳。
//     但时间戳本身不可靠：
//       · 同时按住多个键时，系统只重复"最后按下的那个"，先按的会停止重复；
//       · 最后按下的若是 Shift 这类修饰键，根本不会重复。
//     所以必须再叠加"注入是否被挡"：只有确实推不动光标时，才认为
//     那些超时没收到 keydown 的键真的松开了。
bool KeyLooksReleased(int vk, DWORD lastDown, bool injectionBlocked) {
    if (!ShouldIntercept((DWORD)vk)) {
        return (GetAsyncKeyState(vk) & 0x8000) == 0;
    }
    if (!injectionBlocked) return false;   // 光标还能动，说明键还按着

    const DWORD limit = IsNonRepeatingKey(vk) ? 5000 : 1200;
    return (GetTickCount() - lastDown) > limit;
}

// ==================== 按键看门狗 ====================
// 兜底：如果某个键的 keyup 丢了（切到任务管理器这类高权限窗口、锁屏、
// 钩子被系统摘掉），标记会一直停在"按下"，光标就会自己走。
// 每个 tick 在驱动鼠标之前先跑一遍，把这类残留状态清掉。
// injectionBlocked：最近几帧"要求移动但光标没动"，说明注入被系统挡了。
void WatchdogKeyStates(bool injectionBlocked) {
    if (keyE.load() && KeyLooksReleased(keyUp.load(), g_lastKeyDown[SLOT_UP].load(), injectionBlocked))
        keyE = false;
    if (keyD.load() && KeyLooksReleased(keyDown.load(), g_lastKeyDown[SLOT_DOWN].load(), injectionBlocked))
        keyD = false;
    if (keyS.load() && KeyLooksReleased(keyLeft.load(), g_lastKeyDown[SLOT_LEFT].load(), injectionBlocked))
        keyS = false;
    if (keyF.load() && KeyLooksReleased(keyRight.load(), g_lastKeyDown[SLOT_RIGHT].load(), injectionBlocked))
        keyF = false;
    if (keyJ.load() && KeyLooksReleased(keyLClick.load(), g_lastKeyDown[SLOT_LCLICK].load(), injectionBlocked))
        keyJ = false;
    if (keyK.load() && KeyLooksReleased(keyRClick.load(), g_lastKeyDown[SLOT_RCLICK].load(), injectionBlocked))
        keyK = false;
    if (keyM.load() && KeyLooksReleased(keyMClick.load(), g_lastKeyDown[SLOT_MCLICK].load(), injectionBlocked))
        keyM = false;
    if (keyO.load() && KeyLooksReleased(keyWheelUp.load(), g_lastKeyDown[SLOT_WHEELUP].load(), injectionBlocked))
        keyO = false;
    if (keyL.load() && KeyLooksReleased(keyWheelDown.load(), g_lastKeyDown[SLOT_WHEELDOWN].load(), injectionBlocked))
        keyL = false;
    if (keyBoostDown.load() && KeyLooksReleased(keyBoost.load(), g_lastKeyDown[SLOT_BOOST].load(), injectionBlocked))
        keyBoostDown = false;
    if (wheelBoostDown.load() &&
        KeyLooksReleased(keyWheelBoost.load(), g_lastKeyDown[SLOT_WHEELBOOST].load(), injectionBlocked))
        wheelBoostDown = false;
    if (capsDown.load() && KeyLooksReleased(VK_CAPITAL, g_lastKeyDown[SLOT_CAPS].load(), injectionBlocked))
        capsDown = false;
}

// ==================== 鼠标控制线程 ====================
// 按 TICK_MS 节拍轮询原子标志，驱动鼠标；所有输出都是"事件注入"或"位置设置"。
void MouseControlThread() {
    bool lastToggleState = false;   // 上一轮的组合键状态，用于"边沿"识别
    auto lastWheelTime = steady_clock::now();   // 滚轮节流（限速）的时间戳

    // 滚轮的总速率闸门（L3）：下一次允许发滚轮的最早时刻。
    // 和上面的 lastWheelTime 分开维护：一个管"用户设定的最小间隔"，
    // 一个管"档位总量换算出来的最快时刻"。混用会让
    // "间隔 80ms + 冷却 500ms" 变成实际 580ms，参数含义就乱了。
    auto wheelBudgetUntil = steady_clock::now();

    // 亚像素余量：速度可以是小数，但 SetCursorPos 只收整数。
    // 每帧把带小数的位移累加进来，只走整数部分，小数留到下一帧。
    double accX = 0.0, accY = 0.0;

    // 加速状态：当前速度（px/tick）和上一帧的方向，用来识别"拐弯"。
    double moveSpeedNow = 0.0;
    int lastDirX = 0, lastDirY = 0;

    // 上一帧的"有效初速"。它一变（按下/松开加速键、或改了参数），
    // 就把当前速度直接切过去，避免靠加速度慢慢爬。
    double lastInit = 0.0;

    // 连续多少帧"要求移动但光标没动"。用来判断注入是不是被系统挡住了。
    int blockedTicks = 0;

    TickTimer timer;
    if (timer.coarse) timeBeginPeriod(1);   // 老系统：普通定时器精度不够

    while (isRunning) {
        // 先跑一次看门狗：把因漏掉 keyup 而卡住的状态复位，
        // 否则下面会拿着旧状态继续移动光标。
        // 传入"注入是否被挡"——只有确实推不动光标时才清状态，
        // 避免把"按住多个键、只有最后一个在重复"误判成松开。
        WatchdogKeyStates(blockedTicks >= 3);

        // ---------- 组合键 / 总开关 ----------
        bool pressed = IsSwitchPressed();
        bool holdMode = (switchHoldMode.load() != 0);

        if (holdMode) {
            // 按住模式：总开关直接跟随组合键——按下即开，松开即关。
            // 用 pressed != isEnabled 判断，只在状态真翻转时重放鼠标和改标题，
            // 不会每帧都刷一次。
            if (pressed != isEnabled.load()) {
                isEnabled = pressed;
                if (pressed) {
                    // 启用瞬间补发一次"绝对坐标鼠标移动"，兼容只监听 MOUSEMOVE 的全屏程序
                    POINT pt;
                    GetCursorPos(&pt);
                    MouseMoveTo(pt.x, pt.y);
                }
                UpdateTitleState();
            }
        } else if (pressed && !lastToggleState) {
            // 切换模式：只在"松开→按下"这一刻翻转一次，按住不放不会反复切。
            isEnabled = !isEnabled;

            if (isEnabled) {
                // 启用时重放一次"绝对坐标鼠标移动"。
                // 位置没变，但某些全屏/DirectInput 程序只监听 MOUSEMOVE 事件，
                // 补发一次让它们知道"光标现在在这里"，避免坐标错乱。
                POINT pt;
                GetCursorPos(&pt);
                MouseMoveTo(pt.x, pt.y);
            }

            UpdateTitleState();
        }
        lastToggleState = pressed;

        // 切换模式下，组合键按住期间跳过映射，避免切换瞬间被组合键成员误触发，
        // 同时主动松开三键，防止"按住组合键但鼠标还处于按下"这种矛盾状态。
        // 按住模式不能跳：那段逻辑正是"按住才生效"要走的路径。
        if (!holdMode && pressed)
        {
            MouseLeftUp();
            MouseRightUp();
            MouseMiddleUp();
            blockedTicks = 0;   // 本帧不动，不参与"被挡"判定
            timer.Wait(TICK_MS.load());
            continue;
        }

        if (!isEnabled) {
            // 总开关禁用时同样要做"复位"，否则禁用后鼠标仍可能处于按下态。
            MouseLeftUp();
            MouseRightUp();
            MouseMiddleUp();
            blockedTicks = 0;
            timer.Wait(TICK_MS.load());
            continue;
        }

        // ---------- 读取本帧的所有功能状态 ----------
        bool e = keyE.load();
        bool s = keyS.load();
        bool d = keyD.load();
        bool f = keyF.load();
        bool j = keyJ.load();
        bool k = keyK.load();
        bool o = keyO.load();
        bool l = keyL.load();
        bool m = keyM.load();

        // ---------- 速度（带加速度） ----------
        // 用 SetCursorPos 而不是 SendInput：SetCursorPos 是精确设置位置，
        // 在 Windows 下不会被 Clipping 等机制干扰，对"移动"意图更直接。
        //
        // 速度模型（四个参数都可带小数）：
        //   初始速度 → 起步速度，符号决定方向（负 = 反向，0 = 静止起步）
        //   加速度   → 每 tick 的变化量；负值就是减速，减到区间下界停住
        //   上限     → 比初速大时是天花板，比初速小时当下限用（速度不低于它）
        //   倍率     → 按住加速键时初速/加速度同乘、上限乘它的绝对值；
        //              负值会翻转方向，0 = 刹车
        // 起步 / 拐弯回到初速；负加速度不会把速度带过 0 去调头。
        // 位移可能带小数，但 SetCursorPos 只收整数，所以用 accX/accY 存余量。
        int dirX = (f ? 1 : 0) - (s ? 1 : 0);   // 右正、左负
        int dirY = (d ? 1 : 0) - (e ? 1 : 0);   // 下正、上负

        if (dirX != 0 || dirY != 0) {
            double mul  = keyBoostDown.load() ? SPEED_MULTIPLIER.load() : 1.0;
            double init = MOVE_SPEED.load()     * mul;
            // 上限是"大小"，倍率取绝对值：mul 为负只翻转方向，不该把上限也变负
            double cap  = MOVE_MAX_SPEED.load() * fabs(mul);
            double acc  = MOVE_ACCEL.load()     * mul;

            // 上限有两种角色：
            //   上限 ≥ |初速| → 天花板，速度夹在 [0, 上限]
            //   上限 <  |初速| → 地板，  速度夹在 [上限, |初速|]
            // 这样"上限填得比初速小"时不会把初速压掉，手感退化回旧规则。
            double lo = 0.0, hi = cap;
            if (cap < fabs(init)) {
                lo = cap;
                hi = fabs(init);
            }

            // 有效初速变了（按下/松开加速键、改了参数）→ 直接切到新初速。
            // 不切的话，负倍率翻转方向时会被夹断清零、松开刹车后也爬不起来。
            if (init != lastInit) {
                moveSpeedNow = init;
                lastInit = init;
            }

            // 拐弯判定：只要方向和上一帧不同就算 —— 包括 180° 反向、90° 转向，
            // 以及斜走时多按/少按一个方向键。
            // 上一帧没在动（lastDir 全是 0）时不算拐弯，那是"起步"。
            bool turned = (lastDirX != 0 || lastDirY != 0) &&
                          (dirX != lastDirX || dirY != lastDirY);
            bool justStarted = (lastDirX == 0 && lastDirY == 0);

            if (justStarted || turned) {
                moveSpeedNow = init;        // 起步 / 拐弯：回到初速
            } else {
                moveSpeedNow += acc;

                // 按初速的方向把速度夹进 [lo, hi]。
                // 区间本身就挡住了"调头"，所以不再需要单独的零穿越判断。
                if (init >= 0) {
                    if (moveSpeedNow < lo) moveSpeedNow = lo;
                    if (moveSpeedNow > hi) moveSpeedNow = hi;
                } else {
                    if (moveSpeedNow > -lo) moveSpeedNow = -lo;
                    if (moveSpeedNow < -hi) moveSpeedNow = -hi;
                }
            }

            if (e) accY -= moveSpeedNow;
            if (d) accY += moveSpeedNow;
            if (s) accX -= moveSpeedNow;
            if (f) accX += moveSpeedNow;

            POINT pt;
            GetCursorPos(&pt);
            int dx = (int)accX;  accX -= dx;
            int dy = (int)accY;  accY -= dy;

            SetCursorPos(pt.x + dx, pt.y + dy);

            // 注入是否生效：要求移动了、但位置没变，说明被系统挡住了
            // （高权限窗口在前台、安全桌面等）。连续几帧这样就判定"被挡"。
            if (dx != 0 || dy != 0) {
                POINT after;
                GetCursorPos(&after);
                if (after.x == pt.x && after.y == pt.y) blockedTicks++;
                else blockedTicks = 0;
            }
        } else {
            blockedTicks = 0;   // 没在移动，无从判断
        }

        lastDirX = dirX;
        lastDirY = dirY;
        if (dirX == 0 && dirY == 0) moveSpeedNow = 0.0;   // 停住：下次重新起步

        // ---------- 三键点击 ----------
        // Down 幂等，Up 幂等，因此可以无脑每帧调用。
        if (j) MouseLeftDown(); else MouseLeftUp();
        if (k) MouseRightDown(); else MouseRightUp();
        if (m) MouseMiddleDown(); else MouseMiddleUp();

        // ---------- 滚轮 ----------
        // 三道闸门一起判断：
        //   userOk —— 距上次事件至少 WHEEL_INTERVAL 毫秒（用户设定）
        //   rateOk —— 上次发出的档位已经"冷却"完（L3 总速率上限）
        // 两者取交集，各自含义独立。
        auto now = steady_clock::now();
        auto elapsed = duration_cast<milliseconds>(now - lastWheelTime).count();
        const bool userOk = elapsed >= WHEEL_INTERVAL.load();
        const bool rateOk = now >= wheelBudgetUntil;

        if ((o || l) && userOk && rateOk) {
            int speed = WHEEL_SPEED.load();
            // 只认滚轮加速键，不认"加速键"：两者默认都是 Shift（手感不变），
            // 但把滚轮加速键改到别的键上之后，Shift 就只加速移动、不再影响滚轮。
            if (wheelBoostDown.load()) speed *= WHEEL_SPEED_MULTIPLIER.load();

            // L2：单次事件的档位上限。速度 260 × 加速 260 = 67600 档，
            // 一次交出去等于让目标程序在那一帧滚 67600 行。
            if (speed > MAX_NOTCH_PER_EVENT) speed = MAX_NOTCH_PER_EVENT;

            // 上下滚同时按住时是两次事件，冷却要按两次的量算，
            // 否则"两个键一起按"能绕过 L3 的速率上限。
            int events = (o ? 1 : 0) + (l ? 1 : 0);

            if (o) MouseWheelNotches(speed);
            if (l) MouseWheelNotches(-speed);

            lastWheelTime = now;
            // L3：按实际发出的档位把总速率闸门往后推。
            wheelBudgetUntil = now + milliseconds(NotchCooldownMs(speed * events));
        }

        // ---------- 节拍等待 ----------
        // TICK_MS 略高于 144/165Hz 的刷新周期；线程在这里阻塞，不占 CPU。
        timer.Wait(TICK_MS.load());
    }

    if (timer.coarse) timeEndPeriod(1);

    // 线程退出前的兜底复位：防止因为关程序导致鼠标"粘住"。
    MouseLeftUp();
    MouseRightUp();
    MouseMiddleUp();
}

// ==================== 托盘图标 ====================
// 使用简化的 NIM_ADD 参数；实际上系统会对图标资源要求一个 HICON，
// 这里直接用 IDI_APPLICATION 保证任何一个 Windows 都显示正常。
void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP + 1;   // 托盘事件一律以这个消息通知窗口
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strcpy_s(g_nid.szTip, "光标控制");
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

// 删除托盘图标；即使没添加过（系统会忽略）也不报错。
void RemoveTrayIcon() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

// ==================== 重置 ====================
// 只把草稿恢复成出厂默认并刷新界面，**不**推进内存、**不**写盘。
// 想让它生效得再点"应用"，想让它持久化得点"保存" —— 和其他修改走同一套规则。
// 例外：Esc+0 快捷重置（WM_RESET）调完这里之后会立刻 ApplyDraft，
// 因为它的用途是"快速恢复"，详见该 case 里的注释。
void ResetDraftAndUI(HWND hwnd) {
    DraftDefaults();
    RefreshUIFromDraft(hwnd);
    g_dirty = true;   // 界面已经变了，属于"未保存的修改"
}

// ==================== 开机自启动 ====================
// 原理：往 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 写一个字符串值，
// 登录时 Windows 会自动执行它。选 HKCU 而不是 HKLM，是因为前者不需要管理员权限，
// 也不会影响其它用户；代价是只对当前用户生效。
//
// 值名固定为 "KeyboardCursor"，内容是 exe 的绝对路径，而且必须用双引号包起来
// —— 否则路径带空格时（如 C:\Program Files\...）开机会被拆成多段，启动失败。
//
// 注意：这里只做"普通权限的开机启动"。如果目标程序以管理员运行，
// Run 键启动的进程仍是普通权限，钩不住管理员窗口（受 UIPI 限制，
// 既收不到键也注入不进去），那种情况得用任务计划程序的"最高权限"运行。
static const char* kRunKey   = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* kRunValue = "KeyboardCursor";

// 任务管理器 / "设置 → 应用 → 启动"里的启用状态存在这里。
// 它们并不删除 Run 值，而是往这个键写一条 12 字节记录：
// 首字节最低位为 1 表示"已禁用"，为 0 表示"已启用"。
static const char* kApprovedKey =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

// 查系统层面有没有把这个自启动项禁用。
// 没有这条记录时默认是启用（用户从没在任务管理器里关过）。
bool IsAutoStartApproved() {
    HKEY hKey = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return true;   // 键都不存在 = 从没被禁用过

    BYTE data[16] = {};
    DWORD size = sizeof(data);
    DWORD type = 0;
    LONG ret = RegQueryValueExA(hKey, kRunValue, NULL, &type, data, &size);
    RegCloseKey(hKey);

    if (ret != ERROR_SUCCESS) return true;   // 没有这一项 = 启用
    return (data[0] & 1) == 0;               // 最低位 0 = 启用，1 = 禁用
}

// 查注册表里是否已有我们的自启动项。
// 只关心"值存在且非空"，不比对路径内容 —— 即使 exe 被移动过，
// 也认为开关是打开的，实际路径会在 SetAutoStart(true) 时被刷新。
// 另外还要看系统有没有在任务管理器里把它禁用：Run 值在、但被禁用，
// 等于没启用，勾选框必须显示成未勾选。
bool IsAutoStartEnabled() {
    HKEY hKey = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;   // 键都打不开，当作未设置

    DWORD type = 0;
    DWORD size = 0;
    LONG ret = RegQueryValueExA(hKey, kRunValue, NULL, &type, NULL, &size);
    RegCloseKey(hKey);

    if (ret != ERROR_SUCCESS || size == 0) return false;
    return IsAutoStartApproved();   // 还要没被系统禁用
}

// 打开 / 关闭开机自启动。
// 返回 false 表示注册表操作失败，调用方应当把复选框状态还原并提示用户，
// 避免出现"界面勾着、实际没生效"这种假象。
bool SetAutoStart(bool enable) {
    HKEY hKey = NULL;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return false;

    LONG ret;
    if (enable) {
        // 用绝对路径：开机时的工作目录是 system32，相对路径必然找不到 exe。
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);

        // 加引号，防止路径里的空格把命令行拆断。
        std::string quoted = std::string("\"") + exePath + "\"";
        ret = RegSetValueExA(hKey, kRunValue, 0, REG_SZ,
                             (const BYTE*)quoted.c_str(),
                             (DWORD)(quoted.size() + 1));   // 含结尾的 '\0'
    } else {
        ret = RegDeleteValueA(hKey, kRunValue);
        // 本来就没设置过，也视为"关闭成功"。
        if (ret == ERROR_FILE_NOT_FOUND) ret = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);
    if (ret != ERROR_SUCCESS) return false;

    // 同步系统那边的"启用/禁用"记录：删掉这条记录等于恢复"启用"。
    // 不删的话，用户之前在任务管理器里禁用过，Run 值加回来了也照样不启动，
    // 界面却显示已勾选 —— 这正是最常见的"看着开了其实没开"。
    HKEY hApproved = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &hApproved) == ERROR_SUCCESS) {
        RegDeleteValueA(hApproved, kRunValue);   // 本来就不存在也无所谓
        RegCloseKey(hApproved);
    }
    return true;
}

// 只读：把系统里的真实状态同步到勾选框。
// 用在窗口创建、以及每次窗口重新激活时 —— 用户可能刚在任务管理器或
// "设置 → 应用 → 启动"里改过，勾选框要跟着变回来。
// CheckDlgButton 只发 BM_SETCHECK，不会回发 WM_COMMAND，不会误触发写入。
void RefreshAutoStartCheck(HWND hwnd) {
    CheckDlgButton(hwnd, ID_CHECK_AUTOSTART,
                   IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
}

// ==================== 保存 ====================
// 保存 = 草稿 → 内存（生效） → 写所选存档 → 镜像写 save0。
// save0 是"下次启动要恢复的状态"：保存、应用、加载都会更新它，
// 退出流程不再隐式写盘。
// 返回 false 表示没写成，调用方不能清脏标记。
bool DoSave(HWND hwnd, const std::string& slot) {
    if (!g_saveWritable) return false;

    ReadEditsIntoDraft(hwnd);   // 编辑框里可能还有没点"应用"的数字
    ApplyDraft();

    bool ok = true;
    if (!slot.empty()) ok = SaveToFile(slot);   // 所选存档
    if (ok) ok = SaveToFile("save0");           // 镜像：启动时读它
    if (ok) g_dirty = false;                    // 只有真写成功了才清脏标记

    return ok;
}

// ==================== 统一退出入口 ====================
// 窗口关闭（非托盘模式）和托盘右键"退出"都走这里。
// 有未保存的修改时问一句：是=保存后退出，否=直接退出，取消=回到窗口。
// 两种例外直接退：系统正在关机/注销、以及只读模式（根本存不了，问了是骗人）。
void QuitApp(HWND hwnd) {
    if (!g_shuttingDown && g_dirty && g_saveWritable) {
        int r = MessageBoxA(hwnd,
            "设置已修改但还没保存，退出前保存吗？",
            "退出", MB_YESNOCANCEL | MB_ICONQUESTION);

        if (r == IDCANCEL) return;          // 取消：留在窗口继续编辑
        if (r == IDYES) {
            std::string slot = GetSelectedSlot(hwnd);
            if (!DoSave(hwnd, slot)) {
                // 存失败就别退，否则改动直接丢了
                MessageBoxA(hwnd, "保存失败，已取消退出。请检查存档目录后重试。",
                            "错误", MB_OK | MB_ICONERROR);
                return;
            }
        }
        // IDNO：不保存，直接往下走
    }
    DestroyWindow(hwnd);
}

// ==================== 数字输入过滤 ====================
// 四个小数框只需要"数字 + 一个小数点"，带符号的还允许开头的负号。
// 不加过滤的话字母、汉字都能打进去，虽然应用时会被判非法回退，
// 但输入过程很别扭。做法：子类化编辑框，插入的字符统一走清洗函数；
// 粘贴走的是 WM_PASTE，单独接管。
static WNDPROC g_oldEditProc = NULL;   // 编辑框原始窗口过程（四个框共用同一个）

// 该框是否允许负号。创建时用 SetProp 打标记，避免给每个框写一份窗口过程。
static bool EditAllowsNegative(HWND edit) {
    return GetPropA(edit, "numedit_signed") != NULL;
}

// 把任意文本清洗成合法数字串：
//   负号：只保留开头的一个（前面不能已经有数字）
//   小数点：只保留第一个
//   其它字符：全部丢弃
static std::string SanitizeNumber(const char* src, bool allowNegative) {
    std::string out;
    bool hasDot = false, hasNeg = false;

    for (const char* p = src; p && *p; p++) {
        const char c = *p;
        if (c >= '0' && c <= '9') {
            out += c;
        } else if (c == '.' && !hasDot) {
            out += c;
            hasDot = true;
        } else if (c == '-' && allowNegative && !hasNeg && out.empty()) {
            out += c;
            hasNeg = true;
        }
    }
    return out;
}

// 清洗编辑框当前内容；有变化就把光标放回末尾。只给"粘贴"用。
static void SanitizeEditText(HWND edit) {
    char buf[128] = {};
    GetWindowTextA(edit, buf, sizeof(buf));

    std::string clean = SanitizeNumber(buf, EditAllowsNegative(edit));
    if (clean == buf) return;

    SetWindowTextA(edit, clean.c_str());
    const int len = (int)clean.size();
    SendMessageA(edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

// 判断某个字符能不能插进去：先算出"插入后会变成什么"，
// 再要求这段文本本身就是合法的数字串。
// 这样就不需要在字符已经插进去之后重写文本框——重写会把光标顶到末尾，
// 而且在 WM_CHAR 里改文本容易把编辑框的内部插入状态搞乱。
static bool IsNumberCharAllowed(HWND edit, wchar_t ch) {
    if (ch >= '0' && ch <= '9') return true;        // 数字永远可以
    if (ch != '.' && ch != '-') return false;       // 其它字符一律拒绝
    if (ch == '-' && !EditAllowsNegative(edit)) return false;

    char cur[128] = {};
    GetWindowTextA(edit, cur, sizeof(cur));

    // 有选区时，插入会替换选区，所以要按"替换后"的文本判断
    DWORD selStart = 0, selEnd = 0;
    SendMessageA(edit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);

    std::string next = std::string(cur).substr(0, selStart);
    next += (char)ch;
    next += std::string(cur).substr(selEnd);

    return SanitizeNumber(next.c_str(), EditAllowsNegative(edit)) == next;
}

// 编辑框子类过程
static LRESULT CALLBACK NumberEditProc(HWND edit, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR) {
        // 控制字符（退格、Tab、回车）直接放行；
        // 可打印字符先判断能不能插，不能插就直接吞掉，文本一个字都不改。
        if ((wchar_t)wParam < 32)
            return CallWindowProcA(g_oldEditProc, edit, uMsg, wParam, lParam);
        if (!IsNumberCharAllowed(edit, (wchar_t)wParam))
            return 0;
        return CallWindowProcA(g_oldEditProc, edit, uMsg, wParam, lParam);
    } else if (uMsg == WM_PASTE) {
        // 取剪贴板文本，清洗后替换选区，避免整段非法字符被粘进来
        std::string raw;
        if (OpenClipboard(edit)) {
            HANDLE h = GetClipboardData(CF_TEXT);
            if (h) {
                const char* p = (const char*)GlobalLock(h);
                if (p) raw = p;
                GlobalUnlock(h);
            }
            CloseClipboard();
        }

        std::string clean = SanitizeNumber(raw.c_str(), EditAllowsNegative(edit));
        if (!clean.empty()) {   // 清洗后为空就当作没粘，别把选区删掉
            SendMessageA(edit, EM_REPLACESEL, TRUE, (LPARAM)clean.c_str());
            SanitizeEditText(edit);   // 替换后可能和原有内容拼出两个小数点
        }
        return 0;
    }
    return CallWindowProcA(g_oldEditProc, edit, uMsg, wParam, lParam);
}

// 把四个小数框挂上过滤过程。signedOk 表示该框是否允许负号。
static void AttachNumberFilters(HWND mainWnd) {
    struct { int id; bool signedOk; } fields[] = {
        { ID_EDIT_SPEED,     true  },
        { ID_EDIT_ACCEL,     true  },
        { ID_EDIT_MAXSPEED,  false },   // 上限是"大小"，不接受负号
        { ID_EDIT_SPEED_MUL, true  },
    };

    for (int i = 0; i < 4; i++) {
        HWND edit = GetDlgItem(mainWnd, fields[i].id);
        if (!edit) continue;

        // 四个框同属 EDIT 类，原始过程相同，只记一次即可
        WNDPROC prev = (WNDPROC)SetWindowLongPtrA(edit, GWLP_WNDPROC, (LONG_PTR)NumberEditProc);
        if (!g_oldEditProc) g_oldEditProc = prev;

        if (fields[i].signedOk)
            SetPropA(edit, "numedit_signed", (HANDLE)(INT_PTR)1);
    }
}

// ==================== 窗口过程 ====================
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            // ==================== 布局参数 ====================
            // 改成绝对坐标 + 分组框的排版，不再靠 y 逐行累加：
            //   外距 M=12，左右两栏各 292 宽，栏间距 8，行距 28，控件高 24。
            // 窗口大小在 WinMain 里定死（640×620），这里不处理 WM_SIZE，
            // 所以拉动窗口边缘时控件不会跟着走 —— 这是已知限制，不是 bug。
            g_hMainWnd = hwnd;

            const int M     = 12;             // 外边距
            const int colW  = 292;            // 每栏宽度
            const int col2  = M + colW + 8;   // 右栏起点 = 312
            const int ctlH  = 24;             // 控件高度
            const int wideW = 592;            // 跨两栏的宽度

            // ---------- 顶部：开关 ----------
            // 和下面的"速度""按键映射"一样用分组框，视觉上属于同一套分区，
            // 不再让三个复选框裸放在窗口最上面。
            // 组高 58：标题占 22，复选框在 34 处，底部再留 12 的余量。
            CreateWindowA("BUTTON", "开关",
                WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                M, M, wideW, 58, hwnd, NULL, NULL, NULL);

            const int chkY = M + 22;   // 复选框在组内的 y

            CreateWindowA("BUTTON", "启用按键拦截",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                M + 12, chkY, 150, ctlH, hwnd, (HMENU)ID_CHECK_INTERCEPT, NULL, NULL);
            CheckDlgButton(hwnd, ID_CHECK_INTERCEPT, BST_CHECKED);

            CreateWindowA("BUTTON", "关闭时最小化到托盘",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                M + 174, chkY, 180, ctlH, hwnd, (HMENU)ID_CHECK_TRAY, NULL, NULL);

            // 开机自启动：勾选状态直接读注册表，不经过 ini，
            // 避免"ini 里存的是开、注册表里实际没写"这种两处状态打架。
            CreateWindowA("BUTTON", "开机自启动",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                M + 366, chkY, 130, ctlH, hwnd, (HMENU)ID_CHECK_AUTOSTART, NULL, NULL);
            // 初始状态按"有效状态"来：Run 值存在、且没被系统禁用。
            RefreshAutoStartCheck(hwnd);
            if (IsAutoStartEnabled()) {
                // 只有确实启用时才刷新一次路径（exe 可能被移动过）。
                // 不能在启动时无条件写，否则会把用户在任务管理器里的禁用覆盖掉。
                SetAutoStart(true);
            }

            // ---------- 左栏：速度 ----------
            // BS_GROUPBOX 只是画个框和标题，里面的控件仍然是主窗口的子窗口，
            // 所以坐标必须写绝对位置；先创建分组框，控件才会画在它上面。
            // 组高 256：8 行，最后一行在 302 处（302+22=324），底部留 10 的余量。
            CreateWindowA("BUTTON", "速度",
                WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                M, 78, colW, 256, hwnd, NULL, NULL, NULL);

            const int sx = M + 12;    // 组内左边距
            const int vx = M + 108;   // 数值框的 x

            // 初始速度：刚按下那一帧走多少像素。允许小数，所以不加 ES_NUMBER。
            CreateWindowA("STATIC", "初始速度:",
                WS_VISIBLE | WS_CHILD, sx, 106 + 3, 90, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "1.5",
                WS_VISIBLE | WS_CHILD | WS_BORDER,
                vx, 106, 64, 22, hwnd, (HMENU)ID_EDIT_SPEED, NULL, NULL);

            // 加速度：每个 tick 让速度增加多少（px/tick²）。0 = 不加速。
            CreateWindowA("STATIC", "加速度:",
                WS_VISIBLE | WS_CHILD, sx, 134 + 3, 90, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "0.02",
                WS_VISIBLE | WS_CHILD | WS_BORDER,
                vx, 134, 64, 22, hwnd, (HMENU)ID_EDIT_ACCEL, NULL, NULL);

            // 速度上限：加速到这个值就不再涨；比初速还小时按初速处理。
            CreateWindowA("STATIC", "速度上限:",
                WS_VISIBLE | WS_CHILD, sx, 162 + 3, 90, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "5",
                WS_VISIBLE | WS_CHILD | WS_BORDER,
                vx, 162, 64, 22, hwnd, (HMENU)ID_EDIT_MAXSPEED, NULL, NULL);

            CreateWindowA("STATIC", "加速倍率:",
                WS_VISIBLE | WS_CHILD, sx, 190 + 3, 90, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "3",
                WS_VISIBLE | WS_CHILD | WS_BORDER,
                vx, 190, 64, 22, hwnd, (HMENU)ID_EDIT_SPEED_MUL, NULL, NULL);

            CreateWindowA("STATIC", "滚轮间隔(ms):",
                WS_VISIBLE | WS_CHILD, sx, 218 + 3, 96, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "80",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                vx, 218, 64, 22, hwnd, (HMENU)ID_EDIT_WHEEL_INTERVAL, NULL, NULL);

            CreateWindowA("STATIC", "滚轮速度:",
                WS_VISIBLE | WS_CHILD, sx, 246 + 3, 90, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "1",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                vx, 246, 64, 22, hwnd, (HMENU)ID_EDIT_WHEEL_SPEED, NULL, NULL);

            CreateWindowA("STATIC", "滚轮加速:",
                WS_VISIBLE | WS_CHILD, sx, 274 + 3, 90, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "3",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                vx, 274, 64, 22, hwnd, (HMENU)ID_EDIT_WHEEL_MUL, NULL, NULL);

            // 刷新间隔：控制线程多久醒一次。数值越小越跟手、CPU 唤醒越频繁。
            CreateWindowA("STATIC", "刷新间隔(ms):",
                WS_VISIBLE | WS_CHILD, sx, 302 + 3, 96, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "6",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                vx, 302, 64, 22, hwnd, (HMENU)ID_EDIT_TICK, NULL, NULL);

            // 应用 / 重置都是全量操作：它们推进的是整份草稿
            // （速度/加速度/上限 + 移动加速倍率 + 滚轮 3 项 + 刷新间隔
            //   + 11 个映射/加速键 + 4 个组合键 + 2 个复选框），
            // 所以不放在本组里，统一挪到窗口底部。

            // ---------- 右栏：按键映射 ----------
            CreateWindowA("BUTTON", "按键映射",
                WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                col2, 78, colW, 256, hwnd, NULL, NULL, NULL);

            // 组内排两列，每列「标签 44 + 按键按钮 76」。
            const int kx1 = col2 + 12;           // 第一列标签 x
            const int kx2 = col2 + 12 + 136;     // 第二列标签 x
            const int bw  = 76;                  // 按键按钮宽

            // 11 个映射/加速键的创建逻辑完全一样，用 lambda 收口，避免复制 11 遍。
            // 按钮初始文字取当前映射值，和 UpdateAllButtonText 的结果一致。
            auto addKeyRow = [&](int x, int y, const char* label, int btnId, int vk) {
                CreateWindowA("STATIC", label,
                    WS_VISIBLE | WS_CHILD, x, y + 3, 44, 18, hwnd, NULL, NULL, NULL);
                CreateWindowA("BUTTON", GetKeyName(vk).c_str(),
                    WS_VISIBLE | WS_CHILD, x + 46, y, bw, ctlH,
                    hwnd, (HMENU)(INT_PTR)btnId, NULL, NULL);
            };

            addKeyRow(kx1, 106, "上:",   ID_BTN_KEY_UP,        keyUp.load());
            addKeyRow(kx2, 106, "下:",   ID_BTN_KEY_DOWN,      keyDown.load());
            addKeyRow(kx1, 134, "左:",   ID_BTN_KEY_LEFT,      keyLeft.load());
            addKeyRow(kx2, 134, "右:",   ID_BTN_KEY_RIGHT,     keyRight.load());
            addKeyRow(kx1, 162, "左键:", ID_BTN_KEY_LCLICK,    keyLClick.load());
            addKeyRow(kx2, 162, "右键:", ID_BTN_KEY_RCLICK,    keyRClick.load());
            // 最后两行：滚轮上下放一起，中键和加速键放一起
            addKeyRow(kx1, 190, "上滚:", ID_BTN_KEY_WHEELUP,   keyWheelUp.load());
            addKeyRow(kx2, 190, "下滚:", ID_BTN_KEY_WHEELDOWN, keyWheelDown.load());
            addKeyRow(kx1, 218, "中键:", ID_BTN_KEY_MCLICK,    keyMClick.load());
            // 加速键：按住它只影响"移动速度"，不再管滚轮（滚轮有自己的键）
            addKeyRow(kx2, 218, "加速:", ID_BTN_KEY_BOOST,     keyBoost.load());

            // 滚轮加速键：按住时滚轮档位乘"滚轮加速"。
            // 标签是 4 个字 + 冒号，addKeyRow 固定的 44px 会被截断，
            // 所以这一行单独创建：标签 76px，按钮从 +80 开始。
            // 该行只有这一个控件，第二列是空的，因此可以放心超过
            // 其它行"第一列只占 136px"的限制（80+76=156，仍远小于组宽 292）。
            CreateWindowA("STATIC", "滚轮加速:",
                WS_VISIBLE | WS_CHILD, kx1, 246 + 3, 76, 18, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", GetKeyName(keyWheelBoost.load()).c_str(),
                WS_VISIBLE | WS_CHILD, kx1 + 80, 246, bw, ctlH,
                hwnd, (HMENU)(INT_PTR)ID_BTN_KEY_WHEELBOOST, NULL, NULL);

            // ---------- 开关组合键：跨两栏一整条 ----------
            // 高度 96：第一行 4 个槽位 + 清空，第二行两个模式单选按钮。
            CreateWindowA("BUTTON", "开关组合键（1~4 个键同时按下）",
                WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                M, 342, wideW, 96, hwnd, NULL, NULL, NULL);

            CreateWindowA("BUTTON", "CapsLock",
                WS_VISIBLE | WS_CHILD, M + 12,  364, 70, ctlH, hwnd, (HMENU)ID_BTN_SWITCH1, NULL, NULL);
            CreateWindowA("BUTTON", "无",
                WS_VISIBLE | WS_CHILD, M + 90,  364, 70, ctlH, hwnd, (HMENU)ID_BTN_SWITCH2, NULL, NULL);
            CreateWindowA("BUTTON", "无",
                WS_VISIBLE | WS_CHILD, M + 168, 364, 70, ctlH, hwnd, (HMENU)ID_BTN_SWITCH3, NULL, NULL);
            CreateWindowA("BUTTON", "无",
                WS_VISIBLE | WS_CHILD, M + 246, 364, 70, ctlH, hwnd, (HMENU)ID_BTN_SWITCH4, NULL, NULL);
            // 清空按钮推到最右，和四个槽位拉开距离，防误点。
            CreateWindowA("BUTTON", "清空全部",
                WS_VISIBLE | WS_CHILD, M + 468, 364, 100, ctlH, hwnd, (HMENU)ID_BTN_CLEAR_SWITCH, NULL, NULL);

            // 组合键的生效方式：二选一。
            // WS_GROUP 标在第一个上，把这两个单选按钮圈成一组，
            // 否则它们会和窗口里其它单选按钮（目前没有）互相干扰。
            CreateWindowA("BUTTON", "按下切换（按一下开、再按一下关）",
                WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP,
                M + 12, 398, 250, ctlH, hwnd, (HMENU)ID_RADIO_SWITCH_TOGGLE, NULL, NULL);
            CreateWindowA("BUTTON", "按住生效（按下开、松开关）",
                WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON,
                M + 274, 398, 220, ctlH, hwnd, (HMENU)ID_RADIO_SWITCH_HOLD, NULL, NULL);

            // ---------- 存档：从右侧栏挪到底部，变成一条横向工具栏 ----------
            CreateWindowA("BUTTON", "存档",
                WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                M, 446, wideW, 86, hwnd, NULL, NULL, NULL);

            // 下拉框：CBS_DROPDOWNLIST 禁止用户编辑文本，只能从列表中选择。
            // 高度参数 200 是"收起高度 + 展开后列表高度"的总和，实际列表约 176px
            // （约 8 行）。超过就自动出竖向滚动条，不会无限往下长；
            // WS_VSCROLL 显式写上是为了让这个意图在代码里一眼可见。
            CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST | WS_VSCROLL,
                M + 12, 468, 200, 200, hwnd, (HMENU)ID_COMBO_SAVE, NULL, NULL);

            CreateWindowA("BUTTON", "保存",
                WS_VISIBLE | WS_CHILD, M + 224, 468, 80, ctlH, hwnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
            CreateWindowA("BUTTON", "加载",
                WS_VISIBLE | WS_CHILD, M + 312, 468, 80, ctlH, hwnd, (HMENU)ID_BTN_LOAD, NULL, NULL);

            // 存档目录显示在组内，和存档操作放在一起，不用再往下找一行。
            // SS_ENDELLIPSIS 让过长的路径自动变成省略号，不撑破分组框。
            CreateWindowA("STATIC", "",
                WS_VISIBLE | WS_CHILD | SS_ENDELLIPSIS,
                M + 12, 500, wideW - 24, 18, hwnd, (HMENU)ID_STATIC_STATUS, NULL, NULL);

            // ---------- 底部：快捷键提示 + 操作按钮（同一行） ----------
            // 存档目录已经挪进"存档"分组框，这一行左边放提示、右边放三个按钮。
            // 应用：把整份草稿推进内存，立刻生效，但不写盘。
            // 重置：把整份草稿恢复成出厂默认。
            // 退出：不看"最小化到托盘"设置，直接结束程序。
            // 按钮右对齐，右边距 12，和左边距对称。
            CreateWindowA("BUTTON", "应用", WS_VISIBLE | WS_CHILD,
                336, 540, 80, 28, hwnd, (HMENU)ID_BTN_APPLY, NULL, NULL);
            CreateWindowA("BUTTON", "重置", WS_VISIBLE | WS_CHILD,
                424, 540, 80, 28, hwnd, (HMENU)ID_BTN_RESET, NULL, NULL);
            CreateWindowA("BUTTON", "退出程序", WS_VISIBLE | WS_CHILD,
                512, 540, 80, 28, hwnd, (HMENU)ID_BTN_EXIT, NULL, NULL);

            // Esc+0 只在窗口前台有效（这个限制在钩子里判断，提示里不再写）
            // 和三个按钮放在同一行：提示靠左、按钮靠右，底部只占一行。
            CreateWindowA("STATIC", "重置快捷键：Esc + 0",
                WS_VISIBLE | WS_CHILD,
                M, 546, 300, 18, hwnd, NULL, NULL, NULL);

            // ==================== 存档初始化 ====================
            // g_saveDir 已经在 WinMain 里定好了（正常 / 回退 / 只读三种模式），
            // 这里只按模式决定要不要碰磁盘，不再做目录探测。

            // 先把草稿初始化成当前内存值（此刻就是出厂默认）。
            // 必须放在读档之前：如果 save0 不存在，草稿也得是完整的，
            // 否则之后点"应用"会把映射全推成 0。
            DraftFromApplied();
            g_dirty = false;

            // 立刻按草稿刷一遍界面：单选按钮在创建时没有默认勾选，
            // 不刷这一下会出现"两个都没选中"的状态。
            RefreshUIFromDraft(hwnd);

            if (g_saveWritable) {
                EnsureSaveDir();      // 只在可写时补建默认档
            } else {
                // 只读：只禁"保存"（唯一的写操作）。
                // 扫描目录和加载都不碰磁盘，照常可用。
                EnableWindow(GetDlgItem(hwnd, ID_BTN_SAVE), FALSE);
            }
            ScanSaveDir();
            FillComboBox(hwnd);

            // 读 save0.ini（默认存档）：缺失属首次运行，损坏则提示并保留备份。
            LoadResult lr = LoadFromFile("save0");
            if (lr == LOAD_FAILED) {
                SetStatus(hwnd, "存档读取失败，已使用默认值；原文件将在下次保存前备份为 .bak。");
            } else if (!g_saveWritable) {
                SetStatus(hwnd, "只读模式：程序目录不可写且未启用回退，设置不会保存。");
            } else if (g_saveDirFallback) {
                SetStatus(hwnd, "存档目录（程序目录不可写，已改用用户目录）：" + g_saveDir);
            } else {
                SetStatus(hwnd, "存档目录：" + g_saveDir);
            }

            // 按钮文字只在映射真正改变时刷新（捕获 / 清空 / 重置 / 加载），
            // 不再靠 500ms 定时器兜底：那些原子量是本进程私有的，外部改不到，
            // 而每条改动路径都已经主动刷过一次。

            // 所有控件和初值都就位后再挂输入过滤，避免加载存档时被拦
            AttachNumberFilters(hwnd);
            break;
        }

        case WM_COMMAND: {
            // 通知源：LOWORD(wParam) 是控件 ID，HIWORD(wParam) 是通知码。
            int id = LOWORD(wParam);

            switch (id) {
                // ---------- 数值框内容改动：只置脏，不生效 ----------
                // 改数字不会立刻影响手感，点"应用"才推进内存；写盘还要等"保存"。
                // 程序自己 SetDlgItemInt 时也会触发 EN_CHANGE，用 g_updatingUI 挡掉。
                case ID_EDIT_SPEED:
                case ID_EDIT_ACCEL:
                case ID_EDIT_MAXSPEED:
                case ID_EDIT_SPEED_MUL:
                case ID_EDIT_WHEEL_INTERVAL:
                case ID_EDIT_WHEEL_SPEED:
                case ID_EDIT_WHEEL_MUL:
                case ID_EDIT_TICK:
                    if (HIWORD(wParam) == EN_CHANGE && !g_updatingUI)
                        g_dirty = true;
                    break;

                // ---------- 进入捕获模式：任何映射/组合键按钮点击均触发 ----------
                case ID_BTN_KEY_UP:
                case ID_BTN_KEY_DOWN:
                case ID_BTN_KEY_LEFT:
                case ID_BTN_KEY_RIGHT:
                case ID_BTN_KEY_LCLICK:
                case ID_BTN_KEY_RCLICK:
                case ID_BTN_KEY_MCLICK:
                case ID_BTN_KEY_WHEELUP:
                case ID_BTN_KEY_WHEELDOWN:
                case ID_BTN_KEY_BOOST:
                case ID_BTN_KEY_WHEELBOOST:
                case ID_BTN_SWITCH1:
                case ID_BTN_SWITCH2:
                case ID_BTN_SWITCH3:
                case ID_BTN_SWITCH4: {
                    waitingButtonId = id;
                    SetDlgItemTextA(hwnd, id, "按任意键...");
                    SetFocus(hwnd);   // 把焦点从按钮移到窗口，避免按钮抢占按键
                    break;
                }

                // ---------- 清空全部组合键 ----------
                case ID_BTN_CLEAR_SWITCH: {
                    // 只改草稿：界面立刻变成"无"，但实际组合键要等"应用"才失效。
                    g_draft.switch1 = 0;
                    g_draft.switch2 = 0;
                    g_draft.switch3 = 0;
                    g_draft.switch4 = 0;
                    g_dirty = true;
                    UpdateAllButtonText(hwnd);
                    break;
                }

                // ---------- 组合键模式（两个单选按钮） ----------
                // 只改草稿：模式本身也要点"应用"才生效，和其它设置走同一套规则。
                case ID_RADIO_SWITCH_TOGGLE: {
                    if (HIWORD(wParam) == BN_CLICKED) {
                        g_draft.switchMode = 0;
                        g_dirty = true;
                    }
                    break;
                }

                case ID_RADIO_SWITCH_HOLD: {
                    if (HIWORD(wParam) == BN_CLICKED) {
                        g_draft.switchMode = 1;
                        g_dirty = true;
                    }
                    break;
                }

                // ---------- 两个布尔开关 ----------
                // 同样只改草稿，点"应用"之后才真正影响行为
                // （比如"最小化到托盘"要应用过才会在关窗时生效）。
                case ID_CHECK_INTERCEPT: {
                    g_draft.intercept = (IsDlgButtonChecked(hwnd, ID_CHECK_INTERCEPT) == BST_CHECKED);
                    g_dirty = true;
                    break;
                }

                case ID_CHECK_TRAY: {
                    g_draft.tray = (IsDlgButtonChecked(hwnd, ID_CHECK_TRAY) == BST_CHECKED);
                    g_dirty = true;
                    break;
                }

                // ---------- 开机自启动 ----------
                case ID_CHECK_AUTOSTART: {
                    bool want = (IsDlgButtonChecked(hwnd, ID_CHECK_AUTOSTART) == BST_CHECKED);
                    if (!SetAutoStart(want)) {
                        // 注册表写失败（权限、安全软件拦截等）：把勾选状态还原，
                        // 否则界面显示的和实际生效的会不一致。
                        MessageBoxA(hwnd, "无法修改开机自启动设置（注册表写入失败）。",
                                    "错误", MB_OK | MB_ICONERROR);
                        CheckDlgButton(hwnd, ID_CHECK_AUTOSTART,
                                       want ? BST_UNCHECKED : BST_CHECKED);
                    }
                    break;
                }

                // ---------- 应用 / 重置 ----------
                case ID_BTN_APPLY: {
                    // 编辑框 → 草稿 → 内存，立刻生效。
                    // 然后把当前状态镜像到 save0：这样不挑存档、直接关机，
                    // 下次启动也能恢复到刚应用的状态。
                    // 写成功就清脏标记 —— 内存里的东西已经落到 save0 上了。
                    ReadEditsIntoDraft(hwnd);
                    ApplyDraft();

                    if (!g_saveWritable) {
                        // 只读模式：能生效但存不了，脏标记保留
                        SetStatus(hwnd, "已应用（只读模式，未写入磁盘）");
                    } else if (SaveToFile("save0")) {
                        g_dirty = false;
                        SetStatus(hwnd, "已应用，并写入默认存档 save0");
                    } else {
                        SetStatus(hwnd, "已应用，但写入 save0 失败");
                    }
                    break;
                }

                case ID_BTN_RESET: {
                    ResetDraftAndUI(hwnd);
                    break;
                }

                // ---------- 退出 ----------
                // 刻意不看"最小化到托盘"：这个按钮的用途就是"真的退出"。
                // 但仍走 QuitApp，有未保存的修改会先问一句，避免误点丢配置。
                case ID_BTN_EXIT: {
                    QuitApp(hwnd);
                    break;
                }

                // ---------- 保存 ----------
                // 保存 = 应用 + 写所选存档 + 镜像写 save0。
                case ID_BTN_SAVE: {
                    // 只读模式下按钮本来已置灰，这里再兜一层，防止消息伪造。
                    if (!g_saveWritable) {
                        MessageBoxA(hwnd, "当前为只读模式，设置无法保存。", "提示",
                                    MB_OK | MB_ICONINFORMATION);
                        break;
                    }

                    std::string slot = GetSelectedSlot(hwnd);
                    if (slot.empty()) {
                        // 选中的是 "+"：走新建分支，不覆盖任何已有文件。
                        PostMessageA(hwnd, WM_NEW_SAVE, 0, 0);
                        break;
                    }

                    if (DoSave(hwnd, slot)) {
                        // 成功只更新状态行，不弹窗：保存是高频操作，弹窗打断太烦。
                        // 失败仍然弹窗，因为那是用户必须知道的事。
                        SetStatus(hwnd, "已保存：" + g_saveDir + "\\" + slot + ".ini");
                    } else {
                        // 不再无条件报"保存成功"：写不进去就要明确告诉用户。
                        SetStatus(hwnd, "保存失败：无法写入存档目录。");
                        MessageBoxA(hwnd, "保存失败：无法写入存档目录。", "错误",
                                    MB_OK | MB_ICONERROR);
                    }
                    break;
                }

                // ---------- 加载 ----------
                case ID_BTN_LOAD: {
                    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
                    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                    if (sel < 0) break;

                    char name[256];
                    SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);

                    // "+" 不是存档，跳过。
                    if (strcmp(name, "+") == 0) break;

                    LoadResult lr = LoadFromFile(name);
                    if (lr == LOAD_OK) {
                        // 成功只更新状态行，不弹窗：和保存保持一致。
                        // 失败（文件不存在 / 读取失败）仍然弹窗。
                        // 加载完把这份配置镜像到 save0，下次启动直接恢复到它。
                        if (g_saveWritable && !SaveToFile("save0")) {
                            // name 是 char[]，必须先转成 std::string，
                            // 否则 "字面量 + char[]" 会退化成指针运算。
                            SetStatus(hwnd, std::string("已加载：") + name + "（但写入 save0 失败）");
                        } else {
                            SetStatus(hwnd, "已加载：" + g_saveDir + "\\" + name + ".ini");
                        }
                    } else if (lr == LOAD_MISSING) {
                        MessageBoxA(hwnd, "存档文件不存在。", "提示", MB_OK | MB_ICONWARNING);
                    } else {
                        SetStatus(hwnd, "存档读取失败，已使用默认值。");
                        MessageBoxA(hwnd, "存档读取失败，已使用默认值。", "错误",
                                    MB_OK | MB_ICONERROR);
                    }
                    break;
                }

                // ---------- 下拉框选择变化 ----------
                case ID_COMBO_SAVE: {
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        // 用户选了 "+"，弹新建对话框（当前实现是静默新建）
                        HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
                        int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                        char name[256];
                        SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)name);
                        if (strcmp(name, "+") == 0) {
                            PostMessageA(hwnd, WM_NEW_SAVE, 0, 0);
                        }
                    }
                    break;
                }
            }
            break;
        }

        // ---------- 新建存档 ----------
        case WM_NEW_SAVE: {
            if (!g_saveWritable) {
                MessageBoxA(hwnd, "只读模式下无法新建存档。", "提示", MB_OK | MB_ICONINFORMATION);
                break;
            }

            // 找第一个不存在的编号；若 save1~save999 全被占用则报错退出。
            // 旧实现循环结束后 defaultName 停留在 "save99"（已存在），
            // 会直接覆盖 save99 的内容。
            char defaultName[16] = {};
            bool found = false;
            for (int i = 1; i <= 999 && !found; i++) {
                sprintf_s(defaultName, "save%d", i);
                found = !FileExists(g_saveDir + "\\" + defaultName + ".ini");
            }
            if (!found) {
                MessageBoxA(hwnd, "存档编号已用尽（save1~save999）。", "提示",
                            MB_OK | MB_ICONWARNING);
                break;
            }

            // 简化处理：直接以第一个未占用的 saveN 命名新建存档，暂不提供命名对话框
            // 若用户希望自定义名字，可后续在此处插入输入框。
            // 文件不存在时第一次写入就会创建它，无需先 CreateSaveFile。
            if (!DoSave(hwnd, defaultName)) {
                MessageBoxA(hwnd, "新建存档失败：无法写入存档目录。", "错误",
                            MB_OK | MB_ICONERROR);
                break;
            }
            ScanSaveDir();
            FillComboBox(hwnd);

            // 刷新后把下拉框选中到新建的这个档
            HWND hCombo = GetDlgItem(hwnd, ID_COMBO_SAVE);
            int idx = (int)SendMessageA(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)defaultName);
            if (idx != CB_ERR){
                SendMessageA(hCombo, CB_SETCURSEL, idx, 0);
            }

            SetStatus(hwnd, "已新建存档：" + std::string(defaultName));
            break;
        }

        // ---------- 重置快捷键（Esc+0，来自钩子） ----------
        case WM_RESET: {
            // 快捷重置比"重置"按钮多做一步：重置完直接推进内存，不用再点"应用"。
            // 理由：它的用途就是"一键回到出厂状态"，如果还要再点一次应用，
            // 这个快捷键就只是省了两次点击，意义不大。
            //
            // 但**不写盘**：内存生效 ≠ 覆盖存档。g_dirty 保留，
            // 退出时仍会问"要不要保存"，这样误按 Esc+0 不会把存档悄悄改掉。
            ResetDraftAndUI(hwnd);
            ApplyDraft();
            SetStatus(hwnd, "已重置为出厂默认并立即生效（未保存）");
            return 0;
        }

        // ---------- 捕获到按键，更新对应映射 ----------
        case WM_BIND_KEY:
        {
            int vk = (int)wParam;
            int btnId = (int)lParam;

            // Esc 视为"取消捕获"，保留原映射。
            if (vk == VK_ESCAPE) {
                waitingButtonId = 0;
                UpdateAllButtonText(hwnd);
                return 0;
            }

            // 冲突校验：一个键不能同时承担两种角色。
            // 否则 ShouldIntercept 会按"组合键成员"放行，映射就永久失效。
            // 比对的是草稿：用户看到什么就按什么判定，不能拿"已生效"的旧值来比。
            bool isSwitchButton = (btnId >= ID_BTN_SWITCH1 && btnId <= ID_BTN_SWITCH4);
            bool isBoostButton  = (btnId == ID_BTN_KEY_BOOST);
            bool isWheelBoostButton = (btnId == ID_BTN_KEY_WHEELBOOST);

            bool isMappingKey = (vk == g_draft.keyUp || vk == g_draft.keyDown ||
                                 vk == g_draft.keyLeft || vk == g_draft.keyRight ||
                                 vk == g_draft.keyLClick || vk == g_draft.keyRClick ||
                                 vk == g_draft.keyMClick || vk == g_draft.keyWheelUp ||
                                 vk == g_draft.keyWheelDown);
            bool isSwitchKey = (vk == g_draft.switch1 || vk == g_draft.switch2 ||
                                vk == g_draft.switch3 || vk == g_draft.switch4);

            // 映射键和组合键必须和两个加速键都不重合：加速键不被拦截，
            // 一旦重合，UpdateKeyState 的 if 链会先命中映射分支，加速永远不触发。
            // 但"加速键 ↔ 滚轮加速键"之间**不判冲突**：默认两者都是 Shift，
            // 相等时就是"一个键同时加速移动和滚轮"，这是合法且有意义的配置。
            bool conflict;
            if (isSwitchButton) {
                conflict = isMappingKey || (vk == g_draft.boostKey) ||
                           (vk == g_draft.wheelBoostKey);
            } else if (isBoostButton || isWheelBoostButton) {
                conflict = isMappingKey || isSwitchKey;
            } else {
                conflict = isSwitchKey || (vk == g_draft.boostKey) ||
                           (vk == g_draft.wheelBoostKey);
            }
            if (conflict) {
                MessageBoxA(hwnd, "该键已被另一种用途占用，请换一个键。", "按键冲突",
                            MB_OK | MB_ICONWARNING);
                waitingButtonId = 0;
                UpdateAllButtonText(hwnd);
                return 0;
            }

            // 把新键写进草稿。注意：这里不直接改原子量，
            // 所以"按了键但没点应用"时，旧键仍然是实际生效的那个。
            if (btnId == ID_BTN_KEY_UP){
                g_draft.keyUp = vk;
            }else if (btnId == ID_BTN_KEY_DOWN){
                g_draft.keyDown = vk;
            }else if (btnId == ID_BTN_KEY_LEFT){
                g_draft.keyLeft = vk;
            }else if (btnId == ID_BTN_KEY_RIGHT){
                g_draft.keyRight = vk;
            }else if (btnId == ID_BTN_KEY_LCLICK){
                g_draft.keyLClick = vk;
            }else if (btnId == ID_BTN_KEY_RCLICK){
                g_draft.keyRClick = vk;
            }else if (btnId == ID_BTN_KEY_MCLICK){
                g_draft.keyMClick = vk;
            }else if (btnId == ID_BTN_KEY_WHEELUP){
                g_draft.keyWheelUp = vk;
            }else if (btnId == ID_BTN_KEY_WHEELDOWN){
                g_draft.keyWheelDown = vk;
            }else if (btnId == ID_BTN_KEY_BOOST){
                g_draft.boostKey = vk;
            }else if (btnId == ID_BTN_KEY_WHEELBOOST){
                g_draft.wheelBoostKey = vk;
            }else if (btnId == ID_BTN_SWITCH1){
                g_draft.switch1 = vk;
            }else if (btnId == ID_BTN_SWITCH2){
                g_draft.switch2 = vk;
            }else if (btnId == ID_BTN_SWITCH3){
                g_draft.switch3 = vk;
            }else if (btnId == ID_BTN_SWITCH4){
                g_draft.switch4 = vk;
            }

            g_dirty = true;   // 草稿变了，但还没应用、也没保存
            waitingButtonId = 0;
            UpdateAllButtonText(hwnd);
            return 0;
        }

        // ---------- 窗口失去焦点：取消按键捕获 ----------
        // 捕获模式会吞掉除 Esc 以外的所有键，用户一旦点了别的窗口，
        // 不给出口就会一直卡在"按任意键..."（虽然 Esc 能取消，但不必要求用户知道）。
        // 不加超时：用户可能只是想慢慢按，超时反而会打断。
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                // 失焦时系统可能收不到 keyup（例如弹出 UAC 安全桌面、切换用户），
                // 把自维护的按键状态复位，免得组合键/加速键卡在"一直按着"。
                capsDown = false;
                keyBoostDown = false;
                wheelBoostDown = false;

                if (waitingButtonId.load() != 0) {
                    waitingButtonId = 0;
                    UpdateAllButtonText(hwnd);
                }
            } else {
                // 切回窗口时同步一次自启动勾选：用户可能刚在任务管理器或
                // "设置 → 应用 → 启动"里把它关掉了，这里只读不写。
                RefreshAutoStartCheck(hwnd);
            }
            break;
        }

        // ---------- 系统关机 / 注销 ----------
        // 按约定：什么都不保存，也不弹窗，直接放行。
        // 置标志是为了让随后可能到来的 WM_CLOSE 跳过"是否保存"的询问，
        // 否则关机流程会卡在一个没人点的对话框上，拖住系统关机。
        case WM_QUERYENDSESSION:
            g_shuttingDown = true;
            return TRUE;

        // ---------- 关闭窗口 ----------
        case WM_CLOSE: {
            // 判断用的是"已生效"的 isMinimizeToTray：勾选框只改草稿，
            // 没点"应用"之前不算数。
            if (isMinimizeToTray.load() && !g_shuttingDown) {
                // 走托盘：隐藏窗口 + 添加图标；进程继续运行。
                // 隐藏不等于退出，所以不询问保存。
                ShowWindow(hwnd, SW_HIDE);
                AddTrayIcon(hwnd);
            } else {
                // 真正退出：有未保存的修改会弹 是/否/取消。
                QuitApp(hwnd);
            }
            return 0;
        }

        // ---------- 托盘图标交互 ----------
        case WM_APP + 1: {
            if (lParam == WM_LBUTTONUP) {
                // 左键单击：显示窗口，然后移除图标（避免重复）。
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                RemoveTrayIcon();
            } else if (lParam == WM_RBUTTONUP) {
                // 右键单击：弹出菜单。
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, 1, "显示窗口");
                AppendMenuA(hMenu, MF_STRING, 2, "退出");
                // TrackPopupMenu 要求先把窗口置前台并 SetForegroundWindow，
                // 否则菜单点完不会消失（Windows 的经典小坑）。
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
                if (cmd == 1) {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    RemoveTrayIcon();
                } else if (cmd == 2) {
                    RemoveTrayIcon();
                    // 托盘"退出"也走 QuitApp：同样会询问是否保存未应用的修改。
                    QuitApp(hwnd);
                }
            }
            return 0;
        }

        // ---------- 销毁：全局收尾 ----------
        case WM_DESTROY:
            RemoveTrayIcon();
            // 这里不再隐式写 save0：保存只发生在用户点"保存"，
            // 或在退出提示里选"是"的时候。
            isRunning = false;     // 让控制线程尽快跳出 while
            PostQuitMessage(0);    // 结束消息泵
            return 0;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

// ==================== 程序入口点 ====================
// WinMain 是 Windows GUI 应用的入口（跟 main 不同，不出控制台窗口）。
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    // 单实例：重复启动只会挂上第二个全局钩子，导致按键被吞两次、状态错乱。
    // 已有实例则把它叫到前台，本进程直接退出。
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "Local\\KeyboardCursor_SingleInstance");
    if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND oldWnd = FindWindowA("MyClass", NULL);
        if (oldWnd != NULL) {
            ShowWindow(oldWnd, SW_SHOW);
            SetForegroundWindow(oldWnd);
        }
        CloseHandle(hMutex);
        return 0;
    }

    // 看门狗用的时间戳必须在使用前清零：atomic 的默认构造不保证初始化，
    // 不清的话可能一开始就误判成"很久没按键"，把状态乱清。
    InitKeyTimers();

    // 存档目录必须在创建窗口之前定下来：WM_CREATE 里就要扫描目录、读 save0.ini。
    // 不可写时这里会询问用户是否回退，拒绝则进入只读模式（程序照常运行）。
    ResolveSaveDir();

    // 注册窗口类：lpfnWndProc 指定窗口过程函数，lpszClassName 是类名，
    // 后续 CreateWindowExA 靠这个类名找到对应过程并调起来。
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "MyClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    // 新布局：顶部开关组 + 左右两栏 + 开关组合键 + 存档组 + 单行操作行，640×620。
    // 因为布局是绝对坐标、没有处理 WM_SIZE，所以去掉可拖动边框和最大化按钮，
    // 免得用户把窗口拉大后控件全挤在左上角。
    HWND hwnd = CreateWindowExA(
        0, "MyClass", "光标控制 [禁用]",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        100, 100, 640, 620,
        NULL, NULL, hInst, NULL
    );
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    // 安装低级键盘钩子：
    //   WH_KEYBOARD_LL 是全局钩子（可以监听到焦点不在本程序时的按键），
    //   hMod 传 hInst、dwThreadId 传 0 表示"所有线程"。
    //   低级钩子不需要 DLL 注入，因此天然支持跨越进程。
    g_hKeyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardProc, hInst, 0);
    if (g_hKeyboardHook == NULL) {
        MessageBoxA(NULL, "键盘钩子安装失败！", "错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 启动鼠标控制线程；它与主线程靠 atomic 变量通信。
    std::thread controlThread(MouseControlThread);

    // 主消息泵：标准 GetMessage/DispatchMessage 循环。
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 到这里说明消息泵退出（通常是 WM_QUIT）。清理顺序：
    //   1. 先卸钩子（防止继续吞键）；
    //   2. 再通知控制线程退出；
    //   3. 最后 join，保证线程把鼠标复位做完。
    if (g_hKeyboardHook != NULL) {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = NULL;
    }

    isRunning = false;
    if (controlThread.joinable()) {
        controlThread.join();
    }

    if (hMutex != NULL) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return 0;
}
