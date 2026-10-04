// ============================================================================
//  App.h —— Pic246 公共声明
//  （全局状态 / 工具函数 / 图片处理 / 交互画布 / 各功能页接口）
// ============================================================================
#pragma once

#include "framework.h"
#include "Project001.h"

#include <objbase.h>
#include <commctrl.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>

#include <stdio.h>
#include <wchar.h>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
// 使用 Windows 视觉样式
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
// 图标资源 ID 兼容
//   VS 资源编辑器里导入/替换图标后，IDI_PROJECT001 之类的符号名可能被改掉
//   （例如新图标会叫 IDI_ICON2）。这里按优先级挑一个存在的，全部不存在时用
//   系统图标兜底，保证以后再换图标也不会因此编译不过。
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

#if defined(IDI_SMALL)
#  define APP_ICON_SMALL_ID  IDI_SMALL
#elif defined(IDI_PROJECT001)
#  define APP_ICON_SMALL_ID  IDI_PROJECT001
#elif defined(IDI_ICON2)
#  define APP_ICON_SMALL_ID  IDI_ICON2
#else
#  define APP_ICON_SMALL_ID  0
#endif

// ---------------------------------------------------------------------------
// 应用信息 / 布局尺寸（单位：DIP，96dpi 下的逻辑像素）
// ---------------------------------------------------------------------------
#define APP_NAME             L"Pic246"
#define APP_WINDOW_TITLE     L"Pic246"
#define APP_MAIN_CLASS       L"Pic246MainWnd"

#define SIDEBAR_W_DIP        168
#define TOPBAR_H_DIP         92
#define STATUSBAR_H_DIP      42

// ---------------------------------------------------------------------------
// 自定义消息
// ---------------------------------------------------------------------------
#define WM_APP_PAGE_SELECT    (WM_APP + 1)   // 侧边栏 -> 主窗口：切到第 wParam 页
#define WM_APP_IMAGE_CHANGED  (WM_APP + 2)   // 主窗口 -> 各页：源图已更换
#define WM_APP_RECT_CHANGED   (WM_APP + 3)   // 交互画布 -> 父页面：裁切框变化

// ---------------------------------------------------------------------------
// 控件 / 命令 ID（每个功能页一个号段，避免冲突）
// ---------------------------------------------------------------------------
enum : int
{
    // 主窗口
    IDC_BTN_OPEN         = 1001,
    IDC_STATIC_PATH      = 1002,
    IDC_STATIC_INFO      = 1003,
    IDC_STATIC_STATUS    = 1004,
    IDC_SIDEBAR          = 1010,

    // 图片尺寸缩放页
    IDC_RS_PREVIEW       = 1100,
    IDC_RS_GRP           = 1101,
    IDC_RS_OUT           = 1102,
    IDC_RS_STATUS        = 1103,
    IDC_RS_BTN_PROCESS   = 1104,
    IDC_RS_RADIO_BASE    = 1110,   // 1110 .. 1114

    // 图片裁切页
    IDC_CP_CANVAS        = 1200,
    IDC_CP_GRP_FILL      = 1201,
    IDC_CP_BTN_COLOR     = 1202,
    IDC_CP_CHK_TRANS     = 1203,
    IDC_CP_BTN_RESET     = 1204,
    IDC_CP_BTN_PROCESS   = 1205,
    IDC_CP_STATUS        = 1206,
    IDC_CP_INFO          = 1207,
    IDC_CP_SWATCH        = 1208,
    IDC_CP_BTN_SQUARE    = 1209,

    // 图片调整页
    IDC_AD_PREVIEW       = 1300,
    IDC_AD_TRK_BLUR      = 1301,
    IDC_AD_TRK_BRIGHT    = 1302,
    IDC_AD_TRK_SAT       = 1303,
    IDC_AD_LBL_BLUR      = 1304,
    IDC_AD_LBL_BRIGHT    = 1305,
    IDC_AD_LBL_SAT       = 1306,
    IDC_AD_BTN_RESET     = 1307,
    IDC_AD_BTN_PROCESS   = 1308,
    IDC_AD_STATUS        = 1309,
    IDC_AD_GRP           = 1310,

    // ICO 转换页
    IDC_IC_CANVAS        = 1400,
    IDC_IC_GRP_SIZE      = 1401,
    IDC_IC_BTN_RESET     = 1402,
    IDC_IC_BTN_PROCESS   = 1403,
    IDC_IC_STATUS        = 1404,
    IDC_IC_INFO          = 1405,
    IDC_IC_CHK_BASE      = 1410,   // 1410 .. 1416
    IDC_IC_GRP_FILL      = 1420,
    IDC_IC_BTN_COLOR     = 1421,
    IDC_IC_CHK_TRANS     = 1422,
    IDC_IC_SWATCH        = 1423,

    // 菜单命令
    IDM_OPEN_IMAGE       = 40001,
    IDM_PROCESS_SAVE     = 40002,
    IDM_PAGE_BASE        = 40010,  // 40010 .. 40013
};

// ICO 可选尺寸
#define ICO_SIZE_COUNT 7
extern const int kIcoSizes[ICO_SIZE_COUNT];

// ---------------------------------------------------------------------------
// 全局状态（定义在 Project001.cpp）
// ---------------------------------------------------------------------------
extern HINSTANCE        g_hInst;
extern HWND             g_hMain;
extern int              g_dpi;
extern HFONT            g_hUiFont;
extern std::wstring     g_srcPath;
extern Gdiplus::Image*  g_pSrcImage;
extern IStream*         g_pSrcStream;
extern UINT             g_srcW;
extern UINT             g_srcH;
extern bool             g_srcHasAlpha;
extern COLORREF         g_fillColor;      // 裁切 / ICO 的填充色（两页共享）

// ---------------------------------------------------------------------------
// 通用工具（定义在 ImageOps.cpp）
// ---------------------------------------------------------------------------
inline int D(int dip) { return MulDiv(dip, g_dpi, 96); }

void SetAppStatus(const std::wstring& text);
void ApplyUiFont(HWND h);
HWND CreateCtl(const wchar_t* cls, const wchar_t* text, DWORD style, HWND parent, int id,
               DWORD exStyle = 0);

std::wstring FileNameOf(const std::wstring& path);
std::wstring DirNameOf(const std::wstring& path);
std::wstring StemOf(const std::wstring& path);
std::wstring ExtOf(const std::wstring& path);
bool         FileExists(const std::wstring& path);
std::wstring IntToStr(long long v);
std::wstring FormatSize(UINT w, UINT h);

// ---------------------------------------------------------------------------
// 图片处理（定义在 ImageOps.cpp）
// ---------------------------------------------------------------------------
struct AdjustParams
{
    int blur;        // 0..40，模糊半径（原图像素）
    int brightness;  // -100..100
    int saturation;  // -100..100
};

bool  LoadImageFile(const std::wstring& path, std::wstring& err);
void  ClearImage();
void  NotifyImageChanged();

Gdiplus::Bitmap* ResizeBitmap(Gdiplus::Image* src, int newW, int newH, std::wstring& err);
Gdiplus::Bitmap* CreatePreviewSource(int maxDim);          // 新建，调用者负责 delete
void  ApplyAdjustments(Gdiplus::Bitmap* bmp, const AdjustParams& p);

bool  SaveBitmapWithSuffix(Gdiplus::Bitmap* bmp, const std::wstring& suffix, HWND owner,
                           std::wstring* donePath);        // 内部会 delete bmp
bool  SaveIcoWithSuffix(Gdiplus::Bitmap* square, const std::vector<int>& sizes, HWND owner,
                        std::wstring* donePath);           // 内部会 delete square
COLORREF ChooseFillColor(HWND owner, COLORREF current);
bool  FormatSupportsAlpha();                               // 当前源图扩展名是否支持透明

void  PaintCheckerBackground(HDC hdc, const RECT& rc);
void  PaintImagePreview(HDC hdc, const RECT& rc, Gdiplus::Bitmap* img, const wchar_t* hint);
void  PaintColorSwatch(HDC hdc, const RECT& rc, COLORREF c, bool transparent);

// ---------------------------------------------------------------------------
// 交互画布（定义在 CropCanvas.cpp）
//   支持拖动移动裁切框、拖 8 个控制点缩放；裁切框允许超出图片范围。
//   squareMode = true 时只保留 4 个角控制点，且永远保持正方形（ICO 页用）。
// ---------------------------------------------------------------------------
#define CROPCANVAS_CLASS L"Pic246CropCanvas"

HWND CropCanvas_Create(HWND parent, int id, bool squareMode);
void CropCanvas_ResetToImage(HWND h);                       // 铺满整张图（不超出）
void CropCanvas_ResetToSquareCenter(HWND h);                // 居中最大正方形
void CropCanvas_GetRect(HWND h, float* x, float* y, float* w, float* hh);
void CropCanvas_SetFill(HWND h, COLORREF c, bool transparent);
void CropCanvas_RefreshImage(HWND h);                       // 源图变化后刷新缓存

// ---------------------------------------------------------------------------
// 各功能页（定义在 Page*.cpp），均返回创建好的页面窗口（初始隐藏）
// ---------------------------------------------------------------------------
HWND PageResize_Create(HWND parent);
HWND PageCrop_Create(HWND parent);
HWND PageAdjust_Create(HWND parent);
HWND PageIco_Create(HWND parent);
