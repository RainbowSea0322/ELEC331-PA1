#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <limits.h>
#include <assert.h>

// Time related
// from: https://stackoverflow.com/questions/361363/how-to-measure-time-in-milliseconds-using-ansi-c
struct timeval tval_before, tval_after, tval_result;
int transfer_milliseconds_from_timeval(struct timeval* tval) {
    return tval->tv_sec*1000 + tval->tv_usec/1000;
}

struct timeval* get_cur_time() {
    struct timeval* time = malloc(sizeof(struct timeval));
    gettimeofday(time, NULL);
    return time;
}

int time_diff_in_milliseconds(struct timeval* t_early, struct timeval* t_later) {
    return transfer_milliseconds_from_timeval(t_later) - transfer_milliseconds_from_timeval(t_early);
}

// Package size
#define NUM_STAGE_BYTES 1  // total of 6 stages
#define NUM_LENGTH_BYTES 1 // max is 1460
#define NUM_SEQUENCE_BYTES 8 // long long int has 8 bytes length
#define MY_HEADER_SIZE NUM_STAGE_BYTES + NUM_LENGTH_BYTES + NUM_SEQUENCE_BYTES
#define MAX_PAYLOAD_SIZE 1460 // same as TCP

// Handshaking Protocal
enum CONNECTION_STAGE {CON_SYN, CON_ACK, SEND_SYN, SEND_ACK, FIN, FIN_ACK};

// Congestion control
enum CONGESTION_STAGE {SLOW_START, CONGESTION_AVOIDANCE, FAST_RECOVERY, RECV_ACK};
int cwnd = 1;
int slow_start_threshold = INT_MAX; // wait for the first failure and set to half of cwnd
int dup_ACK_num = 0;
#define DUP_ACK_THRESHOLD 3

bool finish_sending = false; // indicate start of finish stage
unsigned long long int last_sequence_num = -1;
bool get_all_ACKs = false;

// retransmission
#define PACKET_BUFFER_SIZE 100 // TBD
char* packet_buffer[PACKET_BUFFER_SIZE];
struct timeval* packet_sending_time[PACKET_BUFFER_SIZE];  // 1 to 1 match with packet_buffer
struct timeval timeout;

// other
socklen_t *addrlen;

// helper functions

void create_header(enum CONNECTION_STAGE conn_stage, int payload_length, unsigned long long int sequence_num, char* result) {
    // Assuming NUM_STAGE_BYTES, NUM_LENGTH_BYTES, and NUM_SEQUENCE_BYTES are defined correctly
    int offset = 0;

    // Connection Stage
    for (int i = NUM_STAGE_BYTES - 1; i >= 0; i--) {
        result[offset + i] = (char)((conn_stage >> (8 * i)) & 0xFF);
    }
    offset += NUM_STAGE_BYTES;

    // Payload Length
    for (int i = NUM_LENGTH_BYTES - 1; i >= 0; i--) {
        result[offset + i] = (char)((payload_length >> (8 * (NUM_LENGTH_BYTES - 1 - i))) & 0xFF);
    }
    offset += NUM_LENGTH_BYTES;

    // Sequence Number
    for (int i = NUM_SEQUENCE_BYTES - 1; i >= 0; i--) {
        result[offset + i] = (char)((sequence_num >> (8 * (NUM_SEQUENCE_BYTES - 1 - i))) & 0xFF);
    }
}

void create_packet(char* header, char* payload, int payload_length, char* result) {
    memcpy(result, header, sizeof(char) * MY_HEADER_SIZE);
    memcpy(result+MY_HEADER_SIZE, payload, sizeof(char) * payload_length);
}

void confirm_conn_stage(char* packet, enum CONNECTION_STAGE expected_stage) {
    enum CONNECTION_STAGE packet_stage = -1; // false default value

  for (int i = 0; i < NUM_STAGE_BYTES; i++) {
        packet_stage |= (unsigned char)packet[i] << (8 * (NUM_STAGE_BYTES - 1 - i));
    }

    assert(packet_stage == expected_stage);
}

int get_payload_length(char* packet) {
    int payload_length = 0;

    for (int i = NUM_STAGE_BYTES; i<NUM_STAGE_BYTES+NUM_LENGTH_BYTES>; i++) {
        payload_length += packet[i];
        payload_length *= 0xFF;
    }

    return payload_length;
}

unsigned long long int get_sequence_num(char* packet){
    unsigned long long int sequence_num = 0;

    for (int i = NUM_STAGE_BYTES+NUM_LENGTH_BYTES; i < NUM_STAGE_BYTES+NUM_LENGTH_BYTES+NUM_SEQUENCE_BYTES; i++) {
        sequence_num += packet[i];
        sequence_num *= 0xFF;
    }

    return sequence_num;
}

// from:https://www.educative.io/answers/how-to-compute-devrtt-estimated-rtt-time-out-interval-in-ccn
void update_timeout(int new_RTT){
    int est_RTT = new_RTT; // in milli seconds
    int dev_RTT; // in milli seconds
    int temp_timeout;
    double alpah = 0.125;
    double beta = 0.25;

    est_RTT = (1-alpha) * est_RTT + alpha * new_RTT;
    dev_RTT = (1-beta) * dev_RTT + beta * abs(new_RTT - est_RTT);
    temp_timeout = est_RTT + dev_RTT * 4; // in microseconds
    timeout.tv_sec = temp_timeout / 1000; // seconds part
    timeout.tv_usec = (temp_timeout % 1000) * 1000; // micro seconds parts
}

// use 3-way handshaking protocals to establish a connecetion
void connection(int socket_fd, struct sockaddr_in *dest_addr) {
    printf("[sender] Enter connection stage.\n");

    unsigned long long int cur_sequence_num = 0;

    char conn_syn_header[MY_HEADER_SIZE];
    memset(conn_syn_header, 0, MY_HEADER_SIZE);

    char receive_buffer[MY_HEADER_SIZE];
    memset(receive_buffer, 0, MY_HEADER_SIZE);

    // 1. send conn_syn and measure RTT
    // header tranmission should be pretty fast 1 sec timeout
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));  // set timeout

    create_header(CON_SYN, 0, cur_sequence_num, conn_syn_header);
    sendto(socket_fd, conn_syn_header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);
    gettimeofday(&tval_before, NULL);

    // 2. get ACK and confirm info are correct
    while (recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (const struct sockaddr*)dest_addr, addrlen) == -1) {
        // resend on timeout
        sendto(socket_fd, conn_syn_header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);
        gettimeofday(&tval_before, NULL);
    }
    gettimeofday(&tval_after, NULL);
    timersub(&tval_after, &tval_before, &tval_result);
    int new_RTT = transfer_microseconds_from_timeval(&tval_result);
    update_timeout(new_RTT);

    confirm_conn_stage(receive_buffer, CON_ACK);

    unsigned long long int ack_sequence_num = get_sequence_num(receive_buffer);
    int ack_payload_length = get_payload_length(receive_buffer);
    assert(cur_sequence_num == ack_sequence_num); // ACK and SYN should have matching sequence number
    assert(ack_payload_length == 0); // ACK should not have any payload

    // 2. send conn_syn and complete connection stage
    cur_sequence_num++;
    create_header(CON_ACK, 0, cur_sequence_num, conn_syn_header); // CON_ACK in third way handshake
    sendto(socket_fd, conn_syn_header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);

    printf("[sender] Connection stage complete!\n");
}

void send_data(int socket_fd, struct sockaddr_in *dest_addr, unsigned long long int sequence_num, char* payload, int payload_length) {
    int packet_length = MY_HEADER_SIZE + payload_length;

    char header[MY_HEADER_SIZE];
    memset(header, 0, MY_HEADER_SIZE);
    char packet[packet_length] = malloc(sizeof(char) * packet_length); // clean up after use
    memset(packet, 0, packet_length);

    create_header(SEND_SYN, payload_length, sequence_num, header);
    create_packet(header, payload, payload_length, packet);
    sendto(socket_fd, packet, packet_length, 0, (const struct sockaddr *)dest_addr, addrlen);
    // store info in the buffer in case of retransmission needed
    packet_sending_time[sequence_num % PACKET_BUFFER_SIZE] = get_cur_time(); 
    packet_buffer[sequence_num % PACKET_BUFFER_SIZE] = packet;
}

void send_packets(FILE* file, int socket_fd, struct sockaddr_in *dest_addr, unsigned long long int start_sequence_num, unsigned long long int bytesToTransfer) {
    // split data and send
    int bytes_left = bytesToTransfer;
    unsigned long long int sequence_num = start_sequence_num;
    while (bytes_left >= MAX_PAYLOAD_SIZE) {
        char cur_payload[MAX_PAYLOAD_SIZE];
        memset(cur_payload, 0, MAX_PAYLOAD_SIZE);
        // assembly the packet and send
        int bytesRead = fread(cur_payload, 1, MAX_PAYLOAD_SIZE, file);
        send_data(socket_fd, dest_addr, sequence_num, cur_payload, bytesRead);

        // FILE reach its end
        if (bytesRead != MAX_PAYLOAD_SIZE) {
            last_sequence_num = sequence_num;
            return;
        }
        
        sequence_num ++;
        bytes_left -= bytesRead;
    }
    // one last small packet, normally will not be trigered
    if(bytes_left > 0) {
        char cur_payload[bytes_left];
        memset(cur_payload, 0, bytes_left);

        int bytesRead = fread(cur_payload, 1, bytes_left, file);
        send_data(socket_fd, dest_addr, sequence_num, cur_payload, bytesRead);
        
        last_sequence_num = sequence_num;
    }
}

void retransmit(int socket_fd, struct sockaddr_in *dest_addr, unsigned long long int missing_sequence_num, unsigned long long int ) {
    char* packet = packet_buffer[sequence_num % PACKET_BUFFER_SIZE];
    int packet_length = MY_HEADER_SIZE + get_payload_length(packet);

    // use buffer to retransmit and also update info in the buffer.
    sendto(socket_fd, packet, packet_length, 0, (const struct sockaddr *)dest_addr, addrlen);
    // update sending time
    free(packet_sending_time[sequence_num % PACKET_BUFFER_SIZE]);
    packet_sending_time[sequence_num % PACKET_BUFFER_SIZE] = get_cur_time(); 
}

// 4-way finish 
void finish(int socket_fd, struct sockaddr_in *dest_addr) {
    printf("[sender] Enter finish stage.\n");

    unsigned long long int cur_sequence_num = 0;

    // 1. request finish
    char header[MY_HEADER_SIZE];
    memset(header, 0, MY_HEADER_SIZE);

    char receive_buffer[MY_HEADER_SIZE];
    memset(receive_buffer, 0, MY_HEADER_SIZE);

    create_header(FIN, 0, cur_sequence_num, header);
    sendto(socket_fd, header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);

    // 2. get FIN ACK and confirm info are correct
    while (recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (const struct sockaddr*)dest_addr, addrlen) == -1) {
        // resend on timeout
        sendto(socket_fd, header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);
    }
    confirm_conn_stage(receive_buffer, FIN_ACK);

    unsigned long long int ack_sequence_num = get_sequence_num(receive_buffer);
    int ack_payload_length = get_payload_length(receive_buffer);
    assert(cur_sequence_num == ack_sequence_num); // ACK and SYN should have matching sequence number
    assert(ack_payload_length == 0); // ACK should not have any payload]

    cur_sequence_num++; // remember to increment sequence number

    // diable timeout
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;   
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // 3. wait for FIN from reveiver
    while (recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (const struct sockaddr*)dest_addr, addrlen) == -1) {
        // resend on timeout
        sendto(socket_fd, header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);
    }
    confirm_conn_stage(receive_buffer, FIN);

    ack_sequence_num = get_sequence_num(receive_buffer);
    ack_payload_length = get_payload_length(receive_buffer);
    assert(cur_sequence_num == ack_sequence_num); // should be FIN 1
    assert(ack_payload_length == 0); // ACK should not have any payload

    // 4. send last FIN ACK
    create_header(FIN_ACK, 0, cur_sequence_num, header);
    sendto(socket_fd, header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);

    printf("[sender] Finish stage complete!\n");
}

void free_buffers(){
    for (int i = 0; i < PACKET_BUFFER_SIZE; i++) {
        free(packet_buffer[i]);
        free(packet_sending_time[i]);
    }
}

void rsend(char* hostname, 
            unsigned short int hostUDPport, 
            char* filename, 
            unsigned long long int bytesToTransfer) 
{
    // Create UDP socket
    int socket_fd;
    struct sockaddr_in server_addr;
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Fill server information
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; //IPv4
    server_addr.sin_port = htons(hostUDPport);
    server_addr.sin_addr.s_addr = inet_addr(hostname);

    // establish connection
    connection(socket_fd, &server_addr);

    // Open file to read
    FILE *file = fopen(filename, "rb"); // read binary
    if (file == NULL) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    printf("[sender] Sending data.\n");
    
    // congestion control and sending data
    
    enum CONGESTION_STAGE next_stage = SLOW_START;

    char receive_buffer[MY_HEADER_SIZE];    
    
    int in_air_packets_number = 0;
    unsigned long long int cur_sequence_num = 0;
    unsigned long long int first_unACK_sequence_num = 0;
    unsigned long long int remaining_bytes = bytesToTransfer;

    // send the first packet then we can start observing what's happened and adjust congestion states accordingly.
    send_packets(file, socket_fd, sockaddr_in, cur_sequence_num, MAX_PAYLOAD_SIZE * cwnd);
    cur_sequence_num++;
    in_air_packets_number++;

    while (!finish_sending) {

        struct timeval remaining_time;
        struct timeval cur_time;

        int timeout_milli = transfer_milliseconds_from_timeval(timeout);

        gettimeofday(&cur_time, NULL);
        int past_milli = time_diff_in_milliseconds(packet_sending_time[first_unACK_sequence_num % PACKET_BUFFER_SIZE], cur_time);
        
        remaining_time.sec = (timeout_milli-past_milli) / 1000;
        remaining_time.usec = ((timeout_milli-past_milli) % 1000) * 1000;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &remaining_time, sizeof(remaining_time));  // update timeout for next recvfrom()
        
        // state transtions based on the next ACK received
        if (recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (const struct sockaddr*)dest_addr, addrlen) == -1) {
            // timeout event: reset and enter Slow Start
            slow_start_threshold = cwnd / 2; // could be zero
            cwnd = 1;
            dup_ACK_num = 0;
            int success_packets_counter = 0;

            cur_sequence_num = first_unACK_sequence_num; // resend packets from the first missing ACK

            retransmit(socket_fd, sockaddr_in, cur_sequence_num); // retransmit since the last one is lost

            next_stage = SLOW_START;

            // exponential back off
            timeout.tv_sec *= 2;
            timeout.tv_sec += (timeout.tv_usec * 2) / 1000000; // carry out
            timeout.tv_usec = (timeout.tv_usec * 2) % 1000000; // remainder
        } else {
            unsigned long long int ack_sequence_num = get_sequence_num(receive_buffer);
            confirm_conn_stage(receive_buffer, SEND_ACK);
            if (ack_sequence_num == first_unACK_sequence_num) {

                // finish detection
                if (ack_sequence_num == last_sequence_num) {
                    finish_sending = true;
                    continue;
                }
                
                // everything is normal, behavior based on previous state
                first_unACK_sequence_num++;
                in_air_packets_number--;

                if (next_stage == SLOW_START) {
                    cwnd++; // cwnd +1 each time receive an ACK
                    dupACK = 0;

                    // keep as slow start until reach threshold
                    if (cwnd >= slow_start_threshold) {
                        next_stage == CONGESTION_AVOIDANCE;
                    }
                } else if (next_stage == CONGESTION_AVOIDANCE) {
                    success_packets_counter++;
                    if(packets_counter >= cwnd) {
                        cwnd++; // cwnd +1 only when the previous window packets are all success
                        success_packets_counter = 0; // reset for next round
                    }
                    dupACK = 0;
                    // stay in congestion avoidance unless a packet loss (both timeout and dup ACK)
                } else if (next_stage == FAST_RECOVERY) {
                    cwnd = slow_start_threshold > 1 ? slow_start_threshold : 1; // cwnd need to be at least 1, slow_start_threshold can be reduced to 0 
                    dupACK = 0;

                    next_stage == CONGESTION_AVOIDANCE;
                }
            } else if (ack_sequence_num < first_unACK_sequence_num) {
                // got a duplicate ACK
                in_air_packets_number--; // still decrement the in_air_number since we do have one less ACK to reveive in the middle

                dup_ACK_num++;
                if (dup_ACK_num >= DUP_ACK_THRESHOLD) {
                    // package lost enter fast recovery stage
                    slow_start_threshold = cwnd / 2;
                    cwnd = slow_start_threshold > 1 ? slow_start_threshold : 1;
                    dup_ACK_num = 0;

                    retransmit(socket_fd,sockaddr_in, first_unACK_sequence_num, cur_sequence_num);

                    next_stage = FAST_RECOVERY;
                }
                // keep running and remain in the current stage if dunp_ACK haven't reach the threshold 
            }
        }
        // try sending until reach cwnd limits
        if (in_air_packets_number < cwnd && remaining_bytes > 0) {
            // try sending packets
            if (remaining_bytes > MAX_PAYLOAD_SIZE * (cwnd - in_air_packets_number)) {
                (file, socket_fd, sockaddr_in, cur_sequence_num, MAX_PAYLOAD_SIZE * (cwnd - in_air_packets_number));
                cur_sequence_num += (cwnd - in_air_packets_number);
                in_air_packets_number += (cwnd - in_air_packets_number);
                remaining_bytes -= MAX_PAYLOAD_SIZE * (cwnd - in_air_packets_number);
            } else {
                send_packets(file, socket_fd, sockaddr_in, cur_sequence_num, remaining_bytes);
                int num_packets_send = remaining_bytes / MAX_PAYLOAD_SIZE;
                if (remaining_bytes % MAX_PAYLOAD_SIZE != 0) {
                    num_packets_send++;
                }
                cur_sequence_num += num_packets_send;
                in_air_packets_number += num_packets_send;
                remaining_bytes = 0;
            }
            
        }
    } 

    printf("[sender] Finish sending data.\n");

    // terminate connection
    finish(socket_fd, server_addr);

    // buffer clean up after finish sending
    free_buffers();

    // Close file and socket
    fclose(file);
    close(socket_fd);
}

int main(int argc, char** argv) {
    // This is a skeleton of a main function.
    // You should implement this function more completely
    // so that one can invoke the file transfer from the
    // command line.
    int hostUDPport;
    unsigned long long int bytesToTransfer;
    char* hostname = NULL;

    if (argc != 5) {
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    hostUDPport = (unsigned short int) atoi(argv[2]);
    hostname = argv[1];
    bytesToTransfer = atoll(argv[4]);
    file_name = argv[3];

    rsend(hostname, hostUDPportmb, file_name, bytesToTransfer);

    return (EXIT_SUCCESS);
}