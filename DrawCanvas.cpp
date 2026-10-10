// ============================================================================
//  DrawCanvas.cpp —— 涂鸦画布
//    · 画笔绘制：矢量笔迹（图像坐标保存），可撤销 / 清空
//    · 吸色：点击画面取色，取完自动回到画笔模式
//    · 输出时按原图分辨率重新合成一张位图（见 DrawCanvas_RenderFull）
// ============================================================================
#include "App.h"
#include <windowsx.h>

using namespace Gdiplus;

namespace
{
    struct Stroke
    {
        std::vector<PointF> pts;
        Color color;
        float width;          // 图像坐标下的线宽
    };

    struct DoodleState
    {
        std::vector<Stroke> strokes;
        Stroke  cur;
        bool    drawing = false;
        Color   brush = Color(255, 235, 40, 45);
        float   brushW = 5.0f;        // 显示像素
        bool    pick = false;

        Bitmap* srcDisp = nullptr;    // 缩放后的原图（显示用）
        Bitmap* overlay = nullptr;    // 显示尺寸的笔迹层
        Bitmap* sample = nullptr;     // 吸色采样（最长边限制）
        float   sampleScale = 1.0f;

        float   offX = 0, offY = 0, scale = 1.0f;
        int     cw = 0, ch = 0;
    };

    DoodleState* St(HWND h) { return (DoodleState*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    const int PAD = 12;
}

// ---------------------------------------------------------------------------
//  笔迹绘制
// ---------------------------------------------------------------------------
static void DrawWholeStroke(Graphics& g, const Stroke& s)
{
    if (s.pts.empty()) return;

    if (s.pts.size() == 1)
    {
        SolidBrush b(s.color);
        g.FillEllipse(&b, s.pts[0].X - s.width * 0.5f, s.pts[0].Y - s.width * 0.5f,
                      s.width, s.width);
        return;
    }

    Pen p(s.color, s.width);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    g.DrawLines(&p, s.pts.data(), (INT)s.pts.size());
}

static void DrawLastSegment(Graphics& g, const Stroke& s)
{
    if (s.pts.size() < 2) { DrawWholeStroke(g, s); return; }

    PointF seg[2] = { s.pts[s.pts.size() - 2], s.pts[s.pts.size() - 1] };
    Pen p(s.color, s.width);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    g.DrawLines(&p, seg, 2);
}

// 把图像坐标映射到画布坐标后重绘笔迹；incrementalOnly=true 时只补画当前笔的最后一小段
static void RenderOverlay(DoodleState* st, bool incrementalOnly)
{
    if (!st->overlay) return;

    Graphics g(st->overlay);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    Matrix m(st->scale, 0, 0, st->scale, st->offX, st->offY);
    g.SetTransform(&m);

    if (incrementalOnly)
    {
        if (st->drawing) DrawLastSegment(g, st->cur);
        return;
    }

    g.Clear(Color(0, 0, 0, 0));
    for (size_t i = 0; i < st->strokes.size(); ++i)
        DrawWholeStroke(g, st->strokes[i]);
    if (st->drawing) DrawWholeStroke(g, st->cur);
}

// ---------------------------------------------------------------------------
//  显示缓存
// ---------------------------------------------------------------------------
static void RecalcMapping(DoodleState* st, int cw, int ch)
{
    st->cw = cw;
    st->ch = ch;
    st->offX = 0;
    st->offY = 0;
    st->scale = 1.0f;

    if (!g_pSrcImage || g_srcW == 0 || g_srcH == 0) return;

    const float aw = (float)(cw - PAD * 2);
    const float ah = (float)(ch - PAD * 2);
    if (aw <= 4 || ah <= 4) return;

    const float sx = aw / (float)g_srcW;
    const float sy = ah / (float)g_srcH;
    st->scale = (sx < sy) ? sx : sy;
    st->offX = (cw - (float)g_srcW * st->scale) * 0.5f;
    st->offY = (ch - (float)g_srcH * st->scale) * 0.5f;
}

static void RebuildDisplay(DoodleState* st)
{
    if (st->srcDisp) { delete st->srcDisp; st->srcDisp = nullptr; }
    if (st->overlay) { delete st->overlay; st->overlay = nullptr; }
    if (st->sample) { delete st->sample; st->sample = nullptr; }

    if (!g_pSrcImage || st->cw <= 0 || st->ch <= 0) return;

    const int dw = (int)((float)g_srcW * st->scale + 0.5f);
    const int dh = (int)((float)g_srcH * st->scale + 0.5f);

    if (dw > 0 && dh > 0)
    {
        st->srcDisp = new Bitmap(dw, dh, PixelFormat32bppPARGB);
        if (st->srcDisp->GetLastStatus() == Ok)
        {
            Graphics g(st->srcDisp);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.SetCompositingMode(CompositingModeSourceCopy);
            g.DrawImage(g_pSrcImage, 0, 0, dw, dh);
        }
        else
        {
            delete st->srcDisp;
            st->srcDisp = nullptr;
        }
    }

    st->overlay = new Bitmap(st->cw, st->ch, PixelFormat32bppARGB);
    if (st->overlay->GetLastStatus() != Ok)
    {
        delete st->overlay;
        st->overlay = nullptr;
        return;
    }
    RenderOverlay(st, false);
}

// 吸色用采样位图（最长边 <= 2048，避免超大图占内存）
static void EnsureSample(DoodleState* st)
{
    if (st->sample || !g_pSrcImage) return;

    const int maxDim = 2048;
    int sw = (int)g_srcW, sh = (int)g_srcH;
    if (sw > maxDim || sh > maxDim)
    {
        const double k = (double)maxDim / (double)((sw > sh) ? sw : sh);
        sw = (int)(sw * k + 0.5);
        sh = (int)(sh * k + 0.5);
    }
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;

    st->sample = new Bitmap(sw, sh, PixelFormat32bppARGB);
    if (st->sample->GetLastStatus() != Ok)
    {
        delete st->sample;
        st->sample = nullptr;
        return;
    }
    {
        Graphics g(st->sample);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.DrawImage(g_pSrcImage, 0, 0, sw, sh);
    }
    st->sampleScale = (float)sw / (float)g_srcW;
}

// ---------------------------------------------------------------------------
//  鼠标 -> 图像坐标
// ---------------------------------------------------------------------------
static bool ToImage(DoodleState* st, int x, int y, float* ix, float* iy)
{
    if (st->scale <= 0) return false;
    *ix = ((float)x - st->offX) / st->scale;
    *iy = ((float)y - st->offY) / st->scale;
    return true;
}

static void PickColor(HWND hWnd, DoodleState* st, int x, int y)
{
    float ix, iy;
    if (!ToImage(st, x, y, &ix, &iy)) return;
    if (ix < 0 || iy < 0 || ix >= (float)g_srcW || iy >= (float)g_srcH) return;

    EnsureSample(st);
    if (!st->sample) return;

    int sx = (int)(ix * st->sampleScale);
    int sy = (int)(iy * st->sampleScale);
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int)st->sample->GetWidth())  sx = (int)st->sample->GetWidth() - 1;
    if (sy >= (int)st->sample->GetHeight()) sy = (int)st->sample->GetHeight() - 1;

    Color c;
    if (st->sample->GetPixel(sx, sy, &c) != Ok) return;

    const COLORREF cr = RGB(c.GetR(), c.GetG(), c.GetB());
    st->brush = Color(255, c.GetR(), c.GetG(), c.GetB());
    st->pick = false;
    SendMessageW(GetParent(hWnd), WM_APP_PICKED_COLOR, (WPARAM)cr, 0);
}

// ---------------------------------------------------------------------------
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    DoodleState* st = St(hWnd);

    switch (message)
    {
    case WM_CREATE:
        {
            DoodleState* s = new DoodleState();
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)s);
        }
        return 0;

    case WM_DESTROY:
        if (st)
        {
            if (st->srcDisp) delete st->srcDisp;
            if (st->overlay) delete st->overlay;
            if (st->sample) delete st->sample;
            delete st;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        return 0;

    case WM_SIZE:
        if (st)
        {
            RecalcMapping(st, LOWORD(lParam), HIWORD(lParam));
            RebuildDisplay(st);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);

            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);

            PaintCheckerBackground(mem, rc);

            if (st && g_pSrcImage && st->srcDisp && st->overlay)
            {
                Graphics g(mem);
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                g.DrawImage(st->srcDisp, (INT)st->offX, (INT)st->offY,
                            st->srcDisp->GetWidth(), st->srcDisp->GetHeight());
                g.DrawImage(st->overlay, 0, 0);

                Pen p(Color(255, 150, 150, 150));
                g.DrawRectangle(&p, (INT)st->offX, (INT)st->offY,
                                st->srcDisp->GetWidth() - 1, st->srcDisp->GetHeight() - 1);
            }
            else
            {
                SetBkMode(mem, TRANSPARENT);
                SetTextColor(mem, RGB(120, 120, 120));
                const wchar_t* tip = L"点击上方「选择图片...」载入图片\r\n载入后即可用画笔涂鸦";
                DrawTextW(mem, tip, -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            }

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_SETCURSOR:
        if (st && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
        if (st && g_pSrcImage)
        {
            SetFocus(hWnd);
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);

            if (st->pick)
            {
                PickColor(hWnd, st, x, y);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            float ix, iy;
            if (ToImage(st, x, y, &ix, &iy))
            {
                st->cur.pts.clear();
                st->cur.pts.push_back(PointF(ix, iy));
                st->cur.color = st->brush;
                st->cur.width = (st->scale > 0.0f) ? (st->brushW / st->scale) : st->brushW;
                if (st->cur.width < 1.0f) st->cur.width = 1.0f;
                st->drawing = true;
                SetCapture(hWnd);
                RenderOverlay(st, false);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (st && st->drawing)
        {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            float ix, iy;
            if (ToImage(st, x, y, &ix, &iy))
            {
                const PointF& last = st->cur.pts.back();
                const float dx = (ix - last.X) * st->scale;
                const float dy = (iy - last.Y) * st->scale;
                if (dx * dx + dy * dy >= 1.0f)     // 至少移动 1 显示像素才记点
                {
                    st->cur.pts.push_back(PointF(ix, iy));
                    RenderOverlay(st, true);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (st && st->drawing)
        {
            ReleaseCapture();
            st->drawing = false;
            if (!st->cur.pts.empty())
            {
                st->strokes.push_back(st->cur);
                st->cur.pts.clear();
            }
            RenderOverlay(st, false);
            InvalidateRect(hWnd, nullptr, FALSE);
            // 通知父页（撤销按钮可用性）
            PostMessageW(GetParent(hWnd), WM_APP_RECT_CHANGED, 0, 0);
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
HWND DrawCanvas_Create(HWND parent, int id)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = Proc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = DRAWCANVAS_CLASS;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(WS_EX_CLIENTEDGE, DRAWCANVAS_CLASS, L"",
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

void DrawCanvas_RefreshImage(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return;
    RECT rc;
    GetClientRect(h, &rc);
    RecalcMapping(st, rc.right, rc.bottom);
    RebuildDisplay(st);
    InvalidateRect(h, nullptr, FALSE);
}

void DrawCanvas_SetBrush(HWND h, COLORREF color, int width)
{
    DoodleState* st = St(h);
    if (!st) return;
    st->brush = Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
    st->brushW = (float)((width > 0) ? width : 1);
}

void DrawCanvas_SetPickMode(HWND h, bool on)
{
    DoodleState* st = St(h);
    if (!st) return;
    st->pick = on;
    SetCursor(LoadCursorW(nullptr, st->pick ? IDC_CROSS : IDC_ARROW));
}

bool DrawCanvas_CanUndo(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return false;
    return !st->strokes.empty() || st->drawing;
}

void DrawCanvas_Undo(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return;
    if (st->drawing)
    {
        st->drawing = false;
        st->cur.pts.clear();
        ReleaseCapture();
    }
    else if (!st->strokes.empty())
    {
        st->strokes.pop_back();
    }
    RenderOverlay(st, false);
    InvalidateRect(h, nullptr, FALSE);
}

void DrawCanvas_Clear(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return;
    st->strokes.clear();
    st->cur.pts.clear();
    st->drawing = false;
    RenderOverlay(st, false);
    InvalidateRect(h, nullptr, FALSE);
}

Bitmap* DrawCanvas_RenderFull(HWND h)
{
    DoodleState* st = St(h);
    if (!st || !g_pSrcImage) return nullptr;

    Bitmap* out = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
    if (out->GetLastStatus() != Ok)
    {
        delete out;
        return nullptr;
    }

    Graphics g(out);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.Clear(Color(0, 0, 0, 0));
    for (size_t i = 0; i < st->strokes.size(); ++i)
        DrawWholeStroke(g, st->strokes[i]);

    return out;
}
