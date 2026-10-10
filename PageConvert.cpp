// ============================================================================
//  PageConvert.cpp —— 功能页：格式转换
//    · 列出系统可用的图片编码器（PNG / JPEG / BMP / GIF / TIFF / WEBP ...）
//    · 可选有损格式的编码质量，实时预估输出体积
//    · 目标格式不支持透明时，可用填充色把透明区域填掉
//    · 输出到原目录，后缀为 _<扩展名>
// ============================================================================
#include "App.h"

using namespace Gdiplus;

static HWND s_hPage = nullptr;
static HWND s_hPreview = nullptr;
static HWND s_hGrpFormat = nullptr;
static HWND s_hCombo = nullptr;
static HWND s_hLblFmtInfo = nullptr;
static HWND s_hGrpOpt = nullptr;
static HWND s_hTrkQuality = nullptr;
static HWND s_hLblQuality = nullptr;
static HWND s_hChkTrans = nullptr;
static HWND s_hSwatch = nullptr;
static HWND s_hBtnColor = nullptr;
static HWND s_hLblEst = nullptr;
static HWND s_hBtn = nullptr;

static Bitmap* s_pPreview = nullptr;
static std::vector<EncoderChoice> s_encoders;
static bool s_estimating = false;

#define TIMER_ESTIMATE 1

// ---------------------------------------------------------------------------
static const EncoderChoice* CurEncoder()
{
    const int sel = (int)SendMessageW(s_hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s_encoders.size()) return nullptr;
    return &s_encoders[sel];
}

static bool IsLossy(const std::wstring& mime)
{
    return _wcsicmp(mime.c_str(), L"image/jpeg") == 0 ||
           _wcsicmp(mime.c_str(), L"image/webp") == 0;
}

static bool EncoderKeepsAlpha(const std::wstring& mime)
{
    return _wcsicmp(mime.c_str(), L"image/png") == 0 ||
           _wcsicmp(mime.c_str(), L"image/gif") == 0 ||
           _wcsicmp(mime.c_str(), L"image/tiff") == 0 ||
           _wcsicmp(mime.c_str(), L"image/webp") == 0;
}

static int Quality() { return (int)SendMessageW(s_hTrkQuality, TBM_GETPOS, 0, 0); }
static bool KeepTransparent() { return Button_GetCheck(s_hChkTrans) == BST_CHECKED; }

static void UpdateQualityLabel()
{
    const EncoderChoice* e = CurEncoder();
    wchar_t buf[128];
    if (!e)
        wcscpy_s(buf, L"—");
    else if (!IsLossy(e->mime))
        wcscpy_s(buf, L"该格式为无损编码，与画质无关");
    else
        swprintf_s(buf, L"编码质量 %d%%", Quality());
    SetWindowTextW(s_hLblQuality, buf);
}

static void UpdateEnable()
{
    const EncoderChoice* e = CurEncoder();
    if (!e) return;

    const bool lossy = IsLossy(e->mime);
    EnableWindow(s_hTrkQuality, lossy);

    const bool canAlpha = EncoderKeepsAlpha(e->mime) && g_srcHasAlpha;
    EnableWindow(s_hChkTrans, canAlpha);
    if (!canAlpha) Button_SetCheck(s_hChkTrans, BST_UNCHECKED);
    const bool useFill = !canAlpha || !KeepTransparent();
    EnableWindow(s_hSwatch, useFill);
    EnableWindow(s_hBtnColor, useFill);

    UpdateQualityLabel();

    wchar_t info[256];
    swprintf_s(info, L"%s\r\n%s",
               canAlpha ? L"目标格式支持透明，可保留透明背景。"
                        : L"目标格式不支持透明，透明区域将用下面的底色填充。",
               L"输出尺寸与原图一致，不做缩放。");
    SetWindowTextW(s_hLblFmtInfo, info);
    InvalidateRect(s_hSwatch, nullptr, TRUE);
}

// 构造真正会被写出的位图（32bppARGB，必要时先铺底色）。
// 预估与实际保存必须用同一份数据，否则 BMP/TIFF 这类按位深计大小的格式会差很多
// （源图是 24bpp 的 JPEG 时，直接对源图编码得到的是 24bpp BMP，只有实际输出的 3/4）。
static Bitmap* BuildOutputBitmap(bool keepAlpha)
{
    if (!g_pSrcImage || g_srcW <= 0 || g_srcH <= 0) return nullptr;

    Bitmap* work = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
    if (work->GetLastStatus() != Ok) { delete work; return nullptr; }

    Graphics g(work);
    g.SetCompositingMode(CompositingModeSourceCopy);
    if (!keepAlpha)
    {
        g.Clear(Color(255, GetRValue(g_fillColor), GetGValue(g_fillColor), GetBValue(g_fillColor)));
        g.SetCompositingMode(CompositingModeSourceOver);
    }
    g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);
    return work;
}

static void ScheduleEstimate()
{
    if (!s_hPage) return;
    KillTimer(s_hPage, TIMER_ESTIMATE);
    SetTimer(s_hPage, TIMER_ESTIMATE, 320, nullptr);
}

static void DoEstimate()
{
    if (!s_hLblEst) return;
    if (!g_pSrcImage) { SetWindowTextW(s_hLblEst, L"预估大小：—"); return; }
    const EncoderChoice* e = CurEncoder();
    if (!e) { SetWindowTextW(s_hLblEst, L"预估大小：—"); return; }

    if (s_estimating) return;
    s_estimating = true;
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    const ULONG q = (ULONG)(IsLossy(e->mime) ? Quality() : 100);
    const bool keepAlphaEst = EncoderKeepsAlpha(e->mime) && g_srcHasAlpha && KeepTransparent();
    Bitmap* work = BuildOutputBitmap(keepAlphaEst);
    const size_t est = work ? EstimateEncodedSize(work, e->mime, q) : 0;
    delete work;
    const size_t src = SourceFileSize();

    wchar_t buf[512];
    if (est == 0)
    {
        swprintf_s(buf, L"预估大小：计算失败");
    }
    else if (src > 0)
    {
        swprintf_s(buf, L"预估大小：%s（原文件 %s，约为原来的 %.0f%%）",
                   FormatBytes(est).c_str(), FormatBytes(src).c_str(),
                   100.0 * (double)est / (double)src);
    }
    else
    {
        swprintf_s(buf, L"预估大小：%s", FormatBytes(est).c_str());
    }
    SetWindowTextW(s_hLblEst, buf);

    SetCursor(hOld);
    s_estimating = false;
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
    const int cx = rc.right, cy = rc.bottom;
    const int M = D(12);

    int panelW = D(340);
    if (panelW > cx - D(160)) panelW = cx - D(160);
    if (panelW < D(240)) panelW = D(240);

    int prevW = cx - panelW - M * 3;
    if (prevW < D(120)) prevW = D(120);
    MoveWindow(s_hPreview, M, M, prevW, cy - M * 2, TRUE);

    const int px = M * 2 + prevW;
    const int pw = cx - px - M;
    int py = M;

    const int hFmt = D(118);
    MoveWindow(s_hGrpFormat, px, py, pw, hFmt, TRUE);
    MoveWindow(s_hCombo, px + D(16), py + D(26), pw - D(32), D(220), TRUE);
    MoveWindow(s_hLblFmtInfo, px + D(18), py + D(58), pw - D(36), D(52), TRUE);

    py += hFmt + D(8);
    const int hOpt = D(166);
    MoveWindow(s_hGrpOpt, px, py, pw, hOpt, TRUE);
    MoveWindow(s_hTrkQuality, px + D(16), py + D(26), pw - D(32), D(30), TRUE);
    MoveWindow(s_hLblQuality, px + D(18), py + D(58), pw - D(36), D(20), TRUE);
    MoveWindow(s_hChkTrans, px + D(18), py + D(84), pw - D(36), D(24), TRUE);
    MoveWindow(s_hSwatch, px + D(18), py + D(112), D(44), D(26), TRUE);
    MoveWindow(s_hBtnColor, px + D(72), py + D(112), pw - D(90), D(26), TRUE);

    py += hOpt + D(8);
    MoveWindow(s_hLblEst, px, py, pw, D(40), TRUE);

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
    const EncoderChoice* e = CurEncoder();
    if (!e)
    {
        MessageBoxW(hPage, L"请先选择目标格式。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (_wcsicmp(e->ext.c_str(), ExtOf(g_srcPath).c_str()) == 0)
    {
        wchar_t q[256];
        swprintf_s(q, L"目标格式与源文件相同（%s），仍要继续吗？", e->name.c_str());
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    const bool keepAlpha = EncoderKeepsAlpha(e->mime) && g_srcHasAlpha && KeepTransparent();
    const ULONG quality = (ULONG)(IsLossy(e->mime) ? Quality() : 100);

    SetAppStatus(L"正在转换，请稍候...");
    UpdateWindow(g_hMain);

    Bitmap* work = BuildOutputBitmap(keepAlpha);
    if (!work)
    {
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    std::wstring extNoDot = e->ext;
    if (!extNoDot.empty() && extNoDot[0] == L'.') extNoDot.erase(0, 1);

    SaveBitmapAsFormat(work, L"_" + extNoDot, e->mime, e->ext, quality, hPage);
}

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;

            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CV_PREVIEW);

            s_hGrpFormat = CreateCtl(L"Button", L"目标格式", WS_VISIBLE | BS_GROUPBOX,
                                     hWnd, IDC_CV_GRP_FORMAT);
            s_hCombo = CreateCtl(L"ComboBox", L"",
                                 WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                 hWnd, IDC_CV_COMBO_FORMAT);
            s_hLblFmtInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                      hWnd, IDC_CV_LBL_FMTINFO);

            s_hGrpOpt = CreateCtl(L"Button", L"编码选项", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CV_GRP_OPT);
            s_hTrkQuality = CreateCtl(L"msctls_trackbar32", L"",
                                      WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_CV_TRK_QUALITY);
            SendMessageW(s_hTrkQuality, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
            SendMessageW(s_hTrkQuality, TBM_SETPOS, TRUE, 90);
            SendMessageW(s_hTrkQuality, TBM_SETTICFREQ, 10, 0);
            s_hLblQuality = CreateCtl(L"Static", L"", WS_VISIBLE, hWnd, IDC_CV_LBL_QUALITY);
            s_hChkTrans = CreateCtl(L"Button", L"保留透明背景",
                                    WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, hWnd, IDC_CV_CHK_TRANS);
            Button_SetCheck(s_hChkTrans, BST_CHECKED);
            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CV_SWATCH);
            s_hBtnColor = CreateCtl(L"Button", L"选择填充颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_CV_BTN_COLOR);

            s_hLblEst = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CV_LBL_EST);
            s_hBtn = CreateCtl(L"Button", L"转换并输出", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_CV_BTN_PROCESS);

            // 填编码器列表
            s_encoders = ListImageEncoders();
            for (size_t i = 0; i < s_encoders.size(); ++i)
            {
                std::wstring label = s_encoders[i].name + L"  (*" + s_encoders[i].ext + L")";
                SendMessageW(s_hCombo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
            }
            if (!s_encoders.empty()) SendMessageW(s_hCombo, CB_SETCURSEL, 0, 0);

            UpdateEnable();
            Layout();
        }
        return 0;

    case WM_SIZE:
        Layout();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ESTIMATE)
        {
            KillTimer(hWnd, TIMER_ESTIMATE);
            DoEstimate();
        }
        return 0;

    case WM_HSCROLL:
        UpdateQualityLabel();
        ScheduleEstimate();
        return 0;

    case WM_APP_IMAGE_CHANGED:
        RebuildPreview();
        // 默认选中与源文件相同的格式
        {
            const std::wstring srcExt = ExtOf(g_srcPath);
            for (size_t i = 0; i < s_encoders.size(); ++i)
            {
                if (_wcsicmp(s_encoders[i].ext.c_str(), srcExt.c_str()) == 0)
                {
                    SendMessageW(s_hCombo, CB_SETCURSEL, (WPARAM)i, 0);
                    break;
                }
            }
        }
        UpdateEnable();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        DoEstimate();
        return 0;

    case WM_DRAWITEM:
        if (lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (wParam == IDC_CV_PREVIEW)
            {
                PaintImagePreview(dis->hDC, dis->rcItem, s_pPreview,
                                  L"点击上方「选择图片...」载入图片\r\n载入后即可转换格式");
                return TRUE;
            }
            if (wParam == IDC_CV_SWATCH)
            {
                const EncoderChoice* e = CurEncoder();
                const bool canAlpha = e && EncoderKeepsAlpha(e->mime) && g_srcHasAlpha;
                PaintColorSwatch(dis->hDC, dis->rcItem, g_fillColor, canAlpha && KeepTransparent());
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);

            if (id == IDC_CV_BTN_PROCESS) { OnProcess(hWnd); return 0; }
            if (id == IDC_CV_COMBO_FORMAT && code == CBN_SELCHANGE)
            {
                UpdateEnable();
                ScheduleEstimate();
                return 0;
            }
            if (id == IDC_CV_CHK_TRANS)
            {
                UpdateEnable();
                return 0;
            }
            if (id == IDC_CV_BTN_COLOR)
            {
                const COLORREF c = ChooseFillColor(hWnd, g_fillColor);
                if (c != CLR_INVALID && c != g_fillColor)
                {
                    g_fillColor = c;
                    InvalidateRect(s_hSwatch, nullptr, TRUE);
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

HWND PageConvert_Create(HWND parent)
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
        wc.lpszClassName = L"Pic246PageConvert";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageConvert", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
