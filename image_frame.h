// image_frame.h
#ifndef IMAGE_FRAME_H
#define IMAGE_FRAME_H

typedef struct {
    unsigned char *data; // RGBデータへのポインタ
    int width;
    int height;
    int channels;
} ImageFrame;

#endif
