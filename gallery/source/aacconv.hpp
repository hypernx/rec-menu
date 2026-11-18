#pragma once
// 録画した動画の音声(PCM)を AAC に変換する。映像はそのままコピーする。
// スマホのプレーヤーは MP4 の中の PCM 音声を鳴らせないことが多いため。
#include <string>
#include <vector>
#include <functional>
#include <cstdio>
#include "mp4std.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include "faac/faac.h"
}

namespace conv {

enum class Result { Ok, Skipped, Failed, Cancelled };

// ffmpeg に渡すパス("sdmc:" を通信方式の名前と間違えないように)
inline std::string avPath(const std::string &p) { return "file:" + p; }

// 音声が PCM(変換が必要)なら true
inline bool hasPcmAudio(const std::string &path) {
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, avPath(path).c_str(), nullptr, nullptr) < 0) return false;
    bool pcm = false;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters *cp = fmt->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_AUDIO && cp->codec_id == AV_CODEC_ID_PCM_S16LE) pcm = true;
    }
    avformat_close_input(&fmt);
    return pcm;
}

// progress(0〜1) が false を返したら中止する
inline Result toAac(const std::string &path, const std::function<bool(double)> &progress, std::string &err) {
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, avPath(path).c_str(), nullptr, nullptr) < 0) { err = "open"; return Result::Failed; }
    struct CloseFmt { AVFormatContext **f; ~CloseFmt() { if (*f) avformat_close_input(f); } } closeFmt{ &fmt };
    if (avformat_find_stream_info(fmt, nullptr) < 0) { err = "info"; return Result::Failed; }

    int vi = -1, ai = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters *cp = fmt->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && vi < 0 && cp->codec_id == AV_CODEC_ID_H264) vi = int(i);
        if (cp->codec_type == AVMEDIA_TYPE_AUDIO && ai < 0) ai = int(i);
    }
    if (ai < 0 || fmt->streams[ai]->codecpar->codec_id != AV_CODEC_ID_PCM_S16LE) return Result::Skipped;
    if (vi < 0) { err = "no video"; return Result::Failed; }
    const AVCodecParameters *vp = fmt->streams[vi]->codecpar;
    const AVCodecParameters *ap = fmt->streams[ai]->codecpar;
    if (!vp->extradata || vp->extradata_size < 7 || vp->extradata[0] != 1) { err = "no avcC"; return Result::Failed; }
    const int channels = ap->ch_layout.nb_channels;
    const int rate = ap->sample_rate;
    if (channels < 1 || channels > 2 || rate <= 0) { err = "audio format"; return Result::Failed; }

    // ---- AAC エンコーダー(FAAC)----
    faac_params prm;
    faac_params_init(&prm, sizeof(prm));
    prm.sample_rate = uint32_t(rate);
    prm.num_channels = uint32_t(channels);
    prm.mpeg_version = FAAC_MPEG4;
    prm.object_type = FAAC_OBJ_LOW;          // AAC-LC(どの機器でも再生できる)
    prm.joint_mode = FAAC_JOINT_MS;
    prm.use_tns = true;
    prm.bit_rate = 64000;                    // 1チャンネルあたり(ステレオで 128kbps)
    prm.output_format = FAAC_STREAM_RAW;
    prm.input_format = FAAC_INPUT_16BIT;
    faac_encoder *enc = nullptr;
    if (faac_encoder_open(&prm, &enc) != FAAC_OK) { err = "aac open"; return Result::Failed; }
    struct CloseEnc { faac_encoder **e; ~CloseEnc() { faac_encoder_close(e); } } closeEnc{ &enc };
    faac_encoder_info info{};
    info.struct_size = sizeof(info);
    faac_encoder_get_info(enc, &info);
    const uint8_t *asc = nullptr;
    uint32_t ascLen = 0;
    if (faac_encoder_asc(enc, &asc, &ascLen) != FAAC_OK || !asc) { err = "aac asc"; return Result::Failed; }

    // ---- 書き出し(一時ファイル)----
    const std::string tmp = path + ".tmp";
    mp4std::Writer w;
    std::vector<uint8_t> avcC(vp->extradata, vp->extradata + vp->extradata_size);
    if (!w.open(tmp, avcC, uint16_t(vp->width), uint16_t(vp->height), std::vector<uint8_t>(asc, asc + ascLen),
                uint32_t(rate), uint32_t(channels), uint32_t(prm.bit_rate * channels))) {
        err = "write";
        return Result::Failed;
    }
    auto fail = [&](const char *why) { w.abort(); remove(tmp.c_str()); err = why; return Result::Failed; };

    std::vector<uint8_t> out(info.max_output_bytes ? info.max_output_bytes : 8192);
    const uint32_t frameSamples = (info.frame_samples ? info.frame_samples : 1024) * uint32_t(channels);
    std::vector<int16_t> silence(frameSamples, 0);
    auto encode = [&](const int16_t *pcm, uint32_t samples) -> bool {   // samples = 全チャンネル合計
        while (samples > 0) {
            const uint32_t n = samples < frameSamples ? samples : frameSamples;
            uint32_t wr = 0;
            if (faac_encoder_encode(enc, pcm, n, out.data(), uint32_t(out.size()), &wr) != FAAC_OK) return false;
            if (wr && !w.addAudio(out.data(), wr)) return false;
            if (pcm) pcm += n;
            samples -= n;
        }
        return true;
    };

    const AVRational usTb{ 1, 1000000 }, sTb{ 1, rate };
    const AVRational vtb = fmt->streams[vi]->time_base, atb = fmt->streams[ai]->time_base;
    int64_t originUs = AV_NOPTS_VALUE;            // 動画の0秒(最初の映像の時刻)
    int64_t audioStartUs = -1;                    // 音声が始まる位置
    int64_t fed = 0;                              // エンコーダーに入れたサンプル数(1チャンネルあたり)
    const double durUs = fmt->duration > 0 ? double(fmt->duration) : 0;
    AVPacket *pkt = av_packet_alloc();
    struct FreePkt { AVPacket **p; ~FreePkt() { av_packet_free(p); } } freePkt{ &pkt };
    uint64_t lastProgress = 0;

    while (av_read_frame(fmt, pkt) >= 0) {
        int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (pkt->stream_index == vi && ts != AV_NOPTS_VALUE) {
            int64_t us = av_rescale_q(ts, vtb, usTb);
            if (originUs == AV_NOPTS_VALUE) originUs = us;
            us -= originUs;
            if (us < 0) us = 0;
            if (!w.addVideo(pkt->data, size_t(pkt->size), uint64_t(us), (pkt->flags & AV_PKT_FLAG_KEY) != 0)) { av_packet_unref(pkt); return fail("write"); }
            // 進み具合
            if (durUs > 0 && ++lastProgress % 15 == 0 && !progress(std::min(1.0, double(us) / durUs))) {
                av_packet_unref(pkt);
                w.abort();
                remove(tmp.c_str());
                return Result::Cancelled;
            }
        } else if (pkt->stream_index == ai && ts != AV_NOPTS_VALUE && originUs != AV_NOPTS_VALUE) {
            const int64_t pos = av_rescale_q(av_rescale_q(ts, atb, usTb) - originUs, usTb, sTb);   // サンプル位置
            const int16_t *pcm = reinterpret_cast<const int16_t *>(pkt->data);
            int64_t frames = pkt->size / (2 * channels);
            if (audioStartUs < 0) {
                int64_t p = pos;
                if (p < 0) { pcm += -p * channels; frames += p; p = 0; }   // 映像より前の音は使わない
                audioStartUs = av_rescale_q(p, sTb, usTb);
                fed = 0;
                if (frames > 0 && !encode(pcm, uint32_t(frames * channels))) { av_packet_unref(pkt); return fail("aac"); }
                fed += std::max<int64_t>(0, frames);
            } else {
                const int64_t expected = av_rescale_q(audioStartUs, usTb, sTb) + fed;
                int64_t gap = pos - expected;
                if (gap > rate / 20) {   // 音が途切れていた(HOMEメニューなど) → 無音で埋めて、ずれないようにする
                    gap = std::min<int64_t>(gap, int64_t(rate) * 60 * 30);
                    while (gap > 0) {
                        const int64_t n = std::min<int64_t>(gap, frameSamples / channels);
                        if (!encode(silence.data(), uint32_t(n * channels))) { av_packet_unref(pkt); return fail("aac"); }
                        fed += n; gap -= n;
                    }
                } else if (gap < -(rate / 20)) {   // 重なっている部分は飛ばす
                    const int64_t skip = std::min<int64_t>(-gap, frames);
                    pcm += skip * channels; frames -= skip;
                }
                if (frames > 0 && !encode(pcm, uint32_t(frames * channels))) { av_packet_unref(pkt); return fail("aac"); }
                fed += std::max<int64_t>(0, frames);
            }
        }
        av_packet_unref(pkt);
    }

    // エンコーダーに残っている分を出す
    for (int i = 0; i < 64; i++) {
        uint32_t wr = 0;
        if (faac_encoder_encode(enc, nullptr, 0, out.data(), uint32_t(out.size()), &wr) != FAAC_OK) break;
        if (wr == 0) break;
        if (!w.addAudio(out.data(), wr)) return fail("write");
    }
    if (!w.finish(uint64_t(std::max<int64_t>(0, audioStartUs)), info.encoder_delay, uint64_t(fed))) {
        remove(tmp.c_str());
        err = "write";
        return Result::Failed;
    }
    progress(1.0);

    // 元のファイルと入れ替える(途中で失敗しても元の動画が消えないように、一度 .bak に名前を変える)
    avformat_close_input(&fmt);
    const std::string bak = path + ".bak";
    remove(bak.c_str());
    if (rename(path.c_str(), bak.c_str()) != 0) { remove(tmp.c_str()); err = "replace"; return Result::Failed; }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        rename(bak.c_str(), path.c_str());   // 元に戻す
        remove(tmp.c_str());
        err = "rename";
        return Result::Failed;
    }
    remove(bak.c_str());
    return Result::Ok;
}

} // namespace conv
