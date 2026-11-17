#pragma once
#include <switch.h>
#include <cstring>

// grc:d の H.264 を、その場で MP4 (fragmented MP4) にして書き出す最小限のライター。
// - 最初に ftyp + moov(映像トラック1本)を書き、以降は約1秒ごとに moof + mdat を追記する。
// - 途中で電源が落ちても、最後に書けたところまでは再生できる。
// - 映像は 1280x720 / H.264 High。時刻は grc:d のタイムスタンプ(マイクロ秒)を使う。
namespace mp4 {

constexpr u32 kTimescale   = 1000000;   // マイクロ秒
constexpr u32 kDefaultDur  = 33333;     // 30fps
constexpr u32 kMaxSamples  = 64;        // 1フラグメントあたりの最大フレーム数(約2秒)

// 書き込み関数(戻り値 false で失敗)
using WriteFn = bool (*)(const void *data, size_t size);

struct Sample { u32 size; u32 duration; bool sync; };

struct Writer {
    WriteFn write = nullptr;
    u8 *buf = nullptr;          // フレームデータをためるバッファ(mdatの中身)
    size_t cap = 0;
    size_t len = 0;
    Sample samples[kMaxSamples];
    u32 count = 0;
    u32 seq = 0;
    u64 firstTs = 0;            // 最初のフレームの時刻
    u64 fragStart = 0;          // このフラグメントの開始時刻(firstTs基準)
    u64 lastTs = 0;
    u64 elapsed = 0;            // 書いたフレームの合計時間
    bool started = false;       // 最初のIDRを受け取ったか
};

// ---- ビッグエンディアンで書く小さなヘルパー ----
struct Box {
    u8 *p; size_t n = 0;
    explicit Box(u8 *out) : p(out) {}
    void u8_(u32 v) { p[n++] = u8(v); }
    void u16_(u32 v) { u8_(v >> 8); u8_(v); }
    void u32_(u32 v) { u16_(v >> 16); u16_(v); }
    void u64_(u64 v) { u32_(u32(v >> 32)); u32_(u32(v)); }
    void bytes(const void *d, size_t s) { std::memcpy(p + n, d, s); n += s; }
    void zeros(size_t s) { std::memset(p + n, 0, s); n += s; }
    size_t open(const char *type) { size_t at = n; u32_(0); bytes(type, 4); return at; }
    size_t openFull(const char *type, u8 ver, u32 flags) { size_t at = open(type); u32_((u32(ver) << 24) | flags); return at; }
    void close(size_t at) {
        u32 s = u32(n - at);
        p[at] = u8(s >> 24); p[at + 1] = u8(s >> 16); p[at + 2] = u8(s >> 8); p[at + 3] = u8(s);
    }
};

inline void matrix(Box &b) {
    const u32 m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
    for (u32 v : m) b.u32_(v);
}

// ftyp + moov を書く。sps/pps はスタートコード無しのNAL本体
inline bool begin(Writer &w, WriteFn fn, u8 *buf, size_t cap,
                  const u8 *sps, size_t spsLen, const u8 *pps, size_t ppsLen,
                  u16 width, u16 height) {
    w = Writer{};
    w.write = fn; w.buf = buf; w.cap = cap;

    static u8 hdr[1024];
    Box b(hdr);

    size_t ftyp = b.open("ftyp");
    b.bytes("isom", 4); b.u32_(0x200);
    b.bytes("isom", 4); b.bytes("iso6", 4); b.bytes("avc1", 4); b.bytes("mp41", 4);
    b.close(ftyp);

    size_t moov = b.open("moov");
    {
        size_t mvhd = b.openFull("mvhd", 0, 0);
        b.u32_(0); b.u32_(0);           // 作成/変更時刻
        b.u32_(1000); b.u32_(0);        // timescale, duration(フラグメントなので0)
        b.u32_(0x00010000); b.u16_(0x0100); b.zeros(10);
        matrix(b); b.zeros(24);
        b.u32_(2);                      // next_track_ID
        b.close(mvhd);

        size_t trak = b.open("trak");
        {
            size_t tkhd = b.openFull("tkhd", 0, 0x3);
            b.u32_(0); b.u32_(0); b.u32_(1); b.u32_(0); b.u32_(0);
            b.zeros(8); b.u16_(0); b.u16_(0); b.u16_(0); b.u16_(0);
            matrix(b);
            b.u32_(u32(width) << 16); b.u32_(u32(height) << 16);
            b.close(tkhd);

            size_t mdia = b.open("mdia");
            {
                size_t mdhd = b.openFull("mdhd", 0, 0);
                b.u32_(0); b.u32_(0); b.u32_(kTimescale); b.u32_(0);
                b.u16_(0x55C4); b.u16_(0);     // 言語 "und"
                b.close(mdhd);

                size_t hdlr = b.openFull("hdlr", 0, 0);
                b.u32_(0); b.bytes("vide", 4); b.zeros(12); b.bytes("RecMenu\0", 8);
                b.close(hdlr);

                size_t minf = b.open("minf");
                {
                    size_t vmhd = b.openFull("vmhd", 0, 1); b.zeros(8); b.close(vmhd);
                    size_t dinf = b.open("dinf");
                    size_t dref = b.openFull("dref", 0, 0); b.u32_(1);
                    size_t url = b.openFull("url ", 0, 1); b.close(url);
                    b.close(dref); b.close(dinf);

                    size_t stbl = b.open("stbl");
                    {
                        size_t stsd = b.openFull("stsd", 0, 0); b.u32_(1);
                        size_t avc1 = b.open("avc1");
                        b.zeros(6); b.u16_(1);              // data_reference_index
                        b.zeros(16);
                        b.u16_(width); b.u16_(height);
                        b.u32_(0x00480000); b.u32_(0x00480000); b.u32_(0);
                        b.u16_(1); b.zeros(32); b.u16_(0x18); b.u16_(0xFFFF);

                        size_t avcC = b.open("avcC");
                        b.u8_(1); b.u8_(sps[1]); b.u8_(sps[2]); b.u8_(sps[3]);
                        b.u8_(0xFF);                        // NAL長は4バイト
                        b.u8_(0xE1); b.u16_(u32(spsLen)); b.bytes(sps, spsLen);
                        b.u8_(1);    b.u16_(u32(ppsLen)); b.bytes(pps, ppsLen);
                        if (sps[1] == 100 || sps[1] == 110 || sps[1] == 122 || sps[1] == 144) {
                            b.u8_(0xFC | 1); b.u8_(0xF8); b.u8_(0xF8); b.u8_(0);  // 4:2:0 8bit
                        }
                        b.close(avcC);
                        b.close(avc1);
                        b.close(stsd);

                        size_t stts = b.openFull("stts", 0, 0); b.u32_(0); b.close(stts);
                        size_t stsc = b.openFull("stsc", 0, 0); b.u32_(0); b.close(stsc);
                        size_t stsz = b.openFull("stsz", 0, 0); b.u32_(0); b.u32_(0); b.close(stsz);
                        size_t stco = b.openFull("stco", 0, 0); b.u32_(0); b.close(stco);
                    }
                    b.close(stbl);
                }
                b.close(minf);
            }
            b.close(mdia);
        }
        b.close(trak);

        size_t mvex = b.open("mvex");
        size_t trex = b.openFull("trex", 0, 0);
        b.u32_(1); b.u32_(1); b.u32_(0); b.u32_(0); b.u32_(0);
        b.close(trex);
        b.close(mvex);
    }
    b.close(moov);

    return w.write(hdr, b.n);
}

// たまっているフレームを moof + mdat として書く
inline bool flush(Writer &w) {
    if (w.count == 0) return true;

    static u8 moofBuf[256 + kMaxSamples * 12];
    Box b(moofBuf);
    size_t moof = b.open("moof");
    size_t mfhd = b.openFull("mfhd", 0, 0); b.u32_(++w.seq); b.close(mfhd);
    size_t traf = b.open("traf");
    size_t tfhd = b.openFull("tfhd", 0, 0x020000); b.u32_(1); b.close(tfhd);   // default-base-is-moof
    size_t tfdt = b.openFull("tfdt", 1, 0); b.u64_(w.fragStart); b.close(tfdt);
    size_t trun = b.openFull("trun", 0, 0x000701);   // data_offset + duration + size + flags
    b.u32_(w.count);
    size_t dataOffsetAt = b.n; b.u32_(0);
    for (u32 i = 0; i < w.count; i++) {
        b.u32_(w.samples[i].duration);
        b.u32_(w.samples[i].size);
        b.u32_(w.samples[i].sync ? 0x02000000 : 0x01010000);
    }
    b.close(trun);
    b.close(traf);
    b.close(moof);

    // data_offset = moofの大きさ + mdatのヘッダ(8バイト)
    const u32 off = u32(b.n + 8);
    moofBuf[dataOffsetAt] = u8(off >> 24); moofBuf[dataOffsetAt + 1] = u8(off >> 16);
    moofBuf[dataOffsetAt + 2] = u8(off >> 8); moofBuf[dataOffsetAt + 3] = u8(off);

    u8 mdatHdr[8];
    const u32 mdatSize = u32(w.len + 8);
    mdatHdr[0] = u8(mdatSize >> 24); mdatHdr[1] = u8(mdatSize >> 16);
    mdatHdr[2] = u8(mdatSize >> 8);  mdatHdr[3] = u8(mdatSize);
    std::memcpy(mdatHdr + 4, "mdat", 4);

    if (!w.write(moofBuf, b.n) || !w.write(mdatHdr, 8) || !w.write(w.buf, w.len)) return false;

    for (u32 i = 0; i < w.count; i++) w.fragStart += w.samples[i].duration;
    w.count = 0;
    w.len = 0;
    return true;
}

// 1パケット(Annex-B形式、通常NAL 1個)を1フレームとして追加する
inline bool addFrame(Writer &w, const u8 *data, size_t size, u64 ts) {
    // NALの種類を調べる(先頭のスタートコードの後)
    size_t sc = (size >= 4 && data[2] == 1) ? 3 : 4;
    if (size <= sc) return true;
    const u8 nalType = data[sc] & 0x1F;
    if (nalType == 7 || nalType == 8 || nalType == 9) return true;   // SPS/PPS/AUDはmoovにあるので不要
    const bool isIDR = nalType == 5;

    if (!w.started) {
        if (!isIDR) return true;   // 最初は必ずキーフレームから
        w.started = true;
        w.firstTs = ts;
        w.lastTs = ts;
    } else if (w.count > 0) {
        // 1つ前のフレームの長さが、今のフレームの時刻でわかる
        u64 d = ts > w.lastTs ? ts - w.lastTs : kDefaultDur;
        if (d > 10ULL * kTimescale) d = kDefaultDur;
        w.samples[w.count - 1].duration = u32(d);
    }
    w.lastTs = ts;

    // Annex-B → MP4(NALごとに4バイトの長さ)。3バイトのスタートコードだと1バイト増える
    const size_t worst = size + 16;
    if (w.count >= kMaxSamples || w.len + worst > w.cap || (isIDR && w.count > 0)) {
        if (!flush(w)) return false;
    }
    if (worst > w.cap) return true;   // 大きすぎるフレームは捨てる(画面が一瞬乱れるだけ)

    size_t start = w.len;
    size_t i = 0;
    while (i < size) {
        // 次のスタートコードを探す
        size_t s = i;
        while (s + 3 <= size && !(data[s] == 0 && data[s + 1] == 0 &&
               (data[s + 2] == 1 || (s + 4 <= size && data[s + 2] == 0 && data[s + 3] == 1)))) s++;
        if (s + 3 > size) break;
        size_t nalStart = s + (data[s + 2] == 1 ? 3 : 4);
        size_t e = nalStart;
        while (e + 3 <= size && !(data[e] == 0 && data[e + 1] == 0 && (data[e + 2] == 1 ||
               (e + 4 <= size && data[e + 2] == 0 && data[e + 3] == 1)))) e++;
        if (e + 3 > size) e = size;
        const u32 n = u32(e - nalStart);
        u8 *o = w.buf + w.len;
        o[0] = u8(n >> 24); o[1] = u8(n >> 16); o[2] = u8(n >> 8); o[3] = u8(n);
        std::memcpy(o + 4, data + nalStart, n);
        w.len += 4 + n;
        i = e;
    }

    w.samples[w.count++] = Sample{ u32(w.len - start), kDefaultDur, isIDR };
    return true;
}

// 録画終了時に呼ぶ
inline bool finish(Writer &w) {
    return flush(w);
}

inline u64 durationUs(const Writer &w) {
    if (!w.started) return 0;
    return w.lastTs - w.firstTs;
}

} // namespace mp4
