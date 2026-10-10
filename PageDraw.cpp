// ============================================================================
//  【文件说明】 PageDraw.cpp —— Pic246 的「涂鸦」功能页
// ============================================================================
//
//  ● 本页职责（一句话）：
//      在一张已经打开的图片上「用画笔画笔画」：可以调笔的粗细、换颜色、
//      吸色取色（点画面把颜色吸过来当画笔色）、逐笔撤销、一键清空；
//      最后按【原图分辨率】把所有笔迹合成进原图，另存成一个新文件
//      （文件名后缀 _draw，例如 photo_draw.png）。
//
//  ● 它在整个程序里的位置：
//      Pic246 只有一个主窗口（Project001.cpp）。主窗口启动时一次性把 7 个功能页
//      全部创建出来（PageResize / PageCrop / PageAdjust / PageIco /
//      PageCompress / PageConvert / PageDraw），再根据左侧边栏的点击，
//      用 ShowWindow(SW_SHOW / SW_HIDE) 在它们之间切换显示。
//      换句话说：本文件只是「其中一页」，不负责主窗口、不负责打开图片，
//      也不负责真正往位图上落笔 —— 那些都在别的文件里。
//
//  ● 和哪些文件配合（各自负责什么）：
//      · App.h            —— 公共声明（全工程的「总纲 + 通讯录」）。本文件用到的
//                            全局变量（g_pSrcImage / g_srcW / g_srcH / g_hMain）、
//                            控件 ID（IDC_DR_*）、自定义消息（WM_APP_IMAGE_CHANGED /
//                            WM_APP_RECT_CHANGED / WM_APP_PICKED_COLOR）、
//                            工具函数（D() / CreateCtl() / SetAppStatus() /
//                            PaintColorSwatch() / ChooseFillColor() /
//                            GuessSourceMime() / SaveBitmapAsFormat()）以及
//                            DrawCanvas_* 系列函数原型，全都来自这个头文件。
//      · DrawCanvas.cpp   —— 真正「画画」的地方：一个自定义的涂鸦画布子窗口。
//                            它对外暴露 DrawCanvas_Create（建画布）、
//                            DrawCanvas_RefreshImage（换底图）、
//                            DrawCanvas_SetBrush（设笔色与笔宽）、
//                            DrawCanvas_SetPickMode（开/关吸色）、
//                            DrawCanvas_CanUndo / Undo / Clear（撤销与清空）、
//                            DrawCanvas_RenderFull（按原图分辨率合成笔迹层）。
//                            本页只负责「摆控件」和「把用户操作转发给画布」。
//      · ImageOps.cpp     —— 图片引擎。本页用到 GuessSourceMime()
//                            （按源文件扩展名猜输出格式、以及该格式支不支持透明）
//                            和 SaveBitmapAsFormat()（真正写文件并弹出「完成」提示；
//                            ⚠ 它内部会 delete 掉传进去的位图，所以本页不能再删）。
//      · Project001.cpp   —— 主窗口。负责打开图片、设置全局状态、广播
//                            WM_APP_IMAGE_CHANGED，并提供 SetAppStatus() 状态栏。
//
//  ● 消息驱动的整体流程（Win32 程序的典型写法：程序不主动循环，而是等系统
//    把「发生了什么」以消息的形式投递进窗口过程 Proc，再按 message 分发）：
//      WM_CREATE            —— 页面窗口刚建好：一次性创建画布与全部控件，
//                              设置滑块范围与默认笔色，最后调 Layout() 排一次版。
//      WM_SIZE              —— 页面被拉大 / 缩小：重新 Layout() 摆放所有控件。
//      WM_HSCROLL           —— 用户拖动了笔宽滑块：刷新数值标签并把新笔宽告诉画布。
//      WM_COMMAND           —— 有控件被点击（按钮 / 复选）：按控件 ID 分发处理。
//      WM_APP_IMAGE_CHANGED —— 主窗口换了源图：让画布重新载入底图并刷新按钮状态。
//      WM_APP_RECT_CHANGED  —— 画布通知「笔迹集合变了」（收笔 / 撤销）：刷新按钮。
//      WM_APP_PICKED_COLOR  —— 画布吸到颜色了（颜色放在 wParam）：更新笔色与按钮。
//      WM_DRAWITEM          —— 色块（带 SS_OWNERDRAW 的自绘 Static）要求自己画自己。
//
//  ● 新手最容易踩的几个坑（先在开头留个印象，读到具体代码时再看细节）：
//      1) 输出用的输出位图 out，本页故意【没有】自己 delete —— 因为
//         SaveBitmapAsFormat() 内部会 delete 它；本页再 delete 一次就是重复释放，
//         程序会直接崩溃。反过来，overlay 是 DrawCanvas_RenderFull() 的返回值，
//         不在别人的责任范围内，所以本页必须自己 delete。
//      2) 「吸色」按钮虽然带 BS_AUTOCHECKBOX 样式，但它的「按下/弹起」状态
//         要靠 Button_SetCheck() / Button_GetCheck() 来读写；只有这样，
//         程序主动改变状态时按钮外观才会跟着变，不会和实际状态脱节。
//      3) 所有界面尺寸都写成 D(数字)：D() 把 96dpi 下的「逻辑像素」换算成
//         当前屏幕的实际像素。屏幕 150% 缩放时 D(240) = 360；如果直接写 240，
//         高分屏上整个界面会小得看不清。
//      4) 坐标系不止一种：画布里的坐标是【图像坐标】（原图像素），
//         而滑块上的笔宽是【屏幕显示像素】，两者之间差一个缩放倍数，
//         这个换算由 DrawCanvas.cpp 负责，本页只管把「视觉上想要的粗细」传过去。
// ============================================================================
// ============================================================================
//  PageDraw.cpp —— 功能页：涂鸦
//    · 画笔绘制（可调笔粗细 / 颜色）
//    · 吸色（点击画面取色，取完自动回到画笔）
//    · 撤销 / 清空
//    · 输出到原目录，后缀 _draw
// ============================================================================
#include "App.h"   // 公共声明：全局状态、控件 ID、自定义消息、工具函数、DrawCanvas_* 原型

using namespace Gdiplus;   // 直接写 Bitmap / Graphics / Color 等 GDI+ 类型，省去 Gdiplus:: 前缀

// ---------------------------------------------------------------------------
// 本页的控件句柄
//   为什么用 static 全局变量保存句柄？
//     因为窗口过程 Proc 的签名是系统写死的 (HWND, UINT, WPARAM, LPARAM)，
//     没有地方传自定义参数。所以「这个页面上有哪些控件」只能记在文件级
//     （static = 只在当前 .cpp 内可见，不会和别的页面的同名变量冲突）。
//   为什么初始化成 nullptr？
//     WM_CREATE 之前这些句柄还不存在；初值为 nullptr 可以避免拿野句柄去调 API。
// ---------------------------------------------------------------------------
static HWND s_hPage = nullptr;      // 本页窗口自身（Layout() 要用它取客户区大小）
static HWND s_hCanvas = nullptr;    // 涂鸦画布子窗口（在 DrawCanvas.cpp 里实现）
static HWND s_hGrpBrush = nullptr;  // 「画笔设置」分组框（只是视觉框，不处理消息）
static HWND s_hTrkSize = nullptr;   // 笔宽滑块（Trackbar），范围 1..60
static HWND s_hLblSize = nullptr;   // 笔宽数值标签（跟随滑块实时更新文字）
static HWND s_hSwatch = nullptr;    // 当前笔色的小色块（SS_OWNERDRAW，自己画）
static HWND s_hBtnColor = nullptr;  // 「画笔颜色...」按钮（弹出系统颜色对话框）
static HWND s_hBtnPick = nullptr;   // 「吸色」按钮（复选样式：按下=吸色模式）
static HWND s_hBtnUndo = nullptr;   // 「撤销」按钮（无可撤销内容时变灰）
static HWND s_hBtnClear = nullptr;  // 「清空」按钮（无可清空内容时变灰）
static HWND s_hLblHint = nullptr;   // 右侧的用法提示文字（多行 Static）
static HWND s_hBtn = nullptr;       // 底部大按钮「保存涂鸦并输出」

static COLORREF s_brushColor = RGB(235, 40, 45);   // 当前画笔颜色；默认一个偏红的颜色
                                                   // 用 COLORREF 而不是 Gdiplus::Color，
                                                   // 是因为色块绘制 / 颜色对话框都用 COLORREF

// ---------------------------------------------------------------------------
// 画笔相关的两个小工具函数
//   注意：这两个函数都没有加 static —— 保持原样不要动它们。
// ---------------------------------------------------------------------------
static int BrushSize() { return (int)SendMessageW(s_hTrkSize, TBM_GETPOS, 0, 0); }   // 读滑块当前位置 = 当前笔宽（屏幕像素）
// TBM_GETPOS 是 Trackbar 的标准消息，用来查询滑块当前位置（即用户选中的数值）。
// 返回值用 (int) 强制转换：SendMessageW 的返回值类型是 LRESULT（整数），
// 而我们保证滑块范围是 1..60，转成 int 是安全的。

static void UpdateSizeLabel()
{
    wchar_t buf[64];   // 足够放下「画笔大小：60 像素」这种短句的宽字符缓冲区
    swprintf_s(buf, L"画笔大小：%d 像素", BrushSize());   // 安全版 sprintf：会检查缓冲区大小
    SetWindowTextW(s_hLblSize, buf);                     // 把拼好的文字写进标签控件
}
// 用途：把滑块当前值同步显示到「画笔大小：N 像素」这个标签上。
// 什么时候调用：创建时、滑块拖动时（WM_HSCROLL）。
// 坑：这里用的是 wchar_t 和 L"..."宽字符串，且必须用 swprintf_s 而不是 sprintf ——
//     宽字符下 sprintf 会按字节处理，中文会乱码，而且带边界检查的版本更安全。

static void UpdateUndoButton()
{
    EnableWindow(s_hBtnUndo, DrawCanvas_CanUndo(s_hCanvas) ? TRUE : FALSE);   // 有笔迹→可撤销；没有→变灰
    EnableWindow(s_hBtnClear, DrawCanvas_CanUndo(s_hCanvas) ? TRUE : FALSE);  // 清空按钮共用同一条件：
                                                                              // 没画过东西就没什么可清空的
}
// 用途：根据画布「还有没有笔迹」来决定「撤销 / 清空」两个按钮是可用还是变灰。
// 为什么两个按钮用同一个条件：本画的笔迹是逐笔记录在数组里的，
//   「没有可撤销的笔」也就等价于「画布上什么都没有」，所以清空同样无意义。
// 坑：EnableWindow 的第二个参数是 BOOL（真/假），这里用三目运算显式写成 TRUE/FALSE，
//     别把 bool 直接传进去 —— 虽然多数情况下能隐式转换，但显式写更清楚、也不会有警告。

static void ApplyBrush()
{
    DrawCanvas_SetBrush(s_hCanvas, s_brushColor, BrushSize());   // 把「当前颜色 + 当前粗细」下发给画布
}
// 用途：把本页维护的画笔设置（颜色 + 粗细）同步给画布控件。
// 什么时候调用：创建时、换颜色后、拖动滑块后、吸色完成后、切回画笔模式时。
// 为什么要单独抽一个函数：凡是「颜色或粗细可能变了」的地方都调它一次，
//   避免到处重复写同一行代码而漏掉某一处。

static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);   // 取本页客户区大小（rc.right 就是宽度，rc.bottom 是高度）
    const int cx = rc.right, cy = rc.bottom;   // 客户区宽 / 高，后面反复用
    const int M = D(12);                       // 统一的外边距（逻辑 12px，经 DPI 缩放）

    // --- 先决定右侧参数面板的宽度 ---
    int panelW = D(240);                              // 面板理想宽度：逻辑 240px
    if (panelW > cx - D(160)) panelW = cx - D(160);   // 窗口太窄时压缩面板，至少给画布留 160px
    if (panelW < D(180)) panelW = D(180);             // 但面板也不能无限窄，兜底 180px

    // --- 左侧画布：占据剩余空间 ---
    int cvW = cx - panelW - M * 3;                 // 总宽 - 面板宽 - 三条边距（左右 + 面板与画布之间那条）
    if (cvW < D(120)) cvW = D(120);                // 极小窗口下兜底，防止画布被算成 0 或负数
    MoveWindow(s_hCanvas, M, M, cvW, cy - M * 2, TRUE);   // 画布：左边距 M，上下各留 M

    // --- 右侧面板的基准坐标 ---
    const int px = M * 2 + cvW;   // 面板左边界 = 左边距 + 画布宽 + 中间那条边距
    const int pw = cx - px - M;   // 面板可用宽度 = 客户区宽 - 面板左边 - 右边距
    int py = M;                   // 面板内当前摆放位置的纵坐标（从上往下依次排）

    // --- 「画笔设置」分组框及其内部控件 ---
    const int hBrush = D(206);                                         // 分组框固定高度（逻辑 206px）
    MoveWindow(s_hGrpBrush, px, py, pw, hBrush, TRUE);                 // 分组框外框
    MoveWindow(s_hTrkSize, px + D(14), py + D(26), pw - D(28), D(30), TRUE);   // 滑块：左右各缩进 14
    MoveWindow(s_hLblSize, px + D(16), py + D(58), pw - D(32), D(20), TRUE);   // 「画笔大小：N 像素」标签
    MoveWindow(s_hSwatch, px + D(16), py + D(84), D(40), D(26), TRUE);         // 颜色小方块（固定 40x26）
    MoveWindow(s_hBtnColor, px + D(64), py + D(84), pw - D(80), D(26), TRUE);  // 「画笔颜色...」按钮，与色块同一行
    MoveWindow(s_hBtnPick, px + D(16), py + D(118), pw - D(32), D(30), TRUE);  // 「吸色」按钮（独占一行）
    MoveWindow(s_hBtnUndo, px + D(16), py + D(154), (pw - D(40)) / 2, D(30), TRUE);   // 撤销：左半
    MoveWindow(s_hBtnClear, px + D(24) + (pw - D(40)) / 2, py + D(154),
               (pw - D(40)) / 2, D(30), TRUE);   // 清空：右半（D(24)=左 16 + 中间 8 的间隔）
    // 上面这一组 MoveWindow 里的数字都是相对分组框左上角的偏移量，
    // 所以分组框大小一变，里面的控件也会跟着整体平移，不会跑偏。

    py += hBrush + D(8);                                // 排完分组框，往下挪出「高度 + 8px 间隔」
    MoveWindow(s_hLblHint, px, py, pw, D(96), TRUE);    // 用法提示文字块（多行，给 96px 高）

    const int btnH = D(42);                                       // 底部大按钮高度
    MoveWindow(s_hBtn, px, cy - M - btnH, pw, btnH, TRUE);        // 贴底摆放：底边距 M
}
// 用途：按当前页面的客户区大小，用 MoveWindow 把画布和右侧所有控件重新摆一遍。
// 什么时候调用：WM_CREATE 时排一次，WM_SIZE（窗口缩放）时再排一次。
// 为什么不用「固定像素」而要算：这个程序允许自由拉伸窗口，写死坐标会让界面在大窗口下
//   缩在左上角、在小窗口下互相重叠。用相对计算能自适应。
// 坑：
//   · MoveWindow 最后一个参数 TRUE = 立刻重绘该控件；不重绘的话控件会「先移动、
//     后露出旧内容」，看起来像撕裂。干脆用 TRUE，代价是布局时多几次重绘，可接受。
//   · 所有尺寸都过一遍 D()。如果只对部分尺寸做 DPI 缩放，150% 屏上各控件间距就会错乱。

static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)   // 全局源图指针为空 = 用户还没打开图片
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);   // 信息图标 + 只有一个「确定」
        return;   // 没图可涂，直接结束
    }
    if (!DrawCanvas_CanUndo(s_hCanvas))   // 一笔都没画过
    {
        if (MessageBoxW(hPage, L"还没有画任何笔迹，仍要输出原图副本吗？", L"提示",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)   // 问号图标 + 是/否
            return;   // 用户选了「否」，本次不输出
    }
    // 注意：这里【不是】报错，而是给用户一次反悔机会 —— 没涂鸦时就输出等于复制原图，
    // 有些人确实需要这个行为（当「另存为副本」用），所以问一下而不是直接禁止。

    Bitmap* overlay = DrawCanvas_RenderFull(s_hCanvas);   // 让画布按【原图分辨率】把笔迹重新画成一张带透明通道的位图
    if (!overlay)   // 返回 nullptr = 画布合成失败（多半是内存不足）
    {
        MessageBoxW(hPage, L"合成笔迹失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        return;
    }
    // ⚠ overlay 是【本函数负责释放】的：它在下面用完（或出错分支）必须 delete。

    SetAppStatus(L"正在合成并输出，请稍候...");   // 底部状态栏提示，让用户知道程序在忙
    UpdateWindow(g_hMain);                        // 立刻把状态栏重画出来，否则提示要等这一轮处理完才可见

    const OutFmtGuess f = GuessSourceMime();   // 按源文件扩展名推测输出格式（mime / 扩展名 / 是否支持透明）

    Bitmap* out = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);   // 新建一张和原图等大、带 Alpha 的输出位图
    if (out->GetLastStatus() != Ok)   // ⚠ GDI+ 的构造函数不抛异常，分配失败只能靠 GetLastStatus() 判断
    {
        delete out;      // 失败也要删：Bitmap 对象本身可能已分配，只是内部像素缓冲失败
        delete overlay;  // 别忘了把前面合成的 overlay 一起释放，否则内存泄漏
        MessageBoxW(hPage, L"创建输出位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    {
        // 用一个花括号把 Graphics 圈起来：它的作用域一结束就会析构，
        // 从而把绘制内容真正刷进 out 里。不圈的话后续可能拿到还没写完的位图。
        Graphics g(out);   // 在这张输出位图上建立绘图上下文
        g.SetCompositingMode(CompositingModeSourceCopy);   // 先设成「直接覆盖」：目的是把底色写实，而不是往未初始化像素上叠加
        if (!f.alpha)   // 原格式不支持透明通道（典型就是 JPEG）
        {
            g.Clear(Color(255, 255, 255, 255));            // 【关键】先铺一层不透明白底
            g.SetCompositingMode(CompositingModeSourceOver);   // 再切回「正常叠加」，让后面的绘制按各自 Alpha 混合
        }
        // 为什么必须先铺白底：JPEG 没有透明通道，如果源图本身带透明（比如 PNG 转存成 jpg），
        // 那些透明像素在「直接覆盖」模式下会保留 Alpha=0，存成 JPEG 时透明信息丢失，
        // 往往会变成黑色或一片杂色。先铺白底再叠加，透明处自然显示为白色，符合直觉。
        g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);   // 第一层：原图，按原分辨率铺满
        g.DrawImage(overlay, 0, 0, (INT)g_srcW, (INT)g_srcH);       // 第二层：笔迹，叠在图上
    }
    delete overlay;   // 笔迹已合进 out，overlay 用完立刻释放（它不在 SaveBitmapAsFormat 的责任里）

    const ULONG q = (f.mime == L"image/jpeg" || f.mime == L"image/webp") ? 95 : 100;   // 有损格式给 95，无损格式给 100
    // 为什么这样区分：quality 参数只对 JPEG/WebP 这类有损编码有意义；
    // 对 PNG/BMP 这类无损格式，这个值基本被忽略，给 100 只是明确表达「要无损」。
    SaveBitmapAsFormat(out, L"_draw", f.mime, f.ext, q, hPage);   // 保存（后缀 _draw）并弹「完成」提示
    // ⚠ 这一行之后【绝对不要】再 delete out —— SaveBitmapAsFormat 内部已经把它删掉了，
    //    再删一次就是「重复释放同一块内存」，程序会当场崩溃。
    // 另外它内部还会负责弹保存对话框、按后缀生成新文件名（原名_draw.扩展名），并把结果显示到状态栏。
}
// 用途：本页的「保存涂鸦并输出」主流程 —— 校验 → 合成笔迹 → 铺底 → 叠加 → 存盘。
// 参数：hPage —— 本页窗口句柄，用作消息框和保存对话框的父窗口（对话框居中于它、并阻塞它）。
// 返回值：无。
// 坑：
//   · 两条失败分支都必须把已经拿到的临时对象（out / overlay）释放干净，否则多失败几次内存就涨上去了。
//   · overlay 和 out 的释放责任不同（前者归本函数，后者归 SaveBitmapAsFormat），千万别搞混。

static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // 这是本页窗口的窗口过程：系统把发生在该窗口上的事件以消息形式投递到这里。
    // 约定：自己处理完的消息返回 0；没处理的要 break 出去交给 DefWindowProcW，
    //       否则窗口的激活、尺寸、销毁等基础行为都会失效。
    switch (message)
    {
    case WM_CREATE:
        // 窗口刚被创建时触发（在 CreateWindowExW 内部同步调用）。
        // 这是「一次性初始化」的最佳时机：建子控件、设初值、排一次版。
        {
            s_hPage = hWnd;   // 先记住自己，Layout() 里要靠它取客户区大小

            s_hCanvas = DrawCanvas_Create(hWnd, IDC_DR_CANVAS);   // 创建涂鸦画布（真正的绘图逻辑在 DrawCanvas.cpp）

            s_hGrpBrush = CreateCtl(L"Button", L"画笔设置", WS_VISIBLE | BS_GROUPBOX,
                                    hWnd, IDC_DR_GRP_BRUSH);
            // BS_GROUPBOX = 分组框（一圈标题边框），只起视觉归类作用，本身不响应点击。
            s_hTrkSize = CreateCtl(L"msctls_trackbar32", L"",
                                   WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_DR_TRK_SIZE);
            // "msctls_trackbar32" 是 Trackbar（滑块）控件的系统窗口类名。
            // TBS_HORZ = 横向滑块；WS_TABSTOP = 允许用 Tab 键聚焦到它（键盘可用性）。
            SendMessageW(s_hTrkSize, TBM_SETRANGE, TRUE, MAKELPARAM(1, 60));   // 笔宽范围 1..60（屏幕像素）
            // 为什么选 1..60：1 是能看见的最小痕迹，60 已经能当「粗马克笔」用；
            // 范围再大滑块就不好精确拖动了。MAKELPARAM 把低 16 位当最小值、高 16 位当最大值。
            SendMessageW(s_hTrkSize, TBM_SETPOS, TRUE, 5);            // 默认笔宽 5 像素（介于细和粗之间，最常用）
            SendMessageW(s_hTrkSize, TBM_SETTICFREQ, 10, 0);          // 每 10 个单位打一个刻度点，方便用户估算数值
            s_hLblSize = CreateCtl(L"Static", L"", WS_VISIBLE, hWnd, IDC_DR_LBL_SIZE);   // 笔宽数值标签，文字由 UpdateSizeLabel() 填

            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_DR_SWATCH);
            // SS_OWNERDRAW = 这个 Static 不显示文字，而是发 WM_DRAWITEM 让父窗口自己画。
            // 这里用它来显示「当前画笔颜色」的小色块（见本文件 WM_DRAWITEM 分支）。
            s_hBtnColor = CreateCtl(L"Button", L"画笔颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_DR_BTN_COLOR);
            // 普通按钮：点击后弹系统颜色对话框（ChooseFillColor）。
            s_hBtnPick = CreateCtl(L"Button", L"吸色（点击画面取色）",
                                   WS_VISIBLE | WS_TABSTOP | BS_PUSHLIKE | BS_AUTOCHECKBOX,
                                   hWnd, IDC_DR_BTN_PICK);
            // BS_AUTOCHECKBOX = 复选按钮：可以「按下/弹起」两种状态，用它来表示「吸色模式开/关」。
            // BS_PUSHLIKE    = 让它画成普通按钮的样子（而不是带方框的复选框），
            //                  这样视觉上就是「一个开关按钮」，比复选框更符合直觉。
            // ⚠ 坑：程序里【不能】指望「用户点一下，勾选状态就自动同步」就完事 ——
            //    在吸色完成后我们要主动把按钮弹起来，必须用 Button_SetCheck(BST_UNCHECKED)
            //    来改；而读取状态要用 Button_GetCheck()。这两个宏内部实际发的是
            //    BM_SETCHECK / BM_GETCHECK 消息，是管理这种按钮状态的【正确】方式。
            s_hBtnUndo = CreateCtl(L"Button", L"撤销", WS_VISIBLE | WS_TABSTOP, hWnd, IDC_DR_BTN_UNDO);    // 撤销最后一笔
            s_hBtnClear = CreateCtl(L"Button", L"清空", WS_VISIBLE | WS_TABSTOP, hWnd, IDC_DR_BTN_CLEAR);  // 清空全部笔迹（会二次确认）

            s_hLblHint = CreateCtl(L"Static",
                                   L"用法：\r\n"
                                   L"· 在左侧图片上按住左键拖动即可涂鸦\r\n"
                                   L"· 点「吸色」后点击画面取色，取完自动恢复画笔\r\n"
                                   L"· 笔迹可逐笔撤销，输出时按原图分辨率合成",
                                   WS_VISIBLE | SS_LEFT, hWnd, IDC_DR_LBL_HINT);
            // 多行字符串的写法：C/C++ 会把相邻的 "..." 字面量自动拼接成一整串，
            // 所以这里用 4 行 L"..." 拼出一段带换行的提示。
            // \r\n 是 Windows 下标准的两字符换行（\r 回车 + \n 换行），只写 \n 在某些控件里换行会不整齐。
            // SS_LEFT = 文字左对齐（Static 默认就是左对齐，这里显式写出以免以后混淆）。

            s_hBtn = CreateCtl(L"Button", L"保存涂鸦并输出", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_DR_BTN_PROCESS);
            // BS_DEFPUSHBUTTON = 作为「默认按钮」：用户按回车时等价于点它，符合「填完就确认」的习惯。

            UpdateSizeLabel();   // 把「画笔大小：5 像素」写进标签，和刚设的默认滑块位置保持一致
            ApplyBrush();        // 把默认颜色和默认粗细下发给画布
            UpdateUndoButton();  // 刚建好时一笔都没有，撤销/清空按钮应处于变灰状态
            Layout();            // 第一次排版（此时客户区大小已经确定）
        }
        return 0;   // 返回 0 表示「创建成功，继续」

    case WM_SIZE:
        // 页面窗口尺寸变化（拉伸窗口、切换显示时都可能触发）。
        Layout();   // 重新计算所有控件的位置和大小
        return 0;

    case WM_HSCROLL:
        // 滚动条类控件（这里是 Trackbar 滑块）发出通知时会进到这里。
        // lParam 是发通知的控件句柄，wParam 低 16 位是通知码（TB_THUMBTRACK 等）。
        if ((HWND)lParam == s_hTrkSize)   // 只关心笔宽滑块（本页目前也只有这一个滑块）
        {
            UpdateSizeLabel();   // 先刷新「画笔大小：N 像素」的显示
            ApplyBrush();        // 再把新笔宽下发给画布
        }
        return 0;
        // 注意：这里返回 0 而不是 break，所以滑块自身的默认处理（画滑块、移动拇指）
        // 已由系统在发通知前完成，我们只做附加处理，不影响控件外观。

    case WM_APP_IMAGE_CHANGED:
        // 主窗口打开了新图片（或关闭了图片）后广播的通知，见 App.h。
        DrawCanvas_RefreshImage(s_hCanvas);   // 让画布丢掉旧底图缓存、重新按新图建立显示缓存
        UpdateUndoButton();                   // 新图 = 没有笔迹（笔迹属于画布内部状态，会随刷新清空），按钮应变灰
        return 0;

    case WM_APP_RECT_CHANGED:
        // 画布发来的「笔迹集合变了」通知（收笔、撤销之后都会发）。
        UpdateUndoButton();   // 只需刷新按钮可用性
        return 0;
        // 这里复用了裁切画布的同一个消息编号（App.h 里定义为 WM_APP + 3），
        // 含义是「画布内容变化了」；涂鸦页只关心按钮状态，不需要读 wParam/lParam。

    case WM_APP_PICKED_COLOR:
        // 画布吸色成功后发来的通知：吸到的颜色打包在 wParam 里。
        {
            s_brushColor = (COLORREF)wParam;                       // 把吸到的颜色记为本页当前笔色
            Button_SetCheck(s_hBtnPick, BST_UNCHECKED);            // 【关键】主动把「吸色」按钮弹起，否则外观还停在按下状态
            DrawCanvas_SetPickMode(s_hCanvas, false);              // 同时关掉画布的吸色模式（画布内部也会自己关一次，双保险）
            ApplyBrush();                                          // 用新颜色重新下发画笔设置（粗细不变）
            InvalidateRect(s_hSwatch, nullptr, TRUE);              // 请求重画色块；TRUE = 连背景一起擦，保证颜色干净
            wchar_t buf[96];                                       // 装「已吸取颜色：R% G% B%」这句话
            swprintf_s(buf, L"已吸取颜色：R%d G%d B%d",
                       GetRValue(s_brushColor), GetGValue(s_brushColor), GetBValue(s_brushColor));
            // COLORREF 在内存里是 0x00BBGGRR（注意是 BGR 顺序），
            // 所以必须用 GetRValue / GetGValue / GetBValue 宏来取分量，直接位移取会拿反颜色。
            SetAppStatus(buf);                                     // 把这句提示显示到底部状态栏，让用户确认吸到了什么色
        }
        return 0;

    case WM_DRAWITEM:
        // 自绘控件（带 SS_OWNERDRAW / BS_OWNERDRAW 的控件）要求父窗口把它画出来。
        // wParam = 控件 ID，lParam = DRAWITEMSTRUCT*（里面有 HDC、矩形区域、状态等）。
        if (wParam == IDC_DR_SWATCH && lParam)   // 只处理「颜色小方块」，并且指针有效才继续
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;   // 把 lParam 还原成结构体指针
            PaintColorSwatch(dis->hDC, dis->rcItem, s_brushColor, false);   // 在指定 DC、指定矩形里画一块实心颜色
            // 最后一个参数 false = 不是「透明」样式（透明样式会画成灰白格子，用在裁切页的透明填充上）。
            return TRUE;   // 返回 TRUE = 告诉系统「这个控件我已经画好了」，不要再按默认方式画
        }
        break;   // 不是我们的色块（理论上不会发生），交给默认处理
        // 坑：DRAWITEMSTRUCT 里的 hDC 是受系统管理的，画完不要 DeleteDC；
        //     也不要保存这个指针，它只在本次 WM_DRAWITEM 处理期间有效。

    case WM_COMMAND:
        // 控件被点击 / 通知时进入这里。
        // wParam 低 16 位 = 控件 ID（就是我们创建控件时传进去的 IDC_DR_*），
        //        高 16 位 = 通知码（按钮一般是 BN_CLICKED）；
        // lParam    = 发消息的控件句柄（菜单命令时是 0）。
        {
            const int id = LOWORD(wParam);   // 只取控件 ID，用低 16 位
            if (id == IDC_DR_BTN_PROCESS) { OnProcess(hWnd); return 0; }   // 「保存涂鸦并输出」：走完整输出流程
            if (id == IDC_DR_BTN_COLOR)
            {
                const COLORREF c = ChooseFillColor(hWnd, s_brushColor);   // 弹系统颜色对话框，初始色 = 当前笔色
                if (c != s_brushColor)   // 用户真选了别的颜色才更新（取消时返回原色，就不会做无谓的刷新）
                {
                    s_brushColor = c;                            // 记住新颜色
                    ApplyBrush();                                // 下发给画布
                    InvalidateRect(s_hSwatch, nullptr, TRUE);    // 重画色块，让用户看到颜色变了
                }
                return 0;
            }
            if (id == IDC_DR_BTN_PICK)
            {
                const bool on = (Button_GetCheck(s_hBtnPick) == BST_CHECKED);   // 读按钮当前勾选状态 = 是否要进吸色模式
                // ⚠ 一定要用 Button_GetCheck() 读，而不是自己另存一个 bool：
                //    按钮是 BS_AUTOCHECKBOX，它的勾选状态由控件自己维护（所以叫 AUTO），
                //    以控件状态为唯一真相来源，才不会出现「界面显示按下、逻辑却是关」的错位。
                if (on) ApplyBrush();                                   // 进入吸色前先把画笔参数刷一遍（保证吸色前的颜色是最新的）
                DrawCanvas_SetPickMode(s_hCanvas, on);                  // 通知画布：接下来点击是取色还是画线
                SetAppStatus(on ? L"吸色模式：点击左侧图片取色。" : L"已回到画笔模式。");   // 状态栏给出当前模式反馈
                return 0;
            }
            if (id == IDC_DR_BTN_UNDO)
            {
                DrawCanvas_Undo(s_hCanvas);   // 撤销最后一笔（画布内部会弹掉并整层重绘）
                UpdateUndoButton();           // 撤销后可能已经一笔不剩了，刷新按钮可用性
                return 0;
            }
            if (id == IDC_DR_BTN_CLEAR)
            {
                if (DrawCanvas_CanUndo(s_hCanvas) &&   // 有内容才值得问（没有内容就不弹框，避免打扰）
                    MessageBoxW(hWnd, L"确定要清空所有笔迹吗？", L"提示",
                                MB_YESNO | MB_ICONQUESTION) == IDYES)   // 清空不可撤销，必须二次确认
                {
                    DrawCanvas_Clear(s_hCanvas);   // 清空画布上的全部笔迹
                    UpdateUndoButton();            // 清空后按钮应变灰
                }
                return 0;
                // 坑：这里的 && 有「短路求值」特性 —— 如果前面为假（没内容），
                //     后面的 MessageBoxW 根本不会执行，所以不会白弹一个框。
            }
        }
        break;   // 未识别的命令交给默认处理

    default:
        break;   // 其它所有消息都走默认处理
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
    // 未处理的消息【必须】交给系统默认窗口过程，否则窗口的拖动、激活、最小化、
    // 销毁等一大堆基础行为都会失效。这一句是窗口过程的「兜底」，绝对不能省。
}

HWND PageDraw_Create(HWND parent)
{
    static bool registered = false;   // 静态局部变量：整个程序运行期间只初始化一次，用来记住「窗口类注册过没有」
    if (!registered)   // 只有第一次调用才需要注册窗口类
    {
        WNDCLASSEXW wc = {};   // ⚠ 必须清零！结构体字段很多，未填的字段若是栈上垃圾值，注册会莫名失败
        wc.cbSize = sizeof(wc);                                    // 结构体大小，系统靠它识别结构体版本
        wc.style = CS_HREDRAW | CS_VREDRAW;                        // 宽或高变化时整个窗口重绘（本页控件是自适应布局的，整块重画最省心）
        wc.lpfnWndProc = Proc;                                     // 指定窗口过程（就是上面那个 Proc）
        wc.hInstance = g_hInst;                                    // 本程序实例句柄
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);              // 默认光标用系统箭头（画布自己有十字光标）
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);             // 背景用系统「窗口色」画刷（(HBRUSH)宏是系统约定的写法）
        wc.lpszClassName = L"Pic246PageDraw";                      // 本页窗口类名，必须唯一，避免和其它页撞名
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;   // 注册失败且不是「类已存在」→ 真失败，返回空表示建不出来
        // 为什么要容忍 ERROR_CLASS_ALREADY_EXISTS：
        //   注册窗口类是「全程序一次性」的操作；万一别处先注册了同名类，
        //   这里报「已存在」并不影响使用，没必要当成失败。
        registered = true;   // 记下已注册，下次直接跳过
    }

    return CreateWindowExW(0, L"Pic246PageDraw", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
    // 创建页面窗口：
    //   扩展样式 0（页面本身不需要特殊外观）。
    //   WS_CHILD        = 作为子窗口挂在主窗口里（不是独立顶层窗口）。
    //   WS_CLIPCHILDREN = 绘制父窗口时跳过被子窗口盖住的区域，减少闪烁。
    //   位置尺寸 0,0,10,10 只是占位，真正大小由主窗口用 MoveWindow 决定。
    //   控件 ID 传 nullptr：页面窗口本身不需要 ID（需要 ID 的是它内部的那些控件）。
    //   ⚠ 注意：这里【没有】WS_VISIBLE —— 新建的页面初始是隐藏的，
    //      7 个页面都显示会互相重叠，主窗口会在用户点侧边栏时再 ShowWindow 切换。
}
