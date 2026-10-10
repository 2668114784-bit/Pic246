// ============================================================================
//  PageCompress.cpp —— 功能页：图片大小压缩
//    · 按画质压缩（滑块 10~100），实时预估输出体积
//    · 按目标大小压缩（填 KB），自动搜索最合适的画质
//    · 尺寸不变，只压体积；输出到原目录，后缀 _min
// ============================================================================
// ---------------------------------------------------------------------------
//  【文件说明】这个文件负责什么
// ---------------------------------------------------------------------------
//  这是 Pic246（纯 Win32 API + GDI+，没有 MFC / 没有 Qt）的一个“功能页”。
//  整个程序的结构是：
//
//    · Project001.cpp  创建主窗口：左边一条自绘侧边栏，右边一块区域;
//    · 每个功能（缩放 / 裁切 / 调整 / ICO / 压缩 / 格式转换 / 涂鸦）各自一个
//      Page*.cpp 文件，每个文件注册一个自己的窗口类，再把窗口以“子窗口”的
//      形式贴到主窗口右边那块区域上；
//    · 页面之间不直接互相调用，而是靠 App.h 里的全局变量（g_pSrcImage、
//      g_srcW / g_srcH 等，表示“当前载入的源图”）和自定义消息
//      （WM_APP_IMAGE_CHANGED 等）来通信。
//
//  本页（大小压缩页）做的事情：
//    1. 左边一大块预览区，显示当前源图的缩略图（最长边 640 像素）；
//    2. 右边是参数面板，从上到下分三组：
//         「压缩方式」——两个单选：按画质 / 按目标大小；
//         「压缩参数」——一条滑块（画质 10~100）+ 一个输入框（目标 KB）；
//         「输出格式」——保持原格式 / 转 JPEG / 转 PNG；
//    3. 面板下方实时显示：预估输出体积、原文件大小、约占原来的百分比；
//    4. 底部「压缩并输出」按钮，写出 原文件名_min.<扩展名>。
//
//  两种压缩方式的区别（本页最核心的概念）：
//    · 按画质：用户直接指定 JPEG 质量（10~100）。压缩是有损的，质量越低
//      文件越小、画面越糊；这是“正着算”——给定画质求体积。
//    · 按目标大小：用户给出想要的 KB 数，程序去**反着猜**该用多少画质。
//      做法是二分查找（见 ImageOps.cpp 的 FindQualityForTargetSize()）：
//      在 10~100 之间反复试中间值编码、看结果有没有超出目标，逐步逼近
//      “不超过目标体积的最高画质”。之所以能二分，是因为对 JPEG 来说
//      “画质越高 → 文件越大”是单调的。
//
//  大致流程（按时间顺序）：
//    主窗口启动 → 调 PageCompress_Create() 注册窗口类并创建本页窗口（此时
//    还是隐藏的、10×10 的占位尺寸）→ Windows 发 WM_CREATE，本页创建所有子
//    控件并设初值 → 主窗口把本页 MoveWindow 到正确位置/尺寸 → Windows 发
//    WM_SIZE，本页 Layout() 按 DPI 重新摆放所有控件 → 用户拖滑块 / 改输入框
//    / 切单选 → WM_COMMAND 或 WM_HSCROLL → ScheduleEstimate() 起一个 320ms
//    的防抖定时器 → 定时器到点后 DoEstimate() 真正编码一次并刷新预估文字 →
//    点「压缩并输出」→ OnProcess() 再算一次画质、真正保存。
//    另外，每当主窗口载入新图片，它会向本页发 WM_APP_IMAGE_CHANGED，本页据此
//    重建预览图、重置默认目标大小、刷新信息并立刻预估一次。
//
//  【和其他文件的配合】
//    · App.h          —— 全局变量（g_pSrcImage、g_srcW/g_srcH、g_hInst、g_hMain）、
//                        控件 ID（IDC_CM_* 号段）、消息号（WM_APP_IMAGE_CHANGED）、
//                        以及本页用到的所有工具函数声明；
//    · Project001.cpp —— 主窗口、CreateCtl()/SetAppStatus()、页面切换；
//    · ImageOps.cpp   —— 真正的图片处理实现：本页调用的
//                        GuessSourceMime()（按扩展名猜格式）、
//                        FormatBytes()（把字节数格式化成 "1.2 MB"）、
//                        SourceFileSize()（取原文件字节数）、
//                        EstimateEncodedSize()（编码到内存只为量体积）、
//                        FindQualityForTargetSize()（二分找画质）、
//                        SaveBitmapAsFormat()（按指定格式存盘）、
//                        CreatePreviewSource()（做缩略图）、
//                        PaintImagePreview()（画预览）都在这里；
//    · CropCanvas.cpp / DrawCanvas.cpp —— 别的页用，本页无关。
//
//  【本页注册的窗口类名】Pic246PageCompress
//  【本页使用的子控件 ID】见下面各 CreateCtl() 调用处（IDC_CM_* 号段 1500~1515）
// ============================================================================
#include "App.h"          // 公共声明：全局变量、消息号、工具函数、GDI+ 头文件等

using namespace Gdiplus;  // 本文件大量使用 Gdiplus::Bitmap / Image 等，直接引用省前缀

// ---------------------------------------------------------------------------
//  控件句柄（HWND）
// ---------------------------------------------------------------------------
//  Win32 里所有控件都是一个“窗口”，用一个 HWND 句柄代表。这些句柄只在
//  WM_CREATE 里赋值一次，之后整个文件的各个函数都用它们来操作控件
//  （比如 SendMessageW 读滑块位置、SetWindowTextW 改文字）。
//  统一用 static（文件内可见）可以避免和别的 Page*.cpp 里的同名变量冲突。
// ---------------------------------------------------------------------------
static HWND s_hPage = nullptr;         // 本页自己的窗口（父窗口），也是定时器的宿主
static HWND s_hPreview = nullptr;      // 左侧预览区（一个自绘的 Static 控件）
static HWND s_hGrpMode = nullptr;      // 「压缩方式」分组框
static HWND s_hRadioQuality = nullptr;  // 单选：按画质
static HWND s_hRadioTarget = nullptr;   // 单选：按目标大小
static HWND s_hGrpParam = nullptr;      // 「压缩参数」分组框
static HWND s_hTrkQuality = nullptr;    // 画质滑块（trackbar 控件）
static HWND s_hLblQuality = nullptr;    // 滑块下方那行说明文字（"画质 80%" 之类）
static HWND s_hEditTarget = nullptr;    // 目标大小输入框（Edit 控件，只能填数字）
static HWND s_hLblUnit = nullptr;       // 输入框右边的 "KB（目标文件大小）" 静态文字
static HWND s_hGrpFormat = nullptr;     // 「输出格式」分组框
static HWND s_hRadioKeep = nullptr;     // 单选：保持原格式
static HWND s_hRadioJpeg = nullptr;     // 单选：转成 JPEG
static HWND s_hRadioPng = nullptr;      // 单选：转成 PNG
static HWND s_hLblEst = nullptr;        // 显示“预估大小 / 占比”的多行文字
static HWND s_hLblOrig = nullptr;       // 显示“原文件大小 / 原图尺寸 / 输出格式”的多行文字
static HWND s_hBtn = nullptr;           // 「压缩并输出」按钮

// ---------------------------------------------------------------------------
//  本页私有状态
// ---------------------------------------------------------------------------
//  s_pPreview：预览用的缩略图（GDI+ Bitmap）。
//    注意它是 new 出来的，必须自己 delete；本文件在 RebuildPreview() 里遵循
//    “先 delete 旧的、再建新的”的规则，否则每换一次图就泄漏一块内存。
//  s_estimating：防重入标志。预估体积要把整张图**真编码一遍**（很慢），如果
//    用户飞快地拖滑块，定时器可能在上一次还没算完时又触发；用它把并发的
//    第二次调用直接挡掉，避免界面卡死和结果错乱。
// ---------------------------------------------------------------------------
static Bitmap* s_pPreview = nullptr;   // 预览缩略图（本文件负责 delete）
static bool    s_estimating = false;    // true = 正在预估，本次跳过，防止重入

// 定时器 ID。SetTimer/KillTimer 用整数区分定时器，本页只需要一个，就取 1。
// 用宏而不是裸写 1，是为了以后加第二个定时器时不容易撞号。
#define TIMER_ESTIMATE 1

// ---------------------------------------------------------------------------
//  Quality —— 读当前滑块的画质值
//  参数：无。  返回：10~100 的整数（滑块位置）。
//
//  要点：
//    · trackbar（滑块）控件没有“GetPos 函数”，而是通过给控件窗口发消息
//      TBM_GETPOS 来读位置，用 SendMessageW 同步发送，返回值就是位置。
//    · TBM_SETPOS/TBM_GETPOS 的第二个参数是 WPARAM，GetPos 时要传 0。
//    · 千万别改成 PostMessageW：PostMessage 是“投递到消息队列后就返回”，
//      拿不到同步返回值，这里会读到垃圾值。
// ---------------------------------------------------------------------------
static int  Quality() { return (int)SendMessageW(s_hTrkQuality, TBM_GETPOS, 0, 0); }

// ---------------------------------------------------------------------------
//  TargetMode —— 当前是不是“按目标大小”模式
//  参数：无。  返回：true = 用户选了「按目标大小」，false = 「按画质」。
//  用的是 Button_GetCheck 宏（定义在 windowsx.h），它本质上是给按钮发
//  BM_GETCHECK 消息。BST_CHECKED 表示这个单选按钮被选中。
// ---------------------------------------------------------------------------
static bool TargetMode() { return Button_GetCheck(s_hRadioTarget) == BST_CHECKED; }

// ---------------------------------------------------------------------------
//  TargetBytes —— 把输入框里的 KB 数读出来，换算成字节
//  参数：无。  返回：目标字节数（size_t）。
//
//  细节与坑：
//    · GetWindowTextW 把输入框当前文字复制到 buf，读的是控件的“当前内容”；
//      上限 64 个字符，多余部分会被截断（输入框本来是 ES_NUMBER，正常不会超）。
//    · _wtoi64 把字符串转成 64 位整数，遇到非数字会得到 0。
//    · 输入框允许为空（用户全选删除时会短暂为空），空字符串也转成 0，
//      所以这里兜底：小于 1 KB 一律按 1 KB 算，免得后面除零或目标为 0。
//    · 乘 1024 后再转 size_t。这里用 long long 中间量，是为了避免 32 位下
//      int 相乘溢出（比如用户输入 5000000 KB）。
// ---------------------------------------------------------------------------
static size_t TargetBytes()
{
    wchar_t buf[64] = {};
    GetWindowTextW(s_hEditTarget, buf, 64);
    long long kb = _wtoi64(buf);
    if (kb < 1) kb = 1;                        // 兜底：最小按 1 KB 处理
    return (size_t)kb * 1024;
}

// 输出 MIME：保持原格式 / 强制 JPEG / 强制 PNG
// ---------------------------------------------------------------------------
//  OutputMime —— 根据「输出格式」那组单选，算出要用的 MIME 类型
//  参数：extOut —— 输出参数（可以是 nullptr）。用来回传对应的文件扩展名，
//                  例如 L".jpg" / L".png"，保存时拼输出文件名要用。
//  返回：MIME 字符串，如 L"image/jpeg"、L"image/png"。
//
//  为什么是 MIME 而不是“格式名”：GDI+ 的编码器是用 MIME 字符串来定位的
//  （见 ImageOps.cpp 的 GetEncoderClsid / ListImageEncoders），所以程序内部
//  统一用 MIME 作为格式的标识，扩展名只是给文件名用的“附件信息”。
//
//  三种情况：
//    · 选了「保持原格式」→ 交给 GuessSourceMime() 按源文件扩展名去猜，
//      它同时给出 mime 和 ext，所以两个都要取回来；
//    · 选了 JPEG → 强制 image/jpeg，扩展名 .jpg；
//    · 否则（选了 PNG）→ image/png，扩展名 .png。
//  注意 sel 初始化成 2（PNG）：这样即使三个单选都没勾上（理论上不该发生），
//  也会退化成一个有效格式，不会返回空字符串把后面的编码搞崩。
// ---------------------------------------------------------------------------
static std::wstring OutputMime(std::wstring* extOut)
{
    int sel = 2;   // 0=保持 1=jpeg 2=png
    if (Button_GetCheck(s_hRadioKeep) == BST_CHECKED) sel = 0;
    else if (Button_GetCheck(s_hRadioJpeg) == BST_CHECKED) sel = 1;
    else if (Button_GetCheck(s_hRadioPng) == BST_CHECKED) sel = 2;

    if (sel == 0)
    {
        const OutFmtGuess f = GuessSourceMime();   // 定义在 ImageOps.cpp
        if (extOut) *extOut = f.ext;
        return f.mime;
    }
    if (sel == 1) { if (extOut) *extOut = L".jpg"; return L"image/jpeg"; }
    if (extOut) *extOut = L".png";
    return L"image/png";
}

// ---------------------------------------------------------------------------
//  UpdateQualityLabel —— 刷新滑块下面那行说明文字
//  参数：无。  返回：无。
//
//  两种模式下这句话不一样：
//    · 按画质模式：显示 "画质 80%"（80 就是滑块当前位置）；
//    · 按目标大小模式：滑块被禁用了（灰掉），位置不再有意义，所以显示
//      "由目标大小自动决定"，免得用户误以为还能拖。
//  swprintf_s 是微软的安全版 swprintf，会检查缓冲区大小；buf 给 128 个 wchar
//  绰绰有余（最长的一句也就 20 来个字符）。
// ---------------------------------------------------------------------------
static void UpdateQualityLabel()
{
    wchar_t buf[128];
    if (TargetMode())
        swprintf_s(buf, L"由目标大小自动决定");
    else
        swprintf_s(buf, L"画质 %d%%", Quality());   // %% 输出一个字面量百分号
    SetWindowTextW(s_hLblQuality, buf);
}

// ---------------------------------------------------------------------------
//  UpdateModeEnable —— 根据当前模式，启用/禁用该模式才用得上的控件
//  参数：无。  返回：无。
//
//  两个模式的参数完全不同：按画质只用滑块，按目标大小只用输入框。
//  把用不上的那个禁用掉（EnableWindow(hwnd, FALSE)），好处有两点：
//    · 用户一眼就知道现在该填哪个；
//    · 被禁用的控件不会拿到键盘焦点、也不会发通知消息，省掉一堆状态判断。
//  最后顺手刷新一下文字标签，因为“滑块有效/无效”的状态变了。
// ---------------------------------------------------------------------------
static void UpdateModeEnable()
{
    const bool t = TargetMode();
    EnableWindow(s_hTrkQuality, !t);    // 按目标大小模式 → 禁用滑块
    EnableWindow(s_hEditTarget, t);     // 按画质模式 → 禁用输入框
    UpdateQualityLabel();
}

// ---------------------------------------------------------------------------
//  ScheduleEstimate —— 安排一次“稍后预估”（防抖）
//  参数：无。  返回：无。
//
//  【为什么要防抖】
//  预估体积不是简单算数，它要把整张图真的用 GDI+ 编码一遍（可能几百毫秒）。
//  如果滑块每动一下就立刻算一次（WM_HSCROLL 是连续发的，拖一下能发几十条），
//  界面会卡成幻灯片。所以这里不直接算，而是：
//    · 先 KillTimer 取消上一次还没到点的定时器；
//    · 再 SetTimer 重新计时 320 毫秒。
//  于是只有用户“停下来”超过 320ms 后，才真正执行一次预估。这就是防抖。
//
//  【固定套路】KillTimer + SetTimer 是重置定时器的标准写法：SetTimer 对同一个
//  ID 重复调用并不会排队，只会覆盖，所以严格说 KillTimer 不是必须的；但显式
//  写出来意图更清楚，也能兼容某些老系统的行为差异。
//
//  【320】这个数字是经验值：太短（<100ms）用户还没拖完就开算，照样卡；
//  太长（>600ms）用户会觉得“我明明松手了它怎么还没反应”。320ms 折中。
//
//  【定时器属于哪个窗口】SetTimer 的宿主是 s_hPage，所以 WM_TIMER 会被发到
//  本页的窗口过程 Proc()。窗口销毁时系统会自动清掉挂在它身上的定时器。
// ---------------------------------------------------------------------------
static void ScheduleEstimate()
{
    if (!s_hPage) return;                        // 窗口还没建好（理论上不会）就跳过
    KillTimer(s_hPage, TIMER_ESTIMATE);          // 取消上一次待触发的预估
    SetTimer(s_hPage, TIMER_ESTIMATE, 320, nullptr);   // 320ms 后发 WM_TIMER
}

// ---------------------------------------------------------------------------
//  UpdateOriginInfo —— 刷新“原文件大小 / 原图尺寸 / 输出格式”那几行文字
//  参数：无。  返回：无。
//
//  信息全部来自全局状态和当前选项：
//    · FormatBytes() 把字节数变好看（"1.2 MB" / "350.0 KB" / "512 B"）；
//    · SourceFileSize() 直接去问磁盘上源文件的真实大小（不是内存里估计的）；
//    · g_srcW / g_srcH 是源图的像素尺寸，由 LoadImageFile() 载入时填好；
//    · 输出格式那一栏把 MIME 反着翻译成人看的名字（PNG / JPEG / 其它）。
//      用三元表达式嵌套，写在 swprintf_s 的第 5 个参数位置。
//
//  【为什么要判断 mime == image/png 而不是直接看扩展名】
//  因为编码器认的是 MIME；“保持原格式”时扩展名和 MIME 是一起由
//  GuessSourceMime() 推出来的，直接比 MIME 最稳。
//
//  \\r\\n 是换行：SetWindowTextW 到 Static 控件里的多行文字，
//  只有显式写了回车换行才会换行。
// ---------------------------------------------------------------------------
static void UpdateOriginInfo()
{
    if (!s_hLblOrig) return;                 // 控件还没创建，直接返回（防御性检查）
    wchar_t buf[256];
    if (!g_pSrcImage)                        // 没有源图
    {
        wcscpy_s(buf, L"尚未载入图片。");
    }
    else
    {
        std::wstring ext;
        const std::wstring mime = OutputMime(&ext);   // ext 这里用不上，但接口要求传
        swprintf_s(buf, L"原文件大小：%s\r\n原图尺寸：%u × %u        输出格式：%s",
                   FormatBytes(SourceFileSize()).c_str(), g_srcW, g_srcH,
                   (mime == L"image/png") ? L"PNG" : (mime == L"image/jpeg" ? L"JPEG" : L"其它"));
    }
    SetWindowTextW(s_hLblOrig, buf);
}

// 预估与实际保存必须基于同一份位图数据：源图若是 24bpp（JPEG 常见），
// 直接对源图编码得到的 BMP/TIFF/PNG 尺寸会明显偏小。
// ---------------------------------------------------------------------------
//  BuildOutputBitmap —— 造一份“将要拿去输出的位图”
//  参数：mime —— 目标输出格式的 MIME（决定要不要铺白底）。
//  返回：新的 Gdiplus::Bitmap（调用者负责 delete）；失败返回 nullptr。
//
//  【为什么不直接用 g_pSrcImage 去编码】
//  这是本页最重要的一个坑。g_pSrcImage 来自源文件，很多 JPEG 是 24bpp
//  （没有 alpha 通道）的。而输出如果选了 PNG/BMP/TIFF 这类带 alpha 的格式，
//  编码器会按 32bpp 去写；如果直接把 24bpp 的源图交给它，编出来的体积统计
//  会明显偏小，和真正走一遍“32bpp 位图 + 合成”的结果对不上——于是预估数字
//  和保存后的实际文件大小会差一大截。
//  解决办法：预估和保存都先调这个函数，得到**同一份** 32bppARGB 工作位图，
//  两边走完全一样的路径，数字才对得上。
//
//  【步骤说明】
//    1. 新建一张和源图同尺寸的 32bppARGB 空位图（32 = BGRA 每通道 8 位）；
//       GetLastStatus() != Ok 说明内存不够没建起来，delete 掉后返回 nullptr。
//    2. 用 Graphics 把这张位图当画布：
//       · SetCompositingMode(CompositingModeSourceCopy) 表示“直接覆盖”而不是
//         alpha 混合。因为目标位图是新建的全零（全透明）内存，用覆盖模式才有
//         确定的结果；如果用默认的 SourceOver 混合，半透明像素会被“混到黑”上。
//       · 如果目标格式**不支持透明**（不是 png/gif/tiff，典型就是 JPEG/BMP），
//         先铺一层不透明白色（Color(255,255,255,255) 参数是 A,R,G,B），
//         然后切回 SourceOver，让源图正常叠加在白底上。
//         这样 JPEG 输出才不会把透明区域变成黑块。
//       · 支持透明的格式（png/gif/tiff）跳过铺底，保留 alpha。
//    3. DrawImage 把源图原样画上去（尺寸用 g_srcW/g_srcH 强制一致，不缩放）。
//
//  注意：Graphics 对象是栈上的，构造函数里 BeginDraw 之类不用手工管，
//  析构时自动结束——GDI+ 的 C++ 封装就是这么设计的。
// ---------------------------------------------------------------------------
static Bitmap* BuildOutputBitmap(const std::wstring& mime)
{
    if (!g_pSrcImage || g_srcW <= 0 || g_srcH <= 0) return nullptr;   // 没有源图

    Bitmap* work = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);  // 1. 新建工作位图
    if (work->GetLastStatus() != Ok) { delete work; return nullptr; }  // 建失败（内存不足）

    Graphics g(work);                                                 // 2. 以它为画布
    g.SetCompositingMode(CompositingModeSourceCopy);                  //    先“直接覆盖”
    if (!(mime == L"image/png" || mime == L"image/gif" || mime == L"image/tiff"))
    {
        g.Clear(Color(255, 255, 255, 255));      // 不支持透明的格式铺白底
        g.SetCompositingMode(CompositingModeSourceOver);
    }
    g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);         // 3. 原图原样画上去
    return work;
}

// ---------------------------------------------------------------------------
//  DoEstimate —— 真正算一次预估体积，并刷新界面文字（本页最核心的函数）
//  参数：无。  返回：无。
//
//  这是定时器到点后才会被调用的“慢函数”，内部会真的编码一次图片。
//
//  【整体步骤】
//    0. 没有源图 → 直接把标签设成 "预估大小：—" 返回；
//       如果上一次预估还没结束（s_estimating == true）→ 直接返回，防止重入。
//    1. 取得当前输出格式（mime + ext），并判断它是不是**有损**格式
//       （image/jpeg 或 image/webp 才认 quality 参数；PNG/GIF/BMP/TIFF 是无损，
//        画质参数对它们完全无效）。
//    2. 把鼠标设成“忙”的沙漏，让用户知道程序在算（这张图可能要编几百毫秒）。
//    3. 调 BuildOutputBitmap() 得到统一的工作位图（见上面的详细说明）。
//    4. 分两条路显示结果：
//       A. 有损 + 按目标大小模式：
//          调 FindQualityForTargetSize() 做二分搜索，它同时返回选中的画质 q
//          和该画质下的实际字节数 actual。
//            - actual == 0  → 编码失败（比如系统没有对应编码器）；
//            - actual <= target → 达成目标，显示体积和画质；
//            - 否则 → 连最低画质（10）都超了，如实告诉用户“只能压到这个程度”。
//       B. 其它情况（无损格式，或是有损格式的“按画质”模式）：
//          调 EstimateEncodedSize() 编码一次量体积。有损格式用滑块画质，
//          无损格式统一传 100（编码器会忽略它，只是走同一个接口）。
//          再和原文件大小比一下，算出“约为原来的百分之几”。
//    5. 写回标签、释放工作位图、把鼠标恢复原样、清掉重入标志。
//
//  【几个必须注意的坑】
//    · s_estimating 必须在**每一条返回路径**上复位。这里靠“只在函数末尾复位”
//      并且前面两个提前 return 都没有置位，来保证配对正确（谁置位谁负责复位）。
//    · SetCursor 改的是当前光标，函数退出前一定要用 hOld 还原，
//      否则光标会一直是沙漏，用户以为程序卡死了。LoadCursorW(nullptr, IDC_WAIT)
//      是取系统自带的等待光标。
//    · work 是 new 出来的，成功失败都要 delete；这里用“算完统一 delete”的写法。
//    · 预估只编码到内存（EstimateEncodedSize 内部用 CreateStreamOnHGlobal 建了
//      一块内存流），不写磁盘，所以不会有任何文件被创建。
//    · 那句话里 (srcSize > 0) ? L"" : L"" 两个分支都是空字符串，等于没有效果，
//      属于历史遗留写法（本页只加注释、不改代码，所以原样保留）。
// ---------------------------------------------------------------------------
static void DoEstimate()
{
    if (!s_hLblEst) return;                          // 控件还没建好
    if (!g_pSrcImage)                                // 没有源图
    {
        SetWindowTextW(s_hLblEst, L"预估大小：—");
        return;
    }
    if (s_estimating) return;                        // 上一次还没算完，本次跳过（防重入）
    s_estimating = true;

    std::wstring ext;
    const std::wstring mime = OutputMime(&ext);      // 当前输出格式
    const size_t srcSize = SourceFileSize();         // 原文件字节数（可能为 0）
    // 只有 JPEG / WebP 才认“画质”参数，其余格式是无损的，画质没用。
    const bool lossy = (_wcsicmp(mime.c_str(), L"image/jpeg") == 0 ||
                        _wcsicmp(mime.c_str(), L"image/webp") == 0);

    wchar_t buf[512];
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));   // 换成沙漏，记住旧光标

    Bitmap* work = BuildOutputBitmap(mime);          // 统一的工作位图（预估与保存共用）

    if (lossy && TargetMode())
    {
        // ---- A. 有损 + 按目标大小：二分搜索最合适的画质 ----
        const size_t target = TargetBytes();
        size_t actual = 0;
        const int q = work ? FindQualityForTargetSize(work, target, &actual) : 0;
        if (actual == 0)
        {
            swprintf_s(buf, L"预估大小：计算失败");
        }
        else if (actual <= target)
        {
            // 达成目标：显示最终体积 + 二分选出来的画质
            swprintf_s(buf, L"预估大小：%s（画质 %d）\r\n目标 %s：已达成%s",
                       FormatBytes(actual).c_str(), q, FormatBytes(target).c_str(),
                       (srcSize > 0) ? L"" : L"");      // 遗留写法：两个分支都是空串
        }
        else
        {
            // 连最低画质都超过目标：如实告知，还给出实际能压到的体积
            swprintf_s(buf, L"预估大小：%s（已是最低画质 %d）\r\n无法压到目标 %s，只能压到这个程度",
                       FormatBytes(actual).c_str(), q, FormatBytes(target).c_str());
        }
    }
    else
    {
        // ---- B. 其它情况：按当前画质（无损格式则画质无意义）编码一次量体积 ----
        const ULONG q = (ULONG)(lossy ? Quality() : 100);      // 无损格式统一传 100
        const size_t est = work ? EstimateEncodedSize(work, mime, q) : 0;
        if (est == 0)
        {
            swprintf_s(buf, L"预估大小：计算失败");
        }
        else if (srcSize > 0)
        {
            // 有原文件大小，顺带算个百分比（%.0f 表示不保留小数）
            swprintf_s(buf, L"预估大小：%s\r\n原文件 %s，约为原来的 %.0f%%",
                       FormatBytes(est).c_str(), FormatBytes(srcSize).c_str(),
                       100.0 * (double)est / (double)srcSize);
        }
        else
        {
            // 原文件读不到大小（比如路径已失效），只显示预估体积
            swprintf_s(buf, L"预估大小：%s", FormatBytes(est).c_str());
        }
    }

    SetWindowTextW(s_hLblEst, buf);      // 写回标签
    delete work;                         // 工作位图用完就还（delete nullptr 是安全的）
    SetCursor(hOld);                     // 还原鼠标光标
    s_estimating = false;                // 清除重入标志
}

// ---------------------------------------------------------------------------
//  RebuildPreview —— 重建左侧的预览缩略图
//  参数：无。  返回：无。
//
//  调用时机：收到 WM_APP_IMAGE_CHANGED（主窗口换了源图）时。
//
//  【固定套路】先 delete 旧的、再把指针赋成新的：
//    · 不先删，换图多次就会内存泄漏（每张缩略图都是 new 出来的位图）；
//    · 也不能先 delete 再判断——CreatePreviewSource() 在“没有源图”时返回
//      nullptr，这时 s_pPreview 就应该是 nullptr，而 WM_DRAWITEM 里的
//      PaintImagePreview() 正好把 nullptr 解释成“显示提示文字”，所以是对的。
//  【640】是缩略图的最长边像素上限。预览区本身也就几百像素宽，没必要按原图
//  分辨率做缩略图，那样纯属浪费内存和时间。
// ---------------------------------------------------------------------------
static void RebuildPreview()
{
    if (s_pPreview) { delete s_pPreview; s_pPreview = nullptr; }   // 先删旧的
    s_pPreview = CreatePreviewSource(640);                         // 最长边 640 的缩略图
}

// ---------------------------------------------------------------------------
//  Layout —— 按当前客户区大小重新摆放所有子控件（本页的“排版”函数）
//  参数：无。  返回：无。
//
//  调用时机：WM_CREATE 时一次、每次 WM_SIZE 时一次（用户拖动窗口大小）。
//  Win32 的普通控件不会自动排版，得自己算坐标然后 MoveWindow。
//
//  【DPI 缩放】所有尺寸都套一层 D(12) 这种形式。D(int dip) 定义在 App.h：
//  按当前系统 DPI 把“逻辑像素（96dpi 下的设计像素）”换算成真实像素。
//  在 150% 缩放的显示器上，D(12) 会算出 18，于是界面不会显得过小。
//  反过来，千万别写死像素值，否则高 DPI 下字会挤在一起。
//
//  【布局结构】把客户区分成左右两块：
//    · 左边预览区：宽度 = 总宽 - 面板宽 - 3 个边距；
//    · 右边参数面板：宽度固定 D(340)，但会用两个 if 夹在
//      [D(240), cx - D(160)] 之间——窗口太窄时面板不至于把预览挤没，
//      窗口很宽时面板也不会胖得难看。
//  面板内部则是从上往下“流水线”摆放：每放完一组就 py += 高度 + 间距，
//  这样加/删一组时只需要改一处，不用重算后面所有控件。
//
//  MoveWindow 第 五 个参数 TRUE 表示“移动后立刻重画”，能避免拖动窗口时
//  出现残影。rc.right / rc.bottom 就是客户区宽高（这是一个 CHILD 窗口，
//  客户区坐标从 (0,0) 开始）。
//
//  【本文件只加注释、不改代码】下面这些 D(92)、D(112)、D(104)、D(42) 等高度
//  都是按控件内容量出来的经验值（见各自的组内偏移）。
// ---------------------------------------------------------------------------
static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);         // 取本页客户区大小
    const int cx = rc.right;             // 客户区宽
    const int cy = rc.bottom;            // 客户区高
    const int M = D(12);                 // 通用外边距（12 逻辑像素）

    // ---- 先定右边参数面板的宽度 ----
    int panelW = D(340);
    if (panelW > cx - D(160)) panelW = cx - D(160);   // 窗口太窄：保证预览至少 D(160) 宽
    if (panelW < D(240)) panelW = D(240);             // 上限夹紧后太小的话，反过来保面板

    // ---- 左边预览区 ----
    int prevW = cx - panelW - M * 3;                  // 剩下都归预览（左右各留边距 + 中间缝）
    if (prevW < D(120)) prevW = D(120);               // 不低于 D(120)，避免出现负宽度
    MoveWindow(s_hPreview, M, M, prevW, cy - M * 2, TRUE);

    // ---- 右边面板：从 (px, py) 开始往下摆 ----
    const int px = M * 2 + prevW;                     // 面板左边界
    const int pw = cx - px - M;                       // 面板宽度（按实际剩余算，防止越界）
    int py = M;                                       // 当前“光标”的纵坐标

    // 第 1 组：「压缩方式」分组框 + 两个单选按钮
    int hMode = D(92);                                // 组框高度：容纳 2 行单选
    MoveWindow(s_hGrpMode, px, py, pw, hMode, TRUE);
    MoveWindow(s_hRadioQuality, px + D(18), py + D(32), pw - D(36), D(24), TRUE);  // 缩进 18 留边
    MoveWindow(s_hRadioTarget, px + D(18), py + D(58), pw - D(36), D(24), TRUE);   // 第 2 行下移 26

    py += hMode + D(8);                               // 换到下一组（组间距 8）

    // 第 2 组：「压缩参数」分组框（滑块 + 画质文字 + KB 输入框）
    int hParam = D(112);
    MoveWindow(s_hGrpParam, px, py, pw, hParam, TRUE);
    MoveWindow(s_hTrkQuality, px + D(16), py + D(26), pw - D(32), D(30), TRUE);    // 滑块横向铺满
    MoveWindow(s_hLblQuality, px + D(18), py + D(58), pw - D(36), D(20), TRUE);    // 滑块下方文字
    MoveWindow(s_hEditTarget, px + D(18), py + D(80), D(110), D(24), TRUE);        // 输入框固定宽
    MoveWindow(s_hLblUnit, px + D(136), py + D(84), pw - D(150), D(20), TRUE);     // 紧跟其后的 "KB"

    py += hParam + D(8);

    // 第 3 组：「输出格式」分组框 + 三个单选按钮
    int hFmt = D(104);
    MoveWindow(s_hGrpFormat, px, py, pw, hFmt, TRUE);
    MoveWindow(s_hRadioKeep, px + D(18), py + D(28), pw - D(36), D(24), TRUE);     // 第 1 行
    MoveWindow(s_hRadioJpeg, px + D(18), py + D(52), pw - D(36), D(24), TRUE);     // 第 2 行
    MoveWindow(s_hRadioPng, px + D(18), py + D(76), pw - D(36), D(24), TRUE);      // 第 3 行

    py += hFmt + D(8);

    // ---- 两行信息文字 ----
    MoveWindow(s_hLblOrig, px, py, pw, D(40), TRUE);   // 原文件大小 / 原图尺寸 / 输出格式（两行）
    py += D(42);
    MoveWindow(s_hLblEst, px, py, pw, D(52), TRUE);    // 预估大小 / 占比（最多两行）

    // ---- 底部按钮：钉在客户区底部 ----
    const int btnH = D(42);
    MoveWindow(s_hBtn, px, cy - M - btnH, pw, btnH, TRUE);   // 从下往上量，窗口高度变化时始终贴底
}

// ---------------------------------------------------------------------------
//  OnProcess —— 点「压缩并输出」后真正干活：算画质 → 保存文件
//  参数：hPage —— 本页窗口句柄，只用来当 MessageBox 的父窗口（保证弹窗居中于本页）。
//  返回：无。
//
//  【和 DoEstimate 的关系】两者思路很像，但用途不同：
//    · DoEstimate 是“看”，只编码到内存量体积，不写文件；
//    · OnProcess 是“做”，最后会真的落盘。
//  两边都调 BuildOutputBitmap()，保证呈现给用户的预估和最终产物一致。
//
//  【步骤】
//    1. 没载入图片 → 弹个提示就返回（MessageBoxW 的 MB_OK | MB_ICONINFORMATION
//       是“确定按钮 + 信息图标”）。
//    2. 取输出格式，判断是不是有损格式（JPEG/WebP）。
//    3. 决定 quality：
//       · 无损格式 → 固定 100（反正编码器不看这个值）；
//       · 有损 + 按目标大小 → 用 FindQualityForTargetSize() 现算一次。
//         这里传的是 g_pSrcImage 而不是工作位图，所以下面要单独处理
//         “连最低画质都超目标”的情况：弹一个“是/否”询问框（MB_YESNO），
//         用户选否就直接 return（什么都不输出）。
//         【重要】这一处容易踩的坑：FindQualityForTargetSize() 内部写死了
//         按 image/jpeg 编码来试，所以它只对 JPEG 有意义。不过本分支只会在
//         lossy（JPEG/WebP）时进入，WebP 的二分结果会有点偏差，属于已知取舍。
//       · 有损 + 按画质 → 直接读滑块值。
//    4. 更新状态栏文字并立刻 UpdateWindow(g_hMain) 强制重画——因为紧接着的是
//       几百毫秒的同步编码，主窗口会“卡住”不处理消息；先强制刷一次，
//       用户才能看到“正在压缩，请稍候...”，否则这句话要等保存完才出现，
//       等于没提示。
//    5. 建工作位图，失败（内存不足）就报错返回。
//    6. SaveBitmapAsFormat(...)：拼输出路径、问是否覆盖、选编码器、写文件。
//       注意它**会接管（delete）work**，所以本函数成功后不能再 delete work，
//       否则会双重释放崩溃。它内部失败时也会自己 delete，所以这里不需要收尾。
//       输出文件名规则：原目录 + 原文件名（去扩展名） + "_min" + ext。
// ---------------------------------------------------------------------------
static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring ext;
    const std::wstring mime = OutputMime(&ext);        // 输出格式
    const bool lossy = (_wcsicmp(mime.c_str(), L"image/jpeg") == 0 ||
                        _wcsicmp(mime.c_str(), L"image/webp") == 0);

    ULONG quality = 100;                               // 默认 100：无损格式用不到，但有损格式的初值
    if (lossy)
    {
        if (TargetMode())
        {
            // 二分搜索。注意这里用的是 g_pSrcImage（原始源图），不是工作位图。
            size_t actual = 0;
            const int q = FindQualityForTargetSize(g_pSrcImage, TargetBytes(), &actual);
            quality = (ULONG)q;
            if (actual > TargetBytes())                // 最低画质仍然超标
            {
                wchar_t q2[512];
                swprintf_s(q2, L"即使使用最低画质（%d），输出仍有 %s，无法压到目标 %s。\r\n\r\n是否按最低画质继续输出？",
                           q, FormatBytes(actual).c_str(), FormatBytes(TargetBytes()).c_str());
                // IDYES 才继续；选“否”就放弃本次输出。
                if (MessageBoxW(hPage, q2, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return;
            }
        }
        else
        {
            quality = (ULONG)Quality();                // 按画质：直接读滑块
        }
    }

    SetAppStatus(L"正在压缩，请稍候...");               // 主窗口底部状态栏文字
    UpdateWindow(g_hMain);                            // 立刻重画主窗口，让上面那句话可见（不能省）

    Bitmap* work = BuildOutputBitmap(mime);           // 统一的工作位图
    if (!work)
    {
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    // 保存。SaveBitmapAsFormat 内部会 delete work（无论成功失败），此处不再 delete。
    SaveBitmapAsFormat(work, L"_min", mime, ext, quality, hPage);
}

// ---------------------------------------------------------------------------
//  Proc —— 本页窗口的窗口过程（Window Procedure）
//  参数：
//    hWnd    —— 收到消息的窗口（一开始是本节窗口自己；子控件的消息走子控件
//               自己的窗口过程，只有“通知消息”会被转成 WM_COMMAND 发到这里）。
//    message —— 消息号，如 WM_CREATE / WM_SIZE / WM_COMMAND。
//    wParam, lParam —— 消息的两个参数，含义随 message 而变。
//  返回：LRESULT。凡是自己处理掉的消息都 return 0；没处理的交给
//        DefWindowProcW() 做系统默认处理（这一步不能少，否则窗口不能拖动、
//        不能关闭、Static 控件也不会画）。
//
//  【Win32 编程的核心心智模型】窗口过程就是一个大 switch：系统在合适的时机
//  把消息投递进来，你挑需要的处理，剩下的“甩”给默认过程。子控件（按钮、
//  滑块、编辑框）自己也是窗口，它们的“用户操作”会以两种方式通知父窗口：
//    · 按钮 / 编辑框 → 转成 WM_COMMAND，wParam 高位是通知码（EN_CHANGE 等），
//      低位是控件 ID；lParam 是子控件句柄；
//    · 滑块（trackbar）→ 转成 WM_HSCROLL，因为滑块“横向滚动”。
//  这就是为什么下面要分开处理 WM_COMMAND 和 WM_HSCROLL。
// ---------------------------------------------------------------------------
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    // -----------------------------------------------------------------------
    //  WM_CREATE：窗口正在被创建，这是“一次性初始化”的地方
    //  返回 0 表示创建成功；如果返回 -1，CreateWindowEx 会失败。
    //  此刻 hWnd 就是本页窗口，把它存进 s_hPage 供其它函数使用。
    //
    //  创建控件统一用 CreateCtl()（定义在 Project001.cpp），它相当于
    //  CreateWindowExW + 设置字体（继承全局 UI 字体）+ 一些默认处理，
    //  参数依次是：窗口类名 / 文字 / 样式 / 父窗口 / 控件 ID。
    //  控件 ID 用 App.h 里的 IDC_CM_* 常量（1500~1515 号段，本页专用）。
    //
    //  【各种控件类名的小知识】
    //    · L"Button" 用于按钮、单选（BS_AUTORADIOBUTTON）、分组框（BS_GROUPBOX）；
    //      分组框本质就是一个画了边框和标题的按钮，没有实际功能；
    //    · L"Static" 是静态文字/图片区；
    //    · L"Edit" 是输入框；
    //    · L"msctls_trackbar32" 是滑块（trackbar）。它属于“通用控件”
    //      (commctrl)，所以 App.h 里才需要 #include <commctrl.h> 并链接 comctl32、
    //      以及那条 Common-Controls 6.0 manifest（否则滑块会是 Win95 老外观）。
    // -----------------------------------------------------------------------
    case WM_CREATE:
        {
            s_hPage = hWnd;

            // 预览区。SS_OWNERDRAW 表示“不要系统自己画，交给父窗口画”——
            // 系统随后会给父窗口发 WM_DRAWITEM，我们在那里调 PaintImagePreview()。
            // 不用 PictureBox 那种东西，是因为 Win32 根本没这控件。
            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CM_PREVIEW);

            // ---- 「压缩方式」组：两个互斥的单选按钮 ----
            // BS_AUTORADIOBUTTON = 自动单选（选了它，同组其它自动取消勾选）。
            // 【关键】“同一组”的判定标准是：从 WS_GROUP 标志的按钮开始，直到
            // 下一个带 WS_GROUP 的按钮为止。所以这里第一个单选必须带 WS_GROUP，
            // 否则它会和『输出格式』那组串成一组，选一个就把另一个也取消掉。
            // WS_TABSTOP 让按钮能用 Tab 键切换焦点。
            s_hGrpMode = CreateCtl(L"Button", L"压缩方式", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CM_GRP_MODE);
            s_hRadioQuality = CreateCtl(L"Button", L"按画质（拖动滑块，实时预估体积）",
                                        WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                        hWnd, IDC_CM_RADIO_QUALITY);
            s_hRadioTarget = CreateCtl(L"Button", L"按目标大小（自动算最合适的画质）",
                                       WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                       hWnd, IDC_CM_RADIO_TARGET);
            // Button_SetCheck = 给按钮发 BM_SETCHECK。注意设勾选状态**不会**
            // 产生 BN_CLICKED 通知，所以这里不会触发 WM_COMMAND——事件循环里
            // 也就不存在“刚创建就预估一次”的问题（最后手动调用了初始化函数）。
            Button_SetCheck(s_hRadioQuality, BST_CHECKED);   // 默认选「按画质」

            // ---- 「压缩参数」组 ----
            s_hGrpParam = CreateCtl(L"Button", L"压缩参数", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CM_GRP_PARAM);
            s_hTrkQuality = CreateCtl(L"msctls_trackbar32", L"",
                                      WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_CM_TRK_QUALITY);
            // 下面三句是 trackbar 的初始化，全都要用 SendMessageW 发消息：
            //   TBM_SETRANGE  —— 设范围。第二个参数 TRUE 表示“拖动时立刻通知父窗口”
            //                    （否则要等松手才发 WM_HSCROLL，就没有实时预估了）。
            //                    范围值用 MAKELPARAM(最小, 最大) 打包成 32 位：
            //                    低 16 位是最小值，高 16 位是最大值。
            //                    【10 / 100】就是画质的允许区间：低于 10 图像会糊成一片，
            //                    100 是“最高质量”的惯例上限。
            //   TBM_SETPOS    —— 设初始位置，TRUE 表示连滑块一起重画。默认 80：
            //                    “质量还行、体积也小”的一个常用折中值。
            //   TBM_SETTICFREQ—— 每 10 个单位画一个刻度线（10,20,...,100），纯视觉提示。
            // 【坑】想让滑块“移动”必须发 TBM_SETPOS；而如果反过来自己 PostMessage
            // 一条假的 WM_HSCROLL 给父窗口，只是让父窗口以为滑块动了，滑块本身
            // 的位置并不会变（Pos 还是旧值），读出来就是错的。真要用代码改位置，
            // 就得像上面这样用 TBM_SETPOS，必要时再手动调一次处理函数。
            SendMessageW(s_hTrkQuality, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
            SendMessageW(s_hTrkQuality, TBM_SETPOS, TRUE, 80);
            SendMessageW(s_hTrkQuality, TBM_SETTICFREQ, 10, 0);

            s_hLblQuality = CreateCtl(L"Static", L"", WS_VISIBLE, hWnd, IDC_CM_LBL_QUALITY);

            // 目标大小输入框。样式解释：
            //   ES_NUMBER    —— 只允许输入数字（系统级过滤，用户打不进字母）。
            //                   注意它允许空字符串，所以 TargetBytes() 里必须兜底。
            //   ES_AUTOHSCROLL —— 文字超出右边界时自动横向滚动，不换行。
            //   WS_BORDER    —— 画一圈边框，不然看不出这是个输入框。
            s_hEditTarget = CreateCtl(L"Edit", L"",
                                      WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                                      hWnd, IDC_CM_EDIT_TARGET);
            // SetWindowTextW 设初始值 500（KB）。注意：用 SetWindowTextW **不会**
            // 产生 EN_CHANGE 通知（那是用户键入才有的），所以不会引起额外的预估。
            SetWindowTextW(s_hEditTarget, L"500");
            s_hLblUnit = CreateCtl(L"Static", L"KB（目标文件大小）", WS_VISIBLE, hWnd, IDC_CM_LBL_UNIT);

            // ---- 「输出格式」组：三个互斥单选 ----
            // 同样，第一个带 WS_GROUP，把这一组和上一组隔开。
            s_hGrpFormat = CreateCtl(L"Button", L"输出格式", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CM_GRP_FORMAT);
            s_hRadioKeep = CreateCtl(L"Button", L"保持原格式（PNG 无损 / JPEG 有损）",
                                     WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                     hWnd, IDC_CM_RADIO_KEEP);
            s_hRadioJpeg = CreateCtl(L"Button", L"转成 JPEG（压缩率最高）",
                                     WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, hWnd, IDC_CM_RADIO_JPEG);
            s_hRadioPng = CreateCtl(L"Button", L"转成 PNG（无损，通常更大）",
                                    WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, hWnd, IDC_CM_RADIO_PNG);
            Button_SetCheck(s_hRadioKeep, BST_CHECKED);       // 默认「保持原格式」

            // ---- 信息文字与底部按钮 ----
            // SS_LEFTNOWORDWRAP：多行左对齐、不自动折行——换行位置完全由我们
            // 自己写进字符串的 \r\n 决定，排版才可控。
            s_hLblOrig = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CM_LBL_ORIG);
            s_hLblEst = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CM_LBL_EST);
            // BS_DEFPUSHBUTTON：默认按钮（回车时相当于点它），会带一圈高亮边框。
            s_hBtn = CreateCtl(L"Button", L"压缩并输出", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_CM_BTN_PROCESS);

            // 三个初始化调用：先按模式禁用对应控件，再填信息文字，
            // 最后手工排一次版（因为 WM_SIZE 可能在这之前就到了，或者根本还没到）。
            UpdateModeEnable();
            UpdateOriginInfo();
            Layout();
        }
        return 0;

    // -----------------------------------------------------------------------
    //  WM_SIZE：客户区大小变了（窗口被缩放、或主窗口刚把本页摆到位）
    //  重新排版，并让预览区重画（预览是按控件大小自适应的，尺寸变了要重绘）。
    //  InvalidateRect(hwnd, nullptr, TRUE) 的第三个参数 TRUE 表示“顺便擦掉背景”，
    //  这样旧图不会和新图叠在一起。
    // -----------------------------------------------------------------------
    case WM_SIZE:
        Layout();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    // -----------------------------------------------------------------------
    //  WM_TIMER：定时器到点了。wParam 就是定时器 ID，用它区分是哪个定时器。
    //
    //  【为什么在这里做预估】SetTimer 只是“到点提醒”，真正的重活放在消息处理里，
    //  这样在这 320ms 等待期间，窗口仍然能正常响应拖动、重绘等消息，界面不卡。
    //
    //  一进来先 KillTimer：本定时器是一次性的（防抖后只算一次），算完不再需要它。
    //  不 Kill 的话它会每 320ms 反复触发，变成不停重算。
    //  【大图提醒】DoEstimate() 里的编码是同步的，图片很大时这几百毫秒内界面
    //  会短暂无响应——这是本页的已知取舍（真要彻底解决得开 worker 线程，
    //  但那样还要处理回调、同步和 GDI+ 线程初始化，复杂度高得多）。
    // -----------------------------------------------------------------------
    case WM_TIMER:
        if (wParam == TIMER_ESTIMATE)
        {
            KillTimer(hWnd, TIMER_ESTIMATE);   // 一次性：先取消，避免反复触发
            DoEstimate();                      // 真正干活
        }
        return 0;

    // -----------------------------------------------------------------------
    //  WM_HSCROLL：横向滚动条类控件的通知消息，trackbar 就是走这条。
    //  lParam 是滑块控件句柄，wParam 低 16 位是滚动通知码（TB_THUMBTRACK、
    //  TB_LINEUP 等）。本页不关心具体是哪种操作——无论是拖动、点箭头还是
    //  按方向键，处理方式都一样：更新文字 + 安排一次防抖预估。
    //
    //  【坑 / 易混点】不能改成用 WM_COMMAND：trackbar 不发 WM_COMMAND。
    //  反过来，如果自己 PostMessage 一条 WM_HSCROLL 给本窗口来“模拟拖动”，
    //  这只会让这里跑一遍，滑块自身的 Pos 不会变，Quality() 读出来还是旧值。
    //  要真改位置得发 TBM_SETPOS（见 WM_CREATE 里的初始化）。
    // -----------------------------------------------------------------------
    case WM_HSCROLL:
        UpdateQualityLabel();     // "画质 80%" 立刻跟上，让手感灵敏
        ScheduleEstimate();       // 体积预估走防抖，320ms 后才算
        return 0;

    // -----------------------------------------------------------------------
    //  WM_APP_IMAGE_CHANGED：自定义消息（= WM_APP + 2），主窗口在载入新图片后
    //  广播给所有功能页。WM_APP 及以上是给应用自己用的消息号，Windows 不会占用，
    //  这样各页之间完全不用互相认识，只认“图片换了”这一条约定。
    //
    //  收到后要做的四件事：
    //    1. RebuildPreview()：重建左侧缩略图；
    //    2. 重置目标大小输入框的默认值 —— 用源文件的 1/4 体积当默认目标，
    //       因为“压到四分之一”是常见需求。命令行：src/1024/4 把字节换成 KB
    //       再取四分之一；小于 10 KB 时抬到 10，免得小图给个 1、2 KB 的目标
    //       根本达不到。swprintf_s 的 %llu 要配 (unsigned long long) 强转，
    //       否则 32 位下格式串和实参类型不匹配。
    //    3. UpdateModeEnable() / UpdateOriginInfo()：刷新控件可用性和信息文字；
    //    4. DoEstimate()：立刻算一次（这里**不**走防抖——换图是低频动作，
    //       用户预期马上看到数字，而且换图后旧数字留着会误导人）；
    //       顺便让预览区重画。
    // -----------------------------------------------------------------------
    case WM_APP_IMAGE_CHANGED:
        RebuildPreview();
        {
            const size_t src = SourceFileSize();
            if (src > 0)
            {
                size_t kb = src / 1024 / 4;      // 默认目标：原体积的 1/4
                if (kb < 10) kb = 10;            // 下限 10 KB，太小的目标没意义
                wchar_t buf[32];
                swprintf_s(buf, L"%llu", (unsigned long long)kb);
                SetWindowTextW(s_hEditTarget, buf);
            }
        }
        UpdateModeEnable();
        UpdateOriginInfo();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        DoEstimate();                            // 立刻算，不走防抖
        return 0;

    // -----------------------------------------------------------------------
    //  WM_DRAWITEM：自绘消息。凡是带 SS_OWNERDRAW / BS_OWNERDRAW 的控件都不由
    //  系统绘制，而是给父窗口发这条消息，lParam 指向一个 DRAWITEMSTRUCT，
    //  里面有：hDC（画布）、rcItem（可画区域矩形）、CtlID（哪个控件）等。
    //
    //  本页只自绘预览区：检查 wParam（控件 ID）确认是预览，再把 lParam 当
    //  DRAWITEMSTRUCT* 用（先判空，防野指针），取出画布和矩形交给
    //  PaintImagePreview()（定义在 ImageOps.cpp，负责画棋盘格背景 + 居中缩放图片，
    //  图片为空时画 hint 文字）。
    //  return TRUE 表示“这个绘制我处理了”，系统就不会再画一遍。
    //
    //  【易混点】WM_DRAWITEM 只用于自绘（OwnerDraw）控件！普通按钮被点击发的是
    //  WM_COMMAND + BN_CLICKED，两者完全不同。所以如果哪天想用 BM_CLICK 模拟点
    //  「压缩并输出」，会收到 BN_CLICKED（程序正常处理）；而 BM_CLICK 只是
    //  “替用户点了一下”的消息，它不会绕过按钮去直接调 OnProcess，
    //  也就不会因为没人在按钮上按而丢掉消息。
    // -----------------------------------------------------------------------
    case WM_DRAWITEM:
        if (wParam == IDC_CM_PREVIEW && lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            PaintImagePreview(dis->hDC, dis->rcItem, s_pPreview,
                              L"点击上方「选择图片...」载入图片\r\n载入后即可实时预估压缩后的体积");
            return TRUE;   // 已处理，别再走默认绘制
        }
        break;   // 不是预览控件 → 交给 DefWindowProcW

    // -----------------------------------------------------------------------
    //  WM_COMMAND：子控件的命令/通知消息。参数解码方式固定：
    //    LOWORD(wParam) = 控件 ID（就是我们 CreateCtl 时传的那个 IDC_CM_* 号）；
    //    HIWORD(wParam) = 通知码（按钮是 BN_CLICKED，编辑框是 EN_CHANGE 等）；
    //    lParam         = 子控件窗口句柄。
    //
    //  【按钮 vs 编辑框的区别】
    //    · 点按钮 → BN_CLICKED（值 0）。因为很多按钮的通知码就是 0，所以这里
    //      只比 ID、不比 code；这也是惯例写法。
    //    · 输入框内容变化 → EN_CHANGE（值 0x0300）。**只在用户用键盘/粘贴
    //      改动内容时发**；用 SetWindowTextW 以代码改内容不会触发（所以
    //      WM_CREATE 和 WM_APP_IMAGE_CHANGED 里用代码改输入框，都不会引起
    //      额外的 WM_COMMAND）。
    //
    //  处理原则：凡是“会影响输出结果”的选项变化，都要调 ScheduleEstimate()
    //  重新预估（走防抖，因为用户可能连着改好几下）。
    // -----------------------------------------------------------------------
    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);

            if (id == IDC_CM_BTN_PROCESS)
            {
                // 点「压缩并输出」：真正保存文件。这里不加 code == BN_CLICKED 判断，
                // 因为按钮也只有这一种通知。
                OnProcess(hWnd);
                return 0;
            }
            if (id == IDC_CM_RADIO_QUALITY || id == IDC_CM_RADIO_TARGET)
            {
                // 切换压缩方式：该禁用的控件要变，预估算法也完全不同，
                // 所以两个刷新都要做。
                UpdateModeEnable();
                ScheduleEstimate();
                return 0;
            }
            if (id == IDC_CM_RADIO_KEEP || id == IDC_CM_RADIO_JPEG || id == IDC_CM_RADIO_PNG)
            {
                // 切换输出格式：会影响“输出格式”那行文字，也会影响预估
                //（比如从 PNG 换成 JPEG，体积会小很多）。
                UpdateOriginInfo();
                ScheduleEstimate();
                return 0;
            }
            if (id == IDC_CM_EDIT_TARGET && code == EN_CHANGE)
            {
                // 用户改了目标 KB。注意要显式判断 code == EN_CHANGE，
                // 因为 Edit 控件还会发 EN_SETFOCUS / EN_KILLFOCUS 等其它通知，
                // 那些不该触发预估。
                // 另外：用户每按一个数字键就发一次 EN_CHANGE，所以这里必须走
                // 防抖；否则输入“500”就会连算三次编码，界面直接卡住。
                ScheduleEstimate();
                return 0;
            }
        }
        break;   // 其它控件/通知 → 默认处理

    default:
        break;
    }
    // 所有没被上面处理的消息都交给默认过程：窗口的拖动、关闭、
    // Static 控件的默认绘制等基础行为全靠它。
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  PageCompress_Create —— 创建本页窗口（供主窗口 Project001.cpp 调用）
//  参数：parent —— 主窗口句柄，本页将作为它的子窗口。
//  返回：创建好的页面窗口句柄；注册窗口类失败时返回 nullptr。
//
//  【窗口类只需要注册一次】RegisterClassExW 必须在 CreateWindowExW 之前调用，
//  且同一个类名重复注册会失败（GetLastError 返回 ERROR_CLASS_ALREADY_EXISTS）。
//  用 static bool registered 做一次性标记；同时把“已经存在”也当成成功，
//  这样即使别的路径先注册过也不会误判成失败（多重防御）。
//
//  WNDCLASSEXW 各字段：
//    cbSize         —— 结构体大小，必须填，Windows 靠它判版本；
//    style          —— CS_HREDRAW | CS_VREDRAW：窗口宽或高变化时整块重画，
//                      免得残留旧内容；
//    lpfnWndProc    —— 本类的窗口过程，就是上面的 Proc()。以后这个类的每个
//                      窗口收到消息，都由它处理；
//    hInstance      —— 从 App.h 的全局 g_hInst 来（Project001.cpp 里 WinMain 时保存的）；
//    hCursor        —— 鼠标在本页上时的箭头光标；
//    hbrBackground  —— 背景刷，(HBRUSH)(COLOR_WINDOW + 1) 表示“用系统窗口色”，
//                      这也是那个奇怪的 +1 的原因：传系统颜色时要 +1 以免和
//                      自定义画刷句柄混淆，这是 Win32 的历史约定；
//    lpszClassName  —— 类名，必须和 CreateWindowExW 里的一致；
//    【注意】这里没设 hIcon——子窗口不显示在任务栏，用不到图标。
//
//  CreateWindowExW 的样式：
//    WS_CHILD         —— 作为子窗口，被裁剪在父窗口客户区内；
//    WS_CLIPCHILDREN  —— 画父窗口时跳过子窗口占的区域，减少闪烁。
//    【另外】这里没给 WS_VISIBLE，所以刚创建出来是隐藏的，等主窗口决定
//    切到本页时再显示（各功能页统一这么写，避免启动时闪一下）。
//    初始位置 0,0、大小 10×10 也只是占位，主窗口随后会用 MoveWindow 摆正。
// ---------------------------------------------------------------------------
HWND PageCompress_Create(HWND parent)
{
    static bool registered = false;          // 窗口类只需注册一次
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = Proc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Pic246PageCompress";
        // 注册失败且原因不是“类已存在” → 真的失败了，返回 nullptr。
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    return CreateWindowExW(0, L"Pic246PageCompress", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
