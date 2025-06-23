// audio_video_chat_gui.c
// Bidirectional **audio + webcam video** chat with GTK GUI
// ---------------------------------------------------------
// Build (needs GTK3, pthread, OpenCV, SoX, FFmpeg libjpeg):
//   g++ -std=c++17 audio_video_chat_gui.c -o av_chat_gui \
//      $(pkg-config --cflags --libs gtk+-3.0 opencv4) -lpthread
//   # SoX は rec/play 用、OpenCV はカメラ＆JPEG圧縮用
// 2025‑06‑19  (minimal demo)

#include <gtk/gtk.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <opencv2/opencv.hpp>

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
static void *send_video(void*){
    cv::VideoCapture cap(0);
    if(!cap.isOpened()) return NULL;

    // 解像度を設定 (640x360)
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 360);

    cv::Mat frame; std::vector<uchar> buf;
    while(cli_sock_video>=0){
        cap >> frame; if(frame.empty()) break;
        cv::imencode(".jpg", frame, buf, {cv::IMWRITE_JPEG_QUALITY, 50}); // JPEG品質を50に設定
        uint32_t len = htonl(buf.size());
        if(send(cli_sock_video, &len, 4, 0) <= 0) break;
        if(send(cli_sock_video, buf.data(), buf.size(), 0) <= 0) break;

        // FPSを制限 (約30fps)
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    return NULL;
}
static gboolean update_peer_image(gpointer data){
    GdkPixbufLoader* loader=(GdkPixbufLoader*)data;
    GdkPixbuf* pix=gdk_pixbuf_loader_get_pixbuf(loader);
    if(pix) gtk_image_set_from_pixbuf(GTK_IMAGE(app.image_peer),pix);
    g_object_unref(loader);
    return G_SOURCE_REMOVE;
}
static void *receive_video(void*){
    while(cli_sock_video>=0){
        uint32_t len_n; if(recv(cli_sock_video,&len_n,4,MSG_WAITALL)<=0) break;
        uint32_t len=ntohl(len_n);
        std::vector<char> buf(len);
        ssize_t r=0; while(r<len){ssize_t n=recv(cli_sock_video,buf.data()+r,len-r,0); if(n<=0){len=0;break;} r+=n;}
        if(len==0) break;
        GdkPixbufLoader* loader=gdk_pixbuf_loader_new();
        gdk_pixbuf_loader_write(loader,(const guchar*)buf.data(),buf.size(),NULL);
        gdk_pixbuf_loader_close(loader,NULL);
        g_idle_add(update_peer_image,loader);
    }
    return NULL;
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
static int open_connect(const char* ip, int port){
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    connect(s, (struct sockaddr*)&a, sizeof(a));

    // Nagleアルゴリズムを無効化
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return s;
}

static int open_listen(int port){
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr*)&a, sizeof(a));
    listen(s, 1);

    // Nagleアルゴリズムを無効化
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return s;
}

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