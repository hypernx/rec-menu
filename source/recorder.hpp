#pragma once
#include <switch.h>
#include <string>

// 録画バックエンドのスタブ。
// UIから呼ばれるフックだけ定義してあり、実際の録画処理は TODO 部分に実装する。
// 想定構成: 録画本体は別sysmodule、ここからはIPCで開始/停止を送る。
namespace rec {

enum Quality : u8 { Low = 0, Mid = 1, High = 2 };

struct State {
    bool recording = false;
    Quality quality = Mid;
    bool showTimer = true;
    u64 startTick = 0;
    u32 screenshotCount = 0;
};

inline State &state() {
    static State s;
    return s;
}

inline bool isRecording() { return state().recording; }

inline bool start() {
    State &s = state();
    if (s.recording) return true;
    // TODO: sysmoduleへIPCで録画開始を要求する(品質 s.quality を渡す)
    s.startTick = armGetSystemTick();
    s.recording = true;
    return true;
}

inline void stop() {
    State &s = state();
    if (!s.recording) return;
    // TODO: sysmoduleへIPCで録画停止を要求する
    s.recording = false;
}

inline bool screenshot() {
    // TODO: capssc などでスクリーンショットを保存する
    state().screenshotCount++;
    return true;
}

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
