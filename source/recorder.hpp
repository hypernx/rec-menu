#pragma once
#include <switch.h>
#include <string>
#include <cstdio>
#include <sys/stat.h>

// 録画・スクリーンショットのバックエンド。
// スクリーンショットは capssc(caps:sc)でJPEGを取得し、SDカードに保存する。
// 動画録画は未実装(TODO)。
namespace rec {

enum Quality : u8 { Low = 0, Mid = 1, High = 2 };

struct State {
    bool recording = false;
    Quality quality = Mid;
    bool showTimer = true;
    u64 startTick = 0;

    bool capsReady = false;     // capsscInitialize が成功したか
    u32 savedCount = 0;         // 今回の起動中に保存した枚数
    u32 nextIndex = 0;          // 次に試すファイル番号
    std::string shotMsg = "Ready"; // 画面に出す結果メッセージ
};

inline State &state() {
    static State s;
    return s;
}

// initServices の中(smセッション内)から呼ぶ
inline void init() {
    State &s = state();
    Result rc = capsscInitialize();
    s.capsReady = R_SUCCEEDED(rc);
    if (!s.capsReady) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Init err 0x%X", rc);
        s.shotMsg = buf;
    }
    // 注意: ここではSDカードがまだマウントされていないので、SDには触らない。
    // (以前はここで mkdir していて、起動直後にクラッシュしていた)
}

inline void shutdown() {
    if (state().capsReady) capsscExit();
}

inline bool isRecording() { return state().recording; }

inline bool start() {
    State &s = state();
    if (s.recording) return true;
    // TODO: 録画開始(grcdTransfer を別スレッドで読む、または sysmodule へIPC)
    s.startTick = armGetSystemTick();
    s.recording = true;
    return true;
}

inline void stop() {
    State &s = state();
    if (!s.recording) return;
    // TODO: 録画停止
    s.recording = false;
}

inline bool fileExists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// スクリーンショットを撮って /switch/rec-menu/shot_00000.jpg のように保存する
inline bool screenshot() {
    State &s = state();
    char msg[64];

    if (!s.capsReady) {
        s.shotMsg = "caps not ready";
        return false;
    }

    // 大きなバッファはヒープではなく静的領域に置く
    static u8 jpegBuf[CAPSSC_JPEG_BUFFER_SIZE];
    u64 jpegSize = 0;

    Result rc = capsscCaptureJpegScreenShot(&jpegSize, jpegBuf, sizeof(jpegBuf),
                                            ViLayerStack_Screenshot, 1000000000LL);
    if (R_FAILED(rc)) {
        snprintf(msg, sizeof(msg), "Capture err 0x%X", rc);
        s.shotMsg = msg;
        return false;
    }
    if (jpegSize == 0 || jpegSize > sizeof(jpegBuf)) {
        s.shotMsg = "Bad size";
        return false;
    }

    u32 idx = s.nextIndex;
    // SDカードはこの中でだけマウントされる(Teslaの仕様)
    bool ok = false;
    tsl::hlp::doWithSDCardHandle([&]() {
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/rec-menu", 0777);

        // 空いているファイル名を探す(上書き防止)
        char path[96];
        u32 i = s.nextIndex;
        bool found = false;
        for (; i < 100000; i++) {
            snprintf(path, sizeof(path), "sdmc:/switch/rec-menu/shot_%05u.jpg", i);
            if (!fileExists(path)) { found = true; break; }
        }
        if (!found) { s.shotMsg = "No free name"; return; }

        FILE *f = fopen(path, "wb");
        if (!f) { s.shotMsg = "SD write err"; return; }
        size_t written = fwrite(jpegBuf, 1, static_cast<size_t>(jpegSize), f);
        int closeRc = fclose(f);
        if (written != jpegSize || closeRc != 0) {
            remove(path);
            s.shotMsg = "SD write failed";
            return;
        }
        idx = i + 1;
        ok = true;
    });
    if (!ok) return false;

    s.nextIndex = idx;
    s.savedCount++;
    snprintf(msg, sizeof(msg), "Saved %u", s.savedCount);
    s.shotMsg = msg;
    return true;
}

inline const std::string &shotStatus() { return state().shotMsg; }

inline u32 elapsedSec() {
    const State &s = state();
    if (!s.recording) return 0;
    return static_cast<u32>(armTicksToNs(armGetSystemTick() - s.startTick) / 1000000000ULL);
}

inline std::string elapsedText() {
    u32 t = elapsedSec();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u", t / 60, t % 60);
    return buf;
}

inline const char *qualityName(Quality q) {
    switch (q) {
        case Low: return "Low";
        case Mid: return "Mid";
        default:  return "High";
    }
}

} // namespace rec
