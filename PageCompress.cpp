// ============================================================================
//  PageCompress.cpp —— 功能页：图片大小压缩
//    · 按画质压缩（滑块 10~100），实时预估输出体积
//    · 按目标大小压缩（填 KB），自动搜索最合适的画质
//    · 尺寸不变，只压体积；输出到原目录，后缀 _min
// ============================================================================
#include "App.h"

using namespace Gdiplus;

static HWND s_hPage = nullptr;
static HWND s_hPreview = nullptr;
static HWND s_hGrpMode = nullptr;
static HWND s_hRadioQuality = nullptr;
static HWND s_hRadioTarget = nullptr;
static HWND s_hGrpParam = nullptr;
static HWND s_hTrkQuality = nullptr;
static HWND s_hLblQuality = nullptr;
static HWND s_hEditTarget = nullptr;
static HWND s_hLblUnit = nullptr;
static HWND s_hGrpFormat = nullptr;
static HWND s_hRadioKeep = nullptr;
static HWND s_hRadioJpeg = nullptr;
static HWND s_hRadioPng = nullptr;
static HWND s_hLblEst = nullptr;
static HWND s_hLblOrig = nullptr;
static HWND s_hBtn = nullptr;

static Bitmap* s_pPreview = nullptr;
static bool    s_estimating = false;

#define TIMER_ESTIMATE 1

// ---------------------------------------------------------------------------
static int  Quality() { return (int)SendMessageW(s_hTrkQuality, TBM_GETPOS, 0, 0); }
static bool TargetMode() { return Button_GetCheck(s_hRadioTarget) == BST_CHECKED; }

static size_t TargetBytes()
{
    wchar_t buf[64] = {};
    GetWindowTextW(s_hEditTarget, buf, 64);
    long long kb = _wtoi64(buf);
    if (kb < 1) kb = 1;
    return (size_t)kb * 1024;
}

// 输出 MIME：保持原格式 / 强制 JPEG / 强制 PNG
static std::wstring OutputMime(std::wstring* extOut)
{
    int sel = 2;   // 0=保持 1=jpeg 2=png
    if (Button_GetCheck(s_hRadioKeep) == BST_CHECKED) sel = 0;
    else if (Button_GetCheck(s_hRadioJpeg) == BST_CHECKED) sel = 1;
    else if (Button_GetCheck(s_hRadioPng) == BST_CHECKED) sel = 2;

    if (sel == 0)
    {
        const OutFmtGuess f = GuessSourceMime();
        if (extOut) *extOut = f.ext;
        return f.mime;
    }
    if (sel == 1) { if (extOut) *extOut = L".jpg"; return L"image/jpeg"; }
    if (extOut) *extOut = L".png";
    return L"image/png";
}

static void UpdateQualityLabel()
{
    wchar_t buf[128];
    if (TargetMode())
        swprintf_s(buf, L"由目标大小自动决定");
    else
        swprintf_s(buf, L"画质 %d%%", Quality());
    SetWindowTextW(s_hLblQuality, buf);
}

static void UpdateModeEnable()
{
    const bool t = TargetMode();
    EnableWindow(s_hTrkQuality, !t);
    EnableWindow(s_hEditTarget, t);
    UpdateQualityLabel();
}

static void ScheduleEstimate()
{
    if (!s_hPage) return;
    KillTimer(s_hPage, TIMER_ESTIMATE);
    SetTimer(s_hPage, TIMER_ESTIMATE, 320, nullptr);
}

static void UpdateOriginInfo()
{
    if (!s_hLblOrig) return;
    wchar_t buf[256];
    if (!g_pSrcImage)
    {
        wcscpy_s(buf, L"尚未载入图片。");
    }
    else
    {
        std::wstring ext;
        const std::wstring mime = OutputMime(&ext);
        swprintf_s(buf, L"原文件大小：%s\r\n原图尺寸：%u × %u        输出格式：%s",
                   FormatBytes(SourceFileSize()).c_str(), g_srcW, g_srcH,
                   (mime == L"image/png") ? L"PNG" : (mime == L"image/jpeg" ? L"JPEG" : L"其它"));
    }
    SetWindowTextW(s_hLblOrig, buf);
}

// 预估与实际保存必须基于同一份位图数据：源图若是 24bpp（JPEG 常见），
// 直接对源图编码得到的 BMP/TIFF/PNG 尺寸会明显偏小。
static Bitmap* BuildOutputBitmap(const std::wstring& mime)
{
    if (!g_pSrcImage || g_srcW <= 0 || g_srcH <= 0) return nullptr;

    Bitmap* work = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
    if (work->GetLastStatus() != Ok) { delete work; return nullptr; }

    Graphics g(work);
    g.SetCompositingMode(CompositingModeSourceCopy);
    if (!(mime == L"image/png" || mime == L"image/gif" || mime == L"image/tiff"))
    {
        g.Clear(Color(255, 255, 255, 255));      // 不支持透明的格式铺白底
        g.SetCompositingMode(CompositingModeSourceOver);
    }
    g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);
    return work;
}

static void DoEstimate()
{
    if (!s_hLblEst) return;
    if (!g_pSrcImage)
    {
        SetWindowTextW(s_hLblEst, L"预估大小：—");
        return;
    }
    if (s_estimating) return;
    s_estimating = true;

    std::wstring ext;
    const std::wstring mime = OutputMime(&ext);
    const size_t srcSize = SourceFileSize();
    const bool lossy = (_wcsicmp(mime.c_str(), L"image/jpeg") == 0 ||
                        _wcsicmp(mime.c_str(), L"image/webp") == 0);

    wchar_t buf[512];
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    Bitmap* work = BuildOutputBitmap(mime);

    if (lossy && TargetMode())
    {
        const size_t target = TargetBytes();
        size_t actual = 0;
        const int q = work ? FindQualityForTargetSize(work, target, &actual) : 0;
        if (actual == 0)
        {
            swprintf_s(buf, L"预估大小：计算失败");
        }
        else if (actual <= target)
        {
            swprintf_s(buf, L"预估大小：%s（画质 %d）\r\n目标 %s：已达成%s",
                       FormatBytes(actual).c_str(), q, FormatBytes(target).c_str(),
                       (srcSize > 0) ? L"" : L"");
        }
        else
        {
            swprintf_s(buf, L"预估大小：%s（已是最低画质 %d）\r\n无法压到目标 %s，只能压到这个程度",
                       FormatBytes(actual).c_str(), q, FormatBytes(target).c_str());
        }
    }
    else
    {
        const ULONG q = (ULONG)(lossy ? Quality() : 100);
        const size_t est = work ? EstimateEncodedSize(work, mime, q) : 0;
        if (est == 0)
        {
            swprintf_s(buf, L"预估大小：计算失败");
        }
        else if (srcSize > 0)
        {
            swprintf_s(buf, L"预估大小：%s\r\n原文件 %s，约为原来的 %.0f%%",
                       FormatBytes(est).c_str(), FormatBytes(srcSize).c_str(),
                       100.0 * (double)est / (double)srcSize);
        }
        else
        {
            swprintf_s(buf, L"预估大小：%s", FormatBytes(est).c_str());
        }
    }

    SetWindowTextW(s_hLblEst, buf);
    delete work;
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
    const int cx = rc.right;
    const int cy = rc.bottom;
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

    int hMode = D(92);
    MoveWindow(s_hGrpMode, px, py, pw, hMode, TRUE);
    MoveWindow(s_hRadioQuality, px + D(18), py + D(32), pw - D(36), D(24), TRUE);
    MoveWindow(s_hRadioTarget, px + D(18), py + D(58), pw - D(36), D(24), TRUE);

    py += hMode + D(8);
    int hParam = D(112);
    MoveWindow(s_hGrpParam, px, py, pw, hParam, TRUE);
    MoveWindow(s_hTrkQuality, px + D(16), py + D(26), pw - D(32), D(30), TRUE);
    MoveWindow(s_hLblQuality, px + D(18), py + D(58), pw - D(36), D(20), TRUE);
    MoveWindow(s_hEditTarget, px + D(18), py + D(80), D(110), D(24), TRUE);
    MoveWindow(s_hLblUnit, px + D(136), py + D(84), pw - D(150), D(20), TRUE);

    py += hParam + D(8);
    int hFmt = D(104);
    MoveWindow(s_hGrpFormat, px, py, pw, hFmt, TRUE);
    MoveWindow(s_hRadioKeep, px + D(18), py + D(28), pw - D(36), D(24), TRUE);
    MoveWindow(s_hRadioJpeg, px + D(18), py + D(52), pw - D(36), D(24), TRUE);
    MoveWindow(s_hRadioPng, px + D(18), py + D(76), pw - D(36), D(24), TRUE);

    py += hFmt + D(8);
    MoveWindow(s_hLblOrig, px, py, pw, D(40), TRUE);
    py += D(42);
    MoveWindow(s_hLblEst, px, py, pw, D(52), TRUE);

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

    std::wstring ext;
    const std::wstring mime = OutputMime(&ext);
    const bool lossy = (_wcsicmp(mime.c_str(), L"image/jpeg") == 0 ||
                        _wcsicmp(mime.c_str(), L"image/webp") == 0);

    ULONG quality = 100;
    if (lossy)
    {
        if (TargetMode())
        {
            size_t actual = 0;
            const int q = FindQualityForTargetSize(g_pSrcImage, TargetBytes(), &actual);
            quality = (ULONG)q;
            if (actual > TargetBytes())
            {
                wchar_t q2[512];
                swprintf_s(q2, L"即使使用最低画质（%d），输出仍有 %s，无法压到目标 %s。\r\n\r\n是否按最低画质继续输出？",
                           q, FormatBytes(actual).c_str(), FormatBytes(TargetBytes()).c_str());
                if (MessageBoxW(hPage, q2, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return;
            }
        }
        else
        {
            quality = (ULONG)Quality();
        }
    }

    SetAppStatus(L"正在压缩，请稍候...");
    UpdateWindow(g_hMain);

    Bitmap* work = BuildOutputBitmap(mime);
    if (!work)
    {
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    SaveBitmapAsFormat(work, L"_min", mime, ext, quality, hPage);
}

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;

            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CM_PREVIEW);

            s_hGrpMode = CreateCtl(L"Button", L"压缩方式", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CM_GRP_MODE);
            s_hRadioQuality = CreateCtl(L"Button", L"按画质（拖动滑块，实时预估体积）",
                                        WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                        hWnd, IDC_CM_RADIO_QUALITY);
            s_hRadioTarget = CreateCtl(L"Button", L"按目标大小（自动算最合适的画质）",
                                       WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                       hWnd, IDC_CM_RADIO_TARGET);
            Button_SetCheck(s_hRadioQuality, BST_CHECKED);

            s_hGrpParam = CreateCtl(L"Button", L"压缩参数", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CM_GRP_PARAM);
            s_hTrkQuality = CreateCtl(L"msctls_trackbar32", L"",
                                      WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_CM_TRK_QUALITY);
            SendMessageW(s_hTrkQuality, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
            SendMessageW(s_hTrkQuality, TBM_SETPOS, TRUE, 80);
            SendMessageW(s_hTrkQuality, TBM_SETTICFREQ, 10, 0);

            s_hLblQuality = CreateCtl(L"Static", L"", WS_VISIBLE, hWnd, IDC_CM_LBL_QUALITY);
            s_hEditTarget = CreateCtl(L"Edit", L"",
                                      WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                                      hWnd, IDC_CM_EDIT_TARGET);
            SetWindowTextW(s_hEditTarget, L"500");
            s_hLblUnit = CreateCtl(L"Static", L"KB（目标文件大小）", WS_VISIBLE, hWnd, IDC_CM_LBL_UNIT);

            s_hGrpFormat = CreateCtl(L"Button", L"输出格式", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CM_GRP_FORMAT);
            s_hRadioKeep = CreateCtl(L"Button", L"保持原格式（PNG 无损 / JPEG 有损）",
                                     WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                     hWnd, IDC_CM_RADIO_KEEP);
            s_hRadioJpeg = CreateCtl(L"Button", L"转成 JPEG（压缩率最高）",
                                     WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, hWnd, IDC_CM_RADIO_JPEG);
            s_hRadioPng = CreateCtl(L"Button", L"转成 PNG（无损，通常更大）",
                                    WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, hWnd, IDC_CM_RADIO_PNG);
            Button_SetCheck(s_hRadioKeep, BST_CHECKED);

            s_hLblOrig = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CM_LBL_ORIG);
            s_hLblEst = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CM_LBL_EST);
            s_hBtn = CreateCtl(L"Button", L"压缩并输出", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_CM_BTN_PROCESS);

            UpdateModeEnable();
            UpdateOriginInfo();
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
        {
            const size_t src = SourceFileSize();
            if (src > 0)
            {
                size_t kb = src / 1024 / 4;      // 默认目标：原体积的 1/4
                if (kb < 10) kb = 10;
                wchar_t buf[32];
                swprintf_s(buf, L"%llu", (unsigned long long)kb);
                SetWindowTextW(s_hEditTarget, buf);
            }
        }
        UpdateModeEnable();
        UpdateOriginInfo();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        DoEstimate();
        return 0;

    case WM_DRAWITEM:
        if (wParam == IDC_CM_PREVIEW && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            PaintImagePreview(dis->hDC, dis->rcItem, s_pPreview,
                              L"点击上方「选择图片...」载入图片\r\n载入后即可实时预估压缩后的体积");
            return TRUE;
        }
        break;

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);

            if (id == IDC_CM_BTN_PROCESS)
            {
                OnProcess(hWnd);
                return 0;
            }
            if (id == IDC_CM_RADIO_QUALITY || id == IDC_CM_RADIO_TARGET)
            {
                UpdateModeEnable();
                ScheduleEstimate();
                return 0;
            }
            if (id == IDC_CM_RADIO_KEEP || id == IDC_CM_RADIO_JPEG || id == IDC_CM_RADIO_PNG)
            {
                UpdateOriginInfo();
                ScheduleEstimate();
                return 0;
            }
            if (id == IDC_CM_EDIT_TARGET && code == EN_CHANGE)
            {
                ScheduleEstimate();
                return 0;
            }
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

HWND PageCompress_Create(HWND parent)
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
        wc.lpszClassName = L"Pic246PageCompress";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageCompress", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
