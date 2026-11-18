// Rec Gallery - Rec Menu で撮った動画(*.mp4)と写真(*.jpg)を、アルバムのように一覧・再生・削除する
//
// 操作:
//   一覧   … 十字キー: 選択   A: 再生/表示   X: 削除   L/R: 動画⇔写真   −: 並び替え   Y: 更新   +/B: 終了
//            タッチ: サムネイルをタップで開く、上下にドラッグでスクロール、タブ・並び順もタップで切り替え
//   動画(タッチ) … タップ: 一時停止/再開   再生バーをなぞる: 好きな位置へ   左上「戻る」: 一覧へ
//   写真(タッチ) … 左右にスワイプ: 前/次   タップ: 表示の切り替え   右下「削除」・左上「戻る」
//   動画   … A: 一時停止/再開   ←/→: 10秒戻る/進む   L/R: 30秒戻る/進む   B: 一覧に戻る
//   写真   … ←/→: 前/次   X: 削除   B: 一覧に戻る
#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
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
// 入力(ボタンは押しっぱなしでの連続移動つき。タッチはタップとドラッグ)
// ---------------------------------------------------------------------------
static u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

struct Touch {
    bool down = false;   // 触った瞬間
    bool held = false;   // 触っている間
    bool up = false;     // 離した瞬間
    bool tap = false;    // 動かさずに短く触って離した
    bool moved = false;  // 触ってから24px以上動いた
    int x = 0, y = 0;    // 今の位置(離した瞬間は最後の位置)
    int sx = 0, sy = 0;  // 触り始めた位置
    int dx() const { return x - sx; }
    int dy() const { return y - sy; }
};

struct Input { u64 down = 0; u64 held = 0; u64 repeat = 0; Touch t; };

static Touch readTouch() {
    static bool was = false, moved = false;
    static int sx = 0, sy = 0, lx = 0, ly = 0;
    static u64 startMs = 0;
    HidTouchScreenState st{};
    hidGetTouchScreenStates(&st, 1);
    const bool now = st.count > 0;
    Touch t;
    if (now) { t.x = int(st.touches[0].x); t.y = int(st.touches[0].y); }
    if (now && !was) { sx = t.x; sy = t.y; startMs = nowMs(); moved = false; t.down = true; }
    if (now) {
        t.held = true;
        if (std::abs(t.x - sx) > 24 || std::abs(t.y - sy) > 24) moved = true;
        lx = t.x; ly = t.y;
    }
    if (!now && was) {
        t.up = true;
        t.x = lx; t.y = ly;
        t.tap = !moved && nowMs() - startMs < 600;
    }
    t.sx = sx; t.sy = sy; t.moved = moved;
    was = now;
    return t;
}

static bool hit(const Touch &t, int x, int y, int w, int h) {
    return t.x >= x && t.x < x + w && t.y >= y && t.y < y + h;
}

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
    in.t = readTouch();
    return in;
}

// ---------------------------------------------------------------------------
// ファイル一覧(動画 / 写真)
// ---------------------------------------------------------------------------
enum Tab { TabVideo = 0, TabPhoto = 1 };
enum SortMode { SortNewest = 0, SortOldest, SortLargest, SortSmallest, SortCount };
static const char *kSortNames[SortCount] = { "新しい順", "古い順", "大きい順", "小さい順" };

struct Entry {
    std::string path, name;
    s64 size = 0;
    time_t mtime = 0;
    SDL_Texture *thumb = nullptr;
    bool thumbTried = false;
    double duration = -1;
    bool hasAudio = false;
};
static std::vector<Entry> g_lists[2];   // [TabVideo], [TabPhoto]
static Tab g_tab = TabVideo;
static SortMode g_sort = SortNewest;

static std::vector<Entry> &cur() { return g_lists[g_tab]; }

static void freeList(std::vector<Entry> &v) {
    for (auto &e : v) if (e.thumb) SDL_DestroyTexture(e.thumb);
    v.clear();
}

static void sortList(std::vector<Entry> &v) {
    std::stable_sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
        switch (g_sort) {
            case SortOldest:   if (a.mtime != b.mtime) return a.mtime < b.mtime; return a.name < b.name;
            case SortLargest:  if (a.size != b.size) return a.size > b.size; return a.name > b.name;
            case SortSmallest: if (a.size != b.size) return a.size < b.size; return a.name < b.name;
            default:           if (a.mtime != b.mtime) return a.mtime > b.mtime; return a.name > b.name;
        }
    });
}

static void scanDir(Tab tab) {
    std::vector<Entry> &list = g_lists[tab];
    freeList(list);
    DIR *d = opendir(kDir);
    if (!d) return;
    while (dirent *de = readdir(d)) {
        std::string n = de->d_name;
        if (n.size() < 5) continue;
        std::string ext = n.substr(n.size() - 4);
        for (auto &ch : ext) ch = char(tolower(ch));
        const bool want = tab == TabVideo ? ext == ".mp4" : (ext == ".jpg" || ext == "jpeg");
        if (!want) continue;
        Entry e;
        e.name = n;
        e.path = std::string(kDir) + "/" + n;
        struct stat st;
        if (stat(e.path.c_str(), &st) == 0) { e.size = st.st_size; e.mtime = st.st_mtime; }
        list.push_back(std::move(e));
    }
    closedir(d);
    sortList(list);
}

static std::string fmtDate(time_t t) {
    if (t <= 0) return "日付不明";
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M", &tmv);
    return buf;
}

constexpr int kThumbW = 288, kThumbH = 162;

// 画像を読み込んで、指定の大きさに縮小したテクスチャを作る(縦横比は保って中央に置く)
static SDL_Texture *loadImageTexture(const std::string &path, int w, int h) {
    SDL_Surface *src = IMG_Load(path.c_str());
    if (!src) return nullptr;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(src);
    if (!rgba) return nullptr;
    if (w <= 0 || h <= 0) {   // 原寸
        SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, rgba);
        SDL_FreeSurface(rgba);
        return t;
    }
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) { SDL_FreeSurface(rgba); return nullptr; }
    SDL_FillRect(dst, nullptr, SDL_MapRGBA(dst->format, 0, 0, 0, 255));
    const double sc = std::min(double(w) / rgba->w, double(h) / rgba->h);
    SDL_Rect r{ 0, 0, int(rgba->w * sc), int(rgba->h * sc) };
    r.x = (w - r.w) / 2; r.y = (h - r.h) / 2;
    SDL_BlitScaled(rgba, nullptr, dst, &r);
    SDL_FreeSurface(rgba);
    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, dst);
    SDL_FreeSurface(dst);
    return t;
}

// 動画は最初の1フレームを縮小してサムネイルにする
static void makeVideoThumb(Entry &e) {
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
        SwsContext *sws = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
                                         kThumbW, kThumbH, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws) {
            std::vector<u8> rgba(size_t(kThumbW) * kThumbH * 4);
            u8 *dst[4] = { rgba.data(), nullptr, nullptr, nullptr };
            int dstStride[4] = { kThumbW * 4, 0, 0, 0 };
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
            sws_freeContext(sws);
            e.thumb = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, kThumbW, kThumbH);
            if (e.thumb) SDL_UpdateTexture(e.thumb, nullptr, rgba.data(), kThumbW * 4);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

static void makeThumb(Entry &e, Tab tab) {
    e.thumbTried = true;
    if (tab == TabPhoto) e.thumb = loadImageTexture(e.path, kThumbW, kThumbH);
    else makeVideoThumb(e);
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
    bool scrubbing = false;             // 再生バーを指でなぞっている
    double scrubPos = 0;
};

// 再生バーの位置(タッチ判定にも使う)
constexpr int kBarX = 40, kBarY = H - 80, kBarW = W - 80, kBarH = 8;
// 左上の「戻る」ボタン
constexpr int kBackW = 150, kBackH = 64;

static void drawBackButton() {
    fillRect(12, 10, kBackW - 24, kBackH - 20, 255, 255, 255, 40);
    drawText(g_fontS, "＜ 戻る", kBackW / 2, 20, kWhite, Center);
}

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
    double pos = p.scrubbing ? p.scrubPos : std::min(p.shown, p.duration > 0 ? p.duration : p.shown);
    fillRect(0, 0, W, kBackH, 0, 0, 0, 160);
    drawBackButton();
    drawText(g_fontM, title, kBackW + 10, 16, kWhite);
    fillRect(0, H - 96, W, 96, 0, 0, 0, 160);
    fillRect(kBarX, kBarY, kBarW, kBarH, 90, 90, 90);
    if (p.duration > 0) {
        const int fw = int(kBarW * std::clamp(pos / p.duration, 0.0, 1.0));
        fillRect(kBarX, kBarY, fw, kBarH, 0, 220, 200);
        // つまみ
        const int r = p.scrubbing ? 14 : 10;
        fillRect(kBarX + fw - r, kBarY + kBarH / 2 - r, r * 2, r * 2, 0, 220, 200);
    }
    drawText(g_fontS, fmtTime(pos) + " / " + fmtTime(p.duration), kBarX, kBarY + 22, kWhite);
    drawText(g_fontS, std::string(p.paused ? "A/タップ: 再生" : "A/タップ: 一時停止") + "    ←→: 10秒   L/R: 30秒   バーをなぞる: 移動",
             W - 40, kBarY + 22, kGray, Right);
    if (p.paused && !p.scrubbing) drawText(g_fontL, "一時停止中", W / 2, H / 2 - 24, kWhite, Center);
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

        // ---- タッチ ----
        const Touch &t = in.t;
        const bool osdShown = p.paused || nowMs() < p.osdUntil;
        // 再生バーの近く(上下に広めに)を触ったら、指の位置まで移動する
        if (t.down && p.duration > 0 && t.y >= kBarY - 50 && t.y <= kBarY + 50) p.scrubbing = true;
        if (p.scrubbing) {
            p.scrubPos = std::clamp(double(t.x - kBarX) / kBarW, 0.0, 1.0) * p.duration;
            p.osdUntil = nowMs() + 2500;
            if (t.up) {
                p.scrubbing = false;
                seekTo(p, p.scrubPos);
            }
        } else if (t.tap) {
            if (osdShown && hit(t, 0, 0, kBackW, kBackH)) break;   // 戻る
            if (!osdShown) p.osdUntil = nowMs() + 2500;            // まずバーを出す
            else togglePause(p);
        }

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
        if (p.paused || p.scrubbing || nowMs() < p.osdUntil) drawPlayerOsd(p, e.name);
        SDL_RenderPresent(g_ren);
        trimTextCache();
    }

    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    closePlayer(p);
}

// ---------------------------------------------------------------------------
// 写真の表示
// ---------------------------------------------------------------------------
static void drawList(int sel, int scrollRow, const std::string &toast, u64 toastUntil);
static bool confirmDelete(int sel, int scrollRow, const char *what, bool overPhoto, SDL_Texture *photo);
static bool deleteEntry(int &sel, std::string &toast);

// 写真を全画面で表示する。←→で前後の写真へ。戻ったときの選択位置を sel に返す
static void viewPhotos(int &sel, std::string &toast, u64 &toastUntil) {
    SDL_Texture *tex = nullptr;
    int loaded = -1;
    u64 osdUntil = nowMs() + 2500;
    while (appletMainLoop()) {
        g_frameNo++;
        std::vector<Entry> &list = cur();
        if (list.empty()) break;
        if (sel >= int(list.size())) sel = int(list.size()) - 1;
        if (loaded != sel) {
            if (tex) SDL_DestroyTexture(tex);
            tex = loadImageTexture(list[sel].path, 0, 0);
            loaded = sel;
        }

        Input in = readInput();
        if (in.down & HidNpadButton_B) break;
        if ((in.repeat & HidNpadButton_AnyRight) || (in.down & HidNpadButton_R)) { if (sel + 1 < int(list.size())) sel++; osdUntil = nowMs() + 2500; }
        if ((in.repeat & HidNpadButton_AnyLeft) || (in.down & HidNpadButton_L))  { if (sel > 0) sel--; osdUntil = nowMs() + 2500; }
        if (in.down & (HidNpadButton_A | HidNpadButton_AnyUp | HidNpadButton_AnyDown)) osdUntil = nowMs() + 2500;

        // ---- タッチ: 左右にスワイプで前/次、タップで表示の切り替え ----
        const Touch &t = in.t;
        bool wantDelete = false;
        if (t.up && t.moved && std::abs(t.dx()) > 100 && std::abs(t.dx()) > std::abs(t.dy())) {
            if (t.dx() < 0 && sel + 1 < int(list.size())) sel++;
            if (t.dx() > 0 && sel > 0) sel--;
            osdUntil = nowMs() + 2500;
        } else if (t.tap) {
            const bool shown = nowMs() < osdUntil;
            if (shown && hit(t, 0, 0, kBackW, kBackH)) break;
            if (shown && hit(t, W - 170, H - 56, 170, 56)) wantDelete = true;
            else osdUntil = shown ? 0 : nowMs() + 2500;
        }
        if ((in.down & HidNpadButton_X) || wantDelete) {
            if (confirmDelete(sel, 0, "この写真を削除しますか？", true, tex)) {
                deleteEntry(sel, toast);
                toastUntil = nowMs() + 2500;
                loaded = -1;
                osdUntil = nowMs() + 2500;
            }
            padUpdate(&g_pad);
            continue;
        }

        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        if (tex) {
            int tw = 0, th = 0;
            SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
            const double sc = std::min(double(W) / tw, double(H) / th);
            SDL_Rect r{ 0, 0, int(tw * sc), int(th * sc) };
            r.x = (W - r.w) / 2; r.y = (H - r.h) / 2;
            SDL_RenderCopy(g_ren, tex, nullptr, &r);
        } else {
            drawText(g_fontM, "読み込めません", W / 2, H / 2 - 16, kRed, Center);
        }
        if (nowMs() < osdUntil) {
            const Entry &e = list[sel];
            fillRect(0, 0, W, kBackH, 0, 0, 0, 160);
            drawBackButton();
            drawText(g_fontM, e.name, kBackW + 10, 16, kWhite);
            char pos[32];
            snprintf(pos, sizeof(pos), "%d / %d", sel + 1, int(list.size()));
            drawText(g_fontS, pos, W - 30, 22, kGray, Right);
            fillRect(0, H - 56, W, 56, 0, 0, 0, 160);
            char info[96];
            snprintf(info, sizeof(info), "%s    %.1f MB", fmtDate(e.mtime).c_str(), e.size / 1048576.0);
            drawText(g_fontS, info, 30, H - 42, kGray);
            drawText(g_fontS, "←→/スワイプ: 前/次    B: 戻る", W - 200, H - 42, kGray, Right);
            fillRect(W - 160, H - 50, 140, 44, 200, 60, 60, 200);
            drawText(g_fontS, "X: 削除", W - 90, H - 42, kWhite, Center);
        }
        SDL_RenderPresent(g_ren);
        trimTextCache();
    }
    if (tex) SDL_DestroyTexture(tex);
}

// ---------------------------------------------------------------------------
// 一覧画面
// ---------------------------------------------------------------------------
constexpr int kCols = 4;
constexpr int kCellW = 300, kCellH = 222;
constexpr int kGridX = (W - kCols * kCellW) / 2, kGridY = 120;
constexpr int kVisibleRows = 3;

static bool confirmDelete(int sel, int scrollRow, const char *what, bool overPhoto, SDL_Texture *photo) {
    const Entry &e = cur()[sel];
    while (appletMainLoop()) {
        g_frameNo++;
        Input in = readInput();
        if (in.down & HidNpadButton_A) return true;
        if (in.down & (HidNpadButton_B | HidNpadButton_X)) return false;
        {
            const int bw = 640, bh = 240, bx = (W - bw) / 2, by = (H - bh) / 2;
            if (in.t.tap) {
                if (hit(in.t, W / 2 - 270, by + 140, 260, 70)) return true;    // 削除する
                if (hit(in.t, W / 2 + 10, by + 140, 260, 70)) return false;    // やめる
                if (!hit(in.t, bx, by, bw, bh)) return false;                  // 外側
            }
        }

        if (overPhoto) {
            SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
            SDL_RenderClear(g_ren);
            if (photo) SDL_RenderCopy(g_ren, photo, nullptr, nullptr);
        } else {
            drawList(sel, scrollRow, "", 0);
        }
        fillRect(0, 0, W, H, 0, 0, 0, 150);
        const int bw = 640, bh = 240, bx = (W - bw) / 2, by = (H - bh) / 2;
        fillRect(bx, by, bw, bh, 40, 40, 44);
        strokeRect(bx, by, bw, bh, 2, 90, 90, 96);
        drawText(g_fontM, what, W / 2, by + 40, kWhite, Center);
        drawText(g_fontS, e.name, W / 2, by + 92, kGray, Center);
        fillRect(W / 2 - 270, by + 146, 260, 60, 120, 40, 40);
        fillRect(W / 2 + 10, by + 146, 260, 60, 70, 70, 76);
        drawText(g_fontM, "A: 削除する", W / 2 - 140, by + 160, kWhite, Center);
        drawText(g_fontM, "B: やめる", W / 2 + 140, by + 160, kWhite, Center);
        SDL_RenderPresent(g_ren);
    }
    return false;
}

static bool deleteEntry(int &sel, std::string &toast) {
    std::vector<Entry> &list = cur();
    const std::string name = list[sel].name;
    if (remove(list[sel].path.c_str()) != 0) {
        toast = "削除できませんでした";
        return false;
    }
    if (list[sel].thumb) SDL_DestroyTexture(list[sel].thumb);
    list.erase(list.begin() + sel);
    if (sel >= int(list.size())) sel = std::max(0, int(list.size()) - 1);
    toast = name + " を削除しました";
    return true;
}

static void drawList(int sel, int scrollRow, const std::string &toast, u64 toastUntil) {
    const std::vector<Entry> &list = cur();
    SDL_SetRenderDrawColor(g_ren, 24, 24, 28, 255);
    SDL_RenderClear(g_ren);

    // ヘッダー
    fillRect(0, 0, W, 104, 36, 36, 42);
    drawText(g_fontL, "Rec Gallery", 36, 10, kWhite);
    char sortLabel[64];
    snprintf(sortLabel, sizeof(sortLabel), "並び順: %s  (−で変更)", kSortNames[g_sort]);
    drawText(g_fontS, sortLabel, W - 36, 20, kGray, Right);

    // タブ
    const char *tabNames[2] = { "動画", "写真" };
    for (int t = 0; t < 2; t++) {
        char label[32];
        snprintf(label, sizeof(label), "%s (%d)", tabNames[t], int(g_lists[t].size()));
        const int tx = 36 + t * 180;
        drawText(g_fontM, label, tx + 80, 62, t == g_tab ? kWhite : kGray, Center);
        if (t == g_tab) fillRect(tx + 10, 98, 140, 4, 0, 220, 200);
    }
    drawText(g_fontS, "L/R: 切り替え", 36 + 2 * 180 + 10, 68, kGray);

    if (list.empty()) {
        drawText(g_fontM, g_tab == TabVideo ? "動画がありません" : "写真がありません", W / 2, H / 2 - 30, kWhite, Center);
        drawText(g_fontS, "Rec Menu で撮ると /switch/rec-menu/ に保存され、ここに出てきます", W / 2, H / 2 + 20,
                 kGray, Center);
    }

    for (int i = scrollRow * kCols; i < int(list.size()) && i < (scrollRow + kVisibleRows) * kCols; i++) {
        const Entry &e = list[i];
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
        if (g_tab == TabVideo && e.duration >= 0) {
            const std::string d = fmtTime(e.duration);
            fillRect(x + kThumbW - 74, y + kThumbH - 30, 70, 26, 0, 0, 0, 170);
            drawText(g_fontS, d, x + kThumbW - 39, y + kThumbH - 29, kWhite, Center);
        }
        drawText(g_fontS, fmtDate(e.mtime), x, y + kThumbH + 8, i == sel ? kWhite : kGray);
        if (i == sel) strokeRect(x - 6, y - 6, kThumbW + 12, kThumbH + 12, 4, 0, 220, 200);
    }

    // フッター
    fillRect(0, H - 48, W, 48, 36, 36, 42);
    if (!list.empty() && sel < int(list.size())) {
        const Entry &e = list[sel];
        char info[160];
        snprintf(info, sizeof(info), "%s   %.1f MB%s", e.name.c_str(), e.size / 1048576.0,
                 g_tab != TabVideo || !e.thumbTried ? "" : (e.hasAudio ? "   音声あり" : "   音声なし"));
        drawText(g_fontS, info, 36, H - 38, kGray);
    }
    drawText(g_fontS, g_tab == TabVideo ? "A: 再生   X: 削除   Y: 更新   +: 終了" : "A: 表示   X: 削除   Y: 更新   +: 終了",
             W - 36, H - 38, kWhite, Right);

    if (!toast.empty() && nowMs() < toastUntil) {
        fillRect(W / 2 - 300, H - 120, 600, 52, 0, 0, 0, 200);
        drawText(g_fontS, toast, W / 2, H - 106, kCyan, Center);
    }
}

static void galleryLoop() {
    int sel[2] = { 0, 0 }, scrollRow[2] = { 0, 0 };
    std::string toast;
    u64 toastUntil = 0;
    scanDir(TabVideo);
    scanDir(TabPhoto);

    while (appletMainLoop() && !g_quit) {
        g_frameNo++;
        Input in = readInput();
        int &s = sel[g_tab];
        int &sr = scrollRow[g_tab];
        std::vector<Entry> &list = cur();
        const int n = int(list.size());

        if (in.down & (HidNpadButton_Plus | HidNpadButton_B)) break;
        if (in.down & HidNpadButton_Y) { scanDir(g_tab); s = 0; sr = 0; continue; }
        if (in.down & (HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR)) {
            g_tab = g_tab == TabVideo ? TabPhoto : TabVideo;
            continue;
        }
        if (in.down & HidNpadButton_Minus) {
            g_sort = SortMode((g_sort + 1) % SortCount);
            sortList(g_lists[0]);
            sortList(g_lists[1]);
            sel[0] = sel[1] = 0;
            scrollRow[0] = scrollRow[1] = 0;
            toast = std::string("並び順: ") + kSortNames[g_sort];
            toastUntil = nowMs() + 1500;
            continue;
        }

        // ---- タッチ ----
        static int dragStartRow = 0;
        const Touch &t = in.t;
        if (t.down) dragStartRow = sr;
        if (t.tap) {
            // タブ
            bool handled = false;
            for (int tb = 0; tb < 2; tb++) {
                if (hit(t, 36 + tb * 180, 48, 160, 56)) {
                    g_tab = Tab(tb);
                    handled = true;
                }
            }
            if (handled) continue;
            // 並び順
            if (hit(t, W - 460, 0, 460, 48)) {
                g_sort = SortMode((g_sort + 1) % SortCount);
                sortList(g_lists[0]);
                sortList(g_lists[1]);
                sel[0] = sel[1] = 0;
                scrollRow[0] = scrollRow[1] = 0;
                toast = std::string("並び順: ") + kSortNames[g_sort];
                toastUntil = nowMs() + 1500;
                continue;
            }
            // サムネイル: タップで開く
            if (t.y >= kGridY && t.y < H - 48 && t.x >= kGridX && t.x < kGridX + kCols * kCellW) {
                const int col = (t.x - kGridX) / kCellW, row = (t.y - kGridY) / kCellH;
                const int i = (sr + row) * kCols + col;
                if (row < kVisibleRows && i < n) {
                    s = i;
                    in.down |= HidNpadButton_A;
                }
            }
        }
        // 上下にドラッグでスクロール
        if (t.held && t.moved && std::abs(t.dy()) > std::abs(t.dx()) && n > 0) {
            const int maxRow = std::max(0, (n - 1) / kCols - kVisibleRows + 1);
            const int newRow = std::clamp(dragStartRow - int(std::lround(double(t.dy()) / kCellH)), 0, maxRow);
            if (newRow != sr) {
                sr = newRow;
                // 選んでいる物を見えている範囲に入れる
                const int col = s % kCols;
                int row = s / kCols;
                row = std::clamp(row, sr, sr + kVisibleRows - 1);
                s = std::min(row * kCols + col, n - 1);
            }
        }

        if (n > 0) {
            if (in.repeat & HidNpadButton_AnyRight) s = std::min(s + 1, n - 1);
            if (in.repeat & HidNpadButton_AnyLeft)  s = std::max(s - 1, 0);
            if (in.repeat & HidNpadButton_AnyDown)  s = std::min(s + kCols, n - 1);
            if (in.repeat & HidNpadButton_AnyUp)    s = std::max(s - kCols, 0);

            if (in.down & HidNpadButton_A) {
                if (g_tab == TabVideo) playVideo(list[s]);
                else viewPhotos(s, toast, toastUntil);
                padUpdate(&g_pad);   // 再生画面で押したボタンを持ち越さない
            } else if (in.down & HidNpadButton_X) {
                if (confirmDelete(s, sr, g_tab == TabVideo ? "この動画を削除しますか？" : "この写真を削除しますか？", false, nullptr)) {
                    deleteEntry(s, toast);
                    toastUntil = nowMs() + 2500;
                }
                padUpdate(&g_pad);
            }
        }
        // 選んでいる物が画面に入るようにスクロール
        const int n2 = int(cur().size());
        if (s >= n2) s = std::max(0, n2 - 1);
        const int row = s / kCols;
        if (row < sr) sr = row;
        if (row >= sr + kVisibleRows) sr = row - kVisibleRows + 1;

        // サムネイルは1フレームに1つずつ作る(画面が固まらないように)。見えている物を優先
        std::vector<Entry> &l2 = cur();
        for (int i = sr * kCols; i < n2 && i < (sr + kVisibleRows + 1) * kCols; i++) {
            if (!l2[i].thumbTried) {
                drawList(s, sr, toast, toastUntil);
                SDL_RenderPresent(g_ren);
                makeThumb(l2[i], g_tab);
                break;
            }
        }

        drawList(s, sr, toast, toastUntil);
        SDL_RenderPresent(g_ren);
        trimTextCache();
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
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

    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    av_log_set_level(AV_LOG_QUIET);
    galleryLoop();

    freeList(g_lists[0]);
    freeList(g_lists[1]);
    IMG_Quit();
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
