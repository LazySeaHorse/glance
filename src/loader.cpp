#include "loader.h"

#include <propidl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <functional>
#include <string>

#include "com.h"

// mingw's wincodec.h lacks this interface; it lets the JPEG decoder scale by
// 1/2, 1/4 or 1/8 while decoding (DCT scaling), which is several times faster
// than decoding full size and shrinking afterwards.
MIDL_INTERFACE("3B16811B-6A43-4ec9-B713-3D5A0C13B940")
IWICBitmapSourceTransform : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* prc, UINT uiWidth, UINT uiHeight,
                                                 WICPixelFormatGUID* pguidDstFormat,
                                                 WICBitmapTransformOptions dstTransform, UINT nStride,
                                                 UINT cbBufferSize, BYTE* pbBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestSize(UINT* puiWidth, UINT* puiHeight) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestPixelFormat(WICPixelFormatGUID* pguidDstFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoesSupportTransform(WICBitmapTransformOptions dstTransform,
                                                           BOOL* pfIsSupported) = 0;
};
__CRT_UUID_DECL(IWICBitmapSourceTransform, 0x3b16811b, 0x6a43, 0x4ec9, 0xb7, 0x13, 0x3d, 0x5a, 0x0c, 0x13, 0xb9, 0x40)

namespace {

const int kThumbBox = 320;
// Above this many pixels, the "full resolution" level is decoded smaller, to
// bound GPU memory (160 MP of 32-bit tiles is ~640 MB).
const double kFullBudgetPx = 160e6;

using Cancel = std::atomic<bool>;
using Emit = std::function<void(DecodeResult&&)>;

void FitInside(int w, int h, int bw, int bh, int& ow, int& oh) {
    if (bw <= 0 || bh <= 0 || (w <= bw && h <= bh)) { ow = w; oh = h; return; }
    double s = std::min((double)bw / w, (double)bh / h);
    ow = std::max(1, (int)std::lround(w * s));
    oh = std::max(1, (int)std::lround(h * s));
}

UINT BitsPerPixel(IWICImagingFactory* f, REFGUID fmt) {
    ComPtr<IWICComponentInfo> ci;
    if (FAILED(f->CreateComponentInfo(fmt, ci.put()))) return 0;
    ComPtr<IWICPixelFormatInfo> pi;
    if (FAILED(ci.as(pi))) return 0;
    UINT b = 0;
    pi->GetBitsPerPixel(&b);
    return b;
}

// Colour management: photos tagged with a wide-gamut profile (Display P3 from
// phones, Adobe RGB from cameras) are converted to sRGB, which is what a
// typical monitor shows. Untagged and sRGB photos skip this entirely.
struct ColorXf {
    ComPtr<IWICColorContext> src, dst;
    bool active() const { return src && dst; }
};

bool AllocPixels(std::vector<uint8_t>& out, int tw, int th) {
    const uint64_t bytes = (uint64_t)tw * th * 4;
    if (bytes > 0xFFFFFFF0ull) return false;
    try {
        out.resize((size_t)bytes);
    } catch (...) {
        return false;
    }
    return true;
}

// Pulls pixels in bands so a decode nobody wants any more can stop early.
bool CopyBanded(IWICBitmapSource* s, int w, int h, uint8_t* dst, const Cancel& cancel) {
    const UINT stride = (UINT)w * 4;
    // Small enough to stop promptly, big enough that the extra calls cost nothing.
    const int band = std::clamp((int)((2u << 20) / stride), 32, 256);
    for (int y = 0; y < h; y += band) {
        if (cancel) return false;
        const int n = std::min(band, h - y);
        WICRect rc{0, y, w, n};
        if (FAILED(s->CopyPixels(&rc, stride, stride * (UINT)n, dst + (size_t)y * stride))) return false;
    }
    return true;
}

ComPtr<IWICBitmapSource> Scaled(IWICImagingFactory* f, IWICBitmapSource* s, int tw, int th) {
    UINT sw = 0, sh = 0;
    s->GetSize(&sw, &sh);
    if ((int)sw == tw && (int)sh == th) return ComPtr<IWICBitmapSource>(s);
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(f->CreateBitmapScaler(sc.put()))) return {};
    // 4 = WICBitmapInterpolationModeHighQualityCubic (Windows 10+); Fant otherwise.
    HRESULT hr = sc->Initialize(s, tw, th, (WICBitmapInterpolationMode)4);
    if (FAILED(hr)) hr = sc->Initialize(s, tw, th, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return {};
    return ComPtr<IWICBitmapSource>(sc.get());
}

// A source producing premultiplied BGRA, colour-converted to sRGB when needed.
ComPtr<IWICBitmapSource> AsPBGRA(IWICImagingFactory* f, IWICBitmapSource* src, const ColorXf* cm) {
    if (cm && cm->active()) {
        ComPtr<IWICFormatConverter> c1, c2;
        ComPtr<IWICColorTransform> ct;
        if (SUCCEEDED(f->CreateFormatConverter(c1.put())) &&
            SUCCEEDED(c1->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)) &&
            SUCCEEDED(f->CreateColorTransformer(ct.put())) &&
            SUCCEEDED(ct->Initialize(c1.get(), cm->src.get(), cm->dst.get(), GUID_WICPixelFormat32bppBGRA)) &&
            SUCCEEDED(f->CreateFormatConverter(c2.put())) &&
            SUCCEEDED(c2->Initialize(ct.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
            return ComPtr<IWICBitmapSource>(c2.get());
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.put()))) return {};
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut)))
        return {};
    return ComPtr<IWICBitmapSource>(conv.get());
}

// Convert any source to premultiplied BGRA at (tw, th).
bool ToPixels(IWICImagingFactory* f, IWICBitmapSource* src, int tw, int th, std::vector<uint8_t>& out,
              const Cancel& cancel, const ColorXf* cm = nullptr) {
    if (cm && cm->active()) {
        // Scale first, then colour-convert, so the (costly) transform runs on screen-sized pixels.
        ComPtr<IWICFormatConverter> c1;
        if (SUCCEEDED(f->CreateFormatConverter(c1.put())) &&
            SUCCEEDED(c1->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
            ComPtr<IWICBitmapSource> s = Scaled(f, c1.get(), tw, th);
            ComPtr<IWICBitmapSource> p = s ? AsPBGRA(f, s.get(), cm) : ComPtr<IWICBitmapSource>();
            if (p && AllocPixels(out, tw, th) && CopyBanded(p.get(), tw, th, out.data(), cancel)) return true;
            out.clear();
            if (cancel) return false;
        }
    }
    ComPtr<IWICBitmapSource> conv = AsPBGRA(f, src, nullptr);
    if (!conv) return false;
    ComPtr<IWICBitmapSource> s = Scaled(f, conv.get(), tw, th);
    if (!s || !AllocPixels(out, tw, th)) return false;
    if (!CopyBanded(s.get(), tw, th, out.data(), cancel)) {
        out.clear();
        return false;
    }
    return true;
}

// Ask the decoder to do the shrinking (JPEG DCT scaling). Returns null if the
// codec can't, or if it would not save anything.
ComPtr<IWICBitmapSource> DecoderScaled(IWICImagingFactory* f, IWICBitmapFrameDecode* frame, UINT W, UINT H,
                                       int tw, int th) {
    ComPtr<IWICBitmapSourceTransform> tr;
    if (FAILED(frame->QueryInterface(__uuidof(IWICBitmapSourceTransform), tr.putVoid())) || !tr) return {};
    for (int div : {8, 4, 2}) {
        UINT cw = (W + div - 1) / div, ch = (H + div - 1) / div;
        if ((int)cw < tw || (int)ch < th) continue;
        UINT rw = cw, rh = ch;
        if (FAILED(tr->GetClosestSize(&rw, &rh))) return {};
        if ((int)rw < tw || (int)rh < th || rw >= W) continue;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(tr->GetClosestPixelFormat(&fmt))) return {};
        UINT bpp = BitsPerPixel(f, fmt);
        if (!bpp) return {};
        UINT stride = ((rw * bpp + 7) / 8 + 3) & ~3u;
        std::vector<uint8_t> buf((size_t)stride * rh);
        if (FAILED(tr->CopyPixels(nullptr, rw, rh, &fmt, WICBitmapTransformRotate0, stride, (UINT)buf.size(),
                                  buf.data())))
            return {};
        ComPtr<IWICBitmap> bmp;
        if (FAILED(f->CreateBitmapFromMemory(rw, rh, fmt, stride, (UINT)buf.size(), buf.data(), bmp.put())))
            return {};
        return ComPtr<IWICBitmapSource>(bmp.get());
    }
    return {};
}

// ---------------------------------------------------------------- metadata

std::wstring Widen(const std::string& s) {
    std::wstring w;
    if (s.empty()) return w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) {  // not UTF-8: treat as Latin-1
        for (unsigned char c : s) w.push_back((wchar_t)c);
        return w;
    }
    w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == 0)) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') i++;
    return s.substr(i);
}

struct Exif {
    std::wstring make, model, lens, dateTaken;  // dateTaken: "YYYY:MM:DD HH:MM:SS"
    double exposure = 0, fnumber = 0, focal = 0;
    int iso = 0, focal35 = 0, orientation = 0;
};

// Reads the EXIF block straight from a JPEG's APP1 segment. Independent of the
// WIC metadata engine, and cheap (only the first segments of the file are read).
bool ParseJpegExif(const std::wstring& path, Exif& ex) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> b(128 * 1024);
    DWORD got = 0;
    ReadFile(h, b.data(), (DWORD)b.size(), &got, nullptr);
    CloseHandle(h);
    b.resize(got);
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
    size_t p = 2;
    while (p + 4 <= b.size()) {
        if (b[p] != 0xFF) return false;
        const uint8_t marker = b[p + 1];
        if (marker == 0xFF) { p++; continue; }
        if (marker == 0xDA || marker == 0xD9) return false;  // image data started: no EXIF before it
        const size_t len = (b[p + 2] << 8) | b[p + 3];
        const size_t seg = p + 4;
        if (len < 2 || seg + len - 2 > b.size()) return false;
        if (marker == 0xE1 && len >= 16 && memcmp(&b[seg], "Exif\0\0", 6) == 0) {
            const uint8_t* t = &b[seg + 6];
            const size_t tlen = len - 8;
            const bool le = t[0] == 'I' && t[1] == 'I';
            if (!le && !(t[0] == 'M' && t[1] == 'M')) return false;
            auto u16 = [&](size_t o) -> unsigned {
                return o + 2 > tlen ? 0 : le ? (t[o] | t[o + 1] << 8) : (t[o] << 8 | t[o + 1]);
            };
            auto u32 = [&](size_t o) -> uint32_t {
                if (o + 4 > tlen) return 0;
                return le ? ((uint32_t)t[o] | (uint32_t)t[o + 1] << 8 | (uint32_t)t[o + 2] << 16 | (uint32_t)t[o + 3] << 24)
                          : ((uint32_t)t[o] << 24 | (uint32_t)t[o + 1] << 16 | (uint32_t)t[o + 2] << 8 | (uint32_t)t[o + 3]);
            };
            // Value of a directory entry at offset e (type, count, inline value or offset).
            auto ascii = [&](size_t e) -> std::wstring {
                const uint32_t n = u32(e + 4);
                const size_t off = n <= 4 ? e + 8 : u32(e + 8);
                if (n == 0 || off + n > tlen) return L"";
                std::string s(reinterpret_cast<const char*>(t + off), n);
                s = s.substr(0, s.find('\0'));
                return Trim(Widen(s));
            };
            auto rational = [&](size_t e) -> double {
                const uint32_t off = u32(e + 8);
                const uint32_t d = u32(off + 4);
                if (u16(e + 2) == 10) return d ? (double)(int32_t)u32(off) / (int32_t)d : 0;
                return d ? (double)u32(off) / d : 0;
            };
            auto integer = [&](size_t e) -> int {
                const unsigned type = u16(e + 2);
                if (type == 3) return (int)u16(e + 8);
                if (type == 4) return (int)u32(e + 8);
                return 0;
            };
            auto walk = [&](size_t ifd, bool exifIfd) -> size_t {
                size_t sub = 0;
                const unsigned n = u16(ifd);
                for (unsigned i = 0; i < n && i < 512; i++) {
                    const size_t e = ifd + 2 + i * 12;
                    if (e + 12 > tlen) break;
                    switch (u16(e)) {
                        case 0x010F: if (!exifIfd) ex.make = ascii(e); break;
                        case 0x0110: if (!exifIfd) ex.model = ascii(e); break;
                        case 0x0112: if (!exifIfd) ex.orientation = integer(e); break;
                        case 0x0132: if (!exifIfd && ex.dateTaken.empty()) ex.dateTaken = ascii(e); break;
                        case 0x8769: if (!exifIfd) sub = u32(e + 8); break;
                        case 0x829A: ex.exposure = rational(e); break;
                        case 0x829D: ex.fnumber = rational(e); break;
                        case 0x8827: ex.iso = integer(e); break;
                        case 0x9003: ex.dateTaken = ascii(e); break;
                        case 0x920A: ex.focal = rational(e); break;
                        case 0xA405: ex.focal35 = integer(e); break;
                        case 0xA434: ex.lens = ascii(e); break;
                        default: break;
                    }
                }
                return sub;
            };
            const size_t sub = walk(u32(4), false);
            if (sub && sub < tlen) walk(sub, true);
            if (ex.orientation < 1 || ex.orientation > 8) ex.orientation = 0;
            return true;
        }
        p = seg + len - 2;
    }
    return false;
}

int ReadOrientation(IWICBitmapFrameDecode* fr, const GUID& container) {
    // The Windows HEIF decoder already applies the container's rotation.
    if (container == GUID_ContainerFormatHeif) return 1;
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(fr->GetMetadataQueryReader(q.put())) || !q) return 1;
    static const wchar_t* names[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation",
                                     L"/xmp/tiff:Orientation"};
    for (const wchar_t* n : names) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(q->GetMetadataByName(n, &pv))) {
            int v = 0;
            switch (pv.vt) {
                case VT_UI1: v = pv.bVal; break;
                case VT_UI2: v = pv.uiVal; break;
                case VT_I2: v = pv.iVal; break;
                case VT_UI4: v = (int)pv.ulVal; break;
                case VT_I4: v = pv.lVal; break;
                case VT_LPWSTR: v = pv.pwszVal ? _wtoi(pv.pwszVal) : 0; break;
                case VT_LPSTR: v = pv.pszVal ? atoi(pv.pszVal) : 0; break;
                default: break;
            }
            PropVariantClear(&pv);
            if (v >= 1 && v <= 8) return v;
        }
    }
    return 1;
}

unsigned MetaUInt(IWICMetadataQueryReader* q, const wchar_t* name, unsigned def) {
    if (!q) return def;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    unsigned v = def;
    if (SUCCEEDED(q->GetMetadataByName(name, &pv))) {
        switch (pv.vt) {
            case VT_UI1: v = pv.bVal; break;
            case VT_UI2: v = pv.uiVal; break;
            case VT_UI4: v = pv.ulVal; break;
            case VT_I2: v = (unsigned)pv.iVal; break;
            case VT_I4: v = (unsigned)pv.lVal; break;
            case VT_BOOL: v = pv.boolVal ? 1 : 0; break;
            default: break;
        }
    }
    PropVariantClear(&pv);
    return v;
}

// System.Photo.* properties through WIC's metadata policies (TIFF, HEIC, RAW...).
void WicExif(IWICBitmapFrameDecode* frame, Exif& ex) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(q.put())) || !q) return;
    auto get = [&](const wchar_t* name, PROPVARIANT& pv) {
        PropVariantInit(&pv);
        return SUCCEEDED(q->GetMetadataByName(name, &pv));
    };
    auto str = [&](const wchar_t* name) -> std::wstring {
        PROPVARIANT pv;
        std::wstring s;
        if (get(name, pv)) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal) s = pv.pwszVal;
            else if (pv.vt == VT_LPSTR && pv.pszVal) s = Widen(pv.pszVal);
        }
        PropVariantClear(&pv);
        return Trim(s);
    };
    auto num = [&](const wchar_t* name) -> double {
        PROPVARIANT pv;
        double d = 0;
        if (get(name, pv)) {
            switch (pv.vt) {
                case VT_R8: d = pv.dblVal; break;
                case VT_R4: d = pv.fltVal; break;
                case VT_UI2: d = pv.uiVal; break;
                case VT_UI4: d = pv.ulVal; break;
                case VT_I4: d = pv.lVal; break;
                case VT_UI8: {  // packed rational: numerator low, denominator high
                    const uint32_t n = (uint32_t)pv.uhVal.QuadPart, dd = (uint32_t)(pv.uhVal.QuadPart >> 32);
                    d = dd ? (double)n / dd : 0;
                    break;
                }
                default: break;
            }
        }
        PropVariantClear(&pv);
        return d;
    };
    if (ex.make.empty()) ex.make = str(L"System.Photo.CameraManufacturer");
    if (ex.model.empty()) ex.model = str(L"System.Photo.CameraModel");
    if (ex.lens.empty()) ex.lens = str(L"System.Photo.LensModel");
    if (!ex.exposure) ex.exposure = num(L"System.Photo.ExposureTime");
    if (!ex.fnumber) ex.fnumber = num(L"System.Photo.FNumber");
    if (!ex.iso) ex.iso = (int)num(L"System.Photo.ISOSpeed");
    if (!ex.focal) ex.focal = num(L"System.Photo.FocalLength");
    if (!ex.focal35) ex.focal35 = (int)num(L"System.Photo.FocalLengthInFilm");
    if (ex.dateTaken.empty()) {
        PROPVARIANT pv;
        if (get(L"System.Photo.DateTaken", pv) && pv.vt == VT_FILETIME) {
            FILETIME local;
            SYSTEMTIME st;
            if (FileTimeToLocalFileTime(&pv.filetime, &local) && FileTimeToSystemTime(&local, &st)) {
                wchar_t b[32];
                swprintf(b, 32, L"%04d:%02d:%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                         st.wSecond);
                ex.dateTaken = b;
            }
        }
        PropVariantClear(&pv);
    }
}

std::wstring FormatDate(int y, int mo, int d, int h, int mi) {
    static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                      L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (mo < 1 || mo > 12 || y < 1800) return L"";
    wchar_t b[64];
    swprintf(b, 64, L"%d %ls %d, %02d:%02d", d, months[mo - 1], y, h, mi);
    return b;
}

std::wstring Num(double v, int maxDecimals) {
    wchar_t b[32];
    swprintf(b, 32, L"%.*f", maxDecimals, v);
    std::wstring s = b;
    if (s.find(L'.') != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    return s;
}

std::wstring ContainerName(const GUID& g, const std::wstring& path) {
    if (g == GUID_ContainerFormatJpeg) return L"JPEG";
    if (g == GUID_ContainerFormatPng) return L"PNG";
    if (g == GUID_ContainerFormatGif) return L"GIF";
    if (g == GUID_ContainerFormatBmp) return L"BMP";
    if (g == GUID_ContainerFormatTiff) return L"TIFF";
    if (g == GUID_ContainerFormatIco) return L"ICO";
    if (g == GUID_ContainerFormatWmp) return L"JPEG XR";
    if (g == GUID_ContainerFormatHeif) return L"HEIF";
    if (g == GUID_ContainerFormatWebp) return L"WebP";
    if (g == GUID_ContainerFormatDds) return L"DDS";
    size_t dot = path.find_last_of(L'.');
    std::wstring e = dot == std::wstring::npos ? L"" : path.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)std::towupper(c);
    return e;
}

void BuildMeta(IWICBitmapDecoder* dec, IWICBitmapFrameDecode* frame, const GUID& container, const std::wstring& path,
               int W, int H, int frames, DecodeResult& r) {
    Exif ex;
    if (container == GUID_ContainerFormatJpeg) ParseJpegExif(path, ex);
    if (frame) WicExif(frame, ex);
    (void)dec;
    auto add = [&](const wchar_t* k, const std::wstring& v) {
        if (!v.empty()) r.meta.emplace_back(k, v);
    };
    if (ex.dateTaken.size() >= 16) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (swscanf(ex.dateTaken.c_str(), L"%d:%d:%d %d:%d", &y, &mo, &d, &h, &mi) >= 3) add(L"Taken", FormatDate(y, mo, d, h, mi));
    }
    std::wstring cam = ex.model;
    if (!ex.make.empty() && _wcsnicmp(cam.c_str(), ex.make.c_str(), ex.make.size()) != 0) {
        // "NIKON CORPORATION" + "NIKON D850" -> "NIKON D850"; "Canon" + "EOS R5" -> "Canon EOS R5"
        std::wstring first = ex.make.substr(0, ex.make.find(L' '));
        if (_wcsnicmp(cam.c_str(), first.c_str(), first.size()) != 0) cam = ex.make + (cam.empty() ? L"" : L" " + cam);
    }
    add(L"Camera", cam);
    add(L"Lens", ex.lens);
    std::wstring expo;
    auto join = [&](const std::wstring& s) {
        if (s.empty()) return;
        if (!expo.empty()) expo += L"  ·  ";
        expo += s;
    };
    if (ex.exposure > 0) {
        if (ex.exposure >= 0.3) join(Num(ex.exposure, 1) + L" s");
        else join(L"1/" + Num(std::round(1.0 / ex.exposure), 0) + L" s");
    }
    if (ex.fnumber > 0) join(L"f/" + Num(ex.fnumber, 1));
    if (ex.iso > 0) join(L"ISO " + std::to_wstring(ex.iso));
    add(L"Exposure", expo);
    if (ex.focal > 0) {
        std::wstring fl = Num(ex.focal, 1) + L" mm";
        if (ex.focal35 > 0 && std::fabs(ex.focal35 - ex.focal) > 0.5)
            fl += L"  (" + std::to_wstring(ex.focal35) + L" mm equiv.)";
        add(L"Focal length", fl);
    }
    wchar_t b[96];
    const bool sideways = r.orientation >= 5;  // report the size as shown
    swprintf(b, 96, L"%d × %d  ·  %ls MP", sideways ? H : W, sideways ? W : H, Num(W * (double)H / 1e6, 1).c_str());
    add(L"Size", b);
    if (frames > 1) add(L"Frames", std::to_wstring(frames));

    WIN32_FILE_ATTRIBUTE_DATA fa;
    std::wstring fileLine = ContainerName(container, path);
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        const double bytes = (double)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
        std::wstring sz = bytes >= 1e6 ? Num(bytes / 1048576.0, 1) + L" MB" : Num(std::max(1.0, bytes / 1024.0), 0) + L" KB";
        fileLine = sz + L"  ·  " + fileLine;
        FILETIME local;
        SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &st))
            add(L"Modified", FormatDate(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute));
    }
    add(L"File", fileLine);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) add(L"Folder", path.substr(0, slash));
}

// Source colour space of a frame, or nothing if it is untagged or already sRGB.
ColorXf SourceColor(IWICImagingFactory* f, IWICBitmapFrameDecode* frame) {
    ColorXf cm;
    UINT n = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &n)) || n == 0 || n > 8) return cm;
    std::vector<ComPtr<IWICColorContext>> owned(n);
    std::vector<IWICColorContext*> raw(n);
    for (UINT i = 0; i < n; i++) {
        if (FAILED(f->CreateColorContext(owned[i].put()))) return cm;
        raw[i] = owned[i].get();
    }
    if (FAILED(frame->GetColorContexts(n, raw.data(), &n))) return cm;
    for (UINT i = 0; i < n; i++) {
        WICColorContextType type;
        if (FAILED(raw[i]->GetType(&type))) continue;
        if (type == WICColorContextExifColorSpace) {
            UINT cs = 0;
            raw[i]->GetExifColorSpace(&cs);
            if (cs != 2) continue;  // 1 = sRGB, 0xFFFF = uncalibrated; 2 = Adobe RGB
        } else if (type == WICColorContextProfile) {
            UINT size = 0;
            if (FAILED(raw[i]->GetProfileBytes(0, nullptr, &size)) || size < 128 || size > (8u << 20)) continue;
            std::vector<uint8_t> b(size);
            if (FAILED(raw[i]->GetProfileBytes(size, b.data(), &size))) continue;
            if (memcmp(&b[16], "RGB ", 4) != 0) continue;  // grey/CMYK profiles: leave to the codec
            // sRGB profiles name themselves "sRGB ..." in their description; skip those (no-op transform).
            const std::string text(reinterpret_cast<const char*>(b.data()), std::min<size_t>(b.size(), 4096));
            std::string flat;
            for (char c : text)
                if (c) flat.push_back(c);  // UTF-16 descriptions become ASCII-searchable
            if (flat.find("sRGB") != std::string::npos) continue;
        } else {
            continue;
        }
        ComPtr<IWICColorContext> dst;
        if (FAILED(f->CreateColorContext(dst.put())) || FAILED(dst->InitializeFromExifColorSpace(1))) return cm;
        cm.src = owned[i];
        cm.dst = dst;
        return cm;
    }
    return cm;
}

// ---------------------------------------------------------------- Windows thumbnail cache

// Asks Windows for the thumbnail Explorer already made, without generating
// one. Instant when the folder has been viewed in Explorer before.
// The photo's header has already been read (r.imgW/imgH/orientation), so the
// result can be checked: it must have the photo's shape, and a transparent
// picture standing in for a JPEG is an icon, not a thumbnail.
bool ShellThumb(IWICImagingFactory* f, const std::wstring& path, bool opaqueFormat, DecodeResult& r,
                const Cancel& cancel) {
    ComPtr<IShellItemImageFactory> fac;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, __uuidof(IShellItemImageFactory), fac.putVoid())) ||
        !fac)
        return false;
    HBITMAP hb = nullptr;
    SIZE sz{256, 256};
    if (FAILED(fac->GetImage(sz, SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb)
        return false;
    BITMAP bm{};
    bool ok = GetObjectW(hb, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight != 0;
    const int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    std::vector<uint8_t> px;
    if (ok) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        px.resize((size_t)w * h * 4);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h;
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(hb);
    if (!ok) return false;
    bool anyAlpha = false, translucent = false;
    for (size_t i = 3; i < px.size(); i += 4) {
        if (px[i]) anyAlpha = true;
        if (px[i] != 255) translucent = true;
    }
    if (!anyAlpha) {  // plain RGB thumbnail: make it opaque
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        translucent = false;
    }
    if (translucent && opaqueFormat) return false;  // an icon
    // Same shape as the photo, either as stored or turned upright?
    const double ta = (double)w / h, raw = (double)r.imgW / r.imgH;
    const bool matchesRaw = std::fabs(ta / raw - 1) < 0.04, matchesTurned = std::fabs(ta * raw - 1) < 0.04;
    if (!matchesRaw && !matchesTurned) return false;
    const bool quarter = r.orientation >= 5;  // EXIF 5-8: the photo is stored sideways
    // Windows' thumbnails already respect EXIF rotation; if this one is still sideways, say so.
    const bool upright = !quarter || (matchesTurned && !(matchesRaw && std::fabs(raw - 1) < 0.04));
    int ow, oh;
    FitInside(w, h, kThumbBox, kThumbBox, ow, oh);
    if (ow != w || oh != h) {
        ComPtr<IWICBitmap> mem;
        if (FAILED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, (UINT)px.size(), px.data(),
                                             mem.put())) ||
            !ToPixels(f, mem.get(), ow, oh, r.pixels, cancel))
            return false;
    } else {
        r.pixels = std::move(px);
    }
    r.pxW = ow;
    r.pxH = oh;
    r.ok = true;
    r.upright = upright;
    r.shellThumb = true;
    return true;
}

// ---------------------------------------------------------------- animated GIF

// Composite every frame onto the logical screen (honouring offsets,
// transparency and disposal), scaled to fit the box, within a memory budget.
bool DecodeGif(IWICImagingFactory* f, IWICBitmapDecoder* dec, UINT fc, int bw, int bh, int CW, int CH,
               DecodeResult& r, const Cancel& cancel) {
    const uint64_t budget = 384ull << 20;
    fc = std::min(fc, 2000u);
    int ow, oh;
    FitInside(CW, CH, bw, bh, ow, oh);
    const uint64_t need = (uint64_t)ow * oh * 4 * fc;
    if (need > budget) {
        const double k = std::sqrt((double)budget / need);
        ow = std::max(16, (int)(ow * k));
        oh = std::max(16, (int)(oh * k));
    }
    std::vector<uint8_t> canvas((size_t)CW * CH * 4, 0), saved;
    std::vector<uint8_t> px;
    for (UINT i = 0; i < fc; i++) {
        if (cancel) return false;
        ComPtr<IWICBitmapFrameDecode> fr;
        if (FAILED(dec->GetFrame(i, fr.put()))) break;
        ComPtr<IWICMetadataQueryReader> q;
        fr->GetMetadataQueryReader(q.put());
        UINT fw = 0, fh = 0;
        fr->GetSize(&fw, &fh);
        const int left = (int)MetaUInt(q.get(), L"/imgdesc/Left", 0), top = (int)MetaUInt(q.get(), L"/imgdesc/Top", 0);
        const unsigned delay = MetaUInt(q.get(), L"/grctlext/Delay", 10);
        const unsigned disposal = MetaUInt(q.get(), L"/grctlext/Disposal", 0);
        if (disposal == 3) saved = canvas;
        if (fw && fh && ToPixels(f, fr.get(), (int)fw, (int)fh, px, cancel)) {
            for (int y = 0; y < (int)fh; y++) {
                const int cy = top + y;
                if (cy < 0 || cy >= CH) continue;
                for (int x = 0; x < (int)fw; x++) {
                    const int cx = left + x;
                    if (cx < 0 || cx >= CW) continue;
                    const uint8_t* s = &px[((size_t)y * fw + x) * 4];
                    if (s[3] == 0) continue;  // transparent pixel: keep what's underneath
                    memcpy(&canvas[((size_t)cy * CW + cx) * 4], s, 4);
                }
            }
        }
        std::vector<uint8_t> out;
        if (ow == CW && oh == CH) {
            out = canvas;
        } else {
            ComPtr<IWICBitmap> mem;
            if (FAILED(f->CreateBitmapFromMemory(CW, CH, GUID_WICPixelFormat32bppPBGRA, CW * 4, (UINT)canvas.size(),
                                                 canvas.data(), mem.put())) ||
                !ToPixels(f, mem.get(), ow, oh, out, cancel))
                break;
        }
        r.animFrames.push_back(std::move(out));
        r.animDelays.push_back(delay <= 1 ? 100 : (int)delay * 10);  // browsers treat 0/10 ms as 100 ms
        if (disposal == 2) {  // restore to background (transparent) within the frame rect
            for (int y = std::max(0, top); y < std::min(CH, top + (int)fh); y++)
                for (int x = std::max(0, left); x < std::min(CW, left + (int)fw); x++)
                    memset(&canvas[((size_t)y * CW + x) * 4], 0, 4);
        } else if (disposal == 3 && !saved.empty()) {
            canvas.swap(saved);
        }
    }
    if (r.animFrames.size() < 2) {
        r.animFrames.clear();
        r.animDelays.clear();
        return false;
    }
    r.pixels = r.animFrames[0];
    r.pxW = ow;
    r.pxH = oh;
    r.ok = true;
    return true;
}

void MakeThumb(IWICImagingFactory* f, DecodeResult& r, const Cancel& cancel) {
    int tw, tt;
    FitInside(r.pxW, r.pxH, kThumbBox, kThumbBox, tw, tt);
    ComPtr<IWICBitmap> mem;
    if (SUCCEEDED(f->CreateBitmapFromMemory(r.pxW, r.pxH, GUID_WICPixelFormat32bppPBGRA, r.pxW * 4,
                                            (UINT)r.pixels.size(), r.pixels.data(), mem.put()))) {
        if (ToPixels(f, mem.get(), tw, tt, r.thumb, cancel)) {
            r.thW = tw;
            r.thH = tt;
        }
    }
}

// ---------------------------------------------------------------- full resolution, streamed

// Decodes top to bottom (the order JPEG and PNG decoders like) and emits one
// band of kTile rows at a time, each with kGutter extra rows above and below.
// Peak memory is a couple of bands, not the whole image.
bool DecodeFull(IWICImagingFactory* f, IWICBitmapFrameDecode* frame, UINT W, UINT H, const ColorXf& cm,
                DecodeResult& r, const Cancel& cancel, const Emit& emit) {
    ComPtr<IWICBitmapSource> src(frame);
    int lw = (int)W, lh = (int)H;
    if ((double)W * H > kFullBudgetPx) {  // enormous: decode the "full" level smaller
        const double s = std::sqrt(kFullBudgetPx / ((double)W * H));
        lw = std::max(1, (int)(W * s));
        lh = std::max(1, (int)(H * s));
        src = Scaled(f, frame, lw, lh);
        if (!src) return false;
    }
    ComPtr<IWICBitmapSource> chain = AsPBGRA(f, src.get(), &cm);
    if (!chain) return false;
    const size_t stride = (size_t)lw * 4;
    std::vector<uint8_t> buf;  // rows [bufStart, readTo)
    int bufStart = 0, readTo = 0;
    for (int y0 = 0; y0 < lh; y0 += kTile) {
        if (cancel) return false;
        const int y1 = std::min(lh, y0 + kTile);
        const int need = std::min(lh, y1 + kGutter);
        if (need > readTo) {
            const size_t old = buf.size();
            try {
                buf.resize(old + (size_t)(need - readTo) * stride);
            } catch (...) {
                return false;
            }
            WICRect rc{0, readTo, lw, need - readTo};
            if (FAILED(chain->CopyPixels(&rc, (UINT)stride, (UINT)((need - readTo) * stride), buf.data() + old)))
                return false;
            readTo = need;
        }
        const int b0 = std::max(0, y0 - kGutter);
        DecodeResult part;
        part.path = r.path;
        part.level = LV_FULL;
        part.band = true;
        part.ok = true;
        part.infoOk = true;
        part.imgW = r.imgW;
        part.imgH = r.imgH;
        part.orientation = r.orientation;
        part.levelW = lw;
        part.levelH = lh;
        part.coreY0 = y0;
        part.coreY1 = y1;
        part.bufY0 = b0;
        part.bufRows = need - b0;
        part.pixels.assign(buf.begin() + (size_t)(b0 - bufStart) * stride, buf.begin() + (size_t)(need - bufStart) * stride);
        emit(std::move(part));
        const int keep = std::max(bufStart, y1 - kGutter);  // the next band's top gutter
        buf.erase(buf.begin(), buf.begin() + (size_t)(keep - bufStart) * stride);
        bufStart = keep;
    }
    r.levelW = lw;
    r.levelH = lh;
    r.ok = true;
    return true;
}

// ---------------------------------------------------------------- one job

void Decode(IWICImagingFactory* f, const DecodeRequest& rq, DecodeResult& r, const Cancel& cancel, const Emit& emit) {
    r.path = rq.path;
    r.level = rq.level;
    if (!f) return;


    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(rq.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            dec.put())))
        return;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put()))) return;
    UINT W = 0, H = 0;
    if (FAILED(frame->GetSize(&W, &H)) || !W || !H) return;
    GUID container{};
    dec->GetContainerFormat(&container);
    UINT fc = 1;
    dec->GetFrameCount(&fc);
    const bool gif = container == GUID_ContainerFormatGif;
    int CW = (int)W, CH = (int)H;  // GIF: the logical screen can be larger than frame 0
    if (gif) {
        ComPtr<IWICMetadataQueryReader> gq;
        dec->GetMetadataQueryReader(gq.put());
        CW = (int)MetaUInt(gq.get(), L"/logscrdesc/Width", W);
        CH = (int)MetaUInt(gq.get(), L"/logscrdesc/Height", H);
        if (CW <= 0 || CH <= 0) { CW = (int)W; CH = (int)H; }
    }
    r.imgW = CW;
    r.imgH = CH;
    r.frames = (int)fc;
    if (container == GUID_ContainerFormatJpeg) {
        Exif ex;
        if (ParseJpegExif(rq.path, ex)) r.orientation = ex.orientation;
    }
    if (!r.orientation || r.orientation > 8) r.orientation = ReadOrientation(frame.get(), container);
    r.infoOk = true;

    // Wine answers thumbnail-cache requests with file icons, so it's skipped there.
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (rq.level == LV_THUMB && rq.shellFirst && !wine) {
        const bool opaque = container == GUID_ContainerFormatJpeg;
        if (ShellThumb(f, rq.path, opaque, r, cancel)) return;
        if (cancel) return;
    }

    if (rq.level == LV_META) {
        BuildMeta(dec.get(), frame.get(), container, rq.path, CW, CH, (int)fc, r);
        r.ok = true;
        return;
    }

    const ColorXf cm = SourceColor(f, frame.get());

    if (rq.level == LV_THUMB) {
        // 1) Embedded preview (EXIF thumbnail in JPEGs, previews in RAW files).
        ComPtr<IWICBitmapSource> th;
        if (FAILED(frame->GetThumbnail(th.put()))) dec->GetThumbnail(th.put());
        if (th) {
            UINT tw = 0, tth = 0;
            th->GetSize(&tw, &tth);
            const double a1 = (double)W / H, a2 = tth ? (double)tw / tth : 0;
            // Reject letterboxed/odd-aspect previews; they'd flash black bars.
            if (tw >= 96 && tth >= 64 && std::fabs(a1 - a2) / a1 < 0.03) {
                int ow, oh;
                FitInside((int)tw, (int)tth, rq.boxW, rq.boxH, ow, oh);
                if (ToPixels(f, th.get(), ow, oh, r.pixels, cancel, &cm)) {
                    r.pxW = ow;
                    r.pxH = oh;
                    r.ok = true;
                    return;
                }
            }
        }
        // 2) Cheap scaled decode (JPEG), or a small image that's quick anyway.
        int ow, oh;
        FitInside((int)W, (int)H, rq.boxW, rq.boxH, ow, oh);
        ComPtr<IWICBitmapSource> s = DecoderScaled(f, frame.get(), W, H, ow, oh);
        if (!s && rq.fastOnly && (uint64_t)W * H > 3000000ull) {
            r.infoOnly = true;  // the screen-sized decode is on its way anyway
            return;
        }
        if (!s) s = ComPtr<IWICBitmapSource>(frame.get());
        if (ToPixels(f, s.get(), ow, oh, r.pixels, cancel, &cm)) {
            r.pxW = ow;
            r.pxH = oh;
            r.ok = true;
        }
        return;
    }

    if (rq.level == LV_FULL) {
        DecodeFull(f, frame.get(), W, H, cm, r, cancel, emit);
        return;
    }

    int bw = rq.boxW, bh = rq.boxH;
    if (r.orientation >= 5) std::swap(bw, bh);  // box is in display orientation

    if (gif && fc > 1 && DecodeGif(f, dec.get(), fc, bw, bh, CW, CH, r, cancel)) {
        if (rq.wantThumb) MakeThumb(f, r, cancel);
        return;
    }
    if (cancel) return;

    int ow, oh;
    FitInside((int)W, (int)H, bw, bh, ow, oh);
    ComPtr<IWICBitmapSource> s;
    if (ow < (int)W) s = DecoderScaled(f, frame.get(), W, H, ow, oh);
    if (!s) s = ComPtr<IWICBitmapSource>(frame.get());
    if (!ToPixels(f, s.get(), ow, oh, r.pixels, cancel, &cm)) {
        r.pixels.clear();
        return;
    }
    r.pxW = ow;
    r.pxH = oh;
    r.ok = true;
    if (rq.wantThumb) MakeThumb(f, r, cancel);
}

}  // namespace

// ---------------------------------------------------------------- scheduling

std::wstring Loader::key(const std::wstring& path, int level) { return path + L'|' + (wchar_t)(L'0' + level); }

void Loader::start(int threads) {
    threadCount_ = threads;
    for (int i = 0; i < threads; i++) {
        threads_.emplace_back([this] { workerMain(); });
        threads_.back().detach();  // process exits without waiting on decodes
    }
}

void Loader::setNotify(HWND wnd, UINT msg) {
    bool post = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        wnd_ = wnd;
        msg_ = msg;
        if (!results_.empty()) {
            posted_ = true;
            post = true;
        }
    }
    if (post) PostMessageW(wnd, msg, 0, 0);
}

void Loader::setRequests(std::vector<DecodeRequest> reqs) {
    {
        std::lock_guard<std::mutex> lk(m_);
        std::unordered_set<std::wstring> wanted;
        pending_.clear();
        bool urgent = false;
        for (auto& r : reqs) {
            const std::wstring k = key(r.path, r.level);
            if (!wanted.insert(k).second) continue;
            if (inflight_.count(k) || ready_.count(k)) continue;
            if (r.priority < 10) urgent = true;
            pending_.push_back(std::move(r));
        }
        int busy = 0;
        for (auto& kv : inflight_) {
            if (!wanted.count(kv.first)) {  // nobody wants this any more: stop it
                *kv.second.cancel = true;
                kv.second.requeue = false;
            }
            if (!*kv.second.cancel) busy++;
        }
        // The photo on screen is waiting and every worker is busy with
        // background work: pause the least urgent job and run it again later.
        if (urgent && busy >= threadCount_) {
            Job* victim = nullptr;
            for (auto& kv : inflight_) {
                Job& j = kv.second;
                if (*j.cancel || j.rq.priority < 10) continue;
                if (!victim || j.rq.priority > victim->rq.priority) victim = &j;
            }
            if (victim) {
                *victim->cancel = true;
                victim->requeue = true;
            }
        }
    }
    cv_.notify_all();
}

std::vector<DecodeResult> Loader::takeResults() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<DecodeResult> out;
    out.swap(results_);
    ready_.clear();
    posted_ = false;
    return out;
}

void Loader::push(DecodeResult&& r) {
    HWND w = nullptr;
    UINT msg = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        results_.push_back(std::move(r));
        if (!posted_ && wnd_) {
            posted_ = true;
            w = wnd_;
            msg = msg_;
        }
    }
    if (w) PostMessageW(w, msg, 0, 0);
}

void Loader::workerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, f.putVoid());
    for (;;) {
        DecodeRequest rq;
        std::shared_ptr<Cancel> cancel = std::make_shared<Cancel>(false);
        std::wstring k;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return !pending_.empty(); });
            auto it = std::min_element(pending_.begin(), pending_.end(),
                                       [](const DecodeRequest& a, const DecodeRequest& b) { return a.priority < b.priority; });
            rq = std::move(*it);
            pending_.erase(it);
            k = key(rq.path, rq.level);
            inflight_[k] = Job{rq, cancel, false};
        }
        DecodeResult res;
        auto t0 = std::chrono::steady_clock::now();
        Decode(f.get(), rq, res, *cancel, [&](DecodeResult&& part) {
            if (!*cancel) push(std::move(part));
        });
        res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool deliver = true;
        {
            std::lock_guard<std::mutex> lk(m_);
            auto it = inflight_.find(k);
            const bool requeue = it != inflight_.end() && it->second.requeue;
            if (it != inflight_.end()) inflight_.erase(it);
            if (*cancel) {
                deliver = false;
                const bool queued = std::any_of(pending_.begin(), pending_.end(),
                                                [&](const DecodeRequest& p) { return key(p.path, p.level) == k; });
                if (requeue && !queued) pending_.push_back(rq);
            } else {
                ready_.insert(k);
            }
        }
        if (deliver) push(std::move(res));
        else cv_.notify_one();
    }
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

std::vector<std::wstring> SupportedExtensions() {
    std::vector<std::wstring> v;
    ComPtr<IWICImagingFactory> f;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                   f.putVoid()))) {
        ComPtr<IEnumUnknown> en;
        if (SUCCEEDED(f->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, en.put())) && en) {
            IUnknown* u = nullptr;
            ULONG got = 0;
            while (en->Next(1, &u, &got) == S_OK && u) {
                ComPtr<IWICBitmapCodecInfo> ci;
                u->QueryInterface(__uuidof(IWICBitmapCodecInfo), ci.putVoid());
                u->Release();
                u = nullptr;
                if (!ci) continue;
                UINT len = 0;
                if (FAILED(ci->GetFileExtensions(0, nullptr, &len)) || len == 0) continue;
                std::wstring s(len, L'\0');
                if (FAILED(ci->GetFileExtensions(len, &s[0], &len))) continue;
                size_t start = 0;
                while (start < s.size()) {
                    size_t end = s.find_first_of(L",;", start);
                    if (end == std::wstring::npos) end = s.size();
                    std::wstring e = s.substr(start, end - start);
                    e.erase(std::remove_if(e.begin(), e.end(), [](wchar_t c) { return c == L' ' || c == 0; }), e.end());
                    if (!e.empty()) {
                        if (e[0] != L'.') e = L"." + e;
                        v.push_back(Lower(e));
                    }
                    start = end + 1;
                }
            }
        }
    }
    if (v.empty()) {
        v = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif", L".bmp", L".dib", L".tif", L".tiff", L".ico",
             L".jxr", L".wdp", L".dds"};
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

bool HasExtension(const std::wstring& path, const std::vector<std::wstring>& exts) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
    std::wstring e = Lower(path.substr(dot));
    return std::binary_search(exts.begin(), exts.end(), e);
}
