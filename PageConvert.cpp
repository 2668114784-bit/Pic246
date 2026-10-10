// ============================================================================
//  PageConvert.cpp —— 功能页：格式转换
//    · 列出系统可用的图片编码器（PNG / JPEG / BMP / GIF / TIFF / WEBP ...）
//    · 可选有损格式的编码质量，实时预估输出体积
//    · 目标格式不支持透明时，可用填充色把透明区域填掉
//    · 输出到原目录，后缀为 _<扩展名>
// ============================================================================
// ============================================================================
//  【文件说明】—— 第一次读本文件的同学，请先看这一段
//
//  一、这个文件负责什么？
//      它是 Pic246 主窗口右侧「格式转换」功能页的全部实现。页面上左边是预览图，
//      右边是一列控件：目标格式下拉框、画质滑块、「保留透明背景」复选框、
//      填充色按钮 + 颜色小方块(swatch)、预估体积文字，最下面是「转换并输出」按钮。
//      用户点按钮后，本页把源图编码成目标格式写到原图所在目录，
//      文件名为「原文件名去掉扩展名 + _ + 新扩展名」，例如 a.jpg -> a_png.png。
//
//  二、和其它文件怎么配合？
//      · Project001.cpp —— 程序入口和主窗口。它调用本文件末尾的
//        PageConvert_Create() 创建本页；用户载入新图片后，主窗口给本页发
//        WM_APP_IMAGE_CHANGED 消息，本页据此刷新预览和预估。
//      · App.h          —— 公共声明：全局状态（g_pSrcImage / g_srcW / g_srcH /
//        g_srcHasAlpha / g_srcPath / g_fillColor）、通用工具（D() 按系统 DPI 缩放、
//        CreateCtl() 建控件、FormatBytes() 把字节数格式化成 "1.2 MB" 之类）、
//        控件 ID（IDC_CV_*）、自定义消息（WM_APP_IMAGE_CHANGED）。
//      · ImageOps.cpp   —— 真正干活的地方，本文件只是调用方：
//          ListImageEncoders()    枚举系统里真实可用的 GDI+ 编码器
//          SourceFileSize()       源文件在磁盘上的字节数（算「约为原来的 x%」用）
//          EstimateEncodedSize()  把位图编码到内存里量长度（只预估，不写盘）
//          SaveBitmapAsFormat()   真正编码并写文件（注意：它内部会 delete 传入的位图）
//          CreatePreviewSource()  生成缩小后的预览位图
//          PaintImagePreview() / PaintColorSwatch()  自绘预览图 / 颜色小方块
//
//  三、大致流程（认识几个消息就够读全文了）
//      WM_CREATE            建全部子控件 -> 枚举编码器塞进下拉框 -> 默认选第 0 项
//      WM_SIZE              调 Layout() 重新摆放控件（尺寸都套 D() 做 DPI 缩放）
//      WM_COMMAND           按钮点击、下拉框选择变化(CBN_SELCHANGE)、复选框切换
//      WM_HSCROLL           画质滑块被拖动
//      WM_TIMER             定时器到点，才真正做一次体积预估（防抖，见 ScheduleEstimate）
//      WM_APP_IMAGE_CHANGED 有新图片载入了：重建预览、默认选源格式、重新预估
//      WM_DRAWITEM          自绘预览图（含提示文字）和颜色小方块
//      「转换并输出」-> OnProcess() -> BuildOutputBitmap() -> SaveBitmapAsFormat()
// ============================================================================
#include "App.h"

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 控件句柄。Win32 里每个控件都是一个子窗口，用 HWND 句柄代表它。
// 这里统一用 static 全局保存，好处是任何一个函数（Layout / WM_COMMAND ...）
// 都能直接拿来用，不用层层传参；坏处是"本页只能存在一份"，对本程序正合适。
// ---------------------------------------------------------------------------
static HWND s_hPage = nullptr;          // 本页自身的窗口句柄（也是定时器、MessageBox 的宿主）
static HWND s_hPreview = nullptr;       // 左半边的大预览图（自绘 Static）
static HWND s_hGrpFormat = nullptr;     // 「目标格式」分组框（BS_GROUPBOX）
static HWND s_hCombo = nullptr;         // 目标格式下拉框（ComboBox）
static HWND s_hLblFmtInfo = nullptr;    // 分组框里的两行说明文字
static HWND s_hGrpOpt = nullptr;        // 「编码选项」分组框
static HWND s_hTrkQuality = nullptr;    // 画质滑块（msctls_trackbar32）
static HWND s_hLblQuality = nullptr;    // 滑块下方的画质文字
static HWND s_hChkTrans = nullptr;      // 「保留透明背景」复选框
static HWND s_hSwatch = nullptr;        // 填充色小方块（自绘 Static）
static HWND s_hBtnColor = nullptr;      // 「选择填充颜色...」按钮
static HWND s_hLblEst = nullptr;        // 「预估大小：...」文字
static HWND s_hBtn = nullptr;           // 「转换并输出」按钮

static Bitmap* s_pPreview = nullptr;             // 预览用的缩小位图；由本页 new，也由本页 delete
static std::vector<EncoderChoice> s_encoders;    // 系统可用编码器列表（下拉框的第 i 项就是它）
static bool s_estimating = false;                // 重入保护：预估过程中又触发预估时直接跳过

// 定时器 ID。SetTimer/KillTimer 用整数标识定时器，本页只有这一个，取 1 即可
#define TIMER_ESTIMATE 1

// ---------------------------------------------------------------------------
// 取当前下拉框选中的编码器。
//
// 参数：无（从 s_hCombo 的当前选择项和 s_encoders 列表推导）
// 返回：指向 s_encoders 中那一项的指针；没有选中项或列表为空时返回 nullptr。
//
// 说明：ComboBox 自己只存"第几项"（索引），真正的名字/MIME/扩展名在 s_encoders 里，
//       所以每次都要用 CB_GETCURSEL 把索引换成数据，再取对应元素。
//       返回的是 s_encoders 内部元素的地址，调用者别去 delete 它。
// ---------------------------------------------------------------------------
static const EncoderChoice* CurEncoder()
{
    const int sel = (int)SendMessageW(s_hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s_encoders.size()) return nullptr;
    return &s_encoders[sel];
}

// ---------------------------------------------------------------------------
// 判断某个 MIME 是不是"有损压缩"格式。
//
// MIME 是"IANA 媒体类型"，形如 image/jpeg；GDI+ 用 MIME 字符串（不是 CLSID）
// 来查找编码器，本程序也就用它当格式的唯一标识。
// 有损格式才需要画质滑块：JPEG 和 WebP 是有损的，PNG/GIF/BMP/TIFF 是无损的，
// 给无损格式传 quality 参数没有意义。
// _wcsicmp 是 Windows 提供的不区分大小写的宽字符比较，返回 0 表示相等。
// ---------------------------------------------------------------------------
static bool IsLossy(const std::wstring& mime)
{
    return _wcsicmp(mime.c_str(), L"image/jpeg") == 0 ||
           _wcsicmp(mime.c_str(), L"image/webp") == 0;
}

// ---------------------------------------------------------------------------
// 判断某个格式能不能保留透明通道（alpha）。
//
// 这是本页最容易搞错的地方，务必分清：
//   · 能保留透明：PNG（最常用）、GIF（只能 1 位透明，半透明会被丢弃）、
//     TIFF（要带 alpha 通道的变体）、WebP。
//   · 不能保留透明：JPEG、BMP。往这两种格式存带透明的图，透明部分会变黑/变脏，
//     所以本页改用"先用填充色把透明区域铺掉，再编码"的办法（见 BuildOutputBitmap）。
// 是否需要铺底色，最终取决于三件事同时成立：
//   ①源图确实有透明像素 g_srcHasAlpha（源图完全不透明就没必要保留或铺底）；
//   ②目标格式支持透明；③用户勾了「保留透明背景」。
// 三个条件缺一，就得用底色填充。
// ---------------------------------------------------------------------------
static bool EncoderKeepsAlpha(const std::wstring& mime)
{
    return _wcsicmp(mime.c_str(), L"image/png") == 0 ||
           _wcsicmp(mime.c_str(), L"image/gif") == 0 ||
           _wcsicmp(mime.c_str(), L"image/tiff") == 0 ||
           _wcsicmp(mime.c_str(), L"image/webp") == 0;
}

// 读滑块当前位置（0..100 的画质值）。TBM_GETPOS 是 Trackbar 的标准消息。
static int Quality() { return (int)SendMessageW(s_hTrkQuality, TBM_GETPOS, 0, 0); }
// 复选框是否被勾选。BST_CHECKED 表示勾上（Button_GetCheck 是 commctrl 里的宏）。
static bool KeepTransparent() { return Button_GetCheck(s_hChkTrans) == BST_CHECKED; }

// ---------------------------------------------------------------------------
// 刷新滑块下方那行文字（例如"编码质量 90%"）。
// 三种情况：没选格式 -> 显示破折号；无损格式 -> 提示"与画质无关"；
//           有损格式 -> 显示百分比。
// swprintf_s 是"安全版"的宽字符格式化，必须给缓冲区大小，防止溢出。
// ---------------------------------------------------------------------------
static void UpdateQualityLabel()
{
    const EncoderChoice* e = CurEncoder();
    wchar_t buf[128];
    if (!e)
        wcscpy_s(buf, L"—");
    else if (!IsLossy(e->mime))
        wcscpy_s(buf, L"该格式为无损编码，与画质无关");
    else
        swprintf_s(buf, L"编码质量 %d%%", Quality());
    SetWindowTextW(s_hLblQuality, buf);
}

// ---------------------------------------------------------------------------
// 根据"当前选了什么格式/源图有没有透明/复选框状态"统一刷新控件的可用状态。
// 这就是 Win32 程序常见的"一处集中算状态，别处只管调用"写法：
// 任何影响状态的操作（换格式、切复选框、载入新图）之后都调它一次。
//
// EnableWindow(h, FALSE) 会让控件变灰且不可点击；
// 关键点：变灰的同时要把「保留透明背景」强制取消勾选，否则会出现
// "控件是灰的、内部状态却还勾着"的不一致，用户换回 PNG 时会莫名变成不保留透明。
// ---------------------------------------------------------------------------
static void UpdateEnable()
{
    const EncoderChoice* e = CurEncoder();
    if (!e) return;

    const bool lossy = IsLossy(e->mime);
    EnableWindow(s_hTrkQuality, lossy);   // 无损格式时滑块变灰

    const bool canAlpha = EncoderKeepsAlpha(e->mime) && g_srcHasAlpha;
    EnableWindow(s_hChkTrans, canAlpha);
    if (!canAlpha) Button_SetCheck(s_hChkTrans, BST_UNCHECKED);
    // useFill = 需要/必须使用底色填充：要么格式不支持透明，要么用户主动放弃保留
    const bool useFill = !canAlpha || !KeepTransparent();
    EnableWindow(s_hSwatch, useFill);
    EnableWindow(s_hBtnColor, useFill);

    UpdateQualityLabel();

    wchar_t info[256];
    swprintf_s(info, L"%s\r\n%s",
               canAlpha ? L"目标格式支持透明，可保留透明背景。"
                        : L"目标格式不支持透明，透明区域将用下面的底色填充。",
               L"输出尺寸与原图一致，不做缩放。");
    SetWindowTextW(s_hLblFmtInfo, info);
    // 颜色小方块是自绘控件，状态变了要让它重画一次（TRUE = 先擦背景再画）
    InvalidateRect(s_hSwatch, nullptr, TRUE);
}

// 构造真正会被写出的位图（32bppARGB，必要时先铺底色）。
// 预估与实际保存必须用同一份数据，否则 BMP/TIFF 这类按位深计大小的格式会差很多
// （源图是 24bpp 的 JPEG 时，直接对源图编码得到的是 24bpp BMP，只有实际输出的 3/4）。
//
// 参数：keepAlpha —— true 表示保留透明通道；false 表示先用 g_fillColor 铺底再画源图。
// 返回：新 new 出来的 32bppARGB 位图，调用者负责 delete；失败返回 nullptr。
//
// 为什么不能直接拿 g_pSrcImage 去编码？两个原因：
//   ①编码结果必须和预估结果一致，就要统一成同一种像素格式（统一 32bppARGB），
//     否则 BMP/TIFF 这种"按位深算大小"的格式，预估和实际会差一大截；
//   ②目标格式不支持透明时，必须先合成到不透明底色上。
// 两个 GDI+ 细节：
//   · CompositingModeSourceCopy —— 直接覆盖像素（连 alpha 一起写），用于 Clear 铺底；
//   · CompositingModeSourceOver —— 正常的"源覆盖目标"混合，绘制带透明的源图时用它，
//     这样源图半透明的像素才不会被当成"直接把目标像素覆盖成透明"。
// ---------------------------------------------------------------------------
static Bitmap* BuildOutputBitmap(bool keepAlpha)
{
    if (!g_pSrcImage || g_srcW <= 0 || g_srcH <= 0) return nullptr;

    Bitmap* work = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
    // GDI+ 的构造函数不抛异常，要用 GetLastStatus() 判断是否成功
    if (work->GetLastStatus() != Ok) { delete work; return nullptr; }

    Graphics g(work);
    g.SetCompositingMode(CompositingModeSourceCopy);
    if (!keepAlpha)
    {
        // GetRValue/GetGValue/GetBValue 从 COLORREF 里拆出 RGB 三通道；
        // Color(alpha, r, g, b) 第 1 个参数是透明度，255 = 完全不透明（底色必须不透明）
        g.Clear(Color(255, GetRValue(g_fillColor), GetGValue(g_fillColor), GetBValue(g_fillColor)));
        g.SetCompositingMode(CompositingModeSourceOver);
    }
    g.DrawImage(g_pSrcImage, 0, 0, (INT)g_srcW, (INT)g_srcH);
    return work;
}

// ---------------------------------------------------------------------------
// 安排一次"稍后再做"的体积预估（防抖 / debounce）。
//
// 为什么需要它？拖动 Trackbar 会连续产生大量 WM_HSCROLL 消息，
// 若每条都去做一次完整编码（要遍历整张图，JPEG 尤其慢），界面会卡死。
// 做法：每次事件先把上一次的定时器杀掉、重新设一个 320ms 的定时器；
// 只要用户还在连续拖动，定时器就一直被推迟，直到停下来 320ms 才真正执行一次。
// 定时器消息 WM_TIMER 会发到"设置定时器的那个窗口"，也就是本页，
// 所以 WM_TIMER 分支里第一件事就是 KillTimer（一次性定时器要自己收尾）。
// ---------------------------------------------------------------------------
static void ScheduleEstimate()
{
    if (!s_hPage) return;
    KillTimer(s_hPage, TIMER_ESTIMATE);
    SetTimer(s_hPage, TIMER_ESTIMATE, 320, nullptr);
}

// ---------------------------------------------------------------------------
// 真正做一次预估：编码到内存量出字节数，显示成"预估大小：..."。
//
// 要点：
//   · 预估用的 quality、是否保留透明，必须和 OnProcess() 里真正保存时完全一致，
//     否则预估就是骗人的；
//   · 预估用的是临时位图 work，算完立刻 delete，不占用内存；
//   · s_estimating 是重入锁：预估过程本身可能触发消息（例如设置沙漏光标、改文字），
//     若又被要求预估一次就什么都不做，避免无限递归/卡死；
//   · 计算期间把光标换成沙漏（IDC_WAIT），结束再换回原来的，告诉用户"在忙"。
// ---------------------------------------------------------------------------
static void DoEstimate()
{
    if (!s_hLblEst) return;
    if (!g_pSrcImage) { SetWindowTextW(s_hLblEst, L"预估大小：—"); return; }
    const EncoderChoice* e = CurEncoder();
    if (!e) { SetWindowTextW(s_hLblEst, L"预估大小：—"); return; }

    if (s_estimating) return;
    s_estimating = true;
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    // 无损格式统一按 100 传（编码器会忽略它，但保持行为一致、预估与保存一致）
    const ULONG q = (ULONG)(IsLossy(e->mime) ? Quality() : 100);
    // 是否保留透明：格式支持 且 源图确实有透明 且 用户勾了复选框
    const bool keepAlphaEst = EncoderKeepsAlpha(e->mime) && g_srcHasAlpha && KeepTransparent();
    Bitmap* work = BuildOutputBitmap(keepAlphaEst);
    const size_t est = work ? EstimateEncodedSize(work, e->mime, q) : 0;
    delete work;
    const size_t src = SourceFileSize();

    wchar_t buf[512];
    if (est == 0)
    {
        swprintf_s(buf, L"预估大小：计算失败");
    }
    else if (src > 0)
    {
        // %.0f 表示不显示小数；(double) 强制转换避免整数除法丢掉小数部分
        swprintf_s(buf, L"预估大小：%s（原文件 %s，约为原来的 %.0f%%）",
                   FormatBytes(est).c_str(), FormatBytes(src).c_str(),
                   100.0 * (double)est / (double)src);
    }
    else
    {
        swprintf_s(buf, L"预估大小：%s", FormatBytes(est).c_str());
    }
    SetWindowTextW(s_hLblEst, buf);

    SetCursor(hOld);      // 恢复原光标
    s_estimating = false;
}

// ---------------------------------------------------------------------------
// 重建预览位图。
// 源图换了（WM_APP_IMAGE_CHANGED）才需要重建：CreatePreviewSource(640) 会返回
// 一张长边不超过 640 像素的新位图，调用者负责释放 —— 所以旧的必须先 delete，
// 否则每次换图都漏一份内存。640 是预览的显示上限，不影响输出尺寸。
// ---------------------------------------------------------------------------
static void RebuildPreview()
{
    if (s_pPreview) { delete s_pPreview; s_pPreview = nullptr; }
    s_pPreview = CreatePreviewSource(640);
}

// ---------------------------------------------------------------------------
// 布局：把每个控件用 MoveWindow 摆到该在的位置。
// 每次 WM_SIZE 都会调一次，所以要能反复执行、不依赖"第一次"的状态。
//
// 布局思路（横向分两块）：
//   左边预览：从左边距 M 开始，宽度 = 总宽 - 面板宽 - 3*M；
//   右边面板：从 px = 2*M + prevW 开始，宽度 pw = 总宽 - px - M。
// 右侧控件是从上往下"流水账"排的：登记一个 y (py)，用完就加上本组高度和间距。
// 所有尺寸都套 D()（DIP -> 实际像素）做 DPI 缩放，这样 125%/150% 缩放下不会错位。
// 两处宽度保护是为了窗口被拖得很窄时，面板不至于挤压到负数宽度。
// ---------------------------------------------------------------------------
static void Layout()
{
    RECT rc;
    GetClientRect(s_hPage, &rc);                  // 取本页客户区大小（不含标题栏，子窗口没有边框）
    const int cx = rc.right, cy = rc.bottom;
    const int M = D(12);                          // 统一外边距

    int panelW = D(340);                          // 右侧面板理想宽度
    if (panelW > cx - D(160)) panelW = cx - D(160);   // 窗口太窄 -> 压缩面板
    if (panelW < D(240)) panelW = D(240);             // 但不能小于 240，否则控件挤成一条

    int prevW = cx - panelW - M * 3;
    if (prevW < D(120)) prevW = D(120);
    MoveWindow(s_hPreview, M, M, prevW, cy - M * 2, TRUE);   // 最后一个 TRUE = 移动后立即重画

    const int px = M * 2 + prevW;                 // 右侧面板的左边 x 坐标
    const int pw = cx - px - M;
    int py = M;                                   // 右侧当前"流水线"的 y 坐标

    const int hFmt = D(118);                      // 「目标格式」组高度
    MoveWindow(s_hGrpFormat, px, py, pw, hFmt, TRUE);
    // 下拉框的高度参数对 CBS_DROPDOWNLIST 来说指的是"展开后列表的高度"，不是收起时的高度
    MoveWindow(s_hCombo, px + D(16), py + D(26), pw - D(32), D(220), TRUE);
    MoveWindow(s_hLblFmtInfo, px + D(18), py + D(58), pw - D(36), D(52), TRUE);

    py += hFmt + D(8);
    const int hOpt = D(166);                      // 「编码选项」组高度
    MoveWindow(s_hGrpOpt, px, py, pw, hOpt, TRUE);
    MoveWindow(s_hTrkQuality, px + D(16), py + D(26), pw - D(32), D(30), TRUE);
    MoveWindow(s_hLblQuality, px + D(18), py + D(58), pw - D(36), D(20), TRUE);
    MoveWindow(s_hChkTrans, px + D(18), py + D(84), pw - D(36), D(24), TRUE);
    MoveWindow(s_hSwatch, px + D(18), py + D(112), D(44), D(26), TRUE);      // 颜色小方块
    MoveWindow(s_hBtnColor, px + D(72), py + D(112), pw - D(90), D(26), TRUE); // 选色按钮

    py += hOpt + D(8);
    MoveWindow(s_hLblEst, px, py, pw, D(40), TRUE);

    const int btnH = D(42);                       // 大按钮，贴底摆放
    MoveWindow(s_hBtn, px, cy - M - btnH, pw, btnH, TRUE);
}

// ---------------------------------------------------------------------------
// 「转换并输出」按钮的完整处理流程。
//
// 参数：hPage —— 本页窗口句柄，作为 MessageBox 的父窗口（这样对话框会居中在本页上）。
// 流程：①检查有没有载入源图；②检查有没有选格式；
//       ③若目标格式和源文件扩展名相同，问一下用户是否继续（有损格式重复编码会掉画质）；
//       ④算好"是否保留透明""画质"两个参数，把界面刷新一下让"正在转换"能显示出来；
//       ⑤BuildOutputBitmap() 造出真正要写的位图；
//       ⑥去掉扩展名前的点，交给 SaveBitmapAsFormat() 落盘。
//
// 注意：SaveBitmapAsFormat() 内部会 delete 传入的位图，所以这里不要重复 delete；
//       UpdateWindow(g_hMain) 是强制立刻重绘主窗口，否则"正在转换"要等这段代码
//       跑完才有机会显示（单线程消息循环里 setText 只是登记，得让它画一次）。
// ---------------------------------------------------------------------------
static void OnProcess(HWND hPage)
{
    if (!g_pSrcImage)
    {
        MessageBoxW(hPage, L"请先在上方点击「选择图片...」载入一张图片。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const EncoderChoice* e = CurEncoder();
    if (!e)
    {
        MessageBoxW(hPage, L"请先选择目标格式。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // 扩展名一样时提醒用户（例如 JPEG 存成 JPEG，等于再压一次，画质会更差）
    if (_wcsicmp(e->ext.c_str(), ExtOf(g_srcPath).c_str()) == 0)
    {
        wchar_t q[256];
        swprintf_s(q, L"目标格式与源文件相同（%s），仍要继续吗？", e->name.c_str());
        if (MessageBoxW(hPage, q, L"提示", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    // 这两个值必须和 DoEstimate() 里的算法完全一致，预估才可信
    const bool keepAlpha = EncoderKeepsAlpha(e->mime) && g_srcHasAlpha && KeepTransparent();
    const ULONG quality = (ULONG)(IsLossy(e->mime) ? Quality() : 100);

    SetAppStatus(L"正在转换，请稍候...");
    UpdateWindow(g_hMain);   // 立刻把状态栏画出来，不然用户以为程序卡住了

    Bitmap* work = BuildOutputBitmap(keepAlpha);
    if (!work)
    {
        MessageBoxW(hPage, L"创建目标位图失败（内存不足？）", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：内存不足。");
        return;
    }

    // 扩展名统一按"不带点"的形式传给 SaveBitmapAsFormat（它自己拼下划线后缀），
    // erase(0,1) = 从下标 0 开始删掉 1 个字符，也就是去掉开头的 '.'
    std::wstring extNoDot = e->ext;
    if (!extNoDot.empty() && extNoDot[0] == L'.') extNoDot.erase(0, 1);

    // 保存成功/失败的状态栏提示由 SaveBitmapAsFormat 内部负责
    SaveBitmapAsFormat(work, L"_" + extNoDot, e->mime, e->ext, quality, hPage);
}

// ---------------------------------------------------------------------------
// 本页的窗口过程（窗口消息处理函数）。系统每给这个窗口发一条消息就调它一次。
// 约定：处理了的消息 return 0（表示"我处理完了"）；没处理的必须交给
//       DefWindowProcW()，否则窗口的基本行为（绘制、标题、关闭等）会出问题。
//
// 关于 WM_COMMAND：子控件的事件都是用 WM_COMMAND 通知"父窗口"的。
//   低 16 位 LOWORD(wParam) = 控件 ID（IDC_CV_*）
//   高 16 位 HIWORD(wParam) = 通知码（按钮是 BN_CLICKED=0，下拉框是 CBN_SELCHANGE=1）
//   所以必须按 (id, code) 两个维度判断，光看 id 区分不出"点击"还是"选择变化"。
//   新手最容易踩的坑：想用 PostMessage 模拟一次下拉框选择变化，必须发给本页(父窗口)，
//   而不是发给 ComboBox 自己 —— 发给 ComboBox 它不会转告父窗口，什么也不会发生。
// ---------------------------------------------------------------------------
static LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    // 窗口刚创建完、还没显示时到达：适合在这里一次性建好所有子控件。
    // 注意 WM_CREATE 是"创建过程中"的消息，此时窗口尺寸还是初始的小尺寸，
    // 所以布局放在 Layout() 里、由 WM_SIZE 触发，而不是写死在这里。
    case WM_CREATE:
        {
            s_hPage = hWnd;   // 记下自己，供定时器 / ScheduleEstimate 使用

            // CreateCtl(类名, 文字, 样式, 父窗口, 控件ID) 是本工程的封装，
            // 内部还会统一设置字体。SS_OWNERDRAW = 由我们自己在 WM_DRAWITEM 里画。
            s_hPreview = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CV_PREVIEW);

            s_hGrpFormat = CreateCtl(L"Button", L"目标格式", WS_VISIBLE | BS_GROUPBOX,
                                     hWnd, IDC_CV_GRP_FORMAT);
            // CBS_DROPDOWNLIST = 只能选、不能手输的下拉列表
            s_hCombo = CreateCtl(L"ComboBox", L"",
                                 WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                 hWnd, IDC_CV_COMBO_FORMAT);
            s_hLblFmtInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                      hWnd, IDC_CV_LBL_FMTINFO);

            s_hGrpOpt = CreateCtl(L"Button", L"编码选项", WS_VISIBLE | BS_GROUPBOX, hWnd, IDC_CV_GRP_OPT);
            s_hTrkQuality = CreateCtl(L"msctls_trackbar32", L"",
                                      WS_VISIBLE | WS_TABSTOP | TBS_HORZ, hWnd, IDC_CV_TRK_QUALITY);
            // Trackbar 的取值范围/位置/刻度都要用消息设置：
            //   TBM_SETRANGE 参数是 MAKELPARAM(最小值, 最大值)，第 3 个参数 TRUE = 之后重绘；
            //   最小值取 10 而不是 0，避免用户调到 0 得到无法辨认的图；
            //   TBM_SETPOS 设当前位置（TRUE 同上），默认 90 是画质与体积的常用折中；
            //   TBM_SETTICFREQ 每 10 个单位画一个小刻度，纯视觉提示。
            SendMessageW(s_hTrkQuality, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
            SendMessageW(s_hTrkQuality, TBM_SETPOS, TRUE, 90);
            SendMessageW(s_hTrkQuality, TBM_SETTICFREQ, 10, 0);
            s_hLblQuality = CreateCtl(L"Static", L"", WS_VISIBLE, hWnd, IDC_CV_LBL_QUALITY);
            // BS_AUTOCHECKBOX = 系统自动处理勾选状态的复选框
            s_hChkTrans = CreateCtl(L"Button", L"保留透明背景",
                                    WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, hWnd, IDC_CV_CHK_TRANS);
            Button_SetCheck(s_hChkTrans, BST_CHECKED);   // 默认勾上"保留透明背景"
            s_hSwatch = CreateCtl(L"Static", L"", WS_VISIBLE | SS_OWNERDRAW, hWnd, IDC_CV_SWATCH);
            s_hBtnColor = CreateCtl(L"Button", L"选择填充颜色...", WS_VISIBLE | WS_TABSTOP,
                                    hWnd, IDC_CV_BTN_COLOR);

            s_hLblEst = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP, hWnd, IDC_CV_LBL_EST);
            // BS_DEFPUSHBUTTON = 默认按钮（回车键会触发它）
            s_hBtn = CreateCtl(L"Button", L"转换并输出", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               hWnd, IDC_CV_BTN_PROCESS);

            // 填编码器列表：从系统里查真实的编码器，而不是写死一张格式表，
            // 这样在缺少某个编码器的机器上也不会列出用不了的格式。
            s_encoders = ListImageEncoders();
            for (size_t i = 0; i < s_encoders.size(); ++i)
            {
                // 显示成 "JPEG  (*.jpg)" 这样，用户能同时看到名字和扩展名
                std::wstring label = s_encoders[i].name + L"  (*" + s_encoders[i].ext + L")";
                // CB_ADDSTRING 把一项加到列表末尾，返回值是它的索引；
                // lParam 传字符串指针即可，ComboBox 内部会复制一份，不用担心临时变量
                SendMessageW(s_hCombo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
            }
            // CB_SETCURSEL 按索引选中某一项（它不会触发 CBN_SELCHANGE 通知，这点很省事）
            if (!s_encoders.empty()) SendMessageW(s_hCombo, CB_SETCURSEL, 0, 0);

            UpdateEnable();   // 按初始选择刷新控件可用状态
            Layout();         // 首次布局（后续窗口尺寸变化时由 WM_SIZE 再调）
        }
        return 0;

    // 窗口大小改变：重新摆放所有控件。预览是自绘的，还得让它重画一次。
    case WM_SIZE:
        Layout();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        return 0;

    // 定时器到点。这里只处理自己那个 ID 的定时器；
    // 一次性定时器用完必须 KillTimer，否则 320ms 到了会反复触发。
    case WM_TIMER:
        if (wParam == TIMER_ESTIMATE)
        {
            KillTimer(hWnd, TIMER_ESTIMATE);
            DoEstimate();
        }
        return 0;

    // 滑块被拖动：先刷新画质文字（要跟手，立刻更新），再安排一次防抖的预估。
    // 注意这里不看 wParam 是谁——本页只有这一个横向滚动条控件。
    case WM_HSCROLL:
        UpdateQualityLabel();
        ScheduleEstimate();
        return 0;

    // 自定义消息：主窗口告知"源图已更换"。这个宏定义在 App.h 里，
    // 值是 WM_APP+2，属于应用自定义消息区间（WM_APP 以上系统保证不会占用）。
    case WM_APP_IMAGE_CHANGED:
        RebuildPreview();
        // 默认选中与源文件相同的格式
        {
            const std::wstring srcExt = ExtOf(g_srcPath);
            for (size_t i = 0; i < s_encoders.size(); ++i)
            {
                if (_wcsicmp(s_encoders[i].ext.c_str(), srcExt.c_str()) == 0)
                {
                    SendMessageW(s_hCombo, CB_SETCURSEL, (WPARAM)i, 0);
                    break;   // 找到就停，避免同扩展名出现多次时来回跳动
                }
            }
        }
        UpdateEnable();
        if (s_hPreview) InvalidateRect(s_hPreview, nullptr, TRUE);
        DoEstimate();   // 换了图要马上给个预估，不必等防抖定时器
        return 0;

    // 自绘消息：凡是带 SS_OWNERDRAW / BS_OWNERDRAW 的控件，系统都会发这个，
    // lParam 指向 DRAWITEMSTRUCT（含 hDC、rcItem 绘制区域、控件 ID）。
    // wParam 就是控件 ID，用它区分是画预览图还是画颜色小方块。
    case WM_DRAWITEM:
        if (lParam)
        {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (wParam == IDC_CV_PREVIEW)
            {
                // PaintImagePreview 会自己铺棋盘格背景：s_pPreview 为空时显示 hint 提示文字
                PaintImagePreview(dis->hDC, dis->rcItem, s_pPreview,
                                  L"点击上方「选择图片...」载入图片\r\n载入后即可转换格式");
                return TRUE;   // 在 WM_DRAWITEM 里，return TRUE 表示已处理（与一般约定不同）
            }
            if (wParam == IDC_CV_SWATCH)
            {
                // 小方块要不要画成"透明"样式，判断条件必须和 UpdateEnable 里保持一致
                const EncoderChoice* e = CurEncoder();
                const bool canAlpha = e && EncoderKeepsAlpha(e->mime) && g_srcHasAlpha;
                PaintColorSwatch(dis->hDC, dis->rcItem, g_fillColor, canAlpha && KeepTransparent());
                return TRUE;
            }
        }
        break;   // 其它控件的自绘交回系统默认处理

    // 控件事件通知（按钮点击、复选框切换、下拉框选择变化、编辑框输入 ...）
    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);      // 控件 ID
            const int code = HIWORD(wParam);    // 通知码：按钮点击是 BN_CLICKED(0)

            if (id == IDC_CV_BTN_PROCESS) { OnProcess(hWnd); return 0; }
            // 下拉框每次"换了一项"才发 CBN_SELCHANGE，此时 ID 对但 code 不对的事件要忽略
            if (id == IDC_CV_COMBO_FORMAT && code == CBN_SELCHANGE)
            {
                UpdateEnable();       // 换格式可能改变滑块/透明复选框/底色的可用性
                ScheduleEstimate();   // 不同格式压缩率差很多，体积要重算
                return 0;
            }
            if (id == IDC_CV_CHK_TRANS)
            {
                // 复选框的状态由 BS_AUTOCHECKBOX 自己维护，我们只要刷新其它控件的状态
                UpdateEnable();
                return 0;
            }
            if (id == IDC_CV_BTN_COLOR)
            {
                // 通用颜色对话框；用户取消时返回 CLR_INVALID
                const COLORREF c = ChooseFillColor(hWnd, g_fillColor);
                if (c != CLR_INVALID && c != g_fillColor)
                {
                    g_fillColor = c;
                    InvalidateRect(s_hSwatch, nullptr, TRUE);   // 让色块显示新颜色
                }
                return 0;
            }
        }
        break;

    default:
        break;
    }
    // 未处理的消息交给系统默认处理函数，这是所有自写窗口过程必须做的收尾
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 创建本功能页（供 Project001.cpp 调用）。
//
// 参数：parent —— 主窗口，本页会作为它的子窗口创建。
// 返回：新建的页面窗口句柄；注册窗口类失败时返回 nullptr。
//
// 窗口类只需注册一次，所以用函数内的 static 变量做标记（函数内 static 只初始化一次
// 且全程保留）。重复注册会返回 0 并让 GetLastError() 变成 ERROR_CLASS_ALREADY_EXISTS，
// 这里把这种情况当成"已经注册好了"照常继续，属于常见容错写法。
// 创建时用 WS_CHILD（子窗口）、WS_CLIPCHILDREN（画自己时跳过子控件区域，减少闪烁），
// 初始尺寸 10x10，真正的摆放由 WM_SIZE -> Layout() 完成。
// ---------------------------------------------------------------------------
HWND PageConvert_Create(HWND parent)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};                       // = {} 把整个结构体清零，别漏掉否则行为随机
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;        // 宽/高变化时整个重画，避免残影
        wc.lpfnWndProc = Proc;                     // 本页的消息处理函数
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);   // nullptr 表示用系统预定义光标
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);  // COLOR_WINDOW+1 是系统窗口背景色的约定写法
        wc.lpszClassName = L"Pic246PageConvert";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }

    // 注意这里没有 WS_VISIBLE：页面先建好但不显示，由主窗口在切换页时显示/隐藏
    return CreateWindowExW(0, L"Pic246PageConvert", L"", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 10, 10, parent, nullptr, g_hInst, nullptr);
}
