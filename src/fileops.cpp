#include "fileops.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "com.h"

namespace {

WICBitmapTransformOptions OrientationTransform(int o) {
    switch (o) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

int ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(q.put())) || !q) return 1;
    for (const wchar_t* n : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation"}) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        int v = 0;
        if (SUCCEEDED(q->GetMetadataByName(n, &pv))) {
            if (pv.vt == VT_UI2) v = pv.uiVal;
            else if (pv.vt == VT_UI4) v = (int)pv.ulVal;
        }
        PropVariantClear(&pv);
        if (v >= 1 && v <= 8) return v;
    }
    return 1;
}

}  // namespace

bool HasRecycleBin(const std::wstring& path) {
    wchar_t root[MAX_PATH];
    lstrcpynW(root, path.c_str(), MAX_PATH);
    if (!PathStripToRootW(root)) return false;
    if (GetDriveTypeW(root) == DRIVE_REMOTE || PathIsUNCW(path.c_str())) return false;
    SHQUERYRBINFO info{sizeof(info)};
    return SUCCEEDED(SHQueryRecycleBinW(root, &info));
}

bool DeleteForUser(const std::wstring& path) {
    // Recycle Bin when the drive has one and it's switched on; otherwise Windows
    // deletes permanently (no confirmation: Glance's own Ctrl+Z grace period
    // has already given you the chance to change your mind).
    ComPtr<IFileOperation> op;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, __uuidof(IFileOperation), op.putVoid())) &&
        op) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, __uuidof(IShellItem), item.putVoid()))) {
            op->SetOperationFlags(FOF_NO_UI | FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE);
            HRESULT hr = op->DeleteItem(item.get(), nullptr);
            if (SUCCEEDED(hr)) hr = op->PerformOperations();
            BOOL aborted = FALSE;
            op->GetAnyOperationsAborted(&aborted);
            if (SUCCEEDED(hr) && !aborted && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
        }
    }
    // Older shell (or Wine): the classic API.
    std::wstring from = path;
    from.push_back(L'\0');  // double-null terminated list
    SHFILEOPSTRUCTW fo{};
    fo.wFunc = FO_DELETE;
    fo.pFrom = from.c_str();
    fo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&fo) == 0 && !fo.fAnyOperationsAborted &&
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true;
    return DeleteFileW(path.c_str()) != 0;
}

void ShowInExplorer(const std::wstring& path) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        if (FAILED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0))) {
            const std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ILFree(pidl);
    }
    CoUninitialize();
}

void PrepareClipboard(const std::wstring& path, int quarterTurns, ClipData& out) {
    out.path = path;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                f.putVoid())))
        return;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            dec.put())) ||
        FAILED(dec->GetFrame(0, frame.put())))
        return;
    GUID container{};
    dec->GetContainerFormat(&container);
    const int orient = container == GUID_ContainerFormatHeif ? 1 : ReadOrientation(frame.get());

    ComPtr<IWICBitmapSource> src(frame.get());
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    const double maxPx = 120e6;  // keep the clipboard copy within ~480 MB
    if ((double)w * h > maxPx) {
        const double k = std::sqrt(maxPx / ((double)w * h));
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(f->CreateBitmapScaler(sc.put())) ||
            FAILED(sc->Initialize(src.get(), std::max(1u, (UINT)(w * k)), std::max(1u, (UINT)(h * k)),
                                  WICBitmapInterpolationModeFant)))
            return;
        src = ComPtr<IWICBitmapSource>(sc.get());
    }
    // Flip/rotate needs a format it can address per pixel.
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut)))
        return;
    src = ComPtr<IWICBitmapSource>(conv.get());
    for (WICBitmapTransformOptions t : {OrientationTransform(orient), (WICBitmapTransformOptions)(
                                                                          (quarterTurns & 3) == 1 ? WICBitmapTransformRotate90
                                                                          : (quarterTurns & 3) == 2 ? WICBitmapTransformRotate180
                                                                          : (quarterTurns & 3) == 3 ? WICBitmapTransformRotate270
                                                                                                    : WICBitmapTransformRotate0)}) {
        if (t == WICBitmapTransformRotate0) continue;
        ComPtr<IWICBitmapFlipRotator> fr;
        if (FAILED(f->CreateBitmapFlipRotator(fr.put())) || FAILED(fr->Initialize(src.get(), t))) return;
        src = ComPtr<IWICBitmapSource>(fr.get());
    }
    src->GetSize(&w, &h);
    const size_t stride = (size_t)w * 4;
    std::vector<uint8_t> px;
    try {
        px.resize(stride * h);
    } catch (...) {
        return;
    }
    if (FAILED(src->CopyPixels(nullptr, (UINT)stride, (UINT)px.size(), px.data()))) return;

    bool alpha = false;
    for (size_t i = 3; i < px.size(); i += 4)
        if (px[i] != 255) { alpha = true; break; }

    // CF_DIB: header + bottom-up rows.
    BITMAPINFOHEADER bh{};
    bh.biSize = sizeof(bh);
    bh.biWidth = (LONG)w;
    bh.biHeight = (LONG)h;
    bh.biPlanes = 1;
    bh.biBitCount = 32;
    bh.biCompression = BI_RGB;
    bh.biSizeImage = (DWORD)(stride * h);
    try {
        out.dib.resize(sizeof(bh) + stride * h);
    } catch (...) {
        return;
    }
    memcpy(out.dib.data(), &bh, sizeof(bh));
    for (UINT y = 0; y < h; y++) memcpy(&out.dib[sizeof(bh) + (h - 1 - y) * stride], &px[y * stride], stride);

    if (alpha) {  // keep transparency for apps that read PNG from the clipboard
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fe;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) &&
            SUCCEEDED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(fe.put(), nullptr)) && SUCCEEDED(fe->Initialize(nullptr)) &&
            SUCCEEDED(fe->SetSize(w, h))) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            if (SUCCEEDED(fe->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat32bppBGRA &&
                SUCCEEDED(fe->WritePixels(h, (UINT)stride, (UINT)px.size(), px.data())) && SUCCEEDED(fe->Commit()) &&
                SUCCEEDED(enc->Commit())) {
                HGLOBAL hg = nullptr;
                if (SUCCEEDED(GetHGlobalFromStream(stream.get(), &hg)) && hg) {
                    STATSTG st{};
                    stream->Stat(&st, STATFLAG_NONAME);
                    const void* p = GlobalLock(hg);
                    if (p) {
                        out.png.assign((const uint8_t*)p, (const uint8_t*)p + st.cbSize.QuadPart);
                        GlobalUnlock(hg);
                    }
                }
            }
        }
    }
    out.ok = true;
}

static bool PutBytes(UINT format, const void* data, size_t size) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!g) return false;
    void* p = GlobalLock(g);
    if (!p) {
        GlobalFree(g);
        return false;
    }
    memcpy(p, data, size);
    GlobalUnlock(g);
    if (!SetClipboardData(format, g)) {
        GlobalFree(g);
        return false;
    }
    return true;
}

static bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);  // another app may hold it briefly
    }
    return false;
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboardRetry(owner)) return false;
    EmptyClipboard();
    const bool ok = PutBytes(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    CloseClipboard();
    return ok;
}

std::wstring GetClipboardText(HWND owner) {
    std::wstring out;
    if (!OpenClipboardRetry(owner)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

bool SetClipboard(HWND owner, const ClipData& data) {
    if (!data.ok || !OpenClipboardRetry(owner)) return false;
    EmptyClipboard();
    bool ok = true;
    // The file itself, so pasting into Explorer, chat apps or mail attaches the original.
    {
        std::vector<uint8_t> drop(sizeof(DROPFILES) + (data.path.size() + 2) * sizeof(wchar_t), 0);
        auto* df = reinterpret_cast<DROPFILES*>(drop.data());
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(drop.data() + sizeof(DROPFILES), data.path.c_str(), data.path.size() * sizeof(wchar_t));
        ok &= PutBytes(CF_HDROP, drop.data(), drop.size());
        const DWORD effect = DROPEFFECT_COPY;
        PutBytes(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), &effect, sizeof(effect));
    }
    ok &= PutBytes(CF_DIB, data.dib.data(), data.dib.size());
    if (!data.png.empty()) PutBytes(RegisterClipboardFormatW(L"PNG"), data.png.data(), data.png.size());
    CloseClipboard();
    return ok;
}

DWORD RenameForUser(const std::wstring& from, const std::wstring& to) {
    DWORD err = 0;
    for (int i = 0; i < 15; i++) {
        // No REPLACE_EXISTING: a name that's taken is reported, never overwritten.
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) {
            SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW, from.c_str(), to.c_str());
            return 0;
        }
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) break;
        Sleep(30);  // our own decoder may still have the file open for a moment
    }
    return err;
}
