// audio_video_chat_gui.c
// Bidirectional **audio + webcam video** chat with GTK GUI
// ---------------------------------------------------------
// Build (needs GTK3, pthread, OpenCV, SoX, FFmpeg libjpeg):
//   g++ -std=c++17 audio_video_chat_gui.c -o av_chat_gui \
//      $(pkg-config --cflags --libs gtk+-3.0 opencv4) -lpthread
//   # SoX は rec/play 用、OpenCV はカメラ＆JPEG圧縮用
// 2025‑06‑19  (minimal demo)

// ─── system / POSIX ──────────────────────────────────────
#include <gtk/gtk.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <thread>
#include <chrono>


// ─── FFmpeg (C ライブラリ) ───────────────────────────────
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

// ─── C++ ライブラリ (OpenCV など) ────────────────────────
#include <opencv2/opencv.hpp> 

AVCodecContext *enc_ctx = nullptr;
SwsContext     *sws_ctx = nullptr;
static SwsContext     *dec_sws  = nullptr;
static AVCodecContext *dec_ctx  = nullptr;

bool init_encoder(int w, int h, int fps)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) { fprintf(stderr, "libx264 not found\n"); return false; }

    enc_ctx = avcodec_alloc_context3(codec);
    enc_ctx->width     = w;
    enc_ctx->height    = h;
    enc_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = {1, fps};
    enc_ctx->framerate = {fps, 1};
    enc_ctx->bit_rate  = 800'000;               // 例: 800 kbps
    // 低遅延用オプション
    av_opt_set(enc_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(enc_ctx->priv_data, "tune",   "zerolatency", 0);  /* :contentReference[oaicite:0]{index=0} */

    if (avcodec_open2(enc_ctx, codec, nullptr) < 0) return false;

    // BGR(OpenCV) → YUV420P 変換器
    sws_ctx = sws_getContext(w, h, AV_PIX_FMT_BGR24,
                             w, h, AV_PIX_FMT_YUV420P,
                             SWS_FAST_BILINEAR, nullptr,nullptr,nullptr);
    return sws_ctx;
}


/*──────────────────────
  CONFIGURATION
  audio : TCP <port>
  video : TCP <port>+1 (JPEG圧縮フレーム)
──────────────────────*/

static void run_server(const char *port);
static void run_client(const char *ip, const char *port);

typedef struct {
    GtkWidget *entry_ip;
    GtkWidget *entry_port;
    GtkWidget *radio_server;
    GtkWidget *label_status;
    GtkWidget *image_peer;
    pthread_t  worker;
    gboolean   running;
} App;
static App app = {0};

static int srv_sock_audio=-1, cli_sock_audio=-1;
static int srv_sock_video=-1, cli_sock_video=-1;
static FILE *rec_stream=NULL;

/*──────────────────────
  GTK helper
──────────────────────*/
static gboolean status_cb(gpointer d){gtk_label_set_text(GTK_LABEL(app.label_status),(const char*)d);g_free(d);return G_SOURCE_REMOVE;}
static void set_status(const char*s){g_idle_add(status_cb,g_strdup(s));}

/*──────────────────────
  VIDEO helpers
──────────────────────*/
static void *send_video(void *) {
    const int W = 640, H = 360, FPS = 30; // 解像度とFPSを設定
    cv::VideoCapture cap(0);
    if (!cap.isOpened() || !init_encoder(W, H, FPS)) return nullptr;

    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);

    AVFrame *frame = av_frame_alloc();
    frame->format = enc_ctx->pix_fmt;
    frame->width = enc_ctx->width;
    frame->height = enc_ctx->height;
    av_frame_get_buffer(frame, 32);

    AVPacket *pkt = av_packet_alloc();
    int64_t pts = 0;

    cv::Mat bgr;
    auto period = std::chrono::milliseconds(1000 / FPS); // FPS制限
    while (cli_sock_video >= 0) {
        auto t0 = std::chrono::steady_clock::now();
        cap >> bgr;
        if (bgr.empty()) break;

        // BGRからRGBに変換
        cv::cvtColor(bgr, bgr, cv::COLOR_BGR2RGB);

        // BGR → YUV420P
        const uint8_t *bgr_data[1] = {bgr.data};
        int bgr_stride[1] = {static_cast<int>(bgr.step)};
        sws_scale(sws_ctx, bgr_data, bgr_stride, 0, H,
                  frame->data, frame->linesize);

        frame->pts = pts++; // 1フレーム進める

        // エンコード
        avcodec_send_frame(enc_ctx, frame);
        while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
            uint32_t n = htonl(pkt->size);
            if (send(cli_sock_video, &n, 4, 0) <= 0) goto finish;
            if (send(cli_sock_video, pkt->data, pkt->size, 0) <= 0) goto finish;
            av_packet_unref(pkt);
        }

        std::this_thread::sleep_until(t0 + period); // FPS制限
    }
finish:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc_ctx);
    sws_freeContext(sws_ctx);
    return nullptr;
}

static gboolean update_peer_image(gpointer data)
{
    // data == GdkPixbuf*
    GtkImage *img = GTK_IMAGE(app.image_peer);
    gtk_image_set_from_pixbuf(img, (GdkPixbuf*)data);
    g_object_unref(G_OBJECT(data));
    return G_SOURCE_REMOVE;
}
static void *receive_video(void*)
{
    /* 1) デコーダ初期化（最初の 1 回だけ） */
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    dec_ctx = avcodec_alloc_context3(dec);
    avcodec_open2(dec_ctx, dec, nullptr);

    AVPacket *pkt  = av_packet_alloc();
    AVFrame  *yuv  = av_frame_alloc();
    AVFrame  *bgr  = av_frame_alloc();               // RGB/BGR 変換用

    const int W = 640, H = 480;
    dec_sws = sws_getContext(W, H, AV_PIX_FMT_YUV420P,
                             W, H, AV_PIX_FMT_BGR24,
                             SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);

    /* BGR フレーム用バッファ確保 */
    bgr->format = AV_PIX_FMT_BGR24;
    bgr->width  = W;
    bgr->height = H;
    av_frame_get_buffer(bgr, 32);

    std::vector<uint8_t> buf;
    for (;;) {
        /* 2) 封入長を読んでパケットを受信 */
        uint32_t len_n;
        if (recv(cli_sock_video,&len_n,4,MSG_WAITALL) <= 0) break;
        uint32_t len = ntohl(len_n);
        buf.resize(len);
        ssize_t r = 0;
        while (r < (ssize_t)len) {
            ssize_t n = recv(cli_sock_video, buf.data()+r, len-r, 0);
            if (n <= 0) goto finish;
            r += n;
        }

        /* 3) H.264 パケット → AVPacket へ詰める */
        av_packet_unref(pkt);
        pkt->data = buf.data();
        pkt->size = buf.size();

        /* 4) デコード */
        if (avcodec_send_packet(dec_ctx, pkt) < 0) continue;
        while (avcodec_receive_frame(dec_ctx, yuv) == 0) {
            /* 5) YUV420P → BGR */
            sws_scale(dec_sws,
                      yuv->data, yuv->linesize, 0, H,
                      bgr->data, bgr->linesize);

            /* 6) BGR → GdkPixbuf (RGB 順) */
            GdkPixbuf *pix = gdk_pixbuf_new_from_data(
                bgr->data[0], GDK_COLORSPACE_RGB, FALSE, 8,
                W, H, bgr->linesize[0],
                nullptr, nullptr);          // ChatGPT では自前解放しない
            g_idle_add(update_peer_image, pix);
        }
    }
finish:
    av_frame_free(&bgr);
    av_frame_free(&yuv);
    av_packet_free(&pkt);
    sws_freeContext(dec_sws);
    avcodec_free_context(&dec_ctx);
    return nullptr;
}

/*──────────────────────
  AUDIO helpers (以前と同じ)
──────────────────────*/
static void *send_audio(void*){
    char b[4096]; ssize_t n;
    while((n=fread(b,1,sizeof(b),rec_stream))>0){ if(cli_sock_audio<0)break; if(send(cli_sock_audio,b,n,0)<=0)break; }
    return NULL;
}
static void *receive_audio(void*){
    char b[4096]; ssize_t n; FILE* play=popen("play -t raw -b 16 -c 1 -e s -r 44100 -","w");
    while(cli_sock_audio>=0 && (n=recv(cli_sock_audio,b,sizeof(b),0))>0){ fwrite(b,1,n,play);} pclose(play); return NULL; }

/*──────────────────────
  NETWORK server/client
──────────────────────*/
static int open_listen(int port){int s=socket(AF_INET,SOCK_STREAM,0); struct sockaddr_in a={0}; a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=INADDR_ANY; bind(s,(struct sockaddr*)&a,sizeof(a)); listen(s,1); return s;}
static int open_connect(const char*ip,int port){int s=socket(AF_INET,SOCK_STREAM,0); struct sockaddr_in a={0}; a.sin_family=AF_INET;a.sin_port=htons(port); inet_pton(AF_INET,ip,&a.sin_addr); connect(s,(struct sockaddr*)&a,sizeof(a)); return s;}

static void run_server(const char *port){int p=atoi(port);
    srv_sock_audio=open_listen(p);
    srv_sock_video=open_listen(p+1);
    cli_sock_audio=accept(srv_sock_audio,NULL,NULL);
    cli_sock_video=accept(srv_sock_video,NULL,NULL);
    rec_stream=popen("rec -t raw -b 16 -c 1 -e s -r 44100 -","r");
    pthread_t ta,tr,tv_send,tv_recv;
    pthread_create(&ta,NULL,send_audio,NULL);
    pthread_create(&tr,NULL,receive_audio,NULL);
    pthread_create(&tv_send,NULL,send_video,NULL);
    pthread_create(&tv_recv,NULL,receive_video,NULL);
    pthread_join(ta,NULL); pthread_join(tr,NULL); pthread_join(tv_send,NULL); pthread_join(tv_recv,NULL);
}
static void run_client(const char *ip,const char *port){int p=atoi(port);
    cli_sock_audio=open_connect(ip,p);
    cli_sock_video=open_connect(ip,p+1);
    rec_stream=popen("rec -t raw -b 16 -c 1 -e s -r 44100 -","r");
    pthread_t ta,tr,tv_send,tv_recv;
    pthread_create(&ta,NULL,send_audio,NULL);
    pthread_create(&tr,NULL,receive_audio,NULL);
    pthread_create(&tv_send,NULL,send_video,NULL);
    pthread_create(&tv_recv,NULL,receive_video,NULL);
    pthread_join(ta,NULL); pthread_join(tr,NULL); pthread_join(tv_send,NULL); pthread_join(tv_recv,NULL);
}

/*──────────────────────
  GTK UI
──────────────────────*/
static GtkWidget* build_ui(){
    GtkWidget* win=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win),"AV Chat (GTK)"); gtk_window_set_default_size(GTK_WINDOW(win),480,360);
    GtkWidget* grid=gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid),6); gtk_container_set_border_width(GTK_CONTAINER(grid),12);
    gtk_container_add(GTK_CONTAINER(win),grid);
    GtkWidget* radio_srv=gtk_radio_button_new_with_label(NULL,"Server"); GtkWidget* radio_cli=gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radio_srv),"Client");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_srv),TRUE); app.radio_server=radio_srv;
    gtk_grid_attach(GTK_GRID(grid),radio_srv,0,0,1,1); gtk_grid_attach(GTK_GRID(grid),radio_cli,1,0,1,1);

    GtkWidget* lbl_ip=gtk_label_new("IP:"); GtkWidget* entry_ip=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_ip),"127.0.0.1"); app.entry_ip=entry_ip;
    GtkWidget* lbl_port=gtk_label_new("Port:"); GtkWidget* entry_port=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port),"5555"); app.entry_port=entry_port;
    gtk_grid_attach(GTK_GRID(grid),lbl_ip,0,1,1,1); gtk_grid_attach(GTK_GRID(grid),entry_ip,1,1,2,1);
    gtk_grid_attach(GTK_GRID(grid),lbl_port,0,2,1,1); gtk_grid_attach(GTK_GRID(grid),entry_port,1,2,2,1);

    GtkWidget* btn_start=gtk_button_new_with_label("Start"); GtkWidget* btn_stop=gtk_button_new_with_label("Stop");
    gtk_grid_attach(GTK_GRID(grid),btn_start,0,3,1,1); gtk_grid_attach(GTK_GRID(grid),btn_stop,1,3,1,1);

    GtkWidget* lbl=gtk_label_new("idle"); app.label_status=lbl; gtk_grid_attach(GTK_GRID(grid),lbl,0,4,3,1);

    GtkWidget* image_peer=gtk_image_new_from_icon_name("camera-web",GTK_ICON_SIZE_DIALOG); app.image_peer=image_peer;
    gtk_grid_attach(GTK_GRID(grid),gtk_label_new("Peer video:"),0,5,1,1); gtk_grid_attach(GTK_GRID(grid),image_peer,1,5,2,1);

    g_signal_connect(btn_start,"clicked",G_CALLBACK(+[](GtkButton*,gpointer){ if(app.running)return; const char*port=gtk_entry_get_text(GTK_ENTRY(app.entry_port)); if(strlen(port)==0){set_status("port?");return;} app.running=TRUE; pthread_create(&app.worker,NULL,+[](void*)->void*{ gboolean is_srv=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_server)); const char*ip=gtk_entry_get_text(GTK_ENTRY(app.entry_ip)); const char*port=gtk_entry_get_text(GTK_ENTRY(app.entry_port)); if(is_srv){set_status("server waiting…"); run_server(port);} else {set_status("client connecting…"); run_client(ip,port);} set_status("finished"); app.running=FALSE; return NULL;},NULL); }),NULL);
    g_signal_connect(btn_stop,"clicked",G_CALLBACK(+[](GtkButton*,gpointer){ if(!app.running)return; if(cli_sock_audio>0){shutdown(cli_sock_audio,SHUT_RDWR);close(cli_sock_audio);cli_sock_audio=-1;} if(cli_sock_video>0){shutdown(cli_sock_video,SHUT_RDWR);close(cli_sock_video);cli_sock_video=-1;} if(srv_sock_audio>0){close(srv_sock_audio);srv_sock_audio=-1;} if(srv_sock_video>0){close(srv_sock_video);srv_sock_video=-1;} if(rec_stream){pclose(rec_stream);rec_stream=NULL;} pthread_join(app.worker,NULL); app.running=FALSE; set_status("stopped"); }),NULL);

    return win;
}

int main(int argc,char**argv){ gtk_init(&argc,&argv); GtkWidget*win=build_ui(); g_signal_connect(win,"destroy",G_CALLBACK(gtk_main_quit),NULL); gtk_widget_show_all(win); gtk_main(); return 0; }
