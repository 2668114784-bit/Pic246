// ============================================================================
//  CropCanvas.cpp —— 可交互的裁切画布
//    · 拖动框内：移动裁切框
//    · 拖动 8 个控制点（正方形模式 4 个角）：缩放裁切框
//    · 裁切框允许超出图片范围，超出部分按填充色/透明显示
// ============================================================================
#include "App.h"

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
struct CanvasState
{
    bool     square;            // 只允许正方形
    COLORREF fill;              // 填充色
    bool     transparentFill;   // 超出图片部分是否透明
    float    rx, ry, rw, rh;    // 裁切框（图像坐标，可为负）
    bool     rectSet;           // 是否已初始化
    int      dragMode;          // 0 无 1 移动 2..9 控制点
    POINT    dragOrg;
    float    dragRect[4];
    int      hoverMode;
    bool     tracking;
    Bitmap*  disp;              // 显示缓存（已缩放到屏幕尺寸）
    int      dispW, dispH;
    float    scale;             // 图像 -> 窗口
    float    viewMinX, viewMaxX;   // 画布可视范围（图像坐标）
    float    viewMinY, viewMaxY;
    float    offX, offY;
};

static CanvasState* State(HWND h)
{
    return (CanvasState*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static void InvalidateCanvas(HWND h)
{
    InvalidateRect(h, nullptr, FALSE);
}

static void NotifyParent(HWND h)
{
    HWND parent = GetParent(h);
    if (parent)
        SendMessageW(parent, WM_APP_RECT_CHANGED, (WPARAM)h, 0);
}

// ---------------------------------------------------------------------------
// 坐标映射
// ---------------------------------------------------------------------------
static void RecalcMapping(HWND h, CanvasState* s)
{
    RECT rc;
    GetClientRect(h, &rc);
    const int cw = rc.right;
    const int ch = rc.bottom;
    if (!g_pSrcImage || cw < 20 || ch < 20)
    {
        s->scale = 1.0f;
        s->offX = 0.0f;
        s->offY = 0.0f;
        return;
    }

    const float pad = (float)D(16);
    float availW = ((float)cw - pad * 2.0f) * 0.78f;
    float availH = ((float)ch - pad * 2.0f) * 0.78f;
    if (availW < 8) availW = 8;
    if (availH < 8) availH = 8;

    float sx = availW / (float)g_srcW;
    float sy = availH / (float)g_srcH;
    s->scale = (sx < sy) ? sx : sy;
    if (s->scale <= 0.0f) s->scale = 1.0f;

    s->offX = ((float)cw - (float)g_srcW * s->scale) * 0.5f;
    s->offY = ((float)ch - (float)g_srcH * s->scale) * 0.5f;

    s->viewMinX = (0.0f - s->offX) / s->scale;
    s->viewMaxX = ((float)cw - s->offX) / s->scale;
    s->viewMinY = (0.0f - s->offY) / s->scale;
    s->viewMaxY = ((float)ch - s->offY) / s->scale;
}

static void RebuildDisplay(HWND h, CanvasState* s)
{
    if (s->disp) { delete s->disp; s->disp = nullptr; }
    s->dispW = s->dispH = 0;
    if (!g_pSrcImage) return;

    int dw = (int)((float)g_srcW * s->scale + 0.5f);
    int dh = (int)((float)g_srcH * s->scale + 0.5f);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    std::wstring err;
    s->disp = ResizeBitmap(g_pSrcImage, dw, dh, err);
    if (s->disp)
    {
        s->dispW = dw;
        s->dispH = dh;
    }
    (void)h;
}

// ---------------------------------------------------------------------------
// 裁切框约束
// ---------------------------------------------------------------------------
static void EnsureRect(HWND h, CanvasState* s)
{
    if (!g_pSrcImage) { s->rectSet = false; return; }
    if (!s->rectSet)
    {
        s->rx = 0.0f;
        s->ry = 0.0f;
        s->rw = (float)g_srcW;
        s->rh = (float)g_srcH;
        s->rectSet = true;
    }
    (void)h;
}

static void ClampSize(CanvasState* s)
{
    const float W = (float)g_srcW;
    const float H = (float)g_srcH;
    const float minS = 4.0f;
    const float maxW = W * 3.0f;   // 允许最大到图片的 3 倍
    const float maxH = H * 3.0f;

    if (s->rw < minS) s->rw = minS;
    if (s->rh < minS) s->rh = minS;
    if (s->rw > maxW) s->rw = maxW;
    if (s->rh > maxH) s->rh = maxH;
    if (s->square)
    {
        float side = (s->rw > s->rh) ? s->rw : s->rh;
        if (side > maxW) side = maxW;
        if (side > maxH) side = maxH;
        if (side < minS) side = minS;
        s->rw = s->rh = side;
    }
}

// 把裁切框平移回合理范围（保持尺寸，正方形模式不会被破坏）
static void ClampPosition(CanvasState* s)
{
    const float minX = s->viewMinX;
    const float maxX = s->viewMaxX;
    const float minY = s->viewMinY;
    const float maxY = s->viewMaxY;

    if (s->rx < minX) s->rx = minX;
    if (s->ry < minY) s->ry = minY;
    if (s->rx + s->rw > maxX) s->rx = maxX - s->rw;
    if (s->ry + s->rh > maxY) s->ry = maxY - s->rh;
    if (s->rx < minX) s->rx = minX;
    if (s->ry < minY) s->ry = minY;
}

// ---------------------------------------------------------------------------
// 命中测试（窗口坐标）
// ---------------------------------------------------------------------------
static void HandleRects(CanvasState* s, RECT out[8])
{
    float l = s->offX + s->rx * s->scale;
    float t = s->offY + s->ry * s->scale;
    float r = l + s->rw * s->scale;
    float b = t + s->rh * s->scale;
    const int hs = D(5);   // 半边长

    float cx = (l + r) * 0.5f;
    float cy = (t + b) * 0.5f;

    const float xs[8] = { l, cx, r, r, r, cx, l, l };
    const float ys[8] = { t, t, t, cy, b, b, b, cy };

    for (int i = 0; i < 8; ++i)
    {
        out[i].left = (LONG)(xs[i] - hs);
        out[i].top = (LONG)(ys[i] - hs);
        out[i].right = (LONG)(xs[i] + hs);
        out[i].bottom = (LONG)(ys[i] + hs);
    }
}

static int HitTest(CanvasState* s, POINT pt)
{
    RECT hr[8];
    HandleRects(s, hr);

    for (int i = 0; i < 8; ++i)
    {
        if (s->square && (i % 2) == 1)     // 正方形模式只用 4 个角
            continue;
        if (PtInRect(&hr[i], pt))
            return i + 2;
    }

    float l = s->offX + s->rx * s->scale;
    float t = s->offY + s->ry * s->scale;
    float r = l + s->rw * s->scale;
    float b = t + s->rh * s->scale;
    if ((float)pt.x >= l && (float)pt.x <= r && (float)pt.y >= t && (float)pt.y <= b)
        return 1;

    return 0;
}

// ---------------------------------------------------------------------------
// 拖动
// ---------------------------------------------------------------------------
static void ApplyDrag(CanvasState* s, POINT pt)
{
    if (!g_pSrcImage || s->dragMode == 0) return;

    const float dx = (float)(pt.x - s->dragOrg.x) / s->scale;
    const float dy = (float)(pt.y - s->dragOrg.y) / s->scale;

    const float minX = s->viewMinX, maxX = s->viewMaxX;
    const float minY = s->viewMinY, maxY = s->viewMaxY;
    const float minS = 4.0f;

    float l0 = s->dragRect[0], t0 = s->dragRect[1], r0 = s->dragRect[2], b0 = s->dragRect[3];

    if (s->dragMode == 1)          // 移动
    {
        float l = l0 + dx;
        float t = t0 + dy;
        if (l < minX) l = minX;
        if (t < minY) t = minY;
        if (l + s->rw > maxX) l = maxX - s->rw;
        if (t + s->rh > maxY) t = maxY - s->rh;
        s->rx = l;
        s->ry = t;
        return;
    }

    const int hnd = s->dragMode - 2;   // 0..7

    if (s->square)
    {
        // 只用角：0=TL 2=TR 4=BR 6=BL
        float ax = 0, ay = 0;             // 锚点（对角）
        int sx = 0, sy = 0;               // 增长方向
        switch (hnd)
        {
        case 0: ax = r0; ay = b0; sx = -1; sy = -1; break;
        case 2: ax = l0; ay = b0; sx = +1; sy = -1; break;
        case 4: ax = l0; ay = t0; sx = +1; sy = +1; break;
        default: ax = r0; ay = t0; sx = -1; sy = +1; break;   // case 6
        }

        float sideX = (sx > 0) ? ((float)pt.x - s->offX) / s->scale - ax
                               : ax - ((float)pt.x - s->offX) / s->scale;
        float sideY = (sy > 0) ? ((float)pt.y - s->offY) / s->scale - ay
                               : ay - ((float)pt.y - s->offY) / s->scale;
        float side = (sideX > sideY) ? sideX : sideY;
        if (side < minS) side = minS;

        float limitX = (sx > 0) ? (maxX - ax) : (ax - minX);
        float limitY = (sy > 0) ? (maxY - ay) : (ay - minY);
        float limit = (limitX < limitY) ? limitX : limitY;
        if (limit < minS) limit = minS;
        if (side > limit) side = limit;

        float l = (sx > 0) ? ax : (ax - side);
        float t = (sy > 0) ? ay : (ay - side);
        s->rx = l;
        s->ry = t;
        s->rw = side;
        s->rh = side;
        return;
    }

    float l = l0, t = t0, r = r0, b = b0;
    switch (hnd)
    {
    case 0: l = l0 + dx; t = t0 + dy; break;
    case 1: t = t0 + dy; break;
    case 2: r = r0 + dx; t = t0 + dy; break;
    case 3: r = r0 + dx; break;
    case 4: r = r0 + dx; b = b0 + dy; break;
    case 5: b = b0 + dy; break;
    case 6: l = l0 + dx; b = b0 + dy; break;
    default: l = l0 + dx; break;      // case 7
    }

    if (l < minX) l = minX;
    if (t < minY) t = minY;
    if (r > maxX) r = maxX;
    if (b > maxY) b = maxY;
    if (r - l < minS) r = l + minS;
    if (b - t < minS) b = t + minS;
    if (r > maxX) { r = maxX; l = r - minS; }
    if (b > maxY) { b = maxY; t = b - minS; }

    s->rx = l;
    s->ry = t;
    s->rw = r - l;
    s->rh = b - t;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
static void PaintCanvas(HWND h, CanvasState* s, HDC hdcTarget)
{
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right;
    const int hgt = rc.bottom;
    if (w <= 0 || hgt <= 0) return;

    HDC mem = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, w, hgt);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT all = { 0, 0, w, hgt };
    PaintCheckerBackground(mem, all);

    if (!g_pSrcImage)
    {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(120, 126, 134));
        HGDIOBJ oldFont = SelectObject(mem, g_hUiFont);
        DrawTextW(mem, L"请先在上方点击「选择图片...」载入图片", -1, &all,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(mem, oldFont);
    }
    else
    {
        EnsureRect(h, s);

        float l = s->offX + s->rx * s->scale;
        float t = s->offY + s->ry * s->scale;
        float r = l + s->rw * s->scale;
        float b = t + s->rh * s->scale;

        RectF crop(l, t, r - l, b - t);
        RectF img(s->offX, s->offY, (float)g_srcW * s->scale, (float)g_srcH * s->scale);

        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeNone);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);

            // 图片（显示缓存）
            if (s->disp)
            {
                g.SetInterpolationMode(InterpolationModeNearestNeighbor);
                g.DrawImage(s->disp, (INT)s->offX, (INT)s->offY, s->dispW, s->dispH);
            }

            // 图片边框
            Pen imgPen(Color(190, 194, 200), 1.0f);
            g.DrawRectangle(&imgPen, img.X, img.Y, img.Width, img.Height);

            // 裁切框中超出图片的部分：填充色
            if (!s->transparentFill)
            {
                Region outside(crop);
                outside.Exclude(img);
                SolidBrush fillBr(Color(255, GetRValue(s->fill), GetGValue(s->fill), GetBValue(s->fill)));
                g.FillRegion(&fillBr, &outside);
            }

            // 裁切框外遮罩
            Region dim(RectF(0, 0, (REAL)w, (REAL)hgt));
            dim.Exclude(crop);
            SolidBrush dimBr(Color(96, 0, 0, 0));
            g.FillRegion(&dimBr, &dim);

            // 三分线
            {
                Pen grid(Color(90, 255, 255, 255), 1.0f);
                for (int i = 1; i <= 2; ++i)
                {
                    float gx = crop.X + crop.Width * i / 3.0f;
                    float gy = crop.Y + crop.Height * i / 3.0f;
                    g.DrawLine(&grid, gx, crop.Y, gx, crop.Y + crop.Height);
                    g.DrawLine(&grid, crop.X, gy, crop.X + crop.Width, gy);
                }
            }

            // 裁切框
            Pen white(Color(255, 255, 255, 255), 3.0f);
            g.DrawRectangle(&white, crop.X, crop.Y, crop.Width, crop.Height);
            Pen dark(Color(40, 40, 40), 1.0f);
            g.DrawRectangle(&dark, crop.X, crop.Y, crop.Width, crop.Height);

            // 控制点
            RECT hr[8];
            HandleRects(s, hr);
            SolidBrush hb(Color(255, 255, 255, 255));
            Pen hp(Color(60, 64, 70), 1.0f);
            for (int i = 0; i < 8; ++i)
            {
                if (s->square && (i % 2) == 1) continue;
                g.FillRectangle(&hb, (REAL)hr[i].left, (REAL)hr[i].top,
                                (REAL)(hr[i].right - hr[i].left), (REAL)(hr[i].bottom - hr[i].top));
                g.DrawRectangle(&hp, (REAL)hr[i].left, (REAL)hr[i].top,
                                (REAL)(hr[i].right - hr[i].left - 1), (REAL)(hr[i].bottom - hr[i].top - 1));
            }
        }
    }

    HBRUSH bBorder = CreateSolidBrush(RGB(205, 208, 212));
    FrameRect(mem, &all, bBorder);
    DeleteObject(bBorder);

    BitBlt(hdcTarget, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK CanvasProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    CanvasState* s = State(hWnd);

    switch (message)
    {
    case WM_NCCREATE:
        {
            CanvasState* st = new CanvasState();
            memset(st, 0, sizeof(CanvasState));
            st->fill = RGB(255, 255, 255);
            st->transparentFill = false;
            st->scale = 1.0f;
            st->hoverMode = 0;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)st);
            return TRUE;
        }

    case WM_NCDESTROY:
        if (s)
        {
            if (s->disp) delete s->disp;
            delete s;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (s)
        {
            RecalcMapping(hWnd, s);
            RebuildDisplay(hWnd, s);
            if (g_pSrcImage && s->rectSet)
            {
                ClampSize(s);
                ClampPosition(s);
            }
        }
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            if (s) PaintCanvas(hWnd, s, hdc);
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (s && g_pSrcImage)
        {
            EnsureRect(hWnd, s);
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int mode = HitTest(s, pt);
            if (mode == 0)      // 点空白处：把框移到以该点为中心？
                mode = 0;
            s->dragMode = mode;
            s->dragOrg = pt;
            s->dragRect[0] = s->rx;
            s->dragRect[1] = s->ry;
            s->dragRect[2] = s->rx + s->rw;
            s->dragRect[3] = s->ry + s->rh;
            if (mode)
            {
                SetCapture(hWnd);
                SetFocus(hWnd);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (s && g_pSrcImage)
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (s->dragMode)
            {
                ApplyDrag(s, pt);
                InvalidateCanvas(hWnd);
                NotifyParent(hWnd);
            }
            else
            {
                if (!s->tracking)
                {
                    TRACKMOUSEEVENT tme = {};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hWnd;
                    TrackMouseEvent(&tme);
                    s->tracking = true;
                }
                int hv = HitTest(s, pt);
                if (hv != s->hoverMode)
                {
                    s->hoverMode = hv;
                    SetCursor(LoadCursorW(nullptr,
                        (hv == 1) ? IDC_SIZEALL :
                        (hv == 2 || hv == 6) ? IDC_SIZENWSE :
                        (hv == 4 || hv == 8) ? IDC_SIZENESW :
                        (hv == 3 || hv == 7) ? IDC_SIZEWE :
                        (hv == 5 || hv == 9) ? IDC_SIZENS : IDC_ARROW));
                }
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (s && s->dragMode)
        {
            s->dragMode = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_MOUSELEAVE:
        if (s) s->tracking = false;
        return 0;

    case WM_CAPTURECHANGED:
        if (s) s->dragMode = 0;
        return 0;

    case WM_SETCURSOR:
        if (s && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr,
                (s->hoverMode == 1) ? IDC_SIZEALL :
                (s->hoverMode == 2 || s->hoverMode == 6) ? IDC_SIZENWSE :
                (s->hoverMode == 4 || s->hoverMode == 8) ? IDC_SIZENESW :
                (s->hoverMode == 3 || s->hoverMode == 7) ? IDC_SIZEWE :
                (s->hoverMode == 5 || s->hoverMode == 9) ? IDC_SIZENS : IDC_ARROW));
            return TRUE;
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

static bool EnsureClass()
{
    static bool done = false;
    if (done) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CROPCANVAS_CLASS;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    done = true;
    return true;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
HWND CropCanvas_Create(HWND parent, int id, bool squareMode)
{
    if (!EnsureClass()) return nullptr;

    HWND h = CreateWindowExW(0, CROPCANVAS_CLASS, L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (h)
    {
        CanvasState* s = State(h);
        if (s) s->square = squareMode;
    }
    return h;
}

void CropCanvas_ResetToImage(HWND h)
{
    CanvasState* s = State(h);
    if (!s || !g_pSrcImage) return;
    s->rx = 0.0f;
    s->ry = 0.0f;
    s->rw = (float)g_srcW;
    s->rh = (float)g_srcH;
    s->rectSet = true;
    ClampSize(s);
    ClampPosition(s);
    InvalidateCanvas(h);
    NotifyParent(h);
}

void CropCanvas_ResetToSquareCenter(HWND h)
{
    CanvasState* s = State(h);
    if (!s || !g_pSrcImage) return;

    float side = ((float)g_srcW < (float)g_srcH) ? (float)g_srcW : (float)g_srcH;
    s->rw = side;
    s->rh = side;
    s->rx = ((float)g_srcW - side) * 0.5f;
    s->ry = ((float)g_srcH - side) * 0.5f;
    s->rectSet = true;
    ClampSize(s);
    ClampPosition(s);
    InvalidateCanvas(h);
    NotifyParent(h);
}

void CropCanvas_GetRect(HWND h, float* x, float* y, float* w, float* hh)
{
    CanvasState* s = State(h);
    if (!s) return;
    EnsureRect(h, s);
    if (x) *x = s->rx;
    if (y) *y = s->ry;
    if (w) *w = s->rw;
    if (hh) *hh = s->rh;
}

void CropCanvas_SetFill(HWND h, COLORREF c, bool transparent)
{
    CanvasState* s = State(h);
    if (!s) return;
    s->fill = c;
    s->transparentFill = transparent;
    InvalidateCanvas(h);
}

void CropCanvas_RefreshImage(HWND h)
{
    CanvasState* s = State(h);
    if (!s) return;

    s->rectSet = false;
    RecalcMapping(h, s);
    RebuildDisplay(h, s);

    if (g_pSrcImage)
    {
        if (s->square)
            CropCanvas_ResetToSquareCenter(h);
        else
            CropCanvas_ResetToImage(h);
    }
    else
    {
        InvalidateCanvas(h);
    }
}
