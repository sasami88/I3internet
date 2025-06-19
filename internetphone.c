// audio_chat_gui.c
// A minimal GUI wrapper around the bidirectional audio chat program
// Uses GTK 3 in C.  Compile with:
//   gcc audio_chat_gui.c -o audio_chat_gui $(pkg-config --cflags --libs gtk+-3.0) -lpthread
// Requires SoX installed (rec/play commands).
// 2025‑06‑19

#include <gtk/gtk.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ──────────────────────────────────────────────────
//   Forward‑declared networking helpers (taken from
//   the original CLI sample, slightly refactored
//   so that we can run them in a worker thread).
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

// Update the status label safely from any thread
static gboolean set_status_idle_cb(gpointer data) {
    const char *text = (const char *)data;
    gtk_label_set_text(GTK_LABEL(app.label_status), text);
    return G_SOURCE_REMOVE;             // run only once
}
static void set_status_async(const char *msg) {
    g_idle_add(set_status_idle_cb, g_strdup(msg)); // copy msg for idle
}

// Worker thread entry point
static void *worker_thread(void *arg) {
    const gboolean is_server = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_server));
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

// GTK callback: start button
static void on_start_clicked(GtkButton *btn, gpointer user_data) {
    if (app.running) return;            // already running

    // Basic validation
    const char *port = gtk_entry_get_text(GTK_ENTRY(app.entry_port));
    if (strlen(port) == 0) {
        set_status_async("port is required");
        return;
    }

    app.running = TRUE;
    pthread_create(&app.worker, NULL, worker_thread, NULL);
}

// GTK callback: stop button (best‑effort — just cancels the process)
static void on_stop_clicked(GtkButton *btn, gpointer user_data) {
    if (!app.running) return;
    // For brevity we simply cancel the worker thread – in real apps you
    // would arrange a proper shutdown via a global flag and socket close.
    pthread_cancel(app.worker);
    pthread_join(app.worker, NULL);
    app.running = FALSE;
    set_status_async("stopped by user");
}

// Build a very small UI entirely in code
static GtkWidget *build_ui(void) {
    GtkWidget *win  = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Audio Chat (C / GTK)");
    gtk_window_set_default_size(GTK_WINDOW(win), 360, 160);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(win), grid);

    // Mode (server | client)
    GtkWidget *radio_server = gtk_radio_button_new_with_label(NULL, "Server");
    GtkWidget *radio_client = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radio_server), "Client");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_server), TRUE);
    app.radio_server = radio_server;

    gtk_grid_attach(GTK_GRID(grid), radio_server, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), radio_client, 1, 0, 1, 1);

    // IP entry (client only)
    GtkWidget *lbl_ip = gtk_label_new("IP:");
    GtkWidget *entry_ip = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_ip), "127.0.0.1");
    app.entry_ip = entry_ip;

    gtk_grid_attach(GTK_GRID(grid), lbl_ip,   0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_ip, 1, 1, 2, 1);

    // Port entry
    GtkWidget *lbl_port = gtk_label_new("Port:");
    GtkWidget *entry_port = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port), "5555");
    app.entry_port = entry_port;

    gtk_grid_attach(GTK_GRID(grid), lbl_port,   0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_port, 1, 2, 2, 1);

    // Start / Stop buttons
    GtkWidget *btn_start = gtk_button_new_with_label("Start");
    GtkWidget *btn_stop  = gtk_button_new_with_label("Stop");

    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    g_signal_connect(btn_stop,  "clicked", G_CALLBACK(on_stop_clicked),  NULL);

    gtk_grid_attach(GTK_GRID(grid), btn_start, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_stop,  1, 3, 1, 1);

    // Status label
    GtkWidget *lbl_status = gtk_label_new("idle");
    app.label_status = lbl_status;
    gtk_grid_attach(GTK_GRID(grid), lbl_status, 0, 4, 3, 1);

    return win;
}

// ──────────────────────────────────────────────────
//                 Original networking code
//   (slightly shortened; still uses SoX rec/play)
// ──────────────────────────────────────────────────
static int client_socket;
static FILE *rec_stream;

static void *send_audio(void *arg) {
    char buf[4096];
    ssize_t n;
    while ((n = fread(buf, 1, sizeof(buf), rec_stream)) > 0) {
        if (send(client_socket, buf, n, 0) < 0) break;
    }
    return NULL;
}
static void *receive_audio(void *arg) {
    char buf[4096];
    ssize_t n;
    FILE *play_stream = popen("play -t raw -b 16 -c 1 -e s -r 44100 -", "w");
    while ((n = recv(client_socket, buf, sizeof(buf), 0)) > 0) {
        fwrite(buf, 1, n, play_stream);
    }
    pclose(play_stream);
    return NULL;
}
static void run_server(const char *port) {
    int server_socket = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(atoi(port)), .sin_addr.s_addr = INADDR_ANY };
    bind(server_socket, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_socket, 1);
    client_socket = accept(server_socket, NULL, NULL);

    rec_stream = popen("rec -t raw -b 16 -c 1 -e s -r 44100 -", "r");
    pthread_t th_send, th_recv;
    pthread_create(&th_send, NULL, send_audio, NULL);
    pthread_create(&th_recv, NULL, receive_audio, NULL);
    pthread_join(th_send, NULL);
    pthread_join(th_recv, NULL);

    pclose(rec_stream);
    close(client_socket);
    close(server_socket);
}
static void run_client(const char *ip, const char *port) {
    client_socket = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(atoi(port)) };
    inet_pton(AF_INET, ip, &addr.sin_addr);
    connect(client_socket, (struct sockaddr *)&addr, sizeof(addr));

    rec_stream = popen("rec -t raw -b 16 -c 1 -e s -r 44100 -", "r");
    pthread_t th_send, th_recv;
    pthread_create(&th_send, NULL, send_audio, NULL);
    pthread_create(&th_recv, NULL, receive_audio, NULL);
    pthread_join(th_send, NULL);
    pthread_join(th_recv, NULL);

    pclose(rec_stream);
    close(client_socket);
}

// ──────────────────────────────────────────────────
//                        main()
// ──────────────────────────────────────────────────
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkWidget *win = build_ui();
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(win);

    gtk_main();
    return 0;
}
