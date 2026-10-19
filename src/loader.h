// Background image decoding with Windows Imaging Component (WIC).
//
// The main thread describes *what it wants right now* (current photo first,
// then neighbours, then filmstrip thumbnails) and the loader's worker threads
// pull the most urgent job. Results come back as CPU pixel buffers that the
// main thread uploads to the GPU.
//
// Work that stops being wanted is cancelled mid-decode, and background work
// is paused (and re-queued) when the photo you're looking at needs a worker.
#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum DecodeLevel { LV_THUMB = 0, LV_FIT = 1, LV_FULL = 2, LV_META = 3 };

// Full-resolution images are streamed as horizontal bands and cut into tiles
// of kTile x kTile pixels, each with a kGutter-pixel border copied from its
// neighbours so filtering across tile edges leaves no seams.
constexpr int kTile = 512;
constexpr int kGutter = 4;

struct DecodeRequest {
    std::wstring path;
    int level = LV_FIT;
    int boxW = 0, boxH = 0;  // THUMB/FIT: bounding box (display orientation)
    bool fastOnly = false;   // THUMB: return info only if no cheap preview exists
    bool wantThumb = false;  // FIT: also produce a small thumbnail from the result
    bool shellFirst = false; // THUMB: try the Windows thumbnail cache before decoding
    int priority = 0;        // lower = sooner; < 10 counts as urgent (the photo on screen)
};

struct DecodeResult {
    std::wstring path;
    int level = LV_FIT;
    bool ok = false;        // pixels valid (FULL: the whole image was streamed)
    bool infoOk = false;    // dimensions/orientation valid
    bool infoOnly = false;  // THUMB with fastOnly: no cheap preview existed (not an error)
    int imgW = 0, imgH = 0, orientation = 1, frames = 1;
    int pxW = 0, pxH = 0;
    std::vector<uint8_t> pixels;  // 32bpp premultiplied BGRA, stride = pxW*4
    bool upright = false;         // pixels are already in display orientation (Windows thumbnail cache)
    bool shellThumb = false;      // came from the Windows thumbnail cache (no info decoded)
    int thW = 0, thH = 0;
    std::vector<uint8_t> thumb;   // optional by-product thumbnail
    std::vector<std::vector<uint8_t>> animFrames;  // animated GIF: composited frames (pxW x pxH each)
    std::vector<int> animDelays;                    // per-frame display time, ms

    // LV_FULL streams one result per band of kTile rows (band = true), then a final one.
    bool band = false;
    int levelW = 0, levelH = 0;     // size of the full-resolution level (smaller than imgW/H for huge images)
    int coreY0 = 0, coreY1 = 0;     // rows this band owns
    int bufY0 = 0, bufRows = 0;     // rows actually in `pixels` (core plus gutters), stride = levelW*4

    std::vector<std::pair<std::wstring, std::wstring>> meta;  // LV_META: label, value

    double ms = 0;  // decode time, for tracing
};

class Loader {
public:
    void start(int threads);
    void setNotify(HWND wnd, UINT msg);
    void setRequests(std::vector<DecodeRequest> reqs);
    std::vector<DecodeResult> takeResults();

private:
    struct Job {
        DecodeRequest rq;
        std::shared_ptr<std::atomic<bool>> cancel;
        bool requeue = false;  // paused for something urgent; put it back afterwards
    };
    void workerMain();
    void push(DecodeResult&& r);
    static std::wstring key(const std::wstring& path, int level);

    HWND wnd_ = nullptr;
    UINT msg_ = 0;
    int threadCount_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<DecodeRequest> pending_;
    std::unordered_map<std::wstring, Job> inflight_;
    std::unordered_set<std::wstring> ready_;
    std::vector<DecodeResult> results_;
    std::vector<std::thread> threads_;
    bool posted_ = false;
};

// Lower-case extensions (".jpg") of every decoder installed on this PC.
std::vector<std::wstring> SupportedExtensions();
bool HasExtension(const std::wstring& path, const std::vector<std::wstring>& exts);
