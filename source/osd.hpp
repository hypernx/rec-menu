#pragma once
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <cmath>

extern "C" u64 __nx_vi_layer_id;   // libnx の viCreateLayer が使うレイヤーID

// 画面上の小さな表示(OSD)。Teslaのメニューとは別の「レイヤー」に描くので、
// メニューを閉じていても表示でき、ゲームの操作も邪魔しない。
// - 右上: 録画タイマー  「● REC 01:23」
// - 左下: 通知          「録画を開始しました」など(数秒で消える)
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

inline ViDisplay g_display{};
inline Layer g_timer, g_toast;
inline stbtt_fontinfo g_font;
inline bool g_ready = false;
inline Mutex g_lock;

// 通知の内容(どのスレッドからでも設定できる)
inline char g_toastText[128] = {};
inline u32 g_toastColor = 0xFFFFFFFF;
inline u64 g_toastUntilMs = 0;
inline bool g_toastDirty = false;
inline char g_timerText[32] = {};
inline bool g_timerOn = false;
inline bool g_timerDirty = false;

inline u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

// 画面(1920x1080基準)上の位置と、描画用の大きさ(px)を指定してレイヤーを作る
inline bool createLayer(Layer &L, int fbW, int fbH, float x, float y, float scale) {
    // libnx の viCreateLayer は __nx_vi_layer_id を使うので、一時的に差し替えて2枚目以降を作る
    u64 id = 0;
    if (R_FAILED(viCreateManagedLayer(&g_display, ViLayerFlags(0), 0, &id))) return false;
    const u64 saved = __nx_vi_layer_id;
    __nx_vi_layer_id = id;
    Result rc = viCreateLayer(&g_display, &L.layer);
    __nx_vi_layer_id = saved;
    if (R_FAILED(rc)) return false;

    viSetLayerScalingMode(&L.layer, ViScalingMode_FitToLayer);
    s32 z = 0;
    if (R_SUCCEEDED(viGetZOrderCountMax(&g_display, &z)) && z > 0) viSetLayerZ(&L.layer, z);
    // 画面に出す用のスタックだけ。Screenshot / Recording には載せない(撮影したものに写らない)
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Default);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Lcd);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Arbitrary);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_LastFrame);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_Null);
    tsl::hlp::viAddToLayerStack(&L.layer, ViLayerStack_ApplicationForDebug);
    viSetLayerSize(&L.layer, s32(fbW * scale), s32(fbH * scale));
    viSetLayerPosition(&L.layer, x, y);

    if (R_FAILED(nwindowCreateFromLayer(&L.win, &L.layer))) { viCloseLayer(&L.layer); return false; }
    if (R_FAILED(framebufferCreate(&L.fb, &L.win, fbW, fbH, PIXEL_FORMAT_RGBA_8888, 2))) {
        nwindowClose(&L.win); viCloseLayer(&L.layer); return false;
    }
    framebufferMakeLinear(&L.fb);
    L.w = fbW; L.h = fbH;
    L.ok = true;
    return true;
}

inline void destroyLayer(Layer &L) {
    if (!L.ok) return;
    framebufferClose(&L.fb);
    nwindowClose(&L.win);
    viDestroyManagedLayer(&L.layer);
    L.ok = false;
}

// ---- 描画ヘルパー(32bit RGBA、アルファは乗算済みで書く) ----
struct Canvas { u32 *px; u32 stride; int w, h; };

inline u32 premul(u32 rgba, u32 a8) {   // rgba = 0xAABBGGRR、a8 = 追加の不透明度(0-255)
    u32 r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF, a = (rgba >> 24) & 0xFF;
    a = a * a8 / 255;
    return (a << 24) | ((b * a / 255) << 16) | ((g * a / 255) << 8) | (r * a / 255);
}

inline void blend(u32 &dst, u32 src) {   // どちらも乗算済み
    const u32 sa = src >> 24;
    if (sa == 0) return;
    if (sa == 255) { dst = src; return; }
    const u32 ia = 255 - sa;
    u32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        u32 c = ((src >> sh) & 0xFF) + (((dst >> sh) & 0xFF) * ia) / 255;
        out |= (c > 255 ? 255 : c) << sh;
    }
    dst = out;
}

inline void fill(Canvas &c, int x, int y, int w, int h, u32 color, int radius = 0) {
    const u32 p = premul(color, 255);
    for (int yy = (y < 0 ? 0 : y); yy < y + h && yy < c.h; yy++)
        for (int xx = (x < 0 ? 0 : x); xx < x + w && xx < c.w; xx++) {
            if (radius > 0) {   // 角丸
                int dx = 0, dy = 0;
                if (xx < x + radius) dx = x + radius - xx; else if (xx >= x + w - radius) dx = xx - (x + w - radius - 1);
                if (yy < y + radius) dy = y + radius - yy; else if (yy >= y + h - radius) dy = yy - (y + h - radius - 1);
                if (dx * dx + dy * dy > radius * radius) continue;
            }
            blend(c.px[yy * c.stride + xx], p);
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
            blend(c.px[py * c.stride + px], premul(color, u32(cov * 255)));
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
                    blend(c.px[py * c.stride + px2], premul(color, a));
                }
        }
        x += int(adv * sc);
    }
}

// ---- 初期化(Teslaの画面の準備ができた後、メインスレッドから1回呼ぶ) ----
inline bool init() {
    if (g_ready) return true;
    mutexInit(&g_lock);
    PlFontData font{};
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) return false;
    const u8 *data = static_cast<const u8 *>(font.address);
    if (!stbtt_InitFont(&g_font, data, stbtt_GetFontOffsetForIndex(data, 0))) return false;
    if (R_FAILED(viOpenDefaultDisplay(&g_display))) return false;

    const float sc = 1.5f;   // 描画1px = 画面(1080p)1.5px
    // 右上のタイマー: 260x52
    createLayer(g_timer, 260, 52, 1920 - 260 * sc - 30, 30, sc);
    // 左下の通知: 760x60
    createLayer(g_toast, 760, 60, 30, 1080 - 60 * sc - 40, sc);
    g_ready = g_timer.ok || g_toast.ok;
    g_toastDirty = g_timerDirty = true;   // 最初に透明な画面を出しておく
    return g_ready;
}

inline void exit() {
    if (!g_ready) return;
    destroyLayer(g_timer);
    destroyLayer(g_toast);
    viCloseDisplay(&g_display);
    g_ready = false;
}

// ---- 表示内容の設定(どのスレッドからでも可) ----
constexpr u32 kWhite  = 0xFFFFFFFF;
constexpr u32 kGreen  = 0xFF90E0A0;
constexpr u32 kOrange = 0xFF40B0FF;
constexpr u32 kRed    = 0xFF4040FF;

inline void toast(const char *msg, u32 color = kWhite, u32 ms = 3000) {
    mutexLock(&g_lock);
    snprintf(g_toastText, sizeof(g_toastText), "%s", msg);
    g_toastColor = color;
    g_toastUntilMs = nowMs() + ms;
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

// 変化があったときだけ描き直す(監視スレッドから0.1秒ごとに呼ぶ)
inline void tick() {
    if (!g_ready) return;
    mutexLock(&g_lock);
    char toastText[128], timerText[32];
    const bool toastOn = g_toastUntilMs != 0 && nowMs() < g_toastUntilMs;
    const bool toastNeed = g_toastDirty || (g_toast.visible && !toastOn);
    const bool timerNeed = g_timerDirty;
    const bool timerOn = g_timerOn;
    std::memcpy(toastText, g_toastText, sizeof(toastText));
    std::memcpy(timerText, g_timerText, sizeof(timerText));
    const u32 toastColor = g_toastColor;
    g_toastDirty = false;
    g_timerDirty = false;
    mutexUnlock(&g_lock);

    if (toastNeed && g_toast.ok) {
        u32 stride = 0;
        u32 *px = static_cast<u32 *>(framebufferBegin(&g_toast.fb, &stride));
        Canvas c{ px, stride / 4, g_toast.w, g_toast.h };
        for (int y = 0; y < c.h; y++) std::memset(px + y * c.stride, 0, size_t(c.w) * 4);
        if (toastOn) {
            const float fs = 24;
            int tw = textWidth(toastText, fs);
            if (tw > c.w - 40) tw = c.w - 40;
            fill(c, 0, 4, tw + 40, c.h - 8, 0xD8181818, 12);
            fill(c, 0, 4, 6, c.h - 8, toastColor, 0);
            text(c, 22, c.h / 2 + 9, toastText, fs, kWhite);
        }
        framebufferEnd(&g_toast.fb);
        g_toast.visible = toastOn;
    }

    if (timerNeed && g_timer.ok) {
        u32 stride = 0;
        u32 *px = static_cast<u32 *>(framebufferBegin(&g_timer.fb, &stride));
        Canvas c{ px, stride / 4, g_timer.w, g_timer.h };
        for (int y = 0; y < c.h; y++) std::memset(px + y * c.stride, 0, size_t(c.w) * 4);
        if (timerOn) {
            const float fs = 26;
            const int tw = textWidth(timerText, fs);
            const int bw = tw + 58;
            const int bx = c.w - bw;
            fill(c, bx, 4, bw, c.h - 8, 0xB0000000, 12);
            circle(c, bx + 22, c.h / 2, 8, kRed);
            text(c, bx + 40, c.h / 2 + 9, timerText, fs, kWhite);
        }
        framebufferEnd(&g_timer.fb);
        g_timer.visible = timerOn;
    }
}

} // namespace osd
