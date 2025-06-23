#include <opencv2/opencv.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// FFmpeg エンコーダ初期化関数
AVCodecContext *init_ffmpeg_encoder() {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Error: H.264 codec not found.\n");
        return NULL;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Error: Failed to allocate codec context.\n");
        return NULL;
    }

    codec_ctx->bit_rate = 400000;
    codec_ctx->width = 640;  // 映像の幅
    codec_ctx->height = 480; // 映像の高さ
    codec_ctx->time_base = (AVRational){1, 30}; // フレームレート
    codec_ctx->framerate = (AVRational){30, 1};
    codec_ctx->gop_size = 10; // GOPサイズ
    codec_ctx->max_b_frames = 1;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Error: Failed to open codec.\n");
        avcodec_free_context(&codec_ctx);
        return NULL;
    }

    return codec_ctx;
}

// OpenCVのフレームをAVFrameに変換する関数
AVFrame *convert_to_avframe(const cv::Mat &frame, AVCodecContext *codec_ctx) {
    AVFrame *av_frame = av_frame_alloc();
    av_frame->format = codec_ctx->pix_fmt;
    av_frame->width = codec_ctx->width;
    av_frame->height = codec_ctx->height;

    av_frame_get_buffer(av_frame, 32);
    av_frame_make_writable(av_frame);

    // OpenCVのデータをYUV形式に変換
    struct SwsContext *sws_ctx = sws_getContext(
        frame.cols, frame.rows, AV_PIX_FMT_BGR24,
        codec_ctx->width, codec_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    uint8_t *src_data[1] = {frame.data};
    int src_linesize[1] = {(int)frame.step};

    sws_scale(sws_ctx, src_data, src_linesize, 0, frame.rows, av_frame->data, av_frame->linesize);
    sws_freeContext(sws_ctx);

    return av_frame;
}

// 映像を圧縮して送信する関数
void encode_and_send_frame(AVCodecContext *codec_ctx, AVFrame *frame, int socket_fd) {
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Error: Failed to allocate AVPacket.\n");
        return;
    }

    int ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0) {
        fprintf(stderr, "Error: Failed to send frame to encoder.\n");
        av_packet_free(&packet);
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error: Failed to receive packet from encoder.\n");
            break;
        }

        // 圧縮されたデータを送信
        int size = packet->size;
        send(socket_fd, &size, sizeof(size), 0);
        send(socket_fd, packet->data, size, 0);

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

int main() {
    cv::VideoCapture cap(0); // カメラ0番（通常はデフォルトカメラ）

    if (!cap.isOpened()) {
        fprintf(stderr, "Error: カメラが開けません。\n");
        return 1;
    }

    cv::Mat frame;

    // FFmpeg エンコーダ初期化
    AVCodecContext *codec_ctx = init_ffmpeg_encoder();
    if (!codec_ctx) {
        fprintf(stderr, "Error: FFmpeg encoder initialization failed.\n");
        return 1;
    }

    // ソケットの初期化
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        fprintf(stderr, "Error: Failed to create socket.\n");
        return 1;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5555); // ポート番号
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr); // サーバーIP

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "Error: Failed to connect to server.\n");
        close(socket_fd);
        return 1;
    }

    while (true) {
        cap >> frame; // フレームを取得
        if (frame.empty()) break;

        // OpenCVのフレームをAVFrameに変換
        AVFrame *av_frame = convert_to_avframe(frame, codec_ctx);

        // FFmpegで映像を圧縮して送信
        encode_and_send_frame(codec_ctx, av_frame, socket_fd);

        av_frame_free(&av_frame);

        if (cv::waitKey(30) >= 0) break; // キー入力で終了
    }

    cap.release();
    cv::destroyAllWindows();
    avcodec_free_context(&codec_ctx);
    close(socket_fd);

    return 0;
}