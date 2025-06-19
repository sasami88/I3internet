//sever-side socket API

/*
1. make socket
2. bind socket
3. listen socket
4. accept connection
5. read/write data
6. close socket
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

// グローバル変数
int client_socket;
FILE *rec_stream;

// 音声データを送信するスレッド
void *send_audio(void *arg) {
    char buf[1024];
    ssize_t n;

    while ((n = fread(buf, 1, sizeof(buf), rec_stream)) > 0) { // rec_streamから音声データを取得
        if (send(client_socket, buf, n, 0) < 0) { // クライアントに送信
            perror("データ送信に失敗しました");
            break;
        }
    }

    return NULL;
}

// 音声データを受信するスレッド
void *receive_audio(void *arg) {
    char recv_buf[1024];
    ssize_t recv_n;

    // playコマンドを実行して音声を再生
    FILE *play_stream = popen("play -t raw -b 16 -c 1 -e s -r 44100 -", "w");
    if (play_stream == NULL) {
        perror("playコマンドの実行に失敗しました");
        return NULL;
    }

    while ((recv_n = recv(client_socket, recv_buf, sizeof(recv_buf), 0)) > 0) { // クライアントから受信
        fwrite(recv_buf, 1, recv_n, play_stream); // playコマンドにplay_streamでデータを渡して再生
    }

    if (recv_n < 0) {
        perror("受信に失敗しました");
    } else if (recv_n == 0) {
        printf("接続が終了しました\n");
    }

    // playコマンドのストリームを閉じる
    pclose(play_stream);

    return NULL;
}

// サーバーとして動作する関数
void run_server(const char *port) {
    printf("サーバーモードで実行しています。\n ポート %s で待機中...\n", port);

    // ソケット作成
    int server_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("ソケット作成に失敗しました");
        exit(EXIT_FAILURE);
    }

    // ソケットのバインド
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bindに失敗しました");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // 接続待機
    if (listen(server_socket, 10) < 0) {
        perror("listenに失敗しました");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // 接続受け入れ
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &len);
    if (client_socket < 0) {
        perror("acceptに失敗しました");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    printf("クライアントが接続しました\n");

    // 音声データの録音開始
    rec_stream = popen("rec -t raw -b 16 -c 1 -e s -r 44100 -", "r");
    if (rec_stream == NULL) {
        perror("recコマンドの実行に失敗しました");
        close(client_socket);
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // スレッドの作成
    pthread_t send_thread, receive_thread;
    pthread_create(&send_thread, NULL, send_audio, NULL);
    pthread_create(&receive_thread, NULL, receive_audio, NULL);

    // スレッドの終了を待機
    pthread_join(send_thread, NULL);
    pthread_join(receive_thread, NULL);

    // リソースの解放
    pclose(rec_stream);
    close(client_socket);
    close(server_socket);
}

// クライアントとして動作する関数
void run_client(const char *ip, const char *port) {
    printf("クライアントモードで実行しています。\n サーバー %s:%s に接続中...\n", ip, port);

    // ソケット作成
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("ソケット作成に失敗しました");
        exit(EXIT_FAILURE);
    }

    // サーバーのアドレス設定
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(port));
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("無効なIPアドレスです");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // サーバーに接続
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("サーバーへの接続に失敗しました");
        close(sock);
        exit(EXIT_FAILURE);
    }
    printf("サーバーに接続しました\n");

    // recコマンドを起動して録音データを送信
    FILE *rec_stream = popen("rec -t raw -b 16 -c 1 -e s -r 44100 -", "r");
    if (rec_stream == NULL) {
        perror("recコマンドの実行に失敗しました");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // 録音データを送信し続ける
    char buf[1024];
    ssize_t n;
    while ((n = fread(buf, 1, sizeof(buf), rec_stream)) > 0) {
        if (send(sock, buf, n, 0) < 0) {
            perror("データ送信に失敗しました");
            pclose(rec_stream);
            close(sock);
            exit(EXIT_FAILURE);
        }
    }
    if (n < 0) {
        perror("録音データの読み取りに失敗しました");
    }

    // リソースの解放
    pclose(rec_stream);
    close(sock);
}

int main(int argc, char **argv) {
    if (argc == 2) {
        // サーバーモード
        run_server(argv[1]);
    } else if (argc == 3) {
        // クライアントモード
        run_client(argv[1], argv[2]);
    } else {
        printf("Usage:\n");
        printf("  サーバーモード: %s <ポート番号>\n", argv[0]);
        printf("  クライアントモード: %s <サーバーIP> <ポート番号>\n", argv[0]);
        return -1;
    }

    return 0;
}