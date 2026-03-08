#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define TIMEOUT_US 50000   // 50 ms timeout

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int epoll_fd;
    int socket_fd;
    struct sockaddr_in server_addr;

    int window_size;
    long tx_cnt;      // original transmissions only
    long rx_cnt;      // ACKs received
    long lost_cnt;    // stays 0 in Task 2 (we recover via GBN)

    struct {
        int seq;
        long long send_time;
        int acked;
    } window[1024];

} client_thread_data_t;

long long now_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];

    // Register socket with epoll
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event);

    // Go-Back-N state
    int base = 0;                       // first unACKed packet
    int next_seq = 0;                   // next packet to send
    int window_size = data->window_size;

    while (data->rx_cnt < num_requests) {

        long long now = now_us();

        // ----------------------------------------------------
        // 1. SEND PHASE (send new packets if window not full)
        // ----------------------------------------------------
        while (next_seq < num_requests &&
               next_seq < base + window_size) {

            struct {
                int seq;
                char payload[MESSAGE_SIZE];
            } pkt;

            pkt.seq = next_seq;
            memcpy(pkt.payload, "ABCDEFGHIJKMLNOP", MESSAGE_SIZE);

            sendto(data->socket_fd, &pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&data->server_addr,
                   sizeof(data->server_addr));

            // Count only original transmissions
            data->tx_cnt++;

            int idx = next_seq % window_size;
            data->window[idx].seq = next_seq;
            data->window[idx].send_time = now_us();
            data->window[idx].acked = 0;

            next_seq++;
        }

        // ----------------------------------------------------
        // 2. RECEIVE PHASE (ACKs)
        // ----------------------------------------------------
        int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 0);

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == data->socket_fd) {

                struct {
                    int seq;
                    char payload[MESSAGE_SIZE];
                } ack_pkt;

                socklen_t addrlen = sizeof(data->server_addr);

                int n = recvfrom(data->socket_fd, &ack_pkt, sizeof(ack_pkt), 0,
                                 (struct sockaddr *)&data->server_addr,
                                 &addrlen);
                if (n <= 0) continue;

                int ack = ack_pkt.seq;

                // Ignore ACKs outside current window
                if (ack < base || ack >= next_seq)
                    continue;

                int idx = ack % window_size;
                if (!data->window[idx].acked) {
                    data->window[idx].acked = 1;
                    data->rx_cnt++;
                }

                // Slide base forward while contiguous ACKs exist
                while (base < next_seq &&
                       data->window[base % window_size].acked) {
                    base++;
                }
            }
        }

        // ----------------------------------------------------
        // 3. TIMEOUT PHASE (Go-Back-N retransmission)
        // ----------------------------------------------------
        if (base < next_seq) {
            int idx = base % window_size;
            long long sent_time = data->window[idx].send_time;

            if (now - sent_time > TIMEOUT_US) {

                // Timeout on base → retransmit ALL unACKed packets
                for (int seq = base; seq < next_seq; seq++) {
                    int w = seq % window_size;

                    if (data->window[w].acked)
                        continue;

                    struct {
                        int seq;
                        char payload[MESSAGE_SIZE];
                    } pkt;

                    pkt.seq = seq;
                    memcpy(pkt.payload, "ABCDEFGHIJKMLNOP", MESSAGE_SIZE);

                    sendto(data->socket_fd, &pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&data->server_addr,
                           sizeof(data->server_addr));

                    // Do NOT increment tx_cnt for retransmissions
                    data->window[w].send_time = now_us();
                }
            }
        }
    }

    printf("Thread done: tx=%ld rx=%ld lost=%ld\n",
           data->tx_cnt, data->rx_cnt, data->lost_cnt);

    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        thread_data[i].server_addr = server_addr;

        thread_data[i].window_size = 10;   // adjust for experiments
        thread_data[i].tx_cnt = 0;
        thread_data[i].rx_cnt = 0;
        thread_data[i].lost_cnt = 0;

        for (int j = 0; j < 1024; j++) {
            thread_data[i].window[j].acked = 1;
            thread_data[i].window[j].send_time = 0;
            thread_data[i].window[j].seq = 0;
        }
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
}
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    long total_tx = 0, total_rx = 0, total_lost = 0;

    for (int i = 0; i < num_client_threads; i++) {
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_lost += thread_data[i].lost_cnt;
    }

    printf("\n=== TOTAL RESULTS ===\n");
    printf("TX:   %ld\n", total_tx);
    printf("RX:   %ld\n", total_rx);
    printf("LOST: %ld\n", total_lost);
}

void run_server() {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    int epoll_fd = epoll_create1(0);
    struct epoll_event event, events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_fd) {

                struct {
                    int seq;
                    char payload[MESSAGE_SIZE];
                } pkt;

                struct sockaddr_in client_addr;
                socklen_t addrlen = sizeof(client_addr);

                int n = recvfrom(server_fd, &pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&client_addr, &addrlen);

                if (n > 0) {
                    // Echo back as ACK (includes seq)
                    sendto(server_fd, &pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&client_addr, addrlen);
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        run_server();

    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);
        run_client();

    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
