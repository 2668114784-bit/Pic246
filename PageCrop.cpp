// ============================================================================
//  PageCrop.cpp —— 功能页：图片裁切
// ============================================================================
#include "App.h"

using namespace Gdiplus;

static HWND s_hPage = nullptr;
static HWND s_hCanvas = nullptr;
static HWND s_hGrpFill = nullptr;
static HWND s_hSwatch = nullptr;
static HWND s_hBtnColor = nullptr;
static HWND s_hChkTrans = nullptr;
static HWND s_hHint = nullptr;
static HWND s_hInfo = nullptr;
static HWND s_hBtnReset = nullptr;
static HWND s_hBtnSquare = nullptr;
static HWND s_hBtnProcess = nullptr;

static bool FillTransparent()
{
    return Button_GetCheck(s_hChkTrans) == BST_CHECKED;
}

static void PushFillToCanvas()
{
    if (s_hCanvas)
        CropCanvas_SetFill(s_hCanvas, g_fillColor, FillTransparent() && FormatSupportsAlpha());
    if (s_hSwatch)
        InvalidateRect(s_hSwatch, nullptr, TRUE);
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

    wchar_t buf[256];
    swprintf_s(buf, L"裁切区域：%d × %d 像素\r\n起点：%d, %d   后缀：_crop",
               (int)(w + 0.5f), (int)(h + 0.5f), (int)(x + 0.5f), (int)(y + 0.5f));
    SetWindowTextW(s_hInfo, buf);
}

static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);
    const int cx = rc.right;
    const int cy = rc.bottom;
    const int M = D(12);

    int panelW = D(232);
    if (panelW > cx / 2) panelW = cx / 2;

    int canvasW = cx - panelW - M * 3;
    if (canvasW < D(120)) canvasW = D(120);

    MoveWindow(s_hCanvas, M, M, canvasW, cy - M * 2, TRUE);

    int px = M * 2 + canvasW;
    int pw = cx - px - M;
    int py = M;

    int grpH = D(130);
    MoveWindow(s_hGrpFill, px, py, pw, grpH, TRUE);
    MoveWindow(s_hSwatch, px + D(16), py + D(30), D(40), D(26), TRUE);
    MoveWindow(s_hBtnColor, px + D(66), py + D(28), pw - D(82), D(30), TRUE);
    MoveWindow(s_hChkTrans, px + D(16), py + D(64), pw - D(32), D(24), TRUE);
    MoveWindow(s_hHint, px + D(16), py + D(92), pw - D(32), D(30), TRUE);

    py += grpH + D(10);
    MoveWindow(s_hInfo, px, py, pw, D(48), TRUE);

    py += D(52);
    MoveWindow(s_hBtnReset, px, py, pw, D(34), TRUE);
    py += D(40);
    MoveWindow(s_hBtnSquare, px, py, pw, D(34), TRUE);

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

    float x = 0, y = 0, w = 0, h = 0;
    CropCanvas_GetRect(s_hCanvas, &x, &y, &w, &h);

    int nw = (int)(w + 0.5f);
    int nh = (int)(h + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    if ((LONGLONG)nw * nh > 100000000LL)
    {
        wchar_t q[512];
        swprintf_s(q, L"输出尺寸为 %d × %d，大约需要 %lld MB 内存。\r\n\r\n是否继续？",
                   nw, nh, ((LONGLONG)nw * nh * 4) / (1024 * 1024));
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    const bool trans = FillTransparent() && FormatSupportsAlpha();

    SetAppStatus(L"正在处理，请稍候...");
    UpdateWindow(g_hMain);

    Bitmap* dst = new Bitmap(nw, nh, PixelFormat32bppARGB);
    if (dst->GetLastStatus() != Ok)
    {
        delete dst;
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    {
        Graphics g(dst);
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

    SaveBitmapWithSuffix(dst, L"_crop", hPage, nullptr);
}

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;

            s_hCanvas = CropCanvas_Create(hWnd, IDC_CP_CANVAS, false);

            s_hGrpFill = CreateCtl(L"Button", L"裁切框超出图片部分的填充", WS_VISIBLE | BS_GROUPBOX,
                                   hWnd, IDC_CP_GRP_FILL);
            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CP_SWATCH);
            s_hBtnColor = CreateCtl(L"Button", L"选择填充颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_CP_BTN_COLOR);
            s_hChkTrans = CreateCtl(L"Button", L"透明底色（PNG/GIF/TIFF）",
                                    WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, hWnd, IDC_CP_CHK_TRANS);
            s_hHint = CreateCtl(L"Static", L"勾选“透明底色”后，超出图片的部分保持透明；\r\n否则用上面的颜色填充。",
                                WS_VISIBLE, hWnd, -1);

            s_hInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CP_INFO);
            s_hBtnReset = CreateCtl(L"Button", L"重置裁切框（铺满图片）", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_CP_BTN_RESET);
            s_hBtnSquare = CreateCtl(L"Button", L"居中正方形", WS_VISIBLE | WS_TABSTOP,
                                     hWnd, IDC_CP_BTN_SQUARE);
            s_hBtnProcess = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      hWnd, IDC_CP_BTN_PROCESS);

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
        if (wParam == IDC_CP_SWATCH && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            PaintColorSwatch(dis->hDC, dis->rcItem, g_fillColor,
                             FillTransparent() && FormatSupportsAlpha());
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CP_BTN_COLOR:
            g_fillColor = ChooseFillColor(hWnd, g_fillColor);
            PushFillToCanvas();
            return 0;

        case IDC_CP_CHK_TRANS:
            PushFillToCanvas();
            return 0;

        case IDC_CP_BTN_RESET:
            if (s_hCanvas) CropCanvas_ResetToImage(s_hCanvas);
            UpdateInfo();
            return 0;

        case IDC_CP_BTN_SQUARE:
            if (s_hCanvas) CropCanvas_ResetToSquareCenter(s_hCanvas);
            UpdateInfo();
            return 0;

        case IDC_CP_BTN_PROCESS:
            OnProcess(hWnd);
            return 0;

        default:
            break;
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

HWND PageCrop_Create(HWND parent)
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
        wc.lpszClassName = L"Pic246PageCrop";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageCrop", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
