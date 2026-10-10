// ============================================================================
//  Project001.cpp —— Pic246 主窗口
//    · 左侧功能导航（侧边栏）
//    · 顶部：选择图片 / 路径 / 原图信息
//    · 底部：状态栏
//    · 中间：4 个功能页（图片尺寸缩放 / 图片裁切 / 图片调整 / ICO 转换）
// ============================================================================
// ============================================================================
//  【文件说明】（写给第一次接触 Win32 的读者）
// ----------------------------------------------------------------------------
//  1) 这个文件是什么
//     本文件是 Pic246 整个程序的“骨架/入口”：它是唯一带 wWinMain 的文件，
//     负责创建主窗口、主窗口的菜单/工具栏/状态栏、左侧自绘导航栏，
//     并把 7 个功能页（Page*.cpp 里实现的子窗口）嵌进主窗口里来回切换。
//
//     注意：文件头上面那段原有的注释说“中间：4 个功能页”，那是早期版本留下的；
//     现在实际已经扩展到 7 个页面（见下面的 PAGE_COUNT 和 kPageNames）。
//     这里属于“原有注释可以补充”，所以没有删掉它，只是在本说明里更正一下。
//
//  2) 它和哪些文件配合
//     · App.h
//         公共声明。本文件里用到的所有“全局变量”（g_hInst / g_hMain / g_dpi /
//         g_hUiFont / g_srcPath / g_pSrcImage ...）都【定义】在本文件，
//         在 App.h 里用 extern 声明，供其它 .cpp 使用。
//         同时 App.h 提供：DIP 换算宏 D()、自定义消息 WM_APP_*、控件 ID 枚举、
//         以及各功能页的创建函数（PageXxx_Create）。
//     · PageResize.cpp / PageCrop.cpp / PageAdjust.cpp / PageCompress.cpp /
//       PageConvert.cpp / PageIco.cpp / PageDraw.cpp
//         7 个功能页，每个都提供一个 PageXxx_Create(HWND parent)：
//         创建自己的子窗口并返回句柄（创建时是隐藏的，由主窗口决定何时显示）。
//         主窗口通过向它们发 WM_APP_IMAGE_CHANGED 通知“源图换了”，
//         通过向它们转发 WM_COMMAND(BN_CLICKED) 来实现菜单里的“处理并输出”。
//     · ImageOps.cpp
//         图片的载入/释放/保存/各种处理，以及 FileNameOf/FileExists 等小工具。
//         本文件直接调用其中的 LoadImageFile()、ClearImage()、FileNameOf()、
//         FileExists()。
//     · CropCanvas.cpp / DrawCanvas.cpp
//         两个自绘交互画布，被裁切页/ICO 页/涂鸦页内嵌使用，本文件不直接碰。
//     · Project001.rc / resource.h / Project001.h
//         资源脚本与 ID 定义：菜单资源 IDC_PROJECT001、两个对话框模板
//         IDD_ABOUTBOX / IDD_AUTHORBOX 以及图标都来自资源文件。
//
//  3) 启动流程（从上到下的调用顺序）
//     wWinMain
//       → SetProcessDPIAware() + 读取屏幕 DPI 存入 g_dpi（后面 D() 换算都用它）
//       → InitCommonControlsEx()（初始化公共控件：按钮/Static 等的视觉样式）
//       → GdiplusStartup()（启动 GDI+，本项目所有图片解码/绘制都靠它）
//       → CreateUiFont()（按系统设置造一个界面字体）
//       → MyRegisterClass()（注册两个窗口类：侧边栏类 + 主窗口类）
//       → InitInstance()（CreateWindowExW 创建主窗口 → WM_CREATE 里建好所有子控件）
//       → 载入快捷键表 + 命令行参数打开图片
//       → 进入消息循环 GetMessage/Translate/Dispatch，直到收到 WM_QUIT
//       → GdiplusShutdown() 收尾
//
//  4) 阅读提示
//     这是“事件驱动”程序：没有 main 里那种“一步步往下执行”的业务流程，
//     真正的逻辑都挂在 WndProc（窗口过程）的各个 case 分支上，由系统在
//     合适的时机来“回调”。所以读代码时重点看：谁在什么时候发消息，
//     哪个窗口过程收到后做了什么。
// ============================================================================
#include "App.h"   // 公共声明：全局变量 extern、工具函数、自定义消息、控件 ID、DIP 换算

using namespace Gdiplus;   // 本文件大量使用 Gdiplus:: 命名空间下的类型，简写

#define MAX_LOADSTRING 100   // 传统 Win32 模板留下的字符串缓冲长度常量（本文件未真正使用）

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
// 这些变量【在这里定义】（App.h 里是 extern 声明），所以整个程序只有一份实例。
// 因为全局变量对新手最容易“看不懂谁改了它”，这里逐个说明其生命周期：
HINSTANCE       g_hInst = nullptr;   // 本程序实例句柄：创建窗口/载入资源都要它
HWND            g_hMain = nullptr;   // 主窗口句柄：全局保存，方便别处（如命令行开图）直接用
int             g_dpi = 96;          // 屏幕 DPI（每英寸像素数），96=100%缩放；由 wWinMain 填入
HFONT           g_hUiFont = nullptr; // 全局界面字体：所有控件与自绘文字共用，程序退出时删除
std::wstring    g_srcPath;           // 当前打开的图片路径（未打开时为空串）
Gdiplus::Image* g_pSrcImage = nullptr; // 当前打开的原图（GDI+ 对象，需 delete；由 ClearImage 负责）
IStream*        g_pSrcStream = nullptr; // 原图所在的内存流，Image 需要它保持存活，否则图像数据会失效
UINT            g_srcW = 0;          // 原图宽度（像素）
UINT            g_srcH = 0;          // 原图高度（像素）
bool            g_srcHasAlpha = false; // 原图是否带透明通道（决定预览背景、输出格式提示等）
COLORREF        g_fillColor = RGB(255, 255, 255); // 裁切/ICO 页共用的填充色，默认白色

// ---------------------------------------------------------------------------
// 主窗口控件
// ---------------------------------------------------------------------------
// 全是子窗口句柄。注意：这些句柄在【窗口创建后】才有效，
// 在此之前（例如全局对象构造期）不能调用 MoveWindow/SetWindowText 等。
static HWND  g_hSidebar = nullptr;   // 左侧导航栏（自绘，窗口类见 SIDEBAR_CLASS）
static HWND  g_hBtnOpen = nullptr;   // 顶部“选择图片...”按钮
static HWND  g_hPath = nullptr;      // 顶部显示图片完整路径的 Static
static HWND  g_hInfo = nullptr;      // 顶部显示“原图尺寸：宽 × 高”的 Static
static HWND  g_hStatus = nullptr;    // 底部状态栏 Static

#define PAGE_COUNT 7                 // 功能页总数：改这里等于一次性改动菜单/侧边栏/切换逻辑
static HWND  g_pages[PAGE_COUNT] = {}; // 7 个功能页窗口句柄，下标即“页号”
static int   g_curPage = 0;          // 当前显示的页号（0 起）

// 页名数组：侧边栏自绘的文字和“功能”菜单项的文字都取自这里，改名字只需改这一处。
static const wchar_t* kPageNames[PAGE_COUNT] =
{
    L"图片尺寸缩放", L"图片裁切", L"图片调整", L"图片大小压缩",
    L"格式转换", L"ICO 转换", L"涂鸦"
};

// 每一页的“处理并输出”按钮 ID：菜单里的 Ctrl+S 要模拟点击当前页的按钮，
// 所以这里按页号登记各页的处理按钮 ID（ID 定义都在 App.h 的枚举里）。
static const int kPageProcessId[PAGE_COUNT] =
{
    IDC_RS_BTN_PROCESS, IDC_CP_BTN_PROCESS, IDC_AD_BTN_PROCESS, IDC_CM_BTN_PROCESS,
    IDC_CV_BTN_PROCESS, IDC_IC_BTN_PROCESS, IDC_DR_BTN_PROCESS
};

static HACCEL g_hAccelExtra = nullptr; // 运行时创建的快捷键表（Ctrl+O / Ctrl+S）

// ---------------------------------------------------------------------------
// 通用工具
// ---------------------------------------------------------------------------
// 【函数】ApplyUiFont —— 给某个子窗口设置统一界面字体
// 参数：h 目标窗口句柄（允许为 nullptr，此时什么都不做）
// 说明：WM_SETFONT 只对“控件类”窗口（Button/Static/Edit 等）有意义；
//       MAKELPARAM(TRUE, 0) 里的 TRUE 表示“立即重绘以应用新字体”。
//       如果 g_hUiFont 还没创建好（为 nullptr），就保持系统默认字体。
void ApplyUiFont(HWND h)
{
    if (h && g_hUiFont)
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hUiFont, MAKELPARAM(TRUE, 0));
}

// 【函数】CreateCtl —— 创建标准控件的“快捷方式”
// 参数：cls    窗口类名（L"Button" / L"Static" / L"Edit" ...）
//       text   控件文字
//       style  控件自身样式（如 WS_VISIBLE、BS_PUSHBUTTON），函数内会自动补 WS_CHILD
//       parent 父窗口
//       id     控件 ID（会作为 WM_COMMAND 的 LOWORD(wParam) 回传，用于区分是谁被点了）
//       exStyle 扩展样式，默认 0
// 返回：控件窗口句柄；创建失败返回 nullptr
// 说明：1) 位置尺寸先给 0，真正的摆放由 LayoutMain() 在 WM_SIZE/WM_CREATE 里做；
//       2) (HMENU)(INT_PTR)id 这个双重转换很关键：子窗口用 hMenu 参数存放整数 ID，
//          32 位下 INT_PTR 是 32 位、64 位下是 64 位，直接 (HMENU)id 会有警告/截断风险；
//       3) CreateWindowExW 失败只是返回 nullptr，不会弹错误框，必要时应自己检查。
HWND CreateCtl(const wchar_t* cls, const wchar_t* text, DWORD style, HWND parent, int id, DWORD exStyle)
{
    HWND h = CreateWindowExW(exStyle, cls, text, style | WS_CHILD,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    ApplyUiFont(h);   // 新控件立刻套用统一字体
    return h;
}

// 【函数】SetAppStatus —— 更新底部状态栏文字（声明在 App.h，供其它 .cpp 调用）
// 参数：text 要显示的文本
// 说明：UpdateWindow 会立刻把该控件的 WM_PAINT 处理完，
//       避免在紧接着做耗时操作（如读图/存图）时状态栏看起来“没反应”。
void SetAppStatus(const std::wstring& text)
{
    if (g_hStatus)
    {
        SetWindowTextW(g_hStatus, text.c_str());
        UpdateWindow(g_hStatus);
    }
}

// 【函数】CreateUiFont —— 按系统“消息框字体”设置创建界面字体
// 返回：字体句柄；即使系统调用失败也会退回 DEFAULT_GUI_FONT（不会返回 nullptr）
// 说明：SPI_GETNONCLIENTMETRICS 取回来的 lfMessageFont 就是系统 UI 字体，
//       这样中文界面在不同 DPI/主题下的字号才正常。
//       注意：GetStockObject(DEFAULT_GUI_FONT) 返回的是“系统拥有的”字体，
//       不需要（也不能）DeleteObject；两个分支混用时删除要小心。
static HFONT CreateUiFont()
{
    NONCLIENTMETRICSW ncm = {};        // 结构体清零很重要，否则 cbSize 之外可能有垃圾值
    ncm.cbSize = sizeof(ncm);          // 几乎所有 Win32 结构体都要先填 cbSize
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
        HFONT h = CreateFontIndirectW(&ncm.lfMessageFont);   // 用取到的 LOGFONT 造字体
        if (h) return h;               // 成功就直接用
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);   // 兜底：系统默认 GUI 字体
}

// 【函数】LoadAppIcon —— 按尺寸从资源里挑一个能用的图标
// 参数：hInstance 模块句柄；cx/cy 期望的图标宽高（如 SM_CXICON=大图标，SM_CXSMICON=小图标）
//       ids 候选资源 ID 数组；count 数组长度
// 返回：图标句柄（失败时退回系统默认图标 IDI_APPLICATION）
// 说明：1) ids[i]==0 表示该宏在编译时没找到对应资源（见 App.h 里 APP_ICON_MAIN_ID 的定义），跳过；
//       2) LR_SHARED 表示用系统的共享图标，不用我们 DestroyIcon，代价是不能改它；
//       3) LoadImage 成功会返回 HANDLE，图标场景下可直接强转 HICON。
static HICON LoadAppIcon(HINSTANCE hInstance, int cx, int cy, const int* ids, int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (ids[i] == 0)               // 该候选 ID 不存在，换下一个
            continue;
        HICON h = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(ids[i]), IMAGE_ICON, cx, cy, LR_SHARED);
        if (h)
            return h;
    }
    return LoadIconW(nullptr, IDI_APPLICATION);   // 全失败：用系统“应用程序”图标
}

// ---------------------------------------------------------------------------
// 图片载入
// ---------------------------------------------------------------------------
// 【函数】UpdateTopInfo —— 刷新顶部“原图尺寸”提示文字
// 无参数无返回；依赖全局 g_pSrcImage / g_srcW / g_srcH。
// 说明：swprintf_s 是“安全版”格式化（需传缓冲区，越界会触发断言而不是静默破坏内存）。
static void UpdateTopInfo()
{
    wchar_t buf[512];
    if (!g_pSrcImage)                  // 还没选图
    {
        wcscpy_s(buf, L"尚未选择图片。点击「选择图片...」，或把图片文件拖进窗口。");
    }
    else if (g_srcW == g_srcH)         // 正方形图单独给个提示，方便做 ICO
    {
        swprintf_s(buf, L"原图尺寸：%u × %u 像素（正方形）", g_srcW, g_srcH);
    }
    else
    {
        swprintf_s(buf, L"原图尺寸：%u × %u 像素", g_srcW, g_srcH);
    }
    SetWindowTextW(g_hInfo, buf);
}

// 【函数】OpenImagePath —— 真正“打开一张图片”的统一入口
// 参数：hWnd 出错时用来弹提示框的父窗口；path 图片完整路径
// 说明：这是拖放 / 菜单 / 命令行三条路径的汇合点：
//       1) 先把鼠标设成“等待”沙漏（读大图会卡一下，给用户反馈）；
//       2) LoadImageFile 是 ImageOps.cpp 里的函数，成功后会填好所有 g_src* 全局量；
//       3) 成功后必须【广播 WM_APP_IMAGE_CHANGED】给所有页面，否则页面里缓存的
//          预览图还是旧图（这是新手最容易漏的一步）；
//       4) SetCursor(hOld) 要恢复原来的光标，否则沙漏会一直留着。
static void OpenImagePath(HWND hWnd, const std::wstring& path)
{
    HCURSOR hOld = SetCursor(LoadCursorW(nullptr, IDC_WAIT));   // 沙漏光标

    std::wstring err;
    bool ok = LoadImageFile(path, err);   // 载入工作全在 ImageOps.cpp 里

    SetCursor(hOld);                      // 无论成败都恢复光标

    if (!ok)
    {
        MessageBoxW(hWnd, err.c_str(), L"打开失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"载入失败：" + FileNameOf(path));   // FileNameOf: 只取文件名，见 ImageOps.cpp
        return;
    }

    SetWindowTextW(g_hPath, path.c_str());   // 顶部显示完整路径
    UpdateTopInfo();                          // 顶部刷新尺寸信息

    // 通知所有功能页刷新
    for (int i = 0; i < PAGE_COUNT; ++i)
        if (g_pages[i])
            SendMessageW(g_pages[i], WM_APP_IMAGE_CHANGED, 0, 0);

    wchar_t buf[512];
    swprintf_s(buf, L"已载入 %s（%u × %u）。", FileNameOf(path).c_str(), g_srcW, g_srcH);
    SetAppStatus(buf);
}

// 【函数】OnOpenImage —— 弹出系统“打开文件”对话框并载入用户选中的图片
// 参数：hWnd 对话框的属主窗口（会被置灰，避免对话框被主窗口遮住）
// 说明（OPENFILENAMEW 是最容易踩坑的一组 API）：
//       1) lStructSize 必须正确填充，否则调用直接失败；
//       2) lpstrFilter 是“一对一对”的三段字符串：说明 -> 通配符，最后要用 \0\0 双结尾；
//          这就是为什么每个 L"..." 字面量里都写着 "\0"；
//       3) lpstrFile 指向的缓冲区由【调用者提供】，nMaxFile 是它的字符数；
//       4) 用户取消时 GetOpenFileNameW 返回 0（注意是 0 而不是 FALSE 的语义差别在文档里），
//          此时必须直接 return，不能拿缓冲区里的半截内容去开图；
//       5) buf 用 vector<wchar_t> 而不是数组，是为了在栈上不放 32K 的大对象。
static void OnOpenImage(HWND hWnd)
{
    std::vector<wchar_t> buf(32768, 0);   // 32K 字符的路径缓冲（长路径也够用）
    if (!g_srcPath.empty())               // 保留上次的目录/文件名作为默认值
        wcsncpy_s(buf.data(), buf.size(), g_srcPath.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};               // 结构体清零再逐项填，避免选错标志位
    ofn.lStructSize = sizeof(ofn);        // 必填
    ofn.hwndOwner = hWnd;                 // 属主窗口
    ofn.lpstrFilter =                     // 过滤器：说明 + 通配符，成对出现，双 \0 结束
        L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
        L"PNG 图片 (*.png)\0*.png\0"
        L"JPEG 图片 (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
        L"BMP 图片 (*.bmp)\0*.bmp\0"
        L"所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf.data();           // 输入输出缓冲，函数会把选中的路径写进来
    ofn.nMaxFile = (DWORD)buf.size();     // 必须和缓冲区实际大小一致
    ofn.lpstrTitle = L"选择一张图片";      // 对话框标题
    ofn.lpstrDefExt = L"png";             // 用户没输后缀时补 png
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    // OFN_FILEMUSTEXIST  文件必须存在（避免用户乱输入）
    // OFN_PATHMUSTEXIST  路径必须存在
    // OFN_EXPLORER       用新版资源管理器风格界面
    // OFN_HIDEREADONLY   隐藏“只读”复选框

    if (!GetOpenFileNameW(&ofn))          // 用户点了取消 → 什么都没选
        return;

    OpenImagePath(hWnd, std::wstring(buf.data()));   // 走统一的载入流程
}

// ---------------------------------------------------------------------------
// 侧边栏
// ---------------------------------------------------------------------------
#define SIDEBAR_CLASS L"Pic246Sidebar"   // 侧边栏的自定义窗口类名（RegisterClassExW 时使用）

// 侧边栏的“私有数据”：每个 HWND 各自一份，挂在窗口的 GWLP_USERDATA 上。
// 为什么不直接用全局变量？因为窗口类可以被多个窗口复用，
// 把状态挂在窗口上才是 Win32 里可扩展的正确写法（等价于 C++ 里的 this）。
struct SidebarState
{
    int  hover;      // 鼠标悬停的项下标，-1 表示没悬停在任何项上
    int  sel;        // 当前选中（高亮）的项下标，0..PAGE_COUNT-1
    bool tracking;   // 是否已经调用过 TrackMouseEvent 申请 WM_MOUSELEAVE
};

// 【函数】SidebarOf —— 从窗口句柄取回它自己的 SidebarState*
// 说明：GWLP_USERDATA 是一个“每个窗口一个指针大小”的空位，专门留给开发者放自己的数据。
//       必须用 GetWindowLongPtrW/SetWindowLongPtrW（带 Ptr），
//       不要用老式的 GetWindowLongW，否则 64 位下指针会被截断成 32 位而崩溃。
static SidebarState* SidebarOf(HWND h)
{
    return (SidebarState*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static int SidebarItemsTop() { return D(64); }   // 第一个菜单项距顶部的距离（DIP→像素，见 D()）
static int SidebarItemH() { return D(52); }      // 每个菜单项的高度

// 【函数】SidebarHit —— 把“鼠标 y 坐标”换算成“第几个菜单项”
// 参数：y 相对侧边栏客户区的 y 坐标（像素）
// 返回：项下标；不在任何项上返回 -1
// 说明：调用方用 GET_Y_LPARAM(lParam) 取 y，它会自动处理 lParam 里坐标的有符号扩展。
static int SidebarHit(int y)
{
    const int top = SidebarItemsTop();
    if (y < top) return -1;                     // 标题区域
    int idx = (y - top) / SidebarItemH();       // 整除：落在第几个 52 像素格子里
    if (idx < 0 || idx >= PAGE_COUNT) return -1; // 越界（下面空白区域）
    return idx;
}

// 【函数】PaintSidebar —— 侧边栏的“自绘”实现（双缓冲）
// 说明（自绘 + 双缓冲是本文件最重要的两个新手概念之一）：
//   1) 自绘窗口把背景刷（hbrBackground）设为 nullptr，并在 WM_PAINT 里
//      BeginPaint → 画 → EndPaint 全部自己完成；不这样做的后果是
//      系统先擦一遍背景导致闪烁。
//   2) 双缓冲：先在一块内存 DC(mem) 上把整幅画面画完，最后一次性 BitBlt 到屏幕 DC。
//      否则逐条画线/逐个圆角矩形会肉眼可见地闪。
//   3) GDI 对象（笔/刷/字体/位图）用完必须恢复 SelectObject 之前的老对象再删除，
//      否则会造成 GDI 句柄泄漏——这是长期运行的程序最常见的“越用越卡”原因。
static void PaintSidebar(HWND h)
{
    SidebarState* st = SidebarOf(h);        // 取私有状态（可能为 nullptr，下面都做了判空）
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);           // 只能配 WM_PAINT 使用

    RECT rc;
    GetClientRect(h, &rc);                  // 客户区宽高（不含边框标题栏）
    const int w = rc.right;
    const int hgt = rc.bottom;

    HDC mem = CreateCompatibleDC(hdc);          // 内存 DC：先画在这上面
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, hgt);   // 与屏幕兼容的位图作为画布
    HGDIOBJ oldBmp = SelectObject(mem, bmp);    // 绑定后 mem 才能画；记住旧对象待恢复

    // 背景
    HBRUSH bg = CreateSolidBrush(RGB(247, 248, 250));   // 极浅灰：侧边栏底色
    RECT all = { 0, 0, w, hgt };
    FillRect(mem, &all, bg);                    // FillRect 不需要选中画刷，直接传

    SetBkMode(mem, TRANSPARENT);                // 文字背景透明，否则会有白色方块
    HGDIOBJ oldFont = SelectObject(mem, g_hUiFont);   // 使用全局界面字体

    // 标题
    RECT tr = { D(16), D(16), w, D(44) };
    SetTextColor(mem, RGB(28, 32, 38));         // 近黑色
    DrawTextW(mem, APP_NAME, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // -1 表示按 \0 自动算长度；DT_VCENTER 必须配合 DT_SINGLELINE 才垂直居中

    // 分隔线
    HPEN sep = CreatePen(PS_SOLID, 1, RGB(228, 231, 236));
    HGDIOBJ oldPen = SelectObject(mem, sep);    // 画线用画笔
    MoveToEx(mem, 0, D(56), nullptr);           // 第二个参数传 nullptr：不关心旧位置
    LineTo(mem, w, D(56));
    SelectObject(mem, oldPen);                  // 先恢复老笔
    DeleteObject(sep);                          // 再删除新笔

    // 菜单项
    const int top = SidebarItemsTop();
    const int itemH = SidebarItemH();

    for (int i = 0; i < PAGE_COUNT; ++i)
    {
        RECT ir = { 0, top + i * itemH, w, top + (i + 1) * itemH };  // 第 i 项的整条矩形
        const bool sel = (st && st->sel == i);    // 是否选中
        const bool hov = (st && st->hover == i);  // 是否鼠标悬停

        if (sel || hov)                           // 选中或悬停时画圆角“胶囊”底
        {
            RECT hr = ir;                         // 在内缩一点，看起来像按钮而不顶到边
            hr.left += D(8);
            hr.right -= D(8);
            hr.top += D(4);
            hr.bottom -= D(4);
            HBRUSH hb = CreateSolidBrush(sel ? RGB(224, 237, 252) : RGB(238, 242, 247));
            // 选中=淡蓝，悬停=淡灰
            HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));  // NULL_PEN：不描边
            HGDIOBJ ob = SelectObject(mem, hb);
            RoundRect(mem, hr.left, hr.top, hr.right, hr.bottom, D(10), D(10));  // 圆角矩形
            SelectObject(mem, ob);                // 恢复老画刷/老笔
            SelectObject(mem, op);
            DeleteObject(hb);
        }

        if (sel)                                  // 选中项左侧再画一条蓝色竖条做强调
        {
            RECT acc = { D(8), top + i * itemH + D(4), D(12), top + (i + 1) * itemH - D(4) };
            HBRUSH ab = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(mem, &acc, ab);
            DeleteObject(ab);
        }

        RECT lr = { D(24), ir.top, w - D(12), ir.bottom };   // 文字绘制区域（左边留出竖条空间）
        SetTextColor(mem, sel ? RGB(0, 92, 180) : RGB(48, 52, 58));   // 选中=蓝色，普通=深灰
        DrawTextW(mem, kPageNames[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 右边框
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(226, 229, 234));
    oldPen = SelectObject(mem, edge);
    MoveToEx(mem, w - 1, 0, nullptr);            // 贴最右边画 1 像素竖线，和内容区分隔
    LineTo(mem, w - 1, hgt);
    SelectObject(mem, oldPen);
    DeleteObject(edge);

    SelectObject(mem, oldFont);                  // 恢复字体
    BitBlt(hdc, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);   // 一次性把内存画布拷到屏幕（不闪的关键）
    // SRCCOPY = 直接覆盖拷贝

    DeleteObject(bg);                            // 释放本次创建的 GDI 对象
    SelectObject(mem, oldBmp);                   // 位图必须先取消选中才能删
    DeleteObject(bmp);
    DeleteDC(mem);                               // 内存 DC 用 DeleteDC（不是 ReleaseDC）
    EndPaint(h, &ps);                            // 与 BeginPaint 配对
}

// 【函数】SidebarProc —— 侧边栏的窗口过程
// 参数：hWnd 收到消息的窗口；message 消息号；wParam/lParam 消息参数（含义随消息而变）
// 返回：LRESULT；不同消息对返回值有不同要求
// 说明：窗口过程是“回调函数”，由系统调用，不要自己去调用它。
//       凡是自己没有处理的消息，最后都必须交给 DefWindowProcW，
//       否则窗口会失去默认行为（如无法移动/关闭/重绘）。
static LRESULT CALLBACK SidebarProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    SidebarState* st = SidebarOf(hWnd);   // 先取出私有状态，后面每个分支都可能用到

    switch (message)
    {
    // WM_NCCREATE 是窗口创建过程中【最早】能拿到 hWnd 的消息（早于 WM_CREATE），
    // 所以在这里分配并挂载私有数据：这样随后的 WM_CREATE/WM_PAINT 就已经能取到 st 了。
    case WM_NCCREATE:
        {
            SidebarState* s = new SidebarState();
            s->hover = -1;                 // 初始没有悬停项
            s->sel = 0;                    // 初始默认选中第 0 项
            s->tracking = false;           // 还没申请鼠标离开通知
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)s);
        }
        return TRUE;   // 必须返回 TRUE 表示“非客户区创建成功”，返回 FALSE 则窗口创建失败
        // 注意：WM_CREATE 的约定正好相反——返回 0 表示成功，返回 -1 会中止窗口创建。
        // 两个消息的返回值约定不同，是最容易写错的地方之一。

    // WM_NCDESTROY 是窗口销毁过程中【最后】的消息，在这里释放私有数据最安全。
    case WM_NCDESTROY:
        if (st) { delete st; SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0); }   // 顺手清空指针防野指针
        return 0;

    case WM_PAINT:                 // 系统认为窗口需要重画时发来
        PaintSidebar(hWnd);        // 全部绘制逻辑在上面
        return 0;

    case WM_MOUSEMOVE:             // 鼠标在窗口内移动（会非常频繁）
        if (st)
        {
            if (!st->tracking)
            {
                // TrackMouseEvent 是“一次性”的：只申请一次离开通知，之后再进来要重新申请，
                // 所以用 tracking 标志记录状态，并在 WM_MOUSELEAVE 里复位。
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;       // 只关心“离开”
                tme.hwndTrack = hWnd;
                TrackMouseEvent(&tme);
                st->tracking = true;
            }
            int idx = SidebarHit(GET_Y_LPARAM(lParam));   // 宏：从 lParam 取 y（处理符号扩展）
            if (idx != st->hover)             // 只有悬停项变了才重绘，避免无谓刷新
            {
                st->hover = idx;
                InvalidateRect(hWnd, nullptr, FALSE);   // 只标记无效区，真正重绘在 WM_PAINT
                // 第三个参数 FALSE = 不擦背景（因为我们自己全画，擦了反而闪）
            }
            SetCursor(LoadCursorW(nullptr, idx >= 0 ? IDC_HAND : IDC_ARROW));  // 手上：可点击提示
        }
        return 0;

    case WM_MOUSELEAVE:            // 鼠标离开窗口（由上面的 TrackMouseEvent 换来）
        if (st)
        {
            st->tracking = false;      // 复位，下次进入时重新申请
            if (st->hover != -1)
            {
                st->hover = -1;        // 取消悬停高亮
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:           // 左键按下：命中某项就选中并通知主窗口切页
        if (st)
        {
            int idx = SidebarHit(GET_Y_LPARAM(lParam));
            if (idx >= 0)
            {
                st->sel = idx;                             // 自己先高亮
                InvalidateRect(hWnd, nullptr, FALSE);
                // 用 PostMessage（投递，立即返回）而不是 SendMessage（同步等待处理完），
                // 避免“主窗口在处理的中间又被回调进来”的重入问题。
                // WM_APP_PAGE_SELECT 是自定义消息（= WM_APP+1），wParam 携带页号。
                PostMessageW(GetParent(hWnd), WM_APP_PAGE_SELECT, (WPARAM)idx, 0);
            }
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);   // 未处理的消息交给系统默认处理
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
// 【函数】GetGeom —— 一次性算出布局需要的几个公共尺寸
// 参数：全部是【输出参数】（指针），不需要的可以传 nullptr
//       cx/cy 客户区宽高；sw 侧边栏宽；topH 顶部工具区高；stH 底部状态栏高
// 说明：D() 把“96dpi 下的逻辑像素(DIP)”换算成当前 DPI 下的实际像素（见 App.h）。
//       例如高 DPI 屏幕上 168 逻辑像素会变成 252 实际像素，界面才不会显得很小。
static void GetGeom(HWND hWnd, int* cx, int* cy, int* sw, int* topH, int* stH)
{
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (cx) *cx = rc.right;                     // 客户区宽度（left 恒为 0）
    if (cy) *cy = rc.bottom;                    // 客户区高度（top 恒为 0）
    if (sw) *sw = D(SIDEBAR_W_DIP);             // 侧边栏宽（常量在 App.h）
    if (topH) *topH = D(TOPBAR_H_DIP);          // 顶部工具区高
    if (stH) *stH = D(STATUSBAR_H_DIP);         // 底部状态栏高
}

// 【函数】LayoutMain —— 按当前窗口大小重新摆放所有子窗口
// 参数：hWnd 主窗口
// 调用时机：WM_CREATE（首次显示前）和每一次 WM_SIZE（用户拖动改变窗口大小）。
// 说明：这里是最典型的“手工布局”：MoveWindow 的最后一个参数 TRUE 表示
//       移动后立即重绘。所有坐标都是“相对父窗口客户区”的。
static void LayoutMain(HWND hWnd)
{
    int cx = 0, cy = 0, sw = 0, topH = 0, stH = 0;
    GetGeom(hWnd, &cx, &cy, &sw, &topH, &stH);

    MoveWindow(g_hSidebar, 0, 0, sw, cy, TRUE);        // 侧边栏从顶部一直到底，靠左贴边

    const int x = sw + D(14);                          // 右侧内容区左边界（留 14 像素内边距）
    const int y = D(14);                               // 顶行控件的 y
    MoveWindow(g_hBtnOpen, x, y, D(132), D(34), TRUE); // “选择图片...”按钮

    const int px = x + D(132) + D(10);                 // 路径框起点：按钮右侧留 10 像素
    int pathW = cx - D(14) - px;                       // 路径框宽度：占满按钮右侧的剩余空间
    if (pathW < D(60)) pathW = D(60);                  // 窗口太窄时的保底宽度（配合最小窗口限制）
    MoveWindow(g_hPath, px, y, pathW, D(34), TRUE);
    MoveWindow(g_hInfo, x, y + D(42), cx - x - D(14), D(22), TRUE);   // 第二行：原图尺寸信息

    int pageW = cx - sw;                               // 页面区域 = 客户区减去侧边栏
    int pageH = cy - topH - stH;                       // 再减去顶部与底部
    if (pageW < 10) pageW = 10;                        // 防止出现 0/负数尺寸（MoveWindow 会失败）
    if (pageH < 10) pageH = 10;

    for (int i = 0; i < PAGE_COUNT; ++i)
    {
        // 只摆放当前可见的那一页：隐藏的页面没有布局需求，省掉无谓的重绘。
        if (g_pages[i] && IsWindowVisible(g_pages[i]))
            MoveWindow(g_pages[i], sw, topH, pageW, pageH, TRUE);
    }

    MoveWindow(g_hStatus, sw, cy - stH, cx - sw, stH, TRUE);   // 状态栏：底部右侧那一条
}

// 前置声明：SwitchPage 里要用到 SyncSidebarSel，它的完整定义在下面。
static void SyncSidebarSel(int idx);

// 【函数】SwitchPage —— 显示第 idx 页、隐藏其余页
// 参数：idx 页号（越界直接忽略）
// 说明：本项目没有用“标签页控件”，而是【7 个页面窗口全部创建好，只显示一个】，
//       切换成本极低（SwitchPage 会立即移动并显示目标页）。
static void SwitchPage(int idx)
{
    if (idx < 0 || idx >= PAGE_COUNT)
        return;

    g_curPage = idx;                       // 记住当前页，供“处理并输出”菜单使用

    int cx = 0, cy = 0, sw = 0, topH = 0, stH = 0;
    GetGeom(g_hMain, &cx, &cy, &sw, &topH, &stH);   // 注意这里用的是全局主窗口句柄
    int pageW = cx - sw, pageH = cy - topH - stH;
    if (pageW < 10) pageW = 10;
    if (pageH < 10) pageH = 10;

    for (int i = 0; i < PAGE_COUNT; ++i)
    {
        if (!g_pages[i]) continue;         // 该页创建失败，跳过
        if (i == idx)
        {
            // 显示前先摆正位置：因为页面被隐藏期间 WM_SIZE 可能已经变了。
            MoveWindow(g_pages[i], sw, topH, pageW, pageH, TRUE);
            ShowWindow(g_pages[i], SW_SHOW);
        }
        else
        {
            ShowWindow(g_pages[i], SW_HIDE);   // 隐藏页仍存在，只是不可见
        }
    }

    SyncSidebarSel(idx);                          // 同步侧边栏高亮项（用菜单切页时它自己不知道）
}

// 【函数】SyncSidebarSel —— 把侧边栏的高亮项同步成当前页
// 参数：idx 当前页号
// 说明：侧边栏的“选中项”存在它自己的 SidebarState::sel 里，只有【用户点击侧边栏】
//       时它才会自己改。如果是从“功能”菜单（或将来任何代码路径）切页，
//       就必须由主窗口主动把 sel 改过来，否则会出现“内容换页了、左侧高亮还停在旧项上”。
static void SyncSidebarSel(int idx)
{
    SidebarState* st = g_hSidebar ? SidebarOf(g_hSidebar) : nullptr;
    if (!st || st->sel == idx)
        return;                                   // 没有侧边栏 / 本来就一致，不必重画
    st->sel = idx;                                // 改状态
    InvalidateRect(g_hSidebar, nullptr, FALSE);   // 立刻重画，让高亮跟着走
}

// ---------------------------------------------------------------------------
// 菜单
// ---------------------------------------------------------------------------
// 【函数】BuildMenu —— 在资源里那个“空壳菜单”上补齐各个菜单项
// 参数：hWnd 主窗口（用来取菜单、最后重画菜单栏）
// 说明：菜单资源 IDC_PROJECT001 里只定义了顶层项（文件/帮助等），
//       具体条目在运行时插入。这样可以：
//         · 用循环生成 7 个功能页菜单项，免得在 .rc 里写 7 遍；
//         · 让菜单文字和 kPageNames 保持单一来源。
//       MF_BYPOSITION 表示 InsertMenuW 的第二个参数是“位置序号”而不是命令 ID；
//       这正是把新项插到最前面（0、1、2）的原因。
static void BuildMenu(HWND hWnd)
{
    HMENU hMenu = GetMenu(hWnd);           // 取窗口菜单栏句柄
    if (!hMenu) return;                    // 没有菜单就什么也不做

    HMENU hFile = GetSubMenu(hMenu, 0);    // 第 0 个下拉菜单（"文件"）
    if (hFile)
    {
        // \t 在菜单字符串里表示制表符，用来把快捷键显示右对齐；快捷键真正生效靠下面的加速键表。
        InsertMenuW(hFile, 0, MF_BYPOSITION | MF_STRING, IDM_OPEN_IMAGE, L"打开图片(&O)...\tCtrl+O");
        InsertMenuW(hFile, 1, MF_BYPOSITION | MF_STRING, IDM_PROCESS_SAVE, L"处理并输出(&S)\tCtrl+S");
        InsertMenuW(hFile, 2, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);   // 分隔线：ID 必须为 0
    }

    HMENU hFunc = CreatePopupMenu();       // 动态创建一个弹出菜单承载 7 个功能页
    if (hFunc)
    {
        for (int i = 0; i < PAGE_COUNT; ++i)
        {
            UINT flags = MF_STRING | ((i == 0) ? MF_CHECKED : 0);   // 默认勾选第 0 页
            AppendMenuW(hFunc, flags, IDM_PAGE_BASE + i, kPageNames[i]);   // ID 连续，方便按 ID 反推页号
        }
        // MF_POPUP 表示这一项展开成子菜单，(UINT_PTR)hFunc 就是子菜单句柄。
        InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hFunc, L"功能(&P)");
    }

    HMENU hHelp = GetSubMenu(hMenu, 2);    // 第 2 个下拉菜单（"帮助"；中间第 1 个是刚插进去的"功能"）
    if (hHelp)
    {
        InsertMenuW(hHelp, 0, MF_BYPOSITION | MF_STRING, IDM_AUTHOR, L"关于作者(&A)...");
        InsertMenuW(hHelp, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    DrawMenuBar(hWnd);                     // 菜单结构改了，通知系统重画菜单栏
}

// 【函数】SyncPageMenu —— 让“功能”菜单里的勾选状态与当前页保持一致
// 参数：hWnd 主窗口；idx 当前页号
// 说明：CheckMenuItem 用 MF_BYCOMMAND 表示按“命令 ID”定位，而不是按位置。
static void SyncPageMenu(HWND hWnd, int idx)
{
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;
    HMENU hFunc = GetSubMenu(hMenu, 1);    // 1 号子菜单就是上面动态建的“功能”
    if (!hFunc) return;

    for (int i = 0; i < PAGE_COUNT; ++i)
        CheckMenuItem(hFunc, IDM_PAGE_BASE + i, MF_BYCOMMAND | ((i == idx) ? MF_CHECKED : MF_UNCHECKED));
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
// 两个对话框过程的前置声明：DialogBoxW 需要函数指针，而它们的定义在文件末尾，
// 所以必须先声明（否则编译器不认识这两个名字）。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK AuthorBox(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

// 【函数】WndProc —— 主窗口的窗口过程（本文件的核心）
// 参数：hWnd 主窗口；message 消息；wParam/lParam 参数（含义随消息变化）
// 返回：LRESULT（依消息而异）
// 阅读方法：把每个 case 当成一个“事件处理器”，只看自己关心的分支即可。
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    // WM_CREATE：窗口已经创建、但还没显示。这里适合创建子控件。
    // 注意返回值约定：返回 0 表示【成功继续】；返回 -1 则 CreateWindowExW 失败、窗口被销毁。
    case WM_CREATE:
        {
            g_hMain = hWnd;                    // 记到全局，供其它地方的函数使用

            // 侧边栏
            // 这里直接用 CreateWindowExW 而不是 CreateCtl，因为侧边栏是自定义窗口类，
            // 参数也不太一样。(HMENU) 位置放的是控件 ID，后面 LayoutMain 负责摆位置。
            g_hSidebar = CreateWindowExW(0, SIDEBAR_CLASS, L"",
                                         WS_CHILD | WS_VISIBLE,
                                         0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)IDC_SIDEBAR,
                                         g_hInst, nullptr);

            // 顶部
            g_hBtnOpen = CreateCtl(L"Button", L"选择图片...", WS_VISIBLE | WS_TABSTOP,
                                   hWnd, IDC_BTN_OPEN);
            // WS_TABSTOP 让这个控件可以被 Tab 键切换焦点（键盘可用性的基本要求）
            g_hPath = CreateCtl(L"Static", L"",
                                WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_CENTERIMAGE,
                                hWnd, IDC_STATIC_PATH);
            // SS_PATHELLIPSIS：路径太长时自动变成 "C:\...\xxx.png" 这种中间省略形式
            // SS_CENTERIMAGE：文字在控件高度里垂直居中
            g_hInfo = CreateCtl(L"Static", L"", WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                hWnd, IDC_STATIC_INFO);

            // 状态栏
            g_hStatus = CreateCtl(L"Static", L"就绪：请选择一张图片。",
                                  WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                  hWnd, IDC_STATIC_STATUS);

            // 功能页
            // 7 个页面在这里一次性全部创建（创建时是隐藏的），切换页面只是显示/隐藏。
            // 好处：状态在页面内部自然保留；坏处：启动稍慢、内存占用稍大。
            g_pages[0] = PageResize_Create(hWnd);     // 图片尺寸缩放
            g_pages[1] = PageCrop_Create(hWnd);       // 图片裁切
            g_pages[2] = PageAdjust_Create(hWnd);     // 图片调整
            g_pages[3] = PageCompress_Create(hWnd);   // 图片大小压缩
            g_pages[4] = PageConvert_Create(hWnd);    // 格式转换
            g_pages[5] = PageIco_Create(hWnd);        // ICO 转换
            g_pages[6] = PageDraw_Create(hWnd);       // 涂鸦

            BuildMenu(hWnd);              // 补齐菜单项

            UpdateTopInfo();              // 顶部提示“尚未选择图片...”
            LayoutMain(hWnd);             // 首次摆放所有控件
            SwitchPage(0);                // 默认显示第 0 页（侧边栏初始选中项也是 0）

            DragAcceptFiles(hWnd, TRUE);  // 允许把文件拖进窗口（消息 WM_DROPFILES）
        }
        return 0;

    // WM_SIZE：窗口大小改变。子控件不会自己跟着变，必须重算布局。
    case WM_SIZE:
        LayoutMain(hWnd);
        return 0;

    // WM_GETMINMAXINFO：系统在改变窗口大小/最大化前询问限制信息，这里设置“最小尺寸”。
    // 关键点：ptMinTrackSize 指的是【整个窗口（含边框标题栏）】的大小，
    // 所以要先算出“非客户区”多出来的那部分（ncx/ncy）再加到客户区尺寸上。
    case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;   // lParam 就是一个 MINMAXINFO*
            RECT wr = {}, cr = {};
            GetWindowRect(hWnd, &wr);                // 整个窗口的屏幕坐标
            GetClientRect(hWnd, &cr);                // 客户区尺寸（左上角为 0,0）
            int ncx = (wr.right - wr.left) - (cr.right - cr.left);   // 边框宽（左右合计）
            int ncy = (wr.bottom - wr.top) - (cr.bottom - cr.top);   // 标题栏+边框高
            mmi->ptMinTrackSize.x = D(900) + ncx;    // 客户区至少 900×640 逻辑像素
            mmi->ptMinTrackSize.y = D(640) + ncy;
        }
        return 0;

    // 自定义消息：侧边栏点中某项后 PostMessage 过来（wParam = 页号）
    case WM_APP_PAGE_SELECT:
        SwitchPage((int)wParam);
        SyncPageMenu(hWnd, (int)wParam);
        return 0;

    // WM_PAINT：主窗口自己只画一条“状态栏上方”的分隔线。
    // 其余看起来像背景的部分由窗口类的 hbrBackground(COLOR_WINDOW) 负责擦除。
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            int cx = 0, cy = 0, sw = 0, topH = 0, stH = 0;
            GetGeom(hWnd, &cx, &cy, &sw, &topH, &stH);

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(228, 231, 236));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            MoveToEx(hdc, sw, cy - stH, nullptr);   // 从侧边栏右边开始
            LineTo(hdc, cx, cy - stH);              // 一直画到右边界
            SelectObject(hdc, oldPen);              // 恢复老笔
            DeleteObject(pen);                      // 删除新笔

            EndPaint(hWnd, &ps);
        }
        return 0;

    // WM_DROPFILES：用户把文件拖进窗口（需要先 DragAcceptFiles(...,TRUE)）。
    // wParam 是 HDROP，必须用 DragFinish 释放，否则系统资源泄漏。
    case WM_DROPFILES:
        {
            HDROP hDrop = (HDROP)wParam;
            // 0xFFFFFFFF 是个特殊下标，含义是“请告诉我一共拖了几个文件”。
            UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            if (n > 0)
            {
                std::vector<wchar_t> file(32768, 0);   // 长路径缓冲
                // 0 表示只要第 1 个文件（本程序只支持一次打开一张）。
                if (DragQueryFileW(hDrop, 0, file.data(), (UINT)file.size()) > 0)
                    OpenImagePath(hWnd, std::wstring(file.data()));
            }
            DragFinish(hDrop);   // 无论成功与否都要调用
        }
        return 0;

    // WM_COMMAND：按钮被点击、菜单被选择时都会发到父窗口（本窗口）这里。
    // 编码方式（必记）：
    //   LOWORD(wParam) = 控件 ID 或菜单命令 ID
    //   HIWORD(wParam) = 通知码（按钮点击是 BN_CLICKED，值为 0）
    //   lParam         = 控件窗口句柄（菜单触发时为 0）
    // 这里本项目只按 ID 区分，没有再看通知码。
    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);   // 低 16 位才是 ID（高位是通知码）

            switch (id)
            {
            // 按钮点击与菜单“打开图片”走同一段逻辑，两种入口行为一致
            case IDC_BTN_OPEN:
            case IDM_OPEN_IMAGE:
                OnOpenImage(hWnd);
                return 0;

            // “处理并输出”：本程序并不自己处理，而是【代替用户点击当前页的“处理”按钮】。
            // 做法是构造一条 WM_COMMAND 用 SendMessage 发给当前页窗口：
            //   MAKEWPARAM(按钮ID, BN_CLICKED) —— 让页面以为按钮真的被点了。
            // 这是一种很常用的“跨窗口委托”技巧，好处是页面逻辑完全不用改。
            case IDM_PROCESS_SAVE:
                if (g_pages[g_curPage])
                    SendMessageW(g_pages[g_curPage], WM_COMMAND,
                                 MAKEWPARAM(kPageProcessId[g_curPage], BN_CLICKED), 0);
                return 0;

            // 模态对话框：DialogBoxW 会一直阻塞到对话框关闭才返回。
            // MAKEINTRESOURCEW(IDD_ABOUTBOX) 把整数 ID 伪装成字符串指针来传资源 ID。
            case IDM_ABOUT:
                DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_ABOUTBOX), hWnd, About);
                return 0;

            case IDM_AUTHOR:
                DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_AUTHORBOX), hWnd, AuthorBox);
                return 0;

            case IDM_EXIT:            // 菜单“退出”：销毁主窗口即可（会连带触发 WM_DESTROY）
                DestroyWindow(hWnd);
                return 0;

            default:
                // 7 个功能页菜单项的 ID 是连续的：IDM_PAGE_BASE + 页号。
                // 用区间判断可以一条分支处理全部 7 项，页号就是 id - IDM_PAGE_BASE。
                if (id >= IDM_PAGE_BASE && id < IDM_PAGE_BASE + PAGE_COUNT)
                {
                    SwitchPage(id - IDM_PAGE_BASE);
                    SyncPageMenu(hWnd, id - IDM_PAGE_BASE);
                    return 0;
                }
                break;
            }
        }
        return 0;

    // WM_CLOSE：用户点右上角 × 或按 Alt+F4。默认行为就是销毁窗口，
    // 这里显式写出来，方便以后在这个位置加“是否保存”的确认。
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    // WM_DESTROY：窗口正在被销毁（此时子窗口已经被销毁）。
    // 这里是“清理资源 + 结束消息循环”的标准位置。
    case WM_DESTROY:
        DragAcceptFiles(hWnd, FALSE);   // 注销拖放，与 WM_CREATE 里的 TRUE 对应
        ClearImage();                   // 释放 GDI+ 图像与内存流（ImageOps.cpp）
        if (g_hAccelExtra) { DestroyAcceleratorTable(g_hAccelExtra); g_hAccelExtra = nullptr; }
        if (g_hUiFont) { DeleteObject(g_hUiFont); g_hUiFont = nullptr; }
        // 注意：g_hUiFont 必须先删除才能真正释放，且此后不能再被控件引用。
        PostQuitMessage(0);             // 投递 WM_QUIT（退出码 0），GetMessage 会因此返回 0 结束循环
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);   // 其余都交给系统默认处理
}

// ---------------------------------------------------------------------------
// 注册窗口类 / 初始化
// ---------------------------------------------------------------------------
// 【函数】MyRegisterClass —— 注册本程序用到的两个窗口类
// 参数：hInstance 程序实例句柄
// 返回：主窗口类的 ATOM（注册结果，0 表示失败）
// 说明：Win32 的“窗口类”不是 C++ 的 class，而是“窗口的模板”：
//       指定用什么窗口过程、默认光标、背景刷、图标等。
//       同一个类名注册一次，之后就可以创建任意多个同类窗口。
//       必须先注册、后创建，否则 CreateWindowExW 会失败（返回 nullptr）。
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    // 侧边栏
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);                       // 必填
        wc.style = CS_HREDRAW | CS_VREDRAW;           // 宽/高变化时整体重绘
        wc.lpfnWndProc = SidebarProc;                 // 关键：指定窗口过程
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); // 默认箭头（悬停时会自己改成小手）
        wc.hbrBackground = nullptr;                   // 自绘窗口：不设背景刷，避免系统先擦一遍导致闪烁
        wc.lpszClassName = SIDEBAR_CLASS;             // 类名，创建窗口时要用同一个字符串
        RegisterClassExW(&wc);                        // 这里没检查返回值：类名重复注册只会失败，不影响运行
    }

    const int bigIds[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };   // 大图标候选
    const int smlIds[2] = { APP_ICON_SMALL_ID, APP_ICON_MAIN_ID };   // 小图标候选（顺序反过来，优先小图）

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;                       // 主窗口过程
    wcex.hInstance = hInstance;
    // GetSystemMetrics(SM_CXICON/SM_CYICON) 取系统定义的“大图标”标准尺寸（通常 32×32）
    wcex.hIcon = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), bigIds, 2);
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 背景刷：COLOR_WINDOW+1 是“系统颜色索引”的写法（+1 的原因见 Win32 文档），
    // 效果是窗口被系统自动擦成标准窗口底色。
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    // 菜单不在代码里创建，而是直接挂资源里的菜单模板，这样标题栏/菜单栏由系统管理。
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_PROJECT001);
    wcex.lpszClassName = APP_MAIN_CLASS;
    // hIconSm 是“小图标”（标题栏左上角、任务栏），通常 16×16
    wcex.hIconSm = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), smlIds, 2);
    return RegisterClassExW(&wcex);
}

// 【函数】InitInstance —— 创建并显示主窗口
// 参数：hInstance 实例句柄；nCmdShow 显示方式（SW_SHOW / SW_MAXIMIZE ...，由系统传入）
// 返回：TRUE 成功，FALSE 失败（wWinMain 会据此退出）
// 说明：CreateWindowExW 的第 10 个参数若传 nullptr 菜单，则用窗口类里注册的 lpszMenuName。
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    g_hInst = hInstance;                          // 保存到全局：CreateCtl 等函数需要它

    // 先按“客户区 1060×720 逻辑像素”算，再用 AdjustWindowRectEx 把边框/标题栏/菜单
    // 的厚度补进去，得到整个窗口的尺寸。
    // 最后一个参数 TRUE 表示“这个窗口有菜单”，因为菜单栏也要占高度。
    RECT rc = { 0, 0, D(1060), D(720) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0);

    // WS_OVERLAPPEDWINDOW = 标题栏 + 可缩放边框 + 最小化/最大化按钮 + 系统菜单，最常用的主窗口样式。
    // 传 nullptr 作为菜单句柄，于是自动使用窗口类里注册的那个资源菜单。
    HWND hWnd = CreateWindowExW(0, APP_MAIN_CLASS, APP_WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,      // 位置交给系统安排
                                rc.right - rc.left, rc.bottom - rc.top,   // 宽高（算好的整窗尺寸）
                                nullptr, nullptr, hInstance, nullptr);
    if (!hWnd)
        return FALSE;                             // 创建失败：wWinMain 会清理并返回

    ShowWindow(hWnd, nCmdShow);                   // 按系统给的方式显示（此时才真正出现在屏幕上）
    UpdateWindow(hWnd);                           // 立即产生一次 WM_PAINT，避免首帧空白
    return TRUE;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
// 【函数】wWinMain —— 程序入口（Unicode 版，对应控制台的 main）
// 参数：hInstance 本程序实例句柄；hPrevInstance 16 位时代遗留，恒为 nullptr；
//       lpCmdLine 命令行（不含程序名，本项目改用 CommandLineToArgvW 自己解析）；
//       nCmdShow 窗口初始显示方式
// 返回：退出码（取自消息循环里收到的 msg.wParam）
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    // UNREFERENCED_PARAMETER 只是告诉编译器“我知道这两个参数没用”，避免 C4100 警告。
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 让本进程“DPI 感知”：不做这一步，高 DPI 屏幕上 Windows 会把窗口放大后位图拉伸，
    // 界面会变得模糊。这是本项目 D() 换算能正常工作的前提。
    // 更现代的做法是用 SetProcessDpiAwarenessContext，这里保持简单。
    SetProcessDPIAware();
    {
        HDC hdc = GetDC(nullptr);                     // 取“整个屏幕”的 DC
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);       // 水平方向的每英寸像素数（96 的整数倍最常见）
        ReleaseDC(nullptr, hdc);                      // 屏幕 DC 必须 ReleaseDC（不是 DeleteDC）
        if (g_dpi < 96) g_dpi = 96;                   // 兜底：防止某些环境返回 0 导致所有尺寸算成 0
    }

    // 初始化公共控件库：按钮/编辑框等的“视觉样式”（Win10 圆角按钮效果）依赖它。
    // ICC_STANDARD_CLASSES 标准控件；ICC_BAR_CLASSES 状态栏/工具栏/进度条等。
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // 启动 GDI+：所有 Gdiplus::Image/Bitmap 操作都需要先启动它。
    // token 是“凭据”，退出时要用同一个 token 调 GdiplusShutdown。
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gdiplusStartupInput, nullptr) != Ok)
    {
        MessageBoxW(nullptr, L"GDI+ 初始化失败，程序无法启动。", L"错误", MB_OK | MB_ICONERROR);
        return FALSE;                                 // 注意：返回 FALSE 会被当成退出码 0
    }

    g_hUiFont = CreateUiFont();                       // 创建全局界面字体

    MyRegisterClass(hInstance);                       // 注册窗口类
    if (!InitInstance(hInstance, nCmdShow))           // 创建主窗口（失败时要把 GDI+ 关掉再退出）
    {
        GdiplusShutdown(token);                       // 清理第一步启动的资源
        return FALSE;
    }

    // 载入资源里的加速键表（快捷键必须配套一张表才能生效，光在菜单文字里写 \tCtrl+O 没用）。
    HACCEL hAccelTable = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDC_PROJECT001));

    // 运行时再补充两个快捷键：Ctrl+O 打开图片、Ctrl+S 处理并输出。
    // FCONTROL 表示必须按住 Ctrl；FVIRTKEY 表示 key 是“虚拟键码”而不是字符。
    ACCEL extra[2] = {};
    extra[0].fVirt = FCONTROL | FVIRTKEY; extra[0].key = 'O'; extra[0].cmd = IDM_OPEN_IMAGE;
    extra[1].fVirt = FCONTROL | FVIRTKEY; extra[1].key = 'S'; extra[1].cmd = IDM_PROCESS_SAVE;
    g_hAccelExtra = CreateAcceleratorTableW(extra, 2);
    // 按下快捷键时，系统会替我们发出 cmd 对应的 WM_COMMAND，逻辑仍走 WndProc。

    // 命令行传入图片时直接打开（也支持把图片拖到 exe 图标上）
    {
        int argc = 0;
        // CommandLineToArgvW 把命令行拆成参数数组（argv[0] 是 exe 路径）。
        // 用完后必须 LocalFree 释放，否则内存泄漏。
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            for (int i = 1; i < argc; ++i)            // 从 1 开始：跳过 exe 自身路径
            {
                if (FileExists(argv[i]))              // 只认确实存在的文件，避免把开关参数当图片
                {
                    OpenImagePath(g_hMain, argv[i]);
                    break;                            // 只打开第一个可用文件
                }
            }
            LocalFree(argv);
        }
    }

    // 消息循环：Windows 程序的“心脏”。
    // 每一轮取一条消息（GetMessage 在没有消息时会阻塞等待，不占 CPU），
    // 处理完再取下一轮，直到收到 WM_QUIT（此时 GetMessage 返回 0，循环结束）。
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        // TranslateAcceleratorW 会检查当前消息是不是加速键表中的快捷键；
        // 是的话它直接把它翻译成 WM_COMMAND 发给窗口并返回 TRUE，此时 continue 跳过下面的派发。
        if (hAccelTable && TranslateAcceleratorW(msg.hwnd, hAccelTable, &msg))
            continue;
        if (g_hAccelExtra && TranslateAcceleratorW(msg.hwnd, g_hAccelExtra, &msg))
            continue;
        // TranslateMessage 负责把“按键按下/抬起”合成 WM_CHAR 文本消息（普通窗口也需要）。
        TranslateMessage(&msg);
        // DispatchMessageW 把消息送到 msg.hwnd 对应的窗口过程（就是上面的 WndProc/SidebarProc）。
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(token);        // 消息循环结束，关闭 GDI+（与 GdiplusStartup 配对）
    return (int)msg.wParam;        // WM_QUIT 消息里的 wParam 就是退出码（通常来自 PostQuitMessage(0)）
}

// ---------------------------------------------------------------------------
// “关于”对话框
// ---------------------------------------------------------------------------
// 【函数】About —— “关于”对话框的对话框过程
// 参数：hDlg 对话框窗口；message/wParam/lParam 与窗口过程相同
// 返回：Unlike 普通窗口过程，对话框过程返回 BOOL：
//       TRUE  = 这个消息我处理了，别再走默认处理；
//       FALSE = 我没处理，请对话框管理器做默认事情（如 Tab 切换焦点、Esc 关闭）。
//       所以这里没有 DefWindowProc 调用，返回 FALSE 即可。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    // WM_INITDIALOG：对话框刚创建、还没显示，适合在这里做初始化。
    // 返回值 TRUE 表示“请把键盘焦点设置到第一个可Tab到的控件”。
    case WM_INITDIALOG:
        {
            const int ids[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };
            HICON hBig = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), ids, 2);
            HICON hSm = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ids, 2);

            // 给对话框设置大小图标（标题栏左上角、任务栏显示用）
            SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hBig);
            SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSm);

            // 对话框模板里通常用“图标控件”(SS_ICON 的 Static) 显示图标，
            // 但 .rc 里没法指定“用哪个图标”，所以运行时枚举子窗口找到它再塞图标。
            HWND hChild = nullptr;
            // FindWindowExW 依次枚举 hDlg 下所有类名为 Static 的直接子窗口（返回值作为下一次的起点）。
            while ((hChild = FindWindowExW(hDlg, hChild, L"Static", nullptr)) != nullptr)
            {
                // 用 GetWindowLongW 取样式，再和 SS_TYPEMASK 做“与”运算得到纯类型位，判断是不是图标型 Static
                if ((GetWindowLongW(hChild, GWL_STYLE) & SS_TYPEMASK) == SS_ICON)
                {
                    // STM_SETICON 是 Static 控件的专用消息：设置它显示的图标
                    SendMessageW(hChild, STM_SETICON, (WPARAM)hBig, 0);
                    break;   // 找到一个就够了
                }
            }
        }
        return (INT_PTR)TRUE;

    // WM_COMMAND：对话框里的按钮/菜单也会发到对话框过程。
    // IDOK = 确定按钮，IDCANCEL = 取消按钮（El Esc 键也会自动产生 IDCANCEL）。
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            // EndDialog 关闭模态对话框并让 DialogBoxW 返回；第二个参数是“返回值”。
            // 这里把触发关闭的按钮 ID 回传出去。
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;   // 未处理的消息交回对话框管理器
}

// ---------------------------------------------------------------------------
// “关于作者”对话框
// ---------------------------------------------------------------------------
#define IDC_BTN_COPYQQ 1201   // “复制 QQ 号”按钮的 ID：对话框内临时控件，就在本文件里定义

// 【函数】SetClipText —— 把一段文本放进系统剪贴板
// 参数：owner 剪贴板属主窗口（可为 nullptr）；text 要复制的以 \0 结尾的宽字符串
// 说明（剪贴板 API 的固定套路，顺序不能错）：
//   1) OpenClipboard 必须先成功，之后才能操作剪贴板；
//   2) 每个 OpenClipboard 都必须配一个 CloseClipboard（即使中途失败也要关）；
//   3) 交给 SetClipboardData 的内存块必须是用 GMEM_MOVEABLE 分配的，
//      而且【不能自己释放】——所有权已经交给了系统（所以下面成功路径没有 GlobalFree）。
static void SetClipText(HWND owner, const wchar_t* text)
{
    if (!OpenClipboard(owner)) return;    // 别的程序占着剪贴板时可能失败，直接放弃
    EmptyClipboard();                     // 清空后本程序才成为剪贴板的“所有者”
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);   // +1 是结尾的 \0
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem)
    {
        void* p = GlobalLock(hMem);       // 可移动内存必须先锁定才能拿到指针
        if (p)
        {
            memcpy(p, text, bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);   // CF_UNICODETEXT = UTF-16 文本格式
        }
        else
        {
            GlobalFree(hMem);             // 锁定失败：这块内存还归我们，必须自己释放
        }
    }
    CloseClipboard();                     // 与 OpenClipboard 配对
}

// 【函数】AuthorBox —— “关于作者”对话框的对话框过程
// 说明：结构与 About 几乎相同，区别是在 WM_INITDIALOG 里【动态创建】了一个按钮。
INT_PTR CALLBACK AuthorBox(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        {
            const int ids[2] = { APP_ICON_MAIN_ID, APP_ICON_SMALL_ID };
            HICON hBig = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), ids, 2);
            HICON hSm = LoadAppIcon(g_hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ids, 2);

            SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hBig);
            SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSm);

            HWND hChild = nullptr;
            while ((hChild = FindWindowExW(hDlg, hChild, L"Static", nullptr)) != nullptr)
            {
                if ((GetWindowLongW(hChild, GWL_STYLE) & SS_TYPEMASK) == SS_ICON)
                {
                    SendMessageW(hChild, STM_SETICON, (WPARAM)hBig, 0);
                    break;
                }
            }

            // 追加一个“复制 QQ 号”按钮
            // 这里的坐标(100,102,68,16)是相对于对话框客户区的像素值，
            // 直接用数字而不是 D( )，所以高 DPI 下位置不会跟着缩放——属于现状，未改动。
            HWND hCopy = CreateWindowExW(0, L"Button", L"复制 QQ 号",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         100, 102, 68, 16, hDlg, (HMENU)(INT_PTR)IDC_BTN_COPYQQ,
                                         g_hInst, nullptr);
            ApplyUiFont(hCopy);   // 新按钮也要套用统一字体，否则会是系统默认字体
        }
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_COPYQQ)   // 只认按钮 ID，不必再看 HIWORD 的通知码
        {
            SetClipText(hDlg, L"2668114784");   // 作者的 QQ 号
            SetWindowTextW(GetDlgItem(hDlg, IDC_BTN_COPYQQ), L"已复制!");
            // GetDlgItem：按 ID 取对话框内子控件句柄（等价于在子窗口里按 ID 查找）
            return (INT_PTR)TRUE;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
