// ============================================================================
//  CropCanvas.cpp —— 可交互的裁切画布
//    · 拖动框内：移动裁切框
//    · 拖动 8 个控制点（正方形模式 4 个角）：缩放裁切框
//    · 裁切框允许超出图片范围，超出部分按填充色/透明显示
// ============================================================================
//
//  【文件说明】—— 给第一次读本文件的人
//
//  一、这个文件负责什么
//      它实现了一个"自定义窗口类" Pic246CropCanvas（窗口类名见 App.h 里的
//      宏 CROPCANVAS_CLASS）。这个窗口不显示文字也不接收输入框，它是一个
//      "自绘控件"，专门干四件事：
//        1. 把全局源图 g_pSrcImage 等比缩放后居中画在自己的客户区；
//        2. 在图片上叠一个"裁切框"，并画遮罩、三分线、8 个白色控制点；
//        3. 处理鼠标拖拽：在框内按下拖动 = 整体平移；按住控制点拖动 = 缩放；
//        4. 裁切框每次变化时，用自定义消息 WM_APP_RECT_CHANGED 通知父窗口。
//      注意：本文件只负责"显示"和"交互"，真正把像素抠出来的裁切动作由
//      使用它的父页面完成。
//
//  二、和哪些文件配合
//      · App.h          —— 公共声明所在。本文件用到的全局量/工具都在这里：
//                          g_pSrcImage（当前源图）、g_srcW/g_srcH（源图宽高）、
//                          ResizeBitmap()（缩放生成 Bitmap）、
//                          PaintCheckerBackground()（画透明棋盘格）、
//                          D()（DPI 换算，1 DIP = g_dpi/96 像素）、
//                          WM_APP_RECT_CHANGED、CROPCANVAS_CLASS。
//      · PageCrop.cpp   —— "图片裁切"页：创建本控件、响应
//                          WM_APP_RECT_CHANGED、调用 CropCanvas_GetRect() 取值。
//      · PageIco.cpp    —— "ICO 转换"页：复用一模一样的控件（这正是把画布
//                          做成独立窗口类的原因——两页共用一份代码）。
//      · 本文件对外只暴露 5 个 CropCanvas_* 函数（声明在 App.h 第 280~285 行），
//        父页面不需要知道内部细节。
//
//  三、大致交互流程（一次完整的拖动）
//      父页面：CropCanvas_Create() 建窗体 → CropCanvas_SetFill() 设填充色
//              → 换图后 CropCanvas_RefreshImage() 刷新
//      鼠标按下 WM_LBUTTONDOWN：HitTest() 判断按到了哪里（框内/某个控制点）
//              → 记住起点 → SetCapture() 把鼠标"锁"在本窗口
//      鼠标移动 WM_MOUSEMOVE：ApplyDrag() 根据位移改写裁切框
//              → InvalidateRect() 请求重画 → SendMessage(父窗口, WM_APP_RECT_CHANGED)
//      鼠标抬起 WM_LBUTTONUP：dragMode 清零 → ReleaseCapture() 释放鼠标
//      父页面收到通知后调用 CropCanvas_GetRect() 拿到裁切框做后续处理。
//
//  四、建议阅读顺序
//      CanvasState（状态结构体）→ State()（怎么取状态）
//      → RecalcMapping()（坐标换算的地基）→ HandleRects()/HitTest()（命中测试）
//      → ApplyDrag()（拖动数学）→ PaintCanvas()（双缓冲绘制）
//      → CanvasProc()（消息分发）→ 文件末尾的对外接口函数。
// ============================================================================
#include "App.h"

// 把 Gdiplus 里的类型（RectF / Graphics / Pen / SolidBrush / Region ...）引进
// 全局命名空间，这样下文可以少写 Gdiplus:: 前缀。
// 注意 using 放在全局作用域的 .cpp 里是安全的（不会污染别的编译单元）。
using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
// CanvasState —— 本控件的全部"成员变量"。
//
// 【为什么要这个东西】Windows 的窗口过程（WndProc）是一个普通的 C 风格
// 全局函数，没有 this 指针，因此不能像 C++ 类那样直接拥有成员变量。
// 标准做法是：
//   1) 收到 WM_NCCREATE 时 new 一个状态结构体；
//   2) 用 SetWindowLongPtrW(hWnd, GWLP_USERDATA, 指针) 把它挂到窗口上；
//   3) 以后想用时用 GetWindowLongPtrW(hWnd, GWLP_USERDATA) 取回来
//      （本文件封装成了 State() 函数）；
//   4) 收到 WM_NCDESTROY（窗口销毁前最后一条消息）时 delete，否则内存泄漏。
// GWLP_USERDATA 是系统专门留给应用程序存放自定义指针的一个"槽位"，
// 系统自己不会碰它。
struct CanvasState
{
    bool     square;            // 只允许正方形
    COLORREF fill;              // 填充色
    bool     transparentFill;   // 超出图片部分是否透明
    float    rx, ry, rw, rh;    // 裁切框（图像坐标，可为负）
    bool     rectSet;           // 是否已初始化
    int      dragMode;          // 0 无 1 移动 2..9 控制点
    POINT    dragOrg;           // 按下左键那一刻的鼠标客户区坐标（拖动基准点）
    float    dragRect[4];       // 按下瞬间的裁切框左右上下（图像坐标），
                                // 拖动过程中始终基于它计算，避免误差累加
    int      hoverMode;         // 当前鼠标悬停位置（含义同 dragMode），
                                // 用于决定显示哪种鼠标指针形状
    bool     tracking;          // 是否已调用 TrackMouseEvent 申请 WM_MOUSELEAVE
    Bitmap*  disp;              // 显示缓存（已缩放到屏幕尺寸）
                                // 预先缩放一份，避免每帧从原图缩放（很慢）
    int      dispW, dispH;      // 上面缓存的像素宽高
    float    scale;             // 图像 -> 窗口
    float    viewMinX, viewMaxX;   // 画布可视范围（图像坐标）
    float    viewMinY, viewMaxY;   // 即客户区矩形反算回图像坐标的结果
    float    offX, offY;        // 图片左上角在客户区中的像素偏移（居中留白）
};

// State() —— 从窗口句柄取回本窗口的状态结构体指针。
// 参数：h 本控件窗口句柄。
// 返回：CanvasState*；如果还没创建（WM_NCCREATE 之前）则是 nullptr，
//       所以调用它的地方都要判空，不能直接解引用。
// 注意：这是"辅助函数"而不是宏，好处是类型安全、可以下断点。
static CanvasState* State(HWND h)
{
    return (CanvasState*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

// InvalidateCanvas() —— 请求 Windows 重画整个客户区。
// 参数 FALSE 表示"不要先擦背景"：因为我们在 PaintCanvas 里自己画满了整块，
// 让系统擦一次只会造成闪烁。
// 【新手坑】改完任何会影响外观的状态（裁切框、填充色……）后必须调用它，
// 否则界面不会更新——Windows 不会自动发现你的数据变了。
static void InvalidateCanvas(HWND h)
{
    InvalidateRect(h, nullptr, FALSE);
}

// NotifyParent() —— 告诉父窗口"裁切框变了"。
// 用 SendMessage（同步）而不是 PostMessage（异步），这样父窗口处理完才返回，
// 父窗口里可以立即用 CropCanvas_GetRect() 取到最新值。
// wParam 传的是本控件的 HWND，父窗口靠它区分是哪个子控件发的通知
// （图片裁切页和 ICO 页可能有多个画布）。
static void NotifyParent(HWND h)
{
    HWND parent = GetParent(h);
    if (parent)
        SendMessageW(parent, WM_APP_RECT_CHANGED, (WPARAM)h, 0);
}

// ---------------------------------------------------------------------------
// 坐标映射
// ---------------------------------------------------------------------------
// RecalcMapping() —— 重新计算"图像坐标 ↔ 客户区坐标"的换算参数，
// 结果写入 s->scale（缩放比）和 s->offX/offY（图片左上角在窗口里的位置）。
//
// 公式：客户区像素 = 图像坐标 * scale + offX/offY。
//      反算：图像坐标 = (客户区像素 - offX/offY) / scale。
//
// 参数：h 控件窗口；s 状态指针（调用方保证非空）。
// 何时被调用：窗口大小变化（WM_SIZE）以及换图（CropCanvas_RefreshImage）。
//
// 【为什么要有 viewMin/Max】裁切框允许超出图片，但又不希望它拖到看不见的
// 地方，所以把"客户区矩形"反算成图像坐标，得到可视范围，用来限制拖动。
static void RecalcMapping(HWND h, CanvasState* s)
{
    RECT rc;
    GetClientRect(h, &rc);              // 客户区大小（不含标题栏/边框，子窗口基本就是全部）
    const int cw = rc.right;            // 客户区宽
    const int ch = rc.bottom;           // 客户区高
    // 没有图片，或者窗口太小（还没布局好）时，退化成 1:1 的无偏移映射，
    // 防止后面除零或算出无穷大。
    if (!g_pSrcImage || cw < 20 || ch < 20)
    {
        s->scale = 1.0f;
        s->offX = 0.0f;
        s->offY = 0.0f;
        return;
    }

    const float pad = (float)D(16);     // 四周留白 16 DIP（按 DPI 换算成像素）
    float availW = ((float)cw - pad * 2.0f) * 0.78f;   // 再乘 0.78：不铺满，
    float availH = ((float)ch - pad * 2.0f) * 0.78f;   // 留出一点呼吸空间，视觉上更舒服
    if (availW < 8) availW = 8;         // 兜底，避免下面出现 0 或负数
    if (availH < 8) availH = 8;

    // 等比缩放：分别算出"按宽度铺满"和"按高度铺满"的比例，取小的那个，
    // 这样图片两个方向都不会超出可用区域（contain 策略）。
    float sx = availW / (float)g_srcW;
    float sy = availH / (float)g_srcH;
    s->scale = (sx < sy) ? sx : sy;     // 取较小者 = 完整装下
    if (s->scale <= 0.0f) s->scale = 1.0f;   // 源图宽高异常时兜底

    // 居中：把剩余空间平分到左右/上下。
    s->offX = ((float)cw - (float)g_srcW * s->scale) * 0.5f;
    s->offY = ((float)ch - (float)g_srcH * s->scale) * 0.5f;

    // 把客户区左上角 (0,0) 和右下角 (cw,ch) 反算成图像坐标，得到可视范围。
    // 因为图片居中且可能小于客户区，所以 minX/minY 通常是负数。
    s->viewMinX = (0.0f - s->offX) / s->scale;
    s->viewMaxX = ((float)cw - s->offX) / s->scale;
    s->viewMinY = (0.0f - s->offY) / s->scale;
    s->viewMaxY = ((float)ch - s->offY) / s->scale;
}

// RebuildDisplay() —— 重新生成"显示缓存"：把源图按当前 scale 缩放成一份
// 位图存进 s->disp。绘制时直接贴这份缓存，就不必每帧都做一次高质量缩放。
//
// 参数：h 控件窗口（本函数目前用不到，保留是为了接口统一，末尾有 (void)h）。
// 何时被调用：WM_SIZE、换图刷新。
// 【内存】先 delete 旧的再 new，避免反复 resize 时泄漏。
static void RebuildDisplay(HWND h, CanvasState* s)
{
    if (s->disp) { delete s->disp; s->disp = nullptr; }
    s->dispW = s->dispH = 0;
    if (!g_pSrcImage) return;

    // 加 0.5f 再取整 = 四舍五入（C 的强制转换是截断，直接转会一直偏小）。
    int dw = (int)((float)g_srcW * s->scale + 0.5f);
    int dh = (int)((float)g_srcH * s->scale + 0.5f);
    if (dw < 1) dw = 1;                 // GDI+ 不接受 0 宽/高的位图
    if (dh < 1) dh = 1;

    std::wstring err;                   // ResizeBitmap 出错时把原因写在这里
    s->disp = ResizeBitmap(g_pSrcImage, dw, dh, err);
    if (s->disp)                        // 失败时保持 disp == nullptr，
    {                                   // 绘制时会退化成不画图片（不会崩溃）
        s->dispW = dw;
        s->dispH = dh;
    }
    (void)h;                            // 显式忽略参数，避免"未使用参数"警告
}

// ---------------------------------------------------------------------------
// 裁切框约束
// ---------------------------------------------------------------------------
// EnsureRect() —— 惰性初始化裁切框。
// 第一次用到裁切框时（还没设置过），默认让它正好等于整张图片。
// 参数：h 控件窗口（同样只是为接口统一，末尾 (void)h）。
// 【为什么要 rectSet】换图后要先重置一次，之后用户的拖动结果才不会被覆盖。
static void EnsureRect(HWND h, CanvasState* s)
{
    if (!g_pSrcImage) { s->rectSet = false; return; }   // 没图就别初始化
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

// ClampSize() —— 把裁切框的"尺寸"限制在合理区间。只改宽高，不改位置。
//   · 最小 4 像素：太小的框用户抓不住控制点，也容易在后续裁切时出错。
//   · 最大为图片的 3 倍：允许超出图片，但不允许无限大。
// 正方形模式下，取宽高中较大的一边作为边长（保证用户的意图不被缩小），
// 再一起夹到上下限里，最后宽高相等。
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
        // 先用较大的那一边当作正方形边长，尽量不"缩水"用户已有的框。
        float side = (s->rw > s->rh) ? s->rw : s->rh;
        if (side > maxW) side = maxW;
        if (side > maxH) side = maxH;
        if (side < minS) side = minS;
        s->rw = s->rh = side;       // 两个一起赋值：保证严格正方形
    }
}

// 把裁切框平移回合理范围（保持尺寸，正方形模式不会被破坏）
// 【为什么只挪位置不改尺寸】如果在这里改尺寸，正方形模式就可能被破坏；
// 而且用户拉伸时的"手感"会被莫名的缩放打断。
// 注意两次夹取的顺序：先夹左上角，再根据右下角回推左上角，最后再夹一次
// 左上角——因为当裁切框比可视范围还大时，"右下角不出界"会把左上角推出去，
// 所以需要最后再纠正一次（此时右下角必定已经越界，但至少保证左上角可见）。
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
// HandleRects() —— 算出 8 个控制点在"客户区坐标"下的矩形区域。
// 参数：out 输出数组，长度 8；顺序固定，和后面的 switch 一一对应：
//       0=左上 1=上中 2=右上 3=右中 4=右下 5=下中 6=左下 7=左中
// 换算：先用 (offX + rx*scale) 把裁切框从图像坐标变换到客户区像素，
//       再在 8 个位置各自生成一个边长 2*hs 的小方块。
// 注意：这些矩形可以超出客户区（裁切框允许出界），不做裁剪。
static void HandleRects(CanvasState* s, RECT out[8])
{
    // 裁切框在客户区里的四条边（像素）
    float l = s->offX + s->rx * s->scale;   // 左：图像坐标 → 客户区像素
    float t = s->offY + s->ry * s->scale;   // 上
    float r = l + s->rw * s->scale;         // 右 = 左 + 宽*缩放
    float b = t + s->rh * s->scale;         // 下
    const int hs = D(5);   // 半边长（即控制点方块为 10 DIP 见方）

    float cx = (l + r) * 0.5f;              // 中线 x：上中/下中控制点用
    float cy = (t + b) * 0.5f;              // 中线 y：左中/右中控制点用

    // 8 个控制点的中心坐标，顺序见函数头注释。
    const float xs[8] = { l, cx, r, r, r, cx, l, l };
    const float ys[8] = { t, t, t, cy, b, b, b, cy };

    for (int i = 0; i < 8; ++i)
    {
        // 由中心点向四周扩 hs 像素，得到一个小方块。
        out[i].left = (LONG)(xs[i] - hs);
        out[i].top = (LONG)(ys[i] - hs);
        out[i].right = (LONG)(xs[i] + hs);
        out[i].bottom = (LONG)(ys[i] + hs);
    }
}

// HitTest() —— 判断鼠标点 pt 落在了哪里。
// 参数：s 状态；pt 客户区坐标点。
// 返回：0 = 空白处（不可拖动）
//       1 = 裁切框内部（整体移动）
//       2..9 = 第 (返回值-2) 个控制点（缩放）
//       —— 用 1 表示"移动"、用 2+i 表示控制点，好处是"非 0 即表示可以拖动"，
//          而且减 2 就直接得到 0..7 的索引。
// 【顺序很重要】必须先测控制点、再测框内。因为控制点位于边框上，
// 与框内部有重叠；先测控制点用户才抓得住这些小手柄。
static int HitTest(CanvasState* s, POINT pt)
{
    RECT hr[8];
    HandleRects(s, hr);

    for (int i = 0; i < 8; ++i)
    {
        if (s->square && (i % 2) == 1)     // 正方形模式只用 4 个角
            continue;                      // 奇数索引是边中点，跳过
        if (PtInRect(&hr[i], pt))          // PtInRect 是 Win32 自带函数
            return i + 2;                  // 2..9
    }

    // 控制点都没命中，再看是不是在裁切框内部（矩形范围判断）。
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
// ApplyDrag() —— 拖动过程中的核心：根据当前鼠标位置重新计算裁切框。
//
// 参数：s 状态；pt 当前鼠标客户区坐标。
//
// 关键思路：不要用"上一帧位置"，而是始终用"按下瞬间记录的 dragRect +
// 总位移（pt - dragOrg）"来算。这样即使某一帧丢了消息也不会累积误差。
//
// 三种情况：
//   · dragMode == 1：整体平移，只改位置不改大小；
//   · 正方形模式 + 角度控制点：以对角为锚点，取 x/y 方向位移较大的一个作为
//     边长（跟手），再受"到可视边界还能长多少"的限制；
//   · 普通模式 + 任意控制点：改被拖的那一条或两条边。
static void ApplyDrag(CanvasState* s, POINT pt)
{
    if (!g_pSrcImage || s->dragMode == 0) return;

    // 把鼠标位移从"客户区像素"换算成"图像坐标"的位移（除以缩放比）。
    const float dx = (float)(pt.x - s->dragOrg.x) / s->scale;
    const float dy = (float)(pt.y - s->dragOrg.y) / s->scale;

    const float minX = s->viewMinX, maxX = s->viewMaxX;
    const float minY = s->viewMinY, maxY = s->viewMaxY;
    const float minS = 4.0f;            // 最小边长，和 ClampSize 保持一致

    // 按下瞬间的四条边，后面全部基于它计算。
    float l0 = s->dragRect[0], t0 = s->dragRect[1], r0 = s->dragRect[2], b0 = s->dragRect[3];

    if (s->dragMode == 1)          // 移动
    {
        // 只挪位置：新左上 = 原左上 + 位移，然后夹到可视范围内。
        float l = l0 + dx;
        float t = t0 + dy;
        if (l < minX) l = minX;
        if (t < minY) t = minY;
        if (l + s->rw > maxX) l = maxX - s->rw;   // 右边不许超出可视范围
        if (t + s->rh > maxY) t = maxY - s->rh;   // 下边不许超出
        s->rx = l;
        s->ry = t;
        return;
    }

    const int hnd = s->dragMode - 2;   // 0..7

    if (s->square)
    {
        // 只用角：0=TL 2=TR 4=BR 6=BL
        // 拖某个角时，"对角"固定不动，作为锚点；框朝拖动方向生长。
        float ax = 0, ay = 0;             // 锚点（对角）
        int sx = 0, sy = 0;               // 增长方向
        switch (hnd)
        {
        case 0: ax = r0; ay = b0; sx = -1; sy = -1; break;   // 拖左上角，锚点在右下，向左上长
        case 2: ax = l0; ay = b0; sx = +1; sy = -1; break;   // 拖右上角，锚点在左下
        case 4: ax = l0; ay = t0; sx = +1; sy = +1; break;   // 拖右下角，锚点在左上
        default: ax = r0; ay = t0; sx = -1; sy = +1; break;   // case 6
        }

        // 鼠标相对锚点在两个方向上的距离，换算到图像坐标。
        // 因为正方形只能朝 sx/sy 的方向长，所以用三目运算把反方向的负值
        // 处理掉（相当于这里不参与 max 比较）。
        float sideX = (sx > 0) ? ((float)pt.x - s->offX) / s->scale - ax
                               : ax - ((float)pt.x - s->offX) / s->scale;
        float sideY = (sy > 0) ? ((float)pt.y - s->offY) / s->scale - ay
                               : ay - ((float)pt.y - s->offY) / s->scale;
        float side = (sideX > sideY) ? sideX : sideY;   // 取较大者：跟手，不会"掉队"
        if (side < minS) side = minS;                   // 不许小于最小边长

        // 还要保证正方形不超出可视范围：算出 x、y 两个方向各自还能长多少，
        // 取较小的那个作为边长上限，否则一旦超界框就会跑出屏幕。
        float limitX = (sx > 0) ? (maxX - ax) : (ax - minX);
        float limitY = (sy > 0) ? (maxY - ay) : (ay - minY);
        float limit = (limitX < limitY) ? limitX : limitY;
        if (limit < minS) limit = minS;                 // 空间太小时至少保证最小边长
        if (side > limit) side = limit;

        // 由锚点 + 边长反推左上角：朝负方向生长时，左上角 = 锚点 - 边长。
        float l = (sx > 0) ? ax : (ax - side);
        float t = (sy > 0) ? ay : (ay - side);
        s->rx = l;
        s->ry = t;
        s->rw = side;
        s->rh = side;                                   // 宽高相等 = 正方形
        return;
    }

    // 非正方形模式：先取出四条边，再按控制点编号只修改相关的那一/两条边。
    float l = l0, t = t0, r = r0, b = b0;
    switch (hnd)
    {
    case 0: l = l0 + dx; t = t0 + dy; break;   // 左上角：改左和上
    case 1: t = t0 + dy; break;                // 上中：只改上
    case 2: r = r0 + dx; t = t0 + dy; break;   // 右上角：改右和上
    case 3: r = r0 + dx; break;                // 右中：只改右
    case 4: r = r0 + dx; b = b0 + dy; break;   // 右下角：改右和下
    case 5: b = b0 + dy; break;                // 下中：只改下
    case 6: l = l0 + dx; b = b0 + dy; break;   // 左下角：改左和下
    default: l = l0 + dx; break;      // case 7  // 左中：只改左
    }

    // 先把每条边夹进可视范围，避免框拖出屏幕。
    if (l < minX) l = minX;
    if (t < minY) t = minY;
    if (r > maxX) r = maxX;
    if (b > maxY) b = maxY;
    // 再保证"最小尺寸"：如果被夹得反过来（右<左），就把对面那条边推开。
    if (r - l < minS) r = l + minS;
    if (b - t < minS) b = t + minS;
    // 推开之后可能又超出了可视范围，这时改为固定右下边、回退左上边。
    // 这两行是必需的：否则在贴着边界拉伸时框会越界。
    if (r > maxX) { r = maxX; l = r - minS; }
    if (b > maxY) { b = maxY; t = b - minS; }

    // 存回"左、上、宽、高"格式。
    s->rx = l;
    s->ry = t;
    s->rw = r - l;
    s->rh = b - t;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
// PaintCanvas() —— 把整个控件画出来（双缓冲）。
//
// 参数：h 控件窗口；s 状态；hdcTarget 真正要输出到的 DC
//       （来自 BeginPaint，或打印/截图时的其它 DC）。
//
// 【双缓冲是什么、为什么要用】直接在窗口 DC 上画，用户能看到一笔一笔画出来
// 的过程，而且擦背景会导致闪烁。标准做法：先在内存 DC 里把整幅图完整画好，
// 最后用一次 BitBlt 整块拷贝到窗口 DC——用户看到的是"瞬间出现"，不闪。
// 对应的收尾顺序（很重要）：
//   SelectObject 把旧位图选回去 → DeleteObject(bmp) → DeleteDC(mem)
// 【WM_ERASEBKGND 返回 1】也是配套的：告诉系统"背景我自己擦"，避免闪烁。
static void PaintCanvas(HWND h, CanvasState* s, HDC hdcTarget)
{
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right;
    const int hgt = rc.bottom;
    if (w <= 0 || hgt <= 0) return;     // 窗口被缩到 0 时直接返回，避免建 0 尺寸位图

    // ---- 创建内存 DC 和一张同尺寸的位图 ----
    HDC mem = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, w, hgt);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);    // 必须记住旧位图，后面要还回去

    RECT all = { 0, 0, w, hgt };
    // 先铺透明棋盘格底：这样图片有透明区域时能看到"格子"，直观表达透明。
    PaintCheckerBackground(mem, all);

    if (!g_pSrcImage)
    {
        // 没有图片：只画一行灰色的提示文字。
        SetBkMode(mem, TRANSPARENT);            // 文字背景透明，否则会盖住棋盘格
        SetTextColor(mem, RGB(120, 126, 134));
        HGDIOBJ oldFont = SelectObject(mem, g_hUiFont);   // 换成 UI 字体，用完换回
        DrawTextW(mem, L"请先在上方点击「选择图片...」载入图片", -1, &all,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);  // -1 表示字符串以 0 结尾
        SelectObject(mem, oldFont);
    }
    else
    {
        EnsureRect(h, s);       // 保证裁切框已初始化，否则下面画的是垃圾值

        // 裁切框的客户区像素坐标，由图像坐标换算而来。
        float l = s->offX + s->rx * s->scale;
        float t = s->offY + s->ry * s->scale;
        float r = l + s->rw * s->scale;
        float b = t + s->rh * s->scale;

        RectF crop(l, t, r - l, b - t);             // GDI+ 的矩形是 (x, y, 宽, 高)
        RectF img(s->offX, s->offY, (float)g_srcW * s->scale, (float)g_srcH * s->scale);

        {
            // Graphics 是 GDI+ 的绘图对象，构造时绑定到内存 DC。
            // 用花括号包起来是个技巧：让 g 在离开作用域时被析构，释放 GDI+ 资源。
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeNone);          // 关闭抗锯齿，边框更锐利
            g.SetPixelOffsetMode(PixelOffsetModeHalf);      // 像素中心对齐，避免整体偏移半像素

            // 图片（显示缓存）
            if (s->disp)
            {
                // 缓存已经是目标尺寸了，用最近邻放大：速度快，且不会二次模糊。
                g.SetInterpolationMode(InterpolationModeNearestNeighbor);
                g.DrawImage(s->disp, (INT)s->offX, (INT)s->offY, s->dispW, s->dispH);
            }

            // 图片边框
            Pen imgPen(Color(190, 194, 200), 1.0f);         // 浅灰，标出图片实际范围
            g.DrawRectangle(&imgPen, img.X, img.Y, img.Width, img.Height);

            // 裁切框中超出图片的部分：填充色
            // 用 GDI+ 的 Region（区域）做集合运算：
            //   先建 crop 区域，再 Exclude(img) 减掉图片区域，剩下的就是"框内
            //   但不属于图片"的部分；然后只填充这一块。
            // 这就是"裁切框允许超出图片"的视觉表达。
            if (!s->transparentFill)
            {
                Region outside(crop);       // outside = crop
                outside.Exclude(img);       // outside = crop - img
                SolidBrush fillBr(Color(255, GetRValue(s->fill), GetGValue(s->fill), GetBValue(s->fill)));
                g.FillRegion(&fillBr, &outside);
            }

            // 裁切框外遮罩
            // 同理：全窗口区域减去裁切框 = 框外部分，用半透明黑盖住，
            // 突出框内内容（alpha=96，约 38% 不透明度）。
            Region dim(RectF(0, 0, (REAL)w, (REAL)hgt));
            dim.Exclude(crop);              // dim = 整个客户区 - 裁切框
            SolidBrush dimBr(Color(96, 0, 0, 0));
            g.FillRegion(&dimBr, &dim);

            // 三分线
            // 在裁切框内画 2 横 2 竖，把框等分成 3x3，方便用"三分法"构图。
            {
                Pen grid(Color(90, 255, 255, 255), 1.0f);   // 半透明白，别太抢眼
                for (int i = 1; i <= 2; ++i)
                {
                    float gx = crop.X + crop.Width * i / 3.0f;      // 1/3、2/3 处的竖线
                    float gy = crop.Y + crop.Height * i / 3.0f;     // 1/3、2/3 处的横线
                    g.DrawLine(&grid, gx, crop.Y, gx, crop.Y + crop.Height);
                    g.DrawLine(&grid, crop.X, gy, crop.X + crop.Width, gy);
                }
            }

            // 裁切框
            // 画两遍：先白色粗线做底，再深色细线描边。这样框落在浅色和深色
            // 背景上都能看清（"描边"常用技巧）。
            Pen white(Color(255, 255, 255, 255), 3.0f);
            g.DrawRectangle(&white, crop.X, crop.Y, crop.Width, crop.Height);
            Pen dark(Color(40, 40, 40), 1.0f);
            g.DrawRectangle(&dark, crop.X, crop.Y, crop.Width, crop.Height);

            // 控制点
            // 位置由 HandleRects() 统一计算，保证"画出来的"和"点得到的"一致。
            RECT hr[8];
            HandleRects(s, hr);
            SolidBrush hb(Color(255, 255, 255, 255));       // 填充：白
            Pen hp(Color(60, 64, 70), 1.0f);                // 边框：深灰
            for (int i = 0; i < 8; ++i)
            {
                if (s->square && (i % 2) == 1) continue;    // 正方形模式只画 4 个角
                g.FillRectangle(&hb, (REAL)hr[i].left, (REAL)hr[i].top,
                                (REAL)(hr[i].right - hr[i].left), (REAL)(hr[i].bottom - hr[i].top));
                // 边框比填充小 1 像素，是为了让 1px 的线落在方块内部，不被裁掉。
                g.DrawRectangle(&hp, (REAL)hr[i].left, (REAL)hr[i].top,
                                (REAL)(hr[i].right - hr[i].left - 1), (REAL)(hr[i].bottom - hr[i].top - 1));
            }
        }
    }

    // 控件最外圈画一道灰边框，让它和父窗口的背景区分开。
    HBRUSH bBorder = CreateSolidBrush(RGB(205, 208, 212));
    FrameRect(mem, &all, bBorder);
    DeleteObject(bBorder);      // 用完立刻释放，GDI 对象是有限资源

    // ---- 一次性把内存位图拷到真实 DC（这就是双缓冲的"翻页"）----
    BitBlt(hdcTarget, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);  // 把原图选回内存 DC，否则位图无法删除
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
// CanvasProc() —— 窗口过程：本控件所有消息的入口，相当于这个"类"的总调度。
//
// 参数：hWnd 本窗口句柄；message 消息号；wParam/lParam 消息参数
//       （鼠标消息里 lParam 低 16 位是 x、高 16 位是 y，用
//        GET_X_LPARAM/GET_Y_LPARAM 取出——不要自己用 >> 16 拼，
//        那样在负坐标/多显示器下会出错）。
// 返回：默认交给 DefWindowProcW 处理（见函数末尾）。
//
// 【重要】WM_NCCREATE 时状态结构体才被创建，所以本函数第一行取到的 s
// 在第一轮消息里可能是 nullptr，每处用之前都要判断。
static LRESULT CALLBACK CanvasProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    CanvasState* s = State(hWnd);

    switch (message)
    {
    case WM_NCCREATE:
        {
            // 窗口创建过程中的第一条消息：这里 new 状态结构体并挂到窗口上。
            // 用 WM_NCCREATE 而不用 WM_CREATE，是因为它更早，后面的初始化
            // 消息（如 WM_SIZE）就能拿到有效状态了。
            CanvasState* st = new CanvasState();
            memset(st, 0, sizeof(CanvasState));     // 先全部清零，避免读到垃圾值
            st->fill = RGB(255, 255, 255);          // 默认填充色：白
            st->transparentFill = false;
            st->scale = 1.0f;                       // 避免除零
            st->hoverMode = 0;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)st);
            return TRUE;                            // 返回 TRUE 表示"创建继续"
        }

    case WM_NCDESTROY:
        // 窗口销毁前的最后一条消息：在这里释放内存，否则每次开关页面都泄漏。
        if (s)
        {
            if (s->disp) delete s->disp;    // 显示缓存是 new 出来的 Bitmap
            delete s;                       // 状态结构体本身
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);   // 清空槽位，防止野指针
        }
        return 0;

    case WM_ERASEBKGND:
        // 返回 1 = "背景我已经处理了"。配合双缓冲，能消除闪烁。
        return 1;

    case WM_SIZE:
        // 窗口尺寸变了：缩放比、图片位置、显示缓存都得重算。
        if (s)
        {
            RecalcMapping(hWnd, s);     // 先算映射（scale/offX/offY/viewMin...）
            RebuildDisplay(hWnd, s);    // 再按新的 scale 重建显示缓存
            if (g_pSrcImage && s->rectSet)
            {
                // 只做约束，不改动位置意图；保证框仍在可见范围内。
                ClampSize(s);
                ClampPosition(s);
            }
        }
        return 0;

    case WM_PAINT:
        {
            // 标准的 BeginPaint/EndPaint 配对：必须调用，否则系统会不停地
            // 认为这块区域"还是脏的"，导致 CPU 占满。
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            if (s) PaintCanvas(hWnd, s, hdc);   // 真正的绘制在 PaintCanvas
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (s && g_pSrcImage)
        {
            EnsureRect(hWnd, s);        // 确保有初始裁切框，否则下面读到 0
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int mode = HitTest(s, pt);
            if (mode == 0)      // 点空白处：把框移到以该点为中心？
                mode = 0;       // （当前实现是"什么都不做"，保留占位）
            s->dragMode = mode;
            s->dragOrg = pt;    // 记录按下位置，作为整个拖动过程的基准点
            // 把当前框存成"左/上/右/下"四个数，拖动时始终基于它计算，
            // 这样不会有累积误差。
            s->dragRect[0] = s->rx;
            s->dragRect[1] = s->ry;
            s->dragRect[2] = s->rx + s->rw;
            s->dragRect[3] = s->ry + s->rh;
            if (mode)
            {
                // 【必须做】捕获鼠标：之后即使光标移出本窗口，WM_MOUSEMOVE
                // 和 WM_LBUTTONUP 仍然会发给本窗口。
                // 不做这一步的话，用户拖到窗口外松开，我们就收不到抬起消息，
                // 会一直以为还在拖动（"粘住鼠标"的经典 bug）。
                SetCapture(hWnd);
                SetFocus(hWnd);     // 让本控件获得键盘焦点（习惯性动作）
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (s && g_pSrcImage)
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (s->dragMode)
            {
                // 正在拖动：更新裁切框 → 请求重画 → 通知父窗口。
                ApplyDrag(s, pt);
                InvalidateCanvas(hWnd);
                NotifyParent(hWnd);
            }
            else
            {
                // 没在拖动：只根据位置换鼠标指针形状。
                // TrackMouseEvent 是一次性的（只发一次 WM_MOUSELEAVE），
                // 所以用 tracking 标志保证不要重复申请。
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
                if (hv != s->hoverMode)     // 只有位置变了才 SetCursor，省开销
                {
                    s->hoverMode = hv;
                    // 根据悬停位置选择合适的系统指针：
                    //   框内 → 四向移动；左上/右下角 → 西北-东南双向箭头；
                    //   右上/左下角 → 东北-西南；左右边 → 水平；上下边 → 垂直。
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
        // 结束拖动。ReleaseCapture 必须和 SetCapture 配对，否则鼠标会一直被
        // 本窗口"抓住"，其它窗口点不了。
        if (s && s->dragMode)
        {
            s->dragMode = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_MOUSELEAVE:
        // 鼠标离开客户区：重置 tracking，下次进入时重新申请
        // （TrackMouseEvent 是一次性的）。
        if (s) s->tracking = false;
        return 0;

    case WM_CAPTURECHANGED:
        // 鼠标捕获被别人抢走（例如弹出菜单）时，会收到这条消息。
        // 一定要在这里把 dragMode 清零，否则拖动状态会残留在"拖拽中"。
        if (s) s->dragMode = 0;
        return 0;

    case WM_SETCURSOR:
        // 系统在每次鼠标移动时会发这条消息询问"该显示什么指针"。
        // lParam 低 16 位是命中区域代码，只有落在客户区(HTCLIENT)才处理，
        // 否则（边框、标题栏等）交回系统处理。
        // 返回 TRUE 表示"指针我已经设好了，别再改"。
        if (s && LOWORD(lParam) == HTCLIENT)
        {
            // 注意：这里用的 hoverMode 是 WM_MOUSEMOVE 里算好的缓存值，
            // 不重新做命中测试（消息频繁，重算浪费）。
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
    // 自己不处理的消息一律交给系统默认处理。
    // 【新手坑】最后必须走这里，否则窗口连"关闭""重绘"等基本行为都会失效。
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// EnsureClass() —— 确保窗口类已注册，返回 false 表示注册失败。
//
// 【为什么要注册窗口类】Win32 里 CreateWindow 之前必须先 RegisterClassEx：
// 窗口类描述的是"这一类窗口长什么样、由哪个函数处理消息"（注意它和 C++ 的
// class 不是一回事）。同一个进程里重复注册同名类会失败（ERROR_CLASS_ALREADY_EXISTS），
// 那不算错误。
// 【为什么要 static done】RegisterClassEx 只需成功一次，用静态标志做幂等，
// 多次调用 CropCanvas_Create 也不会重复注册。
static bool EnsureClass()
{
    static bool done = false;
    if (done) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);                 // 必须填，系统靠它判断结构体版本
    wc.style = CS_HREDRAW | CS_VREDRAW;     // 宽高变化时整个重画（我们本来就全画）
    wc.lpfnWndProc = CanvasProc;            // 消息处理函数（上面的窗口过程）
    wc.hInstance = g_hInst;                 // 本进程实例句柄
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);   // 默认箭头；实际由 WM_SETCURSOR 覆盖
    wc.hbrBackground = nullptr;             // 不设背景刷：背景我们自己画（配合双缓冲）
    wc.lpszClassName = CROPCANVAS_CLASS;    // L"Pic246CropCanvas"，CreateWindow 时要用同样的名字
    // 返回 0 通常是"类已存在"（本进程重复注册），这种情况视为成功。
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    done = true;
    return true;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
// CropCanvas_Create() —— 创建裁切画布子窗口，供 PageCrop.cpp / PageIco.cpp 调用。
// 参数：parent 父窗口；id 子窗口 ID（父窗口靠它区分消息来源）；
//       squareMode true = 只允许正方形裁切框。
// 返回：新窗口句柄；失败返回 nullptr（调用方必须判空）。
// 说明：样式里带 WS_CHILD | WS_VISIBLE；初始尺寸随便给个 10x10，
//       父窗口随后会用 SetWindowPos/MoveWindow 摆放它，并触发 WM_SIZE 重算。
HWND CropCanvas_Create(HWND parent, int id, bool squareMode)
{
    if (!EnsureClass()) return nullptr;

    HWND h = CreateWindowExW(0, CROPCANVAS_CLASS, L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (h)
    {
        // 窗口创建时 WM_NCCREATE 已经把状态结构体挂好了，这里补上"是否正方形"。
        CanvasState* s = State(h);
        if (s) s->square = squareMode;
    }
    return h;
}

// CropCanvas_ResetToImage() —— 把裁切框恢复成"正好覆盖整张图片"。
// 参数：h 本控件窗口句柄。
// 无返回值；没有图片或窗口无效时直接返回。
// 结束后会重画并通知父窗口（父窗口需要同步更新显示的数字）。
void CropCanvas_ResetToImage(HWND h)
{
    CanvasState* s = State(h);
    if (!s || !g_pSrcImage) return;
    s->rx = 0.0f;
    s->ry = 0.0f;
    s->rw = (float)g_srcW;
    s->rh = (float)g_srcH;
    s->rectSet = true;      // 标记已初始化，EnsureRect 就不会再覆盖
    ClampSize(s);
    ClampPosition(s);
    InvalidateCanvas(h);
    NotifyParent(h);
}

// CropCanvas_ResetToSquareCenter() —— 把裁切框恢复成"居中的最大正方形"。
// 参数：h 本控件窗口句柄。
// 实现要点：边长取图片宽高中较小的一个（否则放不下），
//           再用 (原尺寸 - 边长) / 2 算出居中位置。
void CropCanvas_ResetToSquareCenter(HWND h)
{
    CanvasState* s = State(h);
    if (!s || !g_pSrcImage) return;

    // 取短边作为正方形边长，保证整个正方形都在图片范围内。
    float side = ((float)g_srcW < (float)g_srcH) ? (float)g_srcW : (float)g_srcH;
    s->rw = side;
    s->rh = side;
    s->rx = ((float)g_srcW - side) * 0.5f;      // 居中：左右各留一半的余量
    s->ry = ((float)g_srcH - side) * 0.5f;
    s->rectSet = true;
    ClampSize(s);
    ClampPosition(s);
    InvalidateCanvas(h);
    NotifyParent(h);
}

// CropCanvas_GetRect() —— 取出当前裁切框，单位是"图像坐标"（不是屏幕像素）。
// 参数：x/y/w/hh 输出指针，允许传 nullptr 表示"我不需要这个值"
//       （所以下面每个都先判空再赋值）。
// 【为什么要判空】这样调用方可以只关心其中一两个值；同时避免空指针崩溃。
// 注意参数名用 hh 而不是 h，是为了不和窗口句柄 h 撞名。
void CropCanvas_GetRect(HWND h, float* x, float* y, float* w, float* hh)
{
    CanvasState* s = State(h);
    if (!s) return;
    EnsureRect(h, s);       // 万一还没初始化，先给出"整张图"的默认值
    if (x) *x = s->rx;
    if (y) *y = s->ry;
    if (w) *w = s->rw;
    if (hh) *hh = s->rh;
}

// CropCanvas_SetFill() —— 设置"裁切框超出图片部分"的显示方式。
// 参数：h 本控件窗口句柄；c 填充色；transparent = true 时改用棋盘格
//       （不填充，露出底下的透明格子）。
// 只改状态 + 请求重画，不需要通知父窗口（不影响裁切结果）。
void CropCanvas_SetFill(HWND h, COLORREF c, bool transparent)
{
    CanvasState* s = State(h);
    if (!s) return;
    s->fill = c;
    s->transparentFill = transparent;
    InvalidateCanvas(h);
}

// CropCanvas_RefreshImage() —— 换图之后调用：清缓存、重算映射、
// 重建显示位图，并把裁切框重置为默认状态。
// 参数：h 本控件窗口句柄。
// 说明：s->rectSet = false 是关键——它让裁切框"回到未初始化"，
//       随后的 ResetTo* 才会重新给出合适的默认框，而不是沿用上一张图的位置。
void CropCanvas_RefreshImage(HWND h)
{
    CanvasState* s = State(h);
    if (!s) return;

    s->rectSet = false;             // 先作废旧裁切框
    RecalcMapping(h, s);            // 新图尺寸可能不同，重新算缩放和偏移
    RebuildDisplay(h, s);           // 重建显示缓存

    if (g_pSrcImage)
    {
        // 正方形模式给"居中最大正方形"，否则给"整张图"。
        if (s->square)
            CropCanvas_ResetToSquareCenter(h);
        else
            CropCanvas_ResetToImage(h);
    }
    else
    {
        // 没有图片了（例如被清空）：只需要把界面重画成提示文字。
        InvalidateCanvas(h);
    }
}
