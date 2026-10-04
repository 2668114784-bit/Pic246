// ============================================================================
//  Project001.cpp
//  正方形图片尺寸处理工具（Win32 API + GDI+）
//
//  使用步骤：
//    1) 点击“选择图片...”按钮（或把图片文件直接拖到窗口里）选择一张正方形图片；
//    2) 在“输出尺寸”里选择缩小比例：1/2、1/4、1/6、1/8、1/10（只缩小，不放大）；
//    3) 点击“处理”按钮，结果图片保存到与原图相同的文件夹，
//       文件名后缀为 _2 / _4 / _6 / _8 / _10，扩展名与原图保持一致。
//
//  例：D:\pic\2048.png 选择 1/2  -> D:\pic\2048_2.png  (1024 × 1024)
//      D:\pic\2048.png 选择 1/4  -> D:\pic\2048_4.png  (512  × 512)
//      D:\pic\2048.png 选择 1/10 -> D:\pic\2048_10.png (205  × 205)
//
//  缩放使用 GDI+ 高质量双三次插值（HighQualityBicubic + TileFlipXY），
//  并且先分块读取到内存流再解码，不会锁定原文件。
// ============================================================================

#include "framework.h"
#include "Project001.h"

#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
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
// 使用 Windows 视觉样式（新版按钮 / 单选钮外观）
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 常量 / ID
// ---------------------------------------------------------------------------
#define MAX_LOADSTRING      100
#define APP_CLASS_NAME      L"Project001ImageResizeWnd"
#define APP_WINDOW_TITLE    L"Pic246"

enum : int
{
    IDC_BTN_OPEN        = 1001,
    IDC_BTN_PROCESS     = 1002,
    IDC_GRP_SCALE       = 1003,
    IDC_RADIO_HALF      = 1011,
    IDC_RADIO_QUARTER   = 1012,
    IDC_RADIO_SIXTH     = 1013,
    IDC_RADIO_EIGHTH    = 1014,
    IDC_RADIO_TENTH     = 1015,
    IDC_STATIC_PATH     = 1021,
    IDC_STATIC_INFO     = 1022,
    IDC_STATIC_OUT      = 1023,
    IDC_STATIC_PREVIEW  = 1024,
    IDC_STATIC_STATUS   = 1025,

    IDM_OPEN_IMAGE      = 40001,
    IDM_PROCESS_SAVE    = 40002,
};

// 缩放选项：num/den 为比例，suffix 为输出文件名后缀
struct ScaleOption
{
    const wchar_t* label;
    const wchar_t* suffix;
    int            num;
    int            den;
};

static const ScaleOption kScales[] =
{
    { L"1/2",  L"_2",  1, 2  },
    { L"1/4",  L"_4",  1, 4  },
    { L"1/6",  L"_6",  1, 6  },
    { L"1/8",  L"_8",  1, 8  },
    { L"1/10", L"_10", 1, 10 },
};
static const int kScaleCount = _countof(kScales);

// 支持的输出格式
struct FmtInfo
{
    const wchar_t* ext;
    const wchar_t* mime;
};

static const FmtInfo kFmts[] =
{
    { L".png",  L"image/png"  },
    { L".jpg",  L"image/jpeg" },
    { L".jpeg", L"image/jpeg" },
    { L".bmp",  L"image/bmp"  },
    { L".gif",  L"image/gif"  },
    { L".tif",  L"image/tiff" },
    { L".tiff", L"image/tiff" },
};

// ---------------------------------------------------------------------------
// 图标资源 ID 兼容处理
//
//  注意：在 VS 的“资源视图”里导入/替换图标后，资源编辑器会重写 resource.h，
//  原来的符号名（如 IDI_PROJECT001）可能被改掉或删掉（例如新图标会叫 IDI_ICON2）。
//  所以这里按优先级挑一个当前存在的图标 ID；一个都没有时用系统图标兜底，
//  以后再换图标也不会因此编译不过。
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
// 全局变量
// ---------------------------------------------------------------------------
HINSTANCE   hInst = nullptr;
ULONG_PTR   g_gdiplusToken = 0;

HWND        g_hMain = nullptr;
HWND        g_hBtnOpen = nullptr;
HWND        g_hBtnProcess = nullptr;
HWND        g_hGrpScale = nullptr;
HWND        g_hRadios[kScaleCount] = {};
HWND        g_hPath = nullptr;
HWND        g_hInfo = nullptr;
HWND        g_hOut = nullptr;
HWND        g_hPreview = nullptr;
HWND        g_hStatus = nullptr;

HFONT       g_hFont = nullptr;
HACCEL      g_hAccelExtra = nullptr;
int         g_dpi = 96;

std::wstring    g_srcPath;              // 当前图片完整路径
Image*          g_pSrcImage = nullptr;  // 解码后的原图
IStream*        g_pSrcStream = nullptr; // 内存流（必须与 g_pSrcImage 同生命周期）
UINT            g_srcW = 0;
UINT            g_srcH = 0;
Bitmap*         g_pPreview = nullptr;   // 预览用小图

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static inline int D(int dip) { return MulDiv(dip, g_dpi, 96); }

static void SetStatus(const std::wstring& text)
{
    if (g_hStatus)
        SetWindowTextW(g_hStatus, text.c_str());
}

static std::wstring FileNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

static std::wstring DirNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring() : path.substr(0, p + 1);
}

static const FmtInfo* FindFmt(const std::wstring& ext)
{
    for (int i = 0; i < (int)_countof(kFmts); ++i)
        if (_wcsicmp(kFmts[i].ext, ext.c_str()) == 0)
            return &kFmts[i];
    return nullptr;
}

static bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 计算缩放后的边长（四舍五入，最小 1 像素）
static int ScaledEdge(UINT srcEdge, const ScaleOption& opt)
{
    LONGLONG v = ((LONGLONG)srcEdge * opt.num + opt.den / 2) / opt.den;
    if (v < 1) v = 1;
    return (int)v;
}

// ---------------------------------------------------------------------------
// GDI+ 辅助
// ---------------------------------------------------------------------------
static bool GetEncoderClsid(const wchar_t* mime, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0)
        return false;

    std::vector<BYTE> buf(size);
    ImageCodecInfo* pInfo = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(num, size, pInfo) != Ok)
        return false;

    for (UINT i = 0; i < num; ++i)
    {
        if (_wcsicmp(pInfo[i].MimeType, mime) == 0)
        {
            *pClsid = pInfo[i].Clsid;
            return true;
        }
    }
    return false;
}

// 高质量缩放
static Bitmap* ResizeBitmap(Image* src, int newW, int newH, std::wstring& err)
{
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;

    Bitmap* dst = new Bitmap(newW, newH, PixelFormat32bppARGB);
    if (dst->GetLastStatus() != Ok)
    {
        delete dst;
        err = L"创建目标位图失败（内存不足？）";
        return nullptr;
    }

    {
        Graphics g(dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetCompositingQuality(CompositingQualityHighQuality);
        g.SetCompositingMode(CompositingModeSourceCopy);

        ImageAttributes attr;
        attr.SetWrapMode(WrapModeTileFlipXY);   // 消除边缘黑边

        RectF dest(0.0f, 0.0f, (REAL)newW, (REAL)newH);
        Status st = g.DrawImage(src, dest,
                                0.0f, 0.0f, (REAL)src->GetWidth(), (REAL)src->GetHeight(),
                                UnitPixel, &attr);
        if (st != Ok)
        {
            delete dst;
            err = L"缩放绘制失败。";
            return nullptr;
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// 载入图片
// ---------------------------------------------------------------------------
static void ClearImage()
{
    if (g_pPreview) { delete g_pPreview; g_pPreview = nullptr; }
    if (g_pSrcImage) { delete g_pSrcImage; g_pSrcImage = nullptr; }
    if (g_pSrcStream) { g_pSrcStream->Release(); g_pSrcStream = nullptr; }
    g_srcW = g_srcH = 0;
    g_srcPath.clear();
}

// 先把文件整个读进内存，再用内存流交给 GDI+，避免 GDI+ 长期锁定原文件
static bool LoadImageFile(const std::wstring& path, std::wstring& err)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        err = L"无法打开文件：\r\n" + path;
        return false;
    }

    LARGE_INTEGER liSize = {};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0)
    {
        CloseHandle(hFile);
        err = L"文件为空或无法读取文件大小。";
        return false;
    }
    if (liSize.QuadPart > 1024LL * 1024 * 1024)
    {
        CloseHandle(hFile);
        err = L"文件过大（超过 1 GB），暂不支持。";
        return false;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)liSize.QuadPart);
    if (!hMem)
    {
        CloseHandle(hFile);
        err = L"内存不足，无法读入图片。";
        return false;
    }

    BYTE* pData = (BYTE*)GlobalLock(hMem);
    LONGLONG total = 0;
    bool ok = (pData != nullptr);
    while (ok && total < liSize.QuadPart)
    {
        DWORD want = (DWORD)((liSize.QuadPart - total) > (1 << 20) ? (1 << 20) : (liSize.QuadPart - total));
        DWORD got = 0;
        if (!ReadFile(hFile, pData + total, want, &got, nullptr) || got == 0)
            ok = false;
        else
            total += got;
    }
    GlobalUnlock(hMem);
    CloseHandle(hFile);

    if (!ok)
    {
        GlobalFree(hMem);
        err = L"读取文件失败。";
        return false;
    }

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)))
    {
        GlobalFree(hMem);
        err = L"创建内存流失败。";
        return false;
    }

    Image* pImg = Image::FromStream(pStream);
    if (!pImg || pImg->GetLastStatus() != Ok || pImg->GetWidth() == 0 || pImg->GetHeight() == 0)
    {
        delete pImg;
        pStream->Release();
        err = L"无法解析该图片，格式不受支持或文件已损坏。\r\n\r\n支持：PNG / JPG / BMP / GIF / TIFF";
        return false;
    }

    ClearImage();
    g_pSrcImage = pImg;
    g_pSrcStream = pStream;          // 与图片同生命周期
    g_srcW = pImg->GetWidth();
    g_srcH = pImg->GetHeight();
    g_srcPath = path;
    return true;
}

// 预览小图（最长边 512 像素，绘制时再自适应缩放）
static void BuildPreview()
{
    if (g_pPreview) { delete g_pPreview; g_pPreview = nullptr; }
    if (!g_pSrcImage) return;

    const int kMax = 512;
    double s = 1.0;
    if ((UINT)kMax < g_srcW || (UINT)kMax < g_srcH)
    {
        double sx = (double)kMax / (double)g_srcW;
        double sy = (double)kMax / (double)g_srcH;
        s = (sx < sy) ? sx : sy;
    }

    int pw = (int)(g_srcW * s + 0.5);
    int ph = (int)(g_srcH * s + 0.5);

    std::wstring err;
    g_pPreview = ResizeBitmap(g_pSrcImage, pw, ph, err);
}

// ---------------------------------------------------------------------------
// 界面文本
// ---------------------------------------------------------------------------
static int CurrentScaleIndex()
{
    for (int i = 0; i < kScaleCount; ++i)
        if (Button_GetCheck(g_hRadios[i]) == BST_CHECKED)
            return i;
    return 0;
}

static void UpdateInfoLabel()
{
    wchar_t buf[256];
    if (!g_pSrcImage)
    {
        wcscpy_s(buf, L"尚未选择图片。点击「选择图片...」，或把图片文件拖到窗口中。");
    }
    else if (g_srcW == g_srcH)
    {
        swprintf_s(buf, L"原图尺寸：%u × %u 像素（正方形）", g_srcW, g_srcH);
    }
    else
    {
        swprintf_s(buf, L"原图尺寸：%u × %u 像素（非正方形，将按相同比例缩放）", g_srcW, g_srcH);
    }
    SetWindowTextW(g_hInfo, buf);
}

static void UpdateOutLabel()
{
    if (!g_hOut) return;

    wchar_t buf[256];
    if (!g_pSrcImage)
    {
        wcscpy_s(buf, L"输出尺寸：—");
    }
    else
    {
        const ScaleOption& opt = kScales[CurrentScaleIndex()];
        int nw = ScaledEdge(g_srcW, opt);
        int nh = ScaledEdge(g_srcH, opt);
        swprintf_s(buf, L"输出尺寸：%d × %d 像素   后缀：%s", nw, nh, opt.suffix);
    }
    SetWindowTextW(g_hOut, buf);
}

// ---------------------------------------------------------------------------
// 预览绘制（SS_OWNERDRAW 静态控件的 WM_DRAWITEM）
// ---------------------------------------------------------------------------
static void PaintPreview(const DRAWITEMSTRUCT* dis)
{
    const int w = dis->rcItem.right - dis->rcItem.left;
    const int h = dis->rcItem.bottom - dis->rcItem.top;
    if (w <= 0 || h <= 0) return;

    HDC hdc = dis->hDC;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT all = { 0, 0, w, h };
    HBRUSH bWhite = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH bGray = CreateSolidBrush(RGB(237, 239, 241));
    FillRect(mem, &all, bWhite);

    // 透明棋盘格背景
    const int cell = D(10);
    if (cell > 0)
    {
        for (int y = 0; y < h; y += cell)
        {
            for (int x = 0; x < w; x += cell)
            {
                if (((x / cell) + (y / cell)) & 1)
                {
                    RECT c;
                    c.left = x;
                    c.top = y;
                    c.right = (x + cell < w) ? (x + cell) : w;
                    c.bottom = (y + cell < h) ? (y + cell) : h;
                    FillRect(mem, &c, bGray);
                }
            }
        }
    }

    if (g_pPreview)
    {
        Graphics g(mem);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        const int pad = D(8);
        int aw = w - pad * 2;
        int ah = h - pad * 2;
        if (aw < 1) aw = 1;
        if (ah < 1) ah = 1;

        int iw = (int)g_pPreview->GetWidth();
        int ih = (int)g_pPreview->GetHeight();
        double s = (double)aw / iw;
        double sy = (double)ah / ih;
        if (sy < s) s = sy;

        int dw = (int)(iw * s + 0.5);
        int dh = (int)(ih * s + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        REAL x = (REAL)((w - dw) / 2.0);
        REAL y = (REAL)((h - dh) / 2.0);
        g.DrawImage(g_pPreview, RectF(x, y, (REAL)dw, (REAL)dh),
                    0, 0, (REAL)iw, (REAL)ih, UnitPixel);
    }
    else
    {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(110, 116, 124));
        HGDIOBJ oldFont = SelectObject(mem, g_hFont);
        DrawTextW(mem,
                  L"点击「选择图片...」载入图片\r\n也可以直接把图片文件拖到窗口中",
                  -1, &all, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(mem, oldFont);
    }

    HBRUSH bBorder = CreateSolidBrush(RGB(205, 208, 212));
    FrameRect(mem, &all, bBorder);
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    DeleteObject(bBorder);
    DeleteObject(bWhite);
    DeleteObject(bGray);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------------------
// 窗口布局
// ---------------------------------------------------------------------------
static void LayoutControls(HWND hWnd)
{
    RECT rc;
    GetClientRect(hWnd, &rc);
    const int cx = rc.right - rc.left;
    const int cy = rc.bottom - rc.top;
    const int M = D(16);
    const int gap = D(10);

    // 第一行：选择图片按钮 + 路径
    int y = D(16);
    int hRow = D(34);
    MoveWindow(g_hBtnOpen, M, y, D(140), hRow, TRUE);
    int x = M + D(140) + gap;
    MoveWindow(g_hPath, x, y, cx - x - M, hRow, TRUE);

    // 第二行：原图信息
    y += hRow + D(8);
    MoveWindow(g_hInfo, M, y + D(4), cx - M * 2, D(20), TRUE);

    // 输出尺寸分组框
    y += D(26);
    int grpH = D(92);
    MoveWindow(g_hGrpScale, M, y, cx - M * 2, grpH, TRUE);

    int ry = y + D(36);
    int rx = M + D(20);
    for (int i = 0; i < kScaleCount; ++i)
    {
        MoveWindow(g_hRadios[i], rx, ry, D(76), D(26), TRUE);
        rx += D(84);
    }
    MoveWindow(g_hOut, rx + D(16), ry, cx - M - (rx + D(16)), D(26), TRUE);

    // 预览区
    y += grpH + D(14);
    int bottomH = D(40);
    int previewH = cy - y - bottomH - M * 2;
    if (previewH < D(80)) previewH = D(80);
    MoveWindow(g_hPreview, M, y, cx - M * 2, previewH, TRUE);

    // 底部：处理按钮 + 状态栏
    int by = cy - M - bottomH;
    MoveWindow(g_hBtnProcess, M, by, D(150), bottomH, TRUE);
    int sx = M + D(150) + gap;
    MoveWindow(g_hStatus, sx, by, cx - sx - M, bottomH, TRUE);
}

// ---------------------------------------------------------------------------
// 选择 / 载入图片
// ---------------------------------------------------------------------------
static void OpenImagePath(HWND hWnd, const std::wstring& path)
{
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring err;
    bool ok = LoadImageFile(path, err);
    if (ok)
        BuildPreview();

    SetCursor(hOld);

    if (!ok)
    {
        MessageBoxW(hWnd, err.c_str(), L"打开失败", MB_OK | MB_ICONERROR);
        SetStatus(L"载入失败：" + FileNameOf(path));
        return;
    }

    SetWindowTextW(g_hPath, path.c_str());
    UpdateInfoLabel();
    UpdateOutLabel();
    InvalidateRect(g_hPreview, nullptr, TRUE);

    wchar_t buf[512];
    swprintf_s(buf, L"已载入 %s（%u × %u）。请选择输出尺寸后点击「处理」。",
               FileNameOf(path).c_str(), g_srcW, g_srcH);
    SetStatus(buf);
}

static void OnOpenImage(HWND hWnd)
{
    std::vector<wchar_t> buf(32768, 0);
    if (!g_srcPath.empty())
        wcsncpy_s(buf.data(), buf.size(), g_srcPath.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter =
        L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
        L"PNG 图片 (*.png)\0*.png\0"
        L"JPEG 图片 (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
        L"BMP 图片 (*.bmp)\0*.bmp\0"
        L"所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = L"选择一张（正方形）图片";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn))
        return;

    OpenImagePath(hWnd, std::wstring(buf.data()));
}

// ---------------------------------------------------------------------------
// 处理
// ---------------------------------------------------------------------------
static void OnProcessImage(HWND hWnd)
{
    if (!g_pSrcImage)
    {
        MessageBoxW(hWnd, L"请先点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const ScaleOption& opt = kScales[CurrentScaleIndex()];
    const int newW = ScaledEdge(g_srcW, opt);
    const int newH = ScaledEdge(g_srcH, opt);

    if (g_srcW != g_srcH)
    {
        wchar_t q[512];
        swprintf_s(q, L"当前图片不是正方形（%u × %u）。\r\n\r\n是否仍按相同比例缩放并输出？",
                   g_srcW, g_srcH);
        if (MessageBoxW(hWnd, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    // 大图内存提示
    LONGLONG pixels = (LONGLONG)newW * newH;
    if (pixels > 100000000LL)
    {
        wchar_t q[512];
        swprintf_s(q, L"输出尺寸为 %d × %d，大约需要 %lld MB 内存。\r\n\r\n是否继续？",
                   newW, newH, (pixels * 4) / (1024 * 1024));
        if (MessageBoxW(hWnd, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    // 计算输出路径：同目录 + 原文件名 + 后缀 + 原扩展名
    const std::wstring dir = DirNameOf(g_srcPath);
    std::wstring name = FileNameOf(g_srcPath);
    std::wstring stem = name;
    std::wstring ext;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
    {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }

    const FmtInfo* fmt = FindFmt(ext);
    if (!fmt)
    {
        ext = L".png";                 // 扩展名不认识时，统一按 PNG 输出
        fmt = FindFmt(ext);
    }

    std::wstring outPath = dir + stem + opt.suffix + ext;

    if (FileExists(outPath))
    {
        wchar_t q[1024];
        swprintf_s(q, L"输出文件已存在：\r\n%s\r\n\r\n是否覆盖？", outPath.c_str());
        if (MessageBoxW(hWnd, q, L"确认覆盖", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            SetStatus(L"已取消：输出文件已存在。");
            return;
        }
    }

    CLSID clsid = {};
    if (!GetEncoderClsid(fmt->mime, &clsid))
    {
        MessageBoxW(hWnd, L"系统中找不到对应的图片编码器，无法保存。", L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }

    // 处理中
    SetStatus(L"正在处理，请稍候...");
    UpdateWindow(g_hStatus);
    EnableWindow(g_hBtnOpen, FALSE);
    EnableWindow(g_hBtnProcess, FALSE);
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring err;
    Bitmap* dst = ResizeBitmap(g_pSrcImage, newW, newH, err);
    Status st = GenericError;
    bool saved = false;

    if (dst)
    {
        // GDI+ 覆盖已存在文件可能失败，先删掉
        if (FileExists(outPath))
            DeleteFileW(outPath.c_str());

        if (_wcsicmp(fmt->mime, L"image/jpeg") == 0)
        {
            ULONG quality = 95;
            EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid = EncoderQuality;
            ep.Parameter[0].Type = EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ep.Parameter[0].Value = &quality;
            st = dst->Save(outPath.c_str(), &clsid, &ep);
        }
        else
        {
            st = dst->Save(outPath.c_str(), &clsid, nullptr);
        }
        saved = (st == Ok);
        delete dst;
        dst = nullptr;
    }

    SetCursor(hOld);
    EnableWindow(g_hBtnOpen, TRUE);
    EnableWindow(g_hBtnProcess, TRUE);

    if (!saved)
    {
        if (err.empty())
            err = L"保存文件失败（目标文件可能被其它程序占用，或没有写入权限）。";
        MessageBoxW(hWnd, err.c_str(), L"处理失败", MB_OK | MB_ICONERROR);
        SetStatus(L"处理失败：" + err);
        return;
    }

    wchar_t msg[1200];
    swprintf_s(msg, L"处理完成！\r\n\r\n输出文件：\r\n%s\r\n\r\n输出尺寸：%d × %d 像素\r\n\r\n是否打开输出文件所在的文件夹？",
               outPath.c_str(), newW, newH);

    SetStatus(std::wstring(L"处理完成：") + outPath + L"  (" +
              std::to_wstring(newW) + L" × " + std::to_wstring(newH) + L")");

    if (MessageBoxW(hWnd, msg, L"处理完成", MB_YESNO | MB_ICONINFORMATION) == IDYES)
    {
        std::wstring arg = L"/select,\"" + outPath + L"\"";
        ShellExecuteW(hWnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

// ---------------------------------------------------------------------------
// 菜单（在资源菜单“文件”里追加两项，不修改 .rc）
// ---------------------------------------------------------------------------
static void BuildMenu(HWND hWnd)
{
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;

    HMENU hFile = GetSubMenu(hMenu, 0);
    if (!hFile) return;

    InsertMenuW(hFile, 0, MF_BYPOSITION | MF_STRING, IDM_OPEN_IMAGE, L"打开图片(&O)...\tCtrl+O");
    InsertMenuW(hFile, 1, MF_BYPOSITION | MF_STRING, IDM_PROCESS_SAVE, L"处理并保存(&S)\tCtrl+S");
    InsertMenuW(hFile, 2, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    DrawMenuBar(hWnd);
}

// ---------------------------------------------------------------------------
// 创建控件
// ---------------------------------------------------------------------------
static HFONT CreateUiFont()
{
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
        HFONT h = CreateFontIndirectW(&ncm.lfMessageFont);
        if (h) return h;
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void ApplyFont(HWND h)
{
    if (h && g_hFont)
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
}

static void CreateControls(HWND hWnd)
{
    g_hFont = CreateUiFont();

    g_hBtnOpen = CreateWindowExW(0, L"Button", L"选择图片...",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_BTN_OPEN, hInst, nullptr);

    g_hPath = CreateWindowExW(0, L"Static", L"",
                              WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_CENTERIMAGE,
                              0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_STATIC_PATH, hInst, nullptr);

    g_hInfo = CreateWindowExW(0, L"Static", L"",
                              WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                              0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_STATIC_INFO, hInst, nullptr);

    g_hGrpScale = CreateWindowExW(0, L"Button", L"输出尺寸",
                                  WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                  0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_GRP_SCALE, hInst, nullptr);

    static const int kRadioIds[kScaleCount] =
    {
        IDC_RADIO_HALF, IDC_RADIO_QUARTER, IDC_RADIO_SIXTH, IDC_RADIO_EIGHTH, IDC_RADIO_TENTH
    };
    for (int i = 0; i < kScaleCount; ++i)
    {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
        if (i == 0) style |= WS_GROUP;
        g_hRadios[i] = CreateWindowExW(0, L"Button", kScales[i].label, style,
                                       0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)kRadioIds[i], hInst, nullptr);
    }
    Button_SetCheck(g_hRadios[0], BST_CHECKED);

    g_hOut = CreateWindowExW(0, L"Static", L"",
                             WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                             0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_STATIC_OUT, hInst, nullptr);

    g_hPreview = CreateWindowExW(0, L"Static", L"",
                                 WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                 0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_STATIC_PREVIEW, hInst, nullptr);

    g_hBtnProcess = CreateWindowExW(0, L"Button", L"处理",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_BTN_PROCESS, hInst, nullptr);

    g_hStatus = CreateWindowExW(0, L"Static", L"",
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)IDC_STATIC_STATUS, hInst, nullptr);

    HWND all[] = { g_hBtnOpen, g_hPath, g_hInfo, g_hGrpScale, g_hOut, g_hPreview, g_hBtnProcess, g_hStatus };
    for (int i = 0; i < (int)_countof(all); ++i)
        ApplyFont(all[i]);
    for (int i = 0; i < kScaleCount; ++i)
        ApplyFont(g_hRadios[i]);

    UpdateInfoLabel();
    UpdateOutLabel();
    SetStatus(L"就绪：请选择一张图片。");
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        g_hMain = hWnd;
        CreateControls(hWnd);
        BuildMenu(hWnd);
        DragAcceptFiles(hWnd, TRUE);
        LayoutControls(hWnd);
        SetFocus(g_hBtnProcess);
        return 0;

    case WM_SIZE:
        LayoutControls(hWnd);
        if (g_hPreview)
            InvalidateRect(g_hPreview, nullptr, TRUE);
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            RECT wr = {}, cr = {};
            GetWindowRect(hWnd, &wr);
            GetClientRect(hWnd, &cr);
            int ncx = (wr.right - wr.left) - (cr.right - cr.left);
            int ncy = (wr.bottom - wr.top) - (cr.bottom - cr.top);
            mmi->ptMinTrackSize.x = D(760) + ncx;
            mmi->ptMinTrackSize.y = D(560) + ncy;
        }
        return 0;

    case WM_DRAWITEM:
        if (wParam == IDC_STATIC_PREVIEW)
        {
            PaintPreview((const DRAWITEMSTRUCT*)lParam);
            return TRUE;
        }
        break;

    case WM_DROPFILES:
        {
            HDROP hDrop = (HDROP)wParam;
            UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            if (n > 0)
            {
                std::vector<wchar_t> file(32768, 0);
                if (DragQueryFileW(hDrop, 0, file.data(), (UINT)file.size()) > 0)
                    OpenImagePath(hWnd, std::wstring(file.data()));
            }
            DragFinish(hDrop);
        }
        return 0;

    case WM_COMMAND:
        {
            const int wmId = LOWORD(wParam);
            const int wmEvent = HIWORD(wParam);

            switch (wmId)
            {
            case IDC_BTN_OPEN:
            case IDM_OPEN_IMAGE:
                OnOpenImage(hWnd);
                break;

            case IDC_BTN_PROCESS:
            case IDM_PROCESS_SAVE:
                OnProcessImage(hWnd);
                break;

            case IDC_RADIO_HALF:
            case IDC_RADIO_QUARTER:
            case IDC_RADIO_SIXTH:
            case IDC_RADIO_EIGHTH:
            case IDC_RADIO_TENTH:
                if (wmEvent == BN_CLICKED)
                    UpdateOutLabel();
                break;

            case IDM_ABOUT:
                DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_ABOUTBOX), hWnd, About);
                break;

            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;

            default:
                return DefWindowProcW(hWnd, message, wParam, lParam);
            }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        DragAcceptFiles(hWnd, FALSE);
        ClearImage();
        if (g_hAccelExtra) { DestroyAcceleratorTable(g_hAccelExtra); g_hAccelExtra = nullptr; }
        if (g_hFont) { DeleteObject(g_hFont); g_hFont = nullptr; }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 注册窗口类 / 初始化实例
// ---------------------------------------------------------------------------
// 依次尝试若干图标资源 ID，全部失败则退回系统默认图标
static HICON LoadAppIcon(HINSTANCE hInstance, int cx, int cy, const int* ids, int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (ids[i] == 0)
            continue;

        HICON h = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(ids[i]), IMAGE_ICON,
                                    cx, cy, LR_SHARED);
        if (h)
            return h;
    }
    return LoadIconW(nullptr, IDI_APPLICATION);
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    const int bigIds[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };
    const int smlIds[2] = { APP_ICON_SMALL_ID, APP_ICON_MAIN_ID };

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                             bigIds, 2);
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_PROJECT001);
    wcex.lpszClassName = APP_CLASS_NAME;
    wcex.hIconSm = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                               smlIds, 2);
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    RECT rc = { 0, 0, D(940), D(680) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0);

    HWND hWnd = CreateWindowExW(0, APP_CLASS_NAME, APP_WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hWnd)
        return FALSE;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    return TRUE;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 高 DPI：让系统按显示器缩放，字体/控件不糊
    SetProcessDPIAware();
    {
        HDC hdc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        if (g_dpi < 96) g_dpi = 96;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) != Ok)
    {
        MessageBoxW(nullptr, L"GDI+ 初始化失败，程序无法启动。", L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow))
    {
        GdiplusShutdown(g_gdiplusToken);
        return FALSE;
    }

    HACCEL hAccelTable = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDC_PROJECT001));

    ACCEL extra[2] = {};
    extra[0].fVirt = FCONTROL | FVIRTKEY; extra[0].key = 'O'; extra[0].cmd = IDM_OPEN_IMAGE;
    extra[1].fVirt = FCONTROL | FVIRTKEY; extra[1].key = 'S'; extra[1].cmd = IDM_PROCESS_SAVE;
    g_hAccelExtra = CreateAcceleratorTableW(extra, 2);

    // 命令行传入图片路径时直接打开（也支持把图片拖到 exe 图标上）
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            for (int i = 1; i < argc; ++i)
            {
                if (FileExists(argv[i]))
                {
                    OpenImagePath(g_hMain, argv[i]);
                    break;
                }
            }
            LocalFree(argv);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (hAccelTable && TranslateAcceleratorW(msg.hwnd, hAccelTable, &msg))
            continue;
        if (g_hAccelExtra && TranslateAcceleratorW(msg.hwnd, g_hAccelExtra, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}

// “关于”框的消息处理程序
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        {
            // .rc 里“关于”对话框引用的图标 ID 也可能被资源编辑器改掉，
            // 这里不管资源里写的是哪个 ID，直接用程序图标补上。
            const int ids[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };
            HICON hBig = LoadAppIcon(hInst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), ids, 2);
            HICON hSm = LoadAppIcon(hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ids, 2);

            SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hBig);
            SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSm);

            HWND hChild = nullptr;
            while ((hChild = FindWindowExW(hDlg, hChild, L"Static", nullptr)) != nullptr)
            {
                if ((GetWindowLongW(hChild, GWL_STYLE) & SS_TYPEMASK) == SS_ICON)
                {
                    SendMessageW(hChild, STM_SETICON, (WPARAM)hBig, 0);
                    break;
                }
            }
        }
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
