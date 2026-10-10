// ============================================================================
//  Project001.cpp —— Pic246 主窗口
//    · 左侧功能导航（侧边栏）
//    · 顶部：选择图片 / 路径 / 原图信息
//    · 底部：状态栏
//    · 中间：4 个功能页（图片尺寸缩放 / 图片裁切 / 图片调整 / ICO 转换）
// ============================================================================
#include "App.h"

using namespace Gdiplus;

#define MAX_LOADSTRING 100

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
HINSTANCE       g_hInst = nullptr;
HWND            g_hMain = nullptr;
int             g_dpi = 96;
HFONT           g_hUiFont = nullptr;
std::wstring    g_srcPath;
Gdiplus::Image* g_pSrcImage = nullptr;
IStream*        g_pSrcStream = nullptr;
UINT            g_srcW = 0;
UINT            g_srcH = 0;
bool            g_srcHasAlpha = false;
COLORREF        g_fillColor = RGB(255, 255, 255);

// ---------------------------------------------------------------------------
// 主窗口控件
// ---------------------------------------------------------------------------
static HWND  g_hSidebar = nullptr;
static HWND  g_hBtnOpen = nullptr;
static HWND  g_hBtnAuthor = nullptr;
static HWND  g_hPath = nullptr;
static HWND  g_hInfo = nullptr;
static HWND  g_hStatus = nullptr;

#define PAGE_COUNT 7
static HWND  g_pages[PAGE_COUNT] = {};
static int   g_curPage = 0;

static const wchar_t* kPageNames[PAGE_COUNT] =
{
    L"图片尺寸缩放", L"图片裁切", L"图片调整", L"图片大小压缩",
    L"格式转换", L"ICO 转换", L"涂鸦"
};

static const int kPageProcessId[PAGE_COUNT] =
{
    IDC_RS_BTN_PROCESS, IDC_CP_BTN_PROCESS, IDC_AD_BTN_PROCESS, IDC_CM_BTN_PROCESS,
    IDC_CV_BTN_PROCESS, IDC_IC_BTN_PROCESS, IDC_DR_BTN_PROCESS
};

static HACCEL g_hAccelExtra = nullptr;

// ---------------------------------------------------------------------------
// 通用工具
// ---------------------------------------------------------------------------
void ApplyUiFont(HWND h)
{
    if (h && g_hUiFont)
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hUiFont, MAKELPARAM(TRUE, 0));
}

HWND CreateCtl(const wchar_t* cls, const wchar_t* text, DWORD style, HWND parent, int id, DWORD exStyle)
{
    HWND h = CreateWindowExW(exStyle, cls, text, style | WS_CHILD,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    ApplyUiFont(h);
    return h;
}

void SetAppStatus(const std::wstring& text)
{
    if (g_hStatus)
    {
        SetWindowTextW(g_hStatus, text.c_str());
        UpdateWindow(g_hStatus);
    }
}

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

static HICON LoadAppIcon(HINSTANCE hInstance, int cx, int cy, const int* ids, int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (ids[i] == 0)
            continue;
        HICON h = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(ids[i]), IMAGE_ICON, cx, cy, LR_SHARED);
        if (h)
            return h;
    }
    return LoadIconW(nullptr, IDI_APPLICATION);
}

// ---------------------------------------------------------------------------
// 图片载入
// ---------------------------------------------------------------------------
static void UpdateTopInfo()
{
    wchar_t buf[512];
    if (!g_pSrcImage)
    {
        wcscpy_s(buf, L"尚未选择图片。点击「选择图片...」，或把图片文件拖进窗口。");
    }
    else if (g_srcW == g_srcH)
    {
        swprintf_s(buf, L"原图尺寸：%u × %u 像素（正方形）", g_srcW, g_srcH);
    }
    else
    {
        swprintf_s(buf, L"原图尺寸：%u × %u 像素", g_srcW, g_srcH);
    }
    SetWindowTextW(g_hInfo, buf);
}

static void OpenImagePath(HWND hWnd, const std::wstring& path)
{
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring err;
    bool ok = LoadImageFile(path, err);

    SetCursor(hOld);

    if (!ok)
    {
        MessageBoxW(hWnd, err.c_str(), L"打开失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"载入失败：" + FileNameOf(path));
        return;
    }

    SetWindowTextW(g_hPath, path.c_str());
    UpdateTopInfo();

    // 通知所有功能页刷新
    for (int i = 0; i < PAGE_COUNT; ++i)
        if (g_pages[i])
            SendMessageW(g_pages[i], WM_APP_IMAGE_CHANGED, 0, 0);

    wchar_t buf[512];
    swprintf_s(buf, L"已载入 %s（%u × %u）。", FileNameOf(path).c_str(), g_srcW, g_srcH);
    SetAppStatus(buf);
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
    ofn.lpstrTitle = L"选择一张图片";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn))
        return;

    OpenImagePath(hWnd, std::wstring(buf.data()));
}

// ---------------------------------------------------------------------------
// 侧边栏
// ---------------------------------------------------------------------------
#define SIDEBAR_CLASS L"Pic246Sidebar"

struct SidebarState
{
    int  hover;
    int  sel;
    bool tracking;
};

static SidebarState* SidebarOf(HWND h)
{
    return (SidebarState*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static int SidebarItemsTop() { return D(64); }
static int SidebarItemH() { return D(52); }

static int SidebarHit(int y)
{
    const int top = SidebarItemsTop();
    if (y < top) return -1;
    int idx = (y - top) / SidebarItemH();
    if (idx < 0 || idx >= PAGE_COUNT) return -1;
    return idx;
}

static void PaintSidebar(HWND h)
{
    SidebarState* st = SidebarOf(h);
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);

    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right;
    const int hgt = rc.bottom;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, hgt);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // 背景
    HBRUSH bg = CreateSolidBrush(RGB(247, 248, 250));
    RECT all = { 0, 0, w, hgt };
    FillRect(mem, &all, bg);

    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, g_hUiFont);

    // 标题
    RECT tr = { D(16), D(16), w, D(44) };
    SetTextColor(mem, RGB(28, 32, 38));
    DrawTextW(mem, APP_NAME, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 分隔线
    HPEN sep = CreatePen(PS_SOLID, 1, RGB(228, 231, 236));
    HGDIOBJ oldPen = SelectObject(mem, sep);
    MoveToEx(mem, 0, D(56), nullptr);
    LineTo(mem, w, D(56));
    SelectObject(mem, oldPen);
    DeleteObject(sep);

    // 菜单项
    const int top = SidebarItemsTop();
    const int itemH = SidebarItemH();

    for (int i = 0; i < PAGE_COUNT; ++i)
    {
        RECT ir = { 0, top + i * itemH, w, top + (i + 1) * itemH };
        const bool sel = (st && st->sel == i);
        const bool hov = (st && st->hover == i);

        if (sel || hov)
        {
            RECT hr = ir;
            hr.left += D(8);
            hr.right -= D(8);
            hr.top += D(4);
            hr.bottom -= D(4);
            HBRUSH hb = CreateSolidBrush(sel ? RGB(224, 237, 252) : RGB(238, 242, 247));
            HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
            HGDIOBJ ob = SelectObject(mem, hb);
            RoundRect(mem, hr.left, hr.top, hr.right, hr.bottom, D(10), D(10));
            SelectObject(mem, ob);
            SelectObject(mem, op);
            DeleteObject(hb);
        }

        if (sel)
        {
            RECT acc = { D(8), top + i * itemH + D(4), D(12), top + (i + 1) * itemH - D(4) };
            HBRUSH ab = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(mem, &acc, ab);
            DeleteObject(ab);
        }

        RECT lr = { D(24), ir.top, w - D(12), ir.bottom };
        SetTextColor(mem, sel ? RGB(0, 92, 180) : RGB(48, 52, 58));
        DrawTextW(mem, kPageNames[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 右边框
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(226, 229, 234));
    oldPen = SelectObject(mem, edge);
    MoveToEx(mem, w - 1, 0, nullptr);
    LineTo(mem, w - 1, hgt);
    SelectObject(mem, oldPen);
    DeleteObject(edge);

    SelectObject(mem, oldFont);
    BitBlt(hdc, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);

    DeleteObject(bg);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK SidebarProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    SidebarState* st = SidebarOf(hWnd);

    switch (message)
    {
    case WM_NCCREATE:
        {
            SidebarState* s = new SidebarState();
            s->hover = -1;
            s->sel = 0;
            s->tracking = false;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)s);
        }
        return TRUE;

    case WM_NCDESTROY:
        if (st) { delete st; SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0); }
        return 0;

    case WM_PAINT:
        PaintSidebar(hWnd);
        return 0;

    case WM_MOUSEMOVE:
        if (st)
        {
            if (!st->tracking)
            {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
                TrackMouseEvent(&tme);
                st->tracking = true;
            }
            int idx = SidebarHit(GET_Y_LPARAM(lParam));
            if (idx != st->hover)
            {
                st->hover = idx;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, idx >= 0 ? IDC_HAND : IDC_ARROW));
        }
        return 0;

    case WM_MOUSELEAVE:
        if (st)
        {
            st->tracking = false;
            if (st->hover != -1)
            {
                st->hover = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (st)
        {
            int idx = SidebarHit(GET_Y_LPARAM(lParam));
            if (idx >= 0)
            {
                st->sel = idx;
                InvalidateRect(hWnd, nullptr, FALSE);
                PostMessageW(GetParent(hWnd), WM_APP_PAGE_SELECT, (WPARAM)idx, 0);
            }
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
static void GetGeom(HWND hWnd, int* cx, int* cy, int* sw, int* topH, int* stH)
{
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (cx) *cx = rc.right;
    if (cy) *cy = rc.bottom;
    if (sw) *sw = D(SIDEBAR_W_DIP);
    if (topH) *topH = D(TOPBAR_H_DIP);
    if (stH) *stH = D(STATUSBAR_H_DIP);
}

static void LayoutMain(HWND hWnd)
{
    int cx = 0, cy = 0, sw = 0, topH = 0, stH = 0;
    GetGeom(hWnd, &cx, &cy, &sw, &topH, &stH);

    MoveWindow(g_hSidebar, 0, 0, sw, cy, TRUE);

    const int x = sw + D(14);
    const int y = D(14);
    MoveWindow(g_hBtnOpen, x, y, D(132), D(34), TRUE);

    const int auW = D(104);
    const int auX = cx - D(14) - auW;
    if (g_hBtnAuthor)
        MoveWindow(g_hBtnAuthor, auX, y, auW, D(34), TRUE);

    const int px = x + D(132) + D(10);
    int pathW = auX - D(10) - px;
    if (pathW < D(60)) pathW = D(60);
    MoveWindow(g_hPath, px, y, pathW, D(34), TRUE);
    MoveWindow(g_hInfo, x, y + D(42), cx - x - D(14), D(22), TRUE);

    int pageW = cx - sw;
    int pageH = cy - topH - stH;
    if (pageW < 10) pageW = 10;
    if (pageH < 10) pageH = 10;

    for (int i = 0; i < PAGE_COUNT; ++i)
    {
        if (g_pages[i] && IsWindowVisible(g_pages[i]))
            MoveWindow(g_pages[i], sw, topH, pageW, pageH, TRUE);
    }

    MoveWindow(g_hStatus, sw, cy - stH, cx - sw, stH, TRUE);
}

static void SwitchPage(int idx)
{
    if (idx < 0 || idx >= PAGE_COUNT)
        return;

    g_curPage = idx;

    int cx = 0, cy = 0, sw = 0, topH = 0, stH = 0;
    GetGeom(g_hMain, &cx, &cy, &sw, &topH, &stH);
    int pageW = cx - sw, pageH = cy - topH - stH;
    if (pageW < 10) pageW = 10;
    if (pageH < 10) pageH = 10;

    for (int i = 0; i < PAGE_COUNT; ++i)
    {
        if (!g_pages[i]) continue;
        if (i == idx)
        {
            MoveWindow(g_pages[i], sw, topH, pageW, pageH, TRUE);
            ShowWindow(g_pages[i], SW_SHOW);
        }
        else
        {
            ShowWindow(g_pages[i], SW_HIDE);
        }
    }

    InvalidateRect(g_hSidebar, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 菜单
// ---------------------------------------------------------------------------
static void BuildMenu(HWND hWnd)
{
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;

    HMENU hFile = GetSubMenu(hMenu, 0);
    if (hFile)
    {
        InsertMenuW(hFile, 0, MF_BYPOSITION | MF_STRING, IDM_OPEN_IMAGE, L"打开图片(&O)...\tCtrl+O");
        InsertMenuW(hFile, 1, MF_BYPOSITION | MF_STRING, IDM_PROCESS_SAVE, L"处理并输出(&S)\tCtrl+S");
        InsertMenuW(hFile, 2, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    HMENU hFunc = CreatePopupMenu();
    if (hFunc)
    {
        for (int i = 0; i < PAGE_COUNT; ++i)
        {
            UINT flags = MF_STRING | ((i == 0) ? MF_CHECKED : 0);
            AppendMenuW(hFunc, flags, IDM_PAGE_BASE + i, kPageNames[i]);
        }
        InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hFunc, L"功能(&P)");
    }

    HMENU hHelp = GetSubMenu(hMenu, 2);
    if (hHelp)
    {
        InsertMenuW(hHelp, 0, MF_BYPOSITION | MF_STRING, IDM_AUTHOR, L"关于作者(&A)...");
        InsertMenuW(hHelp, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    DrawMenuBar(hWnd);
}

static void SyncPageMenu(HWND hWnd, int idx)
{
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;
    HMENU hFunc = GetSubMenu(hMenu, 1);
    if (!hFunc) return;

    for (int i = 0; i < PAGE_COUNT; ++i)
        CheckMenuItem(hFunc, IDM_PAGE_BASE + i, MF_BYCOMMAND | ((i == idx) ? MF_CHECKED : MF_UNCHECKED));
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK AuthorBox(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            g_hMain = hWnd;

            // 侧边栏
            g_hSidebar = CreateWindowExW(0, SIDEBAR_CLASS, L"",
                                         WS_CHILD | WS_VISIBLE,
                                         0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)IDC_SIDEBAR,
                                         g_hInst, nullptr);

            // 顶部
            g_hBtnOpen = CreateCtl(L"Button", L"选择图片...", WS_VISIBLE | WS_TABSTOP,
                                   hWnd, IDC_BTN_OPEN);
            g_hBtnAuthor = CreateCtl(L"Button", L"关于作者", WS_VISIBLE | WS_TABSTOP,
                                     hWnd, IDC_BTN_AUTHOR);
            g_hPath = CreateCtl(L"Static", L"",
                                WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_CENTERIMAGE,
                                hWnd, IDC_STATIC_PATH);
            g_hInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                hWnd, IDC_STATIC_INFO);

            // 状态栏
            g_hStatus = CreateCtl(L"Static", L"就绪：请选择一张图片。",
                                  WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                  hWnd, IDC_STATIC_STATUS);

            // 功能页
            g_pages[0] = PageResize_Create(hWnd);
            g_pages[1] = PageCrop_Create(hWnd);
            g_pages[2] = PageAdjust_Create(hWnd);
            g_pages[3] = PageCompress_Create(hWnd);
            g_pages[4] = PageConvert_Create(hWnd);
            g_pages[5] = PageIco_Create(hWnd);
            g_pages[6] = PageDraw_Create(hWnd);

            BuildMenu(hWnd);

            UpdateTopInfo();
            LayoutMain(hWnd);
            SwitchPage(0);

            DragAcceptFiles(hWnd, TRUE);
        }
        return 0;

    case WM_SIZE:
        LayoutMain(hWnd);
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            RECT wr = {}, cr = {};
            GetWindowRect(hWnd, &wr);
            GetClientRect(hWnd, &cr);
            int ncx = (wr.right - wr.left) - (cr.right - cr.left);
            int ncy = (wr.bottom - wr.top) - (cr.bottom - cr.top);
            mmi->ptMinTrackSize.x = D(900) + ncx;
            mmi->ptMinTrackSize.y = D(640) + ncy;
        }
        return 0;

    case WM_APP_PAGE_SELECT:
        SwitchPage((int)wParam);
        SyncPageMenu(hWnd, (int)wParam);
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            int cx = 0, cy = 0, sw = 0, topH = 0, stH = 0;
            GetGeom(hWnd, &cx, &cy, &sw, &topH, &stH);

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(228, 231, 236));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            MoveToEx(hdc, sw, cy - stH, nullptr);
            LineTo(hdc, cx, cy - stH);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);

            EndPaint(hWnd, &ps);
        }
        return 0;

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
            const int id = LOWORD(wParam);

            switch (id)
            {
            case IDC_BTN_OPEN:
            case IDM_OPEN_IMAGE:
                OnOpenImage(hWnd);
                return 0;

            case IDM_PROCESS_SAVE:
                if (g_pages[g_curPage])
                    SendMessageW(g_pages[g_curPage], WM_COMMAND,
                                 MAKEWPARAM(kPageProcessId[g_curPage], BN_CLICKED), 0);
                return 0;

            case IDM_ABOUT:
                DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_ABOUTBOX), hWnd, About);
                return 0;

            case IDC_BTN_AUTHOR:
            case IDM_AUTHOR:
                DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_AUTHORBOX), hWnd, AuthorBox);
                return 0;

            case IDM_EXIT:
                DestroyWindow(hWnd);
                return 0;

            default:
                if (id >= IDM_PAGE_BASE && id < IDM_PAGE_BASE + PAGE_COUNT)
                {
                    SwitchPage(id - IDM_PAGE_BASE);
                    SyncPageMenu(hWnd, id - IDM_PAGE_BASE);
                    return 0;
                }
                break;
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
        if (g_hUiFont) { DeleteObject(g_hUiFont); g_hUiFont = nullptr; }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 注册窗口类 / 初始化
// ---------------------------------------------------------------------------
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    // 侧边栏
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SidebarProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = SIDEBAR_CLASS;
        RegisterClassExW(&wc);
    }

    const int bigIds[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };
    const int smlIds[2] = { APP_ICON_SMALL_ID, APP_ICON_MAIN_ID };

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), bigIds, 2);
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_PROJECT001);
    wcex.lpszClassName = APP_MAIN_CLASS;
    wcex.hIconSm = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), smlIds, 2);
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    g_hInst = hInstance;

    RECT rc = { 0, 0, D(1060), D(720) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0);

    HWND hWnd = CreateWindowExW(0, APP_MAIN_CLASS, APP_WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
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

    SetProcessDPIAware();
    {
        HDC hdc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        if (g_dpi < 96) g_dpi = 96;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gdiplusStartupInput, nullptr) != Ok)
    {
        MessageBoxW(nullptr, L"GDI+ 初始化失败，程序无法启动。", L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    g_hUiFont = CreateUiFont();

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow))
    {
        GdiplusShutdown(token);
        return FALSE;
    }

    HACCEL hAccelTable = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDC_PROJECT001));

    ACCEL extra[2] = {};
    extra[0].fVirt = FCONTROL | FVIRTKEY; extra[0].key = 'O'; extra[0].cmd = IDM_OPEN_IMAGE;
    extra[1].fVirt = FCONTROL | FVIRTKEY; extra[1].key = 'S'; extra[1].cmd = IDM_PROCESS_SAVE;
    g_hAccelExtra = CreateAcceleratorTableW(extra, 2);

    // 命令行传入图片时直接打开（也支持把图片拖到 exe 图标上）
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

    GdiplusShutdown(token);
    return (int)msg.wParam;
}

// ---------------------------------------------------------------------------
// “关于”对话框
// ---------------------------------------------------------------------------
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        {
            const int ids[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };
            HICON hBig = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), ids, 2);
            HICON hSm = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ids, 2);

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

// ---------------------------------------------------------------------------
// “关于作者”对话框
// ---------------------------------------------------------------------------
#define IDC_BTN_COPYQQ 1201

static void SetClipText(HWND owner, const wchar_t* text)
{
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem)
    {
        void* p = GlobalLock(hMem);
        if (p)
        {
            memcpy(p, text, bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        else
        {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

INT_PTR CALLBACK AuthorBox(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        {
            const int ids[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };
            HICON hBig = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), ids, 2);
            HICON hSm = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ids, 2);

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

            // 追加一个“复制 QQ 号”按钮
            HWND hCopy = CreateWindowExW(0, L"Button", L"复制 QQ 号",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         100, 102, 68, 16, hDlg, (HMENU)(INT_PTR)IDC_BTN_COPYQQ,
                                         g_hInst, nullptr);
            ApplyUiFont(hCopy);
        }
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_COPYQQ)
        {
            SetClipText(hDlg, L"2668114784");
            SetWindowTextW(GetDlgItem(hDlg, IDC_BTN_COPYQQ), L"已复制!");
            return (INT_PTR)TRUE;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
