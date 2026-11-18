#pragma once
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <cmath>
#include <algorithm>

extern "C" u64 __nx_vi_layer_id;   // libnx の viCreateLayer が使うレイヤーID

// 画面上の小さな表示(OSD)。Teslaのメニューとは別の「レイヤー」に描くので、
// メニューを閉じていても表示でき、ゲームの操作も邪魔しない。
// - 右上: 録画タイマー  「● REC 01:23」
// - 右下: 通知          「録画を開始しました」など(数秒で消える)
// どちらも録画やスクリーンショットには写らない(画面出力用のレイヤースタックにだけ載せる)。
namespace osd {

struct Layer {
    ViLayer layer{};
    NWindow win{};
    Framebuffer fb{};
    int w = 0, h = 0;
    bool ok = false;
    bool visible = false;   // 最後に描いたとき何か表示していたか
};

inline ViDisplay g_ownDisplay{};
inline ViDisplay *g_display = nullptr;   // Teslaが開いた画面を借りる(同じ画面は2回開けない)
inline bool g_displayOwned = false;
inline Layer g_timer, g_toast;
inline stbtt_fontinfo g_font;
inline bool g_ready = false;
inline char g_status[48] = "not started";   // メニューに出す診断用の状態
inline Mutex g_lock;

// 通知の内容(どのスレッドからでも設定できる)
inline char g_toastText[128] = {};
inline u64 g_toastUntilMs = 0;
inline u64 g_toastShownMs = 0;
inline float g_toastY = -1;   // 今のレイヤーの位置(画面1080p基準)
inline bool g_toastDirty = false;
inline char g_timerText[32] = {};
inline bool g_timerOn = false;
inline bool g_timerDirty = false;

inline u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

// 画面(1920x1080基準)上の位置と、描画用の大きさ(px)を指定してレイヤーを作る
inline Result createLayer(Layer &L, int fbW, int fbH, float x, float y, float scale) {
    // libnx の viCreateLayer は __nx_vi_layer_id を使うので、一時的に差し替えて2枚目以降を作る
    u64 id = 0;
    Result rc = viCreateManagedLayer(g_display, ViLayerFlags(0), 0, &id);
    if (R_FAILED(rc)) return rc;
    const u64 saved = __nx_vi_layer_id;
    __nx_vi_layer_id = id;
    rc = viCreateLayer(g_display, &L.layer);
    __nx_vi_layer_id = saved;
    if (R_FAILED(rc)) return rc;

    viSetLayerScalingMode(&L.layer, ViScalingMode_FitToLayer);
    s32 z = 0;
    if (R_SUCCEEDED(viGetZOrderCountMax(g_display, &z)) && z > 0) viSetLayerZ(&L.layer, z);
    // 画面に出す用のスタックだけ。Screenshot / Recording には載せない(撮影したものに写らない)
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Default);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Lcd);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Arbitrary);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_LastFrame);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Null);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_ApplicationForDebug);
    viSetLayerSize(&L.layer, s32(fbW * scale), s32(fbH * scale));
    viSetLayerPosition(&L.layer, x, y);

    if (R_FAILED(rc = nwindowCreateFromLayer(&L.win, &L.layer))) { viDestroyManagedLayer(&L.layer); return rc; }
    if (R_FAILED(rc = framebufferCreate(&L.fb, &L.win, fbW, fbH, PIXEL_FORMAT_RGBA_4444, 2))) {
        nwindowClose(&L.win); viDestroyManagedLayer(&L.layer); return rc;
    }
    if (R_FAILED(rc = framebufferMakeLinear(&L.fb))) {
        framebufferClose(&L.fb); nwindowClose(&L.win); viDestroyManagedLayer(&L.layer); return rc;
    }
    L.w = fbW; L.h = fbH;
    L.ok = true;
    return 0;
}

inline void destroyLayer(Layer &L) {
    if (!L.ok) return;
    framebufferClose(&L.fb);
    nwindowClose(&L.win);
    viDestroyManagedLayer(&L.layer);
    L.ok = false;
}

// ---- 描画ヘルパー ----
// メモリ節約のため 16bit (RGBA4444、Teslaと同じ形式) で描く。色の指定は 0xAABBGGRR。
struct Canvas { u16 *px; u32 stride; int w, h; };

inline void blendPx(u16 &d, u32 color, u32 cov) {   // cov = 0-255 の覆い具合
    const u32 sa = ((color >> 24) & 0xFF) * cov / 255;
    if (sa == 0) return;
    const u32 sr = color & 0xFF, sg = (color >> 8) & 0xFF, sb = (color >> 16) & 0xFF;
    u32 dr = (d & 0xF) * 17, dg = ((d >> 4) & 0xF) * 17, db = ((d >> 8) & 0xF) * 17, da = ((d >> 12) & 0xF) * 17;
    const u32 oa = sa + da * (255 - sa) / 255;
    if (oa == 0) { d = 0; return; }
    // アルファ付き同士の重ね合わせ(乗算していない色で計算)
    auto mix = [&](u32 s, u32 dd) { return (s * sa + dd * da * (255 - sa) / 255) / oa; };
    dr = mix(sr, dr); dg = mix(sg, dg); db = mix(sb, db);
    d = u16((dr >> 4) | ((dg >> 4) << 4) | ((db >> 4) << 8) | ((oa >> 4) << 12));
}

inline void fill(Canvas &c, int x, int y, int w, int h, u32 color, int radius = 0) {
    for (int yy = (y < 0 ? 0 : y); yy < y + h && yy < c.h; yy++)
        for (int xx = (x < 0 ? 0 : x); xx < x + w && xx < c.w; xx++) {
            if (radius > 0) {   // 角丸
                int dx = 0, dy = 0;
                if (xx < x + radius) dx = x + radius - xx; else if (xx >= x + w - radius) dx = xx - (x + w - radius - 1);
                if (yy < y + radius) dy = y + radius - yy; else if (yy >= y + h - radius) dy = yy - (y + h - radius - 1);
                if (dx * dx + dy * dy > radius * radius) continue;
            }
            blendPx(c.px[yy * c.stride + xx], color, 255);
        }
}

inline void circle(Canvas &c, int cx, int cy, int r, u32 color) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            const float d = std::sqrt(float(x * x + y * y));
            if (d > r + 0.5f) continue;
            const float cov = d < r - 0.5f ? 1.f : (r + 0.5f - d);
            const int px = cx + x, py = cy + y;
            if (px < 0 || py < 0 || px >= c.w || py >= c.h) continue;
            blendPx(c.px[py * c.stride + px], color, u32(cov * 255));
        }
}

inline u32 nextCodepoint(const char *&s) {
    const u8 *p = reinterpret_cast<const u8 *>(s);
    u32 cp = 0;
    int n = 0;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else { cp = p[0] & 0x07; n = 4; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { s += i; return '?'; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    s += n;
    return cp;
}

inline int textWidth(const char *s, float px) {
    const float sc = stbtt_ScaleForPixelHeight(&g_font, px);
    int w = 0;
    while (*s) {
        u32 cp = nextCodepoint(s);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, int(cp), &adv, &lsb);
        w += int(adv * sc);
    }
    return w;
}

// baseline の高さに文字を書く
inline void text(Canvas &c, int x, int baseline, const char *s, float px, u32 color) {
    const float sc = stbtt_ScaleForPixelHeight(&g_font, px);
    static u8 glyph[96 * 96];
    while (*s) {
        u32 cp = nextCodepoint(s);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_font, int(cp), &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&g_font, int(cp), sc, sc, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 96 && gh <= 96) {
            stbtt_MakeCodepointBitmap(&g_font, glyph, gw, gh, gw, sc, sc, int(cp));
            for (int yy = 0; yy < gh; yy++)
                for (int xx = 0; xx < gw; xx++) {
                    const u8 a = glyph[yy * gw + xx];
                    if (!a) continue;
                    const int px2 = x + x0 + xx, py = baseline + y0 + yy;
                    if (px2 < 0 || py < 0 || px2 >= c.w || py >= c.h) continue;
                    blendPx(c.px[py * c.stride + px2], color, a);
                }
        }
        x += int(adv * sc);
    }
}

// ---- 通知の絵 ----
// 色は 0xAABBGGRR。rgb() で普通の RGB から作れる
constexpr u32 rgb(u32 r, u32 g, u32 b, u32 a = 255) { return (a << 24) | (b << 16) | (g << 8) | r; }
constexpr u32 kWhite  = rgb(255, 255, 255);
constexpr u32 kRed    = rgb(255, 72, 72);

enum class Icon { Info, Rec, Stop, Warn, Error, Shot };

inline u32 iconColor(Icon i) {
    switch (i) {
        case Icon::Rec:   return rgb(255, 72, 72);    // 赤
        case Icon::Stop:  return rgb(64, 200, 120);   // 緑
        case Icon::Warn:  return rgb(255, 170, 40);   // オレンジ
        case Icon::Error: return rgb(235, 60, 90);    // 濃いピンク
        case Icon::Shot:  return rgb(70, 160, 255);   // 青
        default:          return rgb(140, 140, 160);
    }
}

// 丸いバッジ(アイコン)と文字の、角の丸い吹き出し。右端に寄せて描く(画面の右下に出すため)
inline void drawToast(Canvas &c, const char *msg, Icon icon) {
    const int h = 52, top = (c.h - h) / 2 - 2, r = h / 2;
    float fs = 22;
    const int textX = 64, padR = 24;
    int tw = textWidth(msg, fs);
    const int maxTw = c.w - textX - padR - 4;
    if (tw > maxTw) {   // 長い文は文字を小さくして収める
        fs = std::max(15.f, fs * float(maxTw) / float(tw));
        tw = std::min(textWidth(msg, fs), maxTw);
    }
    const int w = textX + tw + padR;
    const int x0 = c.w - w - 4;                                   // 右寄せ(右に影の分だけ余白)
    fill(c, x0 + 2, top + 4, w, h, rgb(0, 0, 0, 90), r);         // 影
    fill(c, x0, top, w, h, rgb(36, 38, 48, 242), r);              // 本体

    // バッジ
    const int bx = x0 + 30, by = top + h / 2;
    circle(c, bx, by, 19, iconColor(icon));
    switch (icon) {
        case Icon::Rec:   circle(c, bx, by, 7, kWhite); break;
        case Icon::Stop:  fill(c, bx - 6, by - 6, 12, 12, kWhite, 2); break;
        case Icon::Warn:  fill(c, bx - 2, by - 10, 4, 13, kWhite, 1); circle(c, bx, by + 8, 2, kWhite); break;
        case Icon::Error:
            for (int k = -7; k <= 7; k++) { circle(c, bx + k, by + k, 2, kWhite); circle(c, bx + k, by - k, 2, kWhite); }
            break;
        case Icon::Shot:   // カメラ
            fill(c, bx - 10, by - 6, 20, 14, kWhite, 3);
            fill(c, bx - 4, by - 9, 8, 4, kWhite, 1);
            circle(c, bx, by + 1, 5, iconColor(icon));
            circle(c, bx, by + 1, 2, kWhite);
            break;
        default: circle(c, bx, by, 4, kWhite); break;
    }
    text(c, x0 + textX, by + int(fs * 0.36f), msg, fs, kWhite);
}

inline Icon g_toastIcon = Icon::Info;

// 通知の位置: 画面の右下。下から飛び出して、少し行き過ぎて戻る(ポップな動き)
constexpr float kToastScale = 1.5f;
constexpr int kToastW = 600, kToastH = 64;
constexpr float kToastX = 1920 - kToastW * kToastScale - 30;   // 右下
constexpr float kToastShowY = 1080 - kToastH * kToastScale - 36;   // 表示中の位置
constexpr float kToastHideY = 1080 + 8;                            // 画面の下の外
constexpr u64 kPopInMs = 320, kPopOutMs = 220;

inline float easeOutBack(float t) {   // 少し行き過ぎて戻る
    const float c1 = 1.9f, c3 = c1 + 1;
    const float u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}
inline float easeInCubic(float t) { return t * t * t; }

// ---- 初期化(Teslaの画面の準備ができた後、メインスレッドから1回呼ぶ) ----
inline void setStatus(const char *step, Result rc) {
    snprintf(g_status, sizeof(g_status), "%s err %04u-%04u", step, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
}

inline bool init() {
    if (g_ready) return true;
    mutexInit(&g_lock);
    PlFontData font{};
    Result rc = plGetSharedFontByType(&font, PlSharedFontType_Standard);
    if (R_FAILED(rc)) { setStatus("font", rc); return false; }
    const u8 *data = static_cast<const u8 *>(font.address);
    if (!stbtt_InitFont(&g_font, data, stbtt_GetFontOffsetForIndex(data, 0))) { snprintf(g_status, sizeof(g_status), "font init failed"); return false; }
    if (tsl::impl::sharedDisplay) {
        g_display = tsl::impl::sharedDisplay;
    } else {
        if (R_FAILED(rc = viOpenDefaultDisplay(&g_ownDisplay))) { setStatus("display", rc); return false; }
        g_display = &g_ownDisplay;
        g_displayOwned = true;
    }

    const float sc = 1.5f;   // 描画1px = 画面(1080p)1.5px
    // 右上のタイマー: 200x44
    if (R_FAILED(rc = createLayer(g_timer, 200, 44, 1920 - 200 * sc - 30, 30, sc))) { setStatus("timer", rc); }
    // 右下の通知: 600x64(初めは画面の下の外に置いておく)
    Result rc2 = createLayer(g_toast, kToastW, kToastH, kToastX, kToastHideY, kToastScale);
    g_toastY = kToastHideY;
    if (R_FAILED(rc2)) setStatus("toast", rc2);
    g_ready = g_timer.ok || g_toast.ok;
    if (g_timer.ok && g_toast.ok) snprintf(g_status, sizeof(g_status), "OK");
    g_toastDirty = g_timerDirty = true;   // 最初に透明な画面を出しておく
    return g_ready;
}

inline void exit() {
    if (!g_ready) return;
    destroyLayer(g_timer);
    destroyLayer(g_toast);
    if (g_displayOwned) viCloseDisplay(&g_ownDisplay);
    g_displayOwned = false;
    g_display = nullptr;
    g_ready = false;
}

// ---- 表示内容の設定(どのスレッドからでも可) ----
inline void toast(const char *msg, Icon icon = Icon::Info, u32 ms = 3000) {
    mutexLock(&g_lock);
    snprintf(g_toastText, sizeof(g_toastText), "%s", msg);
    g_toastIcon = icon;
    g_toastShownMs = nowMs();
    g_toastUntilMs = g_toastShownMs + ms;
    g_toastDirty = true;
    mutexUnlock(&g_lock);
}

inline void setTimer(bool on, const char *txt) {
    mutexLock(&g_lock);
    if (on != g_timerOn || (on && std::strcmp(txt, g_timerText) != 0)) {
        g_timerOn = on;
        snprintf(g_timerText, sizeof(g_timerText), "%s", txt);
        g_timerDirty = true;
    }
    mutexUnlock(&g_lock);
}

// 変化があったときだけ描き直す(監視スレッドから呼ぶ)。
// 通知が動いている間は true を返すので、呼ぶ側は間隔を短く(約16ms)する
inline bool tick() {
    if (!g_ready) return false;
    mutexLock(&g_lock);
    char toastText[128], timerText[32];
    const u64 now = nowMs();
    const u64 shownAt = g_toastShownMs, until = g_toastUntilMs;
    const bool toastNeed = g_toastDirty;
    const Icon icon = g_toastIcon;
    const bool timerNeed = g_timerDirty;
    const bool timerOn = g_timerOn;
    std::memcpy(toastText, g_toastText, sizeof(toastText));
    std::memcpy(timerText, g_timerText, sizeof(timerText));
    g_toastDirty = false;
    g_timerDirty = false;
    mutexUnlock(&g_lock);

    bool animating = false;
    if (g_toast.ok) {
        // 新しい通知が来たら1回だけ描く(動かすのはレイヤーの位置だけなので軽い)
        if (toastNeed) {
            u32 stride = 0;
            u16 *px = static_cast<u16 *>(framebufferBegin(&g_toast.fb, &stride));
            Canvas c{ px, stride / 2, g_toast.w, g_toast.h };
            for (int y = 0; y < c.h; y++) std::memset(px + y * c.stride, 0, size_t(c.w) * 2);
            drawToast(c, toastText, icon);
            framebufferEnd(&g_toast.fb);
        }
        // 位置を計算: 飛び出す → 表示 → 下に引っ込む
        float y = kToastHideY;
        if (until != 0 && now < until) {
            const u64 t = now - shownAt;
            if (t < kPopInMs) {
                y = kToastHideY + (kToastShowY - kToastHideY) * easeOutBack(float(t) / kPopInMs);
                animating = true;
            } else {
                y = kToastShowY;
            }
        } else if (until != 0 && now < until + kPopOutMs) {
            y = kToastShowY + (kToastHideY - kToastShowY) * easeInCubic(float(now - until) / kPopOutMs);
            animating = true;
        }
        if (y != g_toastY) {
            viSetLayerPosition(&g_toast.layer, kToastX, y);
            g_toastY = y;
        }
        g_toast.visible = y < kToastHideY;
    }

    if (timerNeed && g_timer.ok) {
        u32 stride = 0;
        u16 *px = static_cast<u16 *>(framebufferBegin(&g_timer.fb, &stride));
        Canvas c{ px, stride / 2, g_timer.w, g_timer.h };
        for (int y = 0; y < c.h; y++) std::memset(px + y * c.stride, 0, size_t(c.w) * 2);
        if (timerOn) {
            const float fs = 24;
            const int tw = textWidth(timerText, fs);
            const int bw = tw + 52;
            const int bx = c.w - bw;
            fill(c, bx, 4, bw, c.h - 8, 0xB0000000, 12);
            circle(c, bx + 20, c.h / 2, 7, kRed);
            text(c, bx + 36, c.h / 2 + 8, timerText, fs, kWhite);
        }
        framebufferEnd(&g_timer.fb);
        g_timer.visible = timerOn;
    }
    return animating;
}

} // namespace osd
