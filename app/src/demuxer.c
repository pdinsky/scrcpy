#include "demuxer.h"

#include <assert.h>
#include <libavutil/time.h>
#include <unistd.h>

#include "decoder.h"
#include "events.h"
#include "packet_merger.h"
#include "recorder.h"
#include "util/binary.h"
#include "util/log.h"

#define SC_PACKET_HEADER_SIZE 12

#define SC_PACKET_FLAG_CONFIG    (UINT64_C(1) << 63)
#define SC_PACKET_FLAG_KEY_FRAME (UINT64_C(1) << 62)

#define SC_PACKET_PTS_MASK (SC_PACKET_FLAG_KEY_FRAME - 1)

static enum AVCodecID
sc_demuxer_to_avcodec_id(uint32_t codec_id) {
#define SC_CODEC_ID_H264 UINT32_C(0x68323634) // "h264" in ASCII
#define SC_CODEC_ID_H265 UINT32_C(0x68323635) // "h265" in ASCII
#define SC_CODEC_ID_AV1 UINT32_C(0x00617631) // "av1" in ASCII
#define SC_CODEC_ID_OPUS UINT32_C(0x6f707573) // "opus" in ASCII
    switch (codec_id) {
        case SC_CODEC_ID_H264:
            return AV_CODEC_ID_H264;
        case SC_CODEC_ID_H265:
            return AV_CODEC_ID_HEVC;
        case SC_CODEC_ID_AV1:
            return AV_CODEC_ID_AV1;
        case SC_CODEC_ID_OPUS:
            return AV_CODEC_ID_OPUS;
        default:
            LOGE("Unknown codec id 0x%08" PRIx32, codec_id);
            return AV_CODEC_ID_NONE;
    }
}

static bool
sc_demuxer_recv_codec_id(struct sc_demuxer *demuxer, uint32_t *codec_id) {
    uint8_t data[4];
    ssize_t r = net_recv_all(demuxer->socket, data, 4);
    if (r < 4) {
        return false;
    }

    *codec_id = sc_read32be(data);
    return true;
}

static bool
sc_demuxer_recv_packet(struct sc_demuxer *demuxer, AVPacket *packet) {
    // The video stream contains raw packets, without time information. When we
    // record, we retrieve the timestamps separately, from a "meta" header
    // added by the server before each raw packet.
    //
    // The "meta" header length is 12 bytes:
    // [. . . . . . . .|. . . .]. . . . . . . . . . . . . . . ...
    //  <-------------> <-----> <-----------------------------...
    //        PTS        packet        raw packet
    //                    size
    //
    // It is followed by <packet_size> bytes containing the packet/frame.
    //
    // The most significant bits of the PTS are used for packet flags:
    //
    //  byte 7   byte 6   byte 5   byte 4   byte 3   byte 2   byte 1   byte 0
    // CK...... ........ ........ ........ ........ ........ ........ ........
    // ^^<------------------------------------------------------------------->
    // ||                                PTS
    // | `- key frame
    //  `-- config packet

    uint8_t header[SC_PACKET_HEADER_SIZE];
    ssize_t r = net_recv_all(demuxer->socket, header, SC_PACKET_HEADER_SIZE);
    if (r < SC_PACKET_HEADER_SIZE) {
        return false;
    }

    uint64_t pts_flags = sc_read64be(header);
    uint32_t len = sc_read32be(&header[8]);
    assert(len);

    if (av_new_packet(packet, len)) {
        LOG_OOM();
        return false;
    }

    r = net_recv_all(demuxer->socket, packet->data, len);
    if (r < 0 || ((uint32_t) r) < len) {
        av_packet_unref(packet);
        return false;
    }

    if (pts_flags & SC_PACKET_FLAG_CONFIG) {
        packet->pts = AV_NOPTS_VALUE;
    } else {
        packet->pts = pts_flags & SC_PACKET_PTS_MASK;
    }

    if (pts_flags & SC_PACKET_FLAG_KEY_FRAME) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    packet->dts = packet->pts;
    return true;
}

static bool
push_packet_to_sinks(struct sc_demuxer *demuxer, const AVPacket *packet) {
    for (unsigned i = 0; i < demuxer->sink_count; ++i) {
        struct sc_packet_sink *sink = demuxer->sinks[i];
        if (!sink->ops->push(sink, packet)) {
            return false;
        }
    }

    return true;
}

static bool
sc_demuxer_push_packet(struct sc_demuxer *demuxer, AVPacket *packet) {
    bool ok = push_packet_to_sinks(demuxer, packet);
    if (!ok) {
        LOGE("Demuxer '%s': could not process packet", demuxer->name);
        return false;
    }

    return true;
}

static void
sc_demuxer_close_first_sinks(struct sc_demuxer *demuxer, unsigned count) {
    while (count) {
        struct sc_packet_sink *sink = demuxer->sinks[--count];
        sink->ops->close(sink);
    }
}

static inline void
sc_demuxer_close_sinks(struct sc_demuxer *demuxer) {
    sc_demuxer_close_first_sinks(demuxer, demuxer->sink_count);
}

static bool
sc_demuxer_open_sinks(struct sc_demuxer *demuxer, const AVCodec *codec) {
    for (unsigned i = 0; i < demuxer->sink_count; ++i) {
        struct sc_packet_sink *sink = demuxer->sinks[i];
        if (!sink->ops->open(sink, codec)) {
            sc_demuxer_close_first_sinks(demuxer, i);
            return false;
        }
    }

    return true;
}

static int
run_demuxer(void *data) {
    struct sc_demuxer *demuxer = data;

    // Flag to report end-of-stream (i.e. device disconnected)
    bool eos = false;

    uint32_t raw_codec_id;
    bool ok = sc_demuxer_recv_codec_id(demuxer, &raw_codec_id);
    if (!ok) {
        eos = true;
        goto end;
    }

    enum AVCodecID codec_id = sc_demuxer_to_avcodec_id(raw_codec_id);
    if (codec_id == AV_CODEC_ID_NONE) {
        // Error already logged
        goto end;
    }

    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        LOGE("Demuxer '%s': decoder not found", demuxer->name);
        goto end;
    }

    if (!sc_demuxer_open_sinks(demuxer, codec)) {
        goto end;
    }

    // Config packets must be merged with the next non-config packet only for
    // video streams
    bool must_merge_config_packet = codec->type == AVMEDIA_TYPE_VIDEO;

    struct sc_packet_merger merger;

    if (must_merge_config_packet) {
        sc_packet_merger_init(&merger);
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        LOG_OOM();
        goto finally_close_sinks;
    }

    for (;;) {
        bool ok = sc_demuxer_recv_packet(demuxer, packet);
        if (!ok) {
            // end of stream
            eos = true;
            break;
        }

        if (must_merge_config_packet) {
            // Prepend any config packet to the next media packet
            ok = sc_packet_merger_merge(&merger, packet);
            if (!ok) {
                av_packet_unref(packet);
                break;
            }
        }

        ok = sc_demuxer_push_packet(demuxer, packet);
        av_packet_unref(packet);
        if (!ok) {
            // cannot process packet (error already logged)
            break;
        }
    }

    LOGD("Demuxer '%s': end of frames", demuxer->name);

    if (must_merge_config_packet) {
        sc_packet_merger_destroy(&merger);
    }

    av_packet_free(&packet);
finally_close_sinks:
    sc_demuxer_close_sinks(demuxer);
end:
    demuxer->cbs->on_ended(demuxer, eos, demuxer->cbs_userdata);

    return 0;
}

void
sc_demuxer_init(struct sc_demuxer *demuxer, const char *name, sc_socket socket,
                const struct sc_demuxer_callbacks *cbs, void *cbs_userdata) {
    assert(socket != SC_SOCKET_NONE);

    demuxer->name = name; // statically allocated
    demuxer->socket = socket;
    demuxer->sink_count = 0;

    assert(cbs && cbs->on_ended);

    demuxer->cbs = cbs;
    demuxer->cbs_userdata = cbs_userdata;
}

void
sc_demuxer_add_sink(struct sc_demuxer *demuxer, struct sc_packet_sink *sink) {
    assert(demuxer->sink_count < SC_DEMUXER_MAX_SINKS);
    assert(sink);
    assert(sink->ops);
    demuxer->sinks[demuxer->sink_count++] = sink;
}

bool
sc_demuxer_start(struct sc_demuxer *demuxer) {
    LOGD("Demuxer '%s': starting thread", demuxer->name);

    bool ok = sc_thread_create(&demuxer->thread, run_demuxer, "scrcpy-demuxer",
                               demuxer);
    if (!ok) {
        LOGE("Demuxer '%s': could not start thread", demuxer->name);
        return false;
    }
    return true;
}

void
sc_demuxer_join(struct sc_demuxer *demuxer) {
    sc_thread_join(&demuxer->thread, NULL);
}
