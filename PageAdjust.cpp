// ============================================================================
//  【文件说明】PageAdjust.cpp —— Pic246 的「图片调整」功能页
// ============================================================================
//  一、这个页面是干什么的？
//    这是主程序左侧边栏里的第 3 个页面（在 Project001.cpp 里由
//    PageAdjust_Create(hWnd) 创建，存进 g_pages[2]）。页面提供三个滑块：
//        · 模糊    blur        0 .. 40
//        · 明度    brightness -100 .. 100
//        · 饱和度  saturation -100 .. 100
//    用户拖动任意一个滑块，右侧预览框立刻显示调整后的效果（实时预览）；
//    点「处理」按钮才用【原图尺寸】真正算一遍，并另存为「原名_adjust.后缀」，
//    所以本页永远不会覆盖用户的原文件。
//
//  二、它在整个程序里的位置
//    · 主窗口在 Project001.cpp：顶栏（选择图片 / 显示路径）、左侧边栏切页、
//      底部状态栏。切页其实就是 ShowWindow 各页窗口，谁被选中就显示谁。
//    · 本文件只负责"这一页自己"：创建控件、排版、收消息，
//      真正的图像运算全部外包给 ImageOps.cpp（本文件一行算法都不写）。
//    · 页面本身是一个自定义窗口类 "Pic246PageAdjust"（见文件末尾
//      PageAdjust_Create），以 WS_CHILD 挂到主窗口客户区里。
//
//  三、与 App.h / ImageOps.cpp 的配合关系
//    来自 App.h 的东西：
//      g_pSrcImage / g_srcW / g_srcH —— 全程序唯一的一份源图与它的尺寸；
//      g_hMain / g_hInst            —— 主窗口句柄、程序实例句柄；
//      g_dpi + D(dip)               —— 把逻辑像素(DIP)换算成当前 DPI 的物理像素；
//      SetAppStatus(text)           —— 在底部状态栏写一行提示文字；
//      CreateCtl(cls,text,style,parent,id) —— 统一封装好的"建子控件"辅助函数；
//      IDC_AD_*                     —— 本页所有控件的 ID 枚举（在 App.h 里定义）；
//      WM_APP_IMAGE_CHANGED         —— 主窗口"源图已更换"的自定义广播消息；
//      AdjustParams                 —— { blur, brightness, saturation } 参数打包。
//    来自 ImageOps.cpp 的 4 个函数（本页的图像能力全靠它们）：
//      · CreatePreviewSource(maxDim)   生成"预览用小图"，最长边不超过 700 像素。
//        为什么需要它？原图可能 8000x6000，每帧把大图缩进预览框界面会卡死；
//        先缩成一张小图再画，界面就流畅了。
//      · ApplyAdjustments(bmp, p)      按参数在【原地】做模糊/明度/饱和度调整。
//      · PaintImagePreview(hdc,rc,...) 在预览框里画灰白格子底 + 等比居中画图；
//        传入的 img 为空时会改为显示提示文字。
//      · SaveBitmapWithSuffix(bmp,...) 弹"另存为"对话框保存，并且【内部会 delete bmp】。
//
//  四、消息驱动的整体流程（窗口过程 Proc，见本文件中后部）
//    WM_CREATE            页面第一次创建：建全部子控件、设置滑块范围与刻度、
//                         初始化数字标签文字、排一次版。
//    WM_SIZE              主窗口改变大小：重新 Layout()，并让预览框重画。
//    WM_HSCROLL           滑块被拖动（或键盘左右键操作）：刷新数字标签 + 重算预览。
//    WM_APP_IMAGE_CHANGED 主窗口广播"源图换了"：重建预览小图 + 重算预览。
//    WM_DRAWITEM          自绘的预览框要求重画：转交给 PaintImagePreview。
//    WM_COMMAND           按钮点击：重置滑块 / 执行处理并保存。
//    WM_TIMER             本页【没有】使用定时器，这是有意为之：
//                         预览图最长边只有 700 像素，ApplyAdjustments 跑一遍很快，
//                         拖滑块时直接算就足够了，不需要"防抖"。
//                         如果将来预览变卡（例如把预览图放大到 2000 像素），
//                         标准做法是：在 WM_HSCROLL 里 KillTimer + SetTimer(约 30~80ms)，
//                         等用户停下拖动、定时器只触发一次后再重算，避免滑块每移动
//                         一个像素就把整张图重算一遍。切记页面销毁时要 KillTimer。
// ---------------------------------------------------------------------------
// ============================================================================
//  PageAdjust.cpp —— 功能页：图片调整（模糊 / 明度 / 饱和度，实时预览）
// ============================================================================
#include "App.h"    // 本程序唯一的公共头：全局变量、D()、CreateCtl()、AdjustParams、IDC_AD_* 等

using namespace Gdiplus;    // GDI+ 的对象都在 Gdiplus 命名空间里，using 之后可以少写前缀

// ---- 本页需要长期记住的窗口句柄 -------------------------------------------
// 这些句柄在 WM_CREATE 里赋值，之后在 WM_SIZE / WM_HSCROLL / WM_COMMAND / 自绘
// 里反复使用，所以必须存成"文件级 static"（只在本 .cpp 内可见，别的页面看不到），
// 而不能是局部变量。初始值统一为 nullptr，便于判断"窗口还没建好"。
static HWND s_hPage = nullptr;          // 本页的"页面窗口"自身（Layout 要靠它取客户区大小）
static HWND s_hPreview = nullptr;       // 预览框：Static 控件 + SS_OWNERDRAW（自己画内容）
static HWND s_hGrp = nullptr;           // 参数区外框（Button 的 BS_GROUPBOX 样式）
static HWND s_hTrkBlur = nullptr;       // 模糊滑块（msctls_trackbar32）
static HWND s_hTrkBright = nullptr;     // 明度滑块
static HWND s_hTrkSat = nullptr;        // 饱和度滑块
static HWND s_hLblBlur = nullptr;       // "模糊"两个字（文字标签）
static HWND s_hLblBright = nullptr;     // "明度"文字标签
static HWND s_hLblSat = nullptr;        // "饱和度"文字标签
static HWND s_hValBlur = nullptr;       // 模糊当前数值（跟着滑块变的数字）
static HWND s_hValBright = nullptr;     // 明度当前数值
static HWND s_hValSat = nullptr;        // 饱和度当前数值
static HWND s_hBtnReset = nullptr;      // "重置参数"按钮：把三个滑块都归零
static HWND s_hBtnProcess = nullptr;    // "处理"按钮：按原图尺寸算一遍并另存

static Bitmap* s_pPrevSrc = nullptr;   // 预览用原图（缩小）
static Bitmap* s_pPrevDst = nullptr;   // 预览结果

// ---------------------------------------------------------------------------
// ReadTrackbars：把三个滑块当前的位置读出来，打包成一个 AdjustParams。
//   参数：无（三个滑块句柄用文件级 static 变量 s_hTrk*，属于"隐式输入"）
//   返回：AdjustParams{ blur, brightness, saturation }，数值就是滑块上的整数
//   说明：TBM_GETPOS 是 trackbar（滑块）控件的标准消息，用来取当前位置；
//         它的返回值类型是 LRESULT（本质是整数），所以这里 (int) 强转一下。
//   坑：本函数只"读"不"写"，拿到的值一定落在 WM_CREATE 里设定的范围内
//       （模糊 0..40，明度/饱和度 -100..100），所以调用方可以放心使用，
//       不需要再做范围检查。
// ---------------------------------------------------------------------------
static AdjustParams ReadTrackbars()
{
    AdjustParams p;                                                     // 在栈上建一个结构体，三个字段马上就会被覆盖
    p.blur = (int)SendMessageW(s_hTrkBlur, TBM_GETPOS, 0, 0);           // wParam/lParam 对 TBM_GETPOS 无意义，固定传 0
    p.brightness = (int)SendMessageW(s_hTrkBright, TBM_GETPOS, 0, 0);
    p.saturation = (int)SendMessageW(s_hTrkSat, TBM_GETPOS, 0, 0);
    return p;                                                           // 按值返回；结构体只有 3 个 int，代价极小
}

// ---------------------------------------------------------------------------
// PreviewParams：在 ReadTrackbars() 的基础上，把"模糊半径"按预览缩放比例换算。
//   参数：无
//   返回：换算后的 AdjustParams（明度/饱和度原样不动）
//   背景：滑块上的模糊半径是按【原图】像素为单位设计的（例如原图 8000 宽，
//         模糊 40 表示 40 像素）。但预览图被缩小到了 700 像素，如果还按 40
//         去模糊，在预览里看起来会糊得多。所以要乘上"预览/原图"的比例，
//         让预览效果和最终输出效果保持一致。
//   注意：不能简单四舍五入——若用户确实拖了>0（p.blur>0）而换算后不足 1 像素，
//         仍然强制取 1，否则"轻轻拖一点却没反应"，用户会以为滑块坏了。
// ---------------------------------------------------------------------------
// 预览图比原图小，模糊半径要按比例换算，这样预览和最终输出效果一致
static AdjustParams PreviewParams()
{
    AdjustParams p = ReadTrackbars();
    if (g_pSrcImage && s_pPrevSrc && g_srcW > 0)                        // 三重保险：有源图、有预览图、宽度非 0（避免除零）
    {
        float scale = (float)s_pPrevSrc->GetWidth() / (float)g_srcW;    // 缩放比例，例如 700/8000 = 0.0875
        int b = (int)((float)p.blur * scale + 0.5f);                    // +0.5f 是"四舍五入"的常用写法
        if (p.blur > 0 && b < 1) b = 1;                                 // 用户确实想要模糊时，至少给 1 像素
        p.blur = b;
    }
    return p;
}

// ---------------------------------------------------------------------------
// UpdateValueLabels：把三个滑块的当前值写进右边的数字标签（s_hVal*）。
//   参数：无
//   返回：无
//   说明：swprintf_s 是"带缓冲区长度检查"的安全版格式化函数（见 <stdio.h>），
//         比老的 sprintf 安全，不会写越界。buf[64] 足够放下任意 int。
//   注意：这里只改标签文字，【不】触发预览重算；要不要重算由调用方决定，
//         避免同一次操作里把预览算两遍。
// ---------------------------------------------------------------------------
static void UpdateValueLabels()
{
    wchar_t buf[64];                                                    // 栈上小缓冲区；%d 最多 11 位，64 个 wchar 绰绰有余
    swprintf_s(buf, L"%d", (int)SendMessageW(s_hTrkBlur, TBM_GETPOS, 0, 0));
    SetWindowTextW(s_hValBlur, buf);                                    // 把字符串写进 Static 控件的标题
    swprintf_s(buf, L"%d", (int)SendMessageW(s_hTrkBright, TBM_GETPOS, 0, 0));   // 同一个 buf 复用，覆盖上一份内容
    SetWindowTextW(s_hValBright, buf);
    swprintf_s(buf, L"%d", (int)SendMessageW(s_hTrkSat, TBM_GETPOS, 0, 0));
    SetWindowTextW(s_hValSat, buf);
}

// ---------------------------------------------------------------------------
// RebuildPreview：源图变化后，重新建立"预览用原图"和"预览结果图"两张离屏位图。
//   参数：无
//   返回：无
//   步骤：1) 先把旧的两张 delete 掉，并将指针置空——否则每换一次图就泄漏一块内存；
//         2) 没有源图就直接返回，两张指针保持 nullptr，绘制时会显示提示文字；
//         3) CreatePreviewSource(700) 生成一张最长边不超过 700 的小图；
//         4) 再 new 一张同样大小的空位图，作为"结果画布"。
//   坑（GDI+ 最经典的坑之一）：new Bitmap(w,h,fmt) 即便失败也【不会返回 nullptr】，
//       而是返回一个状态无效的对象。必须用 GetLastStatus() != Ok 来判断，
//       并且失败时要自己 delete 掉它，否则后面拿它绘图会一直静默失败。
//   所有权：s_pPrevSrc / s_pPrevDst 都由本文件负责释放（换图时和页面销毁时）。
// ---------------------------------------------------------------------------
static void RebuildPreview()
{
    if (s_pPrevSrc) { delete s_pPrevSrc; s_pPrevSrc = nullptr; }        // delete 之后立刻置空，防止变成"野指针"
    if (s_pPrevDst) { delete s_pPrevDst; s_pPrevDst = nullptr; }

    if (!g_pSrcImage) return;                                           // 还没打开图片：到此为止，预览框会显示提示文字

    s_pPrevSrc = CreatePreviewSource(700);                              // 700 = 预览图最长边像素数，与下方布局里的预览框尺寸相匹配
    if (!s_pPrevSrc) return;                                            // 缩图失败（内存不足等），保持两张都是空指针

    s_pPrevDst = new Bitmap(s_pPrevSrc->GetWidth(), s_pPrevSrc->GetHeight(), PixelFormat32bppARGB);   // 结果图与原图等大，带 alpha 通道
    if (s_pPrevDst->GetLastStatus() != Ok)                              // 注意：不能靠 == nullptr 判断失败
    {
        delete s_pPrevDst;
        s_pPrevDst = nullptr;                                           // 置空后 UpdatePreview 会走"直接请求重画"的分支
    }
}

// ---------------------------------------------------------------------------
// UpdatePreview：按当前滑块参数重算一遍预览结果，并请求系统重画预览框。
//   参数：无
//   返回：无
//   步骤：1) 如果没有预览图，就只请求重画（此时 PaintImagePreview 会显示提示文字）；
//         2) 用 Graphics 把"预览原图"整张复制到"结果图"上；
//         3) 调 ApplyAdjustments 在结果图上原地调整；
//         4) InvalidateRect 请求系统重画预览框。
//   为什么用一对大括号 { } 把 Graphics g 包起来？
//       GDI+ 的 Graphics 是在【析构时】才真正提交绘图操作、释放对目标位图的占用。
//       必须让 g 在调用 ApplyAdjustments 之前就析构掉，否则 Graphics 与后面的
//       LockBits 会同时占着同一张位图，导致加锁失败（画面不变）。
//   坑：SetCompositingMode(CompositingModeSourceCopy) 表示"直接覆盖"，
//       而不是默认的"按 alpha 混合"。不设它的话，源图的透明像素会和结果图里
//       上一轮的旧内容混在一起，预览会越拖越花。
//   性能：InvalidateRect 的第三个参数 TRUE 表示"连背景一起擦掉再重画"，
//       对预览区这种内容变化很大的地方是合适的做法。
// ---------------------------------------------------------------------------
static void UpdatePreview()
{
    if (!s_pPrevSrc || !s_pPrevDst)                                     // 没有预览位图（还没选图，或缩图失败）
    {
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);      // nullptr = 整个客户区；让提示文字显示出来
        return;
    }

    {
        Graphics g(s_pPrevDst);                                         // 把结果图当作画布；出作用域即析构
        g.SetCompositingMode(CompositingModeSourceCopy);                // 覆盖模式，避免与上一轮内容混合
        g.DrawImage(s_pPrevSrc, 0, 0, s_pPrevSrc->GetWidth(), s_pPrevSrc->GetHeight());   // 整张原样铺满（尺寸相同，不缩放）
    }

    ApplyAdjustments(s_pPrevDst, PreviewParams());                      // 真正干活：模糊/明度/饱和度，在原地修改

    if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);          // 请求重画，稍后会收到 WM_DRAWITEM
}

// ---------------------------------------------------------------------------
// Layout：按当前客户区大小，重新摆放本页所有控件的位置和大小（只摆放，不绘制）。
//   参数：无
//   返回：无
//   何时调用：WM_CREATE 建完控件之后排一次；WM_SIZE 每次窗口大小改变时再排一次。
//   坐标体系：所有尺寸都写成 D(...)，把"逻辑像素(DIP)"换算成当前 DPI 下的真实像素，
//       这样在 125% / 150% 缩放的屏幕上，间距才不会显得又小又挤。
//   布局思路：左边是预览框（占满剩下的宽度），右边是固定宽度的参数面板。
//   坑：MoveWindow 的最后一个参数 TRUE 表示"移动后立即重画"；控件只有十来个，
//       直接用 TRUE 最省事，传 FALSE 有可能留下移动前的残影。
// ---------------------------------------------------------------------------
static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);                                        // 取本页客户区矩形；rc.left/rc.top 通常为 0
    const int cx = rc.right;                                            // 客户区宽度（像素）
    const int cy = rc.bottom;                                           // 客户区高度（像素）
    const int M = D(12);                                                // 统一外边距：12 逻辑像素 → 当前 DPI 下的实际像素

    int panelW = D(330);                                                // 右侧参数面板的理想宽度
    if (panelW > cx - D(160)) panelW = cx - D(160);                     // 窗口很窄时先保面板；但至少给预览留 160 像素
    if (panelW < D(220)) panelW = D(220);                               // 面板也不能太窄，否则滑块拖不动

    int prevW = cx - panelW - M * 3;                                    // 预览框宽度 = 总宽 - 面板 - 3 个间隙（含面板与预览之间的间隔）
    if (prevW < D(120)) prevW = D(120);                                 // 兜底：预览框至少 120 像素宽
    MoveWindow(s_hPreview, M, M, prevW, cy - M * 2, TRUE);              // 预览框：左边距 M，上下各留 M，占满剩下的高度

    int px = M * 2 + prevW;                                             // 参数面板的左上角 x：预览宽度 + 两个边距
    int pw = cx - px - M;                                               // 面板宽度（用实际剩余宽度算，保证右边也留 M 的边距）
    int py = M;                                                         // 面板起始 y：与预览框顶部对齐

    int grpH = D(232);                                                  // 分组框高度：3 行控件（每行 60）+ 上下留白，刚好装下
    MoveWindow(s_hGrp, px, py, pw, grpH, TRUE);                         // "调整参数"外框

    HWND tracks[3] = { s_hTrkBlur, s_hTrkBright, s_hTrkSat };           // 用数组 + 循环统一摆放，避免三段几乎一样的代码
    HWND labels[3] = { s_hLblBlur, s_hLblBright, s_hLblSat };
    HWND values[3] = { s_hValBlur, s_hValBright, s_hValSat };

    for (int i = 0; i < 3; ++i)
    {
        int y = py + D(32) + i * D(60);                                 // 每行间隔 60 逻辑像素；首行从分组框顶部下移 32，给标题留位置
        MoveWindow(labels[i], px + D(16), y + D(4), D(58), D(22), TRUE);            // 左侧中文标签，+4 是为了和滑块在视觉上对齐
        MoveWindow(tracks[i], px + D(78), y, pw - D(150), D(30), TRUE);             // 滑块：右侧还要给数字标签留 150 逻辑像素
        MoveWindow(values[i], px + pw - D(66), y + D(4), D(50), D(22), TRUE);       // 数字标签靠右，与滑块同高
    }

    py += grpH + D(10);                                                 // 分组框下方留 10 像素，再放"重置参数"按钮
    MoveWindow(s_hBtnReset, px, py, pw, D(34), TRUE);                   // 重置按钮：与面板同宽，高度 34

    int btnH = D(42);                                                   // 「处理」按钮做得更高一些，突出它是本页的主操作
    MoveWindow(s_hBtnProcess, px, cy - M - btnH, pw, btnH, TRUE);       // 贴在本页底部（下面留 M 的边距）
}

// ---------------------------------------------------------------------------
// OnProcess：真正执行"处理并保存"，由 WM_COMMAND 里点击「处理」按钮触发。
//   参数 hPage：本页窗口句柄，仅用于作为弹框的父窗口（让对话框居中显示在本页上）
//   返回：无——成功或失败都通过弹框 + 底部状态栏告知用户
//   流程：1) 还没载入图片 → 提示并直接返回；
//         2) 读滑块参数；三个值都是 0 时先问一句"仍要输出副本吗"，防止误点；
//         3) 在底部状态栏显示"正在处理"，并用 UpdateWindow(g_hMain) 强制立刻重绘，
//            否则界面会停在旧画面上，用户会以为程序卡死了；
//         4) new 一张与原图等大的位图 → 把原图整张画进去 → ApplyAdjustments；
//         5) SaveBitmapWithSuffix(work, L"_adjust", ...) 另存为"原名_adjust.后缀"。
//   所有权：work 交给 SaveBitmapWithSuffix，由【它】负责 delete；
//       这里绝对不能再 delete 一次，否则会 double free（重复释放）直接崩溃。
//   坑：为什么不直接在 g_pSrcImage 上改？
//       g_pSrcImage 是全局共享的唯一源图，所有页面和画布都在用它。
//       就地修改会污染其它页面的显示，而且「重置」也回不去了。
//       所以必须先拷进一张自己的 Bitmap，改自己的副本。
//   注意：g_pSrcImage 的类型是 Image*，而保存接口要的是 Bitmap*，
//       这也是必须先拷贝成 Bitmap 的原因之一。
// ---------------------------------------------------------------------------
static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)                                                   // 还没打开任何图片
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",   // 用 W 版接口，直接支持中文
                    MB_OK | MB_ICONINFORMATION);                        // 提示图标，不是错误
        return;
    }

    const AdjustParams p = ReadTrackbars();                             // 注意：这里用【原图】参数，不做预览缩放换算
    if (p.blur == 0 && p.brightness == 0 && p.saturation == 0)          // 三个参数全为 0，等于什么都没调
    {
        if (MessageBoxW(hPage, L"当前没有任何调整（模糊/明度/饱和度都是 0）。\r\n\r\n仍要输出一张副本吗？",
                        L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)  // 用户点了"否"就中止
            return;
    }

    SetAppStatus(L"正在处理，请稍候...");                                // 底部状态栏给一句反馈，让用户知道程序在忙
    UpdateWindow(g_hMain);                                              // 强制主窗口立即重绘一次，把上面那句状态文字真正画出来

    Bitmap* work = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);    // 按【原图】尺寸新建结果位图（不是预览小图）
    if (work->GetLastStatus() != Ok)                                    // GDI+ 构造失败不会返回 nullptr，必须查状态
    {
        delete work;                                                    // 失败时自己回收，避免泄漏
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    {
        Graphics g(work);                                               // 同样的原因：用作用域让 g 及时析构
        g.SetCompositingMode(CompositingModeSourceCopy);                // 覆盖模式，保证像素是"原图的干净副本"
        g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);       // g_srcW/H 是 UINT，接口要 INT，显式转换
    }

    ApplyAdjustments(work, p);                                          // 在副本上执行模糊/明度/饱和度

    SaveBitmapWithSuffix(work, L"_adjust", hPage, nullptr);             // 内部会 delete work；nullptr 表示不需要回传保存路径
}

// ---------------------------------------------------------------------------
// Proc：本页的窗口过程（WndProc）。所有发给本页窗口的消息都从这里进来。
//   参数 hWnd   ：收到消息的窗口（这里就是页面窗口）
//   参数 message：消息编号，如 WM_CREATE / WM_SIZE / WM_HSCROLL ...
//   参数 wParam / lParam：消息附带的两个参数，含义随 message 变化
//   返回：LRESULT——消息的处理结果；自己没处理的消息必须交给 DefWindowProcW，
//       否则窗口会失去系统默认行为（如无法正常销毁）。
//   设计说明：本函数只是个"调度中心"，每个 case 里只做几件事，
//       真正的算法都在 ImageOps.cpp，真正的排版在 Layout()。
// ---------------------------------------------------------------------------
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    // 页面第一次创建时到达，且【只到达一次】。在这里把全部子控件建出来并排好版。
    // 返回 0 表示"创建允许继续"；若返回 -1 系统会认为创建失败、销毁窗口。
    case WM_CREATE:
        {
            s_hPage = hWnd;                                             // 先记下自己的句柄，Layout() 要用它取客户区大小

            // 预览区：用 Static 控件 + SS_OWNERDRAW（自绘），内容全由我们自己在 WM_DRAWITEM 里画
            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_AD_PREVIEW);
            // 参数区外框：Button 控件的 BS_GROUPBOX 样式，画一个带标题的矩形框
            s_hGrp = CreateCtl(L"Button", L"调整参数", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_AD_GRP);

            // 三个中文文字标签（Static 控件默认就是左对齐显示文字）
            s_hLblBlur = CreateCtl(L"Static", L"模糊", WS_VISIBLE, hWnd, IDC_AD_LBL_BLUR);
            s_hLblBright = CreateCtl(L"Static", L"明度", WS_VISIBLE, hWnd, IDC_AD_LBL_BRIGHT);
            s_hLblSat = CreateCtl(L"Static", L"饱和度", WS_VISIBLE, hWnd, IDC_AD_LBL_SAT);

            // 三个滑块。msctls_trackbar32 是系统内置的 trackbar（滑块）控件类名；
            // WS_TABSTOP 让它们能通过 Tab 键切换焦点（键盘操作也就能用）；
            // TBS_HORZ 表示水平方向。
            s_hTrkBlur = CreateCtl(L"msctls_trackbar32", L"",
                                   WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_AD_TRK_BLUR);
            s_hTrkBright = CreateCtl(L"msctls_trackbar32", L"",
                                     WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_AD_TRK_BRIGHT);
            s_hTrkSat = CreateCtl(L"msctls_trackbar32", L"",
                                  WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_AD_TRK_SAT);

            // TBM_SETRANGE：设置滑块取值范围。wParam=TRUE 表示设置完立即重绘。
            // MAKELPARAM(最小值, 最大值) 把两个 16 位值打包进一个 32 位参数
            // （低 16 位放最小值，高 16 位放最大值；负数也能正确装进去）。
            SendMessageW(s_hTrkBlur, TBM_SETRANGE, TRUE, MAKELPARAM(0, 40));          // 模糊半径 0..40 像素：再大就糊成一片，没实用价值
            SendMessageW(s_hTrkBright, TBM_SETRANGE, TRUE, MAKELPARAM(-100, 100));    // 明度 -100..100：-100=全黑，0=不变，+100=亮度翻倍
            SendMessageW(s_hTrkSat, TBM_SETRANGE, TRUE, MAKELPARAM(-100, 100));       // 饱和度 -100..100：-100=灰度，0=不变，+100=饱和翻倍
            // TBM_SETTICFREQ：设置刻度线的间隔（单位是"值域单位"，不是像素）。
            // 模糊满量程只有 40，每 10 一根刻度；明度/饱和度满量程 200，每 20 一根。
            SendMessageW(s_hTrkBlur, TBM_SETTICFREQ, 10, 0);
            SendMessageW(s_hTrkBright, TBM_SETTICFREQ, 20, 0);
            SendMessageW(s_hTrkSat, TBM_SETTICFREQ, 20, 0);

            // 三个显示数值的标签，初值 "0"（与下面 TBM_SETRANGE... 的中间值一致）。
            // 控件 ID 传 -1 表示"不需要 ID"：它们不响应点击、不发通知，只是为了显示文字。
            s_hValBlur = CreateCtl(L"Static", L"0", WS_VISIBLE, hWnd, -1);
            s_hValBright = CreateCtl(L"Static", L"0", WS_VISIBLE, hWnd, -1);
            s_hValSat = CreateCtl(L"Static", L"0", WS_VISIBLE, hWnd, -1);

            // 两个按钮。BS_DEFPUSHBUTTON 让「处理」成为默认按钮（回车键即可触发），
            // 这也是用户在本页最常用的操作。
            s_hBtnReset = CreateCtl(L"Button", L"重置参数", WS_VISIBLE | WS_TABSTOP, hWnd, IDC_AD_BTN_RESET);
            s_hBtnProcess = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      hWnd, IDC_AD_BTN_PROCESS);

            UpdateValueLabels();                                        // 让数字标签与滑块初值保持一致（都显示 0）
            Layout();                                                   // 控件刚建出来位置还是 0,0，必须立刻排一次版
        }
        return 0;                                                       // 返回 0：告诉系统"创建成功，继续"

    // 窗口大小改变（主窗口调整尺寸、或切页时被拉大拉小）。
    // 重新排布所有控件，并让预览框重画（否则可能残留旧画面）。
    case WM_SIZE:
        Layout();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    // 自定义消息（定义在 App.h）：主窗口广播"源图换了"。
    // 本页缓存的预览小图已经过期，必须重建；然后立刻重算一次预览。
    // 注意：这里不检查 wParam/lParam，因为只需要知道"变了"这一件事。
    case WM_APP_IMAGE_CHANGED:
        RebuildPreview();                                               // 按新的源图重建预览原图 + 结果图
        UpdatePreview();                                                // 用当前滑块参数算出预览并请求重画
        return 0;

    // 滑块被拖动（或聚焦后按方向键）时，trackbar 会向父窗口发 WM_HSCROLL。
    // 这里不区分是哪一个滑块——三个滑块的响应动作完全一样，都是"刷新数字 + 重算预览"。
    // 这也是"实时预览"的实现点：滑块每动一下就算一次。
    case WM_HSCROLL:
        UpdateValueLabels();                                            // 先让右边的数字跟上
        UpdatePreview();                                                // 再重算预览图
        return 0;

    // 自绘消息：预览框是 SS_OWNERDRAW，需要显示内容时系统不会自己画，
    // 而是把绘制任务交给父窗口处理。
    //   wParam = 发出请求的控件 ID；lParam = DRAWITEMSTRUCT*（里面有 HDC、矩形、状态等）。
    // 返回 TRUE 表示"我已经画完了"，系统就不会再做默认处理。
    case WM_DRAWITEM:
        if (wParam == IDC_AD_PREVIEW && lParam)                         // 必须检查 ID：页面里别的自绘控件也会发 WM_DRAWITEM
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;  // 用 const 指针，明确"只看不改"
            // hDC 是系统给好的画布；rcItem 是该控件的矩形区域。
            // 最后一个参数是提示文字：当 s_pPrevDst 为空（还没选图）时会显示它。
            PaintImagePreview(dis->hDC, dis->rcItem, s_pPrevDst,
                              L"点击上方「选择图片...」载入图片\r\n然后拖动右侧滑块即可实时预览");
            return TRUE;                                                // 告诉系统"已处理完毕"
        }
        break;                                                          // 不是本页关心的自绘请求，交回默认处理

    // 控件通知：按钮点击、菜单等都会以 WM_COMMAND 形式送到这里。
    //   LOWORD(wParam) = 控件 ID；HIWORD(wParam) = 通知码（按钮点击是 BN_CLICKED）。
    // 这里只按 ID 分辨，不区分通知码——因为这些控件只发一种通知。
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        // 「重置参数」：把三个滑块都拨回 0，然后刷新显示。
        // 注意：用 SendMessageW(..., TBM_SETPOS, TRUE, 0) 主动设位置。
        //       程序性设置位置【不会】触发 WM_HSCROLL，所以后面必须自己调
        //       UpdateValueLabels() 和 UpdatePreview()，否则界面不会更新。
        case IDC_AD_BTN_RESET:
            SendMessageW(s_hTrkBlur, TBM_SETPOS, TRUE, 0);              // TRUE = 设置完立即重绘滑块本身
            SendMessageW(s_hTrkBright, TBM_SETPOS, TRUE, 0);
            SendMessageW(s_hTrkSat, TBM_SETPOS, TRUE, 0);
            UpdateValueLabels();
            UpdatePreview();
            return 0;                                                   // 已处理，返回 0

        // 「处理」：按原图尺寸真正算一遍并另存为副本。具体逻辑在 OnProcess 里。
        case IDC_AD_BTN_PROCESS:
            OnProcess(hWnd);
            return 0;

        default:
            break;                                                      // 不是本页认识的控件，往下走交回默认处理
        }
        break;

    default:
        break;
    }
    // 本页没有处理的消息交给系统默认过程：窗口移动、重绘、销毁、键盘等等
    // 都靠它。忘了这一句最常见的后果是窗口无法关闭。
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// PageAdjust_Create：创建本页窗口（首次调用时顺便注册窗口类）。
//   参数 parent：主窗口句柄，本页将作为它的子窗口
//   返回：页面窗口句柄；失败返回 nullptr（主窗口把它存进 g_pages[i]，
//       之后切页/广播时会跳过 nullptr，所以不会崩）
//   为什么要 static bool registered？
//       窗口类（WNDCLASS）全程序注册一次就够，重复注册会失败返回 0。
//       用函数内的静态变量记住"已经注册过"，第二次调用直接跳过注册。
//       （static 局部变量只初始化一次，且活到程序结束。）
//   坑：RegisterClassExW 返回 0 时要分清"真失败"和"这个类早就注册过了"
//       （错误码 ERROR_CLASS_ALREADY_EXISTS）。后者其实不算错误，
//       如果不放过它，离开本页再回来就创建不出窗口了。
//   说明：初始尺寸给 10x10 只是为了让窗口"先存在"，真正的大小由主窗口
//       随后的 MoveWindow / WM_SIZE 决定，用户看不到这个瞬时小窗。
// ---------------------------------------------------------------------------
HWND PageAdjust_Create(HWND parent)
{
    static bool registered = false;                                     // 局部静态变量：只在本函数可见，且只初始化一次
    if (!registered)
    {
        WNDCLASSEXW wc = {};                                            // 全部字段清零，避免垃圾值导致注册失败
        wc.cbSize = sizeof(wc);                                         // 必须填结构体大小，系统靠它判断结构版本
        wc.style = CS_HREDRAW | CS_VREDRAW;                             // 窗口宽度/高度变化时整窗重画，避免拉伸残影
        wc.lpfnWndProc = Proc;                                          // 本页的消息处理函数（就是上面那个 Proc）
        wc.hInstance = g_hInst;                                         // 程序实例句柄，注册窗口类必须提供
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);                   // 鼠标在页面上显示普通箭头
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);                  // 用系统窗口色作背景；(COLOR_WINDOW+1) 是"系统颜色"专用写法，
                                                                        // 这样不需要自己建画刷，也就没有"谁来释放"的问题
        wc.lpszClassName = L"Pic246PageAdjust";                         // 窗口类名，全程序唯一，其它页面各用自己的名字
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;                                             // 真正的注册失败：返回 nullptr 让主窗口知道本页没建起来
        registered = true;                                              // 记住已注册，下次调用不再重复注册
    }

    // 创建真正的页面窗口：
    // WS_CHILD        —— 它是主窗口的子窗口，不是独立窗口；
    // WS_CLIPCHILDREN —— 父窗口重画时不覆盖子控件占用的区域，能显著减少闪烁。
    // 后面 4 个数字是初始位置和大小(x=0,y=0,w=10,h=10)，马上就会被重新调整；
    // 父窗口 parent 与实例 g_hInst 必须传对，否则创建失败。
    return CreateWindowExW(0, L"Pic246PageAdjust", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
