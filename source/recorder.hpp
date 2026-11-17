#pragma once
#include <switch.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <sys/stat.h>

// 録画・スクリーンショットのバックエンド。
// - スクリーンショット: capssc(caps:sc)でJPEGを取得し、SDカードに保存する。
// - 録画: grc:d(ゲームの動画キャプチャ用ストリーム)からH.264映像を受け取り、
//         sdmc:/switch/rec-menu/rec_00000.h264 に書き出す(映像のみ・音声なし)。
//   grc:d の使い方は SysDVR(exelix11/SysDVR)の実装に合わせている。
namespace rec {

enum Quality : u8 { Low = 0, Mid = 1, High = 2 };

struct State {
    std::atomic<bool> recording{false};  // 録画スレッドが動いている
    std::atomic<bool> stopRequest{false};
    Quality quality = Mid;
    bool showTimer = true;
    u64 startTick = 0;

    bool capsReady = false;      // capsscInitialize が成功したか
    u32 savedCount = 0;          // 今回の起動中に保存した枚数
    u32 nextIndex = 0;           // 次に試すファイル番号
    std::string shotMsg = "Ready";

    // 録画
    bool grcdOpen = false;
    bool threadValid = false;
    Thread thread{};
    FsFileSystem sdfs{};
    FsFile file{};
    s64 fileOffset = 0;
    std::atomic<u64> bytesWritten{0};
    std::atomic<u32> lastError{0};
    u32 nextRecIndex = 0;
    char recPath[64] = {};
    std::string recMsg = "Ready";
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
}

inline bool isRecording() { return state().recording.load(); }

// ---------------------------------------------------------------------------
// 録画(grc:d)
// ---------------------------------------------------------------------------

// grcdBegin が未実行のとき grcdTransfer が返すエラー(SysDVRより)
constexpr Result GRCD_NOT_INITIALIZED = 0x3E8D4;
// IDRフレームがバッファより大きいとき返るエラー。1フレーム欠けるだけなので続行する
constexpr Result GRCD_BUFFER_TOO_SMALL = 0xCD4;
constexpr u64 SYSDVR_PROGRAM_ID = 0x00FF0000A53BB665ULL;

// SysDVR と同じSPS/PPS。GRCはゲーム起動時に一度しか出さないので、
// 先頭と数回に一度のIDRフレームの前に付け足して、途中からでも再生できるようにする
static const u8 kSPS[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B,
                           0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
static const u8 kPPS[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };

constexpr size_t kXferSize  = 0x54000;   // SysDVRと同じサイズ
constexpr size_t kWriteSize = 0x40000;   // まとめ書き用(256KB)。オーバーレイはメモリが少ないので控えめに

alignas(0x1000) static u8 g_xferBuf[kXferSize];
alignas(0x1000) static u8 g_writeBuf[kWriteSize];
static size_t g_writeLen = 0;

inline bool flushWriteBuf() {
    State &s = state();
    if (g_writeLen == 0) return true;
    Result rc = fsFileWrite(&s.file, s.fileOffset, g_writeBuf, g_writeLen, FsWriteOption_None);
    if (R_FAILED(rc)) {
        s.lastError = rc;
        return false;
    }
    s.fileOffset += g_writeLen;
    s.bytesWritten += g_writeLen;
    g_writeLen = 0;
    return true;
}

inline bool appendData(const void *data, size_t size) {
    if (g_writeLen + size > kWriteSize) {
        if (!flushWriteBuf()) return false;
    }
    if (size > kWriteSize) {  // 起こらないはずだが念のため直接書く
        State &s = state();
        Result rc = fsFileWrite(&s.file, s.fileOffset, data, size, FsWriteOption_None);
        if (R_FAILED(rc)) { s.lastError = rc; return false; }
        s.fileOffset += size;
        s.bytesWritten += size;
        return true;
    }
    std::memcpy(g_writeBuf + g_writeLen, data, size);
    g_writeLen += size;
    return true;
}

inline void recThreadMain(void *) {
    State &s = state();
    bool beginCalled = false;
    bool needMeta = true;   // 最初のフレームの前にSPS/PPSを入れる
    int idrCount = 0;

    while (!s.stopRequest.load()) {
        u32 dataSize = 0;
        u64 timestamp = 0;
        // ゲームが動画撮影に対応していないと、ここでデータが来るまで待ち続ける
        Result rc = grcdTransfer(GrcStream_Video, g_xferBuf, kXferSize, nullptr, &dataSize, &timestamp);

        if (rc == GRCD_NOT_INITIALIZED && !beginCalled) {
            // まだ誰も grcdBegin していない場合だけ呼ぶ。
            // (既に呼ばれている状態でもう一度呼ぶと本体ごと落ちるため、必ずこの順番)
            beginCalled = true;
            rc = grcdBegin();
            if (R_FAILED(rc)) { s.lastError = rc; break; }
            continue;
        }
        if (rc == GRCD_BUFFER_TOO_SMALL) continue;
        if (R_FAILED(rc)) { s.lastError = rc; break; }
        if (dataSize <= 4) continue;

        const bool isIDR = (g_xferBuf[4] & 0x1F) == 5;
        if (needMeta || (isIDR && ++idrCount >= 5)) {
            idrCount = 0;
            needMeta = false;
            if (!appendData(kSPS, sizeof(kSPS)) || !appendData(kPPS, sizeof(kPPS))) break;
        }
        if (!appendData(g_xferBuf, dataSize)) break;
    }

    flushWriteBuf();
    fsFileFlush(&s.file);
    fsFileClose(&s.file);
    fsFsClose(&s.sdfs);
    s.recording = false;
}

inline const char *errText(Result rc, char *buf, size_t n) {
    snprintf(buf, n, "Err %04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return buf;
}

// 録画開始。メインスレッドから呼ぶ
inline bool start() {
    State &s = state();
    char buf[48];
    if (s.recording) return true;

    // 前回のスレッドが終わっていなければ待つ(最大1秒)
    if (s.threadValid) {
        if (R_FAILED(waitSingleHandle(s.thread.handle, 1'000'000'000ULL))) {
            s.recMsg = "Busy (stopping)";
            return false;
        }
        threadClose(&s.thread);
        s.threadValid = false;
    }

    // ゲームが起動していないと録画できない
    u64 appPid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&appPid)) || appPid == 0) {
        s.recMsg = "No game running";
        return false;
    }
    // SysDVR と同時に使うと映像を取り合うので止める
    u64 dvrPid = 0;
    if (R_SUCCEEDED(pmdmntGetProcessId(&dvrPid, SYSDVR_PROGRAM_ID)) && dvrPid != 0) {
        s.recMsg = "Disable SysDVR";
        return false;
    }

    Result rc = 0;
    if (!s.grcdOpen) {
        tsl::hlp::doWithSmSession([&]() { rc = grcdInitialize(); });
        if (R_FAILED(rc)) { s.recMsg = errText(rc, buf, sizeof(buf)); return false; }
        s.grcdOpen = true;
    }

    // 保存先(devoptabは使わず、FS APIで直接書く。スクリーンショット側と干渉しない)
    rc = fsOpenSdCardFileSystem(&s.sdfs);
    if (R_FAILED(rc)) { s.recMsg = errText(rc, buf, sizeof(buf)); return false; }
    fsFsCreateDirectory(&s.sdfs, "/switch");
    fsFsCreateDirectory(&s.sdfs, "/switch/rec-menu");

    bool found = false;
    for (u32 i = s.nextRecIndex; i < 100000; i++) {
        snprintf(s.recPath, sizeof(s.recPath), "/switch/rec-menu/rec_%05u.h264", i);
        FsDirEntryType type;
        if (R_FAILED(fsFsGetEntryType(&s.sdfs, s.recPath, &type))) {
            s.nextRecIndex = i + 1;
            found = true;
            break;
        }
    }
    if (!found) { fsFsClose(&s.sdfs); s.recMsg = "No free name"; return false; }

    rc = fsFsCreateFile(&s.sdfs, s.recPath, 0, 0);
    if (R_SUCCEEDED(rc))
        rc = fsFsOpenFile(&s.sdfs, s.recPath, FsOpenMode_Write | FsOpenMode_Append, &s.file);
    if (R_FAILED(rc)) { fsFsClose(&s.sdfs); s.recMsg = errText(rc, buf, sizeof(buf)); return false; }

    s.fileOffset = 0;
    s.bytesWritten = 0;
    s.lastError = 0;
    g_writeLen = 0;
    s.stopRequest = false;
    s.recording = true;

    rc = threadCreate(&s.thread, recThreadMain, nullptr, nullptr, 0x4000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&s.thread);
    if (R_FAILED(rc)) {
        s.recording = false;
        fsFileClose(&s.file);
        fsFsClose(&s.sdfs);
        s.recMsg = errText(rc, buf, sizeof(buf));
        return false;
    }
    s.threadValid = true;
    s.startTick = armGetSystemTick();
    s.recMsg = "Recording";
    return true;
}

// 録画停止。メインスレッドから呼ぶ
inline void stop() {
    State &s = state();
    if (!s.threadValid) return;
    s.stopRequest = true;
    // 次のフレームが届けばスレッドは終わる(通常33ms以内)
    if (R_SUCCEEDED(waitSingleHandle(s.thread.handle, 1'000'000'000ULL))) {
        threadClose(&s.thread);
        s.threadValid = false;
    }
    char buf[48];
    if (s.lastError != 0) {
        s.recMsg = errText(s.lastError, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "Saved %.1fMB", s.bytesWritten.load() / 1048576.0);
        s.recMsg = buf;
    }
}

// 録画スレッドがエラーで勝手に止まったときの後始末(毎フレーム呼んでよい)
inline void poll() {
    State &s = state();
    if (s.threadValid && !s.recording) {
        stop();
    }
}

inline void shutdown() {
    State &s = state();
    if (s.recording) stop();
    if (s.grcdOpen) grcdExit();
    if (s.capsReady) capsscExit();
}

inline const std::string &recStatus() { return state().recMsg; }

// ---------------------------------------------------------------------------
// スクリーンショット
// ---------------------------------------------------------------------------

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
