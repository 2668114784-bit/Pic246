// ============================================================================
//  PageIco.cpp —— 功能页：ICO 转换
// ============================================================================
// 【文件说明】
//  本文件实现 Pic246 程序的「ICO 转换」功能页。这一页负责把用户已经载入的
//  一张源图，转成 Windows 图标文件（.ico）。ICO 与普通图片最大的不同是：
//  一个 .ico 文件内部可以同时装进多个尺寸的同一张图，Windows 会根据显示
//  场景（桌面大图标 / 任务栏 / 文件列表小图标）自动挑选合适的那一张。
//
//  一、本页的主要职责
//    1. 在页面左侧放一块「交互式裁切画布」，让用户用鼠标拖出想要的那块区域。
//       本页的画布是「正方形模式」（CropCanvas_Create 的第三个参数传 true），
//       因为 ICO 图标在视觉上必须是正方形——长方形的图标会被系统拉伸变形。
//    2. 页面右侧是一组选项：勾选要输出哪些尺寸（16/24/32/48/64/128/256）、
//       选择当「正方形裁切框」超出原图范围时，超出的部分用什么颜色填
//       （或者干脆填成透明）。超出原图的部分是常见需求：把一张长方形照片
//       裁成正方形时，为了不切掉内容，往往让选框比图片大一圈。
//    3. 点击「处理」后，把裁切框里的画面合成到一张真正的正方形位图上，
//       再交给 ImageOps.cpp 里的 SaveIcoWithSuffix() 写成多尺寸 .ico。
//
//  二、本页在整个程序里的位置
//    程序主窗口在 Project001.cpp 里创建，它负责顶栏（选择图片）、左侧边栏、
//    底部状态栏，以及所有功能页的创建与切换。本页只是众多功能页中的一页：
//    Project001.cpp 会调用本文件末尾的 PageIco_Create() 把页面建出来，然后
//    通过 ShowWindow 显隐来切换。页面窗口平时是隐藏的，切到本页才显示。
//
//  三、与其它文件的配合（这些都只调用、不修改）
//    · App.h          ：提供全部公共声明——控件 ID（IDC_IC_*）、自定义消息
//                       （WM_APP_IMAGE_CHANGED / WM_APP_RECT_CHANGED）、
//                       全局变量（g_pSrcImage / g_srcW / g_srcH / g_fillColor）、
//                       辅助函数（D() / CreateCtl() / IntToStr() …）。
//    · CropCanvas.cpp ：交互式裁切画布的完整实现。本页用到的接口有：
//                       CropCanvas_Create()          创建画布，squareMode=true 表示只允许正方形选框
//                       CropCanvas_GetRect()         读回当前选框的 x / y / 宽 / 高（单位是源图像素）
//                       CropCanvas_SetFill()         告诉画布「超出图片的部分」怎么填充
//                       CropCanvas_RefreshImage()    源图换人后让画布丢掉旧缓存，重新缓存新图
//                       CropCanvas_ResetToSquareCenter()  把选框恢复成「图片中央最大正方形」
//                       画布每次拖动选框都会给本页发 WM_APP_RECT_CHANGED，本页据此刷新信息文字。
//    · ImageOps.cpp   ：SaveIcoWithSuffix() 是真正干活的那个函数——它负责把
//                       正方形位图按每个尺寸缩放、编码成 ICO 里的一个个条目、
//                       组装文件头、写盘、遇到同名文件时弹「是否覆盖」、
//                       失败时弹错误框，最后还要弹「完成」提示并可以打开所在文件夹。
//                       本页只需把「合成好的正方形位图 + 尺寸列表」交给它即可，
//                       连内存释放（delete square）都由它内部负责。
//
//  四、消息驱动的整体流程（本页的 Proc() 就是围绕下面几条消息转的）
//    WM_CREATE            页面第一次被创建时收到：建出画布 / 尺寸复选框 /
//                        填充色按钮 / 信息文字 / 重置按钮 / 处理按钮，
//                        然后做一次初始布局。所有控件都是本页的子窗口。
//    WM_SIZE              本页尺寸变化时收到（主窗口切页、拉窗口都会触发）：
//                        重排全部控件。本页不自建滚动条，窗口太小时靠夹取下限兜底。
//    WM_COMMAND          任何子控件被点击（按钮、复选框）都会转发到这里；
//                        用 LOWORD(wParam) 取控件 ID 分发处理。
//    WM_APP_IMAGE_CHANGED 顶栏换了一张源图，主窗口向所有页广播这条自定义消息：
//                        本页要让画布换图、把填充设置重新同步、刷新信息文字。
//    WM_APP_RECT_CHANGED  画布上选框被拖动 / 缩放了，画布主动通知本页：
//                        本页只需更新右侧「正方形区域 / 输出尺寸」信息即可。
//    WM_DRAWITEM          自绘消息。那个显示当前填充色的小方块（颜色样例）
//                        是 SS_OWNERDRAW 静态控件，系统不知道怎么画它，
//                        所以由本页在这里调用 PaintColorSwatch() 亲手画。
// ============================================================================
#include "App.h"
// App.h 里已经 include 了 windows.h / gdiplus.h / <vector> / <string> 等，
// 所以本文件不再重复包含；本页需要的所有 ID、消息、全局变量都在这个头文件里。

using namespace Gdiplus;
// GDI+ 的类都在 Gdiplus 命名空间里（Bitmap / Graphics / Color / ImageAttributes …）。
// 这里写一行 using，后面就不用到处写 Gdiplus::Bitmap 这么长的前缀了。

// ---------------------------------------------------------------------------
// 本页的控件句柄
// 全用 static（文件作用域）保存：一页只有一份控件，切成其它页再过回来时，
// 句柄还在，不用重建；外部文件也不需要看到它们，所以不必放进头文件。
// 命名约定：s_h 开头 = static handle。
// ---------------------------------------------------------------------------
static HWND s_hPage = nullptr;      // 本页的页面窗口句柄（就是 Proc() 收到 WM_CREATE 的那个 hWnd，Layout() 靠它取客户区大小）
static HWND s_hCanvas = nullptr;    // 左侧交互式裁切画布（由 CropCanvas.cpp 实现，本页只保存句柄并调用它的接口）
static HWND s_hGrpSize = nullptr;   // 右上「输出的 ICO 尺寸（像素）」分组框（BS_GROUPBOX，纯装饰，不响应点击）
static HWND s_hGrpFill = nullptr;   // 右中「超出图片部分的填充」分组框（同样是纯装饰）
static HWND s_hSwatch = nullptr;    // 颜色样例小方块（SS_OWNERDRAW 自绘静态控件，画的就是当前填充色）
static HWND s_hBtnColor = nullptr;  // 「选择填充颜色...」按钮，点它弹系统取色对话框
static HWND s_hChkTrans = nullptr;  // 「透明（推荐）」复选框：勾上则超出部分不填色而是透明
static HWND s_hChks[ICO_SIZE_COUNT] = {};
// 7 个尺寸复选框的句柄数组，顺序与 kIcoSizes（16/24/32/48/64/128/256）一一对应。
// 它们的控件 ID 是连续的：IDC_IC_CHK_BASE + i（见 WM_CREATE），
// 这样 WM_COMMAND 里用一句范围判断就能覆盖 7 个复选框，不用写 7 个 if。
static HWND s_hInfo = nullptr;      // 右侧信息静态文字：显示当前正方形区域尺寸 + 将要输出的尺寸列表
static HWND s_hBtnReset = nullptr;  // 「重置裁切框（居中正方形）」按钮
static HWND s_hBtnProcess = nullptr;// 「处理」按钮（BS_DEFPUSHBUTTON，回车键等价于点它）

// 默认勾选：16 / 32 / 48 / 256
// 这 4 个是 Windows 图标最常用的尺寸：
//   16  —— 资源管理器「详细信息/列表」视图、窗口左上角小图标
//   32  —— 桌面、任务栏
//   48  —— 「中等图标」视图
//   256 —— 「大图标/超大图标」视图与商店应用磁贴；256 这一档在 SaveIcoWithSuffix()
//          内部走的是 PNG 编码分支（见下方 OnProcess 的说明），文件才不会爆大。
// 24/64/128 属于「锦上添花」，默认不打勾，避免生成的 .ico 体积无谓变大。
static const bool kDefaultChecked[ICO_SIZE_COUNT] = { true, false, true, true, false, false, true };

// ---------------------------------------------------------------------------
// FillTransparent：读出「透明」复选框当前是否被勾选。
//   参数：无（内部直接用全局句柄 s_hChkTrans）。
//   返回：true = 用户选择「超出图片的部分填成透明」；false = 填成 g_fillColor 指定的纯色。
//   注意：只有控件已创建后才能安全调用；本页所有调用点都在 WM_CREATE 之后，
//         此时 s_hChkTrans 一定非空。若传 nullptr 给 Button_GetCheck，会返回 0，
//         结果等价于「不透明」，不会崩，所以这里不加空判断也安全。
// ---------------------------------------------------------------------------
static bool FillTransparent()
{
    return Button_GetCheck(s_hChkTrans) == BST_CHECKED;
}

// ---------------------------------------------------------------------------
// PushFillToCanvas：把「填充色 + 是否透明」这两个设置推给画布，并让颜色样例重画。
//   参数：无。
//   返回：无。
//   为什么需要它：填充设置存在两处——全局变量 g_fillColor / 复选框（本页的真相来源），
//     和画布内部的副本（它画「超出图片」那块区域时要用）。两处必须保持同步，
//     所以任何改动填充设置的地方（WM_CREATE 初始化、勾选/取消「透明」复选框、
//     选完颜色、换源图）都要调一次本函数，把设置「推」过去。
//   坑：CropCanvas_SetFill() 内部会让画布重画，但那个颜色样例小方块（s_hSwatch）
//     是本页自己画的，画布重画带不动它，所以下面要额外 InvalidateRect 一次；
//     第三个参数 TRUE 表示「顺便擦掉背景」，否则透明切换后可能残留上一帧的颜色。
//   坑：用 InvalidateRect（只登记重画、异步）而不是 UpdateWindow（立刻重画），
//     是为了避免在拖动过程中反复强行重绘造成闪烁。
// ---------------------------------------------------------------------------
static void PushFillToCanvas()
{
    if (s_hCanvas)
        CropCanvas_SetFill(s_hCanvas, g_fillColor, FillTransparent());
    // 把当前颜色与透明开关一起交给画布；g_fillColor 是两页（裁切页/本页）共用的全局填充色。
    if (s_hSwatch)
        InvalidateRect(s_hSwatch, nullptr, TRUE);
    // 颜色样例方块要跟着变色；nullptr = 整个控件区域都重画。
}

// ---------------------------------------------------------------------------
// CheckedSizes：收集用户当前勾选的全部尺寸，按 16→256 的固定顺序返回。
//   参数：无。
//   返回：std::vector<int>，元素取自 kIcoSizes，例如 {16, 32, 256}；
//         一个都没勾时返回空 vector（调用方负责提示用户）。
//   注意：这里返回的是 kIcoSizes 里的「像素值」而不是数组下标，因为
//         SaveIcoWithSuffix() 需要的就是像素值。
//   注意：内层 if 没有加大括号是原代码风格，逻辑就是「勾了就加一个」，不要改动。
// ---------------------------------------------------------------------------
static std::vector<int> CheckedSizes()
{
    std::vector<int> v;
    for (int i = 0; i < ICO_SIZE_COUNT; ++i)
        if (Button_GetCheck(s_hChks[i]) == BST_CHECKED)
            v.push_back(kIcoSizes[i]);
    return v;
}

// ---------------------------------------------------------------------------
// UpdateInfo：刷新右侧那块信息文字。
//   参数：无。
//   返回：无。
//   显示内容形如：
//       正方形区域：320 × 320 像素
//       输出尺寸：16, 32, 256
//       后缀：_ico
//   注意：函数开头要判空。因为本函数会在 WM_CREATE 里被调用，也会被
//         WM_APP_RECT_CHANGED 触发；如果源图还没载入（g_pSrcImage 为空）
//         或画布不存在（s_hCanvas 为空），就没有尺寸可读，直接显示一个破折号。
//   注意：g_pSrcImage 为空时不能去调 CropCanvas_GetRect()——画布内部要访问源图，
//         空指针进去会被里面的防御性代码挡下，但这里提前返回更省事也更安全。
//   坑：swprintf_s 的格式串里用的是 %d，而参数先转成 int；裁切框坐标是 float，
//     直接用 %f 打印出来会是一长串小数，不好看。加 0.5f 再取整 = 四舍五入，
//     不要用 (int)(w) 直接截断，否则 320.9 会显示成 320。
// ---------------------------------------------------------------------------
static void UpdateInfo()
{
    if (!s_hInfo) return;
    if (!g_pSrcImage || !s_hCanvas)
    {
        SetWindowTextW(s_hInfo, L"裁切区域：—");
        return;
    }

    float x = 0, y = 0, w = 0, h = 0;
    CropCanvas_GetRect(s_hCanvas, &x, &y, &w, &h);
    // 向画布要当前选框的位置与宽高。这里只需要 w / h，x / y 是为接口完整性而填的变量。
    std::vector<int> sizes = CheckedSizes();

    std::wstring list;
    for (size_t i = 0; i < sizes.size(); ++i)
    {
        if (i) list += L", ";
        // 第一个元素前面不加逗号，后面的都加，拼成 "16, 32, 256" 这种形式。
        list += IntToStr(sizes[i]);
    }
    if (list.empty()) list = L"（未勾选）";
    // 全都没勾时给个中文占位提示，比显示空白友好。

    wchar_t buf[512];
    swprintf_s(buf, L"正方形区域：%d × %d 像素\r\n输出尺寸：%s\r\n后缀：_ico",
               (int)(w + 0.5f), (int)(h + 0.5f), list.c_str());
    // 用 \r\n 而不是 \n：Win32 的 Static 控件要靠 \r\n 才能正确换行，
    // 只写 \n 时字会全挤在一行上（经典坑）。
    // 后缀固定 _ico，是给用户一个预期：输出文件名 = 原文件名 + "_ico.ico"。
    SetWindowTextW(s_hInfo, buf);
}

// ---------------------------------------------------------------------------
// Layout：按当前页面客户区大小，重新摆放本页所有控件（手动布局，没有用任何布局库）。
//   参数：无，内部用 GetClientRect(s_hPage) 取页面尺寸。
//   返回：无。
//   布局形状（从左到右）：
//       ┌──────────────────────┬──────────────┐
//       │                      │  尺寸分组框   │
//       │     裁切画布          ├──────────────┤
//       │   （占据剩余宽度）      │  填充分组框   │
//       │                      │  信息文字     │
//       │                      │  重置按钮     │
//       │                      │  ↓           │
//       │                      │  处理按钮（贴底）│
//       └──────────────────────┴──────────────┘
//   关键点 1：所有尺寸都写成 D(逻辑像素)，D() 会按屏幕 DPI 把它换算成真实像素，
//             这样在 125% / 150% 缩放的屏幕上控件不会显得过小、文字不会被截断。
//   关键点 2：右边栏宽度先按 D(250) 取，但绝不允许超过页面宽度的一半（cx/2），
//             否则窗口被拉得很窄时画布会被挤没。
//   关键点 3：画布宽度设了最小 D(120) 兜底；注意这只是「不被压成 0 宽」的下限，
//             如果窗口比这个下限还窄，控件会轻微重叠或多出一截，属于可接受的取舍
//             （本页没有做滚动条/自适应换行那套复杂逻辑）。
//   坑：MoveWindow 的最后一个参数 TRUE = 立即重画该子窗口。布局期间会连续调用十几个
//     MoveWindow，因此会有多次重绘；由于本页窗口带 WS_CLIPCHILDREN 且画布自绘，
//     实际看不出闪烁，可以放心用 TRUE。
// ---------------------------------------------------------------------------
static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);
    // 取本页客户区（不含标题栏、边框）；因为本页是无边框子窗口，基本等于窗口大小。
    const int cx = rc.right;
    const int cy = rc.bottom;
    // 客户区左上角恒为 (0,0)，所以宽 = right、高 = bottom，不必再减 left/top。
    const int M = D(12);
    // M = Margin，四周边距，统一用这一个值让间距看起来整齐。

    int panelW = D(250);
    // 右侧选项栏的理想宽度。
    if (panelW > cx / 2) panelW = cx / 2;
    // 窗口太窄时，右栏最多占一半；剩下的一半留给画布。
    // 注意：此处并未给 panelW 设最小值——极端窄窗口下右栏会跟着变小，
    // 但控件文字可能显示不全，这是本页已知的妥协点。

    int canvasW = cx - panelW - M * 3;
    // 画布宽度 = 总宽 - 右栏宽 - 左边距 - 画布右间距 - 右栏左间距（三份 M）。
    if (canvasW < D(120)) canvasW = D(120);
    // 兜底：无论怎么算，画布至少留 120 逻辑像素宽，否则一旦为负 MoveWindow 会失败。
    MoveWindow(s_hCanvas, M, M, canvasW, cy - M * 2, TRUE);
    // 画布上下各留一个 M 的边距，所以高度 = 客户区高 - 2*M。

    int px = M * 2 + canvasW;
    // px = 右栏的起始 X：左边距 + 画布宽 + 一个间距。
    int pw = cx - px - M;
    // pw = 右栏可用宽度：从 px 到右边距之间的部分。
    int py = M;
    // py = 当前摆放位置的光标（Y 方向），每放完一块就往下累加，像排版一样自上而下流式摆放。

    int grpH = D(168);
    // 尺寸分组框高度：标题 1 行 + 4 行复选框（7 个复选框排两列，共 4 行）。
    MoveWindow(s_hGrpSize, px, py, pw, grpH, TRUE);
    {
        int colW = (pw - D(30)) / 2;
        // 两列复选框，每列宽度 = (右栏宽 - 左右内边距) / 2。
        // D(30) 是留给分组框左右内缩和中间的间隔。
        for (int i = 0; i < ICO_SIZE_COUNT; ++i)
        {
            int col = i % 2;
            int row = i / 2;
            // 偶数下标放左列、奇数下标放右列，每隔两个换一行：
            //   16 32
            //   48 64
            //   128 256
            // 这样 7 个复选框正好排成 4 行，控件顺序（Tab 键顺序）仍按 16→256，符合直觉。
            MoveWindow(s_hChks[i], px + D(16) + col * colW, py + D(32) + row * D(32),
                       colW, D(26), TRUE);
            // 起点向内缩 D(16)（避开分组框边框），纵向从 D(32) 起（给分组框标题让位），
            // 行高 D(32)，控件自身高 D(26)——留 6 像素行间距，鼠标更容易点中。
        }
    }

    py += grpH + D(10);
    // 摆完尺寸框，光标下移「框高 + 间距」。
    int fillH = D(122);
    // 填充分组框高度：颜色样例 + 选色按钮 + 透明复选框 三行内容。
    MoveWindow(s_hGrpFill, px, py, pw, fillH, TRUE);
    MoveWindow(s_hSwatch, px + D(16), py + D(30), D(40), D(26), TRUE);
    // 颜色样例：40×26 的小矩形，横向缩进 D(16)，纵向在标题下方 D(30)。
    MoveWindow(s_hBtnColor, px + D(66), py + D(28), pw - D(82), D(30), TRUE);
    // 选色按钮紧跟在颜色样例右侧：X = D(16)+D(40)+D(10) = D(66)；
    // 宽度 = 右栏宽 - 左侧已占用部分 - 右内边距 = pw - D(82)。
    MoveWindow(s_hChkTrans, px + D(16), py + D(64), pw - D(32), D(24), TRUE);
    // 透明复选框：另起一行，左右各缩进 D(16)。
    // 注意：这个复选框本身是 BS_AUTOCHECKBOX——系统会自动切换勾选状态，
    // 所以 WM_COMMAND 里收到点击后不需要手动 Button_SetCheck。

    py += fillH + D(8);
    // 继续下移，间距改成 D(8)（比上面 D(10) 略小，视觉上更紧凑）。
    MoveWindow(s_hInfo, px, py, pw, D(70), TRUE);
    // 信息文字区：三行文字，留 D(70) 高度足够。

    py += D(74);
    // D(70) 控件高 + D(4) 间距 = D(74)，接着摆「重置」按钮。
    MoveWindow(s_hBtnReset, px, py, pw, D(34), TRUE);

    int btnH = D(42);
    // 「处理」按钮的高度。
    MoveWindow(s_hBtnProcess, px, cy - M - btnH, pw, btnH, TRUE);
    // 处理按钮是页面主操作，特意贴底摆放（底边距 = M），不从 py 顺序摆放，
    // 这样无论右栏内容多少，它都固定在右下角，容易被找到。
}

// ---------------------------------------------------------------------------
// OnProcess：本页的核心动作——点「处理」后把裁切框内容合成成正方形位图，
//            然后交给 SaveIcoWithSuffix() 写成多尺寸 .ico。
//   参数：hPage —— 本页窗口句柄，用作 MessageBox 的父窗口（消息框会挂在页面上，
//                  并且自动禁用本页，避免用户在弹框期间又点按钮）。
//   返回：无（成功/失败都通过状态栏和消息框反馈，本页不需要返回值）。
//   执行步骤：
//     1. 前置检查：有没有源图？有没有勾选尺寸？任一不满足就弹提示并返回。
//     2. 读出裁切框，算出正方形边长（取宽高平均值并四舍五入）。
//     3. 边长过大（> 8192）时弹「是否继续？」让用户自己决定。
//     4. 状态栏提示「正在处理」，并强制刷新一次界面，让用户立刻看到反馈。
//     5. new 一张边长×边长的 32 位带 Alpha 位图，先整体填色（透明或纯色）。
//     6. 把源图「平移 -x,-y」画上去：选框左上角正好落到新位图的 (0,0)，
//        这样超出图片的部分就保留为第 5 步的底色——这正是「填充」设置的意义。
//     7. 把位图连同尺寸列表交给 SaveIcoWithSuffix() 收尾。
//   坑 1：new 出来的 Bitmap 不需要手动 delete。第 7 步的 SaveIcoWithSuffix()
//         无论成功失败都会 delete square（见 ImageOps.cpp 该函数各处分支），
//         在这里再 delete 一次会二次释放。唯一要自己 delete 的是 new 失败的
//         那种情况（GetLastStatus() != Ok）。
//   坑 2：必须让 Graphics 先以 CompositingModeSourceCopy + Clear() 铺底，再切回
//         CompositingModeSourceOver 画源图。如果一上来就用 SourceOver，
//         Clear() 会变成「与原有像素混合」，在部分驱动上底色的 Alpha 会不干净。
//   坑 3：源图超出新位图的部分由 GDI+ 自动裁剪，不需要我们算裁剪矩形；
//         反而是「选框超出图片」的部分靠第 5 步的底色来体现，方向不要弄反。
// ---------------------------------------------------------------------------
static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage || !s_hCanvas)
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    // 没有源图 / 没有画布时，点处理没有任何意义，直接提示用户去顶栏选图。
    // 用 MB_ICONINFORMATION（蓝色 i 图标）而不是警告图标——这不是错误。

    std::vector<int> sizes = CheckedSizes();
    if (sizes.empty())
    {
        MessageBoxW(hPage, L"请至少勾选一个 ICO 尺寸。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // 一个尺寸都没勾的话，即使勉强生成，.ico 里也没有任何内容，
    // 与其到最后才失败，不如在入口就挡住。
    // 注意：SaveIcoWithSuffix() 内部其实也做了一模一样的检查（双保险），
    //      这里先拦一道是为了更早给出反馈，且不让流程走到状态栏提示那一步。

    float x = 0, y = 0, w = 0, h = 0;
    CropCanvas_GetRect(s_hCanvas, &x, &y, &w, &h);
    // x / y 是选框左上角在「源图像素坐标系」里的位置；w / h 是选框宽高。
    // 因为是正方形模式，理论上 w 和 h 是相等的，但拖动过程中可能有一两个像素的
    // 浮点误差，所以下面取平均再四舍五入，比直接用 w 更稳。

    int side = (int)((w + h) * 0.5f + 0.5f);
    // (w+h)/2 = 平均边长；+0.5f 后取整 = 四舍五入。
    if (side < 4) side = 4;
    // 下限 4 像素：太小的裁切框没有意义，而且某些 GDI+ 路径对 1~2 像素图会有奇怪表现。
    // 这个夹取同时防止下面 new Bitmap 抛「宽度必须大于 0」的失败。
    if (side > 8192)
    {
        // 上限保护：8192×8192×4 字节 ≈ 256MB，再加上多层中间位图，内存很容易吃紧；
        // 超过就交给用户决定（比如他可能确实是想要一张 4096 的大图标）。
        wchar_t q[512];
        swprintf_s(q, L"正方形区域为 %d × %d，尺寸偏大。\r\n\r\n是否继续？", side, side);
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        // 注意这里只提示、不强制拦截——用户点「否」就干净地返回，什么都不改。
        // 选择 MB_YESNO | MB_ICONQUESTION（问号图标），因为这是「要不要继续」而不是错误。
    }

    const bool trans = FillTransparent();
    // 把「是否透明」读一次存到局部变量：后面要用来选填底色，而且循环外读取
    // 可以保证整个合成过程用的是同一个值（避免期间状态变化导致前后不一致）。

    SetAppStatus(L"正在处理，请稍候...");
    UpdateWindow(g_hMain);
    // 先在底部状态栏写一句提示，然后立刻 UpdateWindow 强制重画主窗口。
    // 为什么要手动更新：接下来会同步做图像缩放和编码，期间不会回到消息循环，
    // 状态栏文字如果不强制刷新，用户会以为程序卡死了。
    // 注意 g_hMain 是主窗口（状态栏、顶栏都在它身上），不是本页窗口。

    Bitmap* sq = new Bitmap(side, side, PixelFormat32bppARGB);
    // 目标正方形位图。像素格式选 PixelFormat32bppARGB：每像素 4 字节，
    // 带 8 位 Alpha 通道——必须带 Alpha，否则「透明填充」根本无从谈起
    // （ICO 的透明正是靠 32bpp 位图的 Alpha 来表达的）。
    // 用 new 而不是栈对象：GDI+ 的 Bitmap 构造失败（内存不足）不会抛异常，
    // 而是把状态记在内部，需要靠 GetLastStatus() 查；new 也让它在
    // SaveIcoWithSuffix() 里被 delete 的时机更明确。
    if (sq->GetLastStatus() != Ok)
    {
        delete sq;
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }
    // GDI+ 的失败是「静默」的，所以对象建好后必须查一次 GetLastStatus()。
    // 失败时先 delete 掉这个半残对象，再弹错误框 + 更新状态栏，然后返回。
    // 这是本函数唯一一处需要自己 delete 的地方。

    {
        // 用一对大括号把 Graphics 关在一个作用域里。
        // 目的：让 Graphics g 在花括号结束时就析构——GDI+ 的 Graphics 在析构时会
        // 结束绘制并把缓存刷回位图。后面的 SaveIcoWithSuffix() 要读这张位图，
        // 如果 g 还活着，位图内容可能还没最终落到内存里（某些驱动上是延迟提交的）。
        Graphics g(sq);
        g.SetCompositingMode(CompositingModeSourceCopy);
        // SourceCopy = 新像素直接覆盖旧像素（不做 Alpha 混合）。
        // 铺底色时必须是这个模式：如果用默认的 SourceOver，往一张
        // 未初始化的位图上画半透明色会得到乱七八糟的结果。
        if (trans)
            g.Clear(Color(0, 0, 0, 0));
        // 透明填充：全 0 就是完全透明，四个分量（A,R,G,B）都是 0。
        // 选黑色而不是白色无所谓——A=0 时颜色分量不参与显示，GDI+ 也归零处理。
        else
            g.Clear(Color(255, GetRValue(g_fillColor), GetGValue(g_fillColor), GetBValue(g_fillColor)));
        // 纯色填充：把 COLORREF（0x00BBGGRR 排列）拆成 R/G/B 三个分量，
        // 再组成 GDI+ 的 Color(A,R,G,B)。注意顺序：Color 构造函数的参数是
        // (Alpha, Red, Green, Blue)，和 COLORREF 的高低字节顺序不一样，
        // 这里必须用 GetRValue/GetGValue/GetBValue 转换，不能直接强转。
        // Alpha 固定 255 = 完全不透明。

        g.SetCompositingMode(CompositingModeSourceOver);
        // 画源图时要切回 SourceOver：源图自己可能带透明通道（PNG/GIF），
        // SourceOver 才能把它正确地叠到底色上。
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        // 双三次插值：缩放时画质最好（代价是慢一点，但只缩放一次可以接受）。
        // 注意：本函数里其实没有缩放，真正缩放发生在 SaveIcoWithSuffix() 里；
        // 这些质量参数在这里主要是给「源图坐标系到目标位图」的变换兜底。
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // 高质量像素偏移：让缩放采样点的中心对齐，消除半像素偏移导致的模糊边缘。
        g.SetCompositingQuality(CompositingQualityHighQuality);
        // 合成质量设为高质量（会做 Gamma 校正），避免叠图后颜色发灰。

        ImageAttributes attr;
        attr.SetWrapMode(WrapModeTileFlipXY);
        // 平铺模式。这里是为了配合后面的 DrawImage 重载：该重载在源矩形
        // 超出图像边界时，需要 WrapMode 决定「越界区域怎么取像素」。
        // TileFlipXY = 以镜像方式平铺（画到画布之外也不会报错）。
        // 关键点：因为选框可以比源图大，g.DrawImage 的源矩形会落在图像外面，
        // 不设 WrapMode 的话 GDI+ 可能直接拒绝绘制或画出黑边。

        RectF dest(-x, -y, (REAL)g_srcW, (REAL)g_srcH);
        // 把整张源图按原尺寸放到「负的选框坐标」处：
        // 源图的 (x,y) 点正好落在目标位图的 (0,0)，即选框左上角对齐目标左上角。
        // 于是目标位图里显示的就是选框范围内的画面，选框之外（源图覆盖不到）
        // 自然保留为前面 Clear() 铺的底色——这就是填充逻辑的全部秘密。
        g.DrawImage(g_pSrcImage, dest,
                    0.0f, 0.0f, (REAL)g_srcW, (REAL)g_srcH, UnitPixel, &attr);
        // 参数含义依次为：源图、目标矩形、源矩形左上角、源矩形尺寸、单位（像素）、图像属性。
        // 源矩形从 (0,0) 开始、宽高取整张图，等于「原样整张画上去」。
        // 注意：源图宽高用的是全局 UINT g_srcW / g_srcH，它们由 LoadImageFile()
        // 在载入图片时维护，这里直接信任（本函数开头已确认 g_pSrcImage 非空）。
    }

    SaveIcoWithSuffix(sq, sizes, hPage, nullptr);
    // 收尾工作全部交给它（ImageOps.cpp）：
    //   · 按尺寸列表逐个缩放并用 BuildBmpEntry（<256）或 EncodePngToMemory（>=256）编码；
    //     256 那一档之所以用 PNG：256×256×4 的裸位图再加上 AND 掩码一张就要 256KB+，
    //     4 张就超过 1MB，而 PNG 对图标这种大面积纯色压缩率极高（通常只剩几 KB）。
    //   · 按 ICO 格式组装 6 字节文件头 + 每个条目 16 字节目录项 + 各自数据；
    //   · 输出路径固定为「原图同目录 + 原文件名 + _ico.ico」；
    //   · 同名文件存在时弹「是否覆盖」，用户拒绝则取消（此时它也会 delete square）；
    //   · 最后弹「完成」对话框并可一键打开所在文件夹，同时更新底部状态栏。
    // 第四个参数 donePath 传 nullptr：本页不需要拿回输出文件路径。
    // 重要：sq 的所有权从这里移交给了 SaveIcoWithSuffix()，本页不要再碰它、更不要 delete。
}

// ---------------------------------------------------------------------------
// Proc：本页的窗口过程（window procedure）。本页所有消息都先到这里，由本函数分发。
//   参数：hWnd    —— 收到消息的窗口（本页窗口，或是子控件？注意：控件消息不进这里，
//                     控件点击是通过 WM_COMMAND 汇总后进来的）。
//         message —— 消息号，如 WM_CREATE / WM_SIZE / WM_COMMAND / WM_DRAWITEM。
//         wParam  —— 附加参数，含义随消息而定（WM_COMMAND 里低 16 位是控件 ID）。
//         lParam  —— 附加参数（WM_COMMAND 里是子控件句柄，WM_DRAWITEM 里是结构体指针）。
//   返回：处理过的消息返回具体结果（如 WM_DRAWITEM 返回 TRUE 表示已自绘）；
//         没处理的一律交给 DefWindowProcW() 走系统默认处理。
//   结构说明：一个 switch + 每个分支 return，没有 fallthrough 的意图，
//             default 分支只是 break 掉再去调 DefWindowProcW，属于标准写法。
//   坑：WM_DRAWITEM / WM_COMMAND 里判断完自己关心的控件后，必须 break 落到
//     DefWindowProcW，不能无脑 return 0——否则分组框、Static 等控件的默认行为
//     （比如系统绘制的边框、焦点处理）会被吃掉，界面会出现看不见的异常。
// ---------------------------------------------------------------------------
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            s_hPage = hWnd;
            // 先把本页窗口句柄存下来，Layout() 后面要靠它 GetClientRect。

            s_hCanvas = CropCanvas_Create(hWnd, IDC_IC_CANVAS, true);
            // 创建交互式裁切画布，并把它作为本页的子窗口。
            // 第三个参数 true = squareMode：画布会强制选框保持正方形，
            // 并限制选框不能大于……（具体约束见 CropCanvas.cpp），这正是图标所需。
            // 注意：这里的 IDC_IC_CANVAS 在 App.h 里定义，与另一个功能页的
            // IDC_CP_CANVAS 必须不同，否则同一父窗口下句柄会冲突。

            s_hGrpSize = CreateCtl(L"Button", L"输出的 ICO 尺寸（像素）", WS_VISIBLE | BS_GROUPBOX,
                                   hWnd, IDC_IC_GRP_SIZE);
            // BS_GROUPBOX：系统自动画一个带标题的凹槽边框，纯装饰，
            // 用户点它的标题不会产生 WM_COMMAND，所以 WM_COMMAND 里不用管它。
            for (int i = 0; i < ICO_SIZE_COUNT; ++i)
            {
                wchar_t text[16];
                // 每个复选框标题就是它的像素值，最长 "256" 三个字符加结束符，16 足够。
                swprintf_s(text, L"%d", kIcoSizes[i]);
                // 标题不写死数组，直接取 ImageOps.cpp 里的 kIcoSizes，
                // 这样万一以后改了尺寸表，界面会自动跟着变（与逻辑永远一致）。
                s_hChks[i] = CreateCtl(L"Button", text, WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       hWnd, IDC_IC_CHK_BASE + i);
                // BS_AUTOCHECKBOX：系统自动处理「点击切换勾选」并重画。
                // ID 用 IDC_IC_CHK_BASE + i 连续编号，WM_COMMAND 里一个范围判断即可。
                if (kDefaultChecked[i])
                    Button_SetCheck(s_hChks[i], BST_CHECKED);
                // 按 kDefaultChecked 设置初始勾选状态（16/32/48/256 默认选中）。
                // 注意：这里必须显式 SetCheck——BS_AUTOCHECKBOX 只负责「用户点击后翻转」，
                // 初始状态仍然是未勾选，不设置的话默认值就是空。
            }

            s_hGrpFill = CreateCtl(L"Button", L"超出图片部分的填充", WS_VISIBLE | BS_GROUPBOX,
                                   hWnd, IDC_IC_GRP_FILL);
            // 第二个分组框：标题点明了本页最容易困惑的地方——选框比图片大时，
            // 多出来的那块区域要怎么处理，就是由这一组控件决定的。
            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_IC_SWATCH);
            // SS_OWNERDRAW：告诉系统「这个 Static 不用你画」，
            // 系统改为给父窗口发 WM_DRAWITEM，由本页的 Proc() 亲手绘制（见下面 WM_DRAWITEM）。
            // 用它来实时展示当前填充色，比只写一句文字直观得多。
            s_hBtnColor = CreateCtl(L"Button", L"选择填充颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_IC_BTN_COLOR);
            // 点它会调用 ChooseFillColor()（ImageOps.cpp），弹系统的颜色选择对话框。
            s_hChkTrans = CreateCtl(L"Button", L"透明（推荐）", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    hWnd, IDC_IC_CHK_TRANS);
            // 「透明（推荐）」：为什么推荐？因为图标通常会被放在各种颜色的背景上
            // （深色任务栏、浅色桌面），把多余部分抠透明才能融入任何背景；
            // 填一个固定颜色则可能在某些背景下显得突兀。
            Button_SetCheck(s_hChkTrans, BST_CHECKED);
            // 默认勾选透明——这是绝大多数场景想要的默认值。

            s_hInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_IC_INFO);
            // SS_LEFTNOWORDWRAP：左对齐 + 不自动折行。
            // 必须禁用自动折行，否则「输出尺寸：16, 32, 48…」这种长行会被系统
            // 在随机位置断成两行，把固定高度的信息区撑乱；换行由我们自己用 \r\n 控制。
            s_hBtnReset = CreateCtl(L"Button", L"重置裁切框（居中正方形）", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_IC_BTN_RESET);
            // 重置按钮：用户拖乱了选框之后，一键回到「图片中央最大正方形」这个
            // 最常用的起点（对应 CropCanvas_ResetToSquareCenter）。
            s_hBtnProcess = CreateCtl(L"Button", L"处理", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      hWnd, IDC_IC_BTN_PROCESS);
            // BS_DEFPUSHBUTTON：把这个按钮标成「默认按钮」，边框会更醒目，
            // 用户在页面上按回车键等价于点它——减少一次鼠标移动。
            // 注意：本页没有自己处理 WM_GETDLGCODE / 键盘消息，默认按钮的实现
            // 依赖 DefWindowProcW 的对话框消息转发，所以上面 return 0 之后
            // 一定要 fallthrough 到 DefWindowProcW。

            PushFillToCanvas();
            // 控件都建好了，把初始填充设置（g_fillColor + 勾选的「透明」）推给画布，
            // 否则画布会用它自己的默认值，跟界面显示不一致。
            UpdateInfo();
            // 先调一次信息刷新，避免刚建好页面时信息区是空白的。
            Layout();
            // 最后做首次布局：WM_CREATE 时页面还是 10×10 的占位大小（见 PageIco_Create），
            // 真正摆正位置要等紧跟着的 WM_SIZE，但这里先排一次可以避免一帧的错位。
        }
        return 0;
        // WM_CREATE 返回 0 表示「创建继续」；若返回 -1 则窗口创建失败。
        // 本页所有控件都建成功与否没有做检查，这是简化处理：
        // CreateCtl 内部失败也只返回 nullptr，后续代码用 if (s_hXxx) 兜底。

    case WM_SIZE:
        Layout();
        return 0;
        // 尺寸变了就重排全部控件。主窗口切换页面对本页来说也是显隐，
        // 但拉大拉小主窗口就会走到这里，因此布局必须是无状态、可反复调用的。
        // 用 lParam 里的宽高也可以，但 Layout() 统一用 GetClientRect 更不容易写错。

    case WM_APP_IMAGE_CHANGED:
        // 顶栏换了源图后，主窗口（Project001.cpp）会向所有页面广播这条自定义消息。
        if (s_hCanvas)
            CropCanvas_RefreshImage(s_hCanvas);
        // 画布内部缓存了旧图的缩放副本，换图后必须让它丢掉旧缓存，
        // 否则画布上还显示上一张图（这是最容易忘、且现象最迷惑人的一步）。
        PushFillToCanvas();
        // 重新把填充设置推给画布：画布换图后内部状态被重置，
        // 需要重新告知填充色/透明选项，顺便让颜色样例重画一次。
        UpdateInfo();
        // 图换了，裁切框通常也被画布重置了，信息文字要跟着更新。
        return 0;

    case WM_APP_RECT_CHANGED:
        UpdateInfo();
        return 0;
        // 画布上选框被拖动/缩放后，画布主动发来这条消息。
        // 本页需要做的事很少——只是把「正方形区域」的像素尺寸刷新到界面上，
        // 让用户一边拖一边能看到实时数字。
        // 注意：这是高频消息（拖动过程中持续发送），所以这里只做必要的
        // SetWindowTextW，不做任何耗时操作，否则拖动会卡顿。

    case WM_DRAWITEM:
        // 某个 SS_OWNERDRAW 控件需要重画时，系统把绘制任务交给父窗口。
        if (wParam == IDC_IC_SWATCH && lParam)
        {
            // wParam = 发来请求的控件 ID，先过滤出我们关心的那个颜色样例；
            // lParam 是 DRAWITEMSTRUCT*，不为空才能安全解引用。
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            // DRAWITEMSTRUCT 里关键的两个成员：
            //   hDC    —— 系统准备好的绘制用设备上下文，直接往里画即可；
            //   rcItem —— 控件矩形，坐标系就是这个 DC 的坐标系。
            PaintColorSwatch(dis->hDC, dis->rcItem, g_fillColor, FillTransparent());
            // 交给公共绘制函数（ImageOps.cpp）：它会在矩形里画当前颜色，
            // 并且当 transparent=true 时改画「灰白棋盘格」来直观表示透明。
            return TRUE;
            // 返回 TRUE 表示「我画完了，系统不要再画」；
            // 返回 FALSE 的话系统可能再叠加一层默认绘制，出现重影。
        }
        break;
        // 不是我们关心的控件：跳出去交给 DefWindowProcW 默认处理。

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            // WM_COMMAND 的 wParam 高 16 位是「通知码」（BN_CLICKED 等），
            // 低 16 位才是控件 ID。本页只关心「哪个控件被点了」，
            // 所以取低 16 位即可——这样按钮和复选框可以统一处理（两者通知码不同）。
            // 如果将来要区分「点击」和「双击」，才需要再看 HIWORD(wParam)。

            if (id == IDC_IC_BTN_COLOR)
            {
                g_fillColor = ChooseFillColor(hWnd, g_fillColor);
                // 弹系统「颜色」对话框，并传入当前颜色作为初始值。
                // 函数返回用户选定的新颜色；如果用户点「取消」，
                // 它内部会返回原值（详见 ImageOps.cpp），所以可以直接赋值。
                PushFillToCanvas();
                // 颜色变了，推给画布 + 让颜色样例重画。
                return 0;
            }
            if (id == IDC_IC_CHK_TRANS)
            {
                PushFillToCanvas();
                // 「透明」开关被切换（BS_AUTOCHECKBOX 已自动翻转勾选状态，
                // 这里不需要再 Button_SetCheck）。只需把新状态推给画布并刷新样例。
                return 0;
            }
            if (id == IDC_IC_BTN_RESET)
            {
                if (s_hCanvas) CropCanvas_ResetToSquareCenter(s_hCanvas);
                // 把选框恢复为「源图中央的最大正方形」——这是最常用的中性起点，
                // 用户改坏了随手就能回来。
                UpdateInfo();
                // 选框变了，刷新信息文字。
                // 注意：画布内部自己也会发 WM_APP_RECT_CHANGED，按说会再刷新一次；
                // 这里显式再调一次是为了在画布因某种原因没发消息时也能同步，
                // 重复调用无副作用（只是多设一次相同的文字）。
                return 0;
            }
            if (id == IDC_IC_BTN_PROCESS)
            {
                OnProcess(hWnd);
                // 真正干活：合成正方形位图并生成 .ico（细节见 OnProcess 上方注释）。
                return 0;
            }
            if (id >= IDC_IC_CHK_BASE && id < IDC_IC_CHK_BASE + ICO_SIZE_COUNT)
            {
                // 用「基础 ID + 数量」做范围判断，一次覆盖全部 7 个尺寸复选框。
                // 这是把复选框 ID 设计成连续值的直接好处：不用写 7 个 if。
                UpdateInfo();
                // 勾选/取消某个尺寸会改变「输出尺寸：…」这一行，所以要刷新信息。
                return 0;
            }
        }
        break;
        // 不是本页关心的控件（或 HIWORD 通知码不匹配）：
        // 落到这里交给 DefWindowProcW，让系统继续默认处理。
        // 常见例子：BS_GROUPBOX 不响应，但如果将来加了编辑框，
        // EN_CHANGE 等通知就必须从这里穿过去，否则控件会失灵。

    default:
        break;
        // 其余所有消息（WM_PAINT、WM_ERASEBKGND、WM_DESTROY 等）都不特殊处理，
        // 直接走系统默认逻辑。
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
    // 默认处理：所有未被 return 截走的路径最终都到这里。
    // 用 DefWindowProcW（W 结尾）与 Unicode 版本的 CreateWindowExW 保持一致。
}

// ---------------------------------------------------------------------------
// PageIco_Create：把「ICO 转换」页创建出来，返回页面窗口句柄。
//   参数：parent —— 父窗口，即主窗口 g_hMain。页面是它的子窗口（WS_CHILD）。
//   返回：成功 = 新页面窗口的 HWND；窗口类注册失败 = nullptr。
//   调用方：Project001.cpp 在初始化各功能页时调用本函数，并把返回值存起来，
//          之后靠 ShowWindow 显隐来切页。
//   设计要点：每个功能页都是一个独立的「自定义窗口类 + 窗口」，
//     页面各自处理自己的消息，主窗口只负责创建、切换和广播全局事件，
//     这样各页互不干扰，加删页面也不用改主窗口的消息处理逻辑。
//   坑：RegisterClassExW 对同一个类名重复注册会失败并返回 0、且 GetLastError()
//     为 ERROR_CLASS_ALREADY_EXISTS。这里用 static bool registered 做一次性
//     注册标记；同时也会容忍「类已存在」这种错误，避免在多次调用的场景下直接失败。
//     注意 static 局部变量只在首次进入时初始化，且这个函数不是多线程调用的，
//     所以不需要额外加锁。
// ---------------------------------------------------------------------------
HWND PageIco_Create(HWND parent)
{
    static bool registered = false;
    // 类注册只做一次。static 局部变量在第一次执行到这里时初始化为 false，
    // 之后跨调用一直保留——正好符合「进程内只注册一次」的需求。
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        // 先全部清零：WNDCLASSEXW 有很多字段，不初始化干净会把栈上的垃圾值带进去。
        wc.cbSize = sizeof(wc);
        // cbSize 必须填，否则 RegisterClassExW 直接失败（最经典的坑之一）。
        wc.style = CS_HREDRAW | CS_VREDRAW;
        // 水平/垂直尺寸变化时系统自动重画整个页面，省掉我们自己在 WM_SIZE 里
        // 调 InvalidateRect 的麻烦。因为本页只是容器，真正的自绘都在子控件里，
        // 这个样式主要是兜底。
        wc.lpfnWndProc = Proc;
        // 指定本页的窗口过程：所有发到页面窗口的消息都进 Proc()。
        wc.hInstance = g_hInst;
        // 程序实例句柄，WinMain 里赋值，注册窗口类必须填。
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 鼠标在页面空白区域显示为普通箭头光标。
        // 注意：传入 nullptr 表示「用系统内置资源」，不用释放，这是标准写法。
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        // 用系统的「窗口背景色」画刷擦背景。
        // 这里用的技巧是「COLOR_WINDOW + 1」这种整数值当 HBRUSH 传进注册结构，
        // 系统能识别它是系统颜色索引而不是真实画刷指针，不需要我们 CreateSolidBrush
        // 也就不需要 DestroyObject——千万不要在这里瞎改数字或调用真实画刷。
        wc.lpszClassName = L"Pic246PageIco";
        // 本页专属窗口类名，全局唯一；与 CropCanvas.cpp、其它页面的类名都不同，
        // 否则注册会互相顶掉。
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        // 注册失败才返回 nullptr；如果失败原因只是「类已存在」则视为成功
        // （比如程序里别处也调用过本函数），继续往下创建窗口。
        registered = true;
        // 标记已注册。注意这行只在成功路径上执行——上面的 return 会拦掉真正的失败。
    }

    return CreateWindowExW(0, L"Pic246PageIco", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
    // 创建页面窗口：
    //   第 1 个参数 0            —— 不使用扩展窗口样式。
    //   第 2 个参数 L"Pic246PageIco" —— 上面注册的窗口类名。
    //   第 3 个参数 L""          —— 无标题文字（子页面不需要标题栏）。
    //   样式 WS_CHILD            —— 本窗口是 parent 的子窗口，随父窗口一起移动/隐藏。
    //       WS_CLIPCHILDREN      —— 画自己时避开子控件的区域，防止擦背景擦掉子控件，
    //                               减少闪烁；父窗口有它，子控件重画互不干扰。
    //   位置大小 (0,0,10,10)     —— 先给个占位大小。真正的尺寸由主窗口在切换/布局时
    //                               用 MoveWindow 设定；WM_CREATE 与首次 WM_SIZE 之间
    //                               的短暂时刻就是 10×10，所以 WM_CREATE 里也要先 Layout()
    //                               一次以尽量减轻错位。
    //   parent                   —— 父窗口（主窗口），决定了页面的 Z 序和生命周期。
    //   菜单/加速键 nullptr、实例 g_hInst、附加参数 nullptr —— 都按最简单的方式传。
}
