// ============================================================================
//  PageIco.cpp —— 功能页：ICO 转换
// ============================================================================
#include "App.h"

using namespace Gdiplus;

static HWND s_hPage = nullptr;
static HWND s_hCanvas = nullptr;
static HWND s_hGrpSize = nullptr;
static HWND s_hGrpFill = nullptr;
static HWND s_hSwatch = nullptr;
static HWND s_hBtnColor = nullptr;
static HWND s_hChkTrans = nullptr;
static HWND s_hChks[ICO_SIZE_COUNT] = {};
static HWND s_hInfo = nullptr;
static HWND s_hBtnReset = nullptr;
static HWND s_hBtnProcess = nullptr;

// 默认勾选：16 / 32 / 48 / 256
static const bool kDefaultChecked[ICO_SIZE_COUNT] = { true, false, true, true, false, false, true };

static bool FillTransparent()
{
    return Button_GetCheck(s_hChkTrans) == BST_CHECKED;
}

static void PushFillToCanvas()
{
    if (s_hCanvas)
        CropCanvas_SetFill(s_hCanvas, g_fillColor, FillTransparent());
    if (s_hSwatch)
        InvalidateRect(s_hSwatch, nullptr, TRUE);
}

static std::vector<int> CheckedSizes()
{
    std::vector<int> v;
    for (int i = 0; i < ICO_SIZE_COUNT; ++i)
        if (Button_GetCheck(s_hChks[i]) == BST_CHECKED)
            v.push_back(kIcoSizes[i]);
    return v;
}

static void UpdateInfo()
{
    if (!s_hInfo) return;
    if (!g_pSrcImage || !s_hCanvas)
    {
        SetWindowTextW(s_hInfo, L"裁切区域：—");
        return;
    }

    float x = 0, y = 0, w = 0, h = 0;
    CropCanvas_GetRect(s_hCanvas, &x, &y, &w, &h);
    std::vector<int> sizes = CheckedSizes();

    std::wstring list;
    for (size_t i = 0; i < sizes.size(); ++i)
    {
        if (i) list += L", ";
        list += IntToStr(sizes[i]);
    }
    if (list.empty()) list = L"（未勾选）";

    wchar_t buf[512];
    swprintf_s(buf, L"正方形区域：%d × %d 像素\r\n输出尺寸：%s\r\n后缀：_ico",
               (int)(w + 0.5f), (int)(h + 0.5f), list.c_str());
    SetWindowTextW(s_hInfo, buf);
}

static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);
    const int cx = rc.right;
    const int cy = rc.bottom;
    const int M = D(12);

    int panelW = D(250);
    if (panelW > cx / 2) panelW = cx / 2;

    int canvasW = cx - panelW - M * 3;
    if (canvasW < D(120)) canvasW = D(120);
    MoveWindow(s_hCanvas, M, M, canvasW, cy - M * 2, TRUE);

    int px = M * 2 + canvasW;
    int pw = cx - px - M;
    int py = M;

    int grpH = D(168);
    MoveWindow(s_hGrpSize, px, py, pw, grpH, TRUE);
    {
        int colW = (pw - D(30)) / 2;
        for (int i = 0; i < ICO_SIZE_COUNT; ++i)
        {
            int col = i % 2;
            int row = i / 2;
            MoveWindow(s_hChks[i], px + D(16) + col * colW, py + D(32) + row * D(32),
                       colW, D(26), TRUE);
        }
    }

    py += grpH + D(10);
    int fillH = D(122);
    MoveWindow(s_hGrpFill, px, py, pw, fillH, TRUE);
    MoveWindow(s_hSwatch, px + D(16), py + D(30), D(40), D(26), TRUE);
    MoveWindow(s_hBtnColor, px + D(66), py + D(28), pw - D(82), D(30), TRUE);
    MoveWindow(s_hChkTrans, px + D(16), py + D(64), pw - D(32), D(24), TRUE);

    py += fillH + D(8);
    MoveWindow(s_hInfo, px, py, pw, D(70), TRUE);

    py += D(74);
    MoveWindow(s_hBtnReset, px, py, pw, D(34), TRUE);

    int btnH = D(42);
    MoveWindow(s_hBtnProcess, px, cy - M - btnH, pw, btnH, TRUE);
}

static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage || !s_hCanvas)
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::vector<int> sizes = CheckedSizes();
    if (sizes.empty())
    {
        MessageBoxW(hPage, L"请至少勾选一个 ICO 尺寸。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    float x = 0, y = 0, w = 0, h = 0;
    CropCanvas_GetRect(s_hCanvas, &x, &y, &w, &h);

    int side = (int)((w + h) * 0.5f + 0.5f);
    if (side < 4) side = 4;
    if (side > 8192)
    {
        wchar_t q[512];
        swprintf_s(q, L"正方形区域为 %d × %d，尺寸偏大。\r\n\r\n是否继续？", side, side);
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    const bool trans = FillTransparent();

    SetAppStatus(L"正在处理，请稍候...");
    UpdateWindow(g_hMain);

    Bitmap* sq = new Bitmap(side, side, PixelFormat32bppARGB);
    if (sq->GetLastStatus() != Ok)
    {
        delete sq;
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    {
        Graphics g(sq);
        g.SetCompositingMode(CompositingModeSourceCopy);
        if (trans)
            g.Clear(Color(0, 0, 0, 0));
        else
            g.Clear(Color(255, GetRValue(g_fillColor), GetGValue(g_fillColor), GetBValue(g_fillColor)));

        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetCompositingQuality(CompositingQualityHighQuality);

        ImageAttributes attr;
        attr.SetWrapMode(WrapModeTileFlipXY);

        RectF dest(-x, -y, (REAL)g_srcW, (REAL)g_srcH);
        g.DrawImage(g_pSrcImage, dest,
                    0.0f, 0.0f, (REAL)g_srcW, (REAL)g_srcH, UnitPixel, &attr);
    }

    SaveIcoWithSuffix(sq, sizes, hPage, nullptr);
}

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;

            s_hCanvas = CropCanvas_Create(hWnd, IDC_IC_CANVAS, true);

            s_hGrpSize = CreateCtl(L"Button", L"输出的 ICO 尺寸（像素）", WS_VISIBLE | BS_GROUPBOX,
                                   hWnd, IDC_IC_GRP_SIZE);
            for (int i = 0; i < ICO_SIZE_COUNT; ++i)
            {
                wchar_t text[16];
                swprintf_s(text, L"%d", kIcoSizes[i]);
                s_hChks[i] = CreateCtl(L"Button", text, WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       hWnd, IDC_IC_CHK_BASE + i);
                if (kDefaultChecked[i])
                    Button_SetCheck(s_hChks[i], BST_CHECKED);
            }

            s_hGrpFill = CreateCtl(L"Button", L"超出图片部分的填充", WS_VISIBLE | BS_GROUPBOX,
                                   hWnd, IDC_IC_GRP_FILL);
            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_IC_SWATCH);
            s_hBtnColor = CreateCtl(L"Button", L"选择填充颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_IC_BTN_COLOR);
            s_hChkTrans = CreateCtl(L"Button", L"透明（推荐）", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    hWnd, IDC_IC_CHK_TRANS);
            Button_SetCheck(s_hChkTrans, BST_CHECKED);

            s_hInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_IC_INFO);
            s_hBtnReset = CreateCtl(L"Button", L"重置裁切框（居中正方形）", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_IC_BTN_RESET);
            s_hBtnProcess = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      hWnd, IDC_IC_BTN_PROCESS);

            PushFillToCanvas();
            UpdateInfo();
            Layout();
        }
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_APP_IMAGE_CHANGED:
        if (s_hCanvas)
            CropCanvas_RefreshImage(s_hCanvas);
        PushFillToCanvas();
        UpdateInfo();
        return 0;

    case WM_APP_RECT_CHANGED:
        UpdateInfo();
        return 0;

    case WM_DRAWITEM:
        if (wParam == IDC_IC_SWATCH && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            PaintColorSwatch(dis->hDC, dis->rcItem, g_fillColor, FillTransparent());
            return TRUE;
        }
        break;

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);

            if (id == IDC_IC_BTN_COLOR)
            {
                g_fillColor = ChooseFillColor(hWnd, g_fillColor);
                PushFillToCanvas();
                return 0;
            }
            if (id == IDC_IC_CHK_TRANS)
            {
                PushFillToCanvas();
                return 0;
            }
            if (id == IDC_IC_BTN_RESET)
            {
                if (s_hCanvas) CropCanvas_ResetToSquareCenter(s_hCanvas);
                UpdateInfo();
                return 0;
            }
            if (id == IDC_IC_BTN_PROCESS)
            {
                OnProcess(hWnd);
                return 0;
            }
            if (id >= IDC_IC_CHK_BASE && id < IDC_IC_CHK_BASE + ICO_SIZE_COUNT)
            {
                UpdateInfo();
                return 0;
            }
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

HWND PageIco_Create(HWND parent)
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
        wc.lpszClassName = L"Pic246PageIco";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageIco", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
