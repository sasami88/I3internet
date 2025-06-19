#include <opencv2/opencv.hpp>
#include <stdio.h>

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

        cv::imshow("Camera", frame); // 取得した映像をウィンドウ表示

        if (cv::waitKey(30) >= 0) break; // キー入力で終了
    }

    cap.release();
    cv::destroyAllWindows();
    return 0;
}
