#pragma once
// 普通の(フラグメントでない)MP4を書き出す小さなライター。スマホで再生しやすい形式。
//  ftyp → mdat(映像と音声のデータ) → moov(目次) の順に書く。
//  映像: H.264(avcC をそのまま使う)、音声: AAC(AudioSpecificConfig を esds に入れる)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace mp4std {

struct Buf {
    std::vector<uint8_t> d;
    void u8(uint32_t v) { d.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void bytes(const void *p, size_t n) { const uint8_t *b = static_cast<const uint8_t *>(p); d.insert(d.end(), b, b + n); }
    void zeros(size_t n) { d.insert(d.end(), n, 0); }
    size_t open(const char *t) { size_t at = d.size(); u32(0); bytes(t, 4); return at; }
    size_t openFull(const char *t, uint8_t ver, uint32_t flags) { size_t at = open(t); u32((uint32_t(ver) << 24) | flags); return at; }
    void close(size_t at) {
        uint32_t s = uint32_t(d.size() - at);
        d[at] = uint8_t(s >> 24); d[at + 1] = uint8_t(s >> 16); d[at + 2] = uint8_t(s >> 8); d[at + 3] = uint8_t(s);
    }
};

struct Track {
    uint32_t timescale = 1000;
    std::vector<uint32_t> sizes;
    std::vector<uint64_t> offsets;
    std::vector<uint64_t> times;     // 映像: 各フレームの時刻(timescale単位)
    std::vector<uint32_t> syncs;     // キーフレームの番号(1から)
};

class Writer {
public:
    // avcC: 映像の設定(MP4の avcC そのもの)、asc: AACの設定(AudioSpecificConfig)
    bool open(const std::string &path, const std::vector<uint8_t> &avcC, uint16_t width, uint16_t height,
              const std::vector<uint8_t> &asc, uint32_t audioRate, uint32_t channels, uint32_t audioBitrate) {
        f_ = fopen(path.c_str(), "wb");
        if (!f_) return false;
        avcC_ = avcC; asc_ = asc; w_ = width; h_ = height;
        aRate_ = audioRate; aCh_ = channels; aBitrate_ = audioBitrate;
        v_.timescale = 1000000;   // 映像の時刻はマイクロ秒
        a_.timescale = audioRate;
        Buf b;
        size_t ftyp = b.open("ftyp");
        b.bytes("isom", 4); b.u32(0x200);
        b.bytes("isom", 4); b.bytes("iso2", 4); b.bytes("avc1", 4); b.bytes("mp41", 4);
        b.close(ftyp);
        // mdat は大きくなるので64bitの大きさで書く(あとで書き直す)
        mdatAt_ = b.d.size();
        b.u32(1); b.bytes("mdat", 4); b.u64(0);
        pos_ = 0;   // write() が書いた分だけ進める
        return write(b.d.data(), b.d.size());
    }

    bool addVideo(const uint8_t *data, size_t size, uint64_t timeUs, bool key) {
        v_.sizes.push_back(uint32_t(size));
        v_.offsets.push_back(pos_);
        v_.times.push_back(timeUs);
        if (key) v_.syncs.push_back(uint32_t(v_.sizes.size()));
        return write(data, size);
    }

    // AACの1フレーム(1024サンプル)
    bool addAudio(const uint8_t *data, size_t size) {
        a_.sizes.push_back(uint32_t(size));
        a_.offsets.push_back(pos_);
        return write(data, size);
    }

    // audioStartUs: 音声が動画の何秒目から始まるか、encoderDelay: AACの先頭の余分なサンプル数、
    // audioSamples: 元の音声のサンプル数(最後の余分な部分を再生しないため。0なら全部)
    bool finish(uint64_t audioStartUs, uint32_t encoderDelay, uint64_t audioSamples = 0) {
        if (!f_) return false;
        bool ok = !failed_;
        const uint64_t mdatEnd = pos_;
        Buf b;
        writeMoov(b, audioStartUs, encoderDelay, audioSamples);
        ok = ok && write(b.d.data(), b.d.size());
        // mdat の大きさを書き直す
        if (ok) {
            uint8_t sz[8];
            const uint64_t mdatSize = mdatEnd - mdatAt_;
            for (int i = 0; i < 8; i++) sz[i] = uint8_t(mdatSize >> (56 - 8 * i));
            ok = fseek(f_, long(mdatAt_ + 8), SEEK_SET) == 0 && fwrite(sz, 1, 8, f_) == 8;
        }
        ok = (fclose(f_) == 0) && ok;
        f_ = nullptr;
        return ok;
    }

    void abort() { if (f_) { fclose(f_); f_ = nullptr; } }
    size_t videoFrames() const { return v_.sizes.size(); }
    size_t audioFrames() const { return a_.sizes.size(); }

private:
    bool write(const void *p, size_t n) {
        if (failed_) return false;
        if (n && fwrite(p, 1, n, f_) != n) { failed_ = true; return false; }
        pos_ += n;
        return true;
    }

    static void matrix(Buf &b) {
        const uint32_t m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
        for (uint32_t v : m) b.u32(v);
    }

    // 映像の各フレームの長さ(最後のフレームは1つ前と同じ長さにする)
    std::vector<uint32_t> videoDurations() const {
        std::vector<uint32_t> d;
        const size_t n = v_.times.size();
        for (size_t i = 0; i + 1 < n; i++) {
            uint64_t x = v_.times[i + 1] > v_.times[i] ? v_.times[i + 1] - v_.times[i] : 33333;
            if (x > 10000000) x = 33333;
            d.push_back(uint32_t(x));
        }
        if (n) d.push_back(d.empty() ? 33333 : d.back());
        return d;
    }

    static void stts(Buf &b, const std::vector<uint32_t> &durs) {
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (uint32_t x : durs) {
            if (!runs.empty() && runs.back().second == x) runs.back().first++;
            else runs.push_back({ 1, x });
        }
        size_t at = b.openFull("stts", 0, 0);
        b.u32(uint32_t(runs.size()));
        for (auto &r : runs) { b.u32(r.first); b.u32(r.second); }
        b.close(at);
    }

    static void commonTables(Buf &b, const Track &t) {
        size_t stsc = b.openFull("stsc", 0, 0);   // 1チャンク = 1サンプル
        b.u32(1); b.u32(1); b.u32(1); b.u32(1);
        b.close(stsc);
        size_t stsz = b.openFull("stsz", 0, 0);
        b.u32(0); b.u32(uint32_t(t.sizes.size()));
        for (uint32_t s : t.sizes) b.u32(s);
        b.close(stsz);
        size_t co64 = b.openFull("co64", 0, 0);
        b.u32(uint32_t(t.offsets.size()));
        for (uint64_t o : t.offsets) b.u64(o);
        b.close(co64);
    }

    static void dinf(Buf &b) {
        size_t d = b.open("dinf");
        size_t dref = b.openFull("dref", 0, 0); b.u32(1);
        size_t url = b.openFull("url ", 0, 1); b.close(url);
        b.close(dref); b.close(d);
    }

    static void tkhd(Buf &b, uint32_t id, uint64_t durMs, bool audio, uint16_t w, uint16_t h) {
        size_t t = b.openFull("tkhd", 0, 3);
        b.u32(0); b.u32(0); b.u32(id); b.u32(0); b.u32(uint32_t(durMs));
        b.zeros(8); b.u16(0); b.u16(audio ? 1 : 0); b.u16(audio ? 0x0100 : 0); b.u16(0);
        matrix(b);
        b.u32(uint32_t(w) << 16); b.u32(uint32_t(h) << 16);
        b.close(t);
    }

    static void mdhd(Buf &b, uint32_t ts, uint64_t dur) {
        size_t m = b.openFull("mdhd", 1, 0);
        b.u64(0); b.u64(0); b.u32(ts); b.u64(dur);
        b.u16(0x55C4); b.u16(0);
        b.close(m);
    }

    static void hdlr(Buf &b, const char *type, const char *name) {
        size_t h = b.openFull("hdlr", 0, 0);
        b.u32(0); b.bytes(type, 4); b.zeros(12); b.bytes(name, strlen(name) + 1);
        b.close(h);
    }

    // 記述子の長さ(MPEG-4 の可変長。4バイトで書く)
    static void descLen(Buf &b, uint32_t n) {
        b.u8(0x80 | ((n >> 21) & 0x7F)); b.u8(0x80 | ((n >> 14) & 0x7F)); b.u8(0x80 | ((n >> 7) & 0x7F)); b.u8(n & 0x7F);
    }

    void writeMoov(Buf &b, uint64_t audioStartUs, uint32_t encoderDelay, uint64_t audioSamples) const {
        const std::vector<uint32_t> vd = videoDurations();
        uint64_t vDur = 0;
        for (uint32_t x : vd) vDur += x;
        const uint64_t aDur = uint64_t(a_.sizes.size()) * 1024;
        const uint64_t vDurMs = vDur / 1000;
        uint64_t aPlay = aDur > encoderDelay ? aDur - encoderDelay : 0;
        if (audioSamples > 0 && audioSamples < aPlay) aPlay = audioSamples;
        const uint64_t aPlayMs = aPlay * 1000 / aRate_;
        const uint64_t aStartMs = audioStartUs / 1000;
        const uint64_t movieMs = std::max(vDurMs, aStartMs + aPlayMs);

        size_t moov = b.open("moov");
        size_t mvhd = b.openFull("mvhd", 1, 0);
        b.u64(0); b.u64(0); b.u32(1000); b.u64(movieMs);
        b.u32(0x00010000); b.u16(0x0100); b.zeros(10);
        matrix(b); b.zeros(24);
        b.u32(3);
        b.close(mvhd);

        // ---- 映像 ----
        {
            size_t trak = b.open("trak");
            tkhd(b, 1, vDurMs, false, w_, h_);
            size_t mdia = b.open("mdia");
            mdhd(b, v_.timescale, vDur);
            hdlr(b, "vide", "Video");
            size_t minf = b.open("minf");
            { size_t vmhd = b.openFull("vmhd", 0, 1); b.zeros(8); b.close(vmhd); }
            dinf(b);
            size_t stbl = b.open("stbl");
            size_t stsd = b.openFull("stsd", 0, 0); b.u32(1);
            size_t avc1 = b.open("avc1");
            b.zeros(6); b.u16(1); b.zeros(16);
            b.u16(w_); b.u16(h_);
            b.u32(0x00480000); b.u32(0x00480000); b.u32(0);
            b.u16(1); b.zeros(32); b.u16(0x18); b.u16(0xFFFF);
            size_t avcC = b.open("avcC"); b.bytes(avcC_.data(), avcC_.size()); b.close(avcC);
            b.close(avc1);
            b.close(stsd);
            stts(b, vd);
            if (!v_.syncs.empty() && v_.syncs.size() != v_.sizes.size()) {
                size_t stss = b.openFull("stss", 0, 0);
                b.u32(uint32_t(v_.syncs.size()));
                for (uint32_t s : v_.syncs) b.u32(s);
                b.close(stss);
            }
            commonTables(b, v_);
            b.close(stbl); b.close(minf); b.close(mdia); b.close(trak);
        }

        // ---- 音声 ----
        if (!a_.sizes.empty()) {
            size_t trak = b.open("trak");
            tkhd(b, 2, aStartMs + aPlayMs, true, 0, 0);
            // 編集リスト: 音声の始まりの位置を合わせ、AACの先頭の余分な部分を飛ばす
            size_t edts = b.open("edts");
            size_t elst = b.openFull("elst", 1, 0);
            b.u32(aStartMs > 0 ? 2 : 1);
            if (aStartMs > 0) { b.u64(aStartMs); b.u64(uint64_t(-1)); b.u16(1); b.u16(0); }
            b.u64(aPlayMs); b.u64(encoderDelay); b.u16(1); b.u16(0);
            b.close(elst);
            b.close(edts);
            size_t mdia = b.open("mdia");
            mdhd(b, a_.timescale, aDur);
            hdlr(b, "soun", "Sound");
            size_t minf = b.open("minf");
            { size_t smhd = b.openFull("smhd", 0, 0); b.u16(0); b.u16(0); b.close(smhd); }
            dinf(b);
            size_t stbl = b.open("stbl");
            size_t stsd = b.openFull("stsd", 0, 0); b.u32(1);
            size_t mp4a = b.open("mp4a");
            b.zeros(6); b.u16(1);
            b.u16(0); b.u16(0); b.u32(0);
            b.u16(aCh_); b.u16(16); b.u16(0); b.u16(0);
            b.u32(aRate_ << 16);
            // esds(AACの設定)
            size_t esds = b.openFull("esds", 0, 0);
            const uint32_t dsiLen = uint32_t(asc_.size());
            const uint32_t dcdLen = 13 + 5 + dsiLen;
            const uint32_t esLen = 3 + 5 + dcdLen + 5 + 1;
            b.u8(0x03); descLen(b, esLen); b.u16(2); b.u8(0);
            b.u8(0x04); descLen(b, dcdLen);
            b.u8(0x40);             // MPEG-4 Audio
            b.u8(0x15);             // 音声ストリーム
            b.u8(0); b.u16(0);      // bufferSizeDB
            b.u32(aBitrate_); b.u32(aBitrate_);
            b.u8(0x05); descLen(b, dsiLen); b.bytes(asc_.data(), asc_.size());
            b.u8(0x06); descLen(b, 1); b.u8(0x02);
            b.close(esds);
            b.close(mp4a);
            b.close(stsd);
            stts(b, std::vector<uint32_t>(a_.sizes.size(), 1024));
            commonTables(b, a_);
            b.close(stbl); b.close(minf); b.close(mdia); b.close(trak);
        }
        b.close(moov);
    }

    FILE *f_ = nullptr;
    bool failed_ = false;
    uint64_t pos_ = 0, mdatAt_ = 0;
    std::vector<uint8_t> avcC_, asc_;
    uint16_t w_ = 0, h_ = 0;
    uint32_t aRate_ = 48000, aCh_ = 2, aBitrate_ = 128000;
    Track v_, a_;
};

} // namespace mp4std
