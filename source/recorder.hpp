#pragma once
#include <switch.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <sys/stat.h>
#include "mp4.hpp"

// 録画・スクリーンショットのバックエンド。
// - スクリーンショット: capssc(caps:sc)でJPEGを取得し、SDカードに保存する。
// - 録画: grc:d(ゲームの動画キャプチャ用ストリーム)からH.264映像を受け取り、
//         音声(PCM)と一緒にMP4にして sdmc:/switch/rec-menu/rec_00000.mp4 に書き出す。
//   grc:d の使い方は SysDVR(exelix11/SysDVR)の実装に合わせている。
namespace rec {

enum Quality : u8 { Low = 0, Mid = 1, High = 2 };

struct State {
    bool recording = false;              // 録画中(メインスレッドだけが書き換える)
    std::atomic<bool> stopRequest{false};
    Quality quality = Mid;
    bool showTimer = true;
    u64 startTick = 0;

    bool capsReady = false;      // capsscInitialize が成功したか
    u32 savedCount = 0;          // 今回の起動中に保存した枚数
    u32 nextIndex = 0;           // 次に試すファイル番号
    std::string shotMsg = "Ready";

    // 録画
    bool vThreadValid = false;
    bool aThreadValid = false;
    Thread vThread{};
    Thread aThread{};
    bool fileOpen = false;
    FsFileSystem sdfs{};
    FsFile file{};
    s64 fileOffset = 0;
    std::atomic<u64> bytesWritten{0};
    std::atomic<u64> recordedUs{0};     // 動画として書けた長さ
    std::atomic<u32> lastError{0};
    std::atomic<u32> audioError{0};
    std::atomic<u32> audioPackets{0};   // 受け取った音声パケット数(確認用)
    u32 nextRecIndex = 0;
    char recPath[64] = {};
    std::string recMsg = "Ready";
    u64 msgExpireMs = 0;        // この時刻を過ぎたら recMsg を消す(0なら消さない)
    std::atomic<bool> stoppedBySleep{false};
};

inline State &state() {
    static State s;
    return s;
}

inline void initSleepWatch();

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
    initSleepWatch();
}

inline bool isRecording() { return state().recording; }

// ---------------------------------------------------------------------------
// 録画(grc:d)
//   映像スレッドと音声スレッドがそれぞれ grc:d からデータを受け取り、MP4に書く。
//   grc:d の受け取りは待ち状態になる(ブロックする)ので、映像と音声で別の接続を使う(SysDVRと同じ)。
// ---------------------------------------------------------------------------

// grcdBegin が未実行のとき grcdTransfer が返すエラー(SysDVRより)
constexpr Result GRCD_NOT_INITIALIZED = 0x3E8D4;
// IDRフレームがバッファより大きいとき返るエラー。1フレーム欠けるだけなので続行する
constexpr Result GRCD_BUFFER_TOO_SMALL = 0xCD4;
constexpr u64 SYSDVR_PROGRAM_ID = 0x00FF0000A53BB665ULL;

// SysDVR と同じSPS/PPS(1280x720 / H.264 High)。GRCはゲーム起動時に一度しか出さないので、
// MP4のヘッダ(avcC)にはこの値を入れる
static const u8 kSPS[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B,
                           0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
static const u8 kPPS[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };

constexpr size_t kVXferSize = 0x54000;   // 映像の受け取り(SysDVRと同じサイズ)
constexpr size_t kVBufSize  = 0x58000;   // MP4の映像1かたまり分(1フレームより大きい必要がある)
constexpr size_t kAXferSize = 0x1000;    // 音声の受け取り(本体の録画機能と同じ)
constexpr size_t kABufSize  = 0x8000;    // MP4の音声1かたまり分(約0.17秒)

alignas(0x1000) static u8 g_vXfer[kVXferSize];
alignas(0x1000) static u8 g_vBuf[kVBufSize];
alignas(0x1000) static u8 g_aXfer[kAXferSize];
alignas(0x1000) static u8 g_aBuf[kABufSize];
static mp4::VideoTrack g_video;
static mp4::AudioTrack g_audio;
static Mutex g_fileLock;
static Service g_grcdVideo;
static Service g_grcdAudio;
static std::atomic<bool> g_beginCalled{false};

inline u64 nowUs() { return armTicksToNs(armGetSystemTick()) / 1000ULL; }
inline u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

inline Result grcdOpen(Service *srv) {
    if (serviceIsActive(srv)) return 0;
    return smGetService(srv, "grc:d");
}

inline Result grcdXfer(Service *srv, GrcStream stream, void *buf, size_t size, u32 *dataSize, u64 *ts) {
    struct { u32 num_frames; u32 data_size; u64 start_timestamp; } out{};
    u32 in = stream;
    Result rc = serviceDispatchInOut(srv, 2, in, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buf, size } },
    );
    if (R_SUCCEEDED(rc)) { *dataSize = out.data_size; *ts = out.start_timestamp; }
    return rc;
}

// MP4への書き込み(映像と音声の両方のスレッドから呼ばれるので排他する)
inline bool fileWrite(const void *hdr, size_t hdrSize, const void *data, size_t dataSize) {
    State &s = state();
    mutexLock(&g_fileLock);
    Result rc = 0;
    if (hdrSize) rc = fsFileWrite(&s.file, s.fileOffset, hdr, hdrSize, FsWriteOption_None);
    if (R_SUCCEEDED(rc) && dataSize)
        rc = fsFileWrite(&s.file, s.fileOffset + hdrSize, data, dataSize, FsWriteOption_None);
    if (R_SUCCEEDED(rc)) {
        s.fileOffset += hdrSize + dataSize;
        s.bytesWritten += hdrSize + dataSize;
    } else {
        s.lastError = rc;
    }
    mutexUnlock(&g_fileLock);
    return R_SUCCEEDED(rc);
}

inline void videoThreadMain(void *) {
    State &s = state();
    while (!s.stopRequest.load()) {
        u32 dataSize = 0;
        u64 ts = 0;
        // ゲームが動画撮影に対応していないと、ここでデータが来るまで待ち続ける
        Result rc = grcdXfer(&g_grcdVideo, GrcStream_Video, g_vXfer, kVXferSize, &dataSize, &ts);

        if (rc == GRCD_NOT_INITIALIZED && !g_beginCalled) {
            // まだ誰も grcdBegin していない場合だけ呼ぶ。
            // (既に呼ばれている状態でもう一度呼ぶと本体ごと落ちるため、必ずこの順番)
            g_beginCalled = true;
            rc = serviceDispatch(&g_grcdVideo, 1);
            if (R_FAILED(rc)) { s.lastError = rc; break; }
            continue;
        }
        if (rc == GRCD_BUFFER_TOO_SMALL) continue;
        if (R_FAILED(rc)) { s.lastError = rc; break; }
        if (dataSize <= 4) continue;

        if (!mp4::addVideo(g_video, g_vXfer, dataSize, ts, nowUs())) break;
        s.recordedUs = mp4::durationUs(g_video);
    }
    mp4::flushVideo(g_video);
    s.stopRequest = true;   // 映像が止まったら音声も止める
}

inline void audioThreadMain(void *) {
    State &s = state();
    while (!s.stopRequest.load()) {
        u32 dataSize = 0;
        u64 ts = 0;
        Result rc = grcdXfer(&g_grcdAudio, GrcStream_Audio, g_aXfer, kAXferSize, &dataSize, &ts);
        if (rc == GRCD_NOT_INITIALIZED) {   // grcdBegin は映像側に任せて少し待つ
            svcSleepThread(20'000'000ULL);
            continue;
        }
        if (R_FAILED(rc)) {   // 音声だけ失敗しても映像は続ける
            s.audioError = rc;
            break;
        }
        s.audioPackets++;
        if (!mp4::addAudio(g_audio, g_video, g_aXfer, dataSize, ts, nowUs())) break;
    }
    mp4::flushAudio(g_audio);
}

inline const char *errText(Result rc, char *buf, size_t n) {
    snprintf(buf, n, "Err %04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return buf;
}

// スレッドの終了を待つ(最大 timeoutNs)。終わったら閉じて true
inline bool joinThread(Thread &t, bool &valid, u64 timeoutNs) {
    if (!valid) return true;
    if (R_FAILED(waitSingleHandle(t.handle, timeoutNs))) return false;
    threadClose(&t);
    valid = false;
    return true;
}

inline void closeFile() {
    State &s = state();
    if (!s.fileOpen) return;
    fsFileFlush(&s.file);
    fsFileClose(&s.file);
    fsFsClose(&s.sdfs);
    s.fileOpen = false;
}

// 録画開始。メインスレッドから呼ぶ
inline bool startLocked() {
    State &s = state();
    char buf[48];
    if (s.recording) return true;

    // 前回のスレッドが終わっていなければ待つ(最大1秒)
    if (!joinThread(s.vThread, s.vThreadValid, 1'000'000'000ULL) ||
        !joinThread(s.aThread, s.aThreadValid, 1'000'000'000ULL)) {
        s.recMsg = "Busy (stopping)";
        return false;
    }
    closeFile();

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
    tsl::hlp::doWithSmSession([&]() {
        rc = grcdOpen(&g_grcdVideo);
        if (R_SUCCEEDED(rc)) rc = grcdOpen(&g_grcdAudio);
    });
    if (R_FAILED(rc)) { s.recMsg = errText(rc, buf, sizeof(buf)); return false; }

    // 保存先(devoptabは使わず、FS APIで直接書く。スクリーンショット側と干渉しない)
    rc = fsOpenSdCardFileSystem(&s.sdfs);
    if (R_FAILED(rc)) { s.recMsg = errText(rc, buf, sizeof(buf)); return false; }
    fsFsCreateDirectory(&s.sdfs, "/switch");
    fsFsCreateDirectory(&s.sdfs, "/switch/rec-menu");

    bool found = false;
    for (u32 i = s.nextRecIndex; i < 100000; i++) {
        snprintf(s.recPath, sizeof(s.recPath), "/switch/rec-menu/rec_%05u.mp4", i);
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
    s.fileOpen = true;

    s.fileOffset = 0;
    s.bytesWritten = 0;
    s.recordedUs = 0;
    s.lastError = 0;
    s.audioError = 0;
    s.audioPackets = 0;
    s.stopRequest = false;
    mutexInit(&g_fileLock);

    if (!mp4::begin(fileWrite, g_video, g_vBuf, kVBufSize, g_audio, g_aBuf, kABufSize,
                    kSPS + 4, sizeof(kSPS) - 4, kPPS + 4, sizeof(kPPS) - 4, 1280, 720)) {
        closeFile();
        s.recMsg = errText(s.lastError, buf, sizeof(buf));
        return false;
    }

    rc = threadCreate(&s.vThread, videoThreadMain, nullptr, nullptr, 0x4000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&s.vThread);
    if (R_FAILED(rc)) {
        closeFile();
        s.recMsg = errText(rc, buf, sizeof(buf));
        return false;
    }
    s.vThreadValid = true;
    s.recording = true;

    // 音声は失敗しても映像だけで録画を続ける
    if (R_SUCCEEDED(threadCreate(&s.aThread, audioThreadMain, nullptr, nullptr, 0x4000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&s.aThread))) s.aThreadValid = true;
        else threadClose(&s.aThread);
    }

    s.startTick = armGetSystemTick();
    s.stoppedBySleep = false;
    s.msgExpireMs = 0;
    s.recMsg = "Recording";
    return true;
}

// 録画停止。メインスレッドから呼ぶ
inline void stopLocked(bool bySleep) {
    State &s = state();
    if (!s.vThreadValid && !s.aThreadValid) return;
    s.stopRequest = true;
    // 次のデータが届けばスレッドは終わる(通常数十ms以内)
    bool done = joinThread(s.vThread, s.vThreadValid, 1'000'000'000ULL);
    done = joinThread(s.aThread, s.aThreadValid, 1'000'000'000ULL) && done;
    if (done) closeFile();
    s.recording = false;
    s.stoppedBySleep = bySleep;
    s.msgExpireMs = 0;

    char buf[48];
    if (s.lastError != 0) {
        s.recMsg = errText(s.lastError, buf, sizeof(buf));
    } else {
        const u32 sec = u32(s.recordedUs.load() / 1000000ULL);
        if (sec == 0 && s.bytesWritten.load() < 4096)
            snprintf(buf, sizeof(buf), "No video data");
        else
        {
            char snd[24];
            if (s.audioError) snprintf(snd, sizeof(snd), " snd:%04u-%04u", 2000 + R_MODULE(s.audioError.load()), R_DESCRIPTION(s.audioError.load()));
            else if (s.audioPackets == 0) snprintf(snd, sizeof(snd), " snd:0");
            else snd[0] = 0;
            snprintf(buf, sizeof(buf), "Saved%s %u:%02u %.1fMB%s", bySleep ? "(sleep)" : "", sec / 60, sec % 60,
                     s.bytesWritten.load() / 1048576.0, snd);
            s.msgExpireMs = nowMs() + 3000;
        }
        s.recMsg = buf;
    }
}

static Mutex g_ctrlLock;   // start / stop を同時に走らせない(スリープ検知は別スレッドから止める)

inline bool start() {
    mutexLock(&g_ctrlLock);
    bool ok = startLocked();
    mutexUnlock(&g_ctrlLock);
    return ok;
}

inline void stop(bool bySleep = false) {
    mutexLock(&g_ctrlLock);
    stopLocked(bySleep);
    mutexUnlock(&g_ctrlLock);
}

// 毎フレーム呼ぶ。録画スレッドがエラーで止まったときや、停止に時間がかかったときの後始末
inline void poll() {
    State &s = state();
    mutexLock(&g_ctrlLock);
    if (s.recording && s.stopRequest.load()) stopLocked(false);
    // 停止時にスレッドが終わりきっていなかった場合、終わったらファイルを閉じる
    if (!s.recording && s.fileOpen &&
        joinThread(s.vThread, s.vThreadValid, 0) && joinThread(s.aThread, s.aThreadValid, 0))
        closeFile();
    mutexUnlock(&g_ctrlLock);
}

// ---------------------------------------------------------------------------
// スリープ検知: 録画中にスリープしたら自動で停止してファイルを閉じる
// (psc:m に登録すると、スリープ/電源オフの直前に通知が来る)
// ---------------------------------------------------------------------------
constexpr u32 kPscModuleId = 0x524D;   // 他と重ならない適当な番号
static PscPmModule g_psc;
static bool g_pscOk = false;
static Thread g_pscThread;
static std::atomic<bool> g_pscRun{false};

inline void pscThreadMain(void *) {
    while (g_pscRun) {
        if (R_FAILED(eventWait(&g_psc.event, 200'000'000ULL))) continue;   // 0.2秒ごとに終了確認
        PscPmState st;
        u32 flags;
        if (R_FAILED(pscPmModuleGetRequest(&g_psc, &st, &flags))) continue;
        if (st == PscPmState_ReadySleep || st == PscPmState_ReadyShutdown) {
            if (state().recording) stop(true);
        }
        pscPmModuleAcknowledge(&g_psc, st);
    }
}

// initServices の中(smセッション内)から呼ぶ
inline void initSleepWatch() {
    mutexInit(&g_ctrlLock);
    if (R_FAILED(pscmInitialize())) return;
    const u32 deps[] = { PscPmModuleId_Fs };   // SDカードが止まる前に通知してもらう
    if (R_FAILED(pscmGetPmModule(&g_psc, PscPmModuleId(kPscModuleId), deps, 1, true))) { pscmExit(); return; }
    g_pscRun = true;
    if (R_FAILED(threadCreate(&g_pscThread, pscThreadMain, nullptr, nullptr, 0x2000, 0x2C, -2)) ||
        R_FAILED(threadStart(&g_pscThread))) {
        g_pscRun = false;
        pscPmModuleFinalize(&g_psc);
        pscPmModuleClose(&g_psc);
        pscmExit();
        return;
    }
    g_pscOk = true;
}

inline void exitSleepWatch() {
    if (!g_pscOk) return;
    g_pscRun = false;
    threadWaitForExit(&g_pscThread);
    threadClose(&g_pscThread);
    pscPmModuleFinalize(&g_psc);
    pscPmModuleClose(&g_psc);
    pscmExit();
    g_pscOk = false;
}

inline void shutdown() {
    State &s = state();
    exitSleepWatch();
    if (s.recording) stop();
    if (serviceIsActive(&g_grcdVideo)) serviceClose(&g_grcdVideo);
    if (serviceIsActive(&g_grcdAudio)) serviceClose(&g_grcdAudio);
    if (s.capsReady) capsscExit();
}

// 「Saved …」は3秒たったら消して Ready に戻す
inline const std::string &recStatus() {
    State &s = state();
    static const std::string ready = "Ready";
    if (s.msgExpireMs != 0 && nowMs() > s.msgExpireMs) {
        s.recMsg = ready;
        s.msgExpireMs = 0;
    }
    return s.recMsg;
}


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
