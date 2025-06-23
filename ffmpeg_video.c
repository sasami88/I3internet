#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h> // エラー文字列用ヘッダーを追加
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

// ──────────────────────────────────────────────────
//   FFmpegでの映像圧縮
// ──────────────────────────────────────────────────
void encode_video_frame(AVCodecContext *codec_ctx, AVFrame *frame, int socket_fd) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE]; // エラー文字列用バッファ
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Failed to allocate AVPacket\n");
        return;
    }

    int ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf)); // エラー文字列を取得
        fprintf(stderr, "Error sending frame to encoder: %s\n", err_buf);
        av_packet_free(&packet);
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf)); // エラー文字列を取得
            fprintf(stderr, "Error receiving packet from encoder: %s\n", err_buf);
            break;
        }

        // Send the encoded packet over the socket
        int size = packet->size;
        send(socket_fd, &size, sizeof(size), 0);
        send(socket_fd, packet->data, size, 0);

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

// ──────────────────────────────────────────────────
//   FFmpegでの映像デコード
// ──────────────────────────────────────────────────
void decode_video_frame(AVCodecContext *codec_ctx, int socket_fd) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE]; // エラー文字列用バッファ
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!packet || !frame) {
        fprintf(stderr, "Failed to allocate AVPacket or AVFrame\n");
        return;
    }

    int size;
    while (recv(socket_fd, &size, sizeof(size), 0) > 0) {
        packet->size = size;
        packet->data = (uint8_t *)malloc(size);
        recv(socket_fd, packet->data, size, 0);

        int ret = avcodec_send_packet(codec_ctx, packet);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf)); // エラー文字列を取得
            fprintf(stderr, "Error sending packet to decoder: %s\n", err_buf);
            free(packet->data);
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf)); // エラー文字列を取得
                fprintf(stderr, "Error receiving frame from decoder: %s\n", err_buf);
                break;
            }

            // Display the decoded frame using OpenCV or GTK
            // (This part will depend on your display implementation)
        }

        free(packet->data);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
}