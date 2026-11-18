#pragma once
#include <switch.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <sys/stat.h>
#include <cstdlib>
#include "mp4.hpp"
#include "osd.hpp"
#ifndef REC_MENU_VERSION
#define REC_MENU_VERSION "?"
#endif
#include "lang.hpp"

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
    std::string shotMsg = TR("Ready", "待機中");

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
    std::string recMsg = TR("Ready", "待機中");
    u64 msgExpireMs = 0;        // この時刻を過ぎたら recMsg を消す(0なら消さない)
    std::atomic<bool> stoppedBySleep{false};
    std::atomic<u64> lastVideoMs{0};    // 最後に映像を受け取った時刻
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

// 録画用のバッファ(約0.7MB)。オーバーレイはメモリが少ないので、録画中だけ確保する
static u8 *g_vXfer = nullptr;
static u8 *g_vBuf = nullptr;
static u8 *g_aXfer = nullptr;
static u8 *g_aBuf = nullptr;

inline void freeBuffers() {
    free(g_vXfer); free(g_vBuf); free(g_aXfer); free(g_aBuf);
    g_vXfer = g_vBuf = g_aXfer = g_aBuf = nullptr;
}

inline bool allocBuffers() {
    if (!g_vXfer) g_vXfer = static_cast<u8 *>(aligned_alloc(0x1000, kVXferSize));
    if (!g_vBuf)  g_vBuf  = static_cast<u8 *>(aligned_alloc(0x1000, kVBufSize));
    if (!g_aXfer) g_aXfer = static_cast<u8 *>(aligned_alloc(0x1000, kAXferSize));
    if (!g_aBuf)  g_aBuf  = static_cast<u8 *>(aligned_alloc(0x1000, kABufSize));
    if (g_vXfer && g_vBuf && g_aXfer && g_aBuf) return true;
    freeBuffers();
    return false;
}
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

        s.lastVideoMs = nowMs();
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

// スレッドが終わった後に呼ぶ(ファイルを閉じてバッファを返す)
inline void closeFile() {
    State &s = state();
    if (s.fileOpen) {
        fsFileFlush(&s.file);
        fsFileClose(&s.file);
        fsFsClose(&s.sdfs);
        s.fileOpen = false;
    }
    freeBuffers();
}

// menu = メニューに出す短い文、note = 右下の通知
inline void startFail(const char *menu, const char *note) {
    state().recMsg = menu;
    state().msgExpireMs = 0;
    osd::toast(note, osd::Icon::Error, 4000);
}

// 録画開始
inline bool startLocked() {
    State &s = state();
    char buf[48];
    if (s.recording) return true;

    // 前回のスレッドが終わっていなければ待つ(最大1秒)
    if (!joinThread(s.vThread, s.vThreadValid, 1'000'000'000ULL) ||
        !joinThread(s.aThread, s.aThreadValid, 1'000'000'000ULL)) {
        startFail(TR("Busy (saving)", "保存中"), TR("Still saving the previous recording. Please wait a moment.", "前の録画を保存中です。少し待ってください"));
        return false;
    }
    closeFile();

    // ゲームが起動していないと録画できない
    u64 appPid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&appPid)) || appPid == 0) {
        startFail(TR("No game running", "ゲームが起動していません"), TR("Can't record: no game is running", "ゲームが起動していないため録画できません"));
        return false;
    }
    // SysDVR と同時に使うと映像を取り合うので止める
    u64 dvrPid = 0;
    if (R_SUCCEEDED(pmdmntGetProcessId(&dvrPid, SYSDVR_PROGRAM_ID)) && dvrPid != 0) {
        startFail(TR("Disable SysDVR", "SysDVRを止めてください"), TR("Can't record while SysDVR is running", "SysDVR が動いているため録画できません"));
        return false;
    }

    if (!allocBuffers()) {
        startFail(TR("Out of memory", "メモリ不足"), TR("Not enough memory to record", "メモリが足りないため録画できません"));
        return false;
    }

    Result rc = 0;
    tsl::hlp::doWithSmSession([&]() {
        rc = grcdOpen(&g_grcdVideo);
        if (R_SUCCEEDED(rc)) rc = grcdOpen(&g_grcdAudio);
    });
    if (R_FAILED(rc)) { startFail(errText(rc, buf, sizeof(buf)), TR("Couldn't start recording", "録画を開始できませんでした")); return false; }

    // 保存先(devoptabは使わず、FS APIで直接書く。スクリーンショット側と干渉しない)
    rc = fsOpenSdCardFileSystem(&s.sdfs);
    if (R_FAILED(rc)) { startFail(errText(rc, buf, sizeof(buf)), TR("Couldn't start recording", "録画を開始できませんでした")); return false; }
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
    if (!found) { fsFsClose(&s.sdfs); startFail(TR("No free file name", "ファイル名がいっぱい"), TR("No free file name to save to", "保存できるファイル名がありません")); return false; }

    rc = fsFsCreateFile(&s.sdfs, s.recPath, 0, 0);
    if (R_SUCCEEDED(rc))
        rc = fsFsOpenFile(&s.sdfs, s.recPath, FsOpenMode_Write | FsOpenMode_Append, &s.file);
    if (R_FAILED(rc)) { fsFsClose(&s.sdfs); startFail(errText(rc, buf, sizeof(buf)), TR("Couldn't start recording", "録画を開始できませんでした")); return false; }
    s.fileOpen = true;

    s.fileOffset = 0;
    s.bytesWritten = 0;
    s.recordedUs = 0;
    s.lastError = 0;
    s.audioError = 0;
    s.audioPackets = 0;
    s.stopRequest = false;
    s.lastVideoMs = nowMs();
    mutexInit(&g_fileLock);

    if (!mp4::begin(fileWrite, g_video, g_vBuf, kVBufSize, g_audio, g_aBuf, kABufSize,
                    kSPS + 4, sizeof(kSPS) - 4, kPPS + 4, sizeof(kPPS) - 4, 1280, 720)) {
        closeFile();
        startFail(errText(s.lastError, buf, sizeof(buf)), TR("Couldn't start recording", "録画を開始できませんでした"));
        return false;
    }

    rc = threadCreate(&s.vThread, videoThreadMain, nullptr, nullptr, 0x4000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&s.vThread);
    if (R_FAILED(rc)) {
        closeFile();
        startFail(errText(rc, buf, sizeof(buf)), TR("Couldn't start recording", "録画を開始できませんでした"));
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
    s.recMsg = TR("Recording", "録画中");
    return true;
}

// 録画停止の理由(通知の文言が変わる)
enum class StopReason { User, Sleep, Home, GameStopped, Exit };

// 録画停止
inline void stopLocked(StopReason reason) {
    State &s = state();
    if (!s.vThreadValid && !s.aThreadValid) return;
    const bool wasRecording = s.recording;
    s.stopRequest = true;
    // 次のデータが届けばスレッドは終わる(通常数十ms以内)。
    // ホーム画面ではデータが来ないので待ちきれないことがある → 後で poll() が閉じる
    bool done = joinThread(s.vThread, s.vThreadValid, 1'000'000'000ULL);
    done = joinThread(s.aThread, s.aThreadValid, 1'000'000'000ULL) && done;
    if (done) closeFile();
    s.recording = false;
    s.stoppedBySleep = reason == StopReason::Sleep;
    s.msgExpireMs = 0;
    if (!wasRecording) return;

    char buf[48];
    char note[128];
    const u32 sec = u32(s.recordedUs.load() / 1000000ULL);
    if (s.lastError != 0) {
        s.recMsg = errText(s.lastError, buf, sizeof(buf));
        snprintf(note, sizeof(note), TR("Recording stopped by an error (%s)", "録画エラーで停止しました (%s)"), s.recMsg.c_str());
        osd::toast(note, osd::Icon::Error, 5000);
        return;
    }
    if (sec == 0 && s.bytesWritten.load() < 4096) {
        s.recMsg = TR("No video data", "映像なし");
        osd::toast(TR("No video was received, so nothing was recorded", "映像が届かなかったため録画できませんでした"), osd::Icon::Error, 5000);
        return;
    }
    char snd[24];
    if (s.audioError) snprintf(snd, sizeof(snd), " snd:%04u-%04u", 2000 + R_MODULE(s.audioError.load()), R_DESCRIPTION(s.audioError.load()));
    else if (s.audioPackets == 0) snprintf(snd, sizeof(snd), " snd:0");
    else snd[0] = 0;
    snprintf(buf, sizeof(buf), TR("Saved %u:%02u %.1fMB%s", "保存 %u:%02u %.1fMB%s"), sec / 60, sec % 60, s.bytesWritten.load() / 1048576.0, snd);
    s.recMsg = buf;
    s.msgExpireMs = nowMs() + 3000;

    char len[32];
    snprintf(len, sizeof(len), TR("%u:%02u, %.1fMB", "%u:%02u・%.1fMB"), sec / 60, sec % 60, s.bytesWritten.load() / 1048576.0);
    switch (reason) {
        case StopReason::Home:
            snprintf(note, sizeof(note), TR("Went to the HOME Menu, so recording stopped (%s)", "ホーム画面に移動したため録画を停止しました (%s)"), len);
            osd::toast(note, osd::Icon::Warn, 5000);
            break;
        case StopReason::Sleep:
            snprintf(note, sizeof(note), TR("Console went to sleep, so recording stopped (%s)", "スリープしたため録画を停止しました (%s)"), len);
            osd::toast(note, osd::Icon::Warn, 5000);
            break;
        case StopReason::GameStopped:
            snprintf(note, sizeof(note), TR("Game video stopped, so recording stopped (%s)", "ゲームの映像が止まったため録画を停止しました (%s)"), len);
            osd::toast(note, osd::Icon::Warn, 5000);
            break;
        case StopReason::Exit:
            break;
        default:
            snprintf(note, sizeof(note), TR("Recording stopped (%s)", "録画を停止しました (%s)"), len);
            osd::toast(note, osd::Icon::Stop, 3000);
            break;
    }
}

static Mutex g_ctrlLock;   // start / stop を同時に走らせない(監視スレッドからも止めるため)

inline bool start() {
    mutexLock(&g_ctrlLock);
    bool ok = startLocked();
    mutexUnlock(&g_ctrlLock);
    if (ok) osd::toast(TR("Recording started", "録画を開始しました"), osd::Icon::Rec, 2500);
    return ok;
}

inline void stop(StopReason reason = StopReason::User) {
    mutexLock(&g_ctrlLock);
    stopLocked(reason);
    mutexUnlock(&g_ctrlLock);
}

// 録画スレッドがエラーで止まったときや、停止に時間がかかったときの後始末
inline void poll() {
    State &s = state();
    mutexLock(&g_ctrlLock);
    if (s.recording && s.stopRequest.load()) stopLocked(StopReason::User);
    // 停止時にスレッドが終わりきっていなかった場合、終わったらファイルを閉じる
    if (!s.recording && s.fileOpen &&
        joinThread(s.vThread, s.vThreadValid, 0) && joinThread(s.aThread, s.aThreadValid, 0))
        closeFile();
    mutexUnlock(&g_ctrlLock);
}

// ---------------------------------------------------------------------------
// スクリーンショット(メニューを閉じてから撮るので、メニューは写らない)
// ---------------------------------------------------------------------------
static std::atomic<u64> g_shotAtMs{0};

inline void requestScreenshot() {
    g_shotAtMs = nowMs() + 450;   // メニューが消えるのを待つ
}

inline void doScreenshot() {
    State &s = state();
    char msg[64];
    if (!s.capsReady) {
        s.shotMsg = "caps not ready";
        osd::toast(TR("Couldn't take a screenshot", "スクリーンショットを撮れませんでした"), osd::Icon::Error, 4000);
        return;
    }
    // 512KBのバッファは撮るときだけ確保する
    u8 *jpegBuf = static_cast<u8 *>(aligned_alloc(0x1000, CAPSSC_JPEG_BUFFER_SIZE));
    if (!jpegBuf) {
        s.shotMsg = TR("Out of memory", "メモリ不足");
        osd::toast(TR("Not enough memory to take a screenshot", "メモリが足りないためスクリーンショットを撮れませんでした"), osd::Icon::Error, 4000);
        return;
    }
    struct Free { u8 *p; ~Free() { free(p); } } freeJpeg{ jpegBuf };
    u64 jpegSize = 0;
    Result rc = capsscCaptureJpegScreenShot(&jpegSize, jpegBuf, CAPSSC_JPEG_BUFFER_SIZE, ViLayerStack_Screenshot, 1000000000LL);
    if (R_FAILED(rc) || jpegSize == 0 || jpegSize > CAPSSC_JPEG_BUFFER_SIZE) {
        s.shotMsg = R_FAILED(rc) ? errText(rc, msg, sizeof(msg)) : "Bad size";
        osd::toast(TR("Couldn't take a screenshot", "スクリーンショットを撮れませんでした"), osd::Icon::Error, 4000);
        return;
    }

    // SDカードへはFS APIで直接書く(録画中でも干渉しない)
    FsFileSystem fs;
    if (R_FAILED(rc = fsOpenSdCardFileSystem(&fs))) {
        s.shotMsg = errText(rc, msg, sizeof(msg));
        osd::toast(TR("Couldn't save to the SD card", "SDカードに保存できませんでした"), osd::Icon::Error, 4000);
        return;
    }
    fsFsCreateDirectory(&fs, "/switch");
    fsFsCreateDirectory(&fs, "/switch/rec-menu");
    char path[64];
    bool found = false;
    for (u32 i = s.nextIndex; i < 100000; i++) {
        snprintf(path, sizeof(path), "/switch/rec-menu/shot_%05u.jpg", i);
        FsDirEntryType type;
        if (R_FAILED(fsFsGetEntryType(&fs, path, &type))) { s.nextIndex = i + 1; found = true; break; }
    }
    FsFile f;
    if (!found) rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
    else if (R_SUCCEEDED(rc = fsFsCreateFile(&fs, path, s64(jpegSize), 0)) &&
             R_SUCCEEDED(rc = fsFsOpenFile(&fs, path, FsOpenMode_Write, &f))) {
        rc = fsFileWrite(&f, 0, jpegBuf, jpegSize, FsWriteOption_Flush);
        fsFileClose(&f);
        if (R_FAILED(rc)) fsFsDeleteFile(&fs, path);
    }
    fsFsClose(&fs);

    if (R_FAILED(rc)) {
        s.shotMsg = errText(rc, msg, sizeof(msg));
        osd::toast(TR("Couldn't save to the SD card", "SDカードに保存できませんでした"), osd::Icon::Error, 4000);
        return;
    }
    s.savedCount++;
    snprintf(msg, sizeof(msg), TR("Saved %u", "保存 %u枚"), s.savedCount);
    s.shotMsg = msg;
    osd::toast(TR("Screenshot saved", "スクリーンショットを保存しました"), osd::Icon::Shot, 2500);
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

// ---------------------------------------------------------------------------
// 監視スレッド(0.1秒ごと)
//   - スリープ/電源オフの直前 → 録画を止める(psc:m の通知)
//   - HOMEボタン → 録画を止める(libtesla に入れた通知フック)
//   - ゲームの映像が1.5秒以上来ない → 録画を止める(ゲーム終了など)
//   - スクリーンショットの撮影、右上タイマーの更新(描くのはメインスレッド)
// ---------------------------------------------------------------------------
constexpr u32 kPscModuleId = 0x524D;   // 他と重ならない適当な番号
static PscPmModule g_psc;
static bool g_pscOk = false;
static Thread g_monThread;
static bool g_monValid = false;
static std::atomic<bool> g_monRun{false};
static std::atomic<bool> g_homePressed{false};

// libtesla のバックグラウンドスレッドから呼ばれる(重い処理はしない)
inline void onHomeButton() { g_homePressed = true; }

// 確認用: タイマーや通知が出ないときに原因を調べるための記録
inline void writeDebug(u32 tick) {
    State &s = state();
    char buf[768];
    char osdLine[512];
    osd::debugLine(osdLine, sizeof(osdLine));
    const int n = snprintf(buf, sizeof(buf),
        "Rec Menu debug  ver=%s  tick=%u\n"
        "recording=%d showTimer=%d videoStarted=%d lastVideoAgo=%llu audioPackets=%u\n%s",
        REC_MENU_VERSION, tick, int(s.recording), int(s.showTimer), int(g_video.started.load()),
        (unsigned long long)(s.recording ? nowMs() - s.lastVideoMs.load() : 0), s.audioPackets.load(), osdLine);
    if (n <= 0) return;
    FsFileSystem fs;
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;
    fsFsCreateDirectory(&fs, "/switch/rec-menu");
    const char *path = "/switch/rec-menu/debug.txt";
    fsFsDeleteFile(&fs, path);
    FsFile f;
    if (R_SUCCEEDED(fsFsCreateFile(&fs, path, n, 0)) && R_SUCCEEDED(fsFsOpenFile(&fs, path, FsOpenMode_Write, &f))) {
        fsFileWrite(&f, 0, buf, size_t(n), FsWriteOption_Flush);
        fsFileClose(&f);
    }
    fsFsClose(&fs);
}

inline void monitorThreadMain(void *) {
    State &s = state();
    u32 debugTick = 0;
    while (g_monRun) {
        // スリープの通知を待つ(最大0.1秒)
        const u64 waitNs = 100'000'000ULL;
        if (g_pscOk && R_SUCCEEDED(eventWait(&g_psc.event, waitNs))) {
            PscPmState st;
            u32 flags;
            if (R_SUCCEEDED(pscPmModuleGetRequest(&g_psc, &st, &flags))) {
                if ((st == PscPmState_ReadySleep || st == PscPmState_ReadyShutdown) && s.recording)
                    stop(StopReason::Sleep);
                pscPmModuleAcknowledge(&g_psc, st);
            }
        } else if (!g_pscOk) {
            svcSleepThread(waitNs);
        }

        if (g_homePressed.exchange(false) && s.recording) stop(StopReason::Home);

        if (s.recording && g_video.started && nowMs() - s.lastVideoMs.load() > 1500)
            stop(StopReason::GameStopped);

        poll();

        const u64 shotAt = g_shotAtMs.load();
        if (shotAt != 0 && nowMs() >= shotAt) {
            g_shotAtMs = 0;
            doScreenshot();
        }

        // 確認用の記録: 2秒ごとに /switch/rec-menu/debug.txt を書き直す
        if (++debugTick % 20 == 0) writeDebug(debugTick);

        if (s.recording && s.showTimer) {
            const u32 t = elapsedSec();
            char txt[32];
            snprintf(txt, sizeof(txt), "REC %02u:%02u", t / 60, t % 60);
            osd::setTimer(true, txt);
        } else {
            osd::setTimer(false, "");
        }
    }
}

// initServices の中(smセッション内)から呼ぶ。スリープ通知の登録だけ先にしておく
inline void initSleepWatch() {
    mutexInit(&g_ctrlLock);
    if (R_FAILED(pscmInitialize())) return;
    const u32 deps[] = { PscPmModuleId_Fs };   // SDカードが止まる前に通知してもらう
    if (R_FAILED(pscmGetPmModule(&g_psc, PscPmModuleId(kPscModuleId), deps, 1, true))) { pscmExit(); return; }
    g_pscOk = true;
}

// 画面の準備ができた後(loadInitialGui)に呼ぶ
inline void startMonitor() {
    if (g_monValid) return;
    g_monRun = true;
    if (R_SUCCEEDED(threadCreate(&g_monThread, monitorThreadMain, nullptr, nullptr, 0x4000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&g_monThread))) g_monValid = true;
        else threadClose(&g_monThread);
    }
    if (!g_monValid) g_monRun = false;
}

// 画面を片付ける前に呼ぶ
inline void stopMonitor() {
    if (!g_monValid) return;
    g_monRun = false;
    threadWaitForExit(&g_monThread);
    threadClose(&g_monThread);
    g_monValid = false;
}

inline void shutdown() {
    State &s = state();
    stopMonitor();
    if (s.recording) stop(StopReason::Exit);
    if (g_pscOk) {
        pscPmModuleFinalize(&g_psc);
        pscPmModuleClose(&g_psc);
        pscmExit();
        g_pscOk = false;
    }
    if (serviceIsActive(&g_grcdVideo)) serviceClose(&g_grcdVideo);
    if (serviceIsActive(&g_grcdAudio)) serviceClose(&g_grcdAudio);
    if (s.capsReady) capsscExit();
}

// 「Saved …」は3秒たったら消して Ready に戻す
inline const std::string &recStatus() {
    State &s = state();
    static const std::string ready = TR("Ready", "待機中");
    if (s.msgExpireMs != 0 && nowMs() > s.msgExpireMs) {
        s.recMsg = ready;
        s.msgExpireMs = 0;
    }
    return s.recMsg;
}

inline const char *qualityName(Quality q) {
    switch (q) {
        case Low: return "Low";
        case Mid: return "Mid";
        default:  return "High";
    }
}

} // namespace rec
