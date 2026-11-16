#pragma once

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// Rec Menu backend.
// Video recording is not implemented yet. Screenshot capture is initialized
// only when requested so opening the overlay does not require caps:sc.
namespace rec {

enum Quality : u8 { Low = 0, Mid = 1, High = 2 };

struct State {
    bool recording = false;
    Quality quality = Mid;
    bool showTimer = true;
    u64 startTick = 0;
    u32 savedCount = 0;
    u32 nextIndex = 0;
    char shotMsg[64] = "Ready";
};

inline State &state() {
    static State s{};
    return s;
}

inline bool isRecording() {
    return state().recording;
}

// UI-only placeholder: this toggles the timer state, not actual video capture.
inline bool start() {
    State &s = state();
    if (s.recording) return true;
    s.startTick = armGetSystemTick();
    s.recording = true;
    return true;
}

inline void stop() {
    State &s = state();
    s.recording = false;
}

inline bool fileExists(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

inline bool screenshotWithCapsService() {
    State &s = state();
    char msg[64] = {};

    Result rc = capsscInitialize();
    if (R_FAILED(rc)) {
        std::snprintf(s.shotMsg, sizeof(s.shotMsg), "Init err 0x%X", rc);
        return false;
    }

    // Keep the large JPEG buffer out of the overlay thread's stack.
    static u8 jpegBuf[CAPSSC_JPEG_BUFFER_SIZE];
    u64 jpegSize = 0;
    rc = capsscCaptureJpegScreenShot(&jpegSize, jpegBuf, sizeof(jpegBuf),
                                     ViLayerStack_Screenshot, 1000000000LL);
    capsscExit();

    if (R_FAILED(rc)) {
        std::snprintf(s.shotMsg, sizeof(s.shotMsg), "Capture err 0x%X", rc);
        return false;
    }
    if (jpegSize == 0 || jpegSize > sizeof(jpegBuf)) {
        std::snprintf(s.shotMsg, sizeof(s.shotMsg), "Bad image size");
        return false;
    }

    bool saved = false;
    tsl::hlp::doWithSDCardHandle([&]() {
        // Ensure the destination directories exist. They may already exist.
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/rec-menu", 0777);

        char path[96] = {};
        u32 idx = s.nextIndex;
        bool found = false;
        while (idx < 100000) {
            std::snprintf(path, sizeof(path), "sdmc:/switch/rec-menu/shot_%05u.jpg", idx);
            if (!fileExists(path)) {
                found = true;
                break;
            }
            ++idx;
        }
        if (!found) {
            std::snprintf(s.shotMsg, sizeof(s.shotMsg), "No free filename");
            return;
        }

        FILE *f = std::fopen(path, "wb");
        if (!f) {
            std::snprintf(s.shotMsg, sizeof(s.shotMsg), "SD write err");
            return;
        }

        const size_t expected = static_cast<size_t>(jpegSize);
        const size_t written = std::fwrite(jpegBuf, 1, expected, f);
        const int closeResult = std::fclose(f);
        if (written != expected || closeResult != 0) {
            std::remove(path);
            std::snprintf(s.shotMsg, sizeof(s.shotMsg), "SD write failed");
            return;
        }

        s.nextIndex = idx + 1;
        ++s.savedCount;
        std::snprintf(msg, sizeof(msg), "Saved %u", s.savedCount);
        std::snprintf(s.shotMsg, sizeof(s.shotMsg), "%s", msg);
        saved = true;
    });
    return saved;
}

inline bool screenshot() {
    bool result = false;
    // capsscInitialize requires an SM session. Do this on demand, not at
    // overlay startup, and always let the helper close its service session.
    tsl::hlp::doWithSmSession([&]() {
        result = screenshotWithCapsService();
    });
    return result;
}

inline const char *shotStatus() {
    return state().shotMsg;
}

inline u32 elapsedSec() {
    const State &s = state();
    if (!s.recording) return 0;
    return static_cast<u32>(armTicksToNs(armGetSystemTick() - s.startTick) / 1000000000ULL);
}

inline void elapsedText(char *out, size_t outSize) {
    const u32 t = elapsedSec();
    std::snprintf(out, outSize, "%02u:%02u", t / 60, t % 60);
}

inline const char *qualityName(Quality q) {
    switch (q) {
        case Low: return "Low";
        case Mid: return "Mid";
        default:  return "High";
    }
}

} // namespace rec
