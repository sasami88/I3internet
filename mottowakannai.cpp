// audio_video_chat_gui.cpp
// Bidirectional **audio + webcam video** chat with GTK GUI (ring‑buffer + non‑block I/O 改良版)
// -----------------------------------------------------------------------------
// Build (GTK3, pthread, OpenCV, SoX, FFmpeg libjpeg, Linux epoll):
//   g++ -std=c++17 audio_video_chat_gui.cpp -o av_chat_gui \
//       $(pkg-config --cflags --libs gtk+-3.0 opencv4) -pthread
//
// 2025‑06‑23  fully‑integrated demo

#include <gtk/gtk.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <opencv2/opencv.hpp>
#include <netinet/tcp.h>   // <- add for TCP_NODELAY
#include <errno.h>         // <- add for errno

//───────────────────────
// CONFIGURATION
//───────────────────────
#define VIDEO_W           640
#define VIDEO_H           360
#define JPEG_QUALITY      50
#define VIDEO_FPS         30
#define AUDIO_RATE        44100
#define AUDIO_FMT_BYTES   2          // 16‑bit
#define AUDIO_CHANNELS    1
#define AUDIO_PKT_NS      20         // 20ms per packet
#define AUDIO_PKT_BYTES   (AUDIO_RATE/1000*AUDIO_PKT_NS*AUDIO_FMT_BYTES)

#define VB_SIZE  32     // video ring buffer (must be 2^n)
#define AB_TX_SIZE 64   // audio TX buffer
#define AB_RX_SIZE 64   // audio RX jitter buffer

static void run_server(const char *port);
static void run_client(const char *ip,const char *port);

//───────────────────────
// Generic single‑producer single‑consumer lock‑free ring buffer
//───────────────────────

template<size_t N>
class RingBuf{
    static_assert((N&(N-1))==0,"N must be power of 2");
    char* buf[N];
    uint32_t len[N];
    std::atomic<uint32_t> head{0},tail{0};
public:
    bool push(char* p,uint32_t l){
        uint32_t h=head.load(std::memory_order_relaxed);
        uint32_t nt=(h+1)&(N-1);
        if(nt==tail.load(std::memory_order_acquire)) return false; // full
        buf[h]=p; len[h]=l;
        head.store(nt,std::memory_order_release);
        return true;
    }
    bool pop(char*& p,uint32_t &l){
        uint32_t t=tail.load(std::memory_order_relaxed);
        if(t==head.load(std::memory_order_acquire)) return false; // empty
        p=buf[t]; l=len[t];
        tail.store((t+1)&(N-1),std::memory_order_release);
        return true;
    }
    size_t count() const{
        int32_t h=head.load(std::memory_order_acquire);
        int32_t t=tail.load(std::memory_order_acquire);
        return (h - t + N) & (N-1);
    }
};

static RingBuf<VB_SIZE>   rb_v_tx;   // encoded JPEG → sender
static RingBuf<VB_SIZE>   rb_v_rx;   // received JPEG → viewer
static RingBuf<AB_TX_SIZE> rb_a_tx;  // raw audio pkt → sender
static RingBuf<AB_RX_SIZE> rb_a_rx;  // received audio pkt (jitter buf)

//───────────────────────
// GTK app struct
//───────────────────────
struct App{
    GtkWidget *entry_ip,*entry_port,*radio_server,*label_status,*image_peer;
    pthread_t  worker; gboolean running;
}app={0};

//───────────────────────
// Utility helpers
//───────────────────────
static gboolean status_cb(gpointer d){gtk_label_set_text(GTK_LABEL(app.label_status),(const char*)d);g_free(d);return G_SOURCE_REMOVE;}
static void set_status(const char*s){g_idle_add(status_cb,g_strdup(s));}

static void set_nonblock(int s){int fl=fcntl(s,F_GETFL,0);fcntl(s,F_SETFL,fl|O_NONBLOCK);} 
static void set_tcp_nodelay(int s){int one=1;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));}

static int open_listen(int port){int s=socket(AF_INET,SOCK_STREAM,0); struct sockaddr_in a={0}; a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=INADDR_ANY; bind(s,(struct sockaddr*)&a,sizeof(a)); listen(s,1);set_nonblock(s);return s;}
static int open_connect(const char*ip,int port){int s=socket(AF_INET,SOCK_STREAM,0); struct sockaddr_in a={0}; a.sin_family=AF_INET;a.sin_port=htons(port); inet_pton(AF_INET,ip,&a.sin_addr); connect(s,(struct sockaddr*)&a,sizeof(a)); set_nonblock(s); return s;}

// Real‑time priority helper
static void set_rt(int prio){
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}

//───────────────────────
// VIDEO threads
//───────────────────────
static void* thread_v_cap(void*) {
    set_rt(4);
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) return NULL;

    // 解像度を下げる
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 360);

    cv::Mat frame;
    std::vector<uchar> buf;
    auto period = std::chrono::milliseconds(1000 / 30); // 約30fps

    while (app.running) {
        auto t0 = std::chrono::steady_clock::now();
        cap >> frame;
        if (frame.empty()) continue;

        // BGRからRGBに変換
        cv::cvtColor(frame, frame, cv::COLOR_BGR2RGB);

        // JPEG品質を下げる
        cv::imencode(".jpg", frame, buf, {cv::IMWRITE_JPEG_QUALITY, 50});

        char* p = (char*)malloc(buf.size());
        memcpy(p, buf.data(), buf.size());
        if (!rb_v_tx.push(p, buf.size())) free(p); // バッファがいっぱいの場合は破棄

        std::this_thread::sleep_until(t0 + period); // 次のフレームまで待機
    }
    return NULL;
}

static bool send_full(int sock,const char*data,size_t len){
    size_t sent=0; while(sent<len){ssize_t n=send(sock,data+sent,len-sent,0); if(n<=0){if(errno==EAGAIN||errno==EWOULDBLOCK){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;} return false;} sent+=n;} return true;}


static void* thread_v_tx(void* arg) {
    int sock = *(int*)arg;
    set_rt(3);

    // Nagleアルゴリズムを無効化
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // TCPバッファサイズを縮小
    int buf_size = 16 * 1024; // 16KB
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    while (app.running) {
        char* p;
        uint32_t l;
        if (!rb_v_tx.pop(p, l)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        uint32_t ln = htonl(l);
        if (!send_full(sock, (char*)&ln, 4) || !send_full(sock, p, l)) {
            free(p);
            break;
        }
        free(p);
    }
    return NULL;
}

static void* thread_v_rx(void*arg){int sock=*(int*)arg; set_rt(2); set_tcp_nodelay(sock);
    uint32_t need_hdr=4; uint32_t pkt_len=0; std::vector<char> pkt;
    char hdr_buf[4]; size_t hdr_pos=0;
    while(app.running){
        // read header
        while(need_hdr){ssize_t n=recv(sock,hdr_buf+hdr_pos,need_hdr,0); if(n>0){need_hdr-=n;hdr_pos+=n;} else if(n==0){return NULL;} else if(errno==EAGAIN||errno==EWOULDBLOCK){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;} else return NULL;}
        if(need_hdr==0){pkt_len=ntohl(*(uint32_t*)hdr_buf); pkt.resize(pkt_len); size_t pos=0; while(pos<pkt_len){ssize_t n=recv(sock,pkt.data()+pos,pkt_len-pos,0); if(n>0){pos+=n;} else if(n==0){return NULL;} else if(errno==EAGAIN||errno==EWOULDBLOCK){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;} else return NULL;}
            char* p=(char*)malloc(pkt_len); memcpy(p,pkt.data(),pkt_len);
            rb_v_rx.push(p,pkt_len);
            need_hdr=4; hdr_pos=0;
        }
    }
    return NULL;
}

static gboolean gui_set_peer(gpointer data){GdkPixbufLoader*ldr=(GdkPixbufLoader*)data;GdkPixbuf*px=gdk_pixbuf_loader_get_pixbuf(ldr); if(px) gtk_image_set_from_pixbuf(GTK_IMAGE(app.image_peer),px); g_object_unref(ldr); return G_SOURCE_REMOVE;}

static void* thread_v_disp(void*){
    set_rt(1);
    while(app.running){
        char* p; uint32_t l;
        if(!rb_v_rx.pop(p,l)){std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
        GdkPixbufLoader*ldr=gdk_pixbuf_loader_new(); gdk_pixbuf_loader_write(ldr,(const guchar*)p,l,NULL); gdk_pixbuf_loader_close(ldr,NULL);
        g_idle_add(gui_set_peer,ldr);
        free(p);
    }
    return NULL;
}

//───────────────────────
// AUDIO threads
//───────────────────────
static void* thread_a_cap(void*){
    set_rt(20);
    FILE* rec=popen("rec -q -t raw -b 16 -c 1 -e s -r 44100 -","r");
    if(!rec) return NULL;
    char* buf=(char*)malloc(AUDIO_PKT_BYTES);
    while(app.running){
        size_t n=fread(buf,1,AUDIO_PKT_BYTES,rec);
        if(n!=AUDIO_PKT_BYTES) break;
        char* p=(char*)malloc(n); memcpy(p,buf,n);
        if(!rb_a_tx.push(p,n)) free(p);
    }
    free(buf); pclose(rec); return NULL;
}

static void* thread_a_tx(void*arg){int sock=*(int*)arg; set_rt(18); set_tcp_nodelay(sock);
    while(app.running){
        char* p; uint32_t l;
        if(!rb_a_tx.pop(p,l)){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
        if(!send_full(sock,p,l)){free(p);break;} free(p);
    }
    return NULL;
}

static void* thread_a_rx(void*arg){int sock=*(int*)arg; set_rt(18); set_tcp_nodelay(sock); char* buf=(char*)malloc(AUDIO_PKT_BYTES);
    while(app.running){
        ssize_t n=recv(sock,buf,AUDIO_PKT_BYTES,0);
        if(n==AUDIO_PKT_BYTES){char* p=(char*)malloc(n); memcpy(p,buf,n); rb_a_rx.push(p,n);} else if(n==0){break;} else if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK)){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;} else break;
    }
    free(buf); return NULL;
}

static void* thread_a_play(void*){
    set_rt(22);
    FILE* play=popen("play -q -t raw -b 16 -c 1 -e s -r 44100 -","w"); if(!play) return NULL;
    while(app.running){
        if(rb_a_rx.count()<3){std::this_thread::sleep_for(std::chrono::milliseconds(AUDIO_PKT_NS));continue;}
        char* p; uint32_t l; if(rb_a_rx.pop(p,l)){fwrite(p,1,l,play); free(p);} else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    pclose(play); return NULL;
}

//───────────────────────
// server / client orchestration
//───────────────────────
static void run_common(int sockA,int sockV){
    pthread_t vcap, vtx, vrx, vdisp, acap, atx, arx, aplay;
    app.running=true;
    pthread_create(&vcap ,NULL,thread_v_cap ,NULL);
    pthread_create(&vtx  ,NULL,thread_v_tx ,&sockV);
    pthread_create(&vrx  ,NULL,thread_v_rx ,&sockV);
    pthread_create(&vdisp,NULL,thread_v_disp,NULL);
    pthread_create(&acap ,NULL,thread_a_cap ,NULL);
    pthread_create(&atx  ,NULL,thread_a_tx ,&sockA);
    pthread_create(&arx  ,NULL,thread_a_rx ,&sockA);
    pthread_create(&aplay,NULL,thread_a_play,NULL);

    pthread_join(vcap ,NULL); pthread_join(vtx ,NULL); pthread_join(vrx ,NULL); pthread_join(vdisp,NULL);
    pthread_join(acap ,NULL); pthread_join(atx ,NULL); pthread_join(arx ,NULL); pthread_join(aplay,NULL);
}

static void run_server(const char*port){int p=atoi(port);
    int lsA=open_listen(p); int lsV=open_listen(p+1);
    set_status("server waiting …");
    int sockA=accept(lsA,NULL,NULL); int sockV=accept(lsV,NULL,NULL);
    set_nonblock(sockA); set_nonblock(sockV);
    run_common(sockA,sockV);
    close(sockA); close(sockV); close(lsA); close(lsV);
}

static void run_client(const char*ip,const char*port){int p=atoi(port);
    int sockA=open_connect(ip,p); int sockV=open_connect(ip,p+1);
    set_status("connected");
    run_common(sockA,sockV);
    close(sockA); close(sockV);
}

//───────────────────────
// GTK UI (ほぼ元のまま)
//───────────────────────
static GtkWidget* build_ui(){
    GtkWidget*win=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win),"AV Chat (GTK)"); gtk_window_set_default_size(GTK_WINDOW(win),480,360);
    GtkWidget*grid=gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid),6); gtk_container_set_border_width(GTK_CONTAINER(grid),12); gtk_container_add(GTK_CONTAINER(win),grid);
    GtkWidget*radio_srv=gtk_radio_button_new_with_label(NULL,"Server"); GtkWidget*radio_cli=gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radio_srv),"Client"); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_srv),TRUE); app.radio_server=radio_srv;
    gtk_grid_attach(GTK_GRID(grid),radio_srv,0,0,1,1); gtk_grid_attach(GTK_GRID(grid),radio_cli,1,0,1,1);
    GtkWidget*lbl_ip=gtk_label_new("IP:"); GtkWidget*entry_ip=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_ip),"127.0.0.1"); app.entry_ip=entry_ip;
    GtkWidget*lbl_port=gtk_label_new("Port:"); GtkWidget*entry_port=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port),"5555"); app.entry_port=entry_port;
    gtk_grid_attach(GTK_GRID(grid),lbl_ip,0,1,1,1); gtk_grid_attach(GTK_GRID(grid),entry_ip,1,1,2,1);
    gtk_grid_attach(GTK_GRID(grid),lbl_port,0,2,1,1); gtk_grid_attach(GTK_GRID(grid),entry_port,1,2,2,1);
    GtkWidget*btn_start=gtk_button_new_with_label("Start"); GtkWidget*btn_stop=gtk_button_new_with_label("Stop"); gtk_grid_attach(GTK_GRID(grid),btn_start,0,3,1,1); gtk_grid_attach(GTK_GRID(grid),btn_stop,1,3,1,1);
    GtkWidget*lbl=gtk_label_new("idle"); app.label_status=lbl; gtk_grid_attach(GTK_GRID(grid),lbl,0,4,3,1);
    GtkWidget*image_peer=gtk_image_new_from_icon_name("camera-web",GTK_ICON_SIZE_DIALOG); app.image_peer=image_peer; gtk_grid_attach(GTK_GRID(grid),gtk_label_new("Peer video:"),0,5,1,1); gtk_grid_attach(GTK_GRID(grid),image_peer,1,5,2,1);

    g_signal_connect(btn_start,"clicked",G_CALLBACK(+[](GtkButton*,gpointer){ if(app.running)return; const char*port=gtk_entry_get_text(GTK_ENTRY(app.entry_port)); if(strlen(port)==0){set_status("port?");return;} app.running=TRUE; pthread_create(&app.worker,NULL,+[](void*)->void*{ gboolean is_srv=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_server)); const char*ip=gtk_entry_get_text(GTK_ENTRY(app.entry_ip)); const char*port=gtk_entry_get_text(GTK_ENTRY(app.entry_port)); if(is_srv){run_server(port);} else {run_client(ip,port);} set_status("finished"); app.running=FALSE; return NULL;},NULL); }),NULL);

    g_signal_connect(btn_stop,"clicked",G_CALLBACK(+[](GtkButton*,gpointer){ if(!app.running)return; app.running=false; pthread_join(app.worker,NULL); set_status("stopped"); }),NULL);
    return win; }

int main(int argc,char**argv){ gtk_init(&argc,&argv); GtkWidget*win=build_ui(); g_signal_connect(win,"destroy",G_CALLBACK(gtk_main_quit),NULL); gtk_widget_show_all(win); gtk_main(); return 0; }
