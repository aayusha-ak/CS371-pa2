
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
    event.events = EPOLLIN;  // notify when socket is has incoming data to read
    event.data.fd = data->socket_fd; // store fd, to know which socket has to be monitored
    epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event); // add the socket to epoll, and notify when it beocmes readable

    //loops until all the requests are either recived or marked lost
    while (data->rx_cnt + data->lost_cnt < num_requests) {

        long long current_time = current_time_helper(); // current time for timeout tracking

        // Sending packets to window, until window is full
        while (data->in_flight < data->window_size &&
               data->next_seq < num_requests) {

            //packet structure
            struct {
                int seq_num; //sequence number
                char payload[MESSAGE_SIZE]; //data being send
            } pkt;

            pkt.seq_num = data->next_seq; //assign the unique sequence number

            // copy data into packet, to ensure paker has a fixed size
            memcpy(pkt.payload, "ABCDEFGHIJKMLNOP", MESSAGE_SIZE);

            //Sending the packet to server
            sendto(data->socket_fd, &pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&data->server_addr,
                   sizeof(data->server_addr));

            //window acts like a circular buffer, so window reuses slots
            int window_index = pkt.seq_num % data->window_size;
            
            //Save packet info in the window
            data->window[window_index].seq_num = pkt.seq_num; //packet sequence number
            data->window[window_index].send_time = current_time; //when it was send
            data->window[window_index].acked = 0; //response recieved or not?

            //updating for next
            data->tx_cnt++; //total packet sent
            data->in_flight++; //packets sent that are not acknowledged yet
            data->next_seq++; //sequence number for the next packet
        }

        

        //Check if the socker has reponses ready, if so read them, and update the sliding window

        int numof_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 0);//check to see if there are any sockets ready for reading?

        //loop throught the events
        for (int i = 0; i < numof_events; i++) {
            //check if event belongs to the socket
            if (events[i].data.fd == data->socket_fd) {

                //structure to store recieved ACK packet, from the server
                struct {
                    int seq_num; //sequence number
                    char payload[MESSAGE_SIZE]; //data
                } ack_packet;

                socklen_t addrlen = sizeof(data->server_addr); //size of the address strucutre
                
                //reads incomign packet from socket
                recvfrom(data->socket_fd, &ack_packet, sizeof(ack_packet), 0,
                         (struct sockaddr *)&data->server_addr,
                         &addrlen);

                //Map the packet sequence number to a position in the window buffer.
                //%, keeps the index wihtin the window size,
                //allowing the window array to behave like a circular buffer
                int index = ack_packet.seq_num % data->window_size; 
                //check if already ACKed
                if (!data->window[index].acked) {
                    data->window[index].acked = 1; //response recieved
                    data->rx_cnt++; 
                    data->in_flight--;
                }
            }
        }

        // Time of dectection: 

        current_time = current_time_helper(); //update current time
        //loop through every slot in the window buffer
        for (int i = 0; i < data->window_size; i++) {
            //check if packet has been acknowledged
            if (!data->window[i].acked){
                //if time is greater then the threshold
                if ((current_time - data->window[i].send_time) > TIMEOUT_VALUE) {

                    data->window[i].acked = 1;
                    data->lost_cnt++;
                    data->in_flight--;
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

        for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].thread_id = i;   //track thread


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
