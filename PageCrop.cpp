// ============================================================================
//  PageCrop.cpp —— 功能页：图片裁切
// ============================================================================
// ---------------------------------------------------------------------------
//  【文件说明】
//  本文件实现 Pic246 的“图片裁切”功能页，它是主窗口右侧内容区里的一个子窗口。
//
//  它负责什么：
//    1. 在页面中间放一块可交互的裁切画布（复用 CropCanvas.cpp 里的
//       Pic246CropCanvas 窗口类）。用户可以用鼠标拖动裁切框、拖 8 个控制点缩放；
//       裁切框允许被拖到图片外面，超出图片的部分按下面的“填充色 / 透明”处理。
//    2. 在页面右侧放一块控制面板：选择填充色、勾选是否透明底色、
//       显示当前裁切区域与起点坐标、以及“重置裁切框（铺满图片）”“居中正方形”两个按钮。
//    3. 点“处理”时，按裁切框从源图里抠出对应区域生成新位图，
//       并通过 ImageOps.cpp 的 SaveBitmapWithSuffix() 保存成
//       “原文件名_crop.原扩展名”（例如 photo.png → photo_crop.png）。
//
//  和哪些文件配合：
//    · App.h          —— 公共声明。里面有全局状态（g_pSrcImage / g_srcW / g_srcH /
//                        g_fillColor / g_hInst / g_hMain …）、本页控件 ID（IDC_CP_*）、
//                        自定义消息（WM_APP_IMAGE_CHANGED / WM_APP_RECT_CHANGED），
//                        以及 CropCanvas_*、SaveBitmapWithSuffix() 等函数的声明。
//                        另外 D() 这个 DPI 换算函数也是 App.h 里的 inline 函数。
//    · CropCanvas.cpp —— “裁切画布”子窗口的实现：真正负责画图、处理鼠标拖动，
//                        并在裁切框变化时主动通知本页。对外接口是 CropCanvas_* 系列。
//    · ImageOps.cpp   —— 图片加载/保存等通用实现：FormatSupportsAlpha()、
//                        ChooseFillColor()、PaintColorSwatch()、SaveBitmapWithSuffix()。
//    · Project001.cpp —— 主窗口：载入新图片后，通过 NotifyImageChanged() 向所有页面
//                        广播 WM_APP_IMAGE_CHANGED；CreateCtl()/SetAppStatus() 也在这里。
//
//  大致流程（一条主线，读代码时可以照着走）：
//    主窗口调用 PageCrop_Create() 创建本页 → WM_CREATE 里建好全部子控件并 Layout() 排一次版
//    → 用户载入图片，主窗口广播 WM_APP_IMAGE_CHANGED，本页刷新画布与坐标显示
//    → 用户在画布上拖动裁切框，画布发来 WM_APP_RECT_CHANGED，本页更新坐标文字
//    → 用户点“处理”，OnProcess() 用 GDI+ 抠图并交给 SaveBitmapWithSuffix() 保存。
//
//  【给新手的几个关键概念：先看这里，再看代码】
//    · 子窗口 / 控件：在 Win32 里，“按钮、静态文字、复选框”本质上都是一个个子窗口，
//      都用 CreateWindowExW() 创建，都有自己的父窗口（这里是本页面窗口）和控件 ID。
//      本项目约定用 CreateCtl() 包一层，省去重复填参数，并自动给控件套上界面字体。
//    · 消息驱动：Windows 程序靠“消息”运转。窗口过程 Proc() 是一个大 switch：
//      系统消息（WM_CREATE / WM_SIZE / WM_COMMAND / WM_DRAWITEM …）由系统发来，
//      自定义消息（WM_APP + n，定义见 App.h）由本程序自己发。
//    · WM_NOTIFY 与自定义消息：标准通用控件（列表、树等）会把事件用 WM_NOTIFY
//      发给父窗口；本页用到的都是简单控件（按钮/静态框/复选框），它们的点击走
//      WM_COMMAND，所以本页不需要处理 WM_NOTIFY。
//      真正需要“子窗口通知父窗口”的是裁切画布，它用的是自定义消息：
//      WM_APP_RECT_CHANGED（画布 → 本页，裁切框变了）。同理，主窗口用
//      WM_APP_IMAGE_CHANGED 通知各页“源图换了”。两条自定义消息都在本页的 Proc() 里接。
//    · GetWindowLongPtrW / GWLP_USERDATA：给窗口挂“私有数据”的常用手段 ——
//      创建窗口时把指针存进去，之后在窗口过程里再取回来。裁切画布内部就是这么做的
//      （见 CropCanvas.cpp 的 State()：GetWindowLongPtrW(h, GWLP_USERDATA)）。
//      本页状态较少，直接用文件级 static 变量保存控件句柄，所以本文件里不出现它。
//    · DPI 缩放：坐标不要写死像素。本项目统一用 D(x)（= MulDiv(x, g_dpi, 96)）把
//      “96dpi 下的逻辑像素”换算成当前显示器的真实像素，保证高 DPI 下界面不变形。
// ============================================================================
#include "App.h"          // 公共头：全局状态、控件 ID、CropCanvas_* 与 ImageOps 接口

using namespace Gdiplus;  // 后面大量使用 Bitmap / Graphics / Color 等 GDI+ 类型

// ---------------------------------------------------------------------------
// 本页面用到的所有控件句柄
//   统一用 static 变量保存（本项目各功能页约定俗成的写法）：
//   · static 表示“只在本 .cpp 文件内可见”，避免和其它功能页的同名变量冲突；
//   · 句柄在 WM_CREATE 里创建后赋值，之后在 Layout() 和消息处理里直接使用；
//   · 初值 nullptr 表示“还没创建”，所以每个使用处都要判空，防止创建失败时崩溃。
// ---------------------------------------------------------------------------
static HWND s_hPage = nullptr;       // 本页自身的窗口句柄（WM_CREATE 里记住，Layout 要用到）
static HWND s_hCanvas = nullptr;     // 中间的裁切画布（CropCanvas.cpp 的 Pic246CropCanvas 类）
static HWND s_hGrpFill = nullptr;    // 分组框“裁切框超出图片部分的填充”
static HWND s_hSwatch = nullptr;     // 颜色小方块（自绘，显示当前填充色或透明棋盘格）
static HWND s_hBtnColor = nullptr;   // “选择填充颜色...”按钮
static HWND s_hChkTrans = nullptr;   // 复选框“透明底色（PNG/GIF/TIFF）”
static HWND s_hHint = nullptr;       // 说明性静态文字
static HWND s_hInfo = nullptr;       // 显示裁切区域 / 起点坐标的静态文字
static HWND s_hBtnReset = nullptr;   // “重置裁切框（铺满图片）”按钮
static HWND s_hBtnSquare = nullptr;  // “居中正方形”按钮
static HWND s_hBtnProcess = nullptr; // “处理”按钮（默认按钮）

// ---------------------------------------------------------------------------
// FillTransparent —— 当前是否勾选了“透明底色”
//   参数：无
//   返回：勾选返回 true，否则返回 false
//   说明：Button_GetCheck() 是 <windowsx.h> 里的宏，等价于
//         SendMessage(h, BM_GETCHECK, 0, 0)，用来读取复选框的勾选状态。
//         因为复选框是 BS_AUTOCHECKBOX，点击后由系统自动切换选中状态，
//         我们只要“读”即可，不需要自己维护一个 bool 变量。
//   坑：只有“源图格式本身支持透明”（png/gif/tif/tiff）时透明才真正生效。
//       这个判断不在这里做，而是由 PushFillToCanvas() 和 OnProcess() 再调
//       FormatSupportsAlpha() 一起决定，避免“勾了透明却存成 jpg”这种矛盾。
// ---------------------------------------------------------------------------
static bool FillTransparent()
{
    return Button_GetCheck(s_hChkTrans) == BST_CHECKED;  // BST_CHECKED = 已勾选
}

// ---------------------------------------------------------------------------
// PushFillToCanvas —— 把“填充色 + 是否透明”同步给画布，并重画颜色小方块
//   参数：无
//   返回：无
//   为什么要单独抽一个函数：填充色或透明选项有多个改动时机
//   （WM_CREATE 初始化、点“选择填充颜色”、勾选/取消复选框、载入新图之后），
//   集中在一处推送，就不会漏掉某条路径，导致画布和右侧面板显示不一致。
//   注意：FormatSupportsAlpha() 为 false 时（例如 jpg / bmp 源图），
//         即使勾了“透明底色”也要传 false 给画布 —— 因为最终会保存成不支持透明的
//         格式，画布上就应该显示填充色。所见即所得。
// ---------------------------------------------------------------------------
static void PushFillToCanvas()
{
    if (s_hCanvas)
        // g_fillColor 是全局填充色（裁切页与 ICO 页共用）；
        // 第二个参数是“超出图片的部分是否透明”。
        CropCanvas_SetFill(s_hCanvas, g_fillColor, FillTransparent() && FormatSupportsAlpha());
    if (s_hSwatch)
        // 颜色小方块是 SS_OWNERDRAW 自绘控件：内容不会自动更新，必须调用
        // InvalidateRect() 请求重画（TRUE = 先擦除背景再画）。
        // 这个重画请求随后会以 WM_DRAWITEM 消息发回本页的窗口过程处理。
        InvalidateRect(s_hSwatch, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// UpdateInfo —— 刷新右侧“裁切区域：W × H 像素 / 起点：X, Y”这两行文字
//   参数：无
//   返回：无
//   坐标说明：CropCanvas_GetRect() 返回的是“图像坐标系”下的裁切框
//             （以源图左上角为原点、单位是原图像素，不是屏幕像素），
//             所以这里显示的就是最终输出的像素尺寸和位置，不会随窗口缩放而变。
//             起点允许为负数、宽高也允许超出图片（裁切框可以拖到图片外面）。
// ---------------------------------------------------------------------------
static void UpdateInfo()
{
    if (!s_hInfo) return;                       // 控件还没创建好：直接返回，防止空句柄
    if (!g_pSrcImage || !s_hCanvas)             // 还没载入图片：显示占位文字
    {
        SetWindowTextW(s_hInfo, L"裁切区域：—");
        return;
    }

    float x = 0, y = 0, w = 0, h = 0;
    CropCanvas_GetRect(s_hCanvas, &x, &y, &w, &h);   // 取当前裁切框（图像坐标，浮点数）

    wchar_t buf[256];                            // 栈上缓冲区，够放下这两行文字
    // swprintf_s 是“安全版”的 swprintf：必须给出缓冲区大小，写超了会直接失败（返回 -1），
    // 而不是像老式 wsprintf 那样冲爆栈。
    // 格式串里用 %d 显示尺寸/坐标，但裁切框是 float，所以先做 (int)(值 + 0.5f) 的四舍五入
    //（注意这不是 C 的 round()：(int) 是截断取整，+0.5f 让 2.6→3、2.2→2）。
    // \r\n 是换行：静态控件要显示两行必须用 \r\n，只写 \n 在有些控件里不会换行。
    swprintf_s(buf, L"裁切区域：%d × %d 像素\r\n起点：%d, %d   后缀：_crop",
               (int)(w + 0.5f), (int)(h + 0.5f), (int)(x + 0.5f), (int)(y + 0.5f));
    SetWindowTextW(s_hInfo, buf);                // 把文字写进静态控件
}

// ---------------------------------------------------------------------------
// Layout —— 根据页面客户区的大小，重新摆放所有子控件
//   参数：无（页面尺寸用 GetClientRect(s_hPage) 现取）
//   返回：无
//   调用时机：WM_CREATE 建完控件后调用一次；之后每次 WM_SIZE（窗口被拖动改变大小）
//             都会再调用。Win32 不会帮你自动排版，所有位置尺寸都要自己算。
//   布局思路：
//     ┌──────────────────────────────────────────────┐
//     │  ┌───────────────────────┐  ┌─────────────┐  │
//     │  │                       │  │  填充色分组框 │  │
//     │  │       裁切画布         │  │  坐标信息     │  │
//     │  │                       │  │  重置/正方形  │  │
//     │  └───────────────────────┘  └───[ 处理 ]───┘  │
//     └──────────────────────────────────────────────┘
//   左边是画布，右边是固定宽度的面板 panelW；面板里从上往下依次排，
//   而“处理”按钮反过来从客户区底部往上定位，永远贴在右下角。
// ---------------------------------------------------------------------------
static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);      // 客户区 = 窗口去掉边框/标题栏后真正能画的区域
    const int cx = rc.right;          // 客户区宽度（rc.left / rc.top 恒为 0）
    const int cy = rc.bottom;         // 客户区高度
    const int M = D(12);              // 四周留白 12 逻辑像素，经 DPI 换算成真实像素

    // 右侧面板宽度：最多 232 逻辑像素；窗口很窄时不超过一半宽度，防止把画布挤没。
    int panelW = D(232);
    if (panelW > cx / 2) panelW = cx / 2;      // 整数除法，这里够用

    // 画布宽度 = 总宽 - 面板宽 - 3 个留白（左 M、中间 M、右 M）
    int canvasW = cx - panelW - M * 3;
    if (canvasW < D(120)) canvasW = D(120);    // 保底 120 逻辑像素，防止出现负宽度或太小

    // MoveWindow(窗口, x, y, 宽, 高, 是否立即重画)：坐标都相对父窗口客户区左上角
    // 画布从 (M, M) 开始，高度 = 客户区高 - 上下各一个 M
    MoveWindow(s_hCanvas, M, M, canvasW, cy - M * 2, TRUE);

    // 右侧面板起点：左边 M + 画布宽 + 中间 M
    int px = M * 2 + canvasW;         // 面板左上角 x
    int pw = cx - px - M;             // 面板宽度（一直算到右边留白为止）
    int py = M;                       // py 表示面板里“当前这一行”的 y 坐标

    // ---- 第 1 组：填充设置分组框（整体高 130 逻辑像素）----
    int grpH = D(130);
    MoveWindow(s_hGrpFill, px, py, pw, grpH, TRUE);                           // 分组框本体
    MoveWindow(s_hSwatch, px + D(16), py + D(30), D(40), D(26), TRUE);        // 颜色小方块
    MoveWindow(s_hBtnColor, px + D(66), py + D(28), pw - D(82), D(30), TRUE); // 选色按钮
    MoveWindow(s_hChkTrans, px + D(16), py + D(64), pw - D(32), D(24), TRUE); // 透明复选框
    MoveWindow(s_hHint, px + D(16), py + D(92), pw - D(32), D(30), TRUE);     // 说明文字

    py += grpH + D(10);                                // 换到下一行：分组框高度 + 间距
    // ---- 第 2 块：裁切区域/起点信息（两行文字，高 48）----
    MoveWindow(s_hInfo, px, py, pw, D(48), TRUE);

    py += D(52);                                       // 48 高 + 4 间距
    // ---- 第 3、4 块：两个“一键摆正裁切框”的按钮，各占一行 ----
    MoveWindow(s_hBtnReset, px, py, pw, D(34), TRUE);
    py += D(40);                                       // 34 高 + 6 间距
    MoveWindow(s_hBtnSquare, px, py, pw, D(34), TRUE);

    // ---- “处理”按钮固定贴在面板最底部：底部留白 M，再往上一个按钮高 ----
    int btnH = D(42);
    MoveWindow(s_hBtnProcess, px, cy - M - btnH, pw, btnH, TRUE);
}

// ---------------------------------------------------------------------------
// OnProcess —— 点“处理”按钮时真正干活：按裁切框从源图抠出区域并保存
//   参数：hPage —— 本页窗口句柄，仅用作消息框的父窗口（让弹窗居中在本页上）
//   返回：无
//   输出文件：与源图同目录、同扩展名，文件名加后缀“_crop”
//             （由 ImageOps.cpp 的 SaveBitmapWithSuffix() 负责拼接路径并保存）。
//   实现要点（也是新手最容易踩的坑）：
//     1) 裁切框可能超出图片范围。超出了，则“图外部分”用填充色填、或保持透明。
//        做法是：先把整张目标位图刷成填充色/透明，再把源图按偏移贴上去。
//     2) 坐标要用“负起点”来平移：目标位图的 (0,0) 对应源图的 (x,y)，
//        所以源图左上角在目标位图里的位置是 (-x, -y)。
//     3) 透明只有在源图扩展名支持时才有意义（FormatSupportsAlpha()）。
//     4) GDI+ 的 Bitmap 要用 new 创建，并且必须保证被 delete；
//        这里把所有权交给 SaveBitmapWithSuffix()，它在内部负责 delete。
//     5) 大图要提醒用户：输出尺寸过大时先问一句，避免一次性吃掉几百 MB 内存。
// ---------------------------------------------------------------------------
static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage || !s_hCanvas)     // 没载图就点“处理”：给个友好提示后返回
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    float x = 0, y = 0, w = 0, h = 0;
    CropCanvas_GetRect(s_hCanvas, &x, &y, &w, &h);   // 裁切框（图像坐标，浮点，可能为负）

    // 转成整数像素：+0.5f 做四舍五入；并保证至少 1×1（Bitmap 不允许 0 宽或 0 高）
    int nw = (int)(w + 0.5f);
    int nh = (int)(h + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    // 内存预警：像素数用 LONGLONG（64 位）相乘，避免 int 溢出；
    // 阈值 1 亿像素 ≈ 32 位 ARGB 的 400MB（估算公式见下面那行 swprintf_s）。
    if ((LONGLONG)nw * nh > 100000000LL)
    {
        wchar_t q[512];
        // %lld 对应 long long；(nw*nh*4) 是 32bpp 的字节数，除以 1024*1024 得到 MB。
        // 这里同样先乘成 LONGLONG 再除，防止中间结果溢出 32 位整数。
        swprintf_s(q, L"输出尺寸为 %d × %d，大约需要 %lld MB 内存。\r\n\r\n是否继续？",
                   nw, nh, ((LONGLONG)nw * nh * 4) / (1024 * 1024));
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;                      // 用户点了“否”：什么都不做，直接返回
    }

    // 是否真的按“透明”输出：勾了复选框 **并且** 源格式支持 alpha（png/gif/tif/tiff）才算。
    // 举例：源图是 .jpg，即便勾选了“透明底色”，这里仍然是 false，会用填充色铺底，
    //       因为 jpg 本身存不了透明通道。
    const bool trans = FillTransparent() && FormatSupportsAlpha();

    // 处理可能比较慢。先把主窗口底部状态栏改成“正在处理，请稍候...”，
    // 并立刻 UpdateWindow(g_hMain) 强制刷新一次 —— 否则消息循环会被后面的耗时操作占住，
    // 用户根本看不到这行提示。
    SetAppStatus(L"正在处理，请稍候...");
    UpdateWindow(g_hMain);

    // 目标位图：nw × nh 像素，32 位带 Alpha 的 ARGB 格式（只有这种格式能承载透明）。
    // new 出来的对象要用 GetLastStatus() 检查是否真的创建成功（内存不足时返回 OutOfMemory）。
    Bitmap* dst = new Bitmap(nw, nh, PixelFormat32bppARGB);
    if (dst->GetLastStatus() != Ok)
    {
        delete dst;                      // 创建失败也要 delete，否则内存泄漏
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    // 用一对花括号限定 Graphics 的生命周期：GDI+ 要求“画完 / Graphics 析构之后”
    // 才能用 Bitmap 去保存，所以 Graphics 必须在这个块结束时先析构。
    {
        Graphics g(dst);                 // 把 dst 当作绘图目标（Graphics 相当于“画布上下文”）
        // SourceCopy = 覆盖式写入，不做 Alpha 混合。
        // 这一步很关键：下面的 g.Clear() 是用来“铺底”的；如果沿用默认的 SourceOver，
        // 透明色 (0,0,0,0) 在已有内容上“混合”并不会真的把它变透明。

        g.SetCompositingMode(CompositingModeSourceCopy);
        if (trans)
            // 透明底色：把整张目标图清成全透明（ARGB = 0,0,0,0）
            g.Clear(Color(0, 0, 0, 0));
        else
            // 用填充色铺底：注意 GDI+ 的 Color(A, R, G, B) 四个分量依次是
            // 不透明度、红、绿、蓝；而 Windows 常用的 COLORREF 是 0x00BBGGRR（BGR 序），
            // 所以这里要用 GetRValue / GetGValue / GetBValue 拆成三个通道再传。
            // 另外：超出图片范围的像素会保留这层底色 —— 这就是“填充色”的实现方式。
            g.Clear(Color(255, GetRValue(g_fillColor), GetGValue(g_fillColor), GetBValue(g_fillColor)));

        // 铺底完成后切回 SourceOver：后面的 DrawImage 要正常叠画在底色之上。
        g.SetCompositingMode(CompositingModeSourceOver);

        // 下面三行统一设成高质量，保证缩放（裁切框可能被放大/缩小过）时边缘平滑：
        //   HighQualityBicubic     —— 双三次插值，缩图质量最好（也最慢）
        //   PixelOffsetModeHighQuality —— 高质量像素偏移，减少半像素采样偏差
        //   CompositingQualityHighQuality —— 高质量混合运算
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetCompositingQuality(CompositingQualityHighQuality);

        // ImageAttributes 用来给 DrawImage 附加取样规则。
        // WrapModeTileFlipXY：当插值需要采到“源图边界之外”的像素时，按镜像/平铺方式
        // 环绕取样，而不是采到空白，从而避免图像边缘出现黑边或透明毛边。
        ImageAttributes attr;
        attr.SetWrapMode(WrapModeTileFlipXY);

        // 关键一步：把整张源图“平移”后画进目标位图。
        //   dest —— 目标矩形（目标位图坐标系）：左上角是 (-x, -y)，大小仍是整张源图。
        //           因为目标位图的 (0,0) 对应源图裁切框左上角 (x,y)，
        //           所以源图原点在目标坐标里就是 (-x, -y)；x/y 为负时它们反而是正的。
        //   DrawImage 后面的源矩形 (0,0,g_srcW,g_srcH) 表示“取整张源图”，
        //   UnitPixel 表示单位是像素。
        //   超出目标位图的绘制部分会被自动裁掉，于是得到的就是裁切框内的内容；
        //   而裁切框超出源图的那部分，则保留上面 Clear() 刷出来的底色/透明。

        RectF dest(-x, -y, (REAL)g_srcW, (REAL)g_srcH);
        g.DrawImage(g_pSrcImage, dest,
                    0.0f, 0.0f, (REAL)g_srcW, (REAL)g_srcH, UnitPixel, &attr);
    }

    // 保存：文件名 = 原目录 + 原文件名(不含扩展名) + "_crop" + 原扩展名。
    // 注意 —— 这个函数内部会 delete dst，调用后不要再使用它（所有权已经转移）。
    SaveBitmapWithSuffix(dst, L"_crop", hPage, nullptr);
}

// ---------------------------------------------------------------------------
// Proc —— 本页窗口的“窗口过程”：所有发给本页的消息都在这里分流处理
//   参数（Win32 固定签名，新手先记住含义即可）：
//     hWnd    —— 消息发给哪个窗口（这里就是本页；创建后也在 s_hPage 里存了一份）
//     message —— 消息编号（WM_xxx，或本程序自定义的 WM_APP+n）
//     wParam / lParam —— 消息附带的两个参数，具体含义随 message 而变
//   返回：返回值的含义同样随消息而变。多数消息返回 0 表示“已处理”；
//         本过程没处理的消息要交给 DefWindowProcW() 做系统默认处理。
// ---------------------------------------------------------------------------
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    // 窗口创建时发来一次：这里是建子控件、做初始化的地方。
    case WM_CREATE:
        {
            s_hPage = hWnd;                          // 记住本页句柄，Layout() 里要用

            // 建裁切画布：第三个参数 false 表示“不是正方形模式”
            //（ICO 页用的是 true：只保留 4 个角控制点，且强制保持正方形）。
            // 返回值就是画布窗口句柄；之后所有画布操作都要通过它调用 CropCanvas_* 函数。
            s_hCanvas = CropCanvas_Create(hWnd, IDC_CP_CANVAS, false);

            // 建一组普通控件。用 CreateCtl() 省去每次都写父窗口/实例句柄/字体，
            // 参数依次是：窗口类名、显示文字、样式、父窗口、控件 ID。
            // 样式含义：
            //   WS_VISIBLE        —— 创建后立即显示
            //   WS_TABSTOP        —— 可以用 Tab 键切换过去
            //   BS_GROUPBOX       —— 分组框（带标题的边框）
            //   SS_OWNERDRAW      —— 静态控件改为自绘，内容由我们处理 WM_DRAWITEM 来画
            //   SS_LEFTNOWORDWRAP —— 左对齐、不自动换行（换行完全靠字符串里的 \r\n 控制）
            //   BS_AUTOCHECKBOX   —— 复选框，点击后系统自动切换勾选状态
            //   BS_DEFPUSHBUTTON  —— 默认按钮（按回车键会触发它）
            s_hGrpFill = CreateCtl(L"Button", L"裁切框超出图片部分的填充", WS_VISIBLE | BS_GROUPBOX,
                                   hWnd, IDC_CP_GRP_FILL);
            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CP_SWATCH);
            s_hBtnColor = CreateCtl(L"Button", L"选择填充颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_CP_BTN_COLOR);
            s_hChkTrans = CreateCtl(L"Button", L"透明底色（PNG/GIF/TIFF）",
                                    WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, hWnd, IDC_CP_CHK_TRANS);
            // 纯说明文字：ID 用 -1，表示“不关心它的命令，只要一个静态控件”。
            s_hHint = CreateCtl(L"Static", L"勾选“透明底色”后，超出图片的部分保持透明；\r\n否则用上面的颜色填充。",
                                WS_VISIBLE, hWnd, -1);

            s_hInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CP_INFO);
            s_hBtnReset = CreateCtl(L"Button", L"重置裁切框（铺满图片）", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_CP_BTN_RESET);
            s_hBtnSquare = CreateCtl(L"Button", L"居中正方形", WS_VISIBLE | WS_TABSTOP,
                                     hWnd, IDC_CP_BTN_SQUARE);
            s_hBtnProcess = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      hWnd, IDC_CP_BTN_PROCESS);

            // 初始化界面状态：
            PushFillToCanvas();   // 把初始填充色/透明状态同步给画布和颜色方块
            UpdateInfo();         // 先把坐标文字设成“未载入图片”的占位
            Layout();             // 现算一遍位置（WM_CREATE 时客户区尺寸已经可用了）
        }
        return 0;   // 坑：WM_CREATE 里必须返回 0；返回 -1 会让 CreateWindow 直接失败

    // 窗口尺寸变化（拖动边框、最大化等）时发来：重排所有子控件。
    case WM_SIZE:
        Layout();
        return 0;

    // 自定义消息：源图已更换（主窗口载入图片后广播给所有页面）。
    // 注意它是“主窗口 → 本页”的单向通知，本页收到后要主动做三件事：
    case WM_APP_IMAGE_CHANGED:
        if (s_hCanvas)
            // 让画布丢掉旧的显示缓存、按新图重新缩放，并把裁切框重置为铺满新图
            CropCanvas_RefreshImage(s_hCanvas);
        PushFillToCanvas();   // 新图扩展名可能不同（比如从 png 换成 jpg），透明是否生效会变
        UpdateInfo();         // 刷新坐标显示
        return 0;

    // 自定义消息：裁切框变了（画布在拖动过程中主动发给本页）。
    // 这是本页“完全不需要轮询”的关键：画布内部用
    // SendMessageW(GetParent(h), WM_APP_RECT_CHANGED, ...) 通知父窗口，
    // 父窗口只要收到消息就刷新坐标文字即可。
    case WM_APP_RECT_CHANGED:
        UpdateInfo();
        return 0;

    // 自绘控件（这里是颜色小方块）需要绘制时发来。
    // 对 SS_OWNERDRAW 的静态控件，系统不会自己画内容，而是把 WM_DRAWITEM 发给父窗口：
    // wParam = 控件 ID，lParam 指向 DRAWITEMSTRUCT（里面有目标 DC、绘制矩形等信息）。
    // 返回 TRUE 表示“我已经画好了，别再让系统画”。
    case WM_DRAWITEM:
        if (wParam == IDC_CP_SWATCH && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            // PaintColorSwatch() 根据 transparent 决定画棋盘格（透明）还是纯色，
            // 最后统一画一圈边框。自绘控件必须自己画，否则就只是一片空白。
            PaintColorSwatch(dis->hDC, dis->rcItem, g_fillColor,
                             FillTransparent() && FormatSupportsAlpha());
            return TRUE;
        }
        break;   // 不是我们要处理的自绘控件：跳出去让 DefWindowProcW 兜底

    // 菜单/按钮/复选框等控件发来的“命令”消息都集中在这里。
    // LOWORD(wParam) 才是控件 ID（HIWORD 是通知码，按钮点击时通常是 BN_CLICKED）。
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CP_BTN_COLOR:
            // 弹出系统“颜色”对话框；用户取消时 ChooseFillColor() 会把原颜色原样返回，
            // 所以这里直接赋值也不会有副作用。g_fillColor 是与 ICO 页共用的全局变量。
            g_fillColor = ChooseFillColor(hWnd, g_fillColor);
            PushFillToCanvas();
            return 0;

        case IDC_CP_CHK_TRANS:
            // 复选框状态由系统自动切换，这里只要同步给画布、重画颜色方块即可。
            PushFillToCanvas();
            return 0;

        case IDC_CP_BTN_RESET:
            // 让画布把裁切框重置成“刚好铺满整张图”（不超出图片范围）
            if (s_hCanvas) CropCanvas_ResetToImage(s_hCanvas);
            UpdateInfo();
            return 0;

        case IDC_CP_BTN_SQUARE:
            // 让画布把裁切框重置成“以图片中心为中心的、最大的正方形”
            if (s_hCanvas) CropCanvas_ResetToSquareCenter(s_hCanvas);
            UpdateInfo();
            return 0;

        case IDC_CP_BTN_PROCESS:
            OnProcess(hWnd);   // 真正干活：抠图 + 保存
            return 0;

        default:
            break;   // 不是本页的控件 ID：交给系统默认处理
        }
        break;

    default:
        break;
    }
    // 本页没处理的消息（或处理完还需要系统收尾的消息）交给系统默认处理，
    // 比如 WM_ERASEBKGND、WM_SETCURSOR 等，全靠它。
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// PageCrop_Create —— 由主窗口调用，创建“裁切”功能页窗口（本文件对外接口）
//   参数：parent —— 父窗口（主窗口的内容区）
//   返回：创建好的页面窗口句柄；失败返回 nullptr
//   说明：
//     · 每个功能页都注册自己的窗口类，这样“页面长什么样、怎么响应”全由本文件决定，
//       主窗口只负责创建、显示、隐藏，不需要知道页面的内部细节。
//     · 窗口类只需要注册一次，用函数内 static bool 记住（第一次调用时注册）。
//     · 创建时只给 WS_CHILD，**故意不加 WS_VISIBLE**：页面初始是隐藏的，
//       之后由主窗口在用户点击侧边栏时用 ShowWindow() 显示。
//     · WS_CLIPCHILDREN 让父窗口重画时跳过子窗口所占区域，可以减少闪烁。
// ---------------------------------------------------------------------------
HWND PageCrop_Create(HWND parent)
{
    static bool registered = false;      // 函数内静态变量：跨多次调用只初始化一次
    if (!registered)
    {
        WNDCLASSEXW wc = {};             // 置零非常重要：结构体里没赋值的字段必须都是 0
        wc.cbSize = sizeof(wc);          // 第一个字段必须填结构体大小，系统靠它判断版本
        wc.style = CS_HREDRAW | CS_VREDRAW;  // 宽/高变化时整个重画，避免留下残影
        wc.lpfnWndProc = Proc;           // 本窗口类的消息处理函数
        wc.hInstance = g_hInst;          // 模块实例句柄（全局变量，程序启动时已填好）
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);  // 鼠标默认箭头
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // 用系统窗口背景色刷背景
        wc.lpszClassName = L"Pic246PageCrop";          // 窗口类名（各页各不相同）
        // 注册失败要区分情况：ERROR_CLASS_ALREADY_EXISTS 说明类已经存在，属于正常；
        // 其它错误才是真的失败，返回 nullptr 让调用方知道页面没建起来。
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    // 创建页面窗口：初始位置大小随便给（0,0,10,10），随后主窗口会调 MoveWindow 摆正；
    // 父窗口是 parent，所以它会跟随主窗口显示/隐藏，并且作为子窗口被裁在客户区内。
    return CreateWindowExW(0, L"Pic246PageCrop", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
