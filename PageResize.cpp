// ============================================================================
//  PageResize.cpp —— 功能页：图片尺寸缩放
// ============================================================================
#include "App.h"

using namespace Gdiplus;

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

static HWND    s_hPage = nullptr;
static HWND    s_hPreview = nullptr;
static HWND    s_hGrp = nullptr;
static HWND    s_hOut = nullptr;
static HWND    s_hBtn = nullptr;
static HWND    s_hRadios[kScaleCount] = {};
static Bitmap* s_pPreview = nullptr;

static int ScaledEdge(UINT srcEdge, const ScaleOption& opt)
{
    LONGLONG v = ((LONGLONG)srcEdge * opt.num + opt.den / 2) / opt.den;
    if (v < 1) v = 1;
    return (int)v;
}

static int CurrentScaleIndex()
{
    for (int i = 0; i < kScaleCount; ++i)
        if (Button_GetCheck(s_hRadios[i]) == BST_CHECKED)
            return i;
    return 0;
}

static void UpdateOutLabel()
{
    wchar_t buf[256];
    if (!g_pSrcImage)
    {
        wcscpy_s(buf, L"输出尺寸：—");
    }
    else
    {
        const ScaleOption& opt = kScales[CurrentScaleIndex()];
        swprintf_s(buf, L"输出尺寸：%d × %d 像素    后缀：%s",
                   ScaledEdge(g_srcW, opt), ScaledEdge(g_srcH, opt), opt.suffix);
    }
    SetWindowTextW(s_hOut, buf);
}

static void RebuildPreview()
{
    if (s_pPreview) { delete s_pPreview; s_pPreview = nullptr; }
    s_pPreview = CreatePreviewSource(640);
}

static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);
    const int cx = rc.right;
    const int cy = rc.bottom;
    const int M = D(12);
    const int grpH = D(84);
    const int btnH = D(42);

    int previewH = cy - M - grpH - M - btnH - M;
    if (previewH < D(100)) previewH = D(100);

    MoveWindow(s_hPreview, M, M, cx - M * 2, previewH, TRUE);

    int gy = M + previewH + M;
    MoveWindow(s_hGrp, M, gy, cx - M * 2, grpH, TRUE);

    int rx = M + D(20);
    int ry = gy + D(34);
    for (int i = 0; i < kScaleCount; ++i)
    {
        MoveWindow(s_hRadios[i], rx, ry, D(76), D(26), TRUE);
        rx += D(84);
    }
    MoveWindow(s_hOut, rx + D(12), ry, cx - M - (rx + D(12)), D(26), TRUE);

    MoveWindow(s_hBtn, M, cy - M - btnH, D(150), btnH, TRUE);
}

static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
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
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    SetAppStatus(L"正在处理，请稍候...");
    UpdateWindow(g_hMain);

    std::wstring err;
    Bitmap* bmp = ResizeBitmap(g_pSrcImage, newW, newH, err);
    if (!bmp)
    {
        MessageBoxW(hPage, err.c_str(), L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：" + err);
        return;
    }

    SaveBitmapWithSuffix(bmp, opt.suffix, hPage, nullptr);
}

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;

            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_RS_PREVIEW);
            s_hGrp = CreateCtl(L"Button", L"输出尺寸", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_RS_GRP);

            for (int i = 0; i < kScaleCount; ++i)
            {
                DWORD style = WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
                if (i == 0) style |= WS_GROUP;
                s_hRadios[i] = CreateCtl(L"Button", kScales[i].label, style, hWnd,
                                         IDC_RS_RADIO_BASE + i);
            }
            Button_SetCheck(s_hRadios[0], BST_CHECKED);

            s_hOut = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_RS_OUT);
            s_hBtn = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_RS_BTN_PROCESS);

            UpdateOutLabel();
            Layout();
        }
        return 0;

    case WM_SIZE:
        Layout();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    case WM_APP_IMAGE_CHANGED:
        RebuildPreview();
        UpdateOutLabel();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    case WM_DRAWITEM:
        if (wParam == IDC_RS_PREVIEW && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            PaintImagePreview(dis->hDC, dis->rcItem, s_pPreview,
                              L"点击上方「选择图片...」载入图片\r\n也可以直接把图片文件拖进窗口");
            return TRUE;
        }
        break;

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            if (id == IDC_RS_BTN_PROCESS)
            {
                OnProcess(hWnd);
                return 0;
            }
            if (id >= IDC_RS_RADIO_BASE && id < IDC_RS_RADIO_BASE + kScaleCount)
            {
                if (HIWORD(wParam) == BN_CLICKED)
                    UpdateOutLabel();
                return 0;
            }
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

HWND PageResize_Create(HWND parent)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = Proc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Pic246PageResize";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageResize", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
