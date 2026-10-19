#include "loader.h"

#include <propidl.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
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

// Scale first, then colour-convert, so the (costly) transform runs on screen-sized pixels.
bool ToPixelsManaged(IWICImagingFactory* f, IWICBitmapSource* src, int tw, int th, const ColorXf& cm,
                     std::vector<uint8_t>& out) {
    ComPtr<IWICFormatConverter> c1;
    if (FAILED(f->CreateFormatConverter(c1.put()))) return false;
    if (FAILED(c1->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeMedianCut)))
        return false;
    ComPtr<IWICBitmapSource> s = Scaled(f, c1.get(), tw, th);
    if (!s) return false;
    ComPtr<IWICColorTransform> ct;
    if (FAILED(f->CreateColorTransformer(ct.put())) ||
        FAILED(ct->Initialize(s.get(), cm.src.get(), cm.dst.get(), GUID_WICPixelFormat32bppBGRA)))
        return false;
    ComPtr<IWICFormatConverter> c2;
    if (FAILED(f->CreateFormatConverter(c2.put()))) return false;
    if (FAILED(c2->Initialize(ct.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeMedianCut)))
        return false;
    if (!AllocPixels(out, tw, th)) return false;
    if (FAILED(c2->CopyPixels(nullptr, tw * 4, (UINT)out.size(), out.data()))) {
        out.clear();
        return false;
    }
    return true;
}

// Convert any source to premultiplied BGRA at (tw, th).
bool ToPixels(IWICImagingFactory* f, IWICBitmapSource* src, int tw, int th, std::vector<uint8_t>& out,
              const ColorXf* cm = nullptr) {
    if (cm && cm->active() && ToPixelsManaged(f, src, tw, th, *cm, out)) return true;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.put()))) return false;
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut)))
        return false;
    ComPtr<IWICBitmapSource> s = Scaled(f, conv.get(), tw, th);
    if (!s) return false;
    if (!AllocPixels(out, tw, th)) return false;
    if (FAILED(s->CopyPixels(nullptr, tw * 4, (UINT)out.size(), out.data()))) {
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

// Reads the EXIF Orientation tag straight from a JPEG's APP1 segment.
// Returns 0 if absent/unreadable. Independent of the WIC metadata engine,
// and cheap (only the first segments of the file are read).
int JpegExifOrientation(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<uint8_t> b(128 * 1024);
    DWORD got = 0;
    ReadFile(h, b.data(), (DWORD)b.size(), &got, nullptr);
    CloseHandle(h);
    b.resize(got);
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return 0;
    size_t p = 2;
    while (p + 4 <= b.size()) {
        if (b[p] != 0xFF) return 0;
        const uint8_t marker = b[p + 1];
        if (marker == 0xFF) { p++; continue; }
        if (marker == 0xDA || marker == 0xD9) return 0;  // start of image data: no EXIF before it
        const size_t len = (b[p + 2] << 8) | b[p + 3];
        const size_t seg = p + 4;
        if (len < 2 || seg + len - 2 > b.size()) return 0;
        if (marker == 0xE1 && len >= 16 && memcmp(&b[seg], "Exif\0\0", 6) == 0) {
            const uint8_t* t = &b[seg + 6];
            const size_t tlen = len - 8;
            const bool le = t[0] == 'I' && t[1] == 'I';
            if (!le && !(t[0] == 'M' && t[1] == 'M')) return 0;
            auto u16 = [&](size_t o) -> unsigned { return o + 2 > tlen ? 0 : le ? (t[o] | t[o + 1] << 8) : (t[o] << 8 | t[o + 1]); };
            auto u32 = [&](size_t o) -> size_t {
                if (o + 4 > tlen) return 0;
                return le ? ((size_t)t[o] | (size_t)t[o + 1] << 8 | (size_t)t[o + 2] << 16 | (size_t)t[o + 3] << 24)
                          : ((size_t)t[o] << 24 | (size_t)t[o + 1] << 16 | (size_t)t[o + 2] << 8 | (size_t)t[o + 3]);
            };
            const size_t ifd = u32(4);
            const unsigned n = u16(ifd);
            for (unsigned i = 0; i < n && i < 512; i++) {
                const size_t e = ifd + 2 + i * 12;
                if (e + 12 > tlen) break;
                if (u16(e) == 0x0112) {
                    const unsigned v = u16(e + 8);
                    return (v >= 1 && v <= 8) ? (int)v : 0;
                }
            }
            return 0;
        }
        p = seg + len - 2;
    }
    return 0;
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

// Animated GIF: composite every frame onto the logical screen (honouring
// offsets, transparency and disposal), scaled to fit the box, within a memory budget.
bool DecodeGif(IWICImagingFactory* f, IWICBitmapDecoder* dec, UINT fc, int bw, int bh, int CW, int CH,
               DecodeResult& r) {
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
        if (fw && fh && ToPixels(f, fr.get(), (int)fw, (int)fh, px)) {
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
                !ToPixels(f, mem.get(), ow, oh, out))
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

void MakeThumb(IWICImagingFactory* f, DecodeResult& r) {
    int tw, tt;
    FitInside(r.pxW, r.pxH, kThumbBox, kThumbBox, tw, tt);
    ComPtr<IWICBitmap> mem;
    if (SUCCEEDED(f->CreateBitmapFromMemory(r.pxW, r.pxH, GUID_WICPixelFormat32bppPBGRA, r.pxW * 4,
                                            (UINT)r.pixels.size(), r.pixels.data(), mem.put()))) {
        if (ToPixels(f, mem.get(), tw, tt, r.thumb)) {
            r.thW = tw;
            r.thH = tt;
        }
    }
}

void Decode(IWICImagingFactory* f, const DecodeRequest& rq, DecodeResult& r) {
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
    r.orientation = container == GUID_ContainerFormatJpeg ? JpegExifOrientation(rq.path) : 0;
    if (!r.orientation) r.orientation = ReadOrientation(frame.get(), container);
    r.infoOk = true;
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
                if (ToPixels(f, th.get(), ow, oh, r.pixels, &cm)) {
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
        if (ToPixels(f, s.get(), ow, oh, r.pixels, &cm)) {
            r.pxW = ow;
            r.pxH = oh;
            r.ok = true;
        }
        return;
    }

    int bw = rq.boxW, bh = rq.boxH;
    if (rq.level == LV_FIT && r.orientation >= 5) std::swap(bw, bh);  // box is in display orientation

    if (gif && fc > 1 && rq.level == LV_FIT && DecodeGif(f, dec.get(), fc, bw, bh, CW, CH, r)) {
        if (rq.wantThumb) MakeThumb(f, r);
        return;
    }

    int ow, oh;
    FitInside((int)W, (int)H, bw, bh, ow, oh);
    ComPtr<IWICBitmapSource> s;
    if (ow < (int)W) s = DecoderScaled(f, frame.get(), W, H, ow, oh);
    if (!s) s = ComPtr<IWICBitmapSource>(frame.get());
    if (!ToPixels(f, s.get(), ow, oh, r.pixels, &cm)) {
        r.pixels.clear();
        return;
    }
    r.pxW = ow;
    r.pxH = oh;
    r.ok = true;
    if (rq.wantThumb) MakeThumb(f, r);
}

}  // namespace

std::wstring Loader::key(const std::wstring& path, int level) { return path + L'|' + (wchar_t)(L'0' + level); }

void Loader::start(int threads) {
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
        pending_.clear();
        for (auto& r : reqs) {
            const std::wstring k = key(r.path, r.level);
            if (inflight_.count(k) || ready_.count(k)) continue;
            pending_.push_back(std::move(r));
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

void Loader::workerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, f.putVoid());
    for (;;) {
        DecodeRequest rq;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return !pending_.empty(); });
            auto it = std::min_element(pending_.begin(), pending_.end(),
                                       [](const DecodeRequest& a, const DecodeRequest& b) { return a.priority < b.priority; });
            rq = std::move(*it);
            pending_.erase(it);
            inflight_.insert(key(rq.path, rq.level));
        }
        DecodeResult res;
        auto t0 = std::chrono::steady_clock::now();
        Decode(f.get(), rq, res);
        res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        HWND w = nullptr;
        UINT msg = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            const std::wstring k = key(rq.path, rq.level);
            inflight_.erase(k);
            ready_.insert(k);
            results_.push_back(std::move(res));
            if (!posted_ && wnd_) {
                posted_ = true;
                w = wnd_;
                msg = msg_;
            }
        }
        if (w) PostMessageW(w, msg, 0, 0);
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
