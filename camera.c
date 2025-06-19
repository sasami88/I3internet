#include <opencv2/opencv.hpp>
#include <stdio.h>
#include "image_frame.h"
#include <stdlib.h>
#include <string.h>

// ImageFrame を作成する関数
ImageFrame create_image_frame(const cv::Mat& frame) {
    ImageFrame img;
    img.width = frame.cols;
    img.height = frame.rows;
    img.channels = frame.channels();

    size_t size = img.width * img.height * img.channels;
    img.data = (unsigned char*)malloc(size);
    memcpy(img.data, frame.data, size);

    return img;
}

// メモリ解放
void free_image_frame(ImageFrame* img) {
    free(img->data);
    img->data = NULL;
}


int main() {
    cv::VideoCapture cap(0); // カメラ0番（通常はデフォルトカメラ）

    if (!cap.isOpened()) {
        fprintf(stderr, "Error: カメラが開けません。\n");
        return 1;
    }

    cv::Mat frame;

    while (true) {
        cap >> frame; // フレームを取得
        if (frame.empty()) break;

        // 映像を表示
        //cv::imshow("Camera", frame);
        // ImageFrame に変換（他モジュール用）
        ImageFrame img = create_image_frame(frame);

        //ここでBさん or Cさんの関数に img を渡す（例：encode_frame(&img) など）

        free_image_frame(&img); // 毎フレーム後に忘れず解放！

        if (cv::waitKey(1) == 'q') break;
   
    }

    cap.release();
    cv::destroyAllWindows();
    return 0;
}
