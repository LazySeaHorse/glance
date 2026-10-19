// Glance - a fast, fluid, Picasa-style photo viewer for Windows.
//
// Architecture in one paragraph: a borderless window covers the monitor.
// Direct2D draws into a DirectComposition swap chain with per-pixel alpha, so
// the live desktop shows through the dimmed background (if composition isn't
// available we fall back to a screenshot of the desktop). Images are decoded
// by WIC on worker threads (loader.cpp): an embedded preview first so
// something appears immediately, then a screen-sized decode, then full
// resolution only when you zoom past it. Every motion is a damped spring
// (spring.h), so animations stay smooth when interrupted.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "com.h"
#include "loader.h"
#include "spring.h"

namespace {

constexpr UINT WM_APP_DECODED = WM_APP + 1;
constexpr UINT WM_APP_LIST = WM_APP + 2;
constexpr UINT_PTR TIMER_TICK = 1;
const wchar_t* kAppName = L"Glance";
constexpr ULONG_PTR kCopyDataOpen = 0x474C4E43;  // 'GLNC

// ---------------------------------------------------------------- timing/trace

double g_qpcFreq = 0;
double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / g_qpcFreq;
}

bool g_trace = false;
double g_traceBase = 0;  // Now() value that corresponds to process creation
void Trace(const wchar_t* fmt, ...) {
    if (!g_trace) return;
    wchar_t msg[600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 600, fmt, ap);
    va_end(ap);
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat(path, L"glance-trace.txt");
    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"%8.1f ms  %ls\n", (Now() - g_traceBase) * 1000.0, msg);
    fclose(f);
}

// ---------------------------------------------------------------- graphics

typedef HRESULT(WINAPI* PFN_DCompCreate)(IDXGIDevice*, REFIID, void**);

struct Gfx {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> dev;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Bitmap1> target;
    HANDLE frameWait = nullptr;
    UINT swapFlags = 0;

    PFN_DCompCreate dcompCreate = nullptr;
    ComPtr<IDCompositionDevice> dc;
    ComPtr<IDCompositionTarget> dcTarget;
    ComPtr<IDCompositionVisual> dcVisual;
    bool useDcomp = false;

    std::vector<uint8_t> desktopPx;  // fallback background
    ComPtr<ID2D1Bitmap1> desktop;

    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> font, fontBig;
    ComPtr<ID2D1StrokeStyle> round;
    ComPtr<ID2D1PathGeometry> icPrev, icNext, icPlay, icPause, icClose, icRot;

    HWND hwnd = nullptr;
    int W = 0, H = 0, monX = 0, monY = 0;
    float s = 1;  // DPI scale
    UINT maxBitmap = 8192;
    bool hqOk = true;
    bool needsRedraw = true;
};
Gfx g;

struct Bmp {
    ComPtr<ID2D1Bitmap1> b;
    int w = 0, h = 0;
};

// ---------------------------------------------------------------- model

struct Entry {
    bool info = false, failed = false, noFastThumb = false;
    bool thumbFailed = false, fullFailed = false;  // stop re-requesting what can't be decoded/uploaded
    int w = 0, h = 0, orient = 1, frames = 1;
    Bmp lv[3];
    std::vector<Bmp> anim;   // animated GIF frames (screen-fit size)
    std::vector<int> delays;  // ms per frame
    int userRot = 0;  // quarter turns, view only
};

Loader g_loader;
std::vector<std::wstring> g_exts;
std::vector<std::wstring> g_files;
std::unordered_map<std::wstring, int> g_fileIndex;
std::unordered_map<std::wstring, Entry> g_entries;
int g_cur = 0;
std::wstring g_curPath;
int g_prevDir = 1;
uint32_t g_scanGen = 0;
bool g_tracedFirst = false, g_tracedFit = false;

Entry& Ent(const std::wstring& p) { return g_entries[p]; }
Entry* Find(const std::wstring& p) {
    auto it = g_entries.find(p);
    return it == g_entries.end() ? nullptr : &it->second;
}
Entry* Cur() { return g_curPath.empty() ? nullptr : &Ent(g_curPath); }

enum AppearMode { APPEAR_OPEN, APPEAR_SLIDE, APPEAR_FADE };

struct View {
    Spring zoom{0.32, 0.9, 1e-4}, panX{0.32, 0.9, 0.2}, panY{0.32, 0.9, 0.2};
    Spring rot{0.45, 0.8, 0.01};
    Spring alpha{0.25, 1.0, 0.002}, offX{0.4, 0.88, 0.2}, scale{0.45, 0.8, 1e-4};
    Spring upgrade{0.16, 1.0, 0.004};
    bool fitted = true, appeared = false;
    int mode = APPEAR_OPEN, dir = 1;
    double navTime = 0;
    Bmp shown, prev;
};
View g_view;

struct Ghost {
    std::wstring path;
    Bmp bmp;
    int imgW = 0, imgH = 0, orient = 1;
    double zoom = 1, panX = 0, panY = 0, rot = 0;
    Spring alpha{0.25, 1.0, 0.004}, offX{0.4, 0.9, 0.5}, scale{0.4, 1.0, 1e-4};
};
std::vector<Ghost> g_ghosts;

Spring g_bg{0.32, 1.0, 0.002};
Spring g_ui{0.22, 1.0, 0.003};
Spring g_strip{0.38, 0.9, 0.3};
bool g_stripSnapped = false;
const float kBgAlpha = 0.88f;

int g_animIdx = 0;
double g_animNext = 0;
bool g_closing = false;
bool g_quit = false;
double g_closeStart = 0;
bool g_slideshow = false;
double g_slideNext = 0;
double g_lastActivity = 0;
POINT g_lastMouse{-1, -1};

enum Btn { B_NONE, B_CLOSE, B_ROTL, B_PREV, B_PLAY, B_NEXT, B_ROTR };
struct BtnRect {
    Btn id;
    D2D1_RECT_F r;
};
std::vector<BtnRect> g_btns;
Btn g_hoverBtn = B_NONE, g_pressBtn = B_NONE;
int g_hoverThumb = -1;

struct Drag {
    bool down = false, active = false, swipe = false, ignoreUp = false;
    POINT start{};
    double startPanX = 0, startPanY = 0, startOff = 0;
    std::deque<std::pair<double, POINT>> hist;
} g_drag;
bool g_releasing = false;

// filmstrip geometry (in file order)
std::vector<float> g_stripW, g_stripL;
float g_stripTotal = 0;
bool g_stripDirty = true;

// ---------------------------------------------------------------- helpers

int OrientQuarter(int o) {
    switch (o) {
        case 3: case 4: return 2;
        case 6: case 7: return 1;
        case 5: case 8: return 3;
        default: return 0;
    }
}
bool OrientFlip(int o) { return o == 2 || o == 4 || o == 5 || o == 7; }

void EffSize(const Entry& e, int userRot, double& ew, double& eh) {
    int q = (OrientQuarter(e.orient) + userRot) & 3;
    ew = (q & 1) ? e.h : e.w;
    eh = (q & 1) ? e.w : e.h;
}

double FitZoom(const Entry& e) {
    if (!e.info || !e.w || !e.h) return 1;
    double ew, eh;
    EffSize(e, e.userRot, ew, eh);
    double m = std::round(std::min(g.W, g.H) * 0.02);
    double z = std::min((g.W - 2 * m) / ew, (g.H - 2 * m) / eh);
    return std::min(z, (double)g.s);  // never upscale past 100% (logical)
}

void ClampPan(const Entry& e, double z, double& x, double& y) {
    double ew, eh;
    EffSize(e, e.userRot, ew, eh);
    double mx = std::max(0.0, (ew * z - g.W) / 2), my = std::max(0.0, (eh * z - g.H) / 2);
    x = std::clamp(x, -mx, mx);
    y = std::clamp(y, -my, my);
}

bool LargerThanScreen(const Entry& e) {
    double ew, eh;
    EffSize(e, e.userRot, ew, eh);
    return ew * g_view.zoom.target > g.W + 1 || eh * g_view.zoom.target > g.H + 1;
}

std::wstring FileName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}
std::wstring DirName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

// ---------------------------------------------------------------- device setup

bool InitDevice() {
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                               D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 5, D3D11_SDK_VERSION,
                                   g.d3d.put(), nullptr, nullptr);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls + 1, 4, D3D11_SDK_VERSION,
                               g.d3d.put(), nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, fls + 1, 4, D3D11_SDK_VERSION,
                               g.d3d.put(), nullptr, nullptr);
    if (FAILED(hr)) return false;
    if (FAILED(g.d3d.as(g.dxgi))) return false;
    ComPtr<IDXGIDevice1> d1;
    if (SUCCEEDED(g.d3d.as(d1))) d1->SetMaximumFrameLatency(1);

    if (!g.factory) {
        D2D1_FACTORY_OPTIONS opts{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                     g.factory.putVoid())))
            return false;
    }
    if (FAILED(g.factory->CreateDevice(g.dxgi.get(), g.dev.put()))) return false;
    if (FAILED(g.dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g.ctx.put()))) return false;
    g.ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    g.ctx->SetDpi(96, 96);
    g.maxBitmap = g.ctx->GetMaximumBitmapSize();
    g.ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), g.brush.put());
    return true;
}

bool CreateSwapchain(bool composition) {
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(g.dxgi->GetAdapter(adapter.put()))) return false;
    ComPtr<IDXGIFactory2> fac;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), fac.putVoid()))) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g.W;
    sd.Height = g.H;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = composition ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    for (int attempt = 0; attempt < 2; attempt++) {
        sd.Flags = attempt == 0 ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
        HRESULT hr = composition ? fac->CreateSwapChainForComposition(g.d3d.get(), &sd, nullptr, g.swap.put())
                                 : fac->CreateSwapChainForHwnd(g.d3d.get(), g.hwnd, &sd, nullptr, nullptr, g.swap.put());
        if (SUCCEEDED(hr)) {
            g.swapFlags = sd.Flags;
            g.frameWait = nullptr;
            ComPtr<IDXGISwapChain2> sc2;
            if (sd.Flags && SUCCEEDED(g.swap.as(sc2))) {
                sc2->SetMaximumFrameLatency(1);
                g.frameWait = sc2->GetFrameLatencyWaitableObject();
            }
            return true;
        }
    }
    return false;
}

bool CreateTarget() {
    ComPtr<IDXGISurface> surf;
    if (FAILED(g.swap->GetBuffer(0, __uuidof(IDXGISurface), surf.putVoid()))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          g.useDcomp ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(g.ctx->CreateBitmapFromDxgiSurface(surf.get(), &bp, g.target.put()))) return false;
    g.ctx->SetTarget(g.target.get());
    return true;
}

bool SetupDcomp() {
    if (!g.dcompCreate) return false;
    if (FAILED(g.dcompCreate(g.dxgi.get(), __uuidof(IDCompositionDevice), g.dc.putVoid())) || !g.dc) return false;
    if (FAILED(g.dc->CreateTargetForHwnd(g.hwnd, TRUE, g.dcTarget.put()))) return false;
    if (FAILED(g.dc->CreateVisual(g.dcVisual.put()))) return false;
    if (FAILED(g.dcVisual->SetContent(g.swap.get()))) return false;
    if (FAILED(g.dcTarget->SetRoot(g.dcVisual.get()))) return false;
    return SUCCEEDED(g.dc->Commit());
}

void CaptureDesktop() {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g.W;
    bi.bmiHeader.biHeight = -g.H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        BitBlt(mem, 0, 0, g.W, g.H, screen, g.monX, g.monY, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        g.desktopPx.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)g.W * g.H * 4);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void UploadDesktop() {
    if (g.desktopPx.empty()) return;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    g.ctx->CreateBitmap(D2D1::SizeU(g.W, g.H), g.desktopPx.data(), g.W * 4, &bp, g.desktop.put());
}

ComPtr<ID2D1PathGeometry> MakePath(const std::vector<std::vector<D2D1_POINT_2F>>& figs, bool closed, bool filled) {
    ComPtr<ID2D1PathGeometry> p;
    if (FAILED(g.factory->CreatePathGeometry(p.put()))) return {};
    ComPtr<ID2D1GeometrySink> sink;
    p->Open(sink.put());
    for (auto& f : figs) {
        if (f.empty()) continue;
        sink->BeginFigure(f[0], filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < f.size(); i++) sink->AddLine(f[i]);
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
    sink->Close();
    return p;
}

void InitStatic() {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(g.dw.put()));
    if (g.dw) {
        const wchar_t* family = L"Segoe UI";
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(g.dw->GetSystemFontCollection(fonts.put(), FALSE)) && fonts) {
            for (const wchar_t* f : {L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans"}) {
                UINT32 idx = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(f, &idx, &exists)) && exists) {
                    family = f;
                    break;
                }
            }
        }
        g.dw->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 13.5f * g.s, L"en-us", g.font.put());
        g.dw->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_LIGHT, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 20.0f * g.s, L"en-us", g.fontBig.put());
        if (g.font) g.font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (g.fontBig) {
            g.fontBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g.fontBig->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    g.factory->CreateStrokeStyle(&sp, nullptr, 0, g.round.put());

    auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
    g.icPrev = MakePath({{P(14.5f, 6), P(8.5f, 12), P(14.5f, 18)}}, false, false);
    g.icNext = MakePath({{P(9.5f, 6), P(15.5f, 12), P(9.5f, 18)}}, false, false);
    g.icPlay = MakePath({{P(8.5f, 6), P(18, 12), P(8.5f, 18)}}, true, true);
    g.icPause = MakePath({{P(8, 6.5f), P(10.5f, 6.5f), P(10.5f, 17.5f), P(8, 17.5f)},
                          {P(13.5f, 6.5f), P(16, 6.5f), P(16, 17.5f), P(13.5f, 17.5f)}},
                         true, true);
    g.icClose = MakePath({{P(7, 7), P(17, 17)}, {P(17, 7), P(7, 17)}}, false, false);
    // Clockwise rotate arrow: an arc with a gap at the top and a head at its end.
    std::vector<D2D1_POINT_2F> arc;
    const float cx = 12, cy = 12.5f, r = 6.5f, pi = 3.14159265f;
    for (int i = 0; i <= 28; i++) {
        float a = (-55.0f + (285.0f * i) / 28.0f) * pi / 180.0f;
        arc.push_back(P(cx + r * std::cos(a), cy + r * std::sin(a)));
    }
    float a1 = 230.0f * pi / 180.0f;
    D2D1_POINT_2F tip = arc.back();
    float dx = -std::sin(a1), dy = std::cos(a1);  // direction of travel at the end
    float nx = -dy, ny = dx;
    const float hl = 3.6f;
    D2D1_POINT_2F w1 = P(tip.x - hl * dx + hl * 0.8f * nx, tip.y - hl * dy + hl * 0.8f * ny);
    D2D1_POINT_2F w2 = P(tip.x - hl * dx - hl * 0.8f * nx, tip.y - hl * dy - hl * 0.8f * ny);
    g.icRot = MakePath({arc, {w1, tip, w2}}, false, false);
}

// ---------------------------------------------------------------- decoding requests

Bmp Upload(const std::vector<uint8_t>& px, int w, int h) {
    Bmp out;
    if (px.empty() || w <= 0 || h <= 0) return out;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (SUCCEEDED(g.ctx->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, out.b.put()))) {
        out.w = w;
        out.h = h;
    }
    return out;
}

bool NeedFull(const Entry& e) {
    if (!e.info || e.failed || e.fullFailed || !e.anim.empty()) return false;
    if (e.lv[2].b) return false;
    const Bmp& fit = e.lv[1];
    if (!fit.b) return false;
    if (fit.w >= e.w || fit.w >= (int)g.maxBitmap) return false;  // already full-res (or capped)
    return e.w * g_view.zoom.target > fit.w * 1.03;
}

void RequestDecodes() {
    if (g_files.empty()) return;
    std::vector<DecodeRequest> rq;
    auto add = [&](const std::wstring& p, int lv, int prio, bool fast, bool wantThumb) {
        DecodeRequest r;
        r.path = p;
        r.level = lv;
        r.priority = prio;
        r.fastOnly = fast;
        r.wantThumb = wantThumb;
        if (lv == LV_THUMB) r.boxW = r.boxH = 320;
        else if (lv == LV_FIT) { r.boxW = g.W; r.boxH = g.H; }
        else r.boxW = r.boxH = (int)g.maxBitmap;
        rq.push_back(std::move(r));
    };
    Entry& e = Ent(g_curPath);
    if (!e.failed) {
        if (!e.lv[0].b && !e.lv[1].b && !e.noFastThumb) add(g_curPath, LV_THUMB, 0, true, false);
        if (!e.lv[1].b) add(g_curPath, LV_FIT, 1, false, !e.lv[0].b);
        if (NeedFull(e)) add(g_curPath, LV_FULL, 2, false, false);
    }
    const int n = (int)g_files.size();
    const int order[] = {1, -1, 2, -2, 3};
    int prio = 10;
    for (int d : order) {
        int i = g_cur + d * g_prevDir;
        if (i < 0 || i >= n) continue;
        Entry& ne = Ent(g_files[i]);
        if (!ne.failed && !ne.lv[1].b) add(g_files[i], LV_FIT, prio, false, !ne.lv[0].b);
        prio++;
    }
    for (int d = 1; d <= 40; d++) {
        for (int sgn : {1, -1}) {
            int i = g_cur + d * sgn;
            if (i < 0 || i >= n) continue;
            Entry* te = Find(g_files[i]);
            if (te && (te->failed || te->thumbFailed || te->lv[0].b || te->lv[1].b)) continue;
            add(g_files[i], LV_THUMB, 100 + d, false, false);
        }
    }
    g_loader.setRequests(std::move(rq));
}

void Evict() {
    for (auto& kv : g_entries) {
        auto it = g_fileIndex.find(kv.first);
        int d = it == g_fileIndex.end() ? (kv.first == g_curPath ? 0 : 1 << 20) : std::abs(it->second - g_cur);
        if (d > 0) kv.second.lv[2] = Bmp();
        if (d > 1) kv.second.anim.clear();
        if (d > 3) kv.second.lv[1] = Bmp();
        if (d > 90) kv.second.lv[0] = Bmp();
    }
}

// ---------------------------------------------------------------- view state

const Bmp* AnyBitmap(const Entry& e) {
    for (int lv : {1, 2, 0})
        if (e.lv[lv].b) return &e.lv[lv];
    return nullptr;
}

const Bmp* Choose(const Entry& e, double neededW) {
    const Bmp* best = nullptr;
    for (int lv : {1, 2}) {
        const Bmp& b = e.lv[lv];
        if (b.b && b.w >= neededW * 0.98 && (!best || b.w < best->w)) best = &b;
    }
    if (best) return best;
    for (int lv : {2, 1, 0}) {
        const Bmp& b = e.lv[lv];
        if (b.b && (!best || b.w > best->w)) best = &b;
    }
    return best;
}

void SetZoomPanParams(double r, double d) {
    g_view.zoom.params(r, d);
    g_view.panX.params(r, d);
    g_view.panY.params(r, d);
}

void RestartAnimation() {
    g_animIdx = 0;
    Entry* e = Cur();
    g_animNext = Now() + ((e && !e->delays.empty()) ? e->delays[0] / 1000.0 : 0.1);
}

void TryAppear() {
    if (g_view.appeared || g_closing) return;
    Entry* e = Cur();
    if (!e || !e->info || !AnyBitmap(*e)) return;
    View& v = g_view;
    v.appeared = true;
    v.zoom.snap(FitZoom(*e));
    v.panX.snap(0);
    v.panY.snap(0);
    v.fitted = true;
    switch (v.mode) {
        case APPEAR_OPEN:
            v.scale.params(0.5, 0.78);
            v.scale.snap(0.84);
            v.alpha.params(0.24, 1.0);
            v.alpha.snap(0);
            v.offX.snap(0);
            break;
        case APPEAR_SLIDE:
            v.offX.params(0.42, 0.9);
            v.offX.snap(v.dir * g.W * 0.07);
            v.alpha.params(0.26, 1.0);
            v.alpha.snap(0);
            v.scale.params(0.42, 0.9);
            v.scale.snap(0.98);
            break;
        case APPEAR_FADE:
            v.offX.snap(0);
            v.alpha.params(0.9, 1.0);  // slow crossfade
            v.alpha.snap(0);
            v.scale.snap(1.0);
            break;
    }
    v.offX.target = 0;
    v.alpha.target = 1;
    v.scale.target = 1;
    v.upgrade.snap(1);
    v.shown = Bmp();
    v.prev = Bmp();
    RestartAnimation();
    if (g_slideshow) g_slideNext = Now() + 4.0;
    if (!g_tracedFirst) {
        g_tracedFirst = true;
        Trace(L"first image on screen");
    }
}

void ResetView(int mode, int dir) {
    View& v = g_view;
    v.appeared = false;
    v.mode = mode;
    v.dir = dir;
    v.navTime = Now();
    v.fitted = true;
    v.shown = Bmp();
    v.prev = Bmp();
    v.alpha.snap(0);
    v.offX.snap(0);
    v.scale.snap(1);
    Entry* e = Cur();
    v.rot.snap(e ? e->userRot * 90.0 : 0.0);
    v.zoom.snap(e && e->info ? FitZoom(*e) : 1.0);
    v.panX.snap(0);
    v.panY.snap(0);
    SetZoomPanParams(0.32, 0.9);
    if (e) SetWindowTextW(g.hwnd, (FileName(g_curPath) + L" - " + kAppName).c_str());
    TryAppear();
}

void PushGhost(int dir, bool slide) {
    Entry* e = Cur();
    if (!e || !g_view.appeared || g_view.alpha.x < 0.01) return;
    const Bmp* b = g_view.shown.b ? &g_view.shown : AnyBitmap(*e);
    if (!b) return;
    Ghost gh;
    gh.path = g_curPath;
    gh.bmp = *b;
    gh.imgW = e->w;
    gh.imgH = e->h;
    gh.orient = e->orient;
    gh.zoom = g_view.zoom.x;
    gh.panX = g_view.panX.x;
    gh.panY = g_view.panY.x;
    gh.rot = g_view.rot.x;
    gh.alpha.x = g_view.alpha.x;
    gh.alpha.target = 0;
    gh.offX.x = g_view.offX.x;
    gh.offX.v = g_view.offX.v;
    gh.scale.x = g_view.scale.x;
    if (slide) {
        gh.alpha.params(0.26, 1.0);
        gh.offX.params(0.42, 0.9);
        double dist = g.W * 0.07;
        gh.offX.target = std::min(gh.offX.x, 0.0) * (dir > 0) + std::max(gh.offX.x, 0.0) * (dir < 0) - dir * dist;
        gh.scale.target = gh.scale.x * 0.98;
    } else {
        gh.alpha.params(0.9, 1.0);
        gh.offX.target = gh.offX.x;
        gh.scale.target = gh.scale.x;
    }
    g_ghosts.push_back(std::move(gh));
    if (g_ghosts.size() > 3) g_ghosts.erase(g_ghosts.begin());
}

void Navigate(int idx, int mode) {
    const int n = (int)g_files.size();
    if (n == 0 || g_closing) return;
    if (idx < 0 || idx >= n) {  // at either end: a little nudge instead
        if (g_view.appeared) {
            g_view.offX.params(0.38, 0.6);
            g_view.offX.v += (idx < 0 ? 1 : -1) * 1400.0 * g.s;
            g.needsRedraw = true;
        }
        return;
    }
    if (idx == g_cur) return;
    if (g_drag.down) {  // e.g. a key pressed mid-drag
        g_drag = Drag();
        g_pressBtn = B_NONE;
        g_releasing = true;
        ReleaseCapture();
        g_releasing = false;
    }
    Trace(L"navigate %d -> %d (%ls)", g_cur, idx, mode == APPEAR_SLIDE ? L"slide" : L"fade");
    int dir = idx > g_cur ? 1 : -1;
    if (mode == APPEAR_FADE) dir = 1;
    PushGhost(dir, mode == APPEAR_SLIDE);
    g_prevDir = dir;
    g_cur = idx;
    g_curPath = g_files[idx];
    ResetView(mode, dir);
    Evict();
    RequestDecodes();
    g.needsRedraw = true;
}

void Close() {
    if (g_closing) return;
    g_closing = true;
    g_closeStart = Now();
    g_slideshow = false;
    g_bg.params(0.26, 1.0);
    g_bg.target = 0;
    g_view.alpha.params(0.2, 1.0);
    g_view.alpha.target = 0;
    g_view.scale.params(0.3, 1.0);
    g_view.scale.target = g_view.scale.x * 0.9;
    for (auto& gh : g_ghosts) gh.alpha.target = 0;
    g_ui.target = 0;
    g.needsRedraw = true;
}

void ZoomTo(double nz, double cx, double cy, double r = 0.3, double d = 0.9) {
    Entry* e = Cur();
    if (!e || !e->info || !g_view.appeared) return;
    const double fit = FitZoom(*e);
    nz = std::clamp(nz, fit, std::max(fit, 20.0));
    if (std::fabs(nz - fit) < fit * 0.015) nz = fit;
    const double Cx = cx - g.W / 2.0, Cy = cy - g.H / 2.0;
    const double z = g_view.zoom.x;
    const double Px = (Cx - g_view.panX.x) / z, Py = (Cy - g_view.panY.x) / z;
    double tx = Cx - Px * nz, ty = Cy - Py * nz;
    g_view.fitted = (nz == fit);
    if (g_view.fitted) tx = ty = 0;
    ClampPan(*e, nz, tx, ty);
    SetZoomPanParams(r, d);
    g_view.zoom.target = nz;
    g_view.panX.target = tx;
    g_view.panY.target = ty;
    RequestDecodes();
    g.needsRedraw = true;
}

void ZoomBy(double factor, double cx, double cy) { ZoomTo(g_view.zoom.target * factor, cx, cy); }

void ZoomToFit() {
    Entry* e = Cur();
    if (e) ZoomTo(FitZoom(*e), g.W / 2.0, g.H / 2.0, 0.38, 0.88);
}

void Rotate(int dir) {
    Entry* e = Cur();
    if (!e || !e->info || !g_view.appeared) return;
    e->userRot = (e->userRot + dir + 4) & 3;
    g_view.rot.params(0.48, 0.78);
    g_view.rot.target += dir * 90.0;
    SetZoomPanParams(0.48, 0.85);
    g_view.zoom.target = FitZoom(*e);
    g_view.panX.target = 0;
    g_view.panY.target = 0;
    g_view.fitted = true;
    g_stripDirty = true;
    RequestDecodes();
    g.needsRedraw = true;
}

void ToggleSlideshow() {
    if (g_files.size() < 2) return;
    g_slideshow = !g_slideshow;
    if (g_slideshow) {
        if (!g_view.fitted) ZoomToFit();
        g_slideNext = Now() + 3.0;
        g_lastActivity = 0;  // hide the controls right away
    }
    g.needsRedraw = true;
}

void SlideshowTick() {
    if (!g_slideshow || !g_view.appeared || g_closing) return;
    const double t = Now();
    if (t < g_slideNext) return;
    const int n = (int)g_files.size();
    const int next = (g_cur + 1) % n;
    Entry* ne = Find(g_files[next]);
    bool ready = ne && (ne->lv[1].b || ne->failed);
    if (!ready && t < g_slideNext + 3.0) return;
    if (next == 0) g_prevDir = 1;
    Navigate(next, APPEAR_FADE);
}

// ---------------------------------------------------------------- results

void ProcessResults() {
    bool curChanged = false;
    for (auto& r : g_loader.takeResults()) {
        Entry& e = Ent(r.path);
        if (r.infoOk) {
            if (!e.info || e.w != r.imgW || e.h != r.imgH || e.orient != r.orientation) g_stripDirty = true;
            e.info = true;
            e.w = r.imgW;
            e.h = r.imgH;
            e.orient = r.orientation;
            e.frames = r.frames;
        } else {
            e.failed = true;
            g_stripDirty = true;
        }
        bool ok = r.ok;
        if (ok) {
            Bmp b = Upload(r.pixels, r.pxW, r.pxH);
            ok = (bool)b.b;
            if (ok) e.lv[r.level] = std::move(b);
        }
        if (ok && !r.animFrames.empty()) {
            e.anim.clear();
            e.delays = r.animDelays;
            for (auto& fpx : r.animFrames) {
                Bmp fb = Upload(fpx, r.pxW, r.pxH);
                if (!fb.b) {
                    e.anim.clear();
                    break;
                }
                e.anim.push_back(std::move(fb));
            }
            if (r.path == g_curPath) RestartAnimation();
        }
        if (!ok && r.infoOk) {  // decodable header but these pixels didn't make it
            if (r.level == LV_THUMB) (r.infoOnly ? e.noFastThumb : e.thumbFailed) = true;
            else if (r.level == LV_FIT) e.failed = true;
            else e.fullFailed = true;  // e.g. too big for memory: keep showing the screen-sized version
        }
        if (!r.thumb.empty() && !e.lv[0].b) e.lv[0] = Upload(r.thumb, r.thW, r.thH);
        if (r.path == g_curPath) {
            curChanged = true;
            Trace(L"decoded %ls level %d (%dx%d) in %.1f ms", FileName(r.path).c_str(), r.level, r.pxW, r.pxH, r.ms);
            if (r.level == LV_FIT && r.ok && !g_tracedFit) {
                g_tracedFit = true;
                Trace(L"screen-resolution image ready");
            }
        }
    }
    if (curChanged) {
        Entry* e = Cur();
        if (e && e->info && g_view.fitted && g_view.appeared) g_view.zoom.target = FitZoom(*e);
        TryAppear();
    }
    RequestDecodes();
    g.needsRedraw = true;
}

// ---------------------------------------------------------------- filmstrip layout

float ThumbH() { return 56.0f * g.s; }
float StripPad() { return 8.0f * g.s; }
float StripGap() { return 6.0f * g.s; }
float StripMaxInner() { return std::max(100.0f, g.W - 2 * 90.0f * g.s - 2 * StripPad()); }
float StripY() { return g.H - 22.0f * g.s - (ThumbH() + 2 * StripPad()); }
bool StripVisible() { return g_files.size() > 1; }

void RebuildStrip() {
    const int n = (int)g_files.size();
    const float th = ThumbH(), gap = StripGap();
    g_stripW.resize(n);
    g_stripL.resize(n);
    float x = 0;
    for (int i = 0; i < n; i++) {
        float w = th;
        Entry* e = Find(g_files[i]);
        if (e && e->info && e->w && e->h) {
            double ew, eh;
            EffSize(*e, e->userRot, ew, eh);
            w = std::clamp((float)(th * ew / eh), th * 0.62f, th * 1.6f);
        }
        g_stripW[i] = w;
        g_stripL[i] = x;
        x += w + gap;
    }
    g_stripTotal = n ? x - gap : 0;
    g_stripDirty = false;
}

float StripTarget() {
    if (g_files.empty() || g_cur >= (int)g_stripL.size()) return 0;
    const float maxIn = StripMaxInner();
    if (g_stripTotal <= maxIn) return g_stripTotal / 2;
    const float c = g_stripL[g_cur] + g_stripW[g_cur] / 2;
    return std::clamp(c, maxIn / 2, g_stripTotal - maxIn / 2);
}

D2D1_RECT_F StripPill() {
    const float inner = std::min(g_stripTotal, StripMaxInner());
    const float pad = StripPad(), y = StripY();
    return D2D1::RectF(g.W / 2.0f - inner / 2 - pad, y, g.W / 2.0f + inner / 2 + pad, y + ThumbH() + 2 * pad);
}

D2D1_RECT_F ThumbRect(int i) {
    const float x = g.W / 2.0f - (float)g_strip.x + g_stripL[i];
    const float y = StripY() + StripPad();
    return D2D1::RectF(x, y, x + g_stripW[i], y + ThumbH());
}

void VisibleThumbs(int& first, int& last) {
    first = 0;
    last = -1;
    if (g_stripL.empty()) return;
    const float inner = std::min(g_stripTotal, StripMaxInner());
    const float lo = (float)g_strip.x - inner / 2 - ThumbH() * 2, hi = (float)g_strip.x + inner / 2;
    first = (int)(std::lower_bound(g_stripL.begin(), g_stripL.end(), lo) - g_stripL.begin());
    first = std::max(0, first - 1);
    last = first;
    while (last + 1 < (int)g_stripL.size() && g_stripL[last + 1] < hi) last++;
}

void LayoutButtons() {
    g_btns.clear();
    const float s = g.s;
    const float r = 18 * s;
    g_btns.push_back({B_CLOSE, D2D1::RectF(g.W - 28 * s - 2 * r, 24 * s, g.W - 28 * s, 24 * s + 2 * r)});
    const float bw = 40 * s, ph = 44 * s;
    const float py = (StripVisible() ? StripY() - 12 * s : g.H - 22 * s) - ph;
    const Btn ids[] = {B_ROTL, B_PREV, B_PLAY, B_NEXT, B_ROTR};
    const float total = 5 * bw;
    float x = g.W / 2.0f - total / 2;
    for (Btn id : ids) {
        g_btns.push_back({id, D2D1::RectF(x, py + (ph - bw) / 2, x + bw, py + (ph + bw) / 2)});
        x += bw;
    }
}

D2D1_RECT_F ControlsPill() {
    const float s = g.s;
    const float bw = 40 * s, ph = 44 * s;
    const float py = (StripVisible() ? StripY() - 12 * s : g.H - 22 * s) - ph;
    return D2D1::RectF(g.W / 2.0f - 2.5f * bw - 6 * s, py, g.W / 2.0f + 2.5f * bw + 6 * s, py + ph);
}

bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

Btn HitButton(float x, float y) {
    if (g_ui.x < 0.3) return B_NONE;
    for (auto& b : g_btns)
        if (InRect(b.r, x, y)) return b.id;
    return B_NONE;
}

int HitThumb(float x, float y) {
    if (g_ui.x < 0.3 || !StripVisible() || !InRect(StripPill(), x, y)) return -1;
    int first, last;
    VisibleThumbs(first, last);
    for (int i = first; i <= last; i++)
        if (InRect(ThumbRect(i), x, y)) return i;
    return -1;
}

bool OverUi(float x, float y) {
    if (g_ui.x < 0.3) return false;
    if (HitButton(x, y) != B_NONE || InRect(ControlsPill(), x, y)) return true;
    return StripVisible() && InRect(StripPill(), x, y);
}

bool OverImage(float x, float y) {
    Entry* e = Cur();
    if (!e || !e->info || !g_view.appeared) return false;
    double ew, eh;
    EffSize(*e, e->userRot, ew, eh);
    const double z = g_view.zoom.x * g_view.scale.x;
    const double cx = g.W / 2.0 + g_view.panX.x + g_view.offX.x, cy = g.H / 2.0 + g_view.panY.x;
    return std::fabs(x - cx) <= ew * z / 2 && std::fabs(y - cy) <= eh * z / 2;
}

// ---------------------------------------------------------------- update

bool Update(double dt) {
    bool anim = false;
    anim |= g_bg.step(dt);
    anim |= g_ui.step(dt);
    if (g_stripDirty) RebuildStrip();
    g_strip.target = StripTarget();
    if (!g_stripSnapped) {
        g_strip.snap(g_strip.target);
        g_stripSnapped = true;
    }
    anim |= g_strip.step(dt);

    View& v = g_view;
    if (!g_drag.active) {
        anim |= v.zoom.step(dt);
        anim |= v.panX.step(dt);
        anim |= v.panY.step(dt);
        anim |= v.offX.step(dt);
    }
    anim |= v.rot.step(dt);
    anim |= v.alpha.step(dt);
    anim |= v.scale.step(dt);
    anim |= v.upgrade.step(dt);
    if (v.upgrade.settled()) v.prev = Bmp();

    for (auto& gh : g_ghosts) {
        gh.alpha.step(dt);
        gh.offX.step(dt);
        gh.scale.step(dt);
    }
    g_ghosts.erase(std::remove_if(g_ghosts.begin(), g_ghosts.end(),
                                  [](const Ghost& gh) { return gh.alpha.x < 0.004 && gh.alpha.target == 0; }),
                   g_ghosts.end());
    if (!g_ghosts.empty()) anim = true;

    // animated GIF: advance frames on their own clock (render only when a frame changes)
    Entry* e = Cur();
    if (e && v.appeared && e->anim.size() > 1 && !g_closing) {
        const double t = Now();
        if (t >= g_animNext) {
            g_animIdx = (g_animIdx + 1) % (int)e->anim.size();
            const double d = e->delays[g_animIdx] / 1000.0;
            g_animNext = (t - g_animNext > 0.25) ? t + d : g_animNext + d;  // catch up without bursts
            g.needsRedraw = true;
        }
    }

    // loading spinner while the current photo hasn't arrived yet
    if (!v.appeared && e && !e->failed && Now() - v.navTime > 0.2) anim = true;

    if (g_closing) {
        anim = true;
        if (!g_quit && ((g_bg.x < 0.004 && v.alpha.x < 0.004) || Now() - g_closeStart > 0.7)) {
            g_quit = true;
            DestroyWindow(g.hwnd);
        }
    }
    return anim || g_drag.active;
}

void UpdateUiTarget() {
    if (g_closing) return;
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(g.hwnd, &p);
    const bool hover = g_ui.x > 0.3 && OverUi((float)p.x, (float)p.y);
    const bool show = (Now() - g_lastActivity < 2.2) || hover || g_drag.active;
    if (!show && g_ui.target > 0.5 && GetForegroundWindow() == g.hwnd) SetCursor(nullptr);  // hide with the controls
    g_ui.target = show ? 1 : 0;
}

// ---------------------------------------------------------------- drawing

void Color(float r, float gg, float b, float a) { g.brush->SetColor(D2D1::ColorF(r, gg, b, a)); }

void DrawImage(const Bmp& b, int imgW, int imgH, int orient, double rotDeg, double zoom, double cx, double cy,
               float alpha, bool hq) {
    if (!b.b || alpha <= 0.002f || imgW <= 0 || imgH <= 0) return;
    const int q = OrientQuarter(orient);
    const bool flip = OrientFlip(orient);
    const double ang = q * 90.0 + rotDeg;
    const double rem = std::fmod(std::fabs(ang), 90.0);
    const bool aligned = rem < 0.01 || rem > 89.99;
    if (hq && aligned) {  // pixel-align the top-left corner when nothing moves
        const bool swap = ((int)std::lround(ang / 90.0)) & 1;
        const double dw = (swap ? imgH : imgW) * zoom, dh = (swap ? imgW : imgH) * zoom;
        cx = std::round(cx - dw / 2) + dw / 2;
        cy = std::round(cy - dh / 2) + dh / 2;
    }
    const D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Scale(flip ? -1.f : 1.f, 1.f) *
                               D2D1::Matrix3x2F::Rotation((float)ang) *
                               D2D1::Matrix3x2F::Scale((float)zoom, (float)zoom) *
                               D2D1::Matrix3x2F::Translation((float)cx, (float)cy);
    g.ctx->SetTransform(m);
    const D2D1_RECT_F dst = D2D1::RectF(-imgW / 2.f, -imgH / 2.f, imgW / 2.f, imgH / 2.f);
    if (aligned) {
        D2D1_INTERPOLATION_MODE im = D2D1_INTERPOLATION_MODE_LINEAR;
        if (hq) im = g.hqOk ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : D2D1_INTERPOLATION_MODE_CUBIC;
        else if (zoom * imgW < b.w * 0.75) im = D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR;
        g.ctx->DrawBitmap(b.b.get(), &dst, alpha, im, nullptr, nullptr);
    } else {
        // Mid-rotation: fill through a bitmap brush so the edges get antialiased.
        D2D1_BITMAP_BRUSH_PROPERTIES1 bbp =
            D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_LINEAR);
        ComPtr<ID2D1BitmapBrush1> br;
        if (SUCCEEDED(g.ctx->CreateBitmapBrush(b.b.get(), &bbp, nullptr, br.put()))) {
            br->SetTransform(D2D1::Matrix3x2F::Scale((float)imgW / b.w, (float)imgH / b.h) *
                             D2D1::Matrix3x2F::Translation(-imgW / 2.f, -imgH / 2.f));
            br->SetOpacity(alpha);
            g.ctx->FillRectangle(dst, br.get());
        }
    }
    g.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void DrawCurrent(bool hq) {
    Entry* e = Cur();
    View& v = g_view;
    if (!e || !v.appeared) return;
    const double z = v.zoom.x * v.scale.x;
    const Bmp* b = (e->anim.size() > 1) ? &e->anim[std::min(g_animIdx, (int)e->anim.size() - 1)] : Choose(*e, e->w * z);
    if (!b) return;
    if (e->anim.size() > 1) {  // frames replace each other directly; no sharpening crossfade
        v.shown = *b;
        v.prev = Bmp();
        DrawImage(*b, e->w, e->h, e->orient, v.rot.x, z, g.W / 2.0 + v.panX.x + v.offX.x, g.H / 2.0 + v.panY.x,
                  (float)std::clamp(v.alpha.x, 0.0, 1.0), hq);
        return;
    }
    if (v.shown.b && v.shown.b != b->b && b->w > v.shown.w) {  // sharper version arrived: crossfade
        v.prev = v.shown;
        v.upgrade.snap(0);
        v.upgrade.target = 1;
    }
    v.shown = *b;
    const double cx = g.W / 2.0 + v.panX.x + v.offX.x, cy = g.H / 2.0 + v.panY.x;
    const float a = (float)std::clamp(v.alpha.x, 0.0, 1.0);
    const bool still = hq && v.upgrade.settled();
    if (v.prev.b && !v.upgrade.settled()) {
        DrawImage(v.prev, e->w, e->h, e->orient, v.rot.x, z, cx, cy, a, false);
        DrawImage(*b, e->w, e->h, e->orient, v.rot.x, z, cx, cy, a * (float)std::clamp(v.upgrade.x, 0.0, 1.0), false);
    } else {
        DrawImage(*b, e->w, e->h, e->orient, v.rot.x, z, cx, cy, a, still);
    }
}

void DrawGhosts() {
    for (auto& gh : g_ghosts) {
        const double cx = g.W / 2.0 + gh.panX + gh.offX.x, cy = g.H / 2.0 + gh.panY;
        DrawImage(gh.bmp, gh.imgW, gh.imgH, gh.orient, gh.rot, gh.zoom * gh.scale.x, cx, cy,
                  (float)std::clamp(gh.alpha.x, 0.0, 1.0), false);
    }
}

void DrawGlyph(ID2D1PathGeometry* geo, float cx, float cy, float size, bool filled, bool mirror, float a) {
    if (!geo) return;
    const float k = size / 24.0f;
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-12, -12) * D2D1::Matrix3x2F::Scale(k, k) *
                         D2D1::Matrix3x2F::Translation(cx, cy);
    if (mirror) m = D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F(12, 12)) * m;
    g.ctx->SetTransform(m);
    Color(1, 1, 1, a);
    if (filled) g.ctx->FillGeometry(geo, g.brush.get(), nullptr);
    else g.ctx->DrawGeometry(geo, g.brush.get(), 1.9f, g.round.get());
    g.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void DrawLabel(const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float w, float a) {
    if (!fmt || !g.dw || text.empty()) return;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(g.dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, w, 200 * g.s, lay.put()))) return;
    Color(0, 0, 0, 0.55f * a);
    g.ctx->DrawTextLayout(D2D1::Point2F(x, y + 1.2f * g.s), lay.get(), g.brush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    Color(1, 1, 1, 0.92f * a);
    g.ctx->DrawTextLayout(D2D1::Point2F(x, y), lay.get(), g.brush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void DrawThumb(const Entry& e, const D2D1_RECT_F& r, float a, bool hq) {
    const Bmp* b = e.lv[0].b ? &e.lv[0] : (e.lv[1].b ? &e.lv[1] : (e.lv[2].b ? &e.lv[2] : nullptr));
    if (!b) {
        Color(1, 1, 1, 0.08f * a);
        g.ctx->FillRectangle(r, g.brush.get());
        return;
    }
    const int q = (OrientQuarter(e.orient) + e.userRot) & 3;
    const bool flip = OrientFlip(e.orient);
    const float rw = r.right - r.left, rh = r.bottom - r.top;
    const float dw = (q & 1) ? rh : rw, dh = (q & 1) ? rw : rh;
    D2D1_RECT_F src;
    if ((float)b->w / b->h > dw / dh) {
        const float cw = b->h * dw / dh;
        src = D2D1::RectF((b->w - cw) / 2, 0, (b->w + cw) / 2, (float)b->h);
    } else {
        const float ch = b->w * dh / dw;
        src = D2D1::RectF(0, (b->h - ch) / 2, (float)b->w, (b->h + ch) / 2);
    }
    g.ctx->SetTransform(D2D1::Matrix3x2F::Scale(flip ? -1.f : 1.f, 1.f) * D2D1::Matrix3x2F::Rotation(q * 90.0f) *
                        D2D1::Matrix3x2F::Translation((r.left + r.right) / 2, (r.top + r.bottom) / 2));
    const D2D1_RECT_F dst = D2D1::RectF(-dw / 2, -dh / 2, dw / 2, dh / 2);
    const D2D1_INTERPOLATION_MODE im = (hq && g.hqOk) ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                                      : D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR;
    g.ctx->DrawBitmap(b->b.get(), &dst, a, im, &src, nullptr);
    g.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void DrawUi(bool hq) {
    const float a = (float)std::clamp(g_ui.x, 0.0, 1.0);
    if (a < 0.003f) return;
    const float s = g.s;
    LayoutButtons();

    // info line (top-left)
    Entry* e = Cur();
    if (e) {
        std::wstring t = FileName(g_curPath);
        wchar_t buf[160];
        swprintf(buf, 160, L"     %d / %d", g_cur + 1, (int)g_files.size());
        t += buf;
        if (e->info) {
            swprintf(buf, 160, L"     %d × %d", e->w, e->h);
            t += buf;
        }
        if (g_view.appeared && (!g_view.fitted || !g_view.zoom.settled())) {
            swprintf(buf, 160, L"     %d%%", (int)std::lround(g_view.zoom.x * 100));
            t += buf;
        }
        if (g_slideshow) t += L"     Slideshow";
        DrawLabel(t, g.font.get(), 28 * s, 30 * s, g.W - 120 * s, a);
    }

    // round buttons
    for (auto& b : g_btns) {
        const float cx = (b.r.left + b.r.right) / 2, cy = (b.r.top + b.r.bottom) / 2;
        const float rad = (b.r.right - b.r.left) / 2;
        if (b.id != B_CLOSE) continue;
        Color(0.08f, 0.08f, 0.09f, 0.55f * a);
        g.ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), g.brush.get());
        if (b.id == g_hoverBtn) {
            Color(1, 1, 1, (g_pressBtn == b.id ? 0.24f : 0.14f) * a);
            g.ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad - 2 * s, rad - 2 * s), g.brush.get());
        }
    }
    // controls pill
    const D2D1_RECT_F cp = ControlsPill();
    Color(0.08f, 0.08f, 0.09f, 0.62f * a);
    g.ctx->FillRoundedRectangle(D2D1::RoundedRect(cp, 22 * s, 22 * s), g.brush.get());
    for (auto& b : g_btns) {  // re-draw hovers above the pill
        if (b.id == B_CLOSE || b.id != g_hoverBtn) continue;
        const float cx = (b.r.left + b.r.right) / 2, cy = (b.r.top + b.r.bottom) / 2;
        const float rad = (b.r.right - b.r.left) / 2 - 2 * s;
        Color(1, 1, 1, (g_pressBtn == b.id ? 0.24f : 0.14f) * a);
        g.ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), g.brush.get());
    }
    const float isz = 22 * s;
    for (auto& b : g_btns) {
        const float cx = (b.r.left + b.r.right) / 2, cy = (b.r.top + b.r.bottom) / 2;
        switch (b.id) {
            case B_CLOSE: DrawGlyph(g.icClose.get(), cx, cy, 20 * s, false, false, 0.95f * a); break;
            case B_PREV: DrawGlyph(g.icPrev.get(), cx, cy, isz, false, false, (g_cur > 0 ? 0.95f : 0.35f) * a); break;
            case B_NEXT:
                DrawGlyph(g.icNext.get(), cx, cy, isz, false, false,
                         (g_cur + 1 < (int)g_files.size() ? 0.95f : 0.35f) * a);
                break;
            case B_PLAY:
                DrawGlyph(g_slideshow ? g.icPause.get() : g.icPlay.get(), cx, cy, isz, true, false,
                         (g_files.size() > 1 ? 0.95f : 0.35f) * a);
                break;
            case B_ROTL: DrawGlyph(g.icRot.get(), cx, cy, isz, false, true, 0.95f * a); break;
            case B_ROTR: DrawGlyph(g.icRot.get(), cx, cy, isz, false, false, 0.95f * a); break;
            default: break;
        }
    }

    // filmstrip
    if (!StripVisible() || g_stripL.size() != g_files.size()) return;
    const D2D1_RECT_F pill = StripPill();
    Color(0.08f, 0.08f, 0.09f, 0.62f * a);
    g.ctx->FillRoundedRectangle(D2D1::RoundedRect(pill, 14 * s, 14 * s), g.brush.get());
    const D2D1_RECT_F clip = D2D1::RectF(pill.left + StripPad() * 0.5f, pill.top, pill.right - StripPad() * 0.5f, pill.bottom);
    g.ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    int first, last;
    VisibleThumbs(first, last);
    const bool stripStill = hq && g_strip.settled();
    for (int i = first; i <= last; i++) {
        const D2D1_RECT_F r = ThumbRect(i);
        Entry* te = Find(g_files[i]);
        const float ta = (i == g_cur ? 1.0f : (i == g_hoverThumb ? 0.95f : 0.62f)) * a;
        if (te) DrawThumb(*te, r, ta, stripStill);
        else {
            Color(1, 1, 1, 0.08f * a);
            g.ctx->FillRectangle(r, g.brush.get());
        }
    }
    g.ctx->PopAxisAlignedClip();
    if (g_cur < (int)g_stripL.size()) {
        D2D1_RECT_F r = ThumbRect(g_cur);
        const float o = 2.5f * s;
        r = D2D1::RectF(r.left - o, r.top - o, r.right + o, r.bottom + o);
        if (r.left > clip.left - o - 1 && r.right < clip.right + o + 1) {
            Color(1, 1, 1, 0.95f * a);
            g.ctx->DrawRoundedRectangle(D2D1::RoundedRect(r, 3 * s, 3 * s), g.brush.get(), 2.0f * s, nullptr);
        }
    }
}

void DrawSpinner() {
    const float s = g.s;
    const double t = Now();
    const float cx = g.W / 2.0f, cy = g.H / 2.0f, r = 14 * s;
    const float fade = (float)std::clamp((t - g_view.navTime - 0.2) / 0.3, 0.0, 1.0);
    for (int i = 0; i < 12; i++) {
        const float a = (float)(i * 3.14159265 * 2 / 12 + t * 7.0);
        const float k = (float)std::fmod(i / 12.0 + t * 1.1, 1.0);
        Color(1, 1, 1, (0.15f + 0.6f * k) * fade * (float)g_bg.x);
        g.ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + r * std::cos(a), cy + r * std::sin(a)), 2.2f * s, 2.2f * s),
                           g.brush.get());
    }
}

void DrawFailed() {
    const float s = g.s;
    const float a = (float)g_bg.x;
    DrawLabel(L"Can’t open this file", g.fontBig.get(), 0, g.H / 2.0f - 28 * s, (float)g.W, a);
    DrawLabel(FileName(g_curPath), g.fontBig.get(), 0, g.H / 2.0f + 4 * s, (float)g.W, 0.6f * a);
}

void ReleaseGpu();
bool RecreateGpu();

void Render(bool hq) {
    g.ctx->BeginDraw();
    g.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    const float bg = (float)std::clamp(g_bg.x, 0.0, 1.0) * kBgAlpha;
    if (g.useDcomp) {
        D2D1_COLOR_F c = D2D1::ColorF(0.02f * bg, 0.02f * bg, 0.025f * bg, bg);  // premultiplied
        g.ctx->Clear(&c);
    } else {
        D2D1_COLOR_F c = D2D1::ColorF(0, 0, 0, 1);
        g.ctx->Clear(&c);
        if (g.desktop) {
            D2D1_RECT_F full = D2D1::RectF(0, 0, (float)g.W, (float)g.H);
            g.ctx->DrawBitmap(g.desktop.get(), &full, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
        }
        Color(0.02f, 0.02f, 0.025f, bg);
        g.ctx->FillRectangle(D2D1::RectF(0, 0, (float)g.W, (float)g.H), g.brush.get());
    }
    DrawGhosts();
    DrawCurrent(hq);
    Entry* e = Cur();
    if (e && e->failed && !g_view.appeared) DrawFailed();
    else if (e && !g_view.appeared && Now() - g_view.navTime > 0.2 && !g_closing) DrawSpinner();
    DrawUi(hq);
    HRESULT hr = g.ctx->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        RecreateGpu();
        return;
    }
    if (FAILED(hr) && hq && g.hqOk) {  // older Direct2D without high-quality cubic
        g.hqOk = false;
        g.needsRedraw = true;
    }
    hr = g.swap->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) RecreateGpu();
}

void ReleaseGpu() {
    for (auto& kv : g_entries)
        for (auto& b : kv.second.lv) b = Bmp();
    g_ghosts.clear();
    g_view.shown = Bmp();
    g_view.prev = Bmp();
    g.desktop.reset();
    g.brush.reset();
    if (g.ctx) g.ctx->SetTarget(nullptr);
    g.target.reset();
    g.dcVisual.reset();
    g.dcTarget.reset();
    g.dc.reset();
    g.swap.reset();
    g.ctx.reset();
    g.dev.reset();
    g.dxgi.reset();
    g.d3d.reset();
    g.frameWait = nullptr;
}

bool RecreateGpu() {
    Trace(L"recreating GPU device");
    ReleaseGpu();
    if (!InitDevice()) return false;
    if (!CreateSwapchain(g.useDcomp)) return false;
    if (g.useDcomp && !SetupDcomp()) return false;
    if (!CreateTarget()) return false;
    UploadDesktop();
    g_view.appeared = false;
    ResetView(APPEAR_FADE, 1);
    RequestDecodes();
    g.needsRedraw = true;
    return true;
}

void OnDisplayChange() {
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int W = mi.rcMonitor.right - mi.rcMonitor.left, H = mi.rcMonitor.bottom - mi.rcMonitor.top;
    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static auto pGetDpi = (PFN_GetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    const float oldS = g.s;
    if (pGetDpi) g.s = pGetDpi(g.hwnd) / 96.0f;
    if (W == g.W && H == g.H && mi.rcMonitor.left == g.monX && mi.rcMonitor.top == g.monY) {
        if (g.s != oldS) {  // same monitor size, new scaling: just re-layout the UI
            InitStatic();
            g_stripDirty = true;
            g.needsRedraw = true;
        }
        return;
    }
    g.W = W;
    g.H = H;
    g.monX = mi.rcMonitor.left;
    g.monY = mi.rcMonitor.top;
    SetWindowPos(g.hwnd, nullptr, g.monX, g.monY, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
    g.ctx->SetTarget(nullptr);
    g.target.reset();
    g.swap->ResizeBuffers(0, W, H, DXGI_FORMAT_UNKNOWN, g.swapFlags);
    CreateTarget();
    g.desktop.reset();
    g.desktopPx.clear();
    InitStatic();
    Entry* e = Cur();
    if (e && g_view.fitted) g_view.zoom.snap(FitZoom(*e));
    g_stripDirty = true;
    g.needsRedraw = true;
}

// ---------------------------------------------------------------- files

struct ListMsg {
    uint32_t gen;
    std::wstring target;
    std::vector<std::wstring> files;
    std::vector<std::wstring> exts;  // filled when the scan had to discover codecs itself
};

void ScanFolderAsync(const std::wstring& file) {
    const uint32_t gen = ++g_scanGen;
    HWND hwnd = g.hwnd;
    std::vector<std::wstring> exts = g_exts;
    std::thread([gen, file, hwnd, exts]() mutable {
        auto* msg = new ListMsg{gen, file, {}, {}};
        if (exts.empty()) {  // first scan: ask Windows which codecs exist (kept off the startup path)
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            exts = SupportedExtensions();
            msg->exts = exts;
        }
        const std::wstring dir = DirName(file);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                    continue;
                if (!HasExtension(fd.cFileName, exts)) continue;
                msg->files.push_back(dir + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(msg->files.begin(), msg->files.end(),
                  [](const std::wstring& a, const std::wstring& b) {
                      return StrCmpLogicalW(FileName(a).c_str(), FileName(b).c_str()) < 0;
                  });
        if (!PostMessageW(hwnd, WM_APP_LIST, 0, (LPARAM)msg)) delete msg;
    }).detach();
}

void OnList(ListMsg* m) {
    if (m->gen != g_scanGen) {
        delete m;
        return;
    }
    std::vector<std::wstring> files = std::move(m->files);
    if (g_exts.empty() && !m->exts.empty()) {
        g_exts = std::move(m->exts);
        if (g_trace) {
            std::wstring all;
            for (auto& e : g_exts) all += e + L" ";
            Trace(L"codecs: %ls", all.c_str());
        }
    }
    delete m;
    int idx = -1;
    for (int i = 0; i < (int)files.size(); i++)
        if (lstrcmpiW(files[i].c_str(), g_curPath.c_str()) == 0) {
            idx = i;
            break;
        }
    if (idx < 0) {  // opened something the folder scan didn't list; keep it in sequence
        auto it = std::lower_bound(files.begin(), files.end(), g_curPath, [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(FileName(a).c_str(), FileName(b).c_str()) < 0;
        });
        idx = (int)(it - files.begin());
        files.insert(it, g_curPath);
    } else if (files[idx] != g_curPath) {  // same file, different case: keep our key
        files[idx] = g_curPath;
    }
    g_files = std::move(files);
    g_fileIndex.clear();
    for (int i = 0; i < (int)g_files.size(); i++) g_fileIndex[g_files[i]] = i;
    g_cur = idx;
    g_stripDirty = true;
    g_stripSnapped = false;
    Evict();
    Trace(L"folder listed: %d images", (int)g_files.size());
    RequestDecodes();
    g.needsRedraw = true;
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(p.c_str(), 32768, buf, nullptr);
    return (n && n < 32768) ? std::wstring(buf, n) : p;
}

void OpenFile(const std::wstring& path, bool first) {
    const std::wstring p = FullPath(path);
    if (!first) {
        PushGhost(1, false);
        if (!g_ghosts.empty()) g_ghosts.back().alpha.params(0.28, 1.0);  // quick fade-out of the old photo
    }
    g_files = {p};
    g_fileIndex.clear();
    g_fileIndex[p] = 0;
    g_cur = 0;
    g_curPath = p;
    g_stripDirty = true;
    g_stripSnapped = false;
    if (!first) ResetView(APPEAR_OPEN, 1);  // eviction waits for the folder list (same folder keeps its cache)
    RequestDecodes();
    if (g.hwnd) ScanFolderAsync(p);
}

std::wstring AskForFile() {
    std::wstring pattern;
    for (auto& e : g_exts) {
        if (!pattern.empty()) pattern += L";";
        pattern += L"*" + e;
    }
    std::wstring filter = L"Images";
    filter.push_back(L'\0');
    filter += pattern;
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = L"Open a photo with Glance";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return L"";
    return buf.data();
}

// "Open with" registration (per user, no admin rights needed).
void SetRegStr(const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

int Register(bool add) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring exeName = PathFindFileNameW(exe);
    const std::wstring base = L"Software\\Classes\\";
    const std::wstring progId = L"Glance.Image";
    if (add) {
        const std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"%1\"";
        SetRegStr(base + progId, nullptr, L"Image (Glance)");
        SetRegStr(base + progId + L"\\DefaultIcon", nullptr, std::wstring(exe) + L",0");
        SetRegStr(base + progId + L"\\shell\\open\\command", nullptr, cmd);
        SetRegStr(base + L"Applications\\" + exeName, L"FriendlyAppName", L"Glance");
        SetRegStr(base + L"Applications\\" + exeName + L"\\shell\\open\\command", nullptr, cmd);
        for (auto& e : g_exts) {
            SetRegStr(base + e + L"\\OpenWithProgids", progId.c_str(), L"");
            SetRegStr(base + L"Applications\\" + exeName + L"\\SupportedTypes", e.c_str(), L"");
        }
    } else {
        RegDeleteTreeW(HKEY_CURRENT_USER, (base + progId).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, (base + L"Applications\\" + exeName).c_str());
        for (auto& e : g_exts) {
            HKEY k;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (base + e + L"\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &k) ==
                ERROR_SUCCESS) {
                RegDeleteValueW(k, progId.c_str());
                RegCloseKey(k);
            }
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    MessageBoxW(nullptr,
                add ? L"Glance is registered.\n\nRight-click a photo → Open with → Choose another app → Glance, "
                      L"and tick “Always” to make it your default viewer."
                    : L"Glance has been removed from “Open with”.",
                kAppName, MB_OK | MB_ICONINFORMATION);
    return 0;
}

// ---------------------------------------------------------------- input

void OnButton(Btn b) {
    switch (b) {
        case B_CLOSE: Close(); break;
        case B_PREV: Navigate(g_cur - 1, APPEAR_SLIDE); break;
        case B_NEXT: Navigate(g_cur + 1, APPEAR_SLIDE); break;
        case B_PLAY: ToggleSlideshow(); break;
        case B_ROTL: Rotate(-1); break;
        case B_ROTR: Rotate(1); break;
        default: break;
    }
}

void UpdateHover(float x, float y) {
    Btn hb = HitButton(x, y);
    int ht = hb == B_NONE ? HitThumb(x, y) : -1;
    if (hb != g_hoverBtn || ht != g_hoverThumb) {
        g_hoverBtn = hb;
        g_hoverThumb = ht;
        g.needsRedraw = true;
    }
}

void Activity() { g_lastActivity = Now(); }

void OnMouseDown(int x, int y) {
    Activity();
    g_drag = Drag();
    g_pressBtn = HitButton((float)x, (float)y);
    if (g_pressBtn != B_NONE || HitThumb((float)x, (float)y) >= 0 || OverUi((float)x, (float)y)) {
        g.needsRedraw = true;
        SetCapture(g.hwnd);
        g_drag.down = true;
        g_drag.ignoreUp = false;
        g_drag.start = {x, y};
        return;
    }
    g_drag.down = true;
    g_drag.start = {x, y};
    g_drag.startPanX = g_view.panX.x;
    g_drag.startPanY = g_view.panY.x;
    g_drag.startOff = g_view.offX.x;
    g_drag.hist.push_back({Now(), POINT{x, y}});
    SetCapture(g.hwnd);
}

double Rubber(double v, double lo, double hi) {
    if (v < lo) return lo - (lo - v) * 0.33;
    if (v > hi) return hi + (v - hi) * 0.33;
    return v;
}

void OnMouseMove(int x, int y) {
    if (x != g_lastMouse.x || y != g_lastMouse.y) {
        g_lastMouse = {x, y};
        if (g_ui.target < 0.5 && !g_closing) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        Activity();
    }
    UpdateHover((float)x, (float)y);
    if (!g_drag.down || g_pressBtn != B_NONE || g_closing) return;
    Entry* e = Cur();
    if (!e) return;
    const int dx = x - g_drag.start.x, dy = y - g_drag.start.y;
    if (!g_drag.active) {
        if (dx * dx + dy * dy < (int)(25 * g.s * g.s)) return;
        if (OverUi((float)g_drag.start.x, (float)g_drag.start.y)) return;
        g_drag.active = true;
        // Not loaded yet (or unreadable): a drag can still swipe to the neighbour.
        g_drag.swipe = !g_view.appeared || !e->info || !LargerThanScreen(*e);
        if (g_view.appeared && e->info) {
            g_view.zoom.snap(g_view.zoom.x);  // freeze any zoom in flight where it is
            g_view.fitted = std::fabs(g_view.zoom.x - FitZoom(*e)) < 1e-3;
        }
    }
    g_drag.hist.push_back({Now(), POINT{x, y}});
    while (g_drag.hist.size() > 2 && Now() - g_drag.hist.front().first > 0.1) g_drag.hist.pop_front();
    if (g_drag.swipe) {
        double off = g_drag.startOff + dx;
        const bool hasNext = g_cur + 1 < (int)g_files.size(), hasPrev = g_cur > 0;
        if ((off < 0 && !hasNext) || (off > 0 && !hasPrev)) off *= 0.33;
        g_view.offX.snap(off);
    } else {
        double ew, eh;
        EffSize(*e, e->userRot, ew, eh);
        const double z = g_view.zoom.x;
        const double mx = std::max(0.0, (ew * z - g.W) / 2), my = std::max(0.0, (eh * z - g.H) / 2);
        g_view.panX.snap(Rubber(g_drag.startPanX + dx, -mx, mx));
        g_view.panY.snap(Rubber(g_drag.startPanY + dy, -my, my));
    }
    g.needsRedraw = true;
}

void OnMouseUp(int x, int y) {
    // Take the drag state *before* releasing capture: ReleaseCapture sends
    // WM_CAPTURECHANGED synchronously, which would otherwise wipe it.
    const Drag d = g_drag;
    g_drag = Drag();
    g_releasing = true;
    ReleaseCapture();
    g_releasing = false;
    if (!d.down) return;
    if (d.ignoreUp) return;
    if (g_pressBtn != B_NONE) {
        Btn b = g_pressBtn;
        g_pressBtn = B_NONE;
        if (HitButton((float)x, (float)y) == b) OnButton(b);
        g.needsRedraw = true;
        return;
    }
    if (!d.active) {
        int t = HitThumb((float)x, (float)y);
        if (t >= 0) {
            if (t != g_cur) Navigate(t, APPEAR_SLIDE);
            return;
        }
        if (OverUi((float)x, (float)y)) return;
        if (!OverImage((float)x, (float)y)) Close();  // click on the dark background closes, like Picasa
        return;
    }
    // release velocity (px/s)
    double vx = 0, vy = 0;
    if (d.hist.size() >= 2) {
        const auto& a = d.hist.front();
        const auto& b = d.hist.back();
        const double dt = std::max(1e-3, b.first - a.first);
        if (Now() - b.first < 0.08) {
            vx = (b.second.x - a.second.x) / dt;
            vy = (b.second.y - a.second.y) / dt;
        }
    }
    Entry* e = Cur();
    if (!e) return;
    if (d.swipe) {
        const double off = g_view.offX.x;
        const bool hasNext = g_cur + 1 < (int)g_files.size(), hasPrev = g_cur > 0;
        const double th = g.W * 0.1, vth = 700 * g.s;
        g_view.offX.v = vx;
        if (hasNext && (off < -th || (vx < -vth && off < 0))) Navigate(g_cur + 1, APPEAR_SLIDE);
        else if (hasPrev && (off > th || (vx > vth && off > 0))) Navigate(g_cur - 1, APPEAR_SLIDE);
        else {
            g_view.offX.params(0.38, 0.82);
            g_view.offX.target = 0;
        }
    } else {
        // momentum: project where a flick would coast to, then spring there
        double tx = g_view.panX.x + vx * 0.24, ty = g_view.panY.x + vy * 0.24;
        ClampPan(*e, g_view.zoom.x, tx, ty);
        g_view.panX.params(0.55, 0.92);
        g_view.panY.params(0.55, 0.92);
        g_view.panX.v = vx;
        g_view.panY.v = vy;
        g_view.panX.target = tx;
        g_view.panY.target = ty;
    }
    g.needsRedraw = true;
}

void OnDoubleClick(int x, int y) {
    g_drag.down = true;
    g_drag.ignoreUp = true;
    SetCapture(g.hwnd);
    if (OverUi((float)x, (float)y)) {
        g_drag.ignoreUp = false;
        OnMouseDown(x, y);
        return;
    }
    Entry* e = Cur();
    if (!e || !e->info || !g_view.appeared) return;
    const double fit = FitZoom(*e);
    if (g_view.fitted && fit < 0.95) ZoomTo(1.0, x, y, 0.38, 0.86);
    else ZoomTo(fit, x, y, 0.38, 0.86);
}

void OnKey(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    const int n = (int)g_files.size();
    switch (vk) {
        case VK_ESCAPE:
            if (g_slideshow) ToggleSlideshow();
            else Close();
            break;
        case VK_RIGHT: case VK_NEXT: case VK_SPACE:
            if (g_slideshow) g_slideshow = false;
            Navigate(g_cur + 1, APPEAR_SLIDE);
            break;
        case VK_LEFT: case VK_PRIOR: case VK_BACK:
            if (g_slideshow) g_slideshow = false;
            Navigate(g_cur - 1, APPEAR_SLIDE);
            break;
        case VK_HOME: Navigate(0, APPEAR_SLIDE); break;
        case VK_END: Navigate(n - 1, APPEAR_SLIDE); break;
        case VK_UP: case VK_DOWN: {
            Entry* e = Cur();
            if (e && !g_view.fitted) {
                double tx = g_view.panX.target, ty = g_view.panY.target + (vk == VK_UP ? 1 : -1) * g.H * 0.15;
                ClampPan(*e, g_view.zoom.target, tx, ty);
                g_view.panY.params(0.3, 0.95);
                g_view.panY.target = ty;
                g.needsRedraw = true;
            }
            break;
        }
        case 'R': Rotate(shift ? -1 : 1); break;
        case 'L': Rotate(-1); break;
        case VK_OEM_6: Rotate(1); break;   // ]
        case VK_OEM_4: Rotate(-1); break;  // [
        case VK_OEM_PLUS: case VK_ADD: ZoomBy(1.4, g.W / 2.0, g.H / 2.0); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: ZoomBy(1 / 1.4, g.W / 2.0, g.H / 2.0); break;
        case '0': case VK_NUMPAD0: ZoomToFit(); break;
        case '1': case VK_NUMPAD1: ZoomTo(1.0, g.W / 2.0, g.H / 2.0, 0.38, 0.86); break;
        case VK_F5: case 'S': ToggleSlideshow(); break;
        case 'W':
            if (ctrl) Close();
            break;
        default: return;
    }
    Activity();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_DECODED: ProcessResults(); return 0;
        case WM_APP_LIST: OnList(reinterpret_cast<ListMsg*>(lp)); return 0;
        case WM_COPYDATA: {  // another launch handing us a file to show
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (g_closing || !cds || cds->dwData != kCopyDataOpen || cds->cbData < sizeof(wchar_t)) return FALSE;
            std::wstring path(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
            while (!path.empty() && path.back() == L'\0') path.pop_back();
            if (path.empty()) return FALSE;
            OpenFile(path, false);
            Activity();
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        case WM_TIMER:
            SlideshowTick();
            return 0;
        case WM_KEYDOWN:
            OnKey(wp);
            return 0;
        case WM_LBUTTONDOWN: OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP: OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONDBLCLK: OnDoubleClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_RBUTTONUP: Close(); return 0;
        case WM_MBUTTONUP: Close(); return 0;
        case WM_XBUTTONUP:
            Navigate(g_cur + (GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? -1 : 1), APPEAR_SLIDE);
            return TRUE;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd && g_drag.down && !g_releasing) {  // capture stolen (e.g. Alt+Tab)
                g_drag = Drag();
                g_pressBtn = B_NONE;
                g_view.offX.target = 0;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            Activity();
            const double delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.0;
            ZoomBy(std::pow(1.2, delta), p.x, p.y);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            Entry* e = Cur();
            if (e && !g_view.fitted) {
                double tx = g_view.panX.target - GET_WHEEL_DELTA_WPARAM(wp) / 120.0 * 80 * g.s, ty = g_view.panY.target;
                ClampPan(*e, g_view.zoom.target, tx, ty);
                g_view.panX.target = tx;
                g.needsRedraw = true;
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                if (g_ui.target < 0.5 && !g_drag.active && !g_closing) SetCursor(nullptr);
                else if (g_hoverBtn != B_NONE || g_hoverThumb >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
                else if (g_drag.active && !g_drag.swipe) SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                else SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            wchar_t path[32768];
            if (DragQueryFileW(drop, 0, path, 32768)) OpenFile(path, false);
            DragFinish(drop);
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            if (g.swap) OnDisplayChange();
            return 0;
        case WM_CLOSE: Close(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            g.needsRedraw = true;
            return 0;
        case WM_DESTROY:
            if (hwnd == g.hwnd) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateMainWindow(DWORD exStyle) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"GlanceViewer";
        RegisterClassExW(&wc);
        registered = true;
    }
    return CreateWindowExW(exStyle | WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, L"GlanceViewer", kAppName, WS_POPUP,
                           g.monX, g.monY, g.W, g.H, nullptr, nullptr, inst, nullptr);
}

void SetDpiAwareness() {
    typedef BOOL(WINAPI * PFN)(HANDLE);
    auto p = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (p) p((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    else SetProcessDPIAware();
}

void PickMonitor() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g.monX = mi.rcMonitor.left;
    g.monY = mi.rcMonitor.top;
    g.W = mi.rcMonitor.right - mi.rcMonitor.left;
    g.H = mi.rcMonitor.bottom - mi.rcMonitor.top;
    UINT dx = 96, dy = 96;
    typedef HRESULT(WINAPI * PFN)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    auto p = shcore ? (PFN)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    if (!p || FAILED(p(mon, 0, &dx, &dy))) {
        HDC dc = GetDC(nullptr);
        dx = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    g.s = dx / 96.0f;
}

double ProcessAgeSeconds() {
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = c.dwLowDateTime;
    a.HighPart = c.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 1e7 : 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;
    g_traceBase = Now() - ProcessAgeSeconds();
    wchar_t env[8];
    g_trace = GetEnvironmentVariableW(L"GLANCE_TRACE", env, 8) > 0;

    SetDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring file;
    bool noDcomp = false, doRegister = false, doUnregister = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--register") doRegister = true;
        else if (a == L"--unregister") doUnregister = true;
        else if (a == L"--no-composition") noDcomp = true;
        else if (a == L"--trace") g_trace = true;
        else if (file.empty()) file = a;
    }
    if (argv) LocalFree(argv);
    Trace(L"---- start");

    // Already open? Hand the photo to that window; it shows it instantly.
    if (!file.empty() && !doRegister && !doUnregister) {
        if (HWND other = FindWindowW(L"GlanceViewer", nullptr)) {
            const std::wstring full = FullPath(file);
            COPYDATASTRUCT cds{kCopyDataOpen, (DWORD)((full.size() + 1) * sizeof(wchar_t)), (void*)full.c_str()};
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            DWORD_PTR res = 0;
            if (SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 1500, &res) && res) {
                Trace(L"handed off to the open viewer");
                return 0;
            }
        }
    }

    // Start decoding the requested photo before anything else is set up.
    PickMonitor();
    unsigned hw = std::thread::hardware_concurrency();
    if (!file.empty()) {
        g_curPath = FullPath(file);
        g_files = {g_curPath};
        g_fileIndex[g_curPath] = 0;
        g.maxBitmap = 16384;
        g_loader.start((int)std::clamp(hw ? hw - 1 : 2u, 2u, 4u));
        RequestDecodes();
    }
    Trace(L"decode started");
    if (doRegister || doUnregister || file.empty()) g_exts = SupportedExtensions();  // otherwise found off-thread
    if (doRegister || doUnregister) return Register(doRegister);
    if (file.empty()) {
        file = AskForFile();
        if (file.empty()) return 0;
        g_curPath = FullPath(file);
        g_files = {g_curPath};
        g_fileIndex[g_curPath] = 0;
        g.maxBitmap = 16384;
        g_loader.start((int)std::clamp(hw ? hw - 1 : 2u, 2u, 4u));
        RequestDecodes();
    }

    if (!InitDevice()) {
        MessageBoxW(nullptr, L"Couldn't start Direct3D/Direct2D on this PC.", kAppName, MB_ICONERROR);
        return 1;
    }
    Trace(L"GPU device ready");
    InitStatic();

    HMODULE dcomp = LoadLibraryW(L"dcomp.dll");
    g.dcompCreate = dcomp ? (PFN_DCompCreate)GetProcAddress(dcomp, "DCompositionCreateDevice") : nullptr;
    bool ok = false;
    if (!noDcomp && g.dcompCreate) {
        g.hwnd = CreateMainWindow(WS_EX_NOREDIRECTIONBITMAP);
        g.useDcomp = true;
        if (g.hwnd && CreateSwapchain(true) && SetupDcomp() && CreateTarget()) ok = true;
        if (!ok) {
            g.dcVisual.reset();
            g.dcTarget.reset();
            g.dc.reset();
            if (g.ctx) g.ctx->SetTarget(nullptr);
            g.target.reset();
            g.swap.reset();
            HWND old = g.hwnd;
            g.hwnd = nullptr;
            if (old) DestroyWindow(old);
        }
    }
    if (!ok) {
        g.useDcomp = false;
        CaptureDesktop();
        g.hwnd = CreateMainWindow(0);
        if (!g.hwnd || !CreateSwapchain(false) || !CreateTarget()) {
            MessageBoxW(nullptr, L"Couldn't create the viewer window.", kAppName, MB_ICONERROR);
            return 1;
        }
        UploadDesktop();
    }
    Trace(L"window ready (%ls)", g.useDcomp ? L"composition" : L"desktop snapshot");

    g_bg.snap(0);
    g_bg.target = 1;
    g_ui.snap(0);
    g_lastActivity = Now() - 0.6;  // controls show briefly, then get out of the way
    ResetView(APPEAR_OPEN, 1);
    SetTimer(g.hwnd, TIMER_TICK, 100, nullptr);
    g_loader.setNotify(g.hwnd, WM_APP_DECODED);
    if (g.frameWait) WaitForSingleObjectEx(g.frameWait, 200, TRUE);
    Render(false);
    ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);
    Trace(L"window shown");
    ScanFolderAsync(g_curPath);

    MSG msg;
    bool running = true, lastAnim = true;
    double last = Now();
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        // Wait for the display's next refresh slot *before* computing the frame,
        // so positions are sampled at the moment they'll actually be shown.
        if ((lastAnim || g.needsRedraw) && g.frameWait) WaitForSingleObjectEx(g.frameWait, 100, TRUE);
        const double t = Now();
        const double dt = std::clamp(t - last, 0.0, 1.0 / 24);
        last = t;
        UpdateUiTarget();
        const bool anim = Update(dt);
        if (g_quit || !g.swap) break;
        if (anim || g.needsRedraw || lastAnim) {
            g.needsRedraw = false;
            const double f0 = Now();
            Render(!anim);
            lastAnim = anim;
            if (!g.frameWait && Now() - f0 < 0.004) Sleep(8);  // no vsync wait available
        } else {
            // Idle: sleep until input arrives (or the next GIF frame is due).
            DWORD timeout = INFINITE;
            Entry* e = Cur();
            if (e && e->anim.size() > 1 && g_view.appeared)
                timeout = (DWORD)std::clamp((g_animNext - Now()) * 1000.0, 0.0, 1000.0);
            MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            last = Now();
        }
    }
    Trace(L"exit");
    ExitProcess(0);  // don't wait for in-flight decodes
    return 0;
}
