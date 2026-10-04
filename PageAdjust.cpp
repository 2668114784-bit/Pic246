// ============================================================================
//  PageAdjust.cpp —— 功能页：图片调整（模糊 / 明度 / 饱和度，实时预览）
// ============================================================================
#include "App.h"

using namespace Gdiplus;

static HWND s_hPage = nullptr;
static HWND s_hPreview = nullptr;
static HWND s_hGrp = nullptr;
static HWND s_hTrkBlur = nullptr;
static HWND s_hTrkBright = nullptr;
static HWND s_hTrkSat = nullptr;
static HWND s_hLblBlur = nullptr;
static HWND s_hLblBright = nullptr;
static HWND s_hLblSat = nullptr;
static HWND s_hValBlur = nullptr;
static HWND s_hValBright = nullptr;
static HWND s_hValSat = nullptr;
static HWND s_hBtnReset = nullptr;
static HWND s_hBtnProcess = nullptr;

static Bitmap* s_pPrevSrc = nullptr;   // 预览用原图（缩小）
static Bitmap* s_pPrevDst = nullptr;   // 预览结果

// ---------------------------------------------------------------------------
static AdjustParams ReadTrackbars()
{
    AdjustParams p;
    p.blur = (int)SendMessageW(s_hTrkBlur, TBM_GETPOS, 0, 0);
    p.brightness = (int)SendMessageW(s_hTrkBright, TBM_GETPOS, 0, 0);
    p.saturation = (int)SendMessageW(s_hTrkSat, TBM_GETPOS, 0, 0);
    return p;
}

// 预览图比原图小，模糊半径要按比例换算，这样预览和最终输出效果一致
static AdjustParams PreviewParams()
{
    AdjustParams p = ReadTrackbars();
    if (g_pSrcImage && s_pPrevSrc && g_srcW > 0)
    {
        float scale = (float)s_pPrevSrc->GetWidth() / (float)g_srcW;
        int b = (int)((float)p.blur * scale + 0.5f);
        if (p.blur > 0 && b < 1) b = 1;
        p.blur = b;
    }
    return p;
}

static void UpdateValueLabels()
{
    wchar_t buf[64];
    swprintf_s(buf, L"%d", (int)SendMessageW(s_hTrkBlur, TBM_GETPOS, 0, 0));
    SetWindowTextW(s_hValBlur, buf);
    swprintf_s(buf, L"%d", (int)SendMessageW(s_hTrkBright, TBM_GETPOS, 0, 0));
    SetWindowTextW(s_hValBright, buf);
    swprintf_s(buf, L"%d", (int)SendMessageW(s_hTrkSat, TBM_GETPOS, 0, 0));
    SetWindowTextW(s_hValSat, buf);
}

static void RebuildPreview()
{
    if (s_pPrevSrc) { delete s_pPrevSrc; s_pPrevSrc = nullptr; }
    if (s_pPrevDst) { delete s_pPrevDst; s_pPrevDst = nullptr; }

    if (!g_pSrcImage) return;

    s_pPrevSrc = CreatePreviewSource(700);
    if (!s_pPrevSrc) return;

    s_pPrevDst = new Bitmap(s_pPrevSrc->GetWidth(), s_pPrevSrc->GetHeight(), PixelFormat32bppARGB);
    if (s_pPrevDst->GetLastStatus() != Ok)
    {
        delete s_pPrevDst;
        s_pPrevDst = nullptr;
    }
}

static void UpdatePreview()
{
    if (!s_pPrevSrc || !s_pPrevDst)
    {
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return;
    }

    {
        Graphics g(s_pPrevDst);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.DrawImage(s_pPrevSrc, 0, 0, s_pPrevSrc->GetWidth(), s_pPrevSrc->GetHeight());
    }

    ApplyAdjustments(s_pPrevDst, PreviewParams());

    if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);
    const int cx = rc.right;
    const int cy = rc.bottom;
    const int M = D(12);

    int panelW = D(330);
    if (panelW > cx - D(160)) panelW = cx - D(160);
    if (panelW < D(220)) panelW = D(220);

    int prevW = cx - panelW - M * 3;
    if (prevW < D(120)) prevW = D(120);
    MoveWindow(s_hPreview, M, M, prevW, cy - M * 2, TRUE);

    int px = M * 2 + prevW;
    int pw = cx - px - M;
    int py = M;

    int grpH = D(232);
    MoveWindow(s_hGrp, px, py, pw, grpH, TRUE);

    HWND tracks[3] = { s_hTrkBlur, s_hTrkBright, s_hTrkSat };
    HWND labels[3] = { s_hLblBlur, s_hLblBright, s_hLblSat };
    HWND values[3] = { s_hValBlur, s_hValBright, s_hValSat };

    for (int i = 0; i < 3; ++i)
    {
        int y = py + D(32) + i * D(60);
        MoveWindow(labels[i], px + D(16), y + D(4), D(58), D(22), TRUE);
        MoveWindow(tracks[i], px + D(78), y, pw - D(150), D(30), TRUE);
        MoveWindow(values[i], px + pw - D(66), y + D(4), D(50), D(22), TRUE);
    }

    py += grpH + D(10);
    MoveWindow(s_hBtnReset, px, py, pw, D(34), TRUE);

    int btnH = D(42);
    MoveWindow(s_hBtnProcess, px, cy - M - btnH, pw, btnH, TRUE);
}

static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const AdjustParams p = ReadTrackbars();
    if (p.blur == 0 && p.brightness == 0 && p.saturation == 0)
    {
        if (MessageBoxW(hPage, L"当前没有任何调整（模糊/明度/饱和度都是 0）。\r\n\r\n仍要输出一张副本吗？",
                        L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    SetAppStatus(L"正在处理，请稍候...");
    UpdateWindow(g_hMain);

    Bitmap* work = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
    if (work->GetLastStatus() != Ok)
    {
        delete work;
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    {
        Graphics g(work);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);
    }

    ApplyAdjustments(work, p);

    SaveBitmapWithSuffix(work, L"_adjust", hPage, nullptr);
}

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;

            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_AD_PREVIEW);
            s_hGrp = CreateCtl(L"Button", L"调整参数", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_AD_GRP);

            s_hLblBlur = CreateCtl(L"Static", L"模糊", WS_VISIBLE, hWnd, IDC_AD_LBL_BLUR);
            s_hLblBright = CreateCtl(L"Static", L"明度", WS_VISIBLE, hWnd, IDC_AD_LBL_BRIGHT);
            s_hLblSat = CreateCtl(L"Static", L"饱和度", WS_VISIBLE, hWnd, IDC_AD_LBL_SAT);

            s_hTrkBlur = CreateCtl(L"msctls_trackbar32", L"",
                                   WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_AD_TRK_BLUR);
            s_hTrkBright = CreateCtl(L"msctls_trackbar32", L"",
                                     WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_AD_TRK_BRIGHT);
            s_hTrkSat = CreateCtl(L"msctls_trackbar32", L"",
                                  WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_AD_TRK_SAT);

            SendMessageW(s_hTrkBlur, TBM_SETRANGE, TRUE, MAKELPARAM(0, 40));
            SendMessageW(s_hTrkBright, TBM_SETRANGE, TRUE, MAKELPARAM(-100, 100));
            SendMessageW(s_hTrkSat, TBM_SETRANGE, TRUE, MAKELPARAM(-100, 100));
            SendMessageW(s_hTrkBlur, TBM_SETTICFREQ, 10, 0);
            SendMessageW(s_hTrkBright, TBM_SETTICFREQ, 20, 0);
            SendMessageW(s_hTrkSat, TBM_SETTICFREQ, 20, 0);

            s_hValBlur = CreateCtl(L"Static", L"0", WS_VISIBLE, hWnd, -1);
            s_hValBright = CreateCtl(L"Static", L"0", WS_VISIBLE, hWnd, -1);
            s_hValSat = CreateCtl(L"Static", L"0", WS_VISIBLE, hWnd, -1);

            s_hBtnReset = CreateCtl(L"Button", L"重置参数", WS_VISIBLE | WS_TABSTOP, hWnd, IDC_AD_BTN_RESET);
            s_hBtnProcess = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      hWnd, IDC_AD_BTN_PROCESS);

            UpdateValueLabels();
            Layout();
        }
        return 0;

    case WM_SIZE:
        Layout();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    case WM_APP_IMAGE_CHANGED:
        RebuildPreview();
        UpdatePreview();
        return 0;

    case WM_HSCROLL:
        UpdateValueLabels();
        UpdatePreview();
        return 0;

    case WM_DRAWITEM:
        if (wParam == IDC_AD_PREVIEW && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            PaintImagePreview(dis->hDC, dis->rcItem, s_pPrevDst,
                              L"点击上方「选择图片...」载入图片\r\n然后拖动右侧滑块即可实时预览");
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_AD_BTN_RESET:
            SendMessageW(s_hTrkBlur, TBM_SETPOS, TRUE, 0);
            SendMessageW(s_hTrkBright, TBM_SETPOS, TRUE, 0);
            SendMessageW(s_hTrkSat, TBM_SETPOS, TRUE, 0);
            UpdateValueLabels();
            UpdatePreview();
            return 0;

        case IDC_AD_BTN_PROCESS:
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

HWND PageAdjust_Create(HWND parent)
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
        wc.lpszClassName = L"Pic246PageAdjust";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageAdjust", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
