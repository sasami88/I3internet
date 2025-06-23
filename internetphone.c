// audio_chat_gui.c
// Bidirectional audio chat with GTK GUI – Stop button now cleanly stops sockets and threads.
// Compile:
//    gcc audio_chat_gui.c -o audio_chat_gui $(pkg-config --cflags --libs gtk+-3.0) -lpthread
// Needs SoX (rec/play).
// 2025‑06‑19

#include <gtk/gtk.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "ffmpeg_video.h"

// ──────────────────────────────────────────────────
//   Forward‑declared networking helpers
// ──────────────────────────────────────────────────
static void run_server(const char *port);
static void run_client(const char *ip, const char *port);

// ──────────────────────────────────────────────────
//   State shared between GTK and the worker thread
// ──────────────────────────────────────────────────
typedef struct {
    GtkWidget *entry_ip;
    GtkWidget *entry_port;
    GtkWidget *radio_server;
    GtkWidget *label_status;
    pthread_t  worker;
    gboolean   running;
} App;

static App app = {0};

// ──────────────────────────────────────────────────
//   Networking globals to allow clean shutdown
// ──────────────────────────────────────────────────
static int server_socket_fd = -1;   // listening socket (server mode)
static int client_socket_fd = -1;   // connected socket (both modes)
static FILE *rec_stream = NULL;

// ──────────────────────────────────────────────────
//   Thread‑safe status label helper
// ──────────────────────────────────────────────────
static gboolean status_idle_cb(gpointer data) {
    const char *text = (const char *)data;
    gtk_label_set_text(GTK_LABEL(app.label_status), text);
    g_free(data);
    return G_SOURCE_REMOVE;
}
static void set_status_async(const char *msg) {
    g_idle_add(status_idle_cb, g_strdup(msg));
}

// ──────────────────────────────────────────────────
//   Worker thread – launches server or client
// ──────────────────────────────────────────────────
static void *worker_thread(void *arg) {
    gboolean is_server = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_server));
    const char *ip   = gtk_entry_get_text(GTK_ENTRY(app.entry_ip));
    const char *port = gtk_entry_get_text(GTK_ENTRY(app.entry_port));

    if (is_server) {
        set_status_async("[server] waiting for connection …");
        run_server(port);
    } else {
        set_status_async("[client] connecting …");
        run_client(ip, port);
    }

    set_status_async("finished (you may start again)");
    app.running = FALSE;
    return NULL;
}

// ──────────────────────────────────────────────────
//   GTK callbacks
// ──────────────────────────────────────────────────
static void on_start_clicked(GtkButton *btn, gpointer user_data) {
    if (app.running) return;

    const char *port = gtk_entry_get_text(GTK_ENTRY(app.entry_port));
    if (strlen(port) == 0) {
        set_status_async("port is required");
        return;
    }

    app.running = TRUE;
    pthread_create(&app.worker, NULL, worker_thread, NULL);
}

static void on_stop_clicked(GtkButton *btn, gpointer user_data) {
    if (!app.running) return;

    // Gracefully close sockets to unblock threads
    if (client_socket_fd > 0) {
        shutdown(client_socket_fd, SHUT_RDWR);
        close(client_socket_fd);
        client_socket_fd = -1;
    }
    if (server_socket_fd > 0) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }

    // Close recording stream (will make fread return 0)
    if (rec_stream) {
        pclose(rec_stream);
        rec_stream = NULL;
    }

    // Wait for worker thread to finish
    pthread_join(app.worker, NULL);
    app.running = FALSE;
    set_status_async("stopped by user");
}

// ──────────────────────────────────────────────────
//   UI builder
// ──────────────────────────────────────────────────
static GtkWidget *build_ui(void) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Audio Chat (C / GTK)");
    gtk_window_set_default_size(GTK_WINDOW(win), 370, 170);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(win), grid);

    GtkWidget *radio_srv = gtk_radio_button_new_with_label(NULL, "Server");
    GtkWidget *radio_cli = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radio_srv), "Client");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_srv), TRUE);
    app.radio_server = radio_srv;

    gtk_grid_attach(GTK_GRID(grid), radio_srv, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), radio_cli, 1, 0, 1, 1);

    GtkWidget *lbl_ip = gtk_label_new("IP:");
    GtkWidget *entry_ip = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_ip), "127.0.0.1");
    app.entry_ip = entry_ip;
    gtk_grid_attach(GTK_GRID(grid), lbl_ip,   0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_ip, 1, 1, 2, 1);

    GtkWidget *lbl_port = gtk_label_new("Port:");
    GtkWidget *entry_port = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port), "5555");
    app.entry_port = entry_port;
    gtk_grid_attach(GTK_GRID(grid), lbl_port,   0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_port, 1, 2, 2, 1);

    GtkWidget *btn_start = gtk_button_new_with_label("Start");
    GtkWidget *btn_stop  = gtk_button_new_with_label("Stop");
    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    g_signal_connect(btn_stop,  "clicked", G_CALLBACK(on_stop_clicked),  NULL);
    gtk_grid_attach(GTK_GRID(grid), btn_start, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_stop,  1, 3, 1, 1);

    GtkWidget *lbl_status = gtk_label_new("idle");
    app.label_status = lbl_status;
    gtk_grid_attach(GTK_GRID(grid), lbl_status, 0, 4, 3, 1);

    return win;
}

// ──────────────────────────────────────────────────
//   Networking helpers
// ──────────────────────────────────────────────────
static void *send_audio(void *arg) {
    char buf[4096];
    ssize_t n;
    while ((n = fread(buf, 1, sizeof(buf), rec_stream)) > 0) {
        if (client_socket_fd < 0) break;           // socket closed externally
        if (send(client_socket_fd, buf, n, 0) < 0) break;
    }
    return NULL;
}

static void *receive_audio(void *arg) {
    char buf[4096];
    ssize_t n;
    FILE *play_stream = popen("play -t raw -b 16 -c 1 -e s -r 44100 -", "w");
    if (!play_stream) return NULL;
    while (client_socket_fd >= 0 && (n = recv(client_socket_fd, buf, sizeof(buf), 0)) > 0) {
        fwrite(buf, 1, n, play_stream);
    }
    pclose(play_stream);
    return NULL;
}

static void run_server(const char *port) {
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) return;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(atoi(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return;
    if (listen(server_socket_fd, 1) < 0) return;

    client_socket_fd = accept(server_socket_fd, NULL, NULL);
    if (client_socket_fd < 0) return;             // stopped before connect

    rec_stream = popen("rec -t raw -b 16 -c 1 -e s -r 44100 -", "r");
    pthread_t th_send, th_recv;
    pthread_create(&th_send, NULL, send_audio, NULL);
    pthread_create(&th_recv, NULL, receive_audio, NULL);
    pthread_join(th_send, NULL);
    pthread_join(th_recv, NULL);

    if (rec_stream) { pclose(rec_stream); rec_stream = NULL; }
    if (client_socket_fd >= 0) { close(client_socket_fd); client_socket_fd = -1; }
    if (server_socket_fd >= 0) { close(server_socket_fd); server_socket_fd = -1; }
}

static void run_client(const char *ip, const char *port) {
    client_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket_fd < 0) return;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(atoi(port));
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(client_socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return;

    rec_stream = popen("rec -t raw -b 16 -c 1 -e s -r 44100 -", "r");
    pthread_t th_send, th_recv;
    pthread_create(&th_send, NULL, send_audio, NULL);
    pthread_create(&th_recv, NULL, receive_audio, NULL);
    pthread_join(th_send, NULL);
    pthread_join(th_recv, NULL);

    if (rec_stream) { pclose(rec_stream); rec_stream = NULL; }
    if (client_socket_fd >= 0) { close(client_socket_fd); client_socket_fd = -1; }
}

// 映像送信スレッド
void *send_video(void *arg) {
    // FFmpegの初期化とエンコード処理を呼び出す
}

// 映像受信スレッド
void *receive_video(void *arg) {
    // FFmpegの初期化とデコード処理を呼び出す
}

// ──────────────────────────────────────────────────
//   main()
// ──────────────────────────────────────────────────
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkWidget *win = build_ui();
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(win);

    gtk_main();
    return 0;
}

