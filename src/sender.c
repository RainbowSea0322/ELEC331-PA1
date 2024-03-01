#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <limits.h>

// Time related
// from: https://stackoverflow.com/questions/361363/how-to-measure-time-in-milliseconds-using-ansi-c
struct timeval tval_before, tval_after, tval_result;
int transfer_microseconds_from_timeval(struct timeval* tval) {
    return tval->tv_sec*1000 + tval->tv_usec/1000;
}

// Package size
#define NUM_STAGE_BYTES 1;  // total of 6 stages
#define NUM_LENGTH_BYTES 1; // max is 1460
#define NUM_SQUENCE_BYTES 8; // long long int has 8 bytes length
#define MY_HEADER_SIZE NUM_STAGE_BYTES + NUM_LENGTH_BYTES + NUM_SQUENCE_BYTES;
#define MAX_PAYLOAD_SIZE 1460; // same as TCP

// Handshaking Protocal
enum CONNECTION_STAGE {CON_SYN, CON_ACK, SEND_SYN, SEND_ACK, FIN, FIN_ACK};

// Congestion control
enum CONGESTION_STAGE {SLOW_START, CONGESTION_AVOIDANCE, FAST_RECOVERY};
int cwnd = 1;
int slow_start_threshold = 64; //TBD

// retransmission
#define PACKET_BUFFER_SIZE INT_MAX; //TBD
char* packet_buffer[PACKET_BUFFER_SIZE];
struct timeval timeout;

// other
socklen_t *addrlen;

// helper functions

void create_header(enum CONNECTION_STAGE conn_stage, int payload_length, unsigned long long int sequence_num, char* result) {
    for(int i=NUM_STAGE_BYTES; i>0; i--){
        result[i] = conn_stage & 0xFF;
        conn_stage /= 0xFF;
    }
    
    for(int i=NUM_STAGE_BYTES+NUM_LENGTH_BYTES; i>NUM_STAGE_BYTES; i--){
        result[i] = payload_length & 0xFF;
        conn_stage /= 0xFF;
    }

    for(int i=NUM_STAGE_BYTES+NUM_LENGTH_BYTES+NUM_SQUENCE_BYTES; i>NUM_STAGE_BYTES+NUM_LENGTH_BYTES; i--){
        result[i] = sequence_num & 0xFF;
        conn_stage /= 0xFF;
    }
}

void create_packet(char* header, char* payload, int payload_length, char* result) {
    memcpy(result, header, sizeof(char) * MY_HEADER_SIZE);
    memcpy(result+MY_HEADER_SIZE, payload, sizeof(char) * payload_length);
}

void confirm_conn_stage(char* packet, enum CONNECTION_STAGE expected_stage) {
    enum CONNECTION_STAGE packet_stage = -1; // false default value

    for(int i = 0; i<NUM_STAGE_BYTES>; i++){
        packet_stage += packet[i];
        packet_stage *= 0xFF;
    }

    assert(packet_stage == expected_stage);
}

int get_payload_length(char* packet) {
    int payload_length = 0;

    for(int i = NUM_STAGE_BYTES; i<NUM_STAGE_BYTES+NUM_LENGTH_BYTES>; i++){
        payload_length += packet[i];
        payload_length *= 0xFF;
    }

    return payload_length;
}

unsigned long long int get_sequence_num(char* packet){
    unsigned long long int sequence_num = 0;

    for(int i = NUM_STAGE_BYTES+NUM_LENGTH_BYTES; i < NUM_STAGE_BYTES+NUM_LENGTH_BYTES+NUM_SQUENCE_BYTES; i++){
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
void connection(int socket_fd, sockaddr_in *dest_addr) {
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
    while(recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (const struct sockaddr*)dest_addr, addrlen) == -1) {
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

void send_data(int socket_fd, sockaddr_in *dest_addr, unsigned long long int sequence_num, char* payload, int payload_length) {
    int packet_length = MY_HEADER_SIZE + payload_length;

    char header[MY_HEADER_SIZE];
    char packet[packet_length];
    create_header(SEND_SYN, payload_length, sequence_num, header);
    create_packet(header, payload, payload_length, packet);
    sendto(socket_fd, packet, packet_length, 0, (const struct sockaddr *)dest_addr, addrlen);
}

// 4-way finish 
void finish(int socket_fd, sockaddr_in *dest_addr) {
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
    while(recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (const struct sockaddr*)dest_addr, addrlen) == -1) {
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
    while(recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (const struct sockaddr*)dest_addr, addrlen) == -1) {
        // resend on timeout
        sendto(socket_fd, header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);
    }
    confirm_conn_stage(receive_buffer, FIN);

    unsigned long long int ack_sequence_num = get_sequence_num(receive_buffer);
    int ack_payload_length = get_payload_length(receive_buffer);
    assert(cur_sequence_num == ack_sequence_num); // should be FIN 1
    assert(ack_payload_length == 0); // ACK should not have any payload

    // 4. send last FIN ACK
    create_header(FIN_ACK, 0, cur_sequence_num, header);
    sendto(socket_fd, header, MY_HEADER_SIZE, 0, (const struct sockaddr *)dest_addr, addrlen);

    printf("[sender] Finish stage complete!\n");
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

    // split data and create packets
    int bytes_left = bytesToTransfer;
    unsigned long long int sequence_num = 0;
    while(bytes_left > MAX_PAYLOAD_SIZE) {
        char cur_payload[MAX_PAYLOAD_SIZE];
        int bytesRead = fread(cur_payload, 1, MAX_PAYLOAD_SIZE, file);
        // assembly the packet and store in the packet buffer
        bytes_left -= bytesRead;
        char cur_header[MY_HEADER_SIZE];
        create_header(SEND_SYN, bytesRead, sequence_num, cur_header);
        char* cur_packet = create_packet(cur_header, cur_payload, bytesRead);
        

        
    }
    
    // TODO

    // congestion control and sending data
    bool sending = true; //start sending packets
    enum CONGESTION_STAGE cur_stage = SLOW_START;
    enum CONGESTION_STAGE next_stage = SLOW_START;

    while(sending) {
        cur_stage = next stage;
        switch(cur_stage){
            case(SLOW_START):
                cwnd++;

                break;
            case(CONGESTION_AVOIDANCE):
                break;
            case(FAST_RECOVERY):
                break;
        }
    } 

    // terminate connection
    finish(socket_fd, server_addr);

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