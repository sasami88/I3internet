#ifndef FFMPEG_VIDEO_H
#define FFMPEG_VIDEO_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

void encode_video_frame(AVCodecContext *codec_ctx, AVFrame *frame, int socket_fd);
void decode_video_frame(AVCodecContext *codec_ctx, int socket_fd);

#endif