// Rec Gallery - Rec Menu で撮った動画(/switch/rec-menu/*.mp4)を、アルバムのように一覧・再生・削除する
//
// 操作:
//   一覧   … 十字キー/スティック: 選択   A: 再生   X: 削除   +/B: 終了
//   再生中 … A: 一時停止/再開   ←/→: 10秒戻る/進む   L/R: 30秒戻る/進む   B: 一覧に戻る
#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

static const char *kDir = "sdmc:/switch/rec-menu";

// ffmpeg に渡すパス。"sdmc:/..." のままだと "sdmc" を通信方式の名前だと思って開けないので "file:" を付ける
static std::string avPath(const std::string &p) { return "file:" + p; }
constexpr int W = 1280, H = 720;

static SDL_Window *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font *g_fontL = nullptr, *g_fontM = nullptr, *g_fontS = nullptr;
static PadState g_pad;
static bool g_quit = false;

// ---------------------------------------------------------------------------
// 文字表示(作ったテクスチャはしばらく使い回す)
// ---------------------------------------------------------------------------
struct TextTex { SDL_Texture *tex; int w, h; u32 lastUse; };
static std::unordered_map<std::string, TextTex> g_textCache;
static u32 g_frameNo = 0;

enum Align { Left, Center, Right };

static void drawText(TTF_Font *font, const std::string &s, int x, int y, SDL_Color c, Align al = Left) {
    if (s.empty()) return;
    char keyPrefix[48];
    snprintf(keyPrefix, sizeof(keyPrefix), "%p|%02x%02x%02x%02x|", (void *)font, c.r, c.g, c.b, c.a);
    std::string key = keyPrefix + s;
    auto it = g_textCache.find(key);
    if (it == g_textCache.end()) {
        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
        if (!surf) return;
        SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, surf);
        TextTex tt{ t, surf->w, surf->h, g_frameNo };
        SDL_FreeSurface(surf);
        if (!t) return;
        it = g_textCache.emplace(key, tt).first;
    }
    it->second.lastUse = g_frameNo;
    SDL_Rect r{ x, y, it->second.w, it->second.h };
    if (al == Center) r.x -= r.w / 2;
    else if (al == Right) r.x -= r.w;
    SDL_RenderCopy(g_ren, it->second.tex, nullptr, &r);
}

// 長く使っていない文字のテクスチャを捨てる
static void trimTextCache() {
    if (g_textCache.size() < 300) return;
    for (auto it = g_textCache.begin(); it != g_textCache.end();) {
        if (g_frameNo - it->second.lastUse > 120) {
            SDL_DestroyTexture(it->second.tex);
            it = g_textCache.erase(it);
        } else {
            ++it;
        }
    }
}

static void fillRect(int x, int y, int w, int h, u8 r, u8 g, u8 b, u8 a = 255) {
    SDL_SetRenderDrawBlendMode(g_ren, a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, r, g, b, a);
    SDL_Rect rc{ x, y, w, h };
    SDL_RenderFillRect(g_ren, &rc);
}

static void strokeRect(int x, int y, int w, int h, int t, u8 r, u8 g, u8 b) {
    fillRect(x, y, w, t, r, g, b);
    fillRect(x, y + h - t, w, t, r, g, b);
    fillRect(x, y, t, h, r, g, b);
    fillRect(x + w - t, y, t, h, r, g, b);
}

static std::string fmtTime(double sec) {
    if (sec < 0 || std::isnan(sec)) sec = 0;
    int s = int(sec);
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

static const SDL_Color kWhite{ 255, 255, 255, 255 };
static const SDL_Color kGray{ 170, 170, 170, 255 };
static const SDL_Color kCyan{ 0, 220, 200, 255 };
static const SDL_Color kRed{ 255, 90, 90, 255 };

// ---------------------------------------------------------------------------
// 入力(押しっぱなしでの連続移動つき)
// ---------------------------------------------------------------------------
struct Input { u64 down = 0; u64 held = 0; u64 repeat = 0; };

static Input readInput() {
    static u64 heldSince = 0, lastRepeat = 0, lastDir = 0;
    padUpdate(&g_pad);
    Input in;
    in.down = padGetButtonsDown(&g_pad);
    in.held = padGetButtons(&g_pad);
    const u64 dirs = HidNpadButton_AnyLeft | HidNpadButton_AnyRight | HidNpadButton_AnyUp | HidNpadButton_AnyDown;
    const u64 now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    const u64 d = in.held & dirs;
    in.repeat = in.down & dirs;
    if (d != lastDir) { heldSince = now; lastRepeat = now; lastDir = d; }
    else if (d && now - heldSince > 350 && now - lastRepeat > 90) { in.repeat |= d; lastRepeat = now; }
    return in;
}

// ---------------------------------------------------------------------------
// 動画ファイル一覧
// ---------------------------------------------------------------------------
struct Entry {
    std::string path, name;
    s64 size = 0;
    time_t mtime = 0;
    SDL_Texture *thumb = nullptr;
    bool thumbTried = false;
    double duration = -1;
    bool hasAudio = false;
};
static std::vector<Entry> g_entries;

static void freeEntries() {
    for (auto &e : g_entries) if (e.thumb) SDL_DestroyTexture(e.thumb);
    g_entries.clear();
}

static void scanDir() {
    freeEntries();
    DIR *d = opendir(kDir);
    if (!d) return;
    while (dirent *de = readdir(d)) {
        std::string n = de->d_name;
        if (n.size() < 5) continue;
        std::string ext = n.substr(n.size() - 4);
        for (auto &ch : ext) ch = char(tolower(ch));
        if (ext != ".mp4") continue;
        Entry e;
        e.name = n;
        e.path = std::string(kDir) + "/" + n;
        struct stat st;
        if (stat(e.path.c_str(), &st) == 0) { e.size = st.st_size; e.mtime = st.st_mtime; }
        g_entries.push_back(std::move(e));
    }
    closedir(d);
    // 新しい番号が先頭に来るように
    std::sort(g_entries.begin(), g_entries.end(), [](const Entry &a, const Entry &b) { return a.name > b.name; });
}

// 最初の1フレームを縮小してサムネイルにする
static void makeThumb(Entry &e) {
    e.thumbTried = true;
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, avPath(e.path).c_str(), nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return; }
    if (fmt->duration > 0) e.duration = fmt->duration / double(AV_TIME_BASE);
    e.hasAudio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;

    const AVCodec *codec = nullptr;
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (vi < 0 || !codec) { avformat_close_input(&fmt); return; }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[vi]->codecpar);
    if (avcodec_open2(dec, codec, nullptr) < 0) { avcodec_free_context(&dec); avformat_close_input(&fmt); return; }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool got = false;
    for (int tries = 0; tries < 300 && !got && av_read_frame(fmt, pkt) >= 0; tries++) {
        if (pkt->stream_index == vi && avcodec_send_packet(dec, pkt) >= 0)
            got = avcodec_receive_frame(dec, frame) >= 0;
        av_packet_unref(pkt);
    }
    if (!got) {   // デコーダに残っているフレームを出す
        avcodec_send_packet(dec, nullptr);
        got = avcodec_receive_frame(dec, frame) >= 0;
    }
    if (got) {
        const int tw = 288, th = 162;
        SwsContext *sws = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
                                         tw, th, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws) {
            std::vector<u8> rgba(size_t(tw) * th * 4);
            u8 *dst[4] = { rgba.data(), nullptr, nullptr, nullptr };
            int dstStride[4] = { tw * 4, 0, 0, 0 };
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
            sws_freeContext(sws);
            e.thumb = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, tw, th);
            if (e.thumb) SDL_UpdateTexture(e.thumb, nullptr, rgba.data(), tw * 4);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

// ---------------------------------------------------------------------------
// 再生
// ---------------------------------------------------------------------------
struct Player {
    AVFormatContext *fmt = nullptr;
    int vi = -1, ai = -1;
    AVCodecContext *vdec = nullptr, *adec = nullptr;
    SwrContext *swr = nullptr;
    SwsContext *sws = nullptr;          // yuv420p 以外が来たとき用
    SDL_Texture *tex = nullptr;
    int texW = 0, texH = 0;
    SDL_AudioDeviceID dev = 0;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    std::deque<AVFrame *> vq;           // デコード済みで表示待ちの映像
    bool eof = false;
    bool paused = false;
    double duration = 0;
    // 時計: 再生位置 = base + (今 - startMs)/1000(一時停止中は base のまま)
    double base = 0;
    u64 startMs = 0;
    double audioEnd = 0;                // キューに入れた音声の終わりの時刻
    double seekTarget = -1;             // シーク直後、この時刻より前のフレームは捨てる
    double shown = 0;                   // 最後に表示したフレームの時刻
    u64 osdUntil = 0;                   // 再生バーを出しておく時刻
};

static u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

static double clockPos(const Player &p) {
    if (p.paused) return p.base;
    return p.base + (nowMs() - p.startMs) / 1000.0;
}

static void setClock(Player &p, double pos) {
    p.base = pos;
    p.startMs = nowMs();
}

static constexpr int kOutRate = 48000;
static constexpr double kBytesPerSec = kOutRate * 4.0;

static void closePlayer(Player &p) {
    if (p.dev) { SDL_CloseAudioDevice(p.dev); p.dev = 0; }
    for (auto *f : p.vq) av_frame_free(&f);
    p.vq.clear();
    if (p.tex) SDL_DestroyTexture(p.tex);
    if (p.sws) sws_freeContext(p.sws);
    if (p.swr) swr_free(&p.swr);
    if (p.vdec) avcodec_free_context(&p.vdec);
    if (p.adec) avcodec_free_context(&p.adec);
    if (p.pkt) av_packet_free(&p.pkt);
    if (p.frame) av_frame_free(&p.frame);
    if (p.fmt) avformat_close_input(&p.fmt);
    p = Player{};
}

static bool openPlayer(Player &p, const std::string &path, std::string &err) {
    if (avformat_open_input(&p.fmt, avPath(path).c_str(), nullptr, nullptr) < 0) { err = "ファイルを開けません"; return false; }
    if (avformat_find_stream_info(p.fmt, nullptr) < 0) { err = "動画の情報を読めません"; return false; }
    p.duration = p.fmt->duration > 0 ? p.fmt->duration / double(AV_TIME_BASE) : 0;

    const AVCodec *vc = nullptr, *ac = nullptr;
    p.vi = av_find_best_stream(p.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);
    if (p.vi < 0 || !vc) { err = "映像がありません"; return false; }
    p.vdec = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(p.vdec, p.fmt->streams[p.vi]->codecpar);
    p.vdec->thread_count = 3;                                // 3コアで並列デコード
    p.vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(p.vdec, vc, nullptr) < 0) { err = "映像をデコードできません"; return false; }

    p.ai = av_find_best_stream(p.fmt, AVMEDIA_TYPE_AUDIO, -1, p.vi, &ac, 0);
    if (p.ai >= 0 && ac) {
        p.adec = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(p.adec, p.fmt->streams[p.ai]->codecpar);
        if (avcodec_open2(p.adec, ac, nullptr) < 0) { avcodec_free_context(&p.adec); p.ai = -1; }
    } else {
        p.ai = -1;
    }

    if (p.adec) {
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&p.swr, &outLayout, AV_SAMPLE_FMT_S16, kOutRate,
                                &p.adec->ch_layout, p.adec->sample_fmt, p.adec->sample_rate, 0, nullptr) < 0 ||
            swr_init(p.swr) < 0) {
            if (p.swr) swr_free(&p.swr);
            avcodec_free_context(&p.adec);
            p.ai = -1;
        } else {
            SDL_AudioSpec want{}, have{};
            want.freq = kOutRate;
            want.format = AUDIO_S16LSB;
            want.channels = 2;
            want.samples = 1024;
            p.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
            if (p.dev) SDL_PauseAudioDevice(p.dev, 0);
        }
    }

    p.pkt = av_packet_alloc();
    p.frame = av_frame_alloc();
    setClock(p, 0);
    p.osdUntil = nowMs() + 2500;
    return true;
}

static double frameTime(const Player &p, const AVFrame *f, int stream) {
    s64 ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = f->pts;
    if (ts == AV_NOPTS_VALUE) return 0;
    return ts * av_q2d(p.fmt->streams[stream]->time_base);
}

static void queueAudio(Player &p, AVFrame *f) {
    if (!p.dev || !p.swr) return;
    double t = frameTime(p, f, p.ai);
    const double len = f->nb_samples / double(f->sample_rate > 0 ? f->sample_rate : kOutRate);
    if (p.seekTarget >= 0 && t + len < p.seekTarget) return;   // シーク先より前の音は捨てる
    int outMax = swr_get_out_samples(p.swr, f->nb_samples);
    if (outMax <= 0) return;
    static std::vector<u8> buf;
    buf.resize(size_t(outMax) * 4);
    u8 *out[1] = { buf.data() };
    int n = swr_convert(p.swr, out, outMax, (const u8 **)f->extended_data, f->nb_samples);
    if (n > 0) SDL_QueueAudio(p.dev, buf.data(), u32(n) * 4);
    p.audioEnd = t + len;
}

// パケットを読んで、映像は表示待ちキューへ、音声はそのまま再生キューへ
static void decodeSome(Player &p) {
    for (int iter = 0; iter < 24 && !p.eof; iter++) {
        const double queuedAudio = p.dev ? SDL_GetQueuedAudioSize(p.dev) / kBytesPerSec : 1.0;
        if (p.vq.size() >= 10 && queuedAudio > 0.25) break;
        if (p.vq.size() >= 30) break;   // メモリを使いすぎないように

        int r = av_read_frame(p.fmt, p.pkt);
        if (r < 0) {
            p.eof = true;
            avcodec_send_packet(p.vdec, nullptr);
        } else if (p.pkt->stream_index == p.vi) {
            avcodec_send_packet(p.vdec, p.pkt);
        } else if (p.pkt->stream_index == p.ai && p.adec) {
            if (avcodec_send_packet(p.adec, p.pkt) >= 0)
                while (avcodec_receive_frame(p.adec, p.frame) >= 0) queueAudio(p, p.frame);
            av_packet_unref(p.pkt);
            continue;
        }
        if (r >= 0) av_packet_unref(p.pkt);

        while (avcodec_receive_frame(p.vdec, p.frame) >= 0) {
            if (p.seekTarget >= 0 && frameTime(p, p.frame, p.vi) < p.seekTarget - 0.02) {
                av_frame_unref(p.frame);
                continue;
            }
            AVFrame *f = av_frame_alloc();
            av_frame_move_ref(f, p.frame);
            p.vq.push_back(f);
        }
    }
    if (p.seekTarget >= 0 && !p.vq.empty()) p.seekTarget = -1;
}

static void uploadFrame(Player &p, AVFrame *f) {
    if (!p.tex || p.texW != f->width || p.texH != f->height) {
        if (p.tex) SDL_DestroyTexture(p.tex);
        p.tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
        p.texW = f->width; p.texH = f->height;
    }
    if (!p.tex) return;
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        SDL_UpdateYUVTexture(p.tex, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1],
                             f->data[2], f->linesize[2]);
        return;
    }
    // 念のため: 他の形式は yuv420p に変換
    static AVFrame *tmp = nullptr;
    if (!tmp) tmp = av_frame_alloc();
    if (tmp->width != f->width || tmp->height != f->height) {
        av_frame_unref(tmp);
        tmp->format = AV_PIX_FMT_YUV420P; tmp->width = f->width; tmp->height = f->height;
        av_frame_get_buffer(tmp, 32);
    }
    p.sws = sws_getCachedContext(p.sws, f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
                                 AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!p.sws) return;
    sws_scale(p.sws, f->data, f->linesize, 0, f->height, tmp->data, tmp->linesize);
    SDL_UpdateYUVTexture(p.tex, nullptr, tmp->data[0], tmp->linesize[0], tmp->data[1], tmp->linesize[1],
                         tmp->data[2], tmp->linesize[2]);
}

static void seekTo(Player &p, double target) {
    if (p.duration > 0 && target > p.duration - 1) target = p.duration - 1;
    if (target < 0) target = 0;
    av_seek_frame(p.fmt, -1, s64(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(p.vdec);
    if (p.adec) avcodec_flush_buffers(p.adec);
    for (auto *f : p.vq) av_frame_free(&f);
    p.vq.clear();
    if (p.dev) SDL_ClearQueuedAudio(p.dev);
    p.eof = false;
    p.seekTarget = target;
    p.audioEnd = target;
    setClock(p, target);
    p.osdUntil = nowMs() + 2500;
}

static void togglePause(Player &p) {
    if (p.paused) {
        p.paused = false;
        setClock(p, p.base);
        if (p.dev) SDL_PauseAudioDevice(p.dev, 0);
    } else {
        p.base = clockPos(p);
        p.paused = true;
        if (p.dev) SDL_PauseAudioDevice(p.dev, 1);
    }
    p.osdUntil = nowMs() + 2500;
}

static void drawPlayerOsd(const Player &p, const std::string &title) {
    const double pos = std::min(p.shown, p.duration > 0 ? p.duration : p.shown);
    fillRect(0, 0, W, 64, 0, 0, 0, 160);
    drawText(g_fontM, title, 30, 16, kWhite);
    fillRect(0, H - 96, W, 96, 0, 0, 0, 160);
    const int bx = 40, by = H - 80, bw = W - 80, bh = 8;
    fillRect(bx, by, bw, bh, 90, 90, 90);
    if (p.duration > 0) fillRect(bx, by, int(bw * std::clamp(pos / p.duration, 0.0, 1.0)), bh, 0, 220, 200);
    drawText(g_fontS, fmtTime(pos) + " / " + fmtTime(p.duration), bx, by + 18, kWhite);
    drawText(g_fontS, std::string(p.paused ? "A: 再生" : "A: 一時停止") + "    ←→: 10秒   L/R: 30秒   B: 戻る",
             W - 40, by + 18, kGray, Right);
    if (p.paused) drawText(g_fontL, "一時停止中", W / 2, H / 2 - 24, kWhite, Center);
}

static void playVideo(const Entry &e) {
    Player p;
    std::string err;
    if (!openPlayer(p, e.path, err)) {
        closePlayer(p);
        // エラーを2秒表示
        const u64 until = nowMs() + 2000;
        while (appletMainLoop() && nowMs() < until) {
            SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
            SDL_RenderClear(g_ren);
            drawText(g_fontM, "再生できません: " + err, W / 2, H / 2 - 16, kRed, Center);
            SDL_RenderPresent(g_ren);
        }
        return;
    }
    // 再生中はCPUを速くする(本体のロード画面と同じ仕組み)
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

    while (appletMainLoop()) {
        g_frameNo++;
        Input in = readInput();
        if (in.down & HidNpadButton_B) break;
        if (in.down & HidNpadButton_A) togglePause(p);
        if (in.down & HidNpadButton_AnyLeft)  seekTo(p, clockPos(p) - 10);
        if (in.down & HidNpadButton_AnyRight) seekTo(p, clockPos(p) + 10);
        if (in.down & HidNpadButton_L)        seekTo(p, clockPos(p) - 30);
        if (in.down & HidNpadButton_R)        seekTo(p, clockPos(p) + 30);
        if (in.down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_Y | HidNpadButton_X))
            p.osdUntil = nowMs() + 2500;

        if (!p.paused) {
            decodeSome(p);

            // 音声があれば、音声の進み具合に時計を合わせる(ずれが大きいときだけ)
            if (p.dev && p.seekTarget < 0) {
                const double queued = SDL_GetQueuedAudioSize(p.dev) / kBytesPerSec;
                if (queued > 0.02) {
                    const double aclk = p.audioEnd - queued;
                    if (std::fabs(aclk - clockPos(p)) > 0.08) setClock(p, aclk);
                }
            }

            // 表示時刻になったフレームを出す(遅れているときは古いものを飛ばす)
            const double now = clockPos(p);
            AVFrame *show = nullptr;
            while (!p.vq.empty() && frameTime(p, p.vq.front(), p.vi) <= now + 0.005) {
                if (show) av_frame_free(&show);
                show = p.vq.front();
                p.vq.pop_front();
            }
            if (show) {
                p.shown = frameTime(p, show, p.vi);
                uploadFrame(p, show);
                av_frame_free(&show);
            }

            // 最後まで再生したら一覧に戻る
            if (p.eof && p.vq.empty() && (!p.dev || SDL_GetQueuedAudioSize(p.dev) == 0)) break;
        }

        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        if (p.tex) {
            SDL_Rect dst{ 0, 0, W, H };
            SDL_RenderCopy(g_ren, p.tex, nullptr, &dst);
        }
        if (p.paused || nowMs() < p.osdUntil) drawPlayerOsd(p, e.name);
        SDL_RenderPresent(g_ren);
        trimTextCache();
    }

    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    closePlayer(p);
}

// ---------------------------------------------------------------------------
// 一覧画面
// ---------------------------------------------------------------------------
constexpr int kCols = 4;
constexpr int kCellW = 300, kCellH = 222;
constexpr int kGridX = (W - kCols * kCellW) / 2, kGridY = 84;
constexpr int kThumbW = 288, kThumbH = 162;
constexpr int kVisibleRows = 3;

static void drawList(int sel, int scrollRow, const std::string &toast, u64 toastUntil);

static bool confirmDelete(int sel, int scrollRow) {
    const Entry &e = g_entries[sel];
    while (appletMainLoop()) {
        g_frameNo++;
        Input in = readInput();
        if (in.down & HidNpadButton_A) return true;
        if (in.down & (HidNpadButton_B | HidNpadButton_X)) return false;

        drawList(sel, scrollRow, "", 0);
        fillRect(0, 0, W, H, 0, 0, 0, 150);
        const int bw = 640, bh = 240, bx = (W - bw) / 2, by = (H - bh) / 2;
        fillRect(bx, by, bw, bh, 40, 40, 44);
        strokeRect(bx, by, bw, bh, 2, 90, 90, 96);
        drawText(g_fontM, "この動画を削除しますか？", W / 2, by + 40, kWhite, Center);
        drawText(g_fontS, e.name, W / 2, by + 92, kGray, Center);
        drawText(g_fontM, "A: 削除する", W / 2 - 140, by + 160, kRed, Center);
        drawText(g_fontM, "B: やめる", W / 2 + 140, by + 160, kWhite, Center);
        SDL_RenderPresent(g_ren);
    }
    return false;
}

static void drawList(int sel, int scrollRow, const std::string &toast, u64 toastUntil) {
    SDL_SetRenderDrawColor(g_ren, 24, 24, 28, 255);
    SDL_RenderClear(g_ren);

    // ヘッダー
    fillRect(0, 0, W, 68, 36, 36, 42);
    drawText(g_fontL, "Rec Gallery", 36, 12, kWhite);
    char cnt[64];
    snprintf(cnt, sizeof(cnt), "%d 本の動画", int(g_entries.size()));
    drawText(g_fontS, cnt, W - 36, 24, kGray, Right);

    if (g_entries.empty()) {
        drawText(g_fontM, "動画がありません", W / 2, H / 2 - 40, kWhite, Center);
        drawText(g_fontS, "Rec Menu で録画すると /switch/rec-menu/ に保存され、ここに出てきます", W / 2, H / 2 + 10,
                 kGray, Center);
    }

    for (int i = scrollRow * kCols; i < int(g_entries.size()) && i < (scrollRow + kVisibleRows) * kCols; i++) {
        const Entry &e = g_entries[i];
        const int row = i / kCols - scrollRow, col = i % kCols;
        const int x = kGridX + col * kCellW + (kCellW - kThumbW) / 2;
        const int y = kGridY + row * kCellH;
        if (e.thumb) {
            SDL_Rect r{ x, y, kThumbW, kThumbH };
            SDL_RenderCopy(g_ren, e.thumb, nullptr, &r);
        } else {
            fillRect(x, y, kThumbW, kThumbH, 50, 50, 56);
            drawText(g_fontS, e.thumbTried ? "読み込めません" : "読み込み中…", x + kThumbW / 2, y + kThumbH / 2 - 12,
                     kGray, Center);
        }
        if (e.duration >= 0) {
            const std::string d = fmtTime(e.duration);
            fillRect(x + kThumbW - 74, y + kThumbH - 30, 70, 26, 0, 0, 0, 170);
            drawText(g_fontS, d, x + kThumbW - 39, y + kThumbH - 29, kWhite, Center);
        }
        drawText(g_fontS, e.name, x, y + kThumbH + 8, i == sel ? kWhite : kGray);
        if (i == sel) strokeRect(x - 6, y - 6, kThumbW + 12, kThumbH + 12, 4, 0, 220, 200);
    }

    // フッター
    fillRect(0, H - 48, W, 48, 36, 36, 42);
    if (!g_entries.empty() && sel < int(g_entries.size())) {
        const Entry &e = g_entries[sel];
        char info[96];
        snprintf(info, sizeof(info), "%.1f MB%s", e.size / 1048576.0,
                 !e.thumbTried ? "" : (e.hasAudio ? "   音声あり" : "   音声なし"));
        drawText(g_fontS, info, 36, H - 38, kGray);
    }
    drawText(g_fontS, "A: 再生    X: 削除    Y: 更新    +: 終了", W - 36, H - 38, kWhite, Right);

    if (!toast.empty() && nowMs() < toastUntil) {
        fillRect(W / 2 - 300, H - 120, 600, 52, 0, 0, 0, 200);
        drawText(g_fontS, toast, W / 2, H - 106, kCyan, Center);
    }
}

static void galleryLoop() {
    int sel = 0, scrollRow = 0;
    std::string toast;
    u64 toastUntil = 0;
    scanDir();

    while (appletMainLoop() && !g_quit) {
        g_frameNo++;
        Input in = readInput();
        const int n = int(g_entries.size());

        if (in.down & HidNpadButton_Plus) break;
        if (in.down & HidNpadButton_B) break;
        if (in.down & HidNpadButton_Y) { scanDir(); sel = 0; scrollRow = 0; }

        if (n > 0) {
            if (in.repeat & HidNpadButton_AnyRight) sel = std::min(sel + 1, n - 1);
            if (in.repeat & HidNpadButton_AnyLeft)  sel = std::max(sel - 1, 0);
            if (in.repeat & HidNpadButton_AnyDown)  sel = std::min(sel + kCols, n - 1);
            if (in.repeat & HidNpadButton_AnyUp)    sel = std::max(sel - kCols, 0);
            const int row = sel / kCols;
            if (row < scrollRow) scrollRow = row;
            if (row >= scrollRow + kVisibleRows) scrollRow = row - kVisibleRows + 1;

            if (in.down & HidNpadButton_A) {
                playVideo(g_entries[sel]);
                padUpdate(&g_pad);   // 再生画面で押したボタンを持ち越さない
                continue;
            }
            if (in.down & HidNpadButton_X) {
                if (confirmDelete(sel, scrollRow)) {
                    const std::string name = g_entries[sel].name;
                    if (remove(g_entries[sel].path.c_str()) == 0) {
                        if (g_entries[sel].thumb) SDL_DestroyTexture(g_entries[sel].thumb);
                        g_entries.erase(g_entries.begin() + sel);
                        if (sel >= int(g_entries.size())) sel = std::max(0, int(g_entries.size()) - 1);
                        toast = name + " を削除しました";
                    } else {
                        toast = "削除できませんでした";
                    }
                    toastUntil = nowMs() + 2500;
                }
                padUpdate(&g_pad);
                continue;
            }
        }

        // サムネイルは1フレームに1つずつ作る(画面が固まらないように)。見えている物を優先
        for (int i = scrollRow * kCols; i < n && i < (scrollRow + kVisibleRows + 1) * kCols; i++) {
            if (!g_entries[i].thumbTried) { drawList(sel, scrollRow, toast, toastUntil); SDL_RenderPresent(g_ren); makeThumb(g_entries[i]); break; }
        }

        drawList(sel, scrollRow, toast, toastUntil);
        SDL_RenderPresent(g_ren);
        trimTextCache();
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    plInitialize(PlServiceType_User);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return 1;
    g_win = SDL_CreateWindow("Rec Gallery", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, 0);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_win || !g_ren) return 1;

    TTF_Init();
    PlFontData font{};
    if (R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
        g_fontL = TTF_OpenFontRW(SDL_RWFromConstMem(font.address, int(font.size)), 1, 34);
        g_fontM = TTF_OpenFontRW(SDL_RWFromConstMem(font.address, int(font.size)), 1, 28);
        g_fontS = TTF_OpenFontRW(SDL_RWFromConstMem(font.address, int(font.size)), 1, 21);
    }
    if (!g_fontL || !g_fontM || !g_fontS) return 1;

    av_log_set_level(AV_LOG_QUIET);
    galleryLoop();

    freeEntries();
    for (auto &kv : g_textCache) SDL_DestroyTexture(kv.second.tex);
    g_textCache.clear();
    TTF_CloseFont(g_fontL); TTF_CloseFont(g_fontM); TTF_CloseFont(g_fontS);
    TTF_Quit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    plExit();
    return 0;
}
