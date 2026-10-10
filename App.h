// ============================================================================
//  App.h —— Pic246 公共声明
//  （全局状态 / 工具函数 / 图片处理 / 交互画布 / 各功能页接口）
// ============================================================================
//  【文件说明 · 新手请先读这一段】
//
//  ● 这个文件是干什么的？
//    Pic246 是用「纯 Win32 API + GDI+」写的图片处理小工具。一个完整的 Windows
//    程序通常会拆成很多个 .cpp：主窗口、侧边栏、7 个功能页、裁切画布、涂鸦画布、
//    图片读写引擎……它们各自独立编译，最后链接成一个 exe。
//    可是问题来了：Project001.cpp 里保存的“当前打开的图片”，PageResize.cpp 也想用，
//    怎么办？答案就是这个 App.h —— 它是整个工程的「总纲 + 通讯录」：
//      · 用 extern 把全局变量“声明”出去，让每个 .cpp 访问到同一份数据；
//      · 声明公共辅助函数（D() / CreateCtl() / LoadImageFile() …）；
//      · 集中定义控件 ID、窗口类名、自定义消息这些“大家都要遵守的约定”。
//    任何一个 .cpp 只要开头写一行 #include "App.h"，就拿到了以上全部内容。
//
//  ● 整体架构（谁调用谁）
//      入口 WinMain（Project001.cpp）
//        └─ 创建主窗口 g_hMain：顶栏（打开按钮 / 路径 / 图片信息）、左侧边栏、底部状态栏
//             └─ 侧边栏被点击 → 向主窗口发 WM_APP_PAGE_SELECT → 主窗口切换到对应功能页
//      各功能页 Page*.cpp（尺寸缩放 / 裁切 / 调整 / ICO / 压缩 / 格式转换 / 涂鸦）
//        · 页面自己用 CreateCtl() 创建控件；页面窗口初始是隐藏的，切到谁就显示谁
//        · 需要处理图片时，统一调用 ImageOps.cpp 里的“图片引擎”函数
//      交互画布 CropCanvas.cpp / DrawCanvas.cpp
//        · 本质是「子窗口控件」，被装在裁切页 / ICO 页 / 涂鸦页内部
//        · 用自定义消息把「用户拖了裁切框」「吸管吸到了颜色」通知给父页面
//      图片引擎 ImageOps.cpp
//        · 加载 / 缩放 / 调色 / 保存 / 枚举编码器 / 预估文件体积
//
//  ● 为什么用 extern 全局变量，而不是一层层传参数？
//    因为“当前打开的图片”这种东西全程序天生只有一份，属于共享状态。若靠参数传递，
//    函数签名会又长又乱；更关键的是 Win32 窗口回调函数 WndProc 的签名由系统写死
//    （永远是 (HWND, UINT, WPARAM, LPARAM) 四个参数），根本没地方塞自定义参数。
//    所以 Win32 程序的通行做法是：全局状态在某一个 .cpp 里“定义”，在头文件里用
//    extern “声明”，其余文件直接读写。
//    请记住一句话：带 extern 的是「声明」（只告诉编译器“有这么个东西”，不分配内存）；
//    不带 extern 的才是「定义」（真的在这里分配内存）。
//    本文件的这些全局变量都定义在 Project001.cpp 中，别在别的文件里再定义一份，
//    否则链接时会报 LNK2005「符号重复定义」。
// ============================================================================

// ---------------------------------------------------------------------------
// 头文件保护
//   #pragma once 是给编译器的指令：同一个 .cpp 里即使 #include "App.h" 两次，
//   本文件内容也只展开一次。否则类型、变量会被重复声明，导致编译错误。
//   （传统写法是 #ifndef APP_H / #define APP_H / #endif 三件套，
//     #pragma once 更简洁，现代编译器都支持。）
// ---------------------------------------------------------------------------
#pragma once

// ---------------------------------------------------------------------------
// 依赖的头文件
//   头文件里用到的东西（HWND、std::wstring、Gdiplus::Image……）必须先“认识”，
//   所以要在本文件顶部把需要的东西都包含进来。
// ---------------------------------------------------------------------------
#include "framework.h"      // VS 模板生成的公共头：包含 <windows.h> 等系统头
#include "Project001.h"     // 本工程的资源头（内部再包含 resource.h 里的图标/菜单 ID）

#include <objbase.h>        // COM 基础（IStream 等）；GDI+ 需要 COM 初始化
#include <commctrl.h>       // 通用控件（按钮、分组框、Trackbar 滑块……）
#include <windowsx.h>       // 控件消息辅助宏，如 GET_X_LPARAM / SetWindowFont
#include <commdlg.h>        // 通用对话框（打开/保存文件、选颜色）
#include <shellapi.h>       // Shell API（ShellExecute 等）
#include <gdiplus.h>        // GDI+：图片解码/编码、绘图、Bitmap / Image 类型

#include <stdio.h>          // C 标准输入输出（swprintf_s 等格式化函数）
#include <wchar.h>          // 宽字符函数（wcslen、wcscpy_s……）
#include <string>           // C++ 标准库 std::wstring（宽字符字符串）
#include <vector>           // C++ 标准库 std::vector（动态数组）

// 告诉链接器去链接下面这些库（lib 文件），这样就不用在项目属性里手工添加了。
#pragma comment(lib, "gdiplus.lib")     // GDI+
#pragma comment(lib, "comctl32.lib")    // 通用控件
#pragma comment(lib, "comdlg32.lib")    // 通用对话框
#pragma comment(lib, "shell32.lib")     // Shell
#pragma comment(lib, "ole32.lib")       // COM
// 使用 Windows 视觉样式
//   这一行的作用：让程序用上 Windows 现代外观（按钮不再是 Win95 那种灰方块），
//   注意它同时要求 exe 旁有正确的 manifest；这里用 pragma 直接把依赖写进链接器。
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
// 图标资源 ID 兼容
//   VS 资源编辑器里导入/替换图标后，IDI_PROJECT001 之类的符号名可能被改掉
//   （例如新图标会叫 IDI_ICON2）。这里按优先级挑一个存在的，全部不存在时用
//   系统图标兜底，保证以后再换图标也不会因此编译不过。
//
//   新手提示：#if defined(...) 是「预处理条件判断」——在“编译之前”决定下面哪几行
//   参与编译。本工程实际情况是：resource.h 里只有 IDI_ICON2（=107）、IDI_SMALL、
//   IDR_MAINFRAME，没有 IDI_PROJECT001 / IDI_ICON1，
//   所以最终 APP_ICON_MAIN_ID 会等于 IDI_ICON2。
// ---------------------------------------------------------------------------
#if defined(IDI_PROJECT001)
#  define APP_ICON_MAIN_ID   IDI_PROJECT001
#elif defined(IDI_ICON2)
#  define APP_ICON_MAIN_ID   IDI_ICON2
#elif defined(IDI_ICON1)
#  define APP_ICON_MAIN_ID   IDI_ICON1
#elif defined(IDR_MAINFRAME)
#  define APP_ICON_MAIN_ID   IDR_MAINFRAME
#else
#  define APP_ICON_MAIN_ID   0
#endif

// 小图标（标题栏左上角、任务栏小图标）直接用主图标。
// Pic246.ico 里本身就打包了 16/24/32/48/64 五个尺寸，Windows 会按需要自动挑合适的一帧。
// 注意：以前这里优先用 IDI_SMALL（VS 模板自带的 small.ico），结果标题栏显示的一直是
// 模板里那个旧图标，换新图标时只换了 exe 图标、窗口图标却没变。所以这里不再单独挑。
#define APP_ICON_SMALL_ID    APP_ICON_MAIN_ID

// ---------------------------------------------------------------------------
// 应用信息 / 布局尺寸（单位：DIP，96dpi 下的逻辑像素）
//   DIP = Device Independent Pixel，逻辑像素。因为 Windows 有“显示缩放”
//   （125% / 150% / 200%），同样是“宽 168 像素”，在 150% 缩放下必须画成 252 个
//   物理像素，否则界面在 4K 屏上会小得像蚂蚁。所以界面尺寸一律按 96dpi 下的
//   逻辑值写好，真正建控件/画图时再统一乘以缩放系数（见下面的 D() 函数）。
// ---------------------------------------------------------------------------
#define APP_NAME             L"Pic246"          // 程序名（需要字符串的地方用它）
#define APP_WINDOW_TITLE     L"Pic246"          // 主窗口标题栏文字
#define APP_MAIN_CLASS       L"Pic246MainWnd"   // 主窗口的“窗口类名”

// 主界面三大区域的高度/宽度（DIP）。窗口类名和尺寸都用宏定义，方便一次改全局生效。
#define SIDEBAR_W_DIP        168    // 左侧边栏宽度：放功能按钮
#define TOPBAR_H_DIP         92     // 顶部栏高度：打开按钮 + 文件路径 + 图片信息
#define STATUSBAR_H_DIP      42     // 底部状态栏高度：显示提示文字

// ---------------------------------------------------------------------------
// 自定义消息
//   为什么从 WM_APP 开始？
//     Windows 把消息编号（UINT）分成几段：0x0000~0x03FF 是系统消息（WM_PAINT 等），
//     0x0400 起是 WM_USER，0x8000 起是 WM_APP。系统保证 WM_APP 之后的编号不会被
//     Windows 自己占用，所以“自定义消息”要从 WM_APP 起算，不会和系统消息撞车。
//     这里用 (WM_APP + 1)、(WM_APP + 2) …… 依次编号，谁是谁一目了然。
//   这些消息用于「子窗口/侧边栏 → 父窗口」的单向通知（不指望返回值）。
// ---------------------------------------------------------------------------
#define WM_APP_PAGE_SELECT    (WM_APP + 1)   // 侧边栏 -> 主窗口：切到第 wParam 页
#define WM_APP_IMAGE_CHANGED  (WM_APP + 2)   // 主窗口 -> 各页：源图已更换
#define WM_APP_RECT_CHANGED   (WM_APP + 3)   // 交互画布 -> 父页面：裁切框变化

// ---------------------------------------------------------------------------
// 控件 / 命令 ID（每个功能页一个号段，避免冲突）
//
//   为什么要给每个控件一个整数 ID？
//     窗口过程 WndProc 收到的消息里，“是哪个按钮被点了”是靠 wParam（低 16 位）
//     这个整数 ID 来区分的，所以每个控件必须有唯一 ID。
//   为什么要分段？
//     工程里有 7 个功能页 + 画布，控件上百个。如果随手编号，很容易撞号
//     （两个按钮同一个 ID，点击时就分不清、消息会串）。这里按“页”分段：
//       1001~1010  主窗口（顶栏、状态栏、侧边栏）
//       1100~1114  图片尺寸缩放页  RS = ReSize
//       1200~1209  图片裁切页      CP = CroP
//       1300~1310  图片调整页      AD = ADjust
//       1400~1423  ICO 转换页      IC = ICo
//       1500~1515  图片大小压缩页  CM = CoMpress
//       1600~1611  格式转换页      CV = ConVert
//       1700~1710  涂鸦页          DR = DRaw
//       40001~     菜单命令 ID（IDM_ 前缀），独立于控件 ID
//     每段留出余量，将来加控件不用重排。
//
//   新手提示：enum : int { ... } 是 C++11 的「限定底层类型的匿名枚举」，
//   用起来像常量（写 IDC_BTN_OPEN 即可），但比 #define 更安全（有作用域、有类型）。
// ---------------------------------------------------------------------------
enum : int
{
    // 主窗口
    IDC_BTN_OPEN         = 1001,   // 顶栏“打开图片”按钮
    IDC_STATIC_PATH      = 1002,   // 顶栏显示当前文件路径的静态文本
    IDC_STATIC_INFO      = 1003,   // 顶栏显示“宽×高 / 是否有透明”等信息的静态文本
    IDC_STATIC_STATUS    = 1004,   // 底部状态栏文字
    IDC_BTN_AUTHOR       = 1005,   // 顶栏“关于作者”按钮
    IDC_SIDEBAR          = 1010,   // 左侧边栏控件（承载一排功能页按钮）

    // 图片尺寸缩放页
    IDC_RS_PREVIEW       = 1100,   // 预览图区域
    IDC_RS_GRP           = 1101,   // 分组框（参数区外框）
    IDC_RS_OUT           = 1102,   // 宽/高输入框
    IDC_RS_STATUS        = 1103,   // 本页状态提示文字
    IDC_RS_BTN_PROCESS   = 1104,   // “开始处理/保存”按钮
    IDC_RS_RADIO_BASE    = 1110,   // 1110 .. 1114
                                   //   一组单选按钮的起始 ID：Win32 规定同一组
                                   //   radio 必须是「连续的 ID」，1110、1111、1112…
                                   //   代码里用 IDC_RS_RADIO_BASE + i 来遍历/判断。

    // 图片裁切页
    IDC_CP_CANVAS        = 1200,   // 裁切画布（可拖动的裁切框子窗口）
    IDC_CP_GRP_FILL      = 1201,   // “填充”分组框
    IDC_CP_BTN_COLOR     = 1202,   // “选择填充色”按钮
    IDC_CP_CHK_TRANS     = 1203,   // “透明填充”复选框
    IDC_CP_BTN_RESET     = 1204,   // “重置裁切框”按钮
    IDC_CP_BTN_PROCESS   = 1205,   // “开始处理/保存”按钮
    IDC_CP_STATUS        = 1206,   // 本页状态提示文字
    IDC_CP_INFO          = 1207,   // 裁切框尺寸信息（如 320 × 240）
    IDC_CP_SWATCH        = 1208,   // 颜色小方块（实时显示当前填充色）
    IDC_CP_BTN_SQUARE    = 1209,   // “裁成正方形”快捷按钮

    // 图片调整页
    IDC_AD_PREVIEW       = 1300,   // 预览图区域
    IDC_AD_TRK_BLUR      = 1301,   // 模糊强度滑块（Trackbar）
    IDC_AD_TRK_BRIGHT    = 1302,   // 亮度滑块
    IDC_AD_TRK_SAT       = 1303,   // 饱和度滑块
    IDC_AD_LBL_BLUR      = 1304,   // 模糊数值标签（跟着滑块变）
    IDC_AD_LBL_BRIGHT    = 1305,   // 亮度数值标签
    IDC_AD_LBL_SAT       = 1306,   // 饱和度数值标签
    IDC_AD_BTN_RESET     = 1307,   // “重置全部参数”按钮
    IDC_AD_BTN_PROCESS   = 1308,   // “开始处理/保存”按钮
    IDC_AD_STATUS        = 1309,   // 本页状态提示文字
    IDC_AD_GRP           = 1310,   // 参数区外框（分组框）

    // ICO 转换页
    IDC_IC_CANVAS        = 1400,   // 画布（限定为正方形裁切，用于选要转成图标的区域）
    IDC_IC_GRP_SIZE      = 1401,   // “输出尺寸”分组框
    IDC_IC_BTN_RESET     = 1402,   // “重置选框”按钮
    IDC_IC_BTN_PROCESS   = 1403,   // “生成 ico 文件”按钮
    IDC_IC_STATUS        = 1404,   // 本页状态提示文字
    IDC_IC_INFO          = 1405,   // 选框尺寸信息
    IDC_IC_CHK_BASE      = 1410,   // 1410 .. 1416
                                   //   7 个尺寸复选框的起始 ID，依次对应
                                   //   16/24/32/48/64/128/256（见 kIcoSizes）
    IDC_IC_GRP_FILL      = 1420,   // “背景填充”分组框
    IDC_IC_BTN_COLOR     = 1421,   // “选择填充色”按钮
    IDC_IC_CHK_TRANS     = 1422,   // “透明背景”复选框
    IDC_IC_SWATCH        = 1423,   // 颜色小方块

    // 图片大小压缩页
    IDC_CM_PREVIEW       = 1500,   // 预览图区域
    IDC_CM_GRP_MODE      = 1501,   // “压缩方式”分组框
    IDC_CM_RADIO_QUALITY = 1502,   // 按质量压缩（自己拖画质滑块）
    IDC_CM_RADIO_TARGET  = 1503,   // 按目标体积压缩（填一个期望大小，程序二分找画质）
    IDC_CM_GRP_PARAM     = 1504,   // “参数”分组框
    IDC_CM_TRK_QUALITY   = 1505,   // 画质滑块（1~100）
    IDC_CM_LBL_QUALITY   = 1506,   // 画质数值标签
    IDC_CM_EDIT_TARGET   = 1507,   // 目标体积输入框（如 200）
    IDC_CM_LBL_UNIT      = 1508,   // 体积单位标签（KB）
    IDC_CM_GRP_FORMAT    = 1509,   // “输出格式”分组框
    IDC_CM_RADIO_KEEP    = 1510,   // 保持原格式
    IDC_CM_RADIO_JPEG    = 1511,   // 输出 JPEG
    IDC_CM_RADIO_PNG     = 1512,   // 输出 PNG
    IDC_CM_LBL_EST       = 1513,   // 预估体积显示标签
    IDC_CM_LBL_ORIG      = 1514,   // 原文件体积显示标签
    IDC_CM_BTN_PROCESS   = 1515,   // “开始压缩并保存”按钮

    // 格式转换页
    IDC_CV_PREVIEW       = 1600,   // 预览图区域
    IDC_CV_GRP_FORMAT    = 1601,   // “目标格式”分组框
    IDC_CV_COMBO_FORMAT  = 1602,   // 格式下拉框（选项来自系统可用编码器）
    IDC_CV_LBL_FMTINFO   = 1603,   // 该格式的说明文字（MIME、是否支持透明）
    IDC_CV_GRP_OPT       = 1604,   // “选项”分组框
    IDC_CV_TRK_QUALITY   = 1605,   // 画质滑块（有损格式才有效）
    IDC_CV_LBL_QUALITY   = 1606,   // 画质数值标签
    IDC_CV_CHK_TRANS     = 1607,   // “透明背景”复选框
    IDC_CV_BTN_COLOR     = 1608,   // “选择填充色”按钮
    IDC_CV_SWATCH        = 1609,   // 颜色小方块
    IDC_CV_LBL_EST       = 1610,   // 预估体积显示标签
    IDC_CV_BTN_PROCESS   = 1611,   // “转换并保存”按钮

    // 涂鸦页
    IDC_DR_CANVAS        = 1700,   // 涂鸦画布（鼠标按下去就画一笔）
    IDC_DR_GRP_BRUSH     = 1701,   // “画笔”分组框
    IDC_DR_TRK_SIZE      = 1702,   // 笔宽滑块
    IDC_DR_LBL_SIZE      = 1703,   // 笔宽数值标签
    IDC_DR_BTN_COLOR     = 1704,   // “选择画笔颜色”按钮
    IDC_DR_SWATCH        = 1705,   // 颜色小方块
    IDC_DR_BTN_PICK      = 1706,   // “吸管取色”按钮（点击画面取色）
    IDC_DR_BTN_UNDO      = 1707,   // “撤销上一笔”按钮
    IDC_DR_BTN_CLEAR     = 1708,   // “清空涂鸦”按钮
    IDC_DR_BTN_PROCESS   = 1709,   // “合成并保存”按钮
    IDC_DR_LBL_HINT      = 1710,   // 操作提示文字
    // 菜单命令
    //   IDM_ 前缀表示“菜单项/命令”而不是控件；它们通过 WM_COMMAND 发到窗口，
    //   编号从 40001 起，和上面 1xxx 的控件 ID 完全隔开，互不影响。
    IDM_OPEN_IMAGE       = 40001,  // 菜单：打开图片
    IDM_PROCESS_SAVE     = 40002,  // 菜单：处理并保存
    IDM_AUTHOR           = 40003,  // 菜单：关于作者
    IDM_PAGE_BASE        = 40010,  // 40010 .. 40016
                                   //   7 个“切换到第 N 页”的菜单项起始 ID，
                                   //   IDM_PAGE_BASE + i 即第 i 页（i 从 0 数）
};

// ICO 可选尺寸
//   kIcoSizes 是一个“只读数组”，内容在 ImageOps.cpp 里定义：
//   { 16, 24, 32, 48, 64, 128, 256 }，正好 7 个，和 ICO 页的 7 个复选框一一对应。
//   这里用 extern const int[] 只是“声明”，告诉编译器别的文件里有这个数组。
#define ICO_SIZE_COUNT 7
extern const int kIcoSizes[ICO_SIZE_COUNT];

// ---------------------------------------------------------------------------
// 全局状态（定义在 Project001.cpp）
//
//   【Win32 句柄类型速查】
//     句柄（handle）就是一个“不透明的编号”，你不该关心它的具体数值，
//     只把它当成系统发给你的“凭据”，把它传回 API 就能操作对应的对象。
//     HINSTANCE  H + INSTANCE（实例）——本程序在内存中的实例句柄；创建窗口、
//                加载资源（图标/字符串）都要用它，相当于“本程序的身份证”。
//     HWND       H + WiNDow ——窗口句柄；“往哪个窗口发消息、给哪个窗口画图”都由它指定。
//     HFONT      字体句柄（H + FONT）。HICON 图标句柄、HBRUSH 画刷句柄同理。
//     COLORREF   32 位颜色值，用 RGB(r,g,b) 宏生成，内存里是 0x00BBGGRR。
//     UINT/DWORD 都是 32 位无符号整数，只是历史叫法不同（UINT 更偏“计数/尺寸”，
//                DWORD 更偏“标志位/长度”）。
//
//   【为什么图片数据要放在全局？】
//     g_pSrcImage 指向的位图可能好几十兆，而且被多个页面同时用于预览、还被画布
//     用于取像素。若每个页面各存一份，内存翻几倍，而且一处改了别处不同步。
//     所以全程序只保存“一份源图”，任何页面的操作都基于这一份（处理结果另存副本）。
// ---------------------------------------------------------------------------
extern HINSTANCE        g_hInst;        // 本程序实例句柄（WinMain 里赋值，创建窗口/载入资源用）
extern HWND             g_hMain;        // 主窗口句柄（很多地方要靠它给主窗口发消息/取 DPI）
extern int              g_dpi;          // 当前屏幕 DPI（96=100%，120=125%，144=150%…）
                                        //   用于把 DIP 逻辑尺寸换算成物理像素，见 D()
extern HFONT            g_hUiFont;      // 全局界面字体（所有控件共享，省得每个控件各建一个字体）
extern std::wstring     g_srcPath;      // 当前打开的图片完整路径（std::wstring 是宽字符，能存中文路径）
extern Gdiplus::Image*  g_pSrcImage;    // 当前源图（GDI+ Image），nullptr 表示还没打开图片
extern IStream*         g_pSrcStream;   // GDI+ 从内存流解码图片，流必须比 Image 活得久，
                                        //   所以单独保存一份引用，换图/退出时才能正确释放
extern UINT             g_srcW;         // 源图宽度（像素）
extern UINT             g_srcH;         // 源图高度（像素）
extern bool             g_srcHasAlpha;  // 源图是否带透明通道（PNG/GIF 等可能是 true）
extern COLORREF         g_fillColor;      // 裁切 / ICO 的填充色（两页共享）

// ---------------------------------------------------------------------------
// 通用工具（定义在 ImageOps.cpp）
// ---------------------------------------------------------------------------

// D()：把“逻辑像素(DIP)”换算成“物理像素”。
//   参数 dip：按 96dpi 设计时写的尺寸，例如 D(168) 里的 168。
//   返回值  ：当前屏幕 DPI 下实际应该使用的像素数。
//   原理    ：MulDiv(a,b,c) = a*b/c 且用 64 位中间结果避免溢出，
//             D(dip) = dip * g_dpi / 96。
//             例如屏幕 150% 缩放(g_dpi=144)：D(168) = 168*144/96 = 252。
//   写成 inline（内联）是因为它会被调用成百上千次，内联可免去函数调用开销。
//   注意：纯计算函数放在头文件里，必须 inline，否则每个 .cpp 都生成一份定义会链接冲突。
inline int D(int dip) { return MulDiv(dip, g_dpi, 96); }

// SetAppStatus：在底部状态栏显示一行提示文字。
//   text 用 std::wstring 传引用（&）是为了避免每次调用都复制一份字符串。
void SetAppStatus(const std::wstring& text);

// ApplyUiFont：把全局界面字体 g_hUiFont 设置到窗口 h 上（Win32 控件默认是“宋体”老样式，
//   必须显式设置字体才好看）。新建控件后一般都要调一次。
void ApplyUiFont(HWND h);

// CreateCtl：统一的“创建子控件”辅助函数（CreateCtl = Create Control）。
//   cls    ：窗口类名，如 L"BUTTON"、L"STATIC"、L"COMBOBOX"、TRACKBAR_CLASS
//   text   ：控件上显示的文字（按钮标题、标签内容），可以为 L""
//   style  ：窗口样式，如 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON
//   parent ：父窗口（控件显示在谁里面）
//   id     ：控件 ID，点击/通知时用它区分是哪个控件（就是上面枚举里的值）
//   exStyle：扩展样式，默认 0，需要如 WS_EX_CLIENTEDGE（凹陷边框）时再传
//   返回值 ：创建出来的控件窗口句柄；失败返回 nullptr
//   为什么要封装？CreateWindowEx 有 12 个参数，直接写又长又容易漏 WS_CHILD 之类，
//   封装后所有页面创建控件的写法统一，也顺便统一了字体和可见性。
HWND CreateCtl(const wchar_t* cls, const wchar_t* text, DWORD style, HWND parent, int id,
               DWORD exStyle = 0);

// ---- 路径与字符串小工具 ----
// 这些函数都接收 std::wstring 路径（宽字符，支持中文路径/文件名）。
std::wstring FileNameOf(const std::wstring& path);   // 取文件名：C:\a\b.jpg -> b.jpg
std::wstring DirNameOf(const std::wstring& path);    // 取目录名：C:\a\b.jpg -> C:\a
std::wstring StemOf(const std::wstring& path);       // 取主名（去扩展名）：b.jpg -> b
std::wstring ExtOf(const std::wstring& path);        // 取扩展名（含点，通常小写）：b.JPG -> .jpg
bool         FileExists(const std::wstring& path);   // 文件是否存在
std::wstring IntToStr(long long v);                  // 整数转字符串：123 -> L"123"
std::wstring FormatSize(UINT w, UINT h);             // 拼尺寸文本：1920,1080 -> L"1920 × 1080"

// ---------------------------------------------------------------------------
// 图片处理（定义在 ImageOps.cpp）
// ---------------------------------------------------------------------------

// AdjustParams：调色页三个滑块的参数打包。
//   把一组相关参数装进 struct，好处是函数签名短、以后加参数不用改所有调用处。
struct AdjustParams
{
    int blur;        // 0..40，模糊半径（原图像素）
    int brightness;  // -100..100
    int saturation;  // -100..100
};

// LoadImageFile：从磁盘加载一张图片到全局源图 g_pSrcImage。
//   path ：图片完整路径
//   err  ：输出参数（传引用），失败时把“给用户看的中文错误原因”写进去；
//          成功时清空。用输出参数而不是返回值，是因为返回值要留给“成功/失败”。
//   返回 ：true 成功，false 失败
//   提醒 ：它内部还会顺带更新 g_srcPath / g_srcW / g_srcH / g_srcHasAlpha / g_pSrcStream。
bool  LoadImageFile(const std::wstring& path, std::wstring& err);

// ClearImage：释放当前源图及其内存流，把全局状态清空（打开新图前/退出时调用）。
void  ClearImage();

// NotifyImageChanged：向所有页面广播 WM_APP_IMAGE_CHANGED，让它们重新生成预览。
//   这样“打开新图”只需要在一个地方写，所有页面自动刷新。
void  NotifyImageChanged();

// ResizeBitmap：把 src 缩放到 newW × newH，返回一张新的位图。
//   返回 ：新位图指针（Gdiplus::Bitmap*）；调用者负责在合适的时候 delete 释放！
//   err  ：失败原因
//   为什么要“返回新对象”而不是原地改？因为 GDI+ 的 Graphics::DrawImage 需要一个
//   目标位图，原地缩放既不好写也不安全；返回新对象则语义清晰。
Gdiplus::Bitmap* ResizeBitmap(Gdiplus::Image* src, int newW, int newH, std::wstring& err);

// CreatePreviewSource：生成“预览用的小图”，最长边不超过 maxDim 像素。
//   为什么要它？原图可能 8000×6000，直接每帧画进预览框会非常卡；
//   先缩小成一张小图再画，界面就流畅了。
//   返回 ：新建的位图，调用者负责 delete（注释里的“调用者负责 delete”一定要遵守）
Gdiplus::Bitmap* CreatePreviewSource(int maxDim);          // 新建，调用者负责 delete

// ApplyAdjustments：按参数 p 对位图 bmp 做模糊/亮度/饱和度调整（原地修改）。
void  ApplyAdjustments(Gdiplus::Bitmap* bmp, const AdjustParams& p);

// SaveBitmapWithSuffix：弹出“另存为”对话框，把 bmp 保存到目标路径。
//   suffix   ：文件名后缀，如 L"_resized"，用来生成“原名_resized.jpg”避免覆盖原文件
//   owner    ：父窗口句柄，用于对话框归属和报错提示框
//   donePath ：输出参数，成功时写回最终保存的完整路径（可为 nullptr 表示不需要）
//   返回     ：true 成功
//   注意     ：函数内部会 delete bmp，调用者保存完之后不要再 delete（否则重复释放崩溃）
bool  SaveBitmapWithSuffix(Gdiplus::Bitmap* bmp, const std::wstring& suffix, HWND owner,
                           std::wstring* donePath);        // 内部会 delete bmp

// SaveIcoWithSuffix：把正方形图 square 生成多尺寸 .ico 文件并保存。
//   sizes    ：要写入 ico 的尺寸列表，元素一般取自 kIcoSizes（16/24/32/48/64/128/256）
//   owner    ：父窗口
//   donePath ：输出参数，最终保存路径
//   返回     ：true 成功；内部会 delete square
bool  SaveIcoWithSuffix(Gdiplus::Bitmap* square, const std::vector<int>& sizes, HWND owner,
                        std::wstring* donePath);           // 内部会 delete square

// ChooseFillColor：弹出系统“颜色选择”对话框。
//   current ：对话框打开时的初始颜色
//   返回    ：用户选定的颜色；用户取消时一般返回 current
COLORREF ChooseFillColor(HWND owner, COLORREF current);

// 编码器枚举 / 体积预估（压缩页、格式转换页使用）
//
//   【名词解释】“编码器(Encoder)”在这里指“把内存里的图片写成某种文件格式的模块”。
//   系统里装了哪些格式（JPEG/PNG/BMP/GIF/TIFF…）是运行时可查询的，所以做成列表。

// EncoderChoice：一个可用输出格式的全部信息。
struct EncoderChoice
{
    std::wstring name;   // 显示名，如 "JPEG"
    std::wstring mime;   // image/jpeg
    std::wstring ext;    // .jpg
};

// OutFmtGuess：根据当前源文件“猜出来”的输出格式。
struct OutFmtGuess
{
    std::wstring mime;
    std::wstring ext;      // 含点，如 .jpg
    bool         alpha;    // 该格式是否支持透明通道
};

std::vector<EncoderChoice> ListImageEncoders();                       // 系统可用的图片编码器
OutFmtGuess GuessSourceMime();                                        // 按源文件扩展名推测输出格式

// EstimateEncodedSize：不真正写文件，先把图片编码到内存，量出“如果保存大约多少字节”。
//   img     ：要估算的图片
//   mime    ：目标格式，如 L"image/jpeg"
//   quality ：画质（1~100，仅对 JPEG 等有损格式有意义）
//   返回    ：估算出的字节数（size_t 是无符号大整数，表示“大小”比 int 更合适）
size_t EstimateEncodedSize(Gdiplus::Image* img, const std::wstring& mime, ULONG quality);

// FindQualityForTargetSize：给定目标体积，二分查找出最接近的 JPEG 画质。
//   img        ：源图
//   targetBytes：希望达到的字节数（用户输入的 KB 换算而来）
//   actualBytes：输出参数，实际二分出来的体积（可能略小或略大）
//   返回       ：找到的画质 1~100；返回负数（如 -1）表示不可行。
//   【为什么用二分？】画质与体积是单调关系（画质越低体积越小），
//   所以不必 1~100 逐个试，二分约 7 次就能锁定，速度快很多。
int    FindQualityForTargetSize(Gdiplus::Image* img, size_t targetBytes, size_t* actualBytes);

// SaveBitmapAsFormat：按指定格式保存（格式转换页用）。
//   bmp    ：要保存的位图（函数内部会 delete 它）
//   suffix ：文件名后缀（如 L"_conv"）
//   mime   ：目标格式的 MIME，如 L"image/png"
//   ext    ：目标扩展名，如 L".png"
//   quality：画质（仅对 JPEG 等有意义）
//   owner  ：父窗口
//   返回   ：true 成功
bool   SaveBitmapAsFormat(Gdiplus::Bitmap* bmp, const std::wstring& suffix,
                          const std::wstring& mime, const std::wstring& ext,
                          ULONG quality, HWND owner);

// FormatBytes：把字节数格式化成人类能读的字符串，如 1536 -> L"1.5 KB"。
std::wstring FormatBytes(size_t bytes);

// SourceFileSize：当前源文件在磁盘上的字节数（用于显示“原文件大小”）。
size_t SourceFileSize();

// FormatSupportsAlpha：当前源图扩展名是否支持透明（png/gif/tiff 支持，jpg 不支持）。
bool  FormatSupportsAlpha();                               // 当前源图扩展名是否支持透明

// ---- 绘制辅助（预览区绘制三件套）----
// HDC = Handle to Device Context，“设备环境句柄”。在 Win32 里，画任何东西都要先拿到
// HDC（相当于“画笔 + 画布”的凭据），再用 GDI/GDI+ 往上画。这些函数在 WM_PAINT 里被调用。
void  PaintCheckerBackground(HDC hdc, const RECT& rc);     // 画灰白格子底，用来衬托透明区域
void  PaintImagePreview(HDC hdc, const RECT& rc, Gdiplus::Bitmap* img, const wchar_t* hint);
                                                           // 在 rc 区域内等比居中画出 img；
                                                           // img 为空时显示 hint 提示文字
void  PaintColorSwatch(HDC hdc, const RECT& rc, COLORREF c, bool transparent);
                                                           // 画一小块颜色方块（transparent=true
                                                           // 时画成“透明”的格子样式）

// ---------------------------------------------------------------------------
// 交互画布（定义在 CropCanvas.cpp）
//   支持拖动移动裁切框、拖 8 个控制点缩放；裁切框允许超出图片范围。
//   squareMode = true 时只保留 4 个角控制点，且永远保持正方形（ICO 页用）。
//
//   【设计说明】裁切画布不是“在页面上随手画几个矩形”，而是一个真正的子窗口
//   （有自己的窗口类名和 WndProc）。这样处理鼠标消息、局部重绘都由系统自动分发，
//   代码清晰很多。页面的 WndProc 只需监听 WM_APP_RECT_CHANGED 就能知道裁切框变了。
// ---------------------------------------------------------------------------
#define CROPCANVAS_CLASS L"Pic246CropCanvas"   // 裁切画布的自定义窗口类名

// CropCanvas_Create：创建裁切画布子窗口。
//   parent    ：父窗口（裁切页或 ICO 页）
//   id        ：控件 ID（IDC_CP_CANVAS 或 IDC_IC_CANVAS）
//   squareMode：true = 正方形模式（ICO 页），false = 自由矩形（裁切页）
//   返回      ：画布窗口句柄
HWND CropCanvas_Create(HWND parent, int id, bool squareMode);

// CropCanvas_ResetToImage：把裁切框重置为“刚好铺满整张图”（不超出图片）。
void CropCanvas_ResetToImage(HWND h);                       // 铺满整张图（不超出）

// CropCanvas_ResetToSquareCenter：把裁切框重置为“图片中央的最大正方形”。
void CropCanvas_ResetToSquareCenter(HWND h);                // 居中最大正方形

// CropCanvas_GetRect：读回当前裁切框的位置和大小。
//   注意：用 float 作坐标单位，不是像素整数。因为画布内部用“原图像素坐标系”
//   记录裁切框（跟随图片实际尺寸），允许取到小数，再换算成屏幕像素时更精确。
//   x/y/w/hh 都是输出参数（传地址，函数里往外写）；hh 这个名字是为了避开
//   同名宏/变量的习惯写法，含义就是高度。
void CropCanvas_GetRect(HWND h, float* x, float* y, float* w, float* hh);

// CropCanvas_SetFill：告诉画布“若裁切框超出图片，超出的部分用什么填充”。
//   c           ：填充色
//   transparent ：true = 超出部分用透明（配合 PNG 输出），false = 用颜色 c 填满
void CropCanvas_SetFill(HWND h, COLORREF c, bool transparent);

// CropCanvas_RefreshImage：源图换了以后，让画布丢掉旧缓存重新缓存新图。
void CropCanvas_RefreshImage(HWND h);                       // 源图变化后刷新缓存

// ---------------------------------------------------------------------------
// 各功能页（定义在 Page*.cpp），均返回创建好的页面窗口（初始隐藏）
//
//   为什么页面一开始是隐藏的？
//     7 个页面如果都显示，会互相重叠。所以主窗口一次性把它们全建出来（只建一次，
//     切换时不重复创建，速度更快），然后靠 ShowWindow(SW_SHOW/SW_HIDE) 切换。
//     每个函数都只需要一个 parent 参数（父窗口 = 主窗口），返回页面窗口句柄
//     —— 主窗口把 7 个句柄存进数组，用索引就能切换。
// ---------------------------------------------------------------------------
HWND PageResize_Create(HWND parent);     // 图片尺寸缩放页
HWND PageCrop_Create(HWND parent);       // 图片裁切页
HWND PageAdjust_Create(HWND parent);     // 图片调整（模糊/亮度/饱和度）页
HWND PageIco_Create(HWND parent);        // ICO 图标转换页
HWND PageCompress_Create(HWND parent);   // 按体积压缩页
HWND PageConvert_Create(HWND parent);    // 格式转换页
HWND PageDraw_Create(HWND parent);       // 涂鸦页
// ---------------------------------------------------------------------------
// 涂鸦画布（定义在 DrawCanvas.cpp）
//   · 画笔绘制（矢量笔迹，可撤销）
//   · 吸色：点击画面取色
//
//   【和裁切画布的区别】涂鸦画布记录的是“一笔一笔的矢量笔迹”而不是最终像素，
//   所以撤销只要删掉最后一笔重画即可；最后保存时用 DrawCanvas_RenderFull()
//   按原图分辨率把笔迹合成上去。
// ---------------------------------------------------------------------------
#define DRAWCANVAS_CLASS L"Pic246DrawCanvas"   // 涂鸦画布的自定义窗口类名
// 注意：这里的自定义消息编号接着上面排（WM_APP + 4），不要和已有的重复。
#define WM_APP_PICKED_COLOR  (WM_APP + 4)   // 画布 -> 父窗口：吸到的颜色在 wParam

// DrawCanvas_Create：创建涂鸦画布子窗口（id 一般传 IDC_DR_CANVAS）。
HWND DrawCanvas_Create(HWND parent, int id);

// DrawCanvas_RefreshImage：源图换了，画布重新加载底图。
void DrawCanvas_RefreshImage(HWND h);              // 源图变化

// DrawCanvas_SetBrush：设置画笔颜色和粗细（宽度单位是原图像素，不是屏幕像素）。
void DrawCanvas_SetBrush(HWND h, COLORREF color, int width);

// DrawCanvas_SetPickMode：开关“吸管模式”。开启后点击画面不画线，而是取色并
//   向父窗口发送 WM_APP_PICKED_COLOR（颜色放在 wParam 里）。
void DrawCanvas_SetPickMode(HWND h, bool on);

// DrawCanvas_CanUndo：还有没有可以撤销的笔迹（没有就让“撤销”按钮变灰）。
bool DrawCanvas_CanUndo(HWND h);

// DrawCanvas_Undo：撤销最后一笔。
void DrawCanvas_Undo(HWND h);

// DrawCanvas_Clear：清空所有笔迹（回到干净底图）。
void DrawCanvas_Clear(HWND h);

// DrawCanvas_RenderFull：把底图和所有笔迹按“原图分辨率”合成一张新位图。
//   返回：新位图，调用者负责释放（delete）。
Gdiplus::Bitmap* DrawCanvas_RenderFull(HWND h);    // 按原图分辨率合成一张（调用者释放）
