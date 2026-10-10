// ============================================================================
//  ImageOps.cpp —— 路径工具、图片载入/保存、缩放、调整（模糊/明度/饱和度）、
//                  ICO 文件写出、以及若干绘制辅助
// ============================================================================
#include "App.h"

using namespace Gdiplus;

const int kIcoSizes[ICO_SIZE_COUNT] = { 16, 24, 32, 48, 64, 128, 256 };

// ---------------------------------------------------------------------------
// 路径 / 字符串工具
// ---------------------------------------------------------------------------
std::wstring FileNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

std::wstring DirNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring() : path.substr(0, p + 1);
}

std::wstring StemOf(const std::wstring& path)
{
    std::wstring name = FileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        return name.substr(0, dot);
    return name;
}

std::wstring ExtOf(const std::wstring& path)
{
    std::wstring name = FileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        return name.substr(dot);
    return std::wstring();
}

bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring IntToStr(long long v)
{
    wchar_t buf[32];
    swprintf_s(buf, L"%lld", v);
    return buf;
}

std::wstring FormatSize(UINT w, UINT h)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%u × %u", w, h);
    return buf;
}

// ---------------------------------------------------------------------------
// 输出格式
// ---------------------------------------------------------------------------
struct OutFmt
{
    const wchar_t* ext;
    const wchar_t* mime;
    bool           alpha;
};

static const OutFmt kOutFmts[] =
{
    { L".png",  L"image/png",  true  },
    { L".jpg",  L"image/jpeg", false },
    { L".jpeg", L"image/jpeg", false },
    { L".bmp",  L"image/bmp",  false },
    { L".gif",  L"image/gif",  true  },
    { L".tif",  L"image/tiff", true  },
    { L".tiff", L"image/tiff", true  },
};

static const OutFmt* FindFmt(const std::wstring& ext)
{
    for (int i = 0; i < (int)_countof(kOutFmts); ++i)
        if (_wcsicmp(kOutFmts[i].ext, ext.c_str()) == 0)
            return &kOutFmts[i];
    return nullptr;
}

static const OutFmt* SourceFmt()
{
    const OutFmt* f = FindFmt(ExtOf(g_srcPath));
    if (!f) f = FindFmt(L".png");
    return f;
}

bool FormatSupportsAlpha()
{
    return SourceFmt()->alpha;
}

OutFmtGuess GuessSourceMime()
{
    const OutFmt* f = SourceFmt();
    OutFmtGuess g;
    g.mime = f->mime;
    g.ext = f->ext;
    g.alpha = f->alpha;
    if (_wcsicmp(g.ext.c_str(), L".jpeg") == 0) g.ext = L".jpg";
    if (_wcsicmp(g.ext.c_str(), L".tiff") == 0) g.ext = L".tif";
    return g;
}

static bool GetEncoderClsid(const wchar_t* mime, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0)
        return false;

    std::vector<BYTE> buf(size);
    ImageCodecInfo* pInfo = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(num, size, pInfo) != Ok)
        return false;

    for (UINT i = 0; i < num; ++i)
    {
        if (_wcsicmp(pInfo[i].MimeType, mime) == 0)
        {
            *pClsid = pInfo[i].Clsid;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 载入 / 释放
// ---------------------------------------------------------------------------
void ClearImage()
{
    if (g_pSrcImage) { delete g_pSrcImage; g_pSrcImage = nullptr; }
    if (g_pSrcStream) { g_pSrcStream->Release(); g_pSrcStream = nullptr; }
    g_srcW = g_srcH = 0;
    g_srcHasAlpha = false;
    g_srcPath.clear();
}

void NotifyImageChanged()
{
    if (g_hMain)
        SendMessageW(g_hMain, WM_APP_IMAGE_CHANGED, 0, 0);
}

// 先把文件整个读进内存，再用内存流交给 GDI+，避免长期锁定原文件
bool LoadImageFile(const std::wstring& path, std::wstring& err)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        err = L"无法打开文件：\r\n" + path;
        return false;
    }

    LARGE_INTEGER liSize = {};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0)
    {
        CloseHandle(hFile);
        err = L"文件为空或无法读取文件大小。";
        return false;
    }
    if (liSize.QuadPart > 1024LL * 1024 * 1024)
    {
        CloseHandle(hFile);
        err = L"文件过大（超过 1 GB），暂不支持。";
        return false;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)liSize.QuadPart);
    if (!hMem)
    {
        CloseHandle(hFile);
        err = L"内存不足，无法读入图片。";
        return false;
    }

    BYTE* pData = (BYTE*)GlobalLock(hMem);
    LONGLONG total = 0;
    bool ok = (pData != nullptr);
    while (ok && total < liSize.QuadPart)
    {
        DWORD want = (DWORD)((liSize.QuadPart - total) > (1 << 20) ? (1 << 20) : (liSize.QuadPart - total));
        DWORD got = 0;
        if (!ReadFile(hFile, pData + total, want, &got, nullptr) || got == 0)
            ok = false;
        else
            total += got;
    }
    GlobalUnlock(hMem);
    CloseHandle(hFile);

    if (!ok)
    {
        GlobalFree(hMem);
        err = L"读取文件失败。";
        return false;
    }

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)))
    {
        GlobalFree(hMem);
        err = L"创建内存流失败。";
        return false;
    }

    Image* pImg = Image::FromStream(pStream);
    if (!pImg || pImg->GetLastStatus() != Ok || pImg->GetWidth() == 0 || pImg->GetHeight() == 0)
    {
        delete pImg;
        pStream->Release();
        err = L"无法解析该图片，格式不受支持或文件已损坏。\r\n\r\n支持：PNG / JPG / BMP / GIF / TIFF";
        return false;
    }

    ClearImage();
    g_pSrcImage = pImg;
    g_pSrcStream = pStream;         // 与图片同生命周期
    g_srcW = pImg->GetWidth();
    g_srcH = pImg->GetHeight();
    g_srcPath = path;
    g_srcHasAlpha = FormatSupportsAlpha();
    return true;
}

// ---------------------------------------------------------------------------
// 缩放
// ---------------------------------------------------------------------------
Bitmap* ResizeBitmap(Image* src, int newW, int newH, std::wstring& err)
{
    if (!src) return nullptr;
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;

    Bitmap* dst = new Bitmap(newW, newH, PixelFormat32bppARGB);
    if (dst->GetLastStatus() != Ok)
    {
        delete dst;
        err = L"创建目标位图失败（内存不足？）";
        return nullptr;
    }

    {
        Graphics g(dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetCompositingQuality(CompositingQualityHighQuality);
        g.SetCompositingMode(CompositingModeSourceCopy);

        ImageAttributes attr;
        attr.SetWrapMode(WrapModeTileFlipXY);   // 消除边缘黑边

        RectF dest(0.0f, 0.0f, (REAL)newW, (REAL)newH);
        Status st = g.DrawImage(src, dest,
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

Bitmap* CreatePreviewSource(int maxDim)
{
    if (!g_pSrcImage) return nullptr;

    double s = 1.0;
    if ((UINT)maxDim < g_srcW || (UINT)maxDim < g_srcH)
    {
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

// ---------------------------------------------------------------------------
// 模糊：可分离盒式模糊，做 3 遍近似高斯；数据为预乘 BGRA
// ---------------------------------------------------------------------------
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
            int xi = (i < 0) ? 0 : ((i >= w) ? w - 1 : i);
            const BYTE* px = s + (size_t)xi * 4;
            for (int c = 0; c < 4; ++c) sum[c] += px[c];
        }

        for (int x = 0; x < w; ++x)
        {
            for (int c = 0; c < 4; ++c) d[(size_t)x * 4 + c] = (BYTE)(sum[c] / win);

            int xOut = x - r; if (xOut < 0) xOut = 0;
            int xIn = x + r + 1; if (xIn >= w) xIn = w - 1;
            const BYTE* pOut = s + (size_t)xOut * 4;
            const BYTE* pIn = s + (size_t)xIn * 4;
            for (int c = 0; c < 4; ++c) sum[c] += pIn[c] - pOut[c];
        }
    }
}

static void BoxBlurV(const BYTE* src, BYTE* dst, int w, int h, int r)
{
    const int win = r * 2 + 1;
    const size_t rowBytes = (size_t)w * 4;

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

            int yOut = y - r; if (yOut < 0) yOut = 0;
            int yIn = y + r + 1; if (yIn >= h) yIn = h - 1;
            const BYTE* pOut = s + (size_t)yOut * rowBytes;
            const BYTE* pIn = s + (size_t)yIn * rowBytes;
            for (int c = 0; c < 4; ++c) sum[c] += pIn[c] - pOut[c];
        }
    }
}

static void BoxBlur(std::vector<BYTE>& buf, int w, int h, int r, int passes)
{
    if (r < 1) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1) return;

    std::vector<BYTE> tmp(buf.size());
    for (int i = 0; i < passes; ++i)
    {
        BoxBlurH(buf.data(), tmp.data(), w, h, r);
        BoxBlurV(tmp.data(), buf.data(), w, h, r);
    }
}

static inline BYTE ClampB(int v) { return (BYTE)((v < 0) ? 0 : ((v > 255) ? 255 : v)); }

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

    const float bf = 1.0f + (float)p.brightness / 100.0f;   // 0 .. 2
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
            float r = (float)px[2];
            const float a = (float)px[3];

            if (p.brightness != 0)
            {
                b *= bf; g *= bf; r *= bf;
            }
            if (p.saturation != 0)
            {
                const float lum = 0.114f * b + 0.587f * g + 0.299f * r;
                b = lum + (b - lum) * sf;
                g = lum + (g - lum) * sf;
                r = lum + (r - lum) * sf;
            }

            const float af = a / 255.0f;
            px[0] = ClampB((int)(ClampB((int)(b + 0.5f)) * af + 0.5f));
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
            memcpy(buf.data() + (size_t)y * lineBytes, base + (size_t)y * stride, lineBytes);

        BoxBlur(buf, w, h, p.blur, 3);

        for (int y = 0; y < h; ++y)
        {
            const BYTE* s = buf.data() + (size_t)y * lineBytes;
            BYTE* row = base + (size_t)y * stride;
            for (int x = 0; x < w; ++x)
            {
                const BYTE* sp = s + (size_t)x * 4;
                BYTE* dp = row + (size_t)x * 4;
                const int a = sp[3];
                if (a == 0)
                {
                    dp[0] = dp[1] = dp[2] = 0;
                    dp[3] = 0;
                }
                else if (a >= 255)
                {
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                }
                else
                {
                    dp[0] = ClampB((sp[0] * 255 + a / 2) / a);
                    dp[1] = ClampB((sp[1] * 255 + a / 2) / a);
                    dp[2] = ClampB((sp[2] * 255 + a / 2) / a);
                    dp[3] = (BYTE)a;
                }
            }
        }
    }

    bmp->UnlockBits(&data);
}

// ---------------------------------------------------------------------------
// 颜色选择
// ---------------------------------------------------------------------------
COLORREF ChooseFillColor(HWND owner, COLORREF current)
{
    static COLORREF cust[16] = {};
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = current;
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc))
        return cc.rgbResult;
    return current;
}

// ---------------------------------------------------------------------------
// 保存：统一走“原目录 + 后缀”的流程
// ---------------------------------------------------------------------------
static const wchar_t* kCompleteTitle = L"处理完成";

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
        ULONG quality = 95;
        EncoderParameters ep;
        ep.Count = 1;
        ep.Parameter[0].Guid = EncoderQuality;
        ep.Parameter[0].Type = EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Value = &quality;
        st = bmp->Save(outPath.c_str(), &clsid, &ep);
    }
    else
    {
        st = bmp->Save(outPath.c_str(), &clsid, nullptr);
    }

    if (st != Ok)
    {
        err = L"保存文件失败（目标文件可能被其它程序占用，或没有写入权限）。";
        return false;
    }
    return true;
}

static void ShowDoneAndOpenFolder(HWND owner, const std::wstring& outPath, const std::wstring& extraLine)
{
    wchar_t msg[1400];
    swprintf_s(msg, L"处理完成！\r\n\r\n输出文件：\r\n%s\r\n\r\n%s\r\n是否打开输出文件所在的文件夹？",
               outPath.c_str(), extraLine.c_str());

    if (MessageBoxW(g_hMain ? g_hMain : owner, msg, kCompleteTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES)
    {
        std::wstring arg = L"/select,\"" + outPath + L"\"";
        ShellExecuteW(owner, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

bool SaveBitmapWithSuffix(Bitmap* bmp, const std::wstring& suffix, HWND owner, std::wstring* donePath)
{
    if (!bmp)
        return false;

    const OutFmt* fmt = SourceFmt();
    std::wstring outPath = DirNameOf(g_srcPath) + StemOf(g_srcPath) + suffix + fmt->ext;

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
    swprintf_s(extra, L"输出尺寸：%d × %d 像素\r\n", w, h);
    ShowDoneAndOpenFolder(g_hMain ? g_hMain : owner, outPath, extra);
    return true;
}

// ---------------------------------------------------------------------------
// ICO 写出
// ---------------------------------------------------------------------------
static void PutU16(std::vector<BYTE>& v, WORD x)
{
    v.push_back((BYTE)(x & 0xFF));
    v.push_back((BYTE)((x >> 8) & 0xFF));
}

static void PutU32(std::vector<BYTE>& v, DWORD x)
{
    v.push_back((BYTE)(x & 0xFF));
    v.push_back((BYTE)((x >> 8) & 0xFF));
    v.push_back((BYTE)((x >> 16) & 0xFF));
    v.push_back((BYTE)((x >> 24) & 0xFF));
}

// 32bpp DIB（自下而上）+ AND 掩码
static bool BuildBmpEntry(Bitmap* bmp, int size, std::vector<BYTE>& out)
{
    Rect rc(0, 0, size, size);
    BitmapData data;
    if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok)
        return false;

    const int pixBytes = size * size * 4;
    const int maskStride = ((size + 31) / 32) * 4;
    out.assign(sizeof(BITMAPINFOHEADER) + (size_t)pixBytes + (size_t)maskStride * size, 0);

    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)out.data();
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = size;
    bih->biHeight = size * 2;      // XOR + AND
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = pixBytes;

    BYTE* pix = out.data() + sizeof(BITMAPINFOHEADER);
    BYTE* mask = pix + pixBytes;

    for (int y = 0; y < size; ++y)
    {
        const BYTE* src = (const BYTE*)data.Scan0 + (size_t)(size - 1 - y) * data.Stride;  // 上下翻转
        BYTE* dst = pix + (size_t)y * size * 4;
        memcpy(dst, src, (size_t)size * 4);

        for (int x = 0; x < size; ++x)
        {
            if (src[(size_t)x * 4 + 3] < 128)
                mask[(size_t)y * maskStride + (x >> 3)] |= (BYTE)(0x80 >> (x & 7));
        }
    }

    bmp->UnlockBits(&data);
    return true;
}

static bool EncodePngToMemory(Bitmap* bmp, std::vector<BYTE>& out)
{
    CLSID clsid = {};
    if (!GetEncoderClsid(L"image/png", &clsid))
        return false;

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)))
        return false;

    bool ok = (bmp->Save(pStream, &clsid, nullptr) == Ok);
    if (ok)
    {
        STATSTG st = {};
        if (SUCCEEDED(pStream->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart > 0 &&
            st.cbSize.QuadPart < (ULONGLONG)64 * 1024 * 1024)
        {
            out.resize((size_t)st.cbSize.QuadPart);
            LARGE_INTEGER zero = {};
            pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
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

bool SaveIcoWithSuffix(Bitmap* square, const std::vector<int>& sizes, HWND owner, std::wstring* donePath)
{
    if (!square)
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
        Bitmap* one = ResizeBitmap(square, sz, sz, err);
        if (!one)
            continue;

        Entry e;
        e.size = sz;
        bool ok = (sz >= 256) ? EncodePngToMemory(one, e.data) : BuildBmpEntry(one, sz, e.data);
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
    PutU16(head, 0);                       // reserved
    PutU16(head, 1);                       // type = icon
    PutU16(head, (WORD)entries.size());

    DWORD offset = (DWORD)(6 + 16 * entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Entry& e = entries[i];
        head.push_back((BYTE)((e.size >= 256) ? 0 : e.size));   // 宽（0 表示 256）
        head.push_back((BYTE)((e.size >= 256) ? 0 : e.size));   // 高
        head.push_back(0);                                      // 调色板数
        head.push_back(0);                                      // reserved
        PutU16(head, 1);                                        // planes
        PutU16(head, 32);                                       // bpp
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
        if (!WriteFile(hf, head.data(), (DWORD)head.size(), &written, nullptr) || written != head.size())
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

// ---------------------------------------------------------------------------
// 绘制辅助
// ---------------------------------------------------------------------------
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

void PaintImagePreview(HDC hdc, const RECT& rc, Bitmap* img, const wchar_t* hint)
{
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT all = { 0, 0, w, h };
    PaintCheckerBackground(mem, all);

    if (img && img->GetWidth() > 0 && img->GetHeight() > 0)
    {
        Graphics g(mem);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        const int pad = 8;
        int aw = w - pad * 2;  if (aw < 1) aw = 1;
        int ah = h - pad * 2;  if (ah < 1) ah = 1;

        int iw = (int)img->GetWidth();
        int ih = (int)img->GetHeight();
        double s = (double)aw / iw;
        double sy = (double)ah / ih;
        if (sy < s) s = sy;

        int dw = (int)(iw * s + 0.5);  if (dw < 1) dw = 1;
        int dh = (int)(ih * s + 0.5);  if (dh < 1) dh = 1;

        REAL x = (REAL)((w - dw) / 2.0);
        REAL y = (REAL)((h - dh) / 2.0);
        g.DrawImage(img, RectF(x, y, (REAL)dw, (REAL)dh), 0, 0, (REAL)iw, (REAL)ih, UnitPixel);
    }
    else
    {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(110, 116, 124));
        HGDIOBJ oldFont = SelectObject(mem, g_hUiFont);
        DrawTextW(mem, hint ? hint : L"请先载入图片", -1, &all,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(mem, oldFont);
    }

    HBRUSH bBorder = CreateSolidBrush(RGB(205, 208, 212));
    FrameRect(mem, &all, bBorder);
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    DeleteObject(bBorder);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

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

// ---------------------------------------------------------------------------
// 编码器枚举 / 体积预估 / 指定格式保存（压缩页、格式转换页使用）
// ---------------------------------------------------------------------------
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

static bool IsLossyMime(const std::wstring& mime)
{
    return _wcsicmp(mime.c_str(), L"image/jpeg") == 0 || _wcsicmp(mime.c_str(), L"image/webp") == 0;
}

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
            pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
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

size_t EstimateEncodedSize(Image* bmp, const std::wstring& mime, ULONG quality)
{
    return EncodeToMemory(bmp, mime, quality, nullptr);
}

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
        if (sz == 0) { hi = mid - 1; continue; }
        if (sz <= targetBytes) { best = mid; bestSize = sz; lo = mid + 1; }
        else hi = mid - 1;
    }

    if (best == 0)
    {
        best = 10;
        bestSize = EncodeToMemory(bmp, L"image/jpeg", 10, nullptr);
    }

    if (actualBytes) *actualBytes = bestSize;
    return best;
}

size_t SourceFileSize()
{
    if (g_srcPath.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(g_srcPath.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (size_t)(((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
}

static size_t FileSizeOf(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (size_t)(((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
}

bool SaveBitmapAsFormat(Bitmap* bmp, const std::wstring& suffix, const std::wstring& mime,
                        const std::wstring& ext, ULONG quality, HWND owner)
{
    if (!bmp) return false;

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
        DeleteFileW(outPath.c_str());

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
        st = bmp->Save(outPath.c_str(), &clsid, &ep);
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