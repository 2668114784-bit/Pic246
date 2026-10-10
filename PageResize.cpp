// ============================================================================
//  PageResize.cpp —— 功能页：图片尺寸缩放
// ============================================================================
// ---------------------------------------------------------------------------
//  【文件说明】这个文件负责什么
// ---------------------------------------------------------------------------
//  这是 Pic246（Win32 + GDI+ 图片小工具）的一个“功能页”。整个程序的结构是：
//
//    · Project001.cpp  创建主窗口，左边一个自绘侧边栏，右边一块空白区域；
//    · 每个功能（缩放 / 裁切 / 调整 / ICO / 压缩 / 格式转换 / 涂鸦）各自一个
//      Page*.cpp 文件，每个文件注册一个自己的窗口类，并把窗口以“子窗口”的
//      形式贴在主窗口右侧那块区域上；
//    · 页面之间不直接互相调用，而是通过 App.h 里的全局变量（g_pSrcImage、
//      g_srcW/g_srcH 等，表示“当前源图”）和自定义消息
//      （WM_APP_IMAGE_CHANGED 等）来通信。
//
//  本页（缩放页）做的事情很简单：
//    1. 上方一块预览区，显示当前源图缩略图；
//    2. 中间一组单选按钮，让用户选 1/2、1/4、1/6、1/8、1/10 中的一个缩小比例；
//    3. 右侧一行文字，实时显示“按当前比例算出来的输出尺寸”和“输出文件名后缀”；
//    4. 底部一个「处理」按钮，点下去真正生成缩小后的图片，并存到
//       “原图所在目录 + 原文件名 + 后缀（如 _2） + 原扩展名”。
//
//  大致流程（按时间顺序）：
//    主窗口启动 → 调 PageResize_Create() 注册窗口类并创建本页窗口（此时还是
//    隐藏的、10×10 的占位尺寸）→ Windows 发 WM_CREATE，本页创建所有子控件 →
//    主窗口把本页 MoveWindow 到正确位置/尺寸 → Windows 发 WM_SIZE，本页的
//    Layout() 按 DPI 重新摆放所有控件 → 用户选比例（WM_COMMAND / BN_CLICKED，
//    刷新尺寸文字）→ 用户点「处理」（OnProcess：算尺寸、问确认、缩图、保存）。
//    另外，每当主窗口载入了一张新图片，它会向本页发 WM_APP_IMAGE_CHANGED，
//    本页据此重建预览图和刷新尺寸文字。
//
//  【和其他文件的配合】
//    · App.h              —— 全局变量、控件 ID、通用函数、消息号的声明（本页
//                            用到的 D()、CreateCtl()、CreatePreviewSource()、
//                            ResizeBitmap()、SaveBitmapWithSuffix()、
//                            PaintImagePreview()、g_pSrcImage、g_srcW/g_srcH 均
//                            来自这里）；
//    · Project001.cpp     —— 主窗口、CreateCtl()/SetAppStatus()、页面切换；
//    · ImageOps.cpp       —— 真正的图片处理实现（缩放、画预览、保存等）；
//    · resource.h / Project001.h —— 资源与工程级声明。
//
//  【本页注册的窗口类名】Pic246PageResize
//  【本页使用的子控件 ID】见下面各 CreateCtl() 调用处（IDC_RS_* 号段）
// ===========================================================================
#include "App.h"          // 公共声明：全局变量、消息号、工具函数、GDI+ 头文件等

using namespace Gdiplus;  // 本文件大量使用 Gdiplus::Bitmap / Image 等，直接引用省前缀

// ---------------------------------------------------------------------------
//  比例选项表
// ---------------------------------------------------------------------------
//  ScaleOption 描述一个“缩小比例”菜单项：
//    label   —— 显示给用户看的文字（如 "1/2"）
//    suffix  —— 输出文件名追加的后缀（如 "_2"，最终得到 图片_2.jpg）
//    num/den —— 分子/分母，用来做整数运算计算新尺寸，避免浮点误差
struct ScaleOption
{
    const wchar_t* label;   // 单选按钮上显示的文字
    const wchar_t* suffix;  // 输出文件名后缀（含下划线，不含扩展名）
    int            num;     // 比例分子（都是 1）
    int            den;     // 比例分母（2、4、6、8、10）
};

// 比例表：顺序就是界面上从左到右、也是 IDC_RS_RADIO_BASE + 0..4 的顺序。
// 注意数组下标和控件 ID 的对应关系——本文件多处依赖“下标 i ↔ 第 i 个单选按钮”。
static const ScaleOption kScales[] =
{
    { L"1/2",  L"_2",  1, 2  },
    { L"1/4",  L"_4",  1, 4  },
    { L"1/6",  L"_6",  1, 6  },
    { L"1/8",  L"_8",  1, 8  },
    { L"1/10", L"_10", 1, 10 },
};
// _countof 是 MSVC 提供的宏（等价于 sizeof(数组)/sizeof(元素[0])），
// 用它可以避免“数组长度改小了、循环却还按旧长度跑”的经典错误。
static const int kScaleCount = _countof(kScales);

// ---------------------------------------------------------------------------
//  本页的控件句柄集合
// ---------------------------------------------------------------------------
//  Win32 里所有子控件都是靠 HWND 操作的，所以统一用一组 static 变量保存，
//  这样到处都能直接用，不必每次去 GetDlgItem()。
//  这些句柄在 WM_CREATE 里一次性创建（见下面 Proc 的 WM_CREATE 分支）。
static HWND    s_hPage = nullptr;                  // 本页自身的窗口句柄（子窗口）
static HWND    s_hPreview = nullptr;               // 预览区：自绘 Static（SS_OWNERDRAW）
static HWND    s_hGrp = nullptr;                   // 比例分组框：Button + BS_GROUPBOX
static HWND    s_hOut = nullptr;                   // “输出尺寸：...”文字：Static
static HWND    s_hBtn = nullptr;                   // 「处理」按钮
static HWND    s_hRadios[kScaleCount] = {};        // 5 个比例单选按钮
static Bitmap* s_pPreview = nullptr;               // 预览用的缩略图（本页自己 new 的，自己 delete）

// ---------------------------------------------------------------------------
//  ScaledEdge：把一条边按比例换算成新边长
// ---------------------------------------------------------------------------
//  参数：
//    srcEdge —— 原始边长（像素）
//    opt     —— 要应用的缩放比例
//  返回：
//    换算后的边长，最小为 1（图片宽高不能是 0，否则 GDI+ 创建位图会失败）。
//
//  为什么这么写：
//    1. 用整数乘加 (`+ opt.den / 2`) 实现四舍五入，比用 double 再 (int) 转换
//       更直观，也彻底避开浮点误差；
//    2. 先转成 LONGLONG(64 位) 再相乘再除，防止超大图（例如 60000 像素）
//       在 32 位 int 里乘法溢出；乘完除完立刻缩小回 int；
//    3. 最后兜底 `if (v < 1) v = 1;`——分母最大是 10，只要原图边长 ≥ 1，
//       通常不会到 0，但加这一句能保证任何输入都得到合法尺寸。
static int ScaledEdge(UINT srcEdge, const ScaleOption& opt)
{
    LONGLONG v = ((LONGLONG)srcEdge * opt.num + opt.den / 2) / opt.den;  // 四舍五入的整数缩放
    if (v < 1) v = 1;                                                    // 尺寸下限保护
    return (int)v;
}

// ---------------------------------------------------------------------------
//  CurrentScaleIndex：查出用户当前勾选了第几个比例
// ---------------------------------------------------------------------------
//  参数：无
//  返回：kScales 的下标（0..kScaleCount-1）；理论上一定有一个被选中
//        （WM_CREATE 里默认勾了第 0 个），万一都没勾就返回 0 兜底。
//
//  Button_GetCheck 是 commctrl.h 提供的宏，展开成
//  SendMessageW(h, BM_GETCHECK, 0, 0)，用于读取单选按钮的选中状态。
static int CurrentScaleIndex()
{
    for (int i = 0; i < kScaleCount; ++i)
        if (Button_GetCheck(s_hRadios[i]) == BST_CHECKED)   // BST_CHECKED == 已选中
            return i;
    return 0;                                               // 兜底：按第一个比例处理
}

// ---------------------------------------------------------------------------
//  UpdateOutLabel：刷新“输出尺寸：宽 × 高   后缀：xx”这行提示文字
// ---------------------------------------------------------------------------
//  参数：无（数据来源都是全局状态和控件状态）
//  返回：无
//
//  什么时候调用：① WM_CREATE 建完控件后；② 用户切换比例按钮时；
//                ③ 主窗口通知“换了新图片”（WM_APP_IMAGE_CHANGED）时。
//  为什么要有它：用户改了比例或换了图，必须马上看到“最终会输出多大”，
//  否则很容易选错比例。
static void UpdateOutLabel()
{
    wchar_t buf[256];                       // 固定大小缓冲；Win32 里常用这种“够大就行”的栈数组
    if (!g_pSrcImage)                       // 还没载入任何图片（全局指针为空）
    {
        wcscpy_s(buf, L"输出尺寸：—");       // 用破折号表示“暂无数据”
    }
    else
    {
        const ScaleOption& opt = kScales[CurrentScaleIndex()];
        // swprintf_s 是安全的格式化函数：会检查缓冲区大小，写不下时返回 -1 而不是越界。
        // %s 在宽字符版里对应 wchar_t*，所以 opt.suffix 直接可用。
        swprintf_s(buf, L"输出尺寸：%d × %d 像素    后缀：%s",
                   ScaledEdge(g_srcW, opt), ScaledEdge(g_srcH, opt), opt.suffix);
    }
    SetWindowTextW(s_hOut, buf);            // 静态文本控件改内容就是 SetWindowText
}

// ---------------------------------------------------------------------------
//  RebuildPreview：重建预览用的缩略图
// ---------------------------------------------------------------------------
//  参数：无
//  返回：无
//
//  注意“先删旧的再建新的”：CreatePreviewSource() 每调一次都会 new 一个
//  Bitmap（见 ImageOps.cpp:280），调用者负责释放。如果不先 delete 就覆盖指针，
//  每次换图都会泄漏一块内存。
//  源图为空时 CreatePreviewSource() 会返回 nullptr，此时 s_pPreview 就是空，
//  预览控件会显示提示文字（见 WM_DRAWITEM 分支）。
static void RebuildPreview()
{
    if (s_pPreview) { delete s_pPreview; s_pPreview = nullptr; }  // 释放上一次的缩略图
    s_pPreview = CreatePreviewSource(640);                        // 最长边压到 640 像素做预览
}

// ---------------------------------------------------------------------------
//  Layout：按当前页面大小重新摆放所有控件（在 WM_SIZE 时调用）
// ---------------------------------------------------------------------------
//  参数：无（用 GetClientRect 取本页客户区大小）
//  返回：无
//
//  ★ DPI 缩放（重要概念）
//    Windows 上的“像素”和 UI 设计尺寸不是一回事：在 150% 缩放的显示器上，
//    按钮、间距都应该相应放大，否则界面会挤成一小团。App.h:204 定义了
//    inline int D(int dip) { return MulDiv(dip, g_dpi, 96); }
//    把“96dpi 下的逻辑像素（DIP）”换算成“当前显示器的实际像素”。
//    所以本函数里所有硬编码尺寸都套了 D()，唯一的例外是控件自身算出来的
//    宽高（cx - M*2 之类），因为那是已经换算过的值参与运算。
//
//  ★ 为什么在 WM_CREATE 里也调一次 Layout：WM_CREATE 时窗口还是 10×10 的
//    占位尺寸，Layout 出来的位置基本没用，但能保证控件都有合理的初始位置；
//    真正的布局发生在主窗口把本页拉到正确大小、发来 WM_SIZE 之后。
static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);            // 客户区：不含标题栏/边框，左上角固定是 (0,0)
    const int cx = rc.right;                // 客户区宽（因为 left 必为 0，right 就等于宽度）
    const int cy = rc.bottom;               // 客户区高
    const int M = D(12);                    // 统一的页边距
    const int grpH = D(84);                 // 分组框高度（够放一行单选按钮）
    const int btnH = D(42);                 // 「处理」按钮高度

    // 垂直方向被“上边距 + 分组框 + 下边距 + 按钮 + 下边距”占掉，
    // 剩下的全给预览区；窗口很矮时兜个底，避免预览区被压成 0（甚至负数）。
    int previewH = cy - M - grpH - M - btnH - M;
    if (previewH < D(100)) previewH = D(100);

    // MoveWindow(hwnd, x, y, 宽, 高, 是否重绘)
    // 最后一个参数 TRUE 表示移动后立刻重画，简单但会闪；本页控件少，可以接受。
    MoveWindow(s_hPreview, M, M, cx - M * 2, previewH, TRUE);

    int gy = M + previewH + M;                                  // 分组框的 y 坐标
    MoveWindow(s_hGrp, M, gy, cx - M * 2, grpH, TRUE);          // 分组框铺满可用宽度

    int rx = M + D(20);                     // 第一个单选按钮的 x（留一点缩进，在框内好看）
    int ry = gy + D(34);                    // 单选按钮的 y（下移一点，避开分组框标题文字）
    for (int i = 0; i < kScaleCount; ++i)
    {
        MoveWindow(s_hRadios[i], rx, ry, D(76), D(26), TRUE);   // 每个按钮固定宽高，横向排开
        rx += D(84);                        // 步进 84 > 76，留出按钮之间的间隙
    }
    // 尺寸文字紧跟在最后一个单选按钮右边，宽度一直顶到右边距。
    // 注意 cx - M - (rx + D(12)) 是算出来的，不是固定值，所以窗口变宽它会跟着变宽。
    MoveWindow(s_hOut, rx + D(12), ry, cx - M - (rx + D(12)), D(26), TRUE);

    // 按钮贴在底部左侧
    MoveWindow(s_hBtn, M, cy - M - btnH, D(150), btnH, TRUE);
}

// ---------------------------------------------------------------------------
//  OnProcess：「处理」按钮被点击时的全部业务逻辑
// ---------------------------------------------------------------------------
//  参数：
//    hPage —— 本页窗口句柄，作为 MessageBox 的父窗口（这样提示框会挡在本页上，
//             并且是模态的；传 nullptr 则可能跑到别的窗口后面去）
//  返回：无
//
//  流程：
//    1. 没载入图片 → 提示并返回；
//    2. 根据勾选的比例算出目标宽高；
//    3. 原图不是正方形时，额外问一句“是否仍按相同比例缩放”（因为像 1/2 这种
//       “比例”在某些人心里其实是“缩成正方形的一半”，不确认容易产生误会）；
//    4. 状态栏提示“正在处理”，并强制刷新一次界面；
//    5. 调 ResizeBitmap() 真正缩放（失败则弹错误框）；
//    6. 调 SaveBitmapWithSuffix() 输出到原图同目录、文件名加后缀。
static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)                       // 全局源图指针为空 = 还没选图片
    {
        // MB_OK | MB_ICONINFORMATION：普通“知道了”提示框。
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const ScaleOption& opt = kScales[CurrentScaleIndex()];  // 取当前比例
    const int newW = ScaledEdge(g_srcW, opt);               // 目标宽
    const int newH = ScaledEdge(g_srcH, opt);               // 目标高

    if (g_srcW != g_srcH)                   // 非正方形：多问一句，避免误解
    {
        wchar_t q[512];
        swprintf_s(q, L"当前图片不是正方形（%u × %u）。\r\n\r\n是否仍按相同比例缩放并输出？",
                   g_srcW, g_srcH);         // %u 对应 UINT，即 g_srcW/g_srcH 的类型
        // MB_YESNO | MB_ICONQUESTION：是/否按钮 + 问号图标。
        // 只要返回值不是 IDYES（包括用户点了关闭按钮）就当作取消。
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    SetAppStatus(L"正在处理，请稍候...");   // 写底部状态栏
    // 大图缩放可能要几百毫秒。上面 SetAppStatus 只是把文字写进控件，
    // 如果马上开始计算，Windows 还没机会重画，用户就看不到这句提示。
    // UpdateWindow 强制立即重绘一次主窗口，保证“请稍候”先出现在屏幕上。
    UpdateWindow(g_hMain);

    std::wstring err;                       // 出错信息输出参数（由 ResizeBitmap 填充）
    Bitmap* bmp = ResizeBitmap(g_pSrcImage, newW, newH, err);
    if (!bmp)
    {
        MessageBoxW(hPage, err.c_str(), L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：" + err);  // 状态栏也显示一遍，方便用户事后查看
        return;
    }

    // ★ 所有权转移：SaveBitmapWithSuffix() 内部会 delete bmp（见 ImageOps.cpp:546），
    //   所以这里千万不要再 delete 一次（会二次释放导致崩溃）。
    //   它的第 4 个参数是“可选”的输出路径；本页不关心，传 nullptr。
    SaveBitmapWithSuffix(bmp, opt.suffix, hPage, nullptr);
}

// ---------------------------------------------------------------------------
//  Proc：本页的窗口过程（窗口消息处理函数）
// ---------------------------------------------------------------------------
//  参数（Windows 规定的固定签名，所有窗口过程都一样）：
//    hWnd    —— 收到消息的窗口句柄
//    message —— 消息 ID（WM_* 或自定义的 WM_APP+xx）
//    wParam / lParam —— 消息的附加参数，含义随 message 而变化
//  返回：
//    LRESULT（本质是 LONG_PTR）。处理过的消息通常返回 0；不处理的消息必须交给
//    DefWindowProcW()，否则窗口会失去默认行为（比如无法拖动、无法关闭）。
//
//  ★ Win32 新手最容易踩的坑
//    · WM_CREATE 的返回值有特殊约定：返回 0 表示“继续创建窗口”，
//      返回 -1 表示“创建失败，取消窗口创建”（此时 CreateWindowEx 会返回 NULL）。
//      本页不需要阻止创建，所以统一 return 0。
//    · 按钮通知有两种完全不同的“点击”：
//        - BN_CLICKED：用户“用鼠标/键盘”真正点了按钮，Windows 会以
//          WM_COMMAND 的形式发给父窗口，HIWORD(wParam) == BN_CLICKED。
//        - BM_CLICK：是代码主动“模拟”点一下（SendMessage(h, BM_CLICK, ...)），
//          它并不是发 WM_COMMAND，而是真的走一遍按下→弹起，从而间接触发
//          BN_CLICKED。所以想用代码点按钮就用 BM_CLICK，别去手动伪造 WM_COMMAND。
//    · 单选按钮 BS_AUTORADIOBUTTON 会自动处理“互斥选中”。同一组按钮要用
//      WS_GROUP 划出分组边界（见 WM_CREATE 里 `if (i == 0) style |= WS_GROUP;`），
//      否则一组里的多个按钮会互相干扰。
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    // 窗口创建时到达一次，是“建子控件”的标准位置。
    // 注意此时窗口尺寸还是 CreateWindowExW 里给的初始值（本页是 10×10）。
    case WM_CREATE:
        {
            s_hPage = hWnd;                 // 记住自己的句柄，Layout() 等函数要用

            // CreateCtl 是项目封装的 CreateWindowExW 包装（见 Project001.cpp:66）：
            // 它会自动补上 WS_CHILD、自动设置控件 ID、并自动把全局 UI 字体套上去。
            // 这样就不用每个控件都写一长串参数、也避免有人漏设字体导致界面字体不一致。
            //
            // SS_OWNERDRAW：这是一个“自绘”静态控件。它自己不画任何东西，
            // 而是每次需要重画时给父窗口发 WM_DRAWITEM，由父窗口负责绘制
            // （见下面 WM_DRAWITEM 分支，实际绘制交给 PaintImagePreview）。
            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_RS_PREVIEW);
            // BS_GROUPBOX：分组框。它只是画一个带标题的方框，本身不接收输入，
            // 用来把 5 个单选按钮在视觉上框成一组。
            s_hGrp = CreateCtl(L"Button", L"输出尺寸", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_RS_GRP);

            for (int i = 0; i < kScaleCount; ++i)
            {
                // WS_TABSTOP：允许用户按 Tab 键把焦点移动到这个控件上。
                // BS_AUTORADIOBUTTON：自动单选按钮，同组内点一个会自动取消别的。
                DWORD style = WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
                // WS_GROUP 标出“这一组从哪开始”。注意不是每个按钮都加，
                // 只在组内第一个按钮上加，之后的按钮如果没遇到新的 WS_GROUP，
                // 就都算同一组。
                if (i == 0) style |= WS_GROUP;
                // 控件 ID 用基址 + 下标，好处是 WM_COMMAND 里可以用
                // (id - IDC_RS_RADIO_BASE) 直接换算出对应 kScales 的下标。
                s_hRadios[i] = CreateCtl(L"Button", kScales[i].label, style, hWnd,
                                         IDC_RS_RADIO_BASE + i);
            }
            // 默认选中第 0 个（1/2）。Button_SetCheck 是 BM_SETCHECK 的宏封装。
            // 不预先勾选的话，CurrentScaleIndex() 会走兜底分支，界面状态也不清楚。
            Button_SetCheck(s_hRadios[0], BST_CHECKED);

            // SS_LEFTNOWORDWRAP：单行静态文本，不自动换行——正好适合这种
            // “输出尺寸：... 后缀：...”的一行提示，不会因为太长而折行变高。
            s_hOut = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_RS_OUT);
            // BS_DEFPUSHBUTTON：默认按钮，回车键会触发它（未设置时回车可能没反应）。
            s_hBtn = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_RS_BTN_PROCESS);

            UpdateOutLabel();               // 先把文字初始化好（此时多半还没图，显示“—”）
            Layout();                       // 按当前（占位）尺寸先摆一次位置
        }
        return 0;                           // 0 = 继续创建；返回 -1 会取消窗口创建

    // 窗口大小改变时到达（主窗口拉伸、或主窗口把本页拉到目标大小时）。
    // 这里只做两件事：重新布局 + 让预览重画一次。
    case WM_SIZE:
        Layout();
        // InvalidateRect(hwnd, nullptr, TRUE)：把整个客户区标记为“需要重画”，
        // 第三个参数 TRUE 表示同时擦除背景。窗口改变大小时预览图要跟着重新
        // 缩放绘制，所以必须让它重画。判空是因为 WM_SIZE 理论上可能早于控件创建。
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    // 自定义消息：主窗口刚载入/更换了新图片，通知各功能页刷新。
    // 定义见 App.h:69  #define WM_APP_IMAGE_CHANGED (WM_APP + 2)
    // 说明：WM_APP 以上的消息号是留给应用程序自己用的，不会和系统消息冲突。
    case WM_APP_IMAGE_CHANGED:
        RebuildPreview();                   // 按新源图重建缩略图
        UpdateOutLabel();                   // 源图尺寸变了，输出尺寸文字也要更新
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);  // 触发重绘
        return 0;

    // 自绘控件请求绘制。父窗口收到的是 WM_DRAWITEM，lParam 指向 DRAWITEMSTRUCT，
    // 里面包含要画的控件 ID（CtlID）、设备上下文（hDC）和绘制区域（rcItem）。
    case WM_DRAWITEM:
        if (wParam == IDC_RS_PREVIEW && lParam)   // 确认是自己那个预览控件
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            // 把实际绘制工作交给公共函数：它会先画棋盘格背景（表示透明），
            // 再等比缩放居中画图片，没图时显示 hint 里的提示文字。
            PaintImagePreview(dis->hDC, dis->rcItem, s_pPreview,
                              L"点击上方「选择图片...」载入图片\r\n也可以直接把图片文件拖进窗口");
            return TRUE;                    // TRUE = 我已经画好了，系统不用再处理
        }
        break;                              // 不是我们的控件：交给下面 DefWindowProcW

    // 控件的通知消息都从这里来（按钮点击、文本变化、滚动条等）。
    // 打包规则：LOWORD(wParam) = 控件 ID，HIWORD(wParam) = 通知码，lParam = 控件句柄。
    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);  // 低 16 位是控件 ID
            if (id == IDC_RS_BTN_PROCESS)   // 「处理」按钮
            {
                // 按钮的通知码一定是 BN_CLICKED，这里不必再判断 HIWORD。
                OnProcess(hWnd);
                return 0;                   // 已处理，直接返回；注意这里用 return 而不是 break
            }
            if (id >= IDC_RS_RADIO_BASE && id < IDC_RS_RADIO_BASE + kScaleCount)
            {
                // 单选按钮除了点击还会发别的通知（如获得焦点），
                // 只有 BN_CLICKED 才是“用户选中了它”，这时才需要刷新尺寸文字。
                if (HIWORD(wParam) == BN_CLICKED)
                    UpdateOutLabel();
                return 0;
            }
        }
        break;

    // 其它不关心的消息，统一在下面交给系统默认处理。
    default:
        break;
    }
    // 不处理的消息必须回落给 DefWindowProcW，否则窗口会丢失默认行为。
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  PageResize_Create：创建本功能页窗口（供主窗口调用，声明见 App.h:290）
// ---------------------------------------------------------------------------
//  参数：
//    parent —— 父窗口（主窗口）。本页是它的 WS_CHILD 子窗口，会被贴在右侧区域。
//  返回：
//    创建成功返回页面窗口句柄（初始是 10×10、隐藏状态，主窗口随后会调整），
//    注册窗口类失败时返回 nullptr。
//
//  关于窗口类（WNDCLASS）：
//    每个功能页用一个独立的窗口类，类名各不相同，这样 Proc 就能按类名分派，
//    页面之间互不干扰。RegisterClassExW 同一个类注册两次会失败并返回 0、
//    GetLastError() == ERROR_CLASS_ALREADY_EXISTS；这里用函数内的 static 局部
//    变量做进程内缓存，并且把“类已存在”也当成成功，避免重复调用时报错。
HWND PageResize_Create(HWND parent)
{
    // 函数内的 static：只在第一次执行到这里时初始化为 false，之后跨调用保持。
    // 用它保证“注册窗口类”这件事在整个进程生命周期内只做一次。
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};                // 全 0 初始化，避免残留垃圾值导致注册失败
        wc.cbSize = sizeof(wc);             // 结构体大小：必须填，否则 RegisterClassExW 直接失败
        // CS_HREDRAW | CS_VREDRAW：窗口宽度/高度变化时强制整窗重画。
        // 本页控件都靠 Layout() 手动摆放，加上这两个标志可以避免拉伸时出现残影。
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = Proc;              // 本类的消息处理函数
        wc.hInstance = g_hInst;             // 当前模块实例（全局变量，见 App.h:189）
        // 标准箭头光标。类的光标决定了鼠标移到窗口客户区时的形状。
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 窗口背景色：COLOR_WINDOW + 1 是“系统窗口色”的刷子编号约定
        // （加 1 是为了和真实的 HBRUSH 句柄区分开）。这样跟随系统主题。
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Pic246PageResize";  // 本页窗口类的名字
        // 返回 0 有两种可能：真失败，或者该类早就注册过了。后者视作成功。
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    // 创建页面窗口本身：
    //   · 父窗口传 parent，它就成子窗口，会随父窗口一起移动/隐藏/销毁；
    //   · 前两个样式：WS_CHILD 子窗口；WS_CLIPCHILDREN 表示“绘制父窗口时跳过
    //     子窗口占用的区域”，能有效减少闪烁（子窗口自己会画自己那块）；
    //   · 初始 10×10 只是个合法的小尺寸占位，主窗口随后会用 MoveWindow 调整；
    //   · 菜单参数传 nullptr、ID 传 nullptr（页面窗口不需要 ID）；
    //   · 此时会触发 WM_CREATE，也就是上面 Proc 里那个分支；
    //   · 注意样式里没有 WS_VISIBLE：窗口创建后是隐藏的，由主窗口在切换页面时显示。
    return CreateWindowExW(0, L"Pic246PageResize", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
