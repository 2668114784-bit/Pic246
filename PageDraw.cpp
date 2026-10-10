// ============================================================================
//  PageDraw.cpp —— 功能页：涂鸦
//    · 画笔绘制（可调笔粗细 / 颜色）
//    · 吸色（点击画面取色，取完自动回到画笔）
//    · 撤销 / 清空
//    · 输出到原目录，后缀 _draw
// ============================================================================
#include "App.h"

using namespace Gdiplus;

static HWND s_hPage = nullptr;
static HWND s_hCanvas = nullptr;
static HWND s_hGrpBrush = nullptr;
static HWND s_hTrkSize = nullptr;
static HWND s_hLblSize = nullptr;
static HWND s_hSwatch = nullptr;
static HWND s_hBtnColor = nullptr;
static HWND s_hBtnPick = nullptr;
static HWND s_hBtnUndo = nullptr;
static HWND s_hBtnClear = nullptr;
static HWND s_hLblHint = nullptr;
static HWND s_hBtn = nullptr;

static COLORREF s_brushColor = RGB(235, 40, 45);

// ---------------------------------------------------------------------------
static int BrushSize() { return (int)SendMessageW(s_hTrkSize, TBM_GETPOS, 0, 0); }

static void UpdateSizeLabel()
{
    wchar_t buf[64];
    swprintf_s(buf, L"画笔大小：%d 像素", BrushSize());
    SetWindowTextW(s_hLblSize, buf);
}

static void UpdateUndoButton()
{
    EnableWindow(s_hBtnUndo, DrawCanvas_CanUndo(s_hCanvas) ? TRUE : FALSE);
    EnableWindow(s_hBtnClear, DrawCanvas_CanUndo(s_hCanvas) ? TRUE : FALSE);
}

static void ApplyBrush()
{
    DrawCanvas_SetBrush(s_hCanvas, s_brushColor, BrushSize());
}

static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);
    const int cx = rc.right, cy = rc.bottom;
    const int M = D(12);

    int panelW = D(240);
    if (panelW > cx - D(160)) panelW = cx - D(160);
    if (panelW < D(180)) panelW = D(180);

    int cvW = cx - panelW - M * 3;
    if (cvW < D(120)) cvW = D(120);
    MoveWindow(s_hCanvas, M, M, cvW, cy - M * 2, TRUE);

    const int px = M * 2 + cvW;
    const int pw = cx - px - M;
    int py = M;

    const int hBrush = D(206);
    MoveWindow(s_hGrpBrush, px, py, pw, hBrush, TRUE);
    MoveWindow(s_hTrkSize, px + D(14), py + D(26), pw - D(28), D(30), TRUE);
    MoveWindow(s_hLblSize, px + D(16), py + D(58), pw - D(32), D(20), TRUE);
    MoveWindow(s_hSwatch, px + D(16), py + D(84), D(40), D(26), TRUE);
    MoveWindow(s_hBtnColor, px + D(64), py + D(84), pw - D(80), D(26), TRUE);
    MoveWindow(s_hBtnPick, px + D(16), py + D(118), pw - D(32), D(30), TRUE);
    MoveWindow(s_hBtnUndo, px + D(16), py + D(154), (pw - D(40)) / 2, D(30), TRUE);
    MoveWindow(s_hBtnClear, px + D(24) + (pw - D(40)) / 2, py + D(154),
               (pw - D(40)) / 2, D(30), TRUE);

    py += hBrush + D(8);
    MoveWindow(s_hLblHint, px, py, pw, D(96), TRUE);

    const int btnH = D(42);
    MoveWindow(s_hBtn, px, cy - M - btnH, pw, btnH, TRUE);
}

static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!DrawCanvas_CanUndo(s_hCanvas))
    {
        if (MessageBoxW(hPage, L"还没有画任何笔迹，仍要输出原图副本吗？", L"提示",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    Bitmap* overlay = DrawCanvas_RenderFull(s_hCanvas);
    if (!overlay)
    {
        MessageBoxW(hPage, L"合成笔迹失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        return;
    }

    SetAppStatus(L"正在合成并输出，请稍候...");
    UpdateWindow(g_hMain);

    const OutFmtGuess f = GuessSourceMime();

    Bitmap* out = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
    if (out->GetLastStatus() != Ok)
    {
        delete out;
        delete overlay;
        MessageBoxW(hPage, L"创建输出位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    {
        Graphics g(out);
        g.SetCompositingMode(CompositingModeSourceCopy);
        if (!f.alpha)
        {
            g.Clear(Color(255, 255, 255, 255));
            g.SetCompositingMode(CompositingModeSourceOver);
        }
        g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);
        g.DrawImage(overlay, 0, 0, (INT)g_srcW, (INT)g_srcH);
    }
    delete overlay;

    const ULONG q = (f.mime == L"image/jpeg" || f.mime == L"image/webp") ? 95 : 100;
    SaveBitmapAsFormat(out, L"_draw", f.mime, f.ext, q, hPage);
}

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;

            s_hCanvas = DrawCanvas_Create(hWnd, IDC_DR_CANVAS);

            s_hGrpBrush = CreateCtl(L"Button", L"画笔设置", WS_VISIBLE | BS_GROUPBOX,
                                    hWnd, IDC_DR_GRP_BRUSH);
            s_hTrkSize = CreateCtl(L"msctls_trackbar32", L"",
                                   WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_DR_TRK_SIZE);
            SendMessageW(s_hTrkSize, TBM_SETRANGE, TRUE, MAKELPARAM(1, 60));
            SendMessageW(s_hTrkSize, TBM_SETPOS, TRUE, 5);
            SendMessageW(s_hTrkSize, TBM_SETTICFREQ, 10, 0);
            s_hLblSize = CreateCtl(L"Static", L"", WS_VISIBLE, hWnd, IDC_DR_LBL_SIZE);

            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_DR_SWATCH);
            s_hBtnColor = CreateCtl(L"Button", L"画笔颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_DR_BTN_COLOR);
            s_hBtnPick = CreateCtl(L"Button", L"吸色（点击画面取色）",
                                   WS_VISIBLE | WS_TABSTOP | BS_PUSHLIKE | BS_AUTOCHECKBOX,
                                   hWnd, IDC_DR_BTN_PICK);
            s_hBtnUndo = CreateCtl(L"Button", L"撤销", WS_VISIBLE | WS_TABSTOP, hWnd, IDC_DR_BTN_UNDO);
            s_hBtnClear = CreateCtl(L"Button", L"清空", WS_VISIBLE | WS_TABSTOP, hWnd, IDC_DR_BTN_CLEAR);

            s_hLblHint = CreateCtl(L"Static",
                                   L"用法：\r\n"
                                   L"· 在左侧图片上按住左键拖动即可涂鸦\r\n"
                                   L"· 点「吸色」后点击画面取色，取完自动恢复画笔\r\n"
                                   L"· 笔迹可逐笔撤销，输出时按原图分辨率合成",
                                   WS_VISIBLE | SS_LEFT, hWnd, IDC_DR_LBL_HINT);

            s_hBtn = CreateCtl(L"Button", L"保存涂鸦并输出", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_DR_BTN_PROCESS);

            UpdateSizeLabel();
            ApplyBrush();
            UpdateUndoButton();
            Layout();
        }
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_HSCROLL:
        if ((HWND)lParam == s_hTrkSize)
        {
            UpdateSizeLabel();
            ApplyBrush();
        }
        return 0;

    case WM_APP_IMAGE_CHANGED:
        DrawCanvas_RefreshImage(s_hCanvas);
        UpdateUndoButton();
        return 0;

    case WM_APP_RECT_CHANGED:
        UpdateUndoButton();
        return 0;

    case WM_APP_PICKED_COLOR:
        {
            s_brushColor = (COLORREF)wParam;
            Button_SetCheck(s_hBtnPick, BST_UNCHECKED);
            DrawCanvas_SetPickMode(s_hCanvas, false);
            ApplyBrush();
            InvalidateRect(s_hSwatch, nullptr, TRUE);
            wchar_t buf[96];
            swprintf_s(buf, L"已吸取颜色：R%d G%d B%d",
                       GetRValue(s_brushColor), GetGValue(s_brushColor), GetBValue(s_brushColor));
            SetAppStatus(buf);
        }
        return 0;

    case WM_DRAWITEM:
        if (wParam == IDC_DR_SWATCH && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            PaintColorSwatch(dis->hDC, dis->rcItem, s_brushColor, false);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            if (id == IDC_DR_BTN_PROCESS) { OnProcess(hWnd); return 0; }
            if (id == IDC_DR_BTN_COLOR)
            {
                const COLORREF c = ChooseFillColor(hWnd, s_brushColor);
                if (c != s_brushColor)
                {
                    s_brushColor = c;
                    ApplyBrush();
                    InvalidateRect(s_hSwatch, nullptr, TRUE);
                }
                return 0;
            }
            if (id == IDC_DR_BTN_PICK)
            {
                const bool on = (Button_GetCheck(s_hBtnPick) == BST_CHECKED);
                if (on) ApplyBrush();
                DrawCanvas_SetPickMode(s_hCanvas, on);
                SetAppStatus(on ? L"吸色模式：点击左侧图片取色。" : L"已回到画笔模式。");
                return 0;
            }
            if (id == IDC_DR_BTN_UNDO)
            {
                DrawCanvas_Undo(s_hCanvas);
                UpdateUndoButton();
                return 0;
            }
            if (id == IDC_DR_BTN_CLEAR)
            {
                if (DrawCanvas_CanUndo(s_hCanvas) &&
                    MessageBoxW(hWnd, L"确定要清空所有笔迹吗？", L"提示",
                                MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    DrawCanvas_Clear(s_hCanvas);
                    UpdateUndoButton();
                }
                return 0;
            }
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

HWND PageDraw_Create(HWND parent)
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
        wc.lpszClassName = L"Pic246PageDraw";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageDraw", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
