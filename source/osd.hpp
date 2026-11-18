#pragma once
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

extern "C" u64 __nx_vi_layer_id;   // Teslaのレイヤー(画面)のID

// 画面上の小さな表示(OSD)
// - 右上: 録画タイマー  「● REC 01:23」
// - 右下: 通知          「録画を開始しました」など(下から飛び出して、数秒で引っ込む)
//
// メニューを閉じている間も Tesla 自身の画面に描く(libtesla に当てたパッチのフックを使う)。
// その間は Tesla の画面を画面の右端に寄せ、入力はゲームに渡したままにする。
// Tesla の画面は録画とスクリーンショットに載せていないので、撮ったものには写らない。
namespace osd {

enum class Icon { Info, Rec, Stop, Warn, Error, Shot };

inline Mutex g_lock;
inline bool g_ready = false;

// 通知
inline char g_toastText[160] = {};
inline Icon g_toastIcon = Icon::Info;
inline u64 g_toastRequestMs = 0;    // 通知が来た時刻
inline u64 g_toastDurMs = 0;        // 表示しておく長さ
inline u64 g_toastShownMs = 0;      // 実際に画面に出始めた時刻(0 = まだ)
inline bool g_toastNew = false;     // 新しい通知が来た(行の分け方を計算し直す)
// タイマー
inline char g_timerText[32] = {};
inline bool g_timerOn = false;
inline bool g_dirty = true;         // 描き直しが必要
inline bool g_layerRight = false;   // Teslaの画面を右端に寄せているか

constexpr u64 kPopInMs = 320, kPopOutMs = 220;
constexpr u64 kStaleMs = 4000;      // メニューを開いている間に来て、これより古くなった通知は出さない

inline u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

inline void setLayerX(float x) {
    ViLayer layer{};
    layer.layer_id = __nx_vi_layer_id;
    layer.initialized = true;
    viSetLayerPosition(&layer, x, 0);
}

// ---- どのスレッドからでも呼べる ----
inline void toast(const char *msg, Icon icon = Icon::Info, u32 ms = 3000) {
    mutexLock(&g_lock);
    snprintf(g_toastText, sizeof(g_toastText), "%s", msg);
    g_toastIcon = icon;
    g_toastRequestMs = nowMs();
    g_toastDurMs = ms;
    g_toastShownMs = 0;
    g_toastNew = true;
    g_dirty = true;
    mutexUnlock(&g_lock);
}

inline void setTimer(bool on, const char *txt) {
    mutexLock(&g_lock);
    if (on != g_timerOn || (on && std::strcmp(txt, g_timerText) != 0)) {
        g_timerOn = on;
        snprintf(g_timerText, sizeof(g_timerText), "%s", txt);
        g_dirty = true;
    }
    mutexUnlock(&g_lock);
}

// 通知がまだ表示中(これから出す分も含む)か。g_lock を持って呼ぶ
inline bool toastActiveLocked(u64 now) {
    if (g_toastRequestMs == 0) return false;
    if (g_toastShownMs == 0) return now - g_toastRequestMs < kStaleMs;
    return now < g_toastShownMs + g_toastDurMs + kPopOutMs;
}

// ---- 描画(メインスレッド、Teslaのループから) ----
using tsl::Color;
inline Color col(u8 r, u8 g, u8 b) { return Color(r >> 4, g >> 4, b >> 4, 0xF); }
inline Color iconColor(Icon i) {
    switch (i) {
        case Icon::Rec:   return col(255, 72, 72);    // 赤
        case Icon::Stop:  return col(64, 200, 120);   // 緑
        case Icon::Warn:  return col(255, 170, 40);   // オレンジ
        case Icon::Error: return col(235, 60, 90);    // 濃いピンク
        case Icon::Shot:  return col(70, 160, 255);   // 青
        default:          return col(140, 140, 160);
    }
}
inline const Color kWhite = col(255, 255, 255);
inline const Color kBg = col(36, 38, 48);

inline int textW(tsl::gfx::Renderer *r, const std::string &s, float fs) {
    return int(r->drawString(s.c_str(), false, 0, 0, fs, Color(0, 0, 0, 0)).first);   // 透明 = 測るだけ
}

// 角が丸い(両端が半円の)帯。不透明な色で描く(重なりのムラが出ないように)
inline void pill(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h, Color c) {
    const s32 rad = h / 2;
    r->drawRect(x + rad, y, w - rad * 2, h, c);
    r->drawCircle(x + rad, y + rad, rad, true, c);
    r->drawCircle(x + w - rad - 1, y + rad, rad, true, c);
}

inline void drawIcon(tsl::gfx::Renderer *r, s32 cx, s32 cy, Icon icon) {
    r->drawCircle(cx, cy, 15, true, iconColor(icon));
    switch (icon) {
        case Icon::Rec:  r->drawCircle(cx, cy, 6, true, kWhite); break;
        case Icon::Stop: r->drawRect(cx - 5, cy - 5, 11, 11, kWhite); break;
        case Icon::Warn: r->drawRect(cx - 1, cy - 8, 3, 10, kWhite); r->drawRect(cx - 1, cy + 5, 3, 3, kWhite); break;
        case Icon::Error:
            for (s32 k = -5; k <= 5; k++) { r->drawRect(cx + k - 1, cy + k - 1, 3, 3, kWhite); r->drawRect(cx + k - 1, cy - k - 1, 3, 3, kWhite); }
            break;
        case Icon::Shot:   // カメラ
            r->drawRect(cx - 8, cy - 5, 17, 12, kWhite);
            r->drawRect(cx - 3, cy - 8, 7, 3, kWhite);
            r->drawCircle(cx, cy + 1, 4, true, iconColor(icon));
            r->drawCircle(cx, cy + 1, 1, true, kWhite);
            break;
        default: r->drawCircle(cx, cy, 3, true, kWhite); break;
    }
}

// UTF-8 の1文字の長さ
inline size_t u8len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

// 幅に収まるように行を分ける(英語は空白で、日本語はどこでも分ける)。最大2行
inline std::vector<std::string> wrap(tsl::gfx::Renderer *r, const std::string &s, float fs, int maxW) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < s.size() && lines.size() < 2) {
        if (lines.size() == 1 || textW(r, s.substr(start), fs) <= maxW) {
            lines.push_back(s.substr(start));
            break;
        }
        size_t i = start, lastFit = start, lastSpace = std::string::npos;
        while (i < s.size()) {
            const size_t n = u8len(static_cast<unsigned char>(s[i]));
            if (textW(r, s.substr(start, i + n - start), fs) > maxW) break;
            i += n;
            lastFit = i;
            if (s[i - 1] == ' ') lastSpace = i;
        }
        size_t cut = (lastSpace != std::string::npos && lastSpace > start) ? lastSpace : lastFit;
        if (cut == start) cut = start + u8len(static_cast<unsigned char>(s[start]));
        std::string line = s.substr(start, cut - start);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        lines.push_back(line);
        start = cut;
        while (start < s.size() && s[start] == ' ') start++;
    }
    return lines;
}

inline float easeOutBack(float t) {   // 少し行き過ぎて戻る
    const float c1 = 1.9f, c3 = c1 + 1, u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}
inline float easeInCubic(float t) { return t * t * t; }

inline std::vector<std::string> g_lines;   // 通知の文を行に分けたもの(メインスレッドだけが使う)
constexpr float kToastFs = 17;

// フック: 何か表示するものがあるか
inline bool hiddenVisible() {
    mutexLock(&g_lock);
    const bool v = g_timerOn || toastActiveLocked(nowMs());
    mutexUnlock(&g_lock);
    return v;
}

// フック: 描き直しが必要か
inline bool hiddenNeedsDraw() {
    mutexLock(&g_lock);
    const u64 now = nowMs();
    bool need = g_dirty;
    if (toastActiveLocked(now)) {
        if (g_toastShownMs == 0) need = true;                                  // これから出す
        else if (now - g_toastShownMs < kPopInMs) need = true;                 // 飛び出し中
        else if (now >= g_toastShownMs + g_toastDurMs) need = true;            // 引っ込み中
    } else if (g_toastShownMs != 0) {
        need = true;   // 引っ込み終わった → 消す
    }
    mutexUnlock(&g_lock);
    return need;
}

// フック: 描く(Teslaのフレームの中で呼ばれる。画面は透明に消してある)
inline void hiddenDraw(tsl::gfx::Renderer *r) {
    if (!g_layerRight) {   // Teslaの画面を右端に寄せる(メニューを開くと元に戻す)
        setLayerX(float(tsl::cfg::ScreenWidth - tsl::cfg::LayerWidth));
        g_layerRight = true;
    }
    const s32 W = tsl::cfg::FramebufferWidth, H = tsl::cfg::FramebufferHeight;

    mutexLock(&g_lock);
    const u64 now = nowMs();
    const bool timerOn = g_timerOn;
    std::string timer = g_timerText;
    std::string text = g_toastText;
    const Icon icon = g_toastIcon;
    const bool isNew = g_toastNew;
    bool toastOn = toastActiveLocked(now);
    if (toastOn && g_toastShownMs == 0) g_toastShownMs = now;   // ここから飛び出す
    if (!toastOn) g_toastShownMs = 0;
    const u64 shownAt = g_toastShownMs, dur = g_toastDurMs;
    g_toastNew = false;
    g_dirty = false;
    mutexUnlock(&g_lock);

    // ---- 右上: タイマー ----
    if (timerOn) {
        const float fs = 20;
        const s32 tw = textW(r, timer, fs);
        const s32 h = 34, w = tw + 46, x = W - w - 10, y = 10;
        pill(r, x, y, w, h, col(16, 16, 20));
        r->drawCircle(x + 19, y + h / 2, 6, true, col(255, 64, 64));
        r->drawString(timer.c_str(), false, x + 32, y + h / 2 + 7, fs, kWhite);
    }

    // ---- 右下: 通知(下から飛び出して、少し行き過ぎて戻る) ----
    if (toastOn) {
        const s32 textX = 48, padR = 16, maxW = W - 12 - textX - padR;
        if (isNew || g_lines.empty()) g_lines = wrap(r, text, kToastFs, maxW);
        s32 tw = 0;
        for (auto &l : g_lines) tw = std::max<s32>(tw, textW(r, l, kToastFs));
        const s32 lineH = 23;
        const s32 h = std::max<s32>(44, 21 + lineH * s32(g_lines.size()));
        const s32 w = textX + tw + padR;
        const s32 x = W - w - 8;
        const float showY = float(H - h - 14), hideY = float(H + 4);
        float y = showY;
        const u64 t = now - shownAt;
        if (t < kPopInMs) y = hideY + (showY - hideY) * easeOutBack(float(t) / kPopInMs);
        else if (t >= dur) y = showY + (hideY - showY) * easeInCubic(std::min(1.f, float(t - dur) / kPopOutMs));
        const s32 yi = s32(y);

        // 角の丸い吹き出し(1行なら両端が半円、2行なら角丸の四角)
        if (g_lines.size() <= 1) {
            pill(r, x, yi, w, h, kBg);
        } else {
            const s32 rad = 14;
            r->drawRect(x + rad, yi, w - rad * 2, h, kBg);
            r->drawRect(x, yi + rad, w, h - rad * 2, kBg);
            r->drawCircle(x + rad, yi + rad, rad, true, kBg);
            r->drawCircle(x + w - rad - 1, yi + rad, rad, true, kBg);
            r->drawCircle(x + rad, yi + h - rad - 1, rad, true, kBg);
            r->drawCircle(x + w - rad - 1, yi + h - rad - 1, rad, true, kBg);
        }
        drawIcon(r, x + 24, yi + h / 2, icon);
        const s32 textTop = yi + (h - lineH * s32(g_lines.size())) / 2;
        for (size_t i = 0; i < g_lines.size(); i++)
            r->drawString(g_lines[i].c_str(), false, x + textX, textTop + s32(i) * lineH + 17, kToastFs, kWhite);
    }
}

// メニューを開くとき(onShow)に呼ぶ: Teslaの画面を元の位置(左端)に戻す
inline void onMenuShow() {
    if (g_layerRight) {
        setLayerX(0);
        g_layerRight = false;
    }
    mutexLock(&g_lock);
    g_dirty = true;
    mutexUnlock(&g_lock);
}

inline bool init() {
    if (g_ready) return true;
    mutexInit(&g_lock);
    tsl::impl::hiddenVisible = hiddenVisible;
    tsl::impl::hiddenNeedsDraw = hiddenNeedsDraw;
    tsl::impl::hiddenDraw = hiddenDraw;
    g_ready = true;
    return true;
}

inline void exit() {
    tsl::impl::hiddenVisible = nullptr;
    tsl::impl::hiddenNeedsDraw = nullptr;
    tsl::impl::hiddenDraw = nullptr;
    g_ready = false;
}

} // namespace osd
