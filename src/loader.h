// Background image decoding with Windows Imaging Component (WIC).
//
// The main thread describes *what it wants right now* (current photo first,
// then neighbours, then filmstrip thumbnails) and the loader's worker threads
// pull the most urgent job. Results come back as CPU pixel buffers that the
// main thread uploads to the GPU.
#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

enum DecodeLevel { LV_THUMB = 0, LV_FIT = 1, LV_FULL = 2 };

struct DecodeRequest {
    std::wstring path;
    int level = LV_FIT;
    int boxW = 0, boxH = 0;  // THUMB/FIT: bounding box (display orientation). FULL: max side.
    bool fastOnly = false;   // THUMB: return info only if no cheap preview exists
    bool wantThumb = false;  // FIT/FULL: also produce a small thumbnail from the result
    int priority = 0;        // lower = sooner
};

struct DecodeResult {
    std::wstring path;
    int level = LV_FIT;
    bool ok = false;      // pixels valid
    bool infoOk = false;  // dimensions/orientation valid
    bool infoOnly = false;  // THUMB with fastOnly: no cheap preview existed (not an error)
    int imgW = 0, imgH = 0, orientation = 1, frames = 1;
    int pxW = 0, pxH = 0;
    std::vector<uint8_t> pixels;  // 32bpp premultiplied BGRA, stride = pxW*4
    int thW = 0, thH = 0;
    std::vector<uint8_t> thumb;   // optional by-product thumbnail
    std::vector<std::vector<uint8_t>> animFrames;  // animated GIF: composited frames (pxW x pxH each)
    std::vector<int> animDelays;                    // per-frame display time, ms
    double ms = 0;                // decode time, for tracing
};

class Loader {
public:
    void start(int threads);
    void setNotify(HWND wnd, UINT msg);
    void setRequests(std::vector<DecodeRequest> reqs);
    std::vector<DecodeResult> takeResults();

private:
    void workerMain();
    static std::wstring key(const std::wstring& path, int level);

    HWND wnd_ = nullptr;
    UINT msg_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<DecodeRequest> pending_;
    std::unordered_set<std::wstring> inflight_;
    std::unordered_set<std::wstring> ready_;
    std::vector<DecodeResult> results_;
    std::vector<std::thread> threads_;
    bool posted_ = false;
};

// Lower-case extensions (".jpg") of every decoder installed on this PC.
std::vector<std::wstring> SupportedExtensions();
bool HasExtension(const std::wstring& path, const std::vector<std::wstring>& exts);
