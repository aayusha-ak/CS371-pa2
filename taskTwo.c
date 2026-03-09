/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: Aayusha Kandel
# Student #2: 
# Student #3: 

*/
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
//used to detect packet loss
//50,000 microseconds = 50ms
//if a response takes longer than 50ms, then it has to be marked lost
#define TIMEOUT_VALUE 50000   

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int epoll_fd;
    int socket_fd;
    struct sockaddr_in server_addr;

    // Pipeline fields
    int window_size; //max number of request allowed at once
    long tx_cnt; //total packets transmited
    long rx_cnt; //total responses received
    long lost_cnt; //total packets that are considered lost
    long next_seq; //sequence number for the next packet
    int in_flight; // number of requests sent that are not acknowledged yet
    int thread_id;
    //sliding window tracing - build to tracks each outstanding request
    struct {
        int seq_num; //sequence number of packet
        long long send_time; // time when packet was sent
        int acked; // Tracks whether response arrived
    } window[1024]; // safe upper bound 

} client_thread_data_t;

//Timer helper function
//Used for measuring packet delay and detecting timeout
long long current_time_helper() {
    struct timeval time_value; //stores current time (seconds and microseconds)
    gettimeofday(&time_value, NULL); //ask OS for current system time
    long long total_sec = time_value.tv_sec * 1000000LL + time_value.tv_usec; //convert seconds to microseconds and add remaining microseonds
    return total_sec;
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];

    // register socket with epoll
    event.events = EPOLLIN;// notify when socket is has incoming data to read
    event.data.fd = data->socket_fd; // store fd, to know which socket has to be monitored
    epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event); // add the socket to epoll, and notify when it beocmes readable

    
    long base = 0; // first unACKed packet
    data->next_seq = 0; // next packet to send
    int W = data->window_size; // window size

    // no packet lost, becasue all losses will be recovered by retransmission
    data->lost_cnt = 0;

    // Continue until all packets have been acknowledged
    while (data->rx_cnt < num_requests) {

        long long current_time = current_time_helper(); // current time for timeout tracking

        // Sending packets to window, until window is full
        while (data->next_seq < num_requests &&
               data->next_seq < base + W) {

            // packet structure
            struct {
                int seq_num; //sequence number
                char payload[MESSAGE_SIZE]; //data being send
            } pkt;

            pkt.seq_num = data->next_seq; //assign the unique sequence number

            // copy data into packet, to ensure paker has a fixed size
            memcpy(pkt.payload, "ABCDEFGHIJKMLNOP", MESSAGE_SIZE);

            // sending the packet to server
            sendto(data->socket_fd, &pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&data->server_addr,
                   sizeof(data->server_addr));

            // count only original transmissions
            data->tx_cnt++;

            // save packet info in window buffer
            int idx = pkt.seq_num % W;
            data->window[idx].seq_num = pkt.seq_num;
            data->window[idx].send_time = current_time_helper();
            data->window[idx].acked = 0; //waiting for ACK

            data->next_seq++; //go to next sequence number
        }

        //  Check if ACKs have arrived
    
        int num_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 0);//check to see if there are any sockets ready for reading?

        //loop throught the events
        for (int i = 0; i < num_events; i++) {
            //check if event belongs to the socket
            if (events[i].data.fd == data->socket_fd) {

                //structure to store recieved ACK packet, from the server
                struct {
                    int seq_num; //sequence number
                    char payload[MESSAGE_SIZE]; //data
                } ack_packet;

                socklen_t addrlen = sizeof(data->server_addr); //size of the address strucutre

                //read incoming ACK from sserver
                int n = recvfrom(data->socket_fd, &ack_packet, sizeof(ack_packet), 0,
                                 (struct sockaddr *)&data->server_addr,
                                 &addrlen);
                //skip, if nothing reads
                if (n <= 0) continue;

                int ack = ack_packet.seq_num;

                // ignore ACKs that fall outside the current window
                if (ack < base || ack >= data->next_seq)
                    continue;

                int index = ack % W; //circular buffer index

                //if packet not yer marked ACKed, mark it now
                if (!data->window[index].acked) {
                    data->window[index].acked = 1;
                    data->rx_cnt++; //count the ACK
                }

                // slide base forward while contiguous ACKs exist
                while (base < data->next_seq &&
                       data->window[base % W].acked) {
                    base++;
                }
            }
        }

        // TIMEOUT 
        //only base packet contols timwout.
        //if the base packe has waited too long wihtout an ACK,
        // then retransmit all unACKed packets in the window
        if (base < data->next_seq) { //only check timeout if window is not empty
            int index = base % W; //index of bas packet
            long long sent_time = data->window[index].send_time;

            //check if base packet timed out
            if (current_time - sent_time > TIMEOUT_VALUE) {

                //retransmit every unACKed packet in the window
                for (long seq = base; seq < data->next_seq; seq++) {
                    int w = seq % W; //circular index

                    //skip packets that are already ACKed
                    if (data->window[w].acked)
                        continue;

                    //rebuild packet for retransmission
                    struct {
                        int seq_num;
                        char payload[MESSAGE_SIZE];
                    } pkt;

                    pkt.seq_num = seq;
                    memcpy(pkt.payload, "ABCDEFGHIJKMLNOP", MESSAGE_SIZE);

                    //send retransmission
                    sendto(data->socket_fd, &pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&data->server_addr,
                           sizeof(data->server_addr));

                    
                    data->window[w].send_time = current_time_helper(); //reset timer
                }
            }
        }
    }

    printf("Thread %d done: tx=%ld rx=%ld lost=%ld\n",
           data->thread_id, data->tx_cnt, data->rx_cnt, data->lost_cnt);

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

    // Create client threads
    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        thread_data[i].server_addr = server_addr; // copy server address into thread context

        // Initialize pipeline fields
        thread_data[i].window_size = 10; //maximum outstanding requests
        thread_data[i].tx_cnt = 0; //packets transmitted
        thread_data[i].rx_cnt = 0; //responses received
        thread_data[i].lost_cnt = 0; //packets considered lost
        thread_data[i].next_seq = 0; // next sequence number to send
        thread_data[i].in_flight = 0; // request currenlty waiting ACK

        thread_data[i].thread_id = i;
        
        //initialize wiindow slots are availble,
        //acked = 1, means slot is free to reuse
        for (int j = 0; j < 1024; j++)
            thread_data[i].window[j].acked = 1;
    }

    //start client worker threads
    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    //wait for all threads to finish
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // Compute results
    long total_tx = 0, total_rx = 0, total_lost = 0;

    //each thread tracks its own TX, RX, and lost packets
    for (int i = 0; i < num_client_threads; i++) {
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_lost += thread_data[i].lost_cnt;
    }

    printf("\n FINAL TOTAL \n");
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
