// ============================================================================
//
//  【这个文件负责什么】
//    Pic246 里所有跟“图片数据”有关的脏活累活都集中在这里，界面代码一行像素都不直接碰。
//    可以把它理解成一个只对外暴露函数、不弹自己窗口的“服务层”。
//
//  【和哪些文件配合】
//    · App.h            —— 公共声明（全局变量 extern、函数原型、AdjustParams 等结构体）。
//    · Project001.cpp   —— 主窗口。它负责 GdiplusStartup/GdiplusShutdown，也定义了
//                          g_srcPath / g_pSrcImage / g_pSrcStream / g_srcW / g_srcH 等全局状态。
//                          注意：这些全局变量在【本文件里是“用”而不是“定义”】。
//    · Page*.cpp        —— 缩放/裁切/调整/ICO/压缩/格式转换/涂鸦 各功能页，调用本文件的函数。
//    · CropCanvas.cpp / DrawCanvas.cpp —— 交互画布，负责取像素、贴图预览。
//
//  【函数分组】
//    1. 路径 / 字符串工具          FileNameOf、DirNameOf、StemOf、ExtOf、FileExists、IntToStr、FormatSize
//    2. 输出格式表与编码器解析      OutFmt、kOutFmts、FindFmt、SourceFmt、GetEncoderClsid
//    3. 载入 / 释放                LoadImageFile、ClearImage、NotifyImageChanged
//    4. 缩放与预览底图              ResizeBitmap、CreatePreviewSource
//    5. 模糊                       BoxBlurH、BoxBlurV、BoxBlur
//    6. 明度 / 饱和度 / 模糊        ClampB、ApplyAdjustments
//    7. 取色对话框                  ChooseFillColor
//    8. 保存（加后缀）              SaveBitmapTo、SaveBitmapWithSuffix、ShowDoneAndOpenFolder
//    9. ICO 写出                    PutU16、PutU32、BuildBmpEntry、EncodePngToMemory、SaveIcoWithSuffix
//   10. 绘制辅助                    PaintCheckerBackground、PaintImagePreview、PaintColorSwatch
//   11. 编码器枚举 / 体积预估 / 指定格式保存
//                                  FormatBytes、ListImageEncoders、IsLossyMime、EncodeToMemory、
//                                  EstimateEncodedSize、FindQualityForTargetSize、SourceFileSize、
//                                  FileSizeOf、SaveBitmapAsFormat
//
//  【新手必读：GDI+ 对象模型与三大坑】
//    (1) 谁负责 GdiplusStartup / GdiplusShutdown？
//        不是本文件，而是主程序（Project001.cpp 的 wWinMain 或 WinMain）。
//        规则：GdiplusStartup 之后才能 new Gdiplus::Bitmap / Graphics；
//              所有 GDI+ 对象必须在 GdiplusShutdown 之前析构完毕，否则退出时行为未定义
//              （典型现象是卡住不退出、或退出时崩溃）。
//
//    (2) 几乎所有 GDI+ 构造函数都【不会抛异常】，错误信息藏在 GetLastStatus() 里。
//        例如 `Bitmap* b = new Bitmap(w, h, PixelFormat32bppARGB);` 即使内存不足也会
//        返回一个非空指针，必须再查 b->GetLastStatus() == Ok 才能确认成功。
//        这与 C++ 的惯例（失败就 throw）不同，是本项目里反复出现的写法，看到别奇怪。
//
//    (3) Status 是 GDI+ 自己的枚举，不是 HRESULT。
//        Ok == 0（所以 `!= Ok` 表示失败）；而 IStream/COM 那些接口返回的是 HRESULT，
//        判断成功要用 SUCCEEDED(...)/FAILED(...)。两者不要混用。
//
//  【其它容易踩的坑（正文里还会重复提醒）】
//    · Bitmap 的“所有权”：本文件里凡是名字带 new 出来的 Bitmap，都是交给调用者，
//      由调用者 delete；而 SaveBitmapWithSuffix / SaveIcoWithSuffix / SaveBitmapAsFormat
//      的约定是【函数内部负责 delete】，调用者不要再删一次（否则二次释放必崩）。
//    · PixelFormat32bppARGB 是【预乘 alpha】：内存里存的是 B*alpha/255 之类的值，
//      不是“纯色 + 一个 alpha 通道”。想直接改颜色必须先反预乘，改完再乘回去，
//      否则透明区域颜色会整体偏暗、边缘发黑。
//    · 从内存流载入的 Image，在它活着的时候必须保证那个 IStream 也活着
//      （见 LoadImageFile 里 g_pSrcStream 的注释）。
//    · 所有 Create* 出来的 GDI / COM 对象（HBRUSH、HDC、IStream、HGLOBAL…）都要手动释放，
//      Win32 没有析构函数帮你收尾。
// ============================================================================
//  ImageOps.cpp —— 路径工具、图片载入/保存、缩放、调整（模糊/明度/饱和度）、
//                  ICO 文件写出、以及若干绘制辅助
// ============================================================================
#include "App.h"

using namespace Gdiplus;

// ICO 页可勾选的 7 个尺寸（16/24/32/48/64/128/256 像素）。
// 与 App.h 里的 #define ICO_SIZE_COUNT 7 必须保持一致：
// 数组大小写的是宏 ICO_SIZE_COUNT，宏改了这里会自动跟着变；
// 但花括号里的初始化元素个数是写死的，宏变大而元素没补，多出来的槽位会是 0，
// 后面按 0×0 去缩放就会失败。改宏时记得同步改这一行。
const int kIcoSizes[ICO_SIZE_COUNT] = { 16, 24, 32, 48, 64, 128, 256 };  // 各长度对应的像素边长，下标与 ICO 页复选框一一对应

//   这几个函数都只做“纯字符串切片”，不访问文件系统、不分配堆内存、不抛异常，
//   所以可以放心在 UI 线程高频调用（例如每次重绘都去算一遍文件名）。
//   共同约定：路径参数一律是 UTF-16 的 std::wstring（Win32 的“宽字符”版本）。
// ---------------------------------------------------------------------------
// 路径 / 字符串工具  // 下面这批只做字符串拼接/切片，不碰文件系统
// ---------------------------------------------------------------------------
// 从完整路径里取出“文件名（含扩展名）”部分。
//   参数 path：形如 L"C:\\imgs\\a.png" 的完整路径；也允许传相对路径或纯文件名。
//   返回：最后一个 '\\' 或 '/' 之后的部分；路径里没有分隔符时原样返回。
// 实现要点：
//   · find_last_of(L"\\/") 同时兼容反斜杠和正斜杠——用户从浏览器/资源管理器里
//     复制来的路径两种都可能出现，这里统一处理。
//   · npos 是 std::string 系列用来表示“没找到”的特殊值（可以理解成 -1），
//     必须判它，否则 npos+1 会绕回到 0，substr 就变成整串了。
//   · 路径以分隔符结尾（L"C:\\dir\\"）时返回空串，符合直觉。
std::wstring FileNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// 从完整路径里取出“所在目录（含结尾的分隔符）”部分。
//   参数 path：完整路径。
//   返回：最后一个分隔符及其之前的内容（所以 L"C:\\a\\b.png" -> L"C:\\a\\"，
//         结尾的 '\\' 是故意保留的，方便后面直接拼文件名）；
//         路径里一个分隔符都没有时返回空串（表示“当前目录”）。
// 本文件里大量使用 “DirNameOf(g_srcPath) + StemOf(g_srcPath) + 后缀 + 扩展名”
// 来生成输出路径，也就是【输出永远和原图放在同一个文件夹】，不会乱丢到别处。
std::wstring DirNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring() : path.substr(0, p + 1);
}

// 取“主文件名”，即去掉目录、去掉最后一个点之后扩展名的部分。
//   例：L"C:\\a\\photo.2024.png" -> L"photo.2024"（只砍最后一个点后面的东西）。
//   参数 path：完整路径或纯文件名。
//   返回：主文件名；没有扩展名时返回原文件名。
// 边界处理：
//   · dot > 0 的判断是为了处理 Unix 风格的隐藏文件名（如 L".gitignore"），
//     此时点号在开头、并不是扩展名分隔符，所以整名返回。
//   · 生成的输出文件（xxx_zoom.png 之类）就是在这个结果后面拼后缀的。
std::wstring StemOf(const std::wstring& path)
{
    std::wstring name = FileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        return name.substr(0, dot);
    return name;
}

// 取“扩展名”，带开头的点。
//   例：L"C:\\a\\b.JPEG" -> L".JPEG"（大小写原样保留，需要比较时另用 _wcsicmp）。
//   没有扩展名（或点号在开头）时返回空串。
// 注意：只按字符串取，不查文件系统。所以 L"archive.tar.gz" 只会得到 L".gz"，
// 这对图片名来说正是想要的行为。
std::wstring ExtOf(const std::wstring& path)
{
    std::wstring name = FileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        return name.substr(dot);
    return std::wstring();
}

// 判断路径是否存在。
//   实现是查文件属性：GetFileAttributesW 在路径不存在时返回 INVALID_FILE_ATTRIBUTES，
//   其余情况（文件、目录、只读文件……）都会返回值。
// 注意两点坑：
//   · 它不区分“文件”和“目录”，目录也会返回 true；本文件只拿它判断输出文件是否已存在，
//     所以不区分也没关系。
//   · 传相对路径时，返回的是相对于【进程当前工作目录】的结果，而不是相对 exe 的位置。
//     拖拽打开文件时 cwd 往往就是 exe 所在目录，容易和用户想的不一样。
bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 把整数转成 wstring（Win32 里没有 std::to_wstring 那样好用的现成函数时常用这招）。
//   参数 v：long long（64 位有符号）。
//   返回：十进制字符串，不带正号。
// 为什么要固定缓冲区：Win32 API 不返回 std::wstring，只能先写进一块栈上缓冲再构造字符串。
//   %lld 最长也只有 20 位（含负号），buf[32] 是绰绰有余的余量；
//   用的是 swprintf_s（带边界检查的安全版），越界会直接终止程序而不是悄悄写坏内存。
// 用它的场景主要是拼状态栏文字，例如“1920 × 1080”。
std::wstring IntToStr(long long v)
{
    wchar_t buf[32];
    swprintf_s(buf, L"%lld", v);  // 缓冲区写成 32 是为了留足余量：long long 最长 20 位
    return buf;
}

// 把宽高拼成“1920 × 1080”这样的可读字符串。
//   参数 w/h：宽、高（UINT，无符号 32 位）。
//   返回：L"宽 × 高"（中间那个是乘号 U+00D7，不是字母 x）。
//   %u 对应 unsigned int；buf[64] 对两个 UINT 来说余量很大。
// 注意：这是【纯展示】用的字符串，不要去解析它——项目和界面无关的地方都直接用数字。
std::wstring FormatSize(UINT w, UINT h)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%u × %u", w, h);  // %u × %u：乘号是 U+00D7，不是字母 x
    return buf;
}

//   一张小表：扩展名 -> MIME -> 是否支持透明通道。
//   MIME 是给 GDI+ 用的“编码器身份证”，GDI+ 不认扩展名，只认 MIME 字符串，
//   所以每次保存前都要先用 MIME 去系统里查出对应的编码器 CLSID。
// ---------------------------------------------------------------------------
// 输出格式
// ---------------------------------------------------------------------------
// 描述一种输出格式的三要素。
//   ext   —— 扩展名，带点，例如 L".png"；
//   mime  —— GDI+ 识别编码器用的 MIME 串，必须和系统注册的一模一样；
//   alpha —— 这个格式能否表达“半透明/透明”。
//            JPG/BMP 没有 alpha 通道，把带透明的图存过去会变成黑底或不透明底色，
//            界面要靠这个标志提前提醒用户，所以它不只是说明性的字段。
struct OutFmt
{
    const wchar_t* ext;
    const wchar_t* mime;
    bool           alpha;  // 这种格式能否保存透明通道；JPG/BMP 会丢 alpha
};

// 全部“能作为输出”的格式表。
//   注意 .jpg 和 .jpeg 都映射到同一个 image/jpeg，.tif/.tiff 同理：
//   它们只是同一种格式的两种写法，所以表里必须各列一行，才能两个都查到。
//   GIF 理论上支持 1 位透明，这里记 true；TIFF 支持 8 位 alpha，也记 true。
//   WebP 不在这里 —— 是否可用取决于系统有没有装对应解码器，见 ListImageEncoders()。
// 表里的 ext 字段用于生成文件名，mime 字段交给 GetEncoderClsid 去查 CLSID，两者分工明确。
static const OutFmt kOutFmts[] =
{
    { L".png",  L"image/png",  true  },  // PNG 无损、支持透明，作为默认输出格式
    { L".jpg",  L"image/jpeg", false },  // JPEG 有损、无 alpha
    { L".jpeg", L"image/jpeg", false },  // .jpeg 是 .jpg 的另一种写法，必须单独列一行才能查到
    { L".bmp",  L"image/bmp",  false },  // BMP 无压缩，体积大，无 alpha
    { L".gif",  L"image/gif",  true  },  // GIF 只支持 1 位透明（全透明或全不透明）
    { L".tif",  L"image/tiff", true  },  // TIFF 支持 alpha
    { L".tiff", L"image/tiff", true  },  // .tiff 是 .tif 的另一种写法
};

// 在 kOutFmts 表里按扩展名查找格式，找不到返回 nullptr。
//   参数 ext：带点的扩展名（L".png"），大小写不敏感（内部用 _wcsicmp）。
//   返回：指向表内静态元素的指针；不需要（也不能）调用者释放。
// 为什么返回指针而不是值：表是 static 常量，指向它永远是安全的；
// 返回结构体副本反而会多一次拷贝。调用方拿到 nullptr 时必须自己兜底默认格式。
static const OutFmt* FindFmt(const std::wstring& ext)
{
    for (int i = 0; i < (int)_countof(kOutFmts); ++i)
        if (_wcsicmp(kOutFmts[i].ext, ext.c_str()) == 0)  // 扩展名大小写不敏感
            return &kOutFmts[i];
    return nullptr;
}

// 根据“当前源图路径”的扩展名，猜出该用哪种输出格式。
//   返回：永远不为空 —— 认不出来时退回 PNG。
// 为什么用扩展名猜：GDI+ 的 Image 对象不提供“我原本是什么格式”的可靠查询，
//   而项目的语义是“同格式另存一份（加后缀）”，于是直接看文件名后缀最省事，
//   也符合用户预期（打开 photo.jpg，处理完仍然得到 jpg）。
// 依赖全局 g_srcPath（定义在 Project001.cpp），所以必须先 LoadImageFile 成功再用。
static const OutFmt* SourceFmt()
{
    const OutFmt* f = FindFmt(ExtOf(g_srcPath));
    if (!f) f = FindFmt(L".png");
    return f;
}

// 对外的查询接口：当前源图对应的输出格式支不支持透明？
//   各功能页在“要不要显示透明棋盘格 / 要不要禁用透明复选框”时会问它。
//   等价于 SourceFmt()->alpha，因为 SourceFmt 保证不返回空指针，这里可以直接解引用。
bool FormatSupportsAlpha()
{
    return SourceFmt()->alpha;  // SourceFmt 保证非空，可以直接取 ->alpha
}

// 把“源文件的格式信息”打包成一个结构体交给调用者，主要给“格式转换页”做默认选中项。
//   返回：{ mime, ext(带点), alpha }。
//   下面两行是把扩展名归一化：
//     .jpeg -> .jpg、.tiff -> .tif，
//   因为界面的下拉框/复选框只提供短写法，统一成短名才能正确匹配选中。
// 注意 g.mime 是从表里取的 const wchar_t*，赋给 std::wstring 时发生了字符串拷贝，
// 所以调用者拿到的副本和那张静态表的生命周期无关，随便存。
OutFmtGuess GuessSourceMime()
{
    const OutFmt* f = SourceFmt();
    OutFmtGuess g;
    g.mime = f->mime;
    g.ext = f->ext;
    g.alpha = f->alpha;
    if (_wcsicmp(g.ext.c_str(), L".jpeg") == 0) g.ext = L".jpg";  // 界面上只提供 .jpg 短写法
    if (_wcsicmp(g.ext.c_str(), L".tiff") == 0) g.ext = L".tif";  // 界面上只提供 .tif 短写法
    return g;
}

// 按 MIME 查出 GDI+ 图像编码器的 CLSID（这是 GDI+ 保存图片最经典的“两次调用”套路）。
//   参数 mime   ：如 L"image/png"；
//   参数 pClsid ：输出参数，成功时写入编码器的 CLSID。
//   返回：找到返回 true，否则 false。
// 为什么必须两次调用：
//   第一次传 (num, size) 只问“有多少个编码器、需要多大一块内存”；
//   第二次才把缓冲区地址传进去真正取数据。缓冲区大小绝不写死，
//   不同 Windows 版本装的东西不一样，写死必然越界。
// 两个易错点：
//   · 缓冲区必须按 BYTE 对齐分配，再强转成 ImageCodecInfo* 使用——
//     GDI+ 要求的是字节缓冲，不是对象数组。
//   · MIME 比较大小写不敏感（_wcsicmp），因为系统里实际存的写法不见得和你写的一样。
// 返回值判断用的是 GDI+ 的 Status（Ok == 0），不是 HRESULT，注意别混。
static bool GetEncoderClsid(const wchar_t* mime, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0)  // 第 1 次调用：只问编码器个数和所需缓冲区大小
        return false;

    std::vector<BYTE> buf(size);
    ImageCodecInfo* pInfo = (ImageCodecInfo*)buf.data();  // 分配字节缓冲区
    if (GetImageEncoders(num, size, pInfo) != Ok)  // 第 2 次调用：真正把编码器信息取回来
        return false;

    for (UINT i = 0; i < num; ++i)
    {
        if (_wcsicmp(pInfo[i].MimeType, mime) == 0)  // MIME 比较忽略大小写
        {
            *pClsid = pInfo[i].Clsid;
            return true;
        }
    }
    return false;
}

//   程序的“当前图片”由一组全局变量共同表示，它们必须始终一致：
//     g_pSrcImage  —— GDI+ 解码出来的原图（始终是它自己的第一帧格式）；
//     g_pSrcStream —— 支撑这张图的 IStream，必须晚于图片释放；
//     g_srcW/H     —— 像素尺寸缓存，避免到处重复调 GetWidth()；
//     g_srcHasAlpha—— 按扩展名猜的“输出是否支持透明”；
//     g_srcPath    —— 原文件完整路径，所有“加后缀另存”都基于它。
//   任何修改这组状态的地方都要走这里提供的两个函数，不要零散地改。
// ---------------------------------------------------------------------------
// 载入 / 释放
// ---------------------------------------------------------------------------
// 卸载当前图片，把全局状态恢复成“没有打开图片”。
//   无参数、无返回值。
// 释放顺序非常重要（这里也是新手最容易写错的地方）：
//   ① 先 delete 图片 —— Image 析构时可能还要回去读流里尚未解码的数据；
//   ② 再 Release 流 —— 流一旦先释放，图片析构就是访问已释放对象（悬垂指针，会随机崩）。
//   顺序搞反平时可能“看起来没事”，换个格式（如 TIFF 多帧）就炸，所以必须按这个顺序写。
//   另外：这一步只是清掉【副本】，原文件从头到尾没有被我们锁定过（LoadImageFile 里是
//   先读进内存再解析的），所以清完之后用户随时可以删掉/覆盖原文件。
//   () 里的三行是 C 语言风格的类型转换，等价于 (UINT) 写法，这里是历史写法，不用改。
void ClearImage()
{
    if (g_pSrcImage) { delete g_pSrcImage; g_pSrcImage = nullptr; }  // 先让图片析构，它会用到流里尚未解码的数据
    if (g_pSrcStream) { g_pSrcStream->Release(); g_pSrcStream = nullptr; }  // 再释放 IStream，顺序不能反
    g_srcW = g_srcH = 0;
    g_srcHasAlpha = false;  // 顺便标记“当前源图不带透明”
    g_srcPath.clear();
}

// 广播“源图已经换了”这个消息，让所有功能页刷新自己的预览图/重置裁切框。
//   走的是自定义消息 WM_APP_IMAGE_CHANGED（见 App.h），wParam/lParam 都不用。
//   为什么用 SendMessage 而不是直接调用各页函数：
//     页面是自己创建的子窗口，主窗口不必知道它们是谁；由主窗口的窗口过程统一分发，
//     新增页面时不需要改这里，耦合最低。
//   SendMessage 是【同步】的：调用期间各页会立刻处理完消息，返回后状态已刷新。
//   先判 g_hMain 是否为空：程序启动早期窗口还没建好时也会调用到这里。
void NotifyImageChanged()
{
    if (g_hMain)  // 主窗口还没创建时什么都不做
        SendMessageW(g_hMain, WM_APP_IMAGE_CHANGED, 0, 0);
}

// 先把文件整个读进内存，再用内存流交给 GDI+，避免长期锁定原文件
// 打开并载入一张图片，成功后成为程序的“当前图片”。
//   参数 path：要打开的完整路径；
//   参数 err ：输出参数，失败时写入给用户看的中文错误信息。
//   返回：成功 true / 失败 false（失败时全局状态保持不变）。
//
// 核心思路（第 156 行那句注释说的“避免长期锁定原文件”）：
//   标准做法是 Image::FromFile(path)，但那样 GDI+ 会一直持有文件句柄，
//   用户在资源管理器里就删不掉、改不了这个文件，直到切换图片。
//   这里改成：CreateFile 读进一块全局内存 -> 用内存流喂给 GDI+ -> 立刻关文件句柄，
//   于是从用户角度看图片是“独占”的，随便改名覆盖都没问题。
//
// 为什么用 CreateFileW 而不是 std::ifstream：
//   ① 支持超过 2 GB 的偏移（虽然这里限了 1 GB）；
//   ② 打开失败能拿到 Win32 错误码；
//   ③ FILE_SHARE_READ | FILE_SHARE_WRITE 表示“允许别人同时读和写”，
//      这样即使别的程序已经打开了这个文件，我们也可能读得到（反之亦然）。
//
// 各步失败时的清理都要自己写：Win32 没有 RAII，漏掉 CloseHandle/GlobalFree 就是句柄泄漏。
bool LoadImageFile(const std::wstring& path, std::wstring& err)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,  // 共享读写：避免独占文件，别人改名/删除时也不会被我们挡住
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        err = L"无法打开文件：\r\n" + path;
        return false;
    }

    LARGE_INTEGER liSize = {};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0)  // 顺带检查文件大小是否可读
    {
        CloseHandle(hFile);
        err = L"文件为空或无法读取文件大小。";
        return false;
    }
    if (liSize.QuadPart > 1024LL * 1024 * 1024)  // 1 GB 上限，防止误选超大文件把内存吃光
    {
        CloseHandle(hFile);
        err = L"文件过大（超过 1 GB），暂不支持。";
        return false;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)liSize.QuadPart);  // 必须是 GMEM_MOVEABLE：CreateStreamOnHGlobal 要求可移动内存块
    if (!hMem)
    {
        CloseHandle(hFile);
        err = L"内存不足，无法读入图片。";
        return false;
    }

    BYTE* pData = (BYTE*)GlobalLock(hMem);  // 锁定后拿到可写指针；理论上可能失败，下面会一起判断
    LONGLONG total = 0;
    bool ok = (pData != nullptr);
    while (ok && total < liSize.QuadPart)
    {
        DWORD want = (DWORD)((liSize.QuadPart - total) > (1 << 20) ? (1 << 20) : (liSize.QuadPart - total));  // 一次读 1 MB，兼顾速度与兼容性
        DWORD got = 0;
        if (!ReadFile(hFile, pData + total, want, &got, nullptr) || got == 0)  // got == 0 表示文件提前结束，同样按失败处理
            ok = false;
        else
            total += got;
    }
    GlobalUnlock(hMem);  // 数据已经拷进缓冲区，句柄可以解锁了（内存块本身仍然有效）
    CloseHandle(hFile);

    if (!ok)
    {
        GlobalFree(hMem);
        err = L"读取文件失败。";
        return false;
    }

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)))  // 第二个参数 TRUE = 由流负责释放这块内存
    {
        GlobalFree(hMem);
        err = L"创建内存流失败。";
        return false;
    }

    Image* pImg = Image::FromStream(pStream);  // 从内存流解码；GDI+ 会自己复制所需数据
    if (!pImg || pImg->GetLastStatus() != Ok || pImg->GetWidth() == 0 || pImg->GetHeight() == 0)  // GetWidth/GetHeight 返回的是第 1 帧尺寸
    {
        delete pImg;
        pStream->Release();
        err = L"无法解析该图片，格式不受支持或文件已损坏。\r\n\r\n支持：PNG / JPG / BMP / GIF / TIFF";
        return false;
    }

    ClearImage();
    g_pSrcImage = pImg;  // 注意：此时才把全局状态换成新图
    g_pSrcStream = pStream;         // 与图片同生命周期  // 流必须活到图片析构之后（见 ClearImage 的顺序）
    g_srcW = pImg->GetWidth();
    g_srcH = pImg->GetHeight();
    g_srcPath = path;
    g_srcHasAlpha = FormatSupportsAlpha();  // 按扩展名猜输出格式是否支持透明
    return true;
}

//   统一产出 32bppARGB 的 Bitmap；缩放本身不是自己写循环，而是让 GDI+ 画一遍
//   （Graphics::DrawImage 会自动做重采样），这样只需要把插值质量调高即可。
// ---------------------------------------------------------------------------
// 缩放
// ---------------------------------------------------------------------------
// 把任意 Image 缩放到指定大小，返回一张新的 Bitmap。
//   参数 src ：源图片，只读，不会被修改；为 nullptr 时直接返回 nullptr；
//   参数 newW/newH：目标宽高（小于 1 会被夹到 1，GDI+ 不接受 0 尺寸）；
//   参数 err ：输出参数，失败时写入中文错误信息。
//   返回：新的 Bitmap（【所有权归调用者，必须 delete】）；失败返回 nullptr。
//
// 为什么直接 new Bitmap 而不是 std::unique_ptr：项目整体是传统 C 风格，
//   这里是显式 new/delete 配对；调用者（各功能页）拿到后用 delete 释放。
// 为什么目标固定 PixelFormat32bppARGB：
//   统一像素格式后，所有后续处理（改像素、模糊、拼 ICO）都只有一种内存布局，
//   不用给每种格式写一套分支。透明通道也能一路带下去。
// 顺带一个 GDI+ 的坑：把 24bpp 的图绘到 32bpp 的画布上时，
//   未被覆盖的区域初始全 0 —— 也就是【完全透明】而不是黑色。
//   这里因为是“整幅覆盖 + CompositingModeSourceCopy”，所以没有问题。
Bitmap* ResizeBitmap(Image* src, int newW, int newH, std::wstring& err)
{
    if (!src) return nullptr;  // 源图为空，直接失败
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;

    Bitmap* dst = new Bitmap(newW, newH, PixelFormat32bppARGB);  // ARGB：带透明通道，后续处理统一按这个格式走
    if (dst->GetLastStatus() != Ok)
    {
        delete dst;
        err = L"创建目标位图失败（内存不足？）";
        return nullptr;
    }

    {
        Graphics g(dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);  // 高质量三次卷积插值，缩小/放大都平滑
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetSmoothingMode(SmoothingModeHighQuality);  // 消除边缘锯齿，避免旋转/缩放时出现半透明毛边
        g.SetCompositingQuality(CompositingQualityHighQuality);
        g.SetCompositingMode(CompositingModeSourceCopy);

        ImageAttributes attr;
        attr.SetWrapMode(WrapModeTileFlipXY);   // 消除边缘黑边  // 关闭平铺，防止边缘出现黑边

        RectF dest(0.0f, 0.0f, (REAL)newW, (REAL)newH);
        Status st = g.DrawImage(src, dest,  // DrawImage 会自动做重采样，不需要自己写循环
                                0.0f, 0.0f, (REAL)src->GetWidth(), (REAL)src->GetHeight(),
                                UnitPixel, &attr);
        if (st != Ok)
        {
            delete dst;
            err = L"缩放绘制失败。";
            return nullptr;
        }
    }
    return dst;
}

// 生成一张“预览底图”：按最长边不超过 maxDim 等比缩小；本来就不大就原样返回一份。
//   参数 maxDim：最长边上限（像素），例如 1600。
//   返回：新的 Bitmap（调用者负责 delete）；没有打开图片时返回 nullptr。
// 为什么要单独缩一张：
//   原图可能是 8000×6000，每帧预览都直接缩放原图会非常卡；
//   先缩一张小的存起来，界面刷新就快得多（这也是预览看起来有点糊的原因）。
// 数值细节：
//   s 初始为 1.0，只有“确实超限”时才会被改写，避免把小图放大。
//   sx、sy 分别算宽和高的缩放比，取较小者才能保证【长边】不超限（等比缩放）。
//   “+ 0.5 再取整”就是四舍五入；C 的强制转换是直接截断，会把 799.9 变成 799。
//   最后再兜一次底：缩得太小的图可能算出 0，而 0 尺寸的 Bitmap 必定创建失败。
// 传进去的 err 是临时变量，这里不关心具体错误文字（预览失败没必要弹窗）。
Bitmap* CreatePreviewSource(int maxDim)
{
    if (!g_pSrcImage) return nullptr;

    double s = 1.0;
    if ((UINT)maxDim < g_srcW || (UINT)maxDim < g_srcH)
    {  // 分别算宽、高缩放比，取小者保证等比不超限
        double sx = (double)maxDim / (double)g_srcW;
        double sy = (double)maxDim / (double)g_srcH;
        s = (sx < sy) ? sx : sy;
    }

    int pw = (int)(g_srcW * s + 0.5);
    int ph = (int)(g_srcH * s + 0.5);
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;

    std::wstring err;
    return ResizeBitmap(g_pSrcImage, pw, ph, err);
}

// 模糊：可分离盒式模糊（separable box blur），做 3 遍近似高斯
//   为什么要“可分离”：直接在二维邻域上做模糊是 O(w·h·r²)，太慢；
//   盒式模糊可以拆成“先横向一遍、再纵向一遍”，复杂度降到 O(w·h·r)，
//   而连续做 3 遍盒式模糊的结果在视觉上已经很接近高斯模糊了。
//   为什么是 4 个通道：缓冲区里的像素是【预乘过的 BGRA】，
//   透明区域的颜色值也是有效数据，必须一起参与模糊，否则边缘会出现渗色。
// ---------------------------------------------------------------------------
// 模糊：可分离盒式模糊，做 3 遍近似高斯；数据为预乘 BGRA
// ---------------------------------------------------------------------------
// 横向盒式模糊：对每行做一维滑动窗口平均。
//   参数 src：源缓冲区（只读，紧凑排列，每像素 4 字节 BGRA，行间无 padding）；
//   参数 dst：目标缓冲区（必须与 src 不同，可写）；
//   参数 w/h：像素宽高；
//   参数 r ：模糊半径（窗口宽度 = 2r+1，r 越大越糊）。
// 滑动窗口的核心思想：不要对每个像素都重新累加 2r+1 个值，而是
//   “上一个窗口的和 - 移出的像素 + 移入的像素”，每个像素只需常数次运算。
// 边界处理：越界的采样点用【最近的边缘像素】代替（也就是复制边缘，clamp），
//   这样图像边缘不会因为和空气平均而变暗；代价是边缘看起来略微被拉伸。
// 为什么循环里一直除 win：每输出一个像素做一次除法，现代 CPU 上可以接受；
//   想更快可以预先算 1.0/win 用乘法，注意别改成会产生误差的写法。
// 定点的细节：所有值都在 0..255 的 BYTE 里做整数运算，
//   单次窗口平均不会溢出（sum 最多 255×(2r+1)，r 已被上层限制得很小），
//   所以这里的 (BYTE)(sum[c] / win) 是安全的。
static void BoxBlurH(const BYTE* src, BYTE* dst, int w, int h, int r)
{
    const int win = r * 2 + 1;
    for (int y = 0; y < h; ++y)
    {
        const BYTE* s = src + (size_t)y * w * 4;
        BYTE* d = dst + (size_t)y * w * 4;
        int sum[4] = { 0, 0, 0, 0 };

        for (int i = -r; i <= r; ++i)
        {
            int xi = (i < 0) ? 0 : ((i >= w) ? w - 1 : i);  // 越界时复制边缘像素（clamp）
            const BYTE* px = s + (size_t)xi * 4;
            for (int c = 0; c < 4; ++c) sum[c] += px[c];
        }

        for (int x = 0; x < w; ++x)
        {
            for (int c = 0; c < 4; ++c) d[(size_t)x * 4 + c] = (BYTE)(sum[c] / win);  // 输出当前窗口平均值

            int xOut = x - r; if (xOut < 0) xOut = 0;  // 窗口右移：先去掉最左边、再加进右边新像素
            int xIn = x + r + 1; if (xIn >= w) xIn = w - 1;
            const BYTE* pOut = s + (size_t)xOut * 4;
            const BYTE* pIn = s + (size_t)xIn * 4;
            for (int c = 0; c < 4; ++c) sum[c] += pIn[c] - pOut[c];
        }
    }
}

// 纵向盒式模糊：逻辑与 BoxBlurH 完全相同，只是每次沿一列走。
//   参数含义同 BoxBlurH。
// 关键差异是内存跨度：横向处理时相邻像素相差 4 字节（连续），
//   纵向处理时相邻像素相差一整行 rowBytes，属于“大步长跳跃”。
//   这是缓存不友好访问，也是为什么先做横向再做纵向通常比反过来略快。
//   预先把 rowBytes = w*4 算出来，避免在循环里反复乘。
static void BoxBlurV(const BYTE* src, BYTE* dst, int w, int h, int r)
{
    const int win = r * 2 + 1;
    const size_t rowBytes = (size_t)w * 4;  // 纵向访问的跨度是一整行

    for (int x = 0; x < w; ++x)
    {
        const BYTE* s = src + (size_t)x * 4;
        BYTE* d = dst + (size_t)x * 4;
        int sum[4] = { 0, 0, 0, 0 };

        for (int i = -r; i <= r; ++i)
        {
            int yi = (i < 0) ? 0 : ((i >= h) ? h - 1 : i);
            const BYTE* px = s + (size_t)yi * rowBytes;
            for (int c = 0; c < 4; ++c) sum[c] += px[c];
        }

        for (int y = 0; y < h; ++y)
        {
            for (int c = 0; c < 4; ++c) d[(size_t)y * rowBytes + c] = (BYTE)(sum[c] / win);

            int yOut = y - r; if (yOut < 0) yOut = 0;  // 同样的滑动窗口，只是步长变成一行
            int yIn = y + r + 1; if (yIn >= h) yIn = h - 1;
            const BYTE* pOut = s + (size_t)yOut * rowBytes;
            const BYTE* pIn = s + (size_t)yIn * rowBytes;
            for (int c = 0; c < 4; ++c) sum[c] += pIn[c] - pOut[c];
        }
    }
}

// 在内存缓冲区上做完整的模糊：横一遍 + 纵一遍，循环 3 次（近似高斯）。
//   参数 buf   ：输入输出缓冲区，尺寸必须是 w*h*4 字节，原地被修改；
//   参数 w/h   ：像素宽高；
//   参数 r     ：半径；
//   参数 passes：重复几轮（调用方固定传 3）。
// 半径夹取（三个 if）是必须的：
//   半径大于半个尺寸时滑动窗口会跨越整个图像，除了浪费算力没有任何视觉效果，
//   甚至可能让窗口比图像还宽；夹到 w/2、h/2 之后再判一次 r<1 直接返回。
// tmp 的作用是让每一步都在“另一块缓冲”之间来回倒：
//   一次横模糊 buf -> tmp，紧接着纵模糊 tmp -> buf，两轮下来 buf 又是最新结果，
//   不需要第三次拷贝。注意 tmp 只分配一次，循环里复用，避免反复申请大块内存。
static void BoxBlur(std::vector<BYTE>& buf, int w, int h, int r, int passes)
{
    if (r < 1) return;
    if (r > w / 2) r = w / 2;  // 半径不能超过半宽，否则窗口会大得没有意义
    if (r > h / 2) r = h / 2;
    if (r < 1) return;

    std::vector<BYTE> tmp(buf.size());  // 用另一块等大缓冲做中转，避免原地覆盖
    for (int i = 0; i < passes; ++i)
    {
        BoxBlurH(buf.data(), tmp.data(), w, h, r);  // 横向一遍
        BoxBlurV(tmp.data(), buf.data(), w, h, r);  // 纵向一遍，结果回到 buf
    }
}

// 把整数夹到 0..255（BYTE 的取值范围）再返回，避免颜色计算溢出回绕。
//   为什么必须夹：如果把 260 直接塞进 BYTE，会变成 4 —— 亮部会突然变成黑点，
//   这种“回绕”在图像处理后处理里是最常见的花屏原因之一。
// 内联（inline）+ 三目运算符：这个函数在这个文件里被调用得非常频繁，
//   写成函数调用开销明显；编译器内联后就是几条比较和选择指令。
// 注意 (v < 0) 用的是有符号比较，所以调用者可以放心传负数进来。
static inline BYTE ClampB(int v) { return (BYTE)((v < 0) ? 0 : ((v > 255) ? 255 : v)); }

// 在【原地】修改位图的像素：先做明度/饱和度，再做模糊。
//   参数 bmp：要处理的位图，会被直接改写（所以调用者应先拷贝一份，别改到原图）；
//   参数 p  ：AdjustParams { blur, brightness, saturation }（约束见 App.h）：
//               blur 0..40 像素半径，brightness -100..100，saturation -100..100。
//   无返回值；参数非法或锁定失败时静默返回。
//
// 为什么用 LockBits 而不是 GetPixel/SetPixel：
//   GetPixel/SetPixel 每次调用都有函数调用 + 参数校验的开销，
//   百万像素的图上要慢几个数量级；LockBits 一次拿到可写的像素指针，
//   之后就是纯粹的指针运算，速度差常在上百倍。
//
// 三个“快速返回”条件都值得理解：
//   p.blur <= 0 且明度、饱和度都是 0 时，用户其实什么都没调，直接不做；
//   尺寸为 0 的位图（创建失败的残次品）也直接跳过，避免后面除零。
//
// 【预乘 alpha 的坑】PixelFormat32bppARGB 在内存里的颜色分量是预乘过的：
//   半透明纯红 (255,0,0,alpha=128) 存下来其实是 (128,0,0,128)。
//   所以本函数读写像素都要按“先反预乘 -> 改颜色 -> 再预乘”的顺序来，
//   否则透明的像素颜色会越来越暗、边缘发黑。
//
// 需要提醒的是：整个过程写在锁定的像素缓冲上，结束后必须 UnlockBits（见函数末尾）。
void ApplyAdjustments(Bitmap* bmp, const AdjustParams& p)
{
    if (!bmp) return;
    if (p.blur <= 0 && p.brightness == 0 && p.saturation == 0) return;

    const int w = bmp->GetWidth();
    const int h = bmp->GetHeight();
    if (w < 1 || h < 1) return;

    Rect rc(0, 0, w, h);
    BitmapData data;
    if (bmp->LockBits(&rc, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok)
        return;

    const int stride = data.Stride;
    BYTE* base = (BYTE*)data.Scan0;

    const float bf = 1.0f + (float)p.brightness / 100.0f;   // 0 .. 2  // +100% 相当于乘 2 倍；-100% 相当于全黑
    const float sf = 1.0f + (float)p.saturation / 100.0f;   // 0 .. 2

    // 1) 明度 / 饱和度，然后预乘 alpha
    for (int y = 0; y < h; ++y)
    {
        BYTE* row = base + (size_t)y * stride;
        for (int x = 0; x < w; ++x)
        {
            BYTE* px = row + (size_t)x * 4;           // B G R A
            float b = (float)px[0];
            float g = (float)px[1];
            float r = (float)px[2];  // 内存顺序是 B G R A，不是 RGBA！
            const float a = (float)px[3];

            if (p.brightness != 0)
            {
                b *= bf; g *= bf; r *= bf;
            }
            if (p.saturation != 0)
            {
                const float lum = 0.114f * b + 0.587f * g + 0.299f * r;  // 按人眼敏感度加权求亮度
                b = lum + (b - lum) * sf;  // 绕开亮度缩放颜色，>1 更鲜艳，<1 偏灰
                g = lum + (g - lum) * sf;
                r = lum + (r - lum) * sf;
            }

            const float af = a / 255.0f;  // 预乘 alpha：颜色分量要乘上 alpha 比例
            px[0] = ClampB((int)(ClampB((int)(b + 0.5f)) * af + 0.5f));  // 外层 ClampB 防除法后越界
            px[1] = ClampB((int)(ClampB((int)(g + 0.5f)) * af + 0.5f));
            px[2] = ClampB((int)(ClampB((int)(r + 0.5f)) * af + 0.5f));
        }
    }

    // 2) 模糊（预乘空间），再反预乘写回
    if (p.blur > 0)
    {
        const size_t lineBytes = (size_t)w * 4;
        std::vector<BYTE> buf((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            memcpy(buf.data() + (size_t)y * lineBytes, base + (size_t)y * stride, lineBytes);  // 行间可能有 padding，先按紧凑布局复制到临时缓冲

        BoxBlur(buf, w, h, p.blur, 3);  // 3 遍盒式模糊足够接近高斯

        for (int y = 0; y < h; ++y)
        {
            const BYTE* s = buf.data() + (size_t)y * lineBytes;
            BYTE* row = base + (size_t)y * stride;
            for (int x = 0; x < w; ++x)
            {
                const BYTE* sp = s + (size_t)x * 4;
                BYTE* dp = row + (size_t)x * 4;
                const int a = sp[3];
                if (a == 0)  // 全透明像素：四个通道一起清零
                {
                    dp[0] = dp[1] = dp[2] = 0;
                    dp[3] = 0;
                }
                else if (a >= 255)  // 完全不透明，不必除法
                {
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                }
                else
                {
                    dp[0] = ClampB((sp[0] * 255 + a / 2) / a);  // 反预乘：除以 alpha 还原真实颜色
                    dp[1] = ClampB((sp[1] * 255 + a / 2) / a);
                    dp[2] = ClampB((sp[2] * 255 + a / 2) / a);
                    dp[3] = (BYTE)a;  // alpha 原样写回
                }
            }
        }
    }

    bmp->UnlockBits(&data);  // 与 LockBits 配对，必须调用
}

//   Win32 自带的 ChooseColor 对话框封装（裁切页 / ICO 页共用）。
// ---------------------------------------------------------------------------
// 颜色选择
// ---------------------------------------------------------------------------
// 弹出系统“颜色”对话框，让用户挑一个填充色。
//   参数 owner  ：父窗口句柄，用来把对话框做成模态（锁住父窗口）；
//   参数 current：打开时预选的颜色（COLORREF，0x00BBGGRR 格式，注意是 BGR 不是 RGB）；
//   返回：用户确认则返回选中的 COLORREF；点了取消则原样返回 current。
// 细节坑：
//   · lStructSize 必须填 sizeof(cc)，不填或填错会失败（Win32 结构体的通用约定）；
//   · COLORREF 是 0x00BBGGRR：低位是蓝、高位是红，和 RGB(255,0,0) 宏帮你转过来的顺序相反，
//     自己手写十六进制时最容易搞反；
//   · lpCustColors 必须是一块可写内存（16 个 COLORREF）且【生命周期要长于对话框】，
//     所以这里安排成 static 数组，让用户自定义的颜色在多次弹框之间还能保留；
//     如果写成局部数组，对话框关闭后那块栈内存就没了，是典型的悬垂指针 bug；
//   · CC_FULLOPEN = 直接展开调色板，CC_RGBINIT = 使用 rgbResult 作为初始色，
//     CC_ANYCOLOR = 允许调色板之外的任意颜色；
//   · 返回 true/false 来自 BOOL，不是 GDI+ 的 Status。
COLORREF ChooseFillColor(HWND owner, COLORREF current)
{
    static COLORREF cust[16] = {};  // 16 个自定义颜色槽，必须一直有效
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = current;
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;  // 允许调板外的任意颜色
    if (ChooseColorW(&cc))  // 返回 BOOL：用户点“确定”还是“取消”
        return cc.rgbResult;
    return current;
}

//   所有页面的保存都符合同一条规则：输出文件 = 原目录 + 原主名 + 后缀(如 _zoom) + 扩展名。
//   好处是不会有任何“保存到哪去了”的疑问：永远在原图旁边。
// ---------------------------------------------------------------------------
// 保存：统一走“原目录 + 后缀”的流程
// ---------------------------------------------------------------------------
// 完成提示框的标题，多个地方共用，抽成常量避免中文写重复、写错。
static const wchar_t* kCompleteTitle = L"处理完成";

// 真正把 Bitmap 写盘（内部函数）：编码器解析 + 覆盖处理 + 质量参数。
//   参数 outPath：输出文件完整路径；
//   参数 fmt    ：输出格式（来自 kOutFmts，含 mime）；
//   参数 bmp    ：要被保存的位图（本函数【不负责 delete】）；
//   参数 err    ：输出参数，失败时写中文错误。
//   返回：成功 true / 失败 false。
//
// 【EncoderQuality 参数怎么写】，这是 GDI+ 新手最容易卡住的地方：
//   EncoderParameters 是一个“参数数组 + 个数”，要保存 JPEG 画质就必须手工填四个字段：
//     Guid            用 GDI+ 预定义的 EncoderQuality（不是自己 new 一个 GUID）；
//     Type            说明值是什么类型，画质是 ULONG，所以是 EncoderParameterValueTypeLong；
//     NumberValues    值的个数，这里是 1；
//     Value           指向有效值的指针 —— 关键坑：GDI+ 在 Save 期间只是【借用】这个指针，
//                     所以被指向的变量必须活到 Save 返回为止。
//   quality 千万不要声明成函数里的临时值然后传给别的线程或在 Save 前就出作用域；
//   这里 q/quality 都是当前栈帧里的局部变量，跟着 Save 一起结束，是最安全的写法。
//   另外只有【有损】格式（JPEG/WebP）才认画质参数，PNG/BMP 传了会被忽略甚至报错，
//   所以下面按 mime 分了两个分支，非 JPEG 一律传 nullptr。
//
// 其它细节：
//   · 目标文件已存在时先 DeleteFileW：GDI+ 的 Save 对已存在文件的行为依编码器而异，
//     先删掉可以避免“保存成功但内容还是旧的”这类诡异现象；
//     删不掉（被占用）也没关系，后面的 Save 会失败并按错误处理，UI 上会看到提示。
//   · 返回的其实是 GDI+ 的 Status，Ok == 0，所以 st != Ok 就是失败。
static bool SaveBitmapTo(const std::wstring& outPath, const OutFmt* fmt, Bitmap* bmp, std::wstring& err)
{
    CLSID clsid = {};
    if (!GetEncoderClsid(fmt->mime, &clsid))
    {
        err = L"系统中找不到对应的图片编码器，无法保存。";
        return false;
    }

    if (FileExists(outPath))
        DeleteFileW(outPath.c_str());

    Status st;
    if (_wcsicmp(fmt->mime, L"image/jpeg") == 0)
    {
        ULONG quality = 95;  // JPEG 画质 1..100，95 基本看不出损失
        EncoderParameters ep;  // 参数数组，下面逐字段填写
        ep.Count = 1;
        ep.Parameter[0].Guid = EncoderQuality;  // 画质是 ULONG，对应 Long 类型
        ep.Parameter[0].Type = EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Value = &quality;  // Value 必须指向生命周期覆盖 Save 调用的变量
        st = bmp->Save(outPath.c_str(), &clsid, &ep);
    }
    else
    {
        st = bmp->Save(outPath.c_str(), &clsid, nullptr);  // 其它格式不需要参数，传 nullptr
    }

    if (st != Ok)
    {
        err = L"保存文件失败（目标文件可能被其它程序占用，或没有写入权限）。";  // 常见原因：文件被占用或没有权限
        return false;
    }
    return true;
}

// 处理成功后的统一收尾：弹一个“完成”对话框，并可选地用资源管理器定位到输出文件。
//   参数 owner    ：父窗口；
//   参数 outPath  ：刚写出的文件路径；
//   参数 extraLine：追加的一行说明（比如“输出尺寸：1280 × 720 像素”）。
// 细节：
//   · msg[1400] 是固定栈缓冲，配合 swprintf_s 的边界检查，处理的是本地短路径不会溢出；
//   · 父窗口优先用 g_hMain，其次才用传入的 owner（两者都可能是空，MessageBox 允许空）；
//   · 用户点“是”才打开；用 ShellExecuteW 启动 explorer.exe，参数写 /select,"..."，
//     效果是打开文件夹并【选中】该文件，比只打开目录更好用；
//   · 路径必须用双引号包起来，否则带空格的路径会被 explorer 当成多个参数；
//   · 这里其实没有用 /select 之外的 ShellExecute 特性，所以直接起 explorer 最稳。
static void ShowDoneAndOpenFolder(HWND owner, const std::wstring& outPath, const std::wstring& extraLine)
{
    wchar_t msg[1400];
    swprintf_s(msg, L"处理完成！\r\n\r\n输出文件：\r\n%s\r\n\r\n%s\r\n是否打开输出文件所在的文件夹？",
               outPath.c_str(), extraLine.c_str());  // 第二个 %s 是调用方传的补充说明

    if (MessageBoxW(g_hMain ? g_hMain : owner, msg, kCompleteTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES)
    {
        std::wstring arg = L"/select,\"" + outPath + L"\"";  // /select 让资源管理器打开并选中该文件
        ShellExecuteW(owner, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

// 对外主入口：把处理好的位图按“原图同格式 + 后缀”保存，并接管位图的生命周期。
//   参数 bmp     ：要保存的位图 —— 【本函数内部会 delete，调用者不要再删】；
//   参数 suffix  ：文件名后缀，如 L"_zoom" / L"_adjust"；
//   参数 owner   ：弹窗用的父窗口；
//   参数 donePath：可空；不为空时写出最终路径，方便调用者做后续动作。
//   返回：成功 true / 失败 false。
//
// 所有失败路径都已考虑 delete bmp，所以调用者无论成功失败都【不要】再 delete 一次，
// 否则就是二次释放（double free），这是 C++ 里最典型的崩溃原因。
// 输出路径由“原目录 + 原主名 + 后缀 + 原格式扩展名”拼成，因此不会影响原文件。
// 覆盖确认、写盘、状态栏文字、完成提示都在这里统一处理，避免各页面各写一套。
bool SaveBitmapWithSuffix(Bitmap* bmp, const std::wstring& suffix, HWND owner, std::wstring* donePath)  // 内部会接管 bmp 的生命周期：调用者不要再 delete
{
    if (!bmp)  // 空指针直接失败
        return false;

    const OutFmt* fmt = SourceFmt();
    std::wstring outPath = DirNameOf(g_srcPath) + StemOf(g_srcPath) + suffix + fmt->ext;  // 与原图同目录、同名，仅多一个后缀

    if (FileExists(outPath))
    {
        wchar_t q[1024];
        swprintf_s(q, L"输出文件已存在：\r\n%s\r\n\r\n是否覆盖？", outPath.c_str());
        if (MessageBoxW(g_hMain ? g_hMain : owner, q, L"确认覆盖", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            delete bmp;
            SetAppStatus(L"已取消：输出文件已存在。");
            return false;
        }
    }

    const int w = bmp->GetWidth();
    const int h = bmp->GetHeight();

    std::wstring err;
    if (!SaveBitmapTo(outPath, fmt, bmp, err))
    {
        delete bmp;
        MessageBoxW(g_hMain ? g_hMain : owner, err.c_str(), L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：" + err);
        return false;
    }
    delete bmp;

    if (donePath) *donePath = outPath;
    SetAppStatus(L"处理完成：" + outPath + L"  (" + IntToStr(w) + L" × " + IntToStr(h) + L")");

    wchar_t extra[256];
    swprintf_s(extra, L"输出尺寸：%d × %d 像素\r\n", w, h);  // 拼出给用户的最终行：路径 + 实际尺寸
    ShowDoneAndOpenFolder(g_hMain ? g_hMain : owner, outPath, extra);
    return true;
}

//   GDI+ 没有 ICO 编码器，所以 ICO 只能手工拼字节。ICO 文件结构很简单：
//     ① ICONDIR  （6 字节：保留位 + 类型 + 条目数）
//     ② ICONDIRENTRY × N（每条 16 字节：尺寸、色数、色深、数据长度、数据偏移）
//     ③ 各条目的图像数据本身（这里 256 用 PNG，其余用裸 BMP）
//   所有整数都是【小端序】，所以下面的 PutU16/PutU32 要一个字节一个字节地放。
// ---------------------------------------------------------------------------
// ICO 写出
// ---------------------------------------------------------------------------
// 往字节缓冲区追加一个 16 位整数（小端序：先低字节、后高字节）。
//   参数 v   ：要被写入的缓冲区（按引用传，直接 push_back 扩容）；
//   参数 x   ：要写入的值。
// 为什么手动拆字节：ICO 属于“文件格式”，规定就是小端；
//   直接往缓冲区里 memcpy 一个 WORD 在 x86 上虽然也是小端，
//   但那种写法可移植性差、字节顺序也不直观，显式拆分更清楚。
static void PutU16(std::vector<BYTE>& v, WORD x)
{
    v.push_back((BYTE)(x & 0xFF));
    v.push_back((BYTE)((x >> 8) & 0xFF));
}

// 往字节缓冲区追加一个 32 位整数（小端序），同 PutU16 的思路，多两个字节。
//   & 0xFF 之后再转 BYTE，本质是“取最低 8 位”，防止高位移入造成意外值。
static void PutU32(std::vector<BYTE>& v, DWORD x)
{
    v.push_back((BYTE)(x & 0xFF));
    v.push_back((BYTE)((x >> 8) & 0xFF));
    v.push_back((BYTE)((x >> 16) & 0xFF));
    v.push_back((BYTE)((x >> 24) & 0xFF));
}

// 32bpp DIB（自下而上）+ AND 掩码
// 把位图打包成 ICO 内部的 32bpp DIB 数据块（自下而上的 XOR 位图 + AND 掩码）。
//   参数 bmp ：源位图（要求至少 size×size，只读）；
//   参数 size：条目边长（16/24/32/48/64/128 用本函数；256 走 PNG 分支）；
//   参数 out ：输出缓冲区，函数内部 assign 成完整大小。
//   返回：成功 true / 失败 false（LockBits 失败时）。
//
// 为什么高度写 size*2：ICO 里的 BMP 是“XOR 图 + AND 掩码”上下叠在一起的，
//   头部 biHeight 要填【两者之和】，Windows 靠这个值来判断掩码从哪里开始。
//   biBitCount=32 表示每像素 4 字节（BGRA），不用调色板；
//   biSizeImage 只算 XOR 图像本身的字节数，不含掩码。
//
// 为什么必须上下翻转：GDI+ 的 Scan0 指向【顶行】（自上而下），
//   而 DIB 规定第一行是【底行】（自下而上），所以源行号要取 size-1-y。
//   这是内存图像转文件格式最容易出错的一处，弄反了图标会上下颠倒。
//
// AND 掩码的作用：1 位表示透明，0 位表示不透明；每行按 4 字节对齐
//   （(size+31)/32*4 就是“每行位数字节数再补齐到 4 的倍数”）。
//   在 32bpp 下 Windows 优先用 alpha 通道，掩码只是老系统的兼容兜底，
//   但规范要求它存在，所以还是要正确填写。
//   位序：最高位对应最左边的像素，所以是 0x80 >> (x & 7)。
// 阈值取 128：alpha < 128 视为透明；半透明像素在这里只能二选一，
//   这是 ICO 掩码本身的限制，真正的半透明信息保留在 XOR 图的 alpha 里。
static bool BuildBmpEntry(Bitmap* bmp, int size, std::vector<BYTE>& out)
{
    Rect rc(0, 0, size, size);
    BitmapData data;
    if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok)  // 只读锁定，本函数不修改源图
        return false;

    const int pixBytes = size * size * 4;
    const int maskStride = ((size + 31) / 32) * 4;  // 每行掩码字节数并补齐到 4 的倍数
    out.assign(sizeof(BITMAPINFOHEADER) + (size_t)pixBytes + (size_t)maskStride * size, 0);

    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)out.data();
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = size;
    bih->biHeight = size * 2;      // XOR + AND  // XOR 图 + AND 掩码，所以高度是两倍
    bih->biPlanes = 1;
    bih->biBitCount = 32;  // 32bpp：每像素 4 字节 BGRA，无需调色板
    bih->biCompression = BI_RGB;
    bih->biSizeImage = pixBytes;

    BYTE* pix = out.data() + sizeof(BITMAPINFOHEADER);  // 像素数据紧跟在头部之后
    BYTE* mask = pix + pixBytes;  // 掩码紧跟在像素数据之后

    for (int y = 0; y < size; ++y)
    {
        const BYTE* src = (const BYTE*)data.Scan0 + (size_t)(size - 1 - y) * data.Stride;  // 上下翻转  // DIB 是自下而上存的，这里要翻转行序
        BYTE* dst = pix + (size_t)y * size * 4;
        memcpy(dst, src, (size_t)size * 4);

        for (int x = 0; x < size; ++x)
        {
            if (src[(size_t)x * 4 + 3] < 128)  // 1 位 = 透明
                mask[(size_t)y * maskStride + (x >> 3)] |= (BYTE)(0x80 >> (x & 7));  // 最高位对应最左像素
        }
    }

    bmp->UnlockBits(&data);
    return true;
}

// 用 PNG 编码器把位图写进一块内存（不落盘），用于 256×256 的 ICO 条目。
//   参数 bmp：源位图；
//   参数 out：输出字节数组。
//   返回：成功 true / 失败 false。
// 为什么做一个“内存流”而不是直接写文件：
//   icos 最终要一次性写成一个文件，各条目先各自编码好再整体拼接，
//   这样才能正确填写每个条目的长度和偏移。
// 关键 API 与坑：
//   · CreateStreamOnHGlobal(nullptr, TRUE, &pStream)：第一个参数为 nullptr 表示
//     “我需要一块会自动增长的内存”，写多少它自己扩多少；
//   · Image::Save 提供了接受 IStream* 的重载版本，用法与文件名版本一致；
//   · 写完必须 pStream->Stat() 才能知道总字节数，GDI+ 不会告诉你写了多少；
//   · 读回数据前要 Seek 到开头：流的读写位置刚写完时在末尾，直接 Read 会读到 0 字节；
//   · 64 MB 的上限是给异常情况兜底的（正常 256×256 的 PNG 只有几十 KB），
//     万一编码器返回了荒谬的尺寸，也不会让下面的 resize 把内存吃光；
//   · pStream->Read 的第三个参数是 ULONG，而 out.size() 是 size_t，
//     大端平台上会截断，但这里 64 MB 上限保证不会超；
//   · 函数所有分支最后都会执行 pStream->Release()（COM 引用计数，必须配对）。
static bool EncodePngToMemory(Bitmap* bmp, std::vector<BYTE>& out)
{
    CLSID clsid = {};
    if (!GetEncoderClsid(L"image/png", &clsid))
        return false;

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)))  // nullptr：只要内存流，不要文件
        return false;

    bool ok = (bmp->Save(pStream, &clsid, nullptr) == Ok);
    if (ok)
    {
        STATSTG st = {};
        if (SUCCEEDED(pStream->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart > 0 &&  // 只有写完后 Stat 才知道总长度
            st.cbSize.QuadPart < (ULONGLONG)64 * 1024 * 1024)
        {  // 64 MB 上限，正常远小于此
            out.resize((size_t)st.cbSize.QuadPart);
            LARGE_INTEGER zero = {};
            pStream->Seek(zero, STREAM_SEEK_SET, nullptr);  // 必须 Seek 回开头再读
            ULONG got = 0;
            if (FAILED(pStream->Read(out.data(), (ULONG)out.size(), &got)) || got != out.size())
                ok = false;
        }
        else
        {
            ok = false;
        }
    }
    pStream->Release();
    return ok;
}

// 把一张正方形位图导出成多尺寸的 .ico 文件。
//   参数 square  ：已经裁好的正方形源图（边长至少是最大勾选尺寸）——
//                  【本函数内部会 delete，调用者不要再删】；
//   参数 sizes   ：要包含的尺寸列表（来自 kIcoSizes，用户勾选的）；
//   参数 owner   ：弹窗父窗口；
//   参数 donePath：可空，成功后写出最终路径。
//   返回：成功 true / 失败 false。
// 输出名固定是 原名 + "_ico.ico"，与原图同目录。
// 尺寸分支的规则来自 Windows 的实际兼容性：
//   ≥256 的条目必须用 PNG 压缩（BMP 形式在 Vista 以后才认，PNG 体积小得多）；
//   小于 256 的用传统 32bpp DIB，兼容性最好（老程序也认）。
// 注意每个尺寸都是 new 一张新位图（ResizeBitmap），用完立刻 delete，
//   失败就 continue 跳过该尺寸，不让整个导出失败。
bool SaveIcoWithSuffix(Bitmap* square, const std::vector<int>& sizes, HWND owner, std::wstring* donePath)  // 内部会接管 square 的生命周期：调用者不要再 delete
{
    if (!square)  // 空指针直接失败
        return false;

    if (sizes.empty())
    {
        delete square;
        MessageBoxW(g_hMain ? g_hMain : owner, L"请至少勾选一个 ICO 尺寸。", L"提示", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    std::wstring outPath = DirNameOf(g_srcPath) + StemOf(g_srcPath) + L"_ico.ico";

    if (FileExists(outPath))
    {
        wchar_t q[1024];
        swprintf_s(q, L"输出文件已存在：\r\n%s\r\n\r\n是否覆盖？", outPath.c_str());
        if (MessageBoxW(g_hMain ? g_hMain : owner, q, L"确认覆盖", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            delete square;
            SetAppStatus(L"已取消：输出文件已存在。");
            return false;
        }
    }

    struct Entry { int size; std::vector<BYTE> data; };
    std::vector<Entry> entries;

    for (size_t i = 0; i < sizes.size(); ++i)
    {
        const int sz = sizes[i];
        std::wstring err;
        Bitmap* one = ResizeBitmap(square, sz, sz, err);  // 每个尺寸都是新建的位图，用完即删
        if (!one)
            continue;  // 单个尺寸失败不影响其它尺寸

        Entry e;
        e.size = sz;
        bool ok = (sz >= 256) ? EncodePngToMemory(one, e.data) : BuildBmpEntry(one, sz, e.data);  // 256 用 PNG，更小的用 DIB
        delete one;

        if (ok && !e.data.empty())
            entries.push_back(e);
    }
    delete square;

    if (entries.empty())
    {
        MessageBoxW(g_hMain ? g_hMain : owner, L"生成 ICO 数据失败。", L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：ICO 数据为空。");
        return false;
    }

    // ---- 组装 ICO ----
    std::vector<BYTE> head;
    PutU16(head, 0);                       // reserved  // 保留字段，必须为 0
    PutU16(head, 1);                       // type = icon  // 1 = 图标（2 是光标）
    PutU16(head, (WORD)entries.size());

    DWORD offset = (DWORD)(6 + 16 * entries.size());  // 各条目数据从目录区之后开始，6 是 ICONDIR 长度
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Entry& e = entries[i];
        head.push_back((BYTE)((e.size >= 256) ? 0 : e.size));   // 宽（0 表示 256）  // 宽：0 表示 256
        head.push_back((BYTE)((e.size >= 256) ? 0 : e.size));   // 高  // 高：同上
        head.push_back(0);                                      // 调色板数
        head.push_back(0);                                      // reserved
        PutU16(head, 1);                                        // planes  // 颜色平面数，固定 1
        PutU16(head, 32);                                       // bpp  // 位深，写 32 表示带 alpha
        PutU32(head, (DWORD)e.data.size());
        PutU32(head, offset);
        offset += (DWORD)e.data.size();
    }

    bool ok = true;
    std::wstring err;
    HANDLE hf = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
    {
        ok = false;
        err = L"无法创建 ICO 文件（可能没有写入权限）。";
    }
    else
    {
        DWORD written = 0;
        if (!WriteFile(hf, head.data(), (DWORD)head.size(), &written, nullptr) || written != head.size())  // 先写目录
            ok = false;
        for (size_t i = 0; ok && i < entries.size(); ++i)
        {
            if (!WriteFile(hf, entries[i].data.data(), (DWORD)entries[i].data.size(), &written, nullptr) ||
                written != entries[i].data.size())
                ok = false;
        }
        CloseHandle(hf);
        if (!ok)
        {
            DeleteFileW(outPath.c_str());
            err = L"写入 ICO 文件失败。";
        }
    }

    if (!ok)
    {
        MessageBoxW(g_hMain ? g_hMain : owner, err.c_str(), L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：" + err);
        return false;
    }

    std::wstring list;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (i) list += L", ";
        list += IntToStr(entries[i].size);
    }

    if (donePath) *donePath = outPath;
    SetAppStatus(L"处理完成：" + outPath + L"  (" + list + L")");

    wchar_t extra[512];
    swprintf_s(extra, L"包含尺寸：%s 像素\r\n", list.c_str());
    ShowDoneAndOpenFolder(g_hMain ? g_hMain : owner, outPath, extra);
    return true;
}

//   都是“给一个 HDC 和一块矩形，把东西画上去”的纯绘制函数，不持有任何状态，
//   被各功能页在 WM_PAINT 里调用。
//   注意点：GDI 句柄（HBRUSH/HDC/HBITMAP/HFONT…）都要自己 Delete/释放。
// ---------------------------------------------------------------------------
// 绘制辅助
// ---------------------------------------------------------------------------
// 绘制“透明棋盘格”背景（灰白相间的小方格），用来示意哪些像素是透明的。
//   参数 hdc：目标设备上下文；
//   参数 rc ：要填充的矩形（使用其中的宽高，坐标按 0,0 处理）。
// 实现要点：
//   · 先整块刷白，再只画灰格 —— 比每个格子判断颜色要少一半的 FillRect 调用；
//   · 用 (x/cell + y/cell) & 1 判断奇偶，得到棋盘交错（& 1 就是取最低位）；
//   · 每个格子都要夹取右/下边界（(x+cell<w)?x+cell:w），否则最后一行一列会画到
//     控件外面去，那种越界绘制在同一个 DC 上会污染相邻控件；
//   · 自己 CreateSolidBrush 出来的两个画刷必须 DeleteObject，GDI 对象是有限资源，
//     泄漏多了会导致整个程序绘制失败（GDI 句柄配额耗尽）。
// 提醒：FillRect 对“画刷”没有所有权，画完才删是安全的。
void PaintCheckerBackground(HDC hdc, const RECT& rc)
{
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HBRUSH bWhite = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH bGray = CreateSolidBrush(RGB(237, 239, 241));
    RECT all = { 0, 0, w, h };
    FillRect(hdc, &all, bWhite);

    const int cell = 10;
    for (int y = 0; y < h; y += cell)
    {
        for (int x = 0; x < w; x += cell)
        {
            if (((x / cell) + (y / cell)) & 1)
            {
                RECT c;
                c.left = x;
                c.top = y;
                c.right = (x + cell < w) ? (x + cell) : w;
                c.bottom = (y + cell < h) ? (y + cell) : h;
                FillRect(hdc, &c, bGray);
            }
        }
    }

    DeleteObject(bWhite);
    DeleteObject(bGray);
}

// 在指定矩形里画一张“等比缩放居中”的预览图；没有图片就画一句灰色提示文字。
//   参数 hdc ：目标 DC；
//   参数 rc  ：预览区矩形（函数内部把坐标平移到 0,0 处理）；
//   参数 img ：要显示的位图，可为 nullptr；
//   参数 hint：无图时显示的文字，为空时用默认的“请先载入图片”。
// 为什么要先画到内存 DC（mem）再一次 BitBlt 贴过去：
//   棋盘格 + 图片 + 边框要分很多步画，直接画到屏幕 DC 上会看到明显的闪烁。
//   先在离屏位图上画完整幅再一次性拷贝，视觉上就是“唰”地出现，没有中间过程。
// 这是 Win32 时代最经典的“双缓冲”做法（今天也可以改用 GDI+ 或 DWM 的缓冲）。
// GDI 资源的顺序要看清：
//   SelectObject 之前要把【原来的位图】存下来，最后再换回去；
//   换回去之后才能 DeleteObject(bmp)，否则位图还选在 DC 里就删除是未定义行为。
//   同理，DeleteDC 必须在该 DC 里不选任何自建对象之后执行。
// 缩放计算：aw/ah 是去掉 8 像素内边距后的可用区域（至少留 1 像素），
//   取 sx、sy 中较小者保证等比且完整显示（不会裁掉）。
//   “+0.5 再取整”是四舍五入；居中靠 (w-dw)/2.0 得到浮点偏移。
//   注意循环里的 `int sy` 是同一个名字的另一个局部变量（这里没有歧义）。
// 一个隐蔽坑：Graphics g(mem) 这个 GDI+ 对象的作用域被限制在一对花括号里，
//   这是刻意的 —— 它必须在下面的 BitBlt/DeleteDC 之前析构，
//   否则 GDI+ 还会占着这个 HDC，之后 DeleteDC 会失败或行为异常。
// 无图分支里换字体也要记着换回来（SelectObject 返回旧对象），否则 mem 上会残留。
// 最后：BitBlt 把整块离屏内容拷到目标 DC，SRCCOPY 表示直接覆盖。
void PaintImagePreview(HDC hdc, const RECT& rc, Bitmap* img, const wchar_t* hint)
{
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC mem = CreateCompatibleDC(hdc);  // 离屏 DC，用于双缓冲
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);  // 与目标同尺寸的离屏位图
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT all = { 0, 0, w, h };
    PaintCheckerBackground(mem, all);  // 先铺棋盘格（Windows 上灰白代表透明）

    if (img && img->GetWidth() > 0 && img->GetHeight() > 0)
    {
        Graphics g(mem);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        const int pad = 8;  // 留 8 像素内边距
        int aw = w - pad * 2;  if (aw < 1) aw = 1;
        int ah = h - pad * 2;  if (ah < 1) ah = 1;

        int iw = (int)img->GetWidth();
        int ih = (int)img->GetHeight();
        double s = (double)aw / iw;  // 按可用区域等比缩放
        double sy = (double)ah / ih;
        if (sy < s) s = sy;  // 取较小缩放比，保证完整显示

        int dw = (int)(iw * s + 0.5);  if (dw < 1) dw = 1;
        int dh = (int)(ih * s + 0.5);  if (dh < 1) dh = 1;

        REAL x = (REAL)((w - dw) / 2.0);  // 居中偏移，可能是 .5 像素，所以用 REAL
        REAL y = (REAL)((h - dh) / 2.0);
        g.DrawImage(img, RectF(x, y, (REAL)dw, (REAL)dh), 0, 0, (REAL)iw, (REAL)ih, UnitPixel);
    }
    else
    {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(110, 116, 124));
        HGDIOBJ oldFont = SelectObject(mem, g_hUiFont);
        DrawTextW(mem, hint ? hint : L"请先载入图片", -1, &all,  // -1 表示 hint 以 \0 结尾，让 DrawTextW 自己算长度
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(mem, oldFont);
    }

    HBRUSH bBorder = CreateSolidBrush(RGB(205, 208, 212));
    FrameRect(mem, &all, bBorder);  // 外框：FrameRect 用画刷画 1 像素边框
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    DeleteObject(bBorder);
    SelectObject(mem, oldBmp);  // 先把原位图选回来，才能安全删除自建位图
    DeleteObject(bmp);  // 删除前必须确保它已不在任何 DC 里
    DeleteDC(mem);
}

// 画一个小色块，作为“当前颜色”的预览。
//   参数 hdc        ：目标 DC；
//   参数 rc         ：色块矩形；
//   参数 c          ：要显示的颜色（COLORREF）；
//   参数 transparent：true 表示当前选的是“透明”，此时画棋盘格而不是纯色 ——
//                     用棋盘格表达透明是图像软件的通例，用户一看就懂。
// 无论哪种情况最后都画一圈灰边框，让色块在浅色背景上也有轮廓。
// 三个分支各自 Create 的画刷都用完即删，没有跨调用的资源。
void PaintColorSwatch(HDC hdc, const RECT& rc, COLORREF c, bool transparent)
{
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (transparent)
    {
        PaintCheckerBackground(hdc, rc);
    }
    else
    {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(hdc, &rc, b);
        DeleteObject(b);
    }

    HBRUSH bBorder = CreateSolidBrush(RGB(160, 164, 170));
    FrameRect(hdc, &rc, bBorder);
    DeleteObject(bBorder);
}

//   这一组函数回答两个问题：
//     ① 这台机器上到底能存成哪些格式？（ListImageEncoders）
//     ② 存成某个格式大概多大、想限定体积该怎么调画质？（EncodeToMemory 系列）
//   思路是“先在内存里编码一遍量一量”，而不是先落盘再删掉 —— 快且不留垃圾文件。
// ---------------------------------------------------------------------------
// 编码器枚举 / 体积预估 / 指定格式保存（压缩页、格式转换页使用）
// ---------------------------------------------------------------------------
// 把字节数格式化成人类可读的字符串（如 L"1.25 MB" / L"320.0 KB" / L"512 B"）。
//   参数 bytes：字节数（size_t）。
// 单位分三档：≥1 MB 用 MB、≥1 KB 用 KB、其余用 B，数值一律按 1024 进制。
// 这里用的是 1024 而非 1000（即 KiB/MiB 的算法），和资源管理器显示的口径一致，
//   但严格说标成 MB 并不严谨 —— 作为显示用途可以接受，改口径前先想想用户会不会困惑。
// 注意 (size_t)1024*1024 这个写法：先转 size_t 再乘，防止 int 在 32 位下算大数时溢出。
std::wstring FormatBytes(size_t bytes)
{
    wchar_t buf[64];
    if (bytes >= (size_t)1024 * 1024)
        swprintf_s(buf, L"%.2f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        swprintf_s(buf, L"%.1f KB", (double)bytes / 1024.0);
    else
        swprintf_s(buf, L"%u B", (unsigned)bytes);
    return buf;
}

// 列出“本机 GDI+ 实际支持的图片编码器”，顺序按下面的 wanted 表来。
//   无参数；返回 EncoderChoice 数组（name 显示名 / mime / 带点扩展名）。
// 为什么要过滤而不是把系统里的编码器全列出来：
//   系统里可能有一些用户根本不认识的编码器（相机厂商等），
//   这里只挑 Pic246 明确支持的 6 种，界面下拉框才干净。
// 双层循环的意图：外层遍历 wanted（保证按固定顺序输出），内层在系统编码器里找匹配。
//   一旦命中就 break，避免同一种格式被重复加入（有些系统装了多个同 MIME 的编码器）。
// 注意 WebP：绝大多数 Windows 默认不带 WebP 编码器，
//   wanted 里有它不代表列表里就一定有它 —— 这正说明了“必须运行时枚举”的必要性，
//   界面应该依据这个返回值来填充下拉框，而不是写死格式列表。
// 两次 GetImageEncoders 的“先问大小再取数据”套路与 GetEncoderClsid 完全相同。
std::vector<EncoderChoice> ListImageEncoders()
{
    std::vector<EncoderChoice> out;

    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0)
        return out;

    std::vector<BYTE> buf(size);
    ImageCodecInfo* pInfo = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(num, size, pInfo) != Ok)
        return out;

    struct Wanted { const wchar_t* mime; const wchar_t* name; const wchar_t* ext; };
    static const Wanted wanted[] =
    {
        { L"image/png",  L"PNG",  L".png"  },
        { L"image/jpeg", L"JPEG", L".jpg"  },
        { L"image/bmp",  L"BMP",  L".bmp"  },
        { L"image/gif",  L"GIF",  L".gif"  },
        { L"image/tiff", L"TIFF", L".tif"  },
        { L"image/webp", L"WebP", L".webp" },
    };

    for (int w = 0; w < (int)_countof(wanted); ++w)
    {
        for (UINT i = 0; i < num; ++i)
        {
            if (_wcsicmp(pInfo[i].MimeType, wanted[w].mime) == 0)
            {
                EncoderChoice c;
                c.name = wanted[w].name;
                c.mime = wanted[w].mime;
                c.ext = wanted[w].ext;
                out.push_back(c);
                break;
            }
        }
    }
    return out;
}

// 判断这个 MIME 是不是“有损”格式（有损才需要、也才支持画质参数）。
//   JPEG 是最典型的有损格式；WebP 也支持有损/无损两种模式，这里按有损处理。
//   PNG/BMP/GIF/TIFF 都是无损的，传画质参数没有意义，所以调用点都先问这个函数。
// 返回 bool；比较用 _wcsicmp 忽略大小写，因为调用方传进来的写法可能不统一。
static bool IsLossyMime(const std::wstring& mime)
{
    return _wcsicmp(mime.c_str(), L"image/jpeg") == 0 || _wcsicmp(mime.c_str(), L"image/webp") == 0;
}

// 把图片编码到内存里，返回字节数（可选地把数据也带出来）。
//   参数 bmp    ：源图（Image*，只读）；
//   参数 mime   ：目标格式的 MIME；
//   参数 quality：有损格式的画质 1..100（无损格式会被忽略）；
//   参数 outData：可空；非空且成功时写入编码后的字节。
//   返回：编码后的字节数；失败返回 0。
// 这是本文件里被调用最频繁的“测量工具”：
//   EstimateEncodedSize 直接用它来估体积（outData 传 nullptr，只要大小），
//   FindQualityForTargetSize 用它做二分（也只要大小）。
//   outData 传 nullptr 时它依然会完整编码一遍 —— 因为想知道大小就只能真编码，
//   GDI+ 没有“预测 JPEG 大小”的接口；这也是压缩页估算会稍微耗时的原因。
// 与 SaveBitmapTo 里相同的 EncoderQuality 四件套在这里再次出现，写法必须一致。
// 读回数据的顺序：Stat 取长度 -> resize 缓冲 -> Seek 回开头 -> Read -> 校验读满；
//   少任何一步都会得到 0 字节或半截数据。
// 最后无论成功失败都 Release 流，保证不泄漏 COM 引用。
static size_t EncodeToMemory(Image* bmp, const std::wstring& mime, ULONG quality, std::vector<BYTE>* outData)
{
    if (!bmp) return 0;

    CLSID clsid = {};
    if (!GetEncoderClsid(mime.c_str(), &clsid))
        return 0;

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)))
        return 0;

    bool ok = false;
    if (IsLossyMime(mime))
    {
        ULONG q = quality;
        EncoderParameters ep;
        ep.Count = 1;
        ep.Parameter[0].Guid = EncoderQuality;
        ep.Parameter[0].Type = EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Value = &q;
        ok = (bmp->Save(pStream, &clsid, &ep) == Ok);
    }
    else
    {
        ok = (bmp->Save(pStream, &clsid, nullptr) == Ok);
    }

    size_t n = 0;
    if (ok)
    {
        STATSTG st = {};
        if (SUCCEEDED(pStream->Stat(&st, STATFLAG_NONAME)))
            n = (size_t)st.cbSize.QuadPart;

        if (outData && n > 0)
        {
            outData->resize(n);
            LARGE_INTEGER zero = {};
            pStream->Seek(zero, STREAM_SEEK_SET, nullptr);  // 回到开头才能读到刚写入的数据
            ULONG got = 0;
            if (FAILED(pStream->Read(outData->data(), (ULONG)n, &got)) || got != n)
            {
                outData->clear();
                n = 0;
            }
        }
    }
    pStream->Release();
    return n;
}

// 预估“按当前格式和质量保存会有多大”。
//   参数 bmp/mime/quality 含义同 EncodeToMemory；
//   返回字节数（0 表示编码失败）。
// 实现就是把 outData 传 nullptr —— 数据不取，只留长度，省一次内存拷贝。
// 界面里滑动画质滑块后实时刷新“预计大小”就是用它（注意别在拖动中每帧都调用，
//   大图上单次编码可能几十毫秒，拖动会卡；实际有个防抖或松手才更新更好）。
size_t EstimateEncodedSize(Image* bmp, const std::wstring& mime, ULONG quality)
{
    return EncodeToMemory(bmp, mime, quality, nullptr);
}

// 二分查找：在“JPEG 且体积不超过目标”的前提下，找出尽可能高的画质。
//   参数 bmp        ：源图；
//   参数 targetBytes：目标体积上限（字节）；
//   参数 actualBytes：可空；输出最终选定的画质实际会生成多大。
//   返回：画质 10..100（一定大于 0）。
// 为什么用二分而不是从 100 往下线性试：
//   画质与体积大致单调（画质越高、文件越大），满足二分的前提；
//   线性可能要试 90 次，二分只要约 7 次，而每次都要真编码一遍，差距非常明显。
// 边界的两个安排：
//   · 下界取 10 而不是 1：画质个位数时 JPEG 会糊成一片、块效应严重，
//     与其给用户一张不能看的图，不如允许“达不到目标体积”；
//   · 如果连 10 都超出目标（best 仍为 0），退而求其次固定用 10 并如实报告实际大小，
//     由界面提示用户“目标体积过小，已使用最低画质”。
// sz == 0 表示编码失败，此时往小（hi = mid-1）方向收缩继续试，避免死循环。
// 注意每次迭代都是一次完整编码，所以这个函数在大图上会花费可观的时间，
//   应在 UI 上给出等待提示、不要在快速重复触发的地方调用。
int FindQualityForTargetSize(Image* bmp, size_t targetBytes, size_t* actualBytes)
{
    if (actualBytes) *actualBytes = 0;
    if (!bmp || targetBytes == 0) return 0;

    int best = 0;
    size_t bestSize = 0;

    // 二分找“不超过目标体积”的最高画质
    int lo = 10, hi = 100;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        size_t sz = EncodeToMemory(bmp, L"image/jpeg", (ULONG)mid, nullptr);
        if (sz == 0) { hi = mid - 1; continue; }  // 编码失败：往更小画质方向收缩
        if (sz <= targetBytes) { best = mid; bestSize = sz; lo = mid + 1; }  // 体积达标，记下来并尝试更高画质
        else hi = mid - 1;
    }  // 体积超标，降低画质

    if (best == 0)
    {
        best = 10;
        bestSize = EncodeToMemory(bmp, L"image/jpeg", 10, nullptr);
    }

    if (actualBytes) *actualBytes = bestSize;
    return best;
}

// 取当前源文件在磁盘上的字节数（用于显示压缩前后对比、“节省了百分之多少”）。
//   无参数；返回字节数，取不到时返回 0。
// 为什么不用 GetFileSize：那需要一个已打开的句柄，而这里我们只想“查一下”；
//   GetFileAttributesExW 一次调用就能拿到属性（含大小），也不用打开文件。
// 大小是拆成高低两个 DWORD 返回的，必须拼成 64 位再转 size_t：
//   (ULONGLONG)nFileSizeHigh << 32 | nFileSizeLow，写反/漏掉会得到错误的小数值。
size_t SourceFileSize()
{
    if (g_srcPath.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(g_srcPath.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (size_t)(((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);  // 高低 32 位合成 64 位文件大小
}

// 与 SourceFileSize 逻辑相同，只是路径由参数给出（用来量刚写出的输出文件）。
//   参数 path：要查询的文件路径；返回字节数，失败返回 0。
// 单独写一个而不是复用 SourceFileSize：后者写死了读全局 g_srcPath，语义不一样。
static size_t FileSizeOf(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (size_t)(((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);  // 同上，这里换成参数指定的路径
}

// 对外主入口：把位图保存成“指定格式 + 后缀”（压缩页、格式转换页用）。
//   参数 bmp    ：要保存的位图 —— 【本函数内部会 delete，调用者不要再删】；
//   参数 suffix ：文件名后缀；
//   参数 mime   ：目标格式 MIME；
//   参数 ext    ：目标扩展名（带点，由调用方从编码器列表里带过来）；
//   参数 quality：有损格式画质 1..100（无损格式忽略）；
//   参数 owner  ：弹窗父窗口。
//   返回：成功 true / 失败 false。
// 与 SaveBitmapWithSuffix 的区别：那个保持原格式，这个可以换格式（于是扩展名也换），
//   因此路径拼接用参数 ext 而不是从源格式表里查。
// 成功后会用 SourceFileSize/FileSizeOf 算压缩比，把“原大小 → 新大小（百分比）”
//   显示在完成提示里，这就是压缩功能最直观的反馈。
// 同样地：所有失败分支和成功分支都已经 delete 过 bmp，调用者不要再删。
bool SaveBitmapAsFormat(Bitmap* bmp, const std::wstring& suffix, const std::wstring& mime,  // 内部会接管 bmp 的生命周期：调用者不要再 delete
                        const std::wstring& ext, ULONG quality, HWND owner)
{
    if (!bmp) return false;  // 空指针直接失败

    std::wstring outPath = DirNameOf(g_srcPath) + StemOf(g_srcPath) + suffix + ext;

    if (FileExists(outPath))
    {
        wchar_t q[1024];
        swprintf_s(q, L"输出文件已存在：\r\n%s\r\n\r\n是否覆盖？", outPath.c_str());
        if (MessageBoxW(g_hMain ? g_hMain : owner, q, L"确认覆盖", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            delete bmp;
            SetAppStatus(L"已取消：输出文件已存在。");
            return false;
        }
    }

    CLSID clsid = {};
    if (!GetEncoderClsid(mime.c_str(), &clsid))
    {
        delete bmp;
        MessageBoxW(g_hMain ? g_hMain : owner, L"系统中找不到对应的图片编码器，无法保存。", L"处理失败",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    const int w = bmp->GetWidth();
    const int h = bmp->GetHeight();

    if (FileExists(outPath))
        DeleteFileW(outPath.c_str());  // 先删除已存在的文件，避免旧内容残留

    Status st;
    if (IsLossyMime(mime))
    {
        ULONG q = quality;
        EncoderParameters ep;
        ep.Count = 1;
        ep.Parameter[0].Guid = EncoderQuality;
        ep.Parameter[0].Type = EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Value = &q;
        st = bmp->Save(outPath.c_str(), &clsid, &ep);  // 有损格式才传 EncoderQuality
    }
    else
    {
        st = bmp->Save(outPath.c_str(), &clsid, nullptr);
    }
    delete bmp;

    if (st != Ok)
    {
        std::wstring err = L"保存文件失败（目标文件可能被其它程序占用，或没有写入权限）。";
        MessageBoxW(g_hMain ? g_hMain : owner, err.c_str(), L"处理失败", MB_OK | MB_ICONERROR);
        SetAppStatus(L"处理失败：" + err);
        return false;
    }

    const size_t outSize = FileSizeOf(outPath);
    const size_t srcSize = SourceFileSize();

    SetAppStatus(L"处理完成：" + outPath + L"  (" + FormatBytes(outSize) + L")");

    wchar_t extra[512];
    if (srcSize > 0 && outSize > 0)
        swprintf_s(extra, L"输出尺寸：%d × %d 像素\r\n文件大小：%s → %s（%.0f%%）\r\n",
                   w, h, FormatBytes(srcSize).c_str(), FormatBytes(outSize).c_str(),
                   100.0 * (double)outSize / (double)srcSize);
    else
        swprintf_s(extra, L"输出尺寸：%d × %d 像素\r\n文件大小：%s\r\n",
                   w, h, FormatBytes(outSize).c_str());

    ShowDoneAndOpenFolder(g_hMain ? g_hMain : owner, outPath, extra);
    return true;
}