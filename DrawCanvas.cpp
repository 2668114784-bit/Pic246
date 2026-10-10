// ============================================================================
//  DrawCanvas.cpp —— 涂鸦画布
//    · 画笔绘制：矢量笔迹（图像坐标保存），可撤销 / 清空
//    · 吸色：点击画面取色，取完自动回到画笔模式
//    · 输出时按原图分辨率重新合成一张位图（见 DrawCanvas_RenderFull）
// ============================================================================
//
// ===== 文件说明（给第一次接触 Win32 / GDI+ 的读者） =========================
//
// 【本文件负责什么】
//   实现 Pic246（Win32 + GDI+ 图片小工具）里的「涂鸦画布控件」。
//   它是一个自定义窗口类，类名是字符串常量 DRAWCANVAS_CLASS（= L"Pic246DrawCanvas"，
//   定义在 App.h）。把这个类 CreateWindow 到某个父窗口上，用户就能：
//     1) 在图片上按住鼠标左键拖动来画线（自由涂鸦）；
//     2) 进入「吸色」模式后点击画面，把画面上的颜色取来当画笔颜色；
//     3) 逐笔撤销、一键清空；
//     4) 输出时按【原图分辨率】重新合成一张「原图 + 笔迹」的位图。
//
// 【和哪些文件配合】
//   · App.h
//       —— 公共声明。本文件用到的全局变量（g_pSrcImage / g_srcW / g_srcH /
//          g_hInst）、自定义消息（WM_APP_RECT_CHANGED / WM_APP_PICKED_COLOR）、
//          工具函数（PaintCheckerBackground），以及 DRAWCANVAS_CLASS 和
//          DrawCanvas_* 系列函数原型，全都声明在这里。
//   · PageDraw.cpp
//       —— 「涂鸦」功能页。它负责创建画布、摆放「画笔粗细滑块 / 颜色按钮 /
//          吸色 / 撤销 / 清空 / 输出」这些控件，并调用本文件对外暴露的
//          DrawCanvas_* 函数；同时接收本文件发给它的自定义消息。
//   · Project001.cpp
//       —— 定义 g_pSrcImage 等全局状态；源图更换后会广播 WM_APP_IMAGE_CHANGED。
//
// 【大致数据流 / 交互流程】
//   ① 用户选择图片
//        Project001.cpp 设置 g_pSrcImage / g_srcW / g_srcH 并广播 WM_APP_IMAGE_CHANGED
//        → PageDraw.cpp 收到后调用 DrawCanvas_RefreshImage(画布)
//        → 本文件 RecalcMapping() 算出缩放比 scale 与居中偏移 offX/offY，
//          再由 RebuildDisplay() 重建「显示用缩略图 srcDisp」和「与画布等大的透明笔迹层 overlay」
//   ② 用户按住左键拖动
//        WM_LBUTTONDOWN：把鼠标坐标换算成【图像坐标】，开一笔新笔迹，并 SetCapture
//        WM_MOUSEMOVE  ：继续往笔迹里追加图像坐标点，只增量重画最后一小段
//        WM_LBUTTONUP  ：收笔，把这条 Stroke 存进 strokes 数组（从此可撤销），
//                        并 PostMessage(WM_APP_RECT_CHANGED) 让父页刷新按钮状态
//   ③ 用户点「保存涂鸦并输出」
//        PageDraw.cpp 调 DrawCanvas_RenderFull() 拿到【原图分辨率】的笔迹层，
//        再与 g_pSrcImage 叠加，存盘（文件名后缀 _draw）
//
// 【先记住这 3 个核心概念（后面所有代码都围绕它们）】
//   1) 笔迹是【矢量】保存的：只存一串点 + 颜色 + 线宽（struct Stroke），而不是像素。
//      于是「撤销」= 把数组最后一个元素弹掉；「输出」= 按原始分辨率重新画一遍，
//      放大缩小都不会糊，也不会失真。
//   2) 显示是【缩放 + 居中】的：屏幕上看到的图通常比原图小，所以鼠标坐标必须用
//      ToImage() 换算回图像坐标；反过来绘制时，用 Gdiplus::Graphics 的变换矩阵
//      把图像坐标再映射回画布坐标。
//   3) 「增量绘制」与「全量重绘」是两回事：拖动时每来一个点就整层擦掉重画会很卡
//      （overlay 和整个画布一样大），所以拖动过程中只补画最新那一小段
//      （DrawLastSegment）；而撤销 / 清空 / 收笔这种会改变整体内容的操作，
//      必须整层重来（RenderOverlay(st, false)）。
// ============================================================================
#include "App.h"
#include <windowsx.h>

using namespace Gdiplus;

// 匿名的命名空间（anonymous namespace）：
// 里面定义的类型和函数只在【本 .cpp 文件内】可见，相当于「文件私有」。
// 这样可以避免和别的 .cpp 里同名的东西发生重复定义（链接冲突）。
namespace
{
    // ------------------------------------------------------------------------
    // struct Stroke —— 一笔（一条笔迹）
    //   本文件的「数据核心」：一笔不是一个像素图，而是一串点 + 颜色 + 线宽。
    //   好处：撤销只要丢掉一个元素；输出时可以按任意分辨率重画，不会失真。
    // ------------------------------------------------------------------------
    struct Stroke
    {
        std::vector<PointF> pts;
        // 这一笔经过的所有点，坐标是【图像坐标】（原图像素坐标系，原点在左上角）。
        // PointF 是 GDI+ 的浮点坐标点（float X, Y）。用浮点是为了缩放后不丢精度。
        // std::vector 自己管理内存：push_back 添加、clear 清空、析构时自动释放；
        // 我们完全不需要 new/delete，也不会内存泄漏。
        Color color;
        // 这一笔的颜色（GDI+ 的 Color 自带 Alpha 通道，本程序里恒为 255 不透明）。
        float width;          // 图像坐标下的线宽
        // 注意：这里是【图像坐标】下的线宽，不是屏幕像素！
        // 屏幕上看起来多粗，取决于 scale —— 见 WM_LBUTTONDOWN 里的换算。
        // 每个 Stroke 各自存颜色和线宽，所以「改画笔设置」只影响以后画的笔，
        // 不会把已经画好的笔也改掉。
    };

    // ------------------------------------------------------------------------
    // struct DoodleState —— 画布的全部内部状态
    //   每个画布窗口一份，挂在窗口的 GWLP_USERDATA 槽位上（见 St() 与 WM_CREATE）。
    //   用窗口私有状态而不是全局变量，可以支持同时存在多个画布窗口。
    // ------------------------------------------------------------------------
    struct DoodleState
    {
        std::vector<Stroke> strokes;
        // 已经画完、可以撤销的笔迹。撤销 = pop_back()。
        Stroke  cur;
        // 正在画的那一笔（鼠标还没松开）。松手后它会被 push_back 进 strokes。
        bool    drawing = false;
        // 是否正在拖动绘制（左键按下到松开之间为 true）。
        Color   brush = Color(255, 235, 40, 45);
        // 当前画笔颜色。Color(A, R, G, B)，这里是 A=255、R=235、G=40、B=45 的红色。
        float   brushW = 5.0f;        // 显示像素
        // 画笔粗细，单位是【屏幕显示像素】（来自用户拖的滑块）。
        // 真正存进 Stroke::width 之前，要除以 scale 换算成图像坐标线宽。
        bool    pick = false;
        // 是否处于吸色模式：true 时左键点击是「取色」，不是「画线」。

        Bitmap* srcDisp = nullptr;    // 缩放后的原图（显示用）
        // 源图预先缩放到当前显示尺寸的结果。
        // 【为什么要这份缓存】否则每次 WM_PAINT 都要把整张原图重采样一遍，
        // 大图会明显卡顿。初始为 nullptr，表示「还没建好」。
        Bitmap* overlay = nullptr;    // 显示尺寸的笔迹层
        // 与画布客户区等大的 32 位透明位图：笔迹先画在它上面，最后整张叠到窗口上。
        // 把笔迹和原图分成两层，重画笔迹时就不必碰原图，效率更高。
        Bitmap* sample = nullptr;     // 吸色采样（最长边限制）
        // 吸色专用的采样位图，惰性创建：第一次真正吸色时才建。
        // 【为什么不直接用原图 GetPixel】原图可能是 8000×6000 的超大图、
        // 甚至是不常见的像素格式，GDI+ 的 Bitmap::GetPixel 在这种图上可能失败或极慢。
        // 缩到最长边 <= 2048 并统一成 32bppARGB 后，取色又快又稳。
        float   sampleScale = 1.0f;
        // 采样位图相对原图的缩放比 = 采样图宽 / 原图宽。
        // 换算关系：图像坐标 × sampleScale = 采样图坐标。

        float   offX = 0, offY = 0, scale = 1.0f;
        // 显示映射参数（由 RecalcMapping() 计算）：
        //   scale      —— 原图 → 屏幕的等比缩放倍数（取宽、高两个方向比例中较小的那个）
        //   offX/offY  —— 缩放后图片左上角在画布客户区里的像素偏移（用于居中）
        int     cw = 0, ch = 0;
        // 画布客户区的宽高（像素），在 WM_SIZE / RecalcMapping 时更新。
    };

    // 从窗口的「用户数据」槽位里取出 DoodleState*。
    // WM_CREATE 时 new 一个放进去，WM_DESTROY 时再取出来 delete，
    // 于是每个窗口独立持有一份状态。
    DoodleState* St(HWND h) { return (DoodleState*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    const int PAD = 12;
    // 魔法数字：图片四周留 12 像素留白，避免图片紧贴画布边缘显得拥挤，
    // 同时让用户能看清图片的边界（配合 WM_PAINT 里画的那圈灰边框）。
}

// ---------------------------------------------------------------------------
//  笔迹绘制
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  DrawWholeStroke —— 把一整笔（Stroke）画到 Graphics 上
//  参数：
//    g —— GDI+ 绘图对象。注意：调用它时 g 上通常已经设好了
//         「图像坐标 → 画布坐标」的变换矩阵，所以这里直接用 pts 里的
//         图像坐标给值即可，不用自己做缩放。
//    s —— 要画的那一笔。
//  返回值：无。
//  为什么单独抽成一个函数：全量重绘（RenderOverlay）和最终输出
//    （DrawCanvas_RenderFull）都要画「整笔」，逻辑共用一份，
//    才能保证「屏幕上看到的形状」和「输出的形状」完全一致。
// ---------------------------------------------------------------------------
static void DrawWholeStroke(Graphics& g, const Stroke& s)
{
    if (s.pts.empty()) return;
    // 空笔迹直接返回。下面用了 s.pts[0] 这种下标访问，为空会越界崩溃。

    if (s.pts.size() == 1)
    {
        // 只有 1 个点的情况：用户点了鼠标但没拖动。
        // DrawLines 画不出一条线（线段至少要 2 个点），所以特判成
        // 「画一个直径 = 线宽的实心圆点」，让单击也能留下痕迹。
        SolidBrush b(s.color);
        g.FillEllipse(&b, s.pts[0].X - s.width * 0.5f, s.pts[0].Y - s.width * 0.5f,
                      s.width, s.width);
        // FillEllipse 的参数是「外接矩形的左上角 + 宽 + 高」，
        // 所以左上角要各减去半个线宽，圆心才会正好落在那个点上。
        return;
    }

    Pen p(s.color, s.width);
    p.SetStartCap(LineCapRound);   // 起点画成圆头，否则是平头，笔画衔接处会出现缺口
    p.SetEndCap(LineCapRound);     // 终点同理
    p.SetLineJoin(LineJoinRound);  // 拐角也做圆角，转折处不会出现尖角或缺口
    g.DrawLines(&p, s.pts.data(), (INT)s.pts.size());
    // DrawLines 把数组里相邻的点依次连成折线，一次调用画完整笔。
    // 比「每两个点画一条线」快很多，也不会在连接处反复叠加导致颜色变深。
    // 强转 (INT) 是因为 GDI+ 的计数参数类型是 INT。
}

// ---------------------------------------------------------------------------
//  DrawLastSegment —— 只画当前笔迹的【最后一段】（增量绘制用）
//  拖动鼠标时，每来一个点就把整层擦掉重画会卡，所以这里只补画
//  「倒数第二个点 → 最后一个点」这一小段：前面已经画好的像素原样保留，
//  只在新的一段上落笔，性能好得多。
//  参数：g、s 同上。调用时 s.pts 通常至少有 2 个点。
// ---------------------------------------------------------------------------
static void DrawLastSegment(Graphics& g, const Stroke& s)
{
    if (s.pts.size() < 2) { DrawWholeStroke(g, s); return; }
    // 只有 1 个点时没有「上一段」可补，退化成画整笔（结果是一个圆点）。

    PointF seg[2] = { s.pts[s.pts.size() - 2], s.pts[s.pts.size() - 1] };
    // 取出最后两个点，组成只有一段的折线。
    Pen p(s.color, s.width);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    // 这里没有设 LineJoin：只有一段，不存在拐角，设了也没用。
    // 【小知识】圆头和上一段必然是重叠的（重叠长度约等于半个线宽），
    // 但因为笔颜色 Alpha = 255（完全不透明），SourceOver 覆盖上去看不出接缝；
    // 如果哪天要用半透明笔，这里就会显出「一段深一段浅」的痕迹，届时要改成整笔重画。
    g.DrawLines(&p, seg, 2);
}

// 把图像坐标映射到画布坐标后重绘笔迹；incrementalOnly=true 时只补画当前笔的最后一小段
// ---------------------------------------------------------------------------
//  RenderOverlay —— 重建 overlay（笔迹层）
//  参数：
//    st              —— 画布状态。st->overlay 为空（还没建好）时直接返回。
//    incrementalOnly —— false：先把整层擦成全透明，再把所有笔迹重画一遍（全量重绘）；
//                       true ：不动已有像素，只补画当前笔的最后一小段（增量绘制）。
//  为什么需要两套：
//    overlay 是和整个画布一样大的位图，全量重绘的代价随笔画数量增长；
//    拖动鼠标时如果每个点都全量重绘，会明显卡顿。而撤销 / 清空 / 收笔
//    会影响「已经画好的内容」，就必须整层重来，否则被删掉的那笔还留在位图上。
//  关键概念：
//    GDI+ 的 Graphics 一旦 SetTransform 了变换矩阵，之后所有绘制坐标
//    都会被这个矩阵自动映射。这里矩阵 = 缩放 + 平移，于是笔迹里存的
//    图像坐标可以「原样」画到画布上的正确位置。
// ---------------------------------------------------------------------------
static void RenderOverlay(DoodleState* st, bool incrementalOnly)
{
    if (!st->overlay) return;

    Graphics g(st->overlay);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // 抗锯齿：线条边缘做平滑过渡，否则斜线会有明显的台阶状锯齿。
    g.SetCompositingMode(CompositingModeSourceOver);
    // SourceOver = 普通的「叠加绘制」：新画的内容按自身 Alpha 混合到已有像素上。
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    // 像素偏移模式。Half 表示「像素中心位于整数坐标 + 0.5 处」，
    // 这是 GDI+ 画线和位图的默认约定，配合抗锯齿能得到最标准的线条粗细。

    Matrix m(st->scale, 0, 0, st->scale, st->offX, st->offY);
    // 3×2 仿射矩阵：  x' = x * scale + offX ，  y' = y * scale + offY
    // 也就是「先按 scale 缩放，再平移 offX / offY」。
    // 之后所有绘制都用图像坐标给值，GDI+ 会自动换算到画布坐标。
    g.SetTransform(&m);

    if (incrementalOnly)
    {
        if (st->drawing) DrawLastSegment(g, st->cur);
        return;
        // 增量分支到此结束：完全不碰已有像素，只补一小段。
    }

    g.Clear(Color(0, 0, 0, 0));
    // 全量分支：先把整层清成完全透明（Color(0,0,0,0) 的 Alpha 是 0），
    // 相当于「橡皮擦」。不擦的话，被撤销的笔画会一直残留在位图上。
    for (size_t i = 0; i < st->strokes.size(); ++i)
        DrawWholeStroke(g, st->strokes[i]);
    if (st->drawing) DrawWholeStroke(g, st->cur);
    // 画「已经收笔的笔迹」+「正在画的那一笔」（如果鼠标还按着）。
    // 每笔的颜色 / 线宽都各自存在 Stroke 里，所以这里什么都不用管，直接画即可。
}

// ---------------------------------------------------------------------------
//  显示缓存
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  RecalcMapping —— 计算「图像坐标 ↔ 画布坐标」的映射参数
//  参数：
//    st     —— 画布状态。
//    cw, ch —— 画布客户区的宽、高（像素）。
//  做的事：算出 st->scale（等比缩放比）和 st->offX / st->offY（居中偏移）。
//  为什么每次都要重算：窗口大小会变、图片也可能换，这两个值必须跟着更新 ——
//    ToImage()、RenderOverlay()、WM_PAINT 里的贴图全都依赖它们。
// ---------------------------------------------------------------------------
static void RecalcMapping(DoodleState* st, int cw, int ch)
{
    st->cw = cw;
    st->ch = ch;
    st->offX = 0;
    st->offY = 0;
    st->scale = 1.0f;
    // 先恢复到「不缩放、不偏移」的安全默认值。
    // 这样后面任何一步提前 return，映射也不会是脏数据 ——
    // 尤其 scale 绝不能为 0，否则 ToImage() 和绘制时都会除以 0。

    if (!g_pSrcImage || g_srcW == 0 || g_srcH == 0) return;
    // 还没载入图片（或图片宽高异常）时没什么可映射的。

    const float aw = (float)(cw - PAD * 2);
    const float ah = (float)(ch - PAD * 2);
    // aw / ah = 「可用区域」的宽和高：扣掉左右（上下）各 12 像素的内边距。
    if (aw <= 4 || ah <= 4) return;
    // 窗口太小（比留白还小）时放弃缩放，沿用前面 1.0 的默认值。

    const float sx = aw / (float)g_srcW;
    const float sy = ah / (float)g_srcH;
    st->scale = (sx < sy) ? sx : sy;
    // 【关键公式】等比缩放比：取宽、高两个方向比例的【较小值】。
    // 这样图片能「完整装进可用区域」而不会被裁掉（类似 CSS 的 contain）；
    // 用较小的那个 = 只会贴满一个方向，另一个方向留白。
    st->offX = (cw - (float)g_srcW * st->scale) * 0.5f;
    st->offY = (ch - (float)g_srcH * st->scale) * 0.5f;
    // 【关键公式】居中偏移：缩放后的图片比画布小多少，就把差值的一半放到左边 / 上边。
}

// ---------------------------------------------------------------------------
//  RebuildDisplay —— 重建显示用的位图缓存
//  参数：st —— 画布状态。
//  什么时候调用：源图更换了、或者窗口大小变了（因为缩放比变了）。
//  重建三个缓存：
//    srcDisp —— 缩放后的原图，用于显示
//    overlay —— 与画布等大的透明笔迹层
//    sample  —— 吸色采样图（会被一并丢掉落，下次吸色时重建）
//  注意：st->strokes（笔迹数据）不受影响 —— 它们存的是图像坐标，
//        缩放变化后重新画一遍就能对上新的位置，不会因为窗口缩放而丢失。
// ---------------------------------------------------------------------------
static void RebuildDisplay(DoodleState* st)
{
    if (st->srcDisp) { delete st->srcDisp; st->srcDisp = nullptr; }
    if (st->overlay) { delete st->overlay; st->overlay = nullptr; }
    if (st->sample) { delete st->sample; st->sample = nullptr; }
    // 先删掉旧的三个缓存位图。Gdiplus::Bitmap 用 delete 释放（不是 DeleteObject）。
    // 删完立刻置 nullptr，防止后面某一步提前 return 时留下悬空指针（野指针）。

    if (!g_pSrcImage || st->cw <= 0 || st->ch <= 0) return;

    const int dw = (int)((float)g_srcW * st->scale + 0.5f);
    const int dh = (int)((float)g_srcH * st->scale + 0.5f);
    // 缩放后图片的显示尺寸。
    // 【坑】末尾的 +0.5f 是「四舍五入」：C 语言 (int) 强转是直接截断（向下取整），
    // 不加 0.5 会让结果整体偏小 1 像素。

    if (dw > 0 && dh > 0)
    {
        st->srcDisp = new Bitmap(dw, dh, PixelFormat32bppPARGB);
        // 新建一张空位图当画布。
        // PixelFormat32bppPARGB = 32 位带 Alpha，且是「预乘 Alpha」（P = Premultiplied）。
        // 预乘格式处理更快，是「绘制目标」位图的常用选择。
        if (st->srcDisp->GetLastStatus() == Ok)
        {
            // 【重要】GDI+ 构造函数不会抛异常，分配失败要靠 GetLastStatus() 检查。
            // 所以每次 new Bitmap 之后都必须判一下状态，否则后面会在坏对象上操作。
            Graphics g(st->srcDisp);
            // 用「这张位图」构造 Graphics：之后的绘制就落在这张位图上（离屏绘制）。
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            // 插值模式 = 缩放时「新像素的颜色怎么由周围像素算出来」：
            //   · InterpolationModeNearestNeighbor（最近邻）最快，但缩小照片会有明显锯齿；
            //   · InterpolationModeHighQualityBicubic（高质量双三次）最慢但最平滑，适合图片预览。
            // 【重要】这里是把原图「缩小」，每个新像素都是多个原始像素混合的结果，
            // 所以【这份位图不能拿来当取色源】—— 取色要用高保真的采样图
            // （见 EnsureSample），否则吸到的颜色会和原图有可见偏差。
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            // 高质量像素偏移，配合 HighQualityBicubic 让缩放结果更准确。
            g.SetCompositingMode(CompositingModeSourceCopy);
            // SourceCopy = 直接覆盖，不做 Alpha 混合。
            // 目标是刚 new 出来、像素尚未初始化的位图，用 Copy 才能把整片区域写实，
            // 避免和未初始化的垃圾像素混出杂色（用 SourceOver 会出现彩色噪点）。
            g.DrawImage(g_pSrcImage, 0, 0, dw, dh);
            // 把源图整体缩放绘制到 (0,0)-(dw,dh) 这块区域。
        }
        else
        {
            delete st->srcDisp;
            st->srcDisp = nullptr;
            // 分配失败：释放并置空。之后 WM_PAINT 会走「显示提示文字」的分支。
        }
    }

    st->overlay = new Bitmap(st->cw, st->ch, PixelFormat32bppARGB);
    // 笔迹层：和画布客户区【一样大】，并且必须支持透明。
    // PixelFormat32bppARGB = 32 位带 Alpha，但【不是】预乘格式。
    // 为什么这里不预乘：笔迹层要先 g.Clear(Color(0,0,0,0)) 清成全透明再往上叠加笔迹，
    // 非预乘格式在「透明底 + SourceOver 叠加」时行为更直观。
    if (st->overlay->GetLastStatus() != Ok)
    {
        delete st->overlay;
        st->overlay = nullptr;
        return;
    }
    RenderOverlay(st, false);
    // 位图建好后，把已有的笔迹全部重画上去（全量重绘）。
}

// 吸色用采样位图（最长边 <= 2048，避免超大图占内存）
// ---------------------------------------------------------------------------
//  EnsureSample —— 惰性创建吸色用的采样位图
//  参数：st —— 画布状态。
//  「惰性」= 第一次真正要吸色的时候才创建（不是一载入图片就建）：
//    用户可能根本不吸色，提前建一张大位图纯属浪费内存。
//  为什么单独做一张采样图，而不是直接对原图 GetPixel：
//    · 原图可能是 12000×8000，或者是不常见的像素格式，GDI+ 的
//      Bitmap::GetPixel 在这种图上可能失败、也可能极慢；
//    · 缩到「最长边 <= 2048」并统一成 32bppARGB 后，取色又快又稳。
//  代价：吸到的是缩放插值之后的颜色，不保证和原图某个像素 100% 相同；
//        但对于「拿画面颜色当画笔色」这个用途，精度完全够用。
//  返回值：无（结果直接写进 st->sample 和 st->sampleScale）。
// ---------------------------------------------------------------------------
static void EnsureSample(DoodleState* st)
{
    if (st->sample || !g_pSrcImage) return;
    // 已经建过（或不该建）就直接返回，保证同一时刻只有一张采样图。

    const int maxDim = 2048;
    // 魔法数字：采样图最长边的上限。2048 足够配色使用，内存也可控
    // （2048 × 2048 × 4 字节 ≈ 16 MB）。
    int sw = (int)g_srcW, sh = (int)g_srcH;
    if (sw > maxDim || sh > maxDim)
    {
        const double k = (double)maxDim / (double)((sw > sh) ? sw : sh);
        // 缩放系数 = 2048 / 较长的那条边。
        // 故意用 double 计算：整数除法会把小数部分丢掉，比例就不准了。
        sw = (int)(sw * k + 0.5);
        sh = (int)(sh * k + 0.5);
        // 同样用 +0.5 实现四舍五入。
    }
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    // 兜底：极端的瘦长图片（比如 1×10000）算下来可能变成 0，
    // 而宽或高为 0 的 Bitmap 是非法的，所以强行提到 1。

    st->sample = new Bitmap(sw, sh, PixelFormat32bppARGB);
    if (st->sample->GetLastStatus() != Ok)
    {
        delete st->sample;
        st->sample = nullptr;
        return;
        // 建失败（内存不足）就放弃；PickColor() 里会检查 st->sample 是否为空。
    }
    {
        Graphics g(st->sample);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.DrawImage(g_pSrcImage, 0, 0, sw, sh);
    }
    // 这里特意用一个花括号把 Graphics 包起来，让它尽快析构。
    // 【坑】GDI+ 在位图还被 Graphics 占用时，不能对该位图锁定位图数据；
    // 把作用域缩到最小，后面的 GetPixel 才不会撞上这个限制。
    st->sampleScale = (float)sw / (float)g_srcW;
    // 记录「采样图 / 原图」的比例，PickColor() 里用它把图像坐标换成采样图坐标。
}

// ---------------------------------------------------------------------------
//  鼠标 -> 图像坐标
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  ToImage —— 把「画布客户区坐标」换算成「图像坐标」
//  参数：
//    st     —— 画布状态（用到 scale / offX / offY）。
//    x, y   —— 鼠标在客户区里的像素坐标（来自消息的 lParam）。
//    ix, iy —— 输出参数：换算出来的图像坐标（原图像素坐标系，浮点）。
//  返回值：true 表示换算成功；false 表示 scale 非法（会除零），此时输出值不可用。
//  【公式推导】屏幕上是这样把图片摆上去的：
//        screenX = imgX * scale + offX
//    反过来解出 imgX：
//        imgX = (screenX - offX) / scale
//  ⚠ 注意：本函数【只做换算，不判断点是否落在图片范围内】。
//    调用者（PickColor、WM_LBUTTONDOWN）要自己判断，
//    否则在图片外的留白区域点击会得到负数或超界的图像坐标。
// ---------------------------------------------------------------------------
static bool ToImage(DoodleState* st, int x, int y, float* ix, float* iy)
{
    if (st->scale <= 0) return false;
    // 防除零。scale 理论上恒大于 0，但重算映射的过程中可能短暂为 0，必须挡住。
    *ix = ((float)x - st->offX) / st->scale;
    *iy = ((float)y - st->offY) / st->scale;
    return true;
}

// ---------------------------------------------------------------------------
//  PickColor —— 吸色：取画面上某点的颜色，设为画笔色并通知父窗口
//  参数：hWnd —— 画布窗口；st —— 画布状态；x, y —— 鼠标客户区坐标。
//  流程：
//    1) 换算成图像坐标，并【过滤掉点在图片外的点击】（画布留白处不算数）；
//    2) 惰性准备采样位图；
//    3) 把图像坐标换算成采样图坐标，并做边界钳制（clamp）；
//    4) GetPixel 取色 → 更新画笔颜色 → 退出吸色模式
//       → 用 SendMessage 把 RGB 值（放在 wParam）发给父窗口。
//  为什么用 SendMessage 而不是 PostMessage：
//    吸色是「立刻要结果」的交互，同步发送能让父窗口马上更新颜色块和状态栏；
//    而且发送时机就在本次鼠标消息的处理过程内，先后顺序非常清晰。
// ---------------------------------------------------------------------------
static void PickColor(HWND hWnd, DoodleState* st, int x, int y)
{
    float ix, iy;
    if (!ToImage(st, x, y, &ix, &iy)) return;
    if (ix < 0 || iy < 0 || ix >= (float)g_srcW || iy >= (float)g_srcH) return;
    // 【坑】点在图片外的留白区域：直接忽略，不能取色。
    // 注意右侧 / 下方用的是 >=：有效像素坐标范围是 0 .. W-1，等于 W 已经越界。

    EnsureSample(st);
    if (!st->sample) return;
    // 采样图建失败（内存不足）就放弃这一次吸色。

    int sx = (int)(ix * st->sampleScale);
    int sy = (int)(iy * st->sampleScale);
    // 图像坐标 → 采样图坐标：乘上缩放比即可。
    // 这里直接截断取整（不像别处那样 +0.5 四舍五入）完全可以，
    // 因为采样图本身就是缩小的，取色允许的误差远大于 1 个像素。
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int)st->sample->GetWidth())  sx = (int)st->sample->GetWidth() - 1;
    if (sy >= (int)st->sample->GetHeight()) sy = (int)st->sample->GetHeight() - 1;
    // 【坑】边界钳制（clamp）：缩放误差可能让坐标刚好等于宽 / 高，
    // 而 GetPixel 越界会失败或返回垃圾值，所以强行夹到
    // [0, 宽-1] 和 [0, 高-1] 这个合法区间里。

    Color c;
    if (st->sample->GetPixel(sx, sy, &c) != Ok) return;
    // GetPixel 读取一个像素的颜色。它返回 Status，必须判断是否 Ok。

    const COLORREF cr = RGB(c.GetR(), c.GetG(), c.GetB());
    // 转成 Win32 的 COLORREF（0x00BBGGRR），因为父窗口的按钮 / 色块用的是 COLORREF。
    st->brush = Color(255, c.GetR(), c.GetG(), c.GetB());
    // 画笔换成吸到的颜色。Alpha 强制写 255（不透明）。
    st->pick = false;
    // 取完色自动退出吸色模式（父窗口也会同步把「吸色」按钮弹起来）。
    SendMessageW(GetParent(hWnd), WM_APP_PICKED_COLOR, (WPARAM)cr, 0);
    // 通知父页（PageDraw.cpp）：颜色值放在 wParam 里。
    // WM_APP_PICKED_COLOR = WM_APP + 4，定义在 App.h。
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  窗口过程 Proc —— 处理画布窗口的所有消息
//  这是 Win32 程序的核心：系统把发生在窗口上的事件（鼠标、键盘、重绘……）
//  以「消息」的形式投递到这里，我们按 message 的值分类处理。
//  返回值：已经处理的消息返回 0；没处理的消息【必须】交给 DefWindowProcW
//          做默认处理，否则窗口的拖动、激活、销毁等基础行为都会不正常。
//  注意：st 可能为 nullptr（WM_CREATE 还没执行完，或窗口正在销毁），
//        所以每个分支都要先判断 st 再去解引用它。
// ---------------------------------------------------------------------------
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    DoodleState* st = St(hWnd);

    switch (message)
    {
    case WM_CREATE:
        // 窗口刚被创建时触发（在 CreateWindowExW 内部同步调用到）。
        // 这时还没有用户数据，所以上面取到的 st 此刻一定是 nullptr，
        // 这里必须自己 new 一个。
        {
            DoodleState* s = new DoodleState();
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)s);
            // 把状态指针存进窗口的「用户数据槽」。之后 St() 就能随时取回它。
            // 用窗口自己的槽位而不是全局变量，可以支持多个画布窗口各自独立。
        }
        return 0;

    case WM_DESTROY:
        // 窗口销毁：在这里释放所有堆内存，否则每开关一次页面就会泄漏一份。
        if (st)
        {
            if (st->srcDisp) delete st->srcDisp;
            if (st->overlay) delete st->overlay;
            if (st->sample) delete st->sample;
            // 三张 GDI+ 位图。注意：Gdiplus::Bitmap 用 delete 释放（不是 DeleteObject）。
            delete st;
            // 连 DoodleState 本体一起删掉；它内部的 st->strokes 是个 std::vector，
            // 会随之自动析构，里面的点数据一并回收，不需要手动管。
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            // 清空槽位，防止后续消息拿到已经释放的野指针。
        }
        return 0;

    case WM_SIZE:
        // 窗口大小改变。lParam 低 16 位 = 新客户区宽，高 16 位 = 高。
        if (st)
        {
            RecalcMapping(st, LOWORD(lParam), HIWORD(lParam));
            // LOWORD / HIWORD 把 lParam 拆成宽和高（各 16 位）。
            RebuildDisplay(st);
            // 尺寸变了 → 缩放比变了 → 显示缓存必须重建，笔迹也要按新的缩放重画一遍。
            InvalidateRect(hWnd, nullptr, FALSE);
            // 请求重绘。FALSE = 不擦除背景（因为我们自己在 WM_PAINT 里会把整幅画满）。
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;
        // 直接返回 1（表示「已处理」）告诉系统：背景我不擦了。
        // 【为什么】WM_PAINT 里会用内存 DC 把整块客户区完整画一遍，
        // 让系统再擦一次背景不但没用，还会造成明显的闪烁。

    case WM_PAINT:
        // 收到重绘请求。这里用「双缓冲」画法：先全部画到内存 DC，
        // 最后一次性 BitBlt 到屏幕，避免闪烁。
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // BeginPaint / EndPaint 必须成对出现，而且只能在 WM_PAINT 里调用。

            RECT rc;
            GetClientRect(hWnd, &rc);
            // rc.right / rc.bottom 就是客户区宽高（rc.left / rc.top 恒为 0）。

            HDC mem = CreateCompatibleDC(hdc);
            // 创建一个和屏幕兼容的内存 DC，也就是「离屏画布」。
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            // 【坑】这张新位图的内容是【未初始化的垃圾】，
            // 所以紧接着第一件事就是用 PaintCheckerBackground 把它整块盖掉。
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            // 把位图选进内存 DC，并保存被替换出来的旧位图句柄。
            // 稍后必须把旧位图选回去，才能 DeleteObject 这张新位图 ——
            // GDI 的规则是：位图不能在被 DC 选中时删除。

            PaintCheckerBackground(mem, rc);
            // 画透明棋盘格背景（灰白格子），让用户直观地看出哪里是透明区域。
            // 实现在 ImageOps.cpp。

            if (st && g_pSrcImage && st->srcDisp && st->overlay)
            {
                Graphics g(mem);
                // 用内存 DC 构造 GDI+ 的 Graphics。
                // 【重要】Graphics 对象在析构时才会把绘制内容真正刷到 DC 上，
                // 所以它的作用域要限制在这个 if 块内（下面还有 GDI 的 BitBlt 要用这个 DC）。
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                // 这里其实是 1:1 地贴图，仍然设置高质量参数，
                // 是为了在高 DPI 缩放的场景下让像素对齐更准确。
                g.DrawImage(st->srcDisp, (INT)st->offX, (INT)st->offY,
                            st->srcDisp->GetWidth(), st->srcDisp->GetHeight());
                // 把缩放后的原图贴到居中偏移的位置上。
                // 【坑】位置用 (INT)offX / (INT)offY 取整，保证位图落在整数像素上；
                // 传小数的话 GDI+ 会再做一次重采样，画面会发糊。
                g.DrawImage(st->overlay, 0, 0);
                // 再把笔迹层整张叠上去（overlay 和窗口等大，所以从 (0,0) 贴）。
                // overlay 里面的内容是按 scale 变换后画的，所以这里不需要再缩放。

                Pen p(Color(255, 150, 150, 150));
                g.DrawRectangle(&p, (INT)st->offX, (INT)st->offY,
                                st->srcDisp->GetWidth() - 1, st->srcDisp->GetHeight() - 1);
                // 给图片画一圈灰色的细边框（Color(255,150,150,150) = 不透明的中灰）。
                // 【坑】宽高要各减 1：DrawRectangle 的宽度 / 高度是从左上角往右下算的边长，
                // 不减 1 的话右边和下边那条线会落在图片外面一个像素处。
            }
            else
            {
                SetBkMode(mem, TRANSPARENT);
                // 文字背景透明（否则文字会带一块实色底，很丑）。
                SetTextColor(mem, RGB(120, 120, 120));
                const wchar_t* tip = L"点击上方「选择图片...」载入图片\r\n载入后即可用画笔涂鸦";
                DrawTextW(mem, tip, -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
                // 还没载入图片（或缓存没建好）时，在画布正中央显示提示文字。
                // -1 = 字符串以 \0 结尾；DT_CENTER | DT_VCENTER = 水平垂直居中；
                // DT_WORDBREAK = 允许按 \r\n 和可用宽度自动换行。
            }

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            // 双缓冲的最后一步：把离屏画好的整幅内容一次性拷到屏幕 DC 上。
            // SRCCOPY = 直接覆盖（不做任何光栅运算）。

            SelectObject(mem, oldBmp);
            // 先把旧位图选回内存 DC（此时 bmp 已经不被选中了）。
            DeleteObject(bmp);
            DeleteDC(mem);
            // 释放 GDI 对象。
            // 【为什么要较真】GDI 对象数量有配额上限（默认每进程 10000 个），
            // 漏删会造成「GDI 对象泄漏」，程序跑久了就什么都画不出来了。
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_SETCURSOR:
        // 系统在询问「鼠标停在这个窗口上应该显示什么光标」。
        // lParam 低位是命中测试码；只有在客户区里才改光标，
        // 否则边框、标题栏也会被改成十字光标，看起来很奇怪。
        if (st && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            // 用系统自带的十字光标，提示用户「这里可以画」。
            return TRUE;
            // 返回 TRUE = 告诉系统「光标我已经设好了，你别再改」。
        }
        break;
        // 其它区域（边框、标题栏等）走默认处理。

    case WM_LBUTTONDOWN:
        // 鼠标左键按下 —— 一次绘制的开始（落笔）。
        if (st && g_pSrcImage)
        {
            SetFocus(hWnd);
            // 把键盘焦点交给画布（否则窗口标题栏看起来是灰的，不符合常规交互习惯）。
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            // 【坑】lParam 里打包的是「两个 16 位有符号数」，
            // 必须用 GET_X_LPARAM / GET_Y_LPARAM 宏（来自 windowsx.h）来拆，
            // 不能自己用 LOWORD / HIWORD：那样会当成无符号数，
            // 多显示器或鼠标拖到窗口左上方时出现的负坐标就会算错。

            if (st->pick)
            {
                PickColor(hWnd, st, x, y);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
                // 吸色模式：这次点击不是画线，而是取色。
                // 取完色后在 PickColor 里已经改了画笔颜色和 pick 状态，
                // 这里再请求一次重绘刷新画面。
            }

            float ix, iy;
            if (ToImage(st, x, y, &ix, &iy))
            {
                // 注意这里【故意没有】判断点是否落在图片范围内：
                // 笔迹允许画到图片外的留白上（输出时超出画布的部分会被自然裁掉），
                // 这样手感更像「在整块画布上涂鸦」，而不是被图片边界卡住。
                st->cur.pts.clear();
                // 清掉上一笔残留的点（cur 是复用的成员变量，必须自己清）。
                st->cur.pts.push_back(PointF(ix, iy));
                // 落笔：把起点（图像坐标）加进当前笔迹。
                st->cur.color = st->brush;
                // 颜色在「落笔这一刻」就固定下来，之后用户改颜色不会影响已经画的笔。
                st->cur.width = (st->scale > 0.0f) ? (st->brushW / st->scale) : st->brushW;
                // 【核心换算】屏幕上的粗细 → 图像坐标下的粗细：要【除以】scale。
                // 举例：scale = 0.5（图片被缩小一半显示），用户希望屏幕上看起来 5 像素粗，
                // 换算到图像坐标就是 5 / 0.5 = 10 —— 因为绘制时这个 10 又会被乘回 0.5，
                // 屏幕上看到的仍然是 5 像素。
                // 这样线宽就「跟着视觉走」：不管窗口怎么缩放，屏幕上的粗细始终是滑块
                // 设定的值；而输出到原图分辨率时，线宽会自动按原图比例放大，视觉一致。
                if (st->cur.width < 1.0f) st->cur.width = 1.0f;
                // 下限 1 像素：图片被放大很多倍时 scale 可能有好几十，
                // brushW / scale 会小于 1，线细得几乎看不见，所以兜底到 1。
                st->drawing = true;
                SetCapture(hWnd);
                // 【必须】捕获鼠标！这样即使光标拖到画布外面，
                // 鼠标消息仍然会发给这个窗口 —— 否则「拖出窗口再松开」时
                // WM_LBUTTONUP 收不到，drawing 会一直卡在 true，笔画粘住无法结束。
                RenderOverlay(st, false);
                // 立刻把新开的这一笔（目前只有起点）画到笔迹层。
                InvalidateRect(hWnd, nullptr, FALSE);
                // 请求重绘，让用户马上看到起笔。
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        // 鼠标在窗口内移动（每来一个点触发一次，频率取决于鼠标回报率和系统设置）。
        if (st && st->drawing)
        {
            // 只有「正在绘制」时才处理；没按左键时的移动直接忽略。
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            float ix, iy;
            if (ToImage(st, x, y, &ix, &iy))
            {
                const PointF& last = st->cur.pts.back();
                // 取笔迹的最后一个点（后面要算当前点离它多远）。
                // back() 返回的是引用，用 const 引用接收可以避免复制一次 PointF。
                const float dx = (ix - last.X) * st->scale;
                const float dy = (iy - last.Y) * st->scale;
                // 【注意】这里特意乘回 scale，把「图像坐标下的距离」换算成
                // 「屏幕上的距离」，因为下面那个 1 像素的阈值是屏幕概念。
                if (dx * dx + dy * dy >= 1.0f)     // 至少移动 1 显示像素才记点
                {
                    // 用「距离的平方」来比较（两边都平方，省掉一次开方运算），结果等价。
                    // 【为什么要这个阈值】鼠标移动消息可能在同一像素上反复触发，
                    // 不过滤会塞进大量重复点：既浪费内存，又让撤销的粒度变碎。
                    st->cur.pts.push_back(PointF(ix, iy));
                    // 追加一个新点。注意存的是【图像坐标】，不是屏幕坐标！
                    RenderOverlay(st, true);
                    // 【增量绘制】只补画最后这一小段，不整层重画 —— 这是拖动手感流畅的关键。
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
        }
        return 0;

    case WM_LBUTTONUP:
        // 鼠标左键松开 —— 一次绘制结束（收笔）。
        if (st && st->drawing)
        {
            ReleaseCapture();
            // 释放鼠标捕获，和 WM_LBUTTONDOWN 里的 SetCapture 配对出现。
            // 【坑】忘了 ReleaseCapture 的话，鼠标会被一直「捕获」在这个窗口上，
            // 界面上其它控件都点不动，像是程序卡死了。
            st->drawing = false;
            if (!st->cur.pts.empty())
            {
                st->strokes.push_back(st->cur);
                // 【撤销的基础】把这一笔存进数组。
                // push_back 会复制一份 st->cur 放进数组（Stroke 里含 vector，属于深拷贝），
                // 所以下面可以放心地 clear。
                st->cur.pts.clear();
                // 清空当前笔的点，准备画下一笔。
                // 注意 color / width 不用清，下次落笔时会被重新赋值覆盖。
            }
            RenderOverlay(st, false);
            // 【必须全量重绘】收笔是一次「结构性变化」：
            // 这一笔刚刚从 cur 转正进 strokes，而拖动过程中用的是增量绘制，
            // 只补画了最后一小段；这里整层重画一遍，保证笔迹完整、边缘干净。
            InvalidateRect(hWnd, nullptr, FALSE);
            // 通知父页（撤销按钮可用性）
            PostMessageW(GetParent(hWnd), WM_APP_RECT_CHANGED, 0, 0);
            // 用 PostMessage（异步投递）而不是 SendMessage：
            // 这里只是通知父页刷新按钮，不需要立刻拿到结果；
            // 异步发送还能避免「在消息处理过程中又同步调用父窗口过程」造成的重入。
            // PageDraw.cpp 收到后调用 UpdateUndoButton()，让「撤销 / 清空」按钮变为可用。
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
    // 未处理的消息交给系统默认处理 —— 这一步【绝对不能漏】，
    // 否则窗口激活、滚动、销毁等一大堆基础行为都会失效。
}

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  DrawCanvas_Create —— 创建涂鸦画布窗口（供 PageDraw.cpp 调用）
//  参数：parent —— 父窗口；id —— 子窗口 ID（App.h 里是 IDC_DR_CANVAS = 1700）。
//  返回值：画布窗口句柄；失败返回 nullptr。
//  说明：注册窗口类是「一次性」操作，用一个 static bool 保证只注册一次；
//        重复注册会返回 ERROR_CLASS_ALREADY_EXISTS，这里特意容忍这种情况。
// ---------------------------------------------------------------------------
HWND DrawCanvas_Create(HWND parent, int id)
{
    static bool registered = false;
    // static 局部变量只初始化一次，并在多次调用之间保持它的值 ——
    // 正好用来当「这个窗口类注册过没有」的开关。
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        // 【坑】必须把结构体清零（= {}）！WNDCLASSEXW 字段很多，
        // 漏填的字段如果留着栈上的垃圾值，RegisterClassExW 会莫名其妙地失败。
        wc.cbSize = sizeof(wc);
        // 结构体大小。系统靠它判断结构体版本，必须填。
        wc.style = CS_HREDRAW | CS_VREDRAW;
        // 窗口的宽度或高度变化时整个窗口重绘（图片是缩放的，本来就要全部重画）。
        wc.lpfnWndProc = Proc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        // 默认光标用十字（WM_SETCURSOR 里还会再设一次）。
        wc.hbrBackground = nullptr;
        // 【关键】不设背景画刷。
        // 因为 WM_ERASEBKGND 里我们直接返回 1 表示「自己擦」；
        // 如果这里设了画刷，系统仍然会用它擦一遍背景，就会闪。
        wc.lpszClassName = DRAWCANVAS_CLASS;
        // 类名 "Pic246DrawCanvas"，定义在 App.h。
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        // 注册失败要区分原因：如果只是「类已存在」不算真失败（可能别的地方先注册了），
        // 其它错误才真的返回 nullptr 表示创建失败。
        registered = true;
    }

    return CreateWindowExW(WS_EX_CLIENTEDGE, DRAWCANVAS_CLASS, L"",
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    // 扩展样式 WS_EX_CLIENTEDGE = 带一圈下凹的 3D 边框，看起来像一块画板。
    // 窗口样式：WS_CHILD（子窗口）| WS_VISIBLE（创建后立即显示）
    //           | WS_CLIPSIBLINGS（裁剪掉被同级窗口盖住的部分，减少闪烁）。
    // 位置尺寸 0,0,10,10 只是个占位值，真正的布局由 PageDraw.cpp 的 Layout()
    //   用 MoveWindow 完成。
    // (HMENU)(INT_PTR)id —— 子窗口用「控件 ID」来标识，这里做两次强制转换，
    //   是为了适配 64 位下指针宽度的变化。
}

// ---------------------------------------------------------------------------
//  DrawCanvas_RefreshImage —— 源图变化后刷新画布缓存
//  参数：h —— 画布窗口句柄。由 PageDraw.cpp 在收到 WM_APP_IMAGE_CHANGED 时调用。
//  做的事：重新算映射 → 重建显示缓存 → 请求重绘。
//  注意：这里【不会】清空已有笔迹（st->strokes 原样保留）。
//        因为笔迹存的是图像坐标，换一张图后它们会被原样画在新图上。
//        如果将来希望「换图就清空笔迹」，需要在这里额外调用 DrawCanvas_Clear。
// ---------------------------------------------------------------------------
void DrawCanvas_RefreshImage(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return;
    RECT rc;
    GetClientRect(h, &rc);
    // 取当前真实的客户区尺寸（而不是用缓存的 st->cw / st->ch），保证映射准确。
    RecalcMapping(st, rc.right, rc.bottom);
    RebuildDisplay(st);
    InvalidateRect(h, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  DrawCanvas_SetBrush —— 设置画笔颜色和粗细
//  参数：h —— 画布；color —— COLORREF 颜色；width —— 粗细（屏幕像素，<=0 时按 1 处理）。
//  说明：只影响【之后】新画的笔迹。已经画完的笔不受影响，
//        因为每笔各自的颜色和线宽都存在自己的 Stroke 里。
// ---------------------------------------------------------------------------
void DrawCanvas_SetBrush(HWND h, COLORREF color, int width)
{
    DoodleState* st = St(h);
    if (!st) return;
    st->brush = Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
    // 【坑】COLORREF 的内存布局是 0x00BBGGRR（注意是 BGR 顺序！），
    // 必须用 GetRValue / GetGValue / GetBValue 宏来拆分，不能按 RGB 顺序自己拆。
    // GDI+ 的 Color 则是 (A, R, G, B)，这里 Alpha 固定填 255（不透明）。
    st->brushW = (float)((width > 0) ? width : 1);
    // 防御性处理：宽度非法（0 或负数）时按 1 算，避免画出「看不见」的线。
}

// ---------------------------------------------------------------------------
//  DrawCanvas_SetPickMode —— 打开 / 关闭吸色模式
//  参数：h —— 画布；on —— true 进入吸色模式，false 回到普通画笔模式。
//  说明：吸色成功后 PickColor() 会自动把 pick 置回 false；
//        父页 PageDraw.cpp 也会同步把「吸色」按钮弹起
//        （见它处理 WM_APP_PICKED_COLOR 的分支）。
// ---------------------------------------------------------------------------
void DrawCanvas_SetPickMode(HWND h, bool on)
{
    DoodleState* st = St(h);
    if (!st) return;
    st->pick = on;
    SetCursor(LoadCursorW(nullptr, st->pick ? IDC_CROSS : IDC_ARROW));
    // 立即换一次光标，给用户一点反馈。
    // 【坑】光标形状最终是由 WM_SETCURSOR 决定的：鼠标一动，WM_SETCURSOR
    // 就会把光标设回十字，所以这里的视觉提示其实是「瞬时」的。
    // 真正的模式状态以 st->pick 为准，UI 上的按钮状态由 PageDraw.cpp 维护。
}

// ---------------------------------------------------------------------------
//  DrawCanvas_CanUndo —— 是否有内容可以撤销
//  返回值：有已经完成的笔迹，或者正在画的那一笔时返回 true；窗口无效返回 false。
//  用途：PageDraw.cpp 用它决定「撤销 / 清空」按钮是否可点（不可用时灰掉）。
//  实现：strokes 非空，或者 drawing == true（正在画也允许撤销 —— 含义是放弃当前这一笔）。
// ---------------------------------------------------------------------------
bool DrawCanvas_CanUndo(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return false;
    return !st->strokes.empty() || st->drawing;
}

// ---------------------------------------------------------------------------
//  DrawCanvas_Undo —— 撤销
//  行为分两种情况：
//    · 正在画（鼠标还按着没松）：这一步「撤销」= 放弃当前这一笔，
//      并且必须 ReleaseCapture() 解除鼠标捕获，否则鼠标会一直粘在画布上。
//    · 没在画：把 strokes 数组的最后一个元素弹掉（pop_back），就是撤销一笔。
//  最后【必须整层全量重绘】：位图上还留着被删掉的那一笔，
//  只有先 Clear 再把剩余的笔迹全部重画，它才会真正从画面上消失。
// ---------------------------------------------------------------------------
void DrawCanvas_Undo(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return;
    if (st->drawing)
    {
        st->drawing = false;
        st->cur.pts.clear();
        ReleaseCapture();
        // 与 WM_LBUTTONDOWN 里的 SetCapture(hWnd) 配对，放开鼠标。
        // 【坑】撤销时若忘了 ReleaseCapture，鼠标会被继续捕获在这个窗口上，
        // 之后的点击全跑到这里来，界面看上去就像卡住了。
    }
    else if (!st->strokes.empty())
    {
        st->strokes.pop_back();
        // pop_back 删除并析构最后一个元素（std::vector 自动管理内存）。
        // 这就是「矢量笔迹」最爽的地方：一次撤销只要一行代码。
    }
    RenderOverlay(st, false);
    // 【必须全量重绘】不先清空 overlay 再重画的话，被撤销的那一笔会残留在位图上。
    InvalidateRect(h, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  DrawCanvas_Clear —— 清空所有笔迹
//  参数：h —— 画布窗口。
//  说明：清空是「一次全删」；只想删最后一笔请用 DrawCanvas_Undo。
//        同样需要全量重绘，屏幕上的旧笔迹才会消失。
//  注意：这里不需要 ReleaseCapture —— 正常流程下点按钮时鼠标早就松开了。
// ---------------------------------------------------------------------------
void DrawCanvas_Clear(HWND h)
{
    DoodleState* st = St(h);
    if (!st) return;
    st->strokes.clear();
    // 清空已完成的笔迹（vector 的容量会保留，元素全部析构）。
    st->cur.pts.clear();
    // 也清掉可能正在画的那一笔。
    st->drawing = false;
    RenderOverlay(st, false);
    InvalidateRect(h, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  DrawCanvas_RenderFull —— 按【原图分辨率】合成笔迹层（输出用）
//  参数：h —— 画布窗口。
//  返回值：新建的 Bitmap*（尺寸 = 原图 g_srcW × g_srcH，透明底，内容只有笔迹）；
//          失败返回 nullptr。
//  【重要：所有权】返回的是一个新对象，由【调用者负责 delete】。
//        PageDraw.cpp 的 OnProcess() 里用它和原图叠加，用完会 delete；
//        注意不要在这里 delete 它，也不要让它指向 st 内部的缓存。
//  为什么不直接把屏幕上的 overlay 拿去用：
//        屏幕上的 overlay 是按显示缩放算出来的，尺寸可能只有原图的几分之一，
//        直接输出会糊。这里把【图像坐标】的笔迹在原始尺寸上重画一遍，
//        于是输出永远和屏幕上看到的形状一致，分辨率却是原图的。
//  注意：这里 scale = 1、offset = 0（没有设置任何变换矩阵），
//        所以 Stroke 里的图像坐标直接就是输出位图的像素坐标。
// ---------------------------------------------------------------------------
Bitmap* DrawCanvas_RenderFull(HWND h)
{
    DoodleState* st = St(h);
    if (!st || !g_pSrcImage) return nullptr;

    Bitmap* out = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
    // 按原图的宽高建一张透明画布（g_srcW / g_srcH 是 UINT，构造函数要 INT，隐式转换即可）。
    if (out->GetLastStatus() != Ok)
    {
        delete out;
        return nullptr;
        // 分配失败（超大图内存不足）：先释放再返回 nullptr。
        // 调用方 PageDraw.cpp 会弹「合成笔迹失败（内存不足？）」的提示。
    }

    Graphics g(out);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // 抗锯齿，保证边缘效果和屏幕上看到的保持一致。
    g.SetCompositingMode(CompositingModeSourceOver);
    g.Clear(Color(0, 0, 0, 0));
    // 清成全透明：这一层只放笔迹，最终由调用方把原图铺在它下面。
    for (size_t i = 0; i < st->strokes.size(); ++i)
        DrawWholeStroke(g, st->strokes[i]);
    // 把每一笔按图像坐标原样重画 —— 全程没有任何缩放，所以这就是「原始分辨率」。
    // 复用 DrawWholeStroke()，和屏幕上用的是同一套绘制逻辑，形状必然一致。

    return out;
}
