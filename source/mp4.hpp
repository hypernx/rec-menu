#pragma once
#include <switch.h>
#include <cstring>
#include <atomic>

// grc:d の映像(H.264)と音声(PCM)を、その場で MP4 (fragmented MP4) にして書き出す最小限のライター。
// - 最初に ftyp + moov(映像トラック1 + 音声トラック2)を書き、以降は moof + mdat を追記する。
// - 途中で電源が落ちても、最後に書けたところまでは再生できる。
// - 映像: 1280x720 / H.264 High。時刻は grc:d のタイムスタンプ(マイクロ秒)。
// - 音声: 48kHz / 16bit / ステレオ の PCM('sowt')。
// - 映像と音声は別スレッドから書かれるので、書き込み関数(WriteFn)側で排他すること。
namespace mp4 {

constexpr u32 kTimescale   = 1000000;   // 映像: マイクロ秒
constexpr u32 kDefaultDur  = 33333;     // 30fps
constexpr u32 kMaxSamples  = 64;        // 1フラグメントあたりの最大フレーム数(約2秒)
constexpr u32 kAudioRate   = 48000;
constexpr u32 kAudioFrame  = 4;         // 16bit x 2ch

// 1フラグメント(ヘッダ部 + データ部)を書く関数。戻り値 false で失敗
using WriteFn = bool (*)(const void *hdr, size_t hdrSize, const void *data, size_t dataSize);

inline std::atomic<u32> g_seq{0};

struct Sample { u32 size; u32 duration; bool sync; };

struct VideoTrack {
    WriteFn write = nullptr;
    u8 *buf = nullptr;          // フレームデータをためるバッファ(mdatの中身)
    size_t cap = 0;
    size_t len = 0;
    Sample samples[kMaxSamples];
    u32 count = 0;
    u64 firstTs = 0;            // 最初のフレームの時刻(これが動画の0秒)
    u64 fragStart = 0;          // このフラグメントの開始時刻(firstTs基準)
    u64 lastTs = 0;
    std::atomic<bool> started{false};   // 最初のIDRを受け取ったか
};

struct AudioTrack {
    WriteFn write = nullptr;
    u8 *buf = nullptr;
    size_t cap = 0;
    size_t len = 0;
    bool started = false;
    u64 fragStart = 0;          // このフラグメントの開始位置(サンプル数、動画の0秒基準)
    u64 nextPos = 0;            // 次に来るはずの位置(サンプル数)
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
    void close(size_t at) { put32(at, u32(n - at)); }
    void put32(size_t at, u32 v) {
        p[at] = u8(v >> 24); p[at + 1] = u8(v >> 16); p[at + 2] = u8(v >> 8); p[at + 3] = u8(v);
    }
};

inline void matrix(Box &b) {
    const u32 m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
    for (u32 v : m) b.u32_(v);
}

inline void emptyTables(Box &b) {
    size_t stts = b.openFull("stts", 0, 0); b.u32_(0); b.close(stts);
    size_t stsc = b.openFull("stsc", 0, 0); b.u32_(0); b.close(stsc);
    size_t stsz = b.openFull("stsz", 0, 0); b.u32_(0); b.u32_(0); b.close(stsz);
    size_t stco = b.openFull("stco", 0, 0); b.u32_(0); b.close(stco);
}

inline void dinf(Box &b) {
    size_t d = b.open("dinf");
    size_t dref = b.openFull("dref", 0, 0); b.u32_(1);
    size_t url = b.openFull("url ", 0, 1); b.close(url);
    b.close(dref); b.close(d);
}

inline void tkhd(Box &b, u32 trackId, bool audio, u16 width, u16 height) {
    size_t t = b.openFull("tkhd", 0, 0x3);
    b.u32_(0); b.u32_(0); b.u32_(trackId); b.u32_(0); b.u32_(0);
    b.zeros(8); b.u16_(0); b.u16_(audio ? 1 : 0); b.u16_(audio ? 0x0100 : 0); b.u16_(0);
    matrix(b);
    b.u32_(u32(width) << 16); b.u32_(u32(height) << 16);
    b.close(t);
}

inline void mdhd(Box &b, u32 timescale) {
    size_t m = b.openFull("mdhd", 0, 0);
    b.u32_(0); b.u32_(0); b.u32_(timescale); b.u32_(0);
    b.u16_(0x55C4); b.u16_(0);     // 言語 "und"
    b.close(m);
}

inline void hdlr(Box &b, const char *type, const char *name, size_t nameLen) {
    size_t h = b.openFull("hdlr", 0, 0);
    b.u32_(0); b.bytes(type, 4); b.zeros(12); b.bytes(name, nameLen);
    b.close(h);
}

// ftyp + moov を書く。sps/pps はスタートコード無しのNAL本体
inline bool begin(WriteFn fn, VideoTrack &v, u8 *vbuf, size_t vcap, AudioTrack &a, u8 *abuf, size_t acap,
                  const u8 *sps, size_t spsLen, const u8 *pps, size_t ppsLen, u16 width, u16 height) {
    v.write = fn; v.buf = vbuf; v.cap = vcap; v.len = 0; v.count = 0;
    v.firstTs = 0; v.fragStart = 0; v.lastTs = 0; v.started = false;
    a.write = fn; a.buf = abuf; a.cap = acap; a.len = 0;
    a.started = false; a.fragStart = 0; a.nextPos = 0;
    g_seq = 0;

    static u8 hdr[1536];
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
        b.u32_(3);                      // next_track_ID
        b.close(mvhd);

        // ---- 映像トラック ----
        size_t trak = b.open("trak");
        tkhd(b, 1, false, width, height);
        size_t mdia = b.open("mdia");
        mdhd(b, kTimescale);
        hdlr(b, "vide", "Video\0", 6);
        size_t minf = b.open("minf");
        { size_t vmhd = b.openFull("vmhd", 0, 1); b.zeros(8); b.close(vmhd); }
        dinf(b);
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
            emptyTables(b);
        }
        b.close(stbl); b.close(minf); b.close(mdia); b.close(trak);

        // ---- 音声トラック ----
        trak = b.open("trak");
        tkhd(b, 2, true, 0, 0);
        mdia = b.open("mdia");
        mdhd(b, kAudioRate);
        hdlr(b, "soun", "Sound\0", 6);
        minf = b.open("minf");
        { size_t smhd = b.openFull("smhd", 0, 0); b.u16_(0); b.u16_(0); b.close(smhd); }
        dinf(b);
        stbl = b.open("stbl");
        {
            size_t stsd = b.openFull("stsd", 0, 0); b.u32_(1);
            size_t sowt = b.open("sowt");           // 16bitリトルエンディアンPCM
            b.zeros(6); b.u16_(1);
            b.u16_(0); b.u16_(0); b.u32_(0);        // version, revision, vendor
            b.u16_(2); b.u16_(16);                  // channels, bits
            b.u16_(0); b.u16_(0);
            b.u32_(kAudioRate << 16);
            b.close(sowt);
            b.close(stsd);
            emptyTables(b);
        }
        b.close(stbl); b.close(minf); b.close(mdia); b.close(trak);

        size_t mvex = b.open("mvex");
        for (u32 id = 1; id <= 2; id++) {
            size_t trex = b.openFull("trex", 0, 0);
            b.u32_(id); b.u32_(1); b.u32_(0); b.u32_(0); b.u32_(0);
            b.close(trex);
        }
        b.close(mvex);
    }
    b.close(moov);

    return fn(hdr, b.n, nullptr, 0);
}

// ---------------------------------------------------------------------------
// 映像
// ---------------------------------------------------------------------------

inline bool flushVideo(VideoTrack &w) {
    if (w.count == 0) return true;

    static u8 moofBuf[256 + kMaxSamples * 12];
    Box b(moofBuf);
    size_t moof = b.open("moof");
    size_t mfhd = b.openFull("mfhd", 0, 0); b.u32_(++g_seq); b.close(mfhd);
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
    b.put32(dataOffsetAt, u32(b.n + 8));          // moofの大きさ + mdatのヘッダ
    size_t mdat = b.n; b.u32_(u32(w.len + 8)); b.bytes("mdat", 4); (void)mdat;

    if (!w.write(moofBuf, b.n, w.buf, w.len)) return false;

    for (u32 i = 0; i < w.count; i++) w.fragStart += w.samples[i].duration;
    w.count = 0;
    w.len = 0;
    return true;
}

// 1パケット(Annex-B形式、通常NAL 1個)を1フレームとして追加する
inline bool addVideo(VideoTrack &w, const u8 *data, size_t size, u64 ts) {
    size_t sc = (size >= 4 && data[2] == 1) ? 3 : 4;
    if (size <= sc) return true;
    const u8 nalType = data[sc] & 0x1F;
    if (nalType == 7 || nalType == 8 || nalType == 9) return true;   // SPS/PPS/AUDはmoovにあるので不要
    const bool isIDR = nalType == 5;

    if (!w.started) {
        if (!isIDR) return true;   // 最初は必ずキーフレームから
        w.firstTs = ts;
        w.lastTs = ts;
        w.started = true;
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
        if (!flushVideo(w)) return false;
    }
    if (worst > w.cap) return true;   // 大きすぎるフレームは捨てる(画面が一瞬乱れるだけ)

    size_t start = w.len;
    size_t i = 0;
    auto isStart = [&](size_t p) {
        return p + 3 <= size && data[p] == 0 && data[p + 1] == 0 &&
               (data[p + 2] == 1 || (p + 4 <= size && data[p + 2] == 0 && data[p + 3] == 1));
    };
    while (i < size) {
        size_t s = i;
        while (s + 3 <= size && !isStart(s)) s++;
        if (s + 3 > size) break;
        size_t nalStart = s + (data[s + 2] == 1 ? 3 : 4);
        size_t e = nalStart;
        while (e + 3 <= size && !isStart(e)) e++;
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

inline u64 durationUs(const VideoTrack &w) {
    if (!w.started) return 0;
    return w.lastTs - w.firstTs;
}

// ---------------------------------------------------------------------------
// 音声
// ---------------------------------------------------------------------------

inline bool flushAudio(AudioTrack &a) {
    if (a.len == 0) return true;
    const u32 count = u32(a.len / kAudioFrame);

    u8 moofBuf[160];
    Box b(moofBuf);
    size_t moof = b.open("moof");
    size_t mfhd = b.openFull("mfhd", 0, 0); b.u32_(++g_seq); b.close(mfhd);
    size_t traf = b.open("traf");
    // default-base-is-moof + 既定の長さ(1サンプル) + 既定の大きさ(4バイト)
    size_t tfhd = b.openFull("tfhd", 0, 0x020000 | 0x08 | 0x10);
    b.u32_(2); b.u32_(1); b.u32_(kAudioFrame);
    b.close(tfhd);
    size_t tfdt = b.openFull("tfdt", 1, 0); b.u64_(a.fragStart); b.close(tfdt);
    size_t trun = b.openFull("trun", 0, 0x000001);   // data_offset のみ
    b.u32_(count);
    size_t dataOffsetAt = b.n; b.u32_(0);
    b.close(trun);
    b.close(traf);
    b.close(moof);
    b.put32(dataOffsetAt, u32(b.n + 8));
    b.u32_(u32(a.len + 8)); b.bytes("mdat", 4);

    if (!a.write(moofBuf, b.n, a.buf, a.len)) return false;
    a.fragStart += count;
    a.len = 0;
    return true;
}

// 音声パケット(PCM)を追加する。映像が始まる前の音は捨て、映像の0秒に合わせる
inline bool addAudio(AudioTrack &a, const VideoTrack &v, const u8 *data, size_t size, u64 ts) {
    size -= size % kAudioFrame;
    if (size == 0 || !v.started) return true;

    // このパケットの位置(動画の0秒からのサンプル数)
    s64 pos;
    if (ts >= v.firstTs) pos = s64((ts - v.firstTs) * kAudioRate / 1000000ULL);
    else                 pos = -s64((v.firstTs - ts) * kAudioRate / 1000000ULL);

    if (!a.started) {
        if (pos < 0) {   // 映像より前の部分は切り捨てる
            const size_t skip = size_t(-pos) * kAudioFrame;
            if (skip >= size) return true;
            data += skip; size -= skip; pos = 0;
        }
        a.started = true;
        a.fragStart = u64(pos);
        a.nextPos = u64(pos);
    }

    // HOMEメニュー等で音が途切れたら、新しいフラグメントを正しい位置から始める
    const s64 gap = pos - s64(a.nextPos);
    if (gap > s64(kAudioRate / 20) || gap < -s64(kAudioRate / 20)) {
        if (!flushAudio(a)) return false;
        if (pos > s64(a.fragStart)) a.fragStart = u64(pos);
        a.nextPos = a.fragStart;
    }

    while (size > 0) {
        size_t n = a.cap - a.len;
        if (n > size) n = size;
        std::memcpy(a.buf + a.len, data, n);
        a.len += n; data += n; size -= n;
        a.nextPos += n / kAudioFrame;
        if (a.len == a.cap && !flushAudio(a)) return false;
    }
    return true;
}

} // namespace mp4
