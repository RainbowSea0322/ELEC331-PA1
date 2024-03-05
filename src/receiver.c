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

// Package size
#define NUM_STAGE_BYTES 1  // total of 6 stages
#define NUM_LENGTH_BYTES 1 // max is 1460
#define NUM_SEQUENCE_BYTES 8 // long long int has 8 bytes length
#define MY_HEADER_SIZE NUM_STAGE_BYTES + NUM_LENGTH_BYTES + NUM_SEQUENCE_BYTES
#define MAX_PAYLOAD_SIZE 1460 // same as TCP

// Handshaking Protocal
enum CONNECTION_STAGE {CON_SYN, CON_ACK, SEND_SYN, SEND_ACK, FIN, FIN_ACK};

// store the packets
#define PACKET_BUFFER_SIZE 100
char* packet_buffer[PACKET_BUFFER_SIZE];
bool received[PACKET_BUFFER_SIZE];
bool written[PACKET_BUFFER_SIZE];

// other
struct sockaddr_in src_addr;
socklen_t addrlen;
struct timeval timeout;

void create_header(enum CONNECTION_STAGE conn_stage, int payload_length, unsigned long long int sequence_num, char* result) {
    for (int i=NUM_STAGE_BYTES-1; i>=0; i--) {
        result[i] = conn_stage & 0xFF;
        conn_stage /= 0xFF;
    }
    
    for (int i=NUM_STAGE_BYTES+NUM_LENGTH_BYTES-1; i>=NUM_STAGE_BYTES; i--) {
        result[i] = payload_length & 0xFF;
        conn_stage /= 0xFF;
    }

    for (int i=NUM_STAGE_BYTES+NUM_LENGTH_BYTES+NUM_SEQUENCE_BYTES-1; i>=NUM_STAGE_BYTES+NUM_LENGTH_BYTES; i--) {
        result[i] = sequence_num & 0xFF;
        conn_stage /= 0xFF;
    }
}

void confirm_conn_stage(char* packet, enum CONNECTION_STAGE expected_stage) {
    enum CONNECTION_STAGE packet_stage = -1; // false default value

    for (int i = 0; i<NUM_STAGE_BYTES; i++) {
        packet_stage += packet[i];
        packet_stage *= 0xFF;
    }

    assert(packet_stage == expected_stage);
}

enum CONNECTION_STAGE get_conn_stage(char* packet) {
    enum CONNECTION_STAGE packet_stage = -1; // false default value

    for (int i = 0; i<NUM_STAGE_BYTES; i++) {
        packet_stage += packet[i];
        packet_stage *= 0xFF;
    }

    return packet_stage;
}

int get_payload_length(char* packet) {
    int payload_length = 0;

    for (int i = NUM_STAGE_BYTES; i<NUM_STAGE_BYTES+NUM_LENGTH_BYTES; i++) {
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

void init_buffers() {
    for (int i = 0; i < PACKET_BUFFER_SIZE; i++) {
        packet_buffer[i] = NULL;
        received[i] = false;
        written[i] = false;
    }
}

// use 3-way handshaking protocals to establish a connecetion
    printf("[receiver] Enter connection stage.\n");

    char conn_ack_header[MY_HEADER_SIZE];
    memset(conn_ack_header, 0, MY_HEADER_SIZE);

    char receive_buffer[MY_HEADER_SIZE];
    memset(receive_buffer, 0, MY_HEADER_SIZE);

    printf("[receiver] Before Getting CON_SYN 0.\n");

    // 1. get SYN and confirm info are correct
    recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (struct sockaddr*)&src_addr, &addrlen);
    confirm_conn_stage(receive_buffer, CON_SYN);

    unsigned long long int syn_sequence_num = get_sequence_num(receive_buffer);
    int syn_payload_length = get_payload_length(receive_buffer);
    assert(syn_sequence_num == 0); // first SYN should have sequence number 0
    assert(syn_payload_length == 0); // SYN should not have any payload

    printf("[receiver] Got CON_SYN 0.\n");

    // 2. send CON_ACK
    create_header(CON_ACK, 0, 0, conn_ack_header); // CON_ACK in third way handshake
    sendto(socket_fd, conn_ack_header, MY_HEADER_SIZE, 0, (struct sockaddr *)&src_addr, addrlen);

    printf("[receiver] Sent CON_ACK 0.\n");

    // 3. get CON_ACK and confirm info are correct
    recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (struct sockaddr *)&src_addr, &addrlen);
    confirm_conn_stage(receive_buffer, CON_ACK);

    printf("[receiver] Got CON_ACK 1.\n");

    syn_sequence_num  = get_sequence_num(receive_buffer);
    syn_payload_length = get_payload_length(receive_buffer);
    assert(syn_sequence_num == 1); // CON_ACK should have sequence number 1
    assert(syn_payload_length == 0); // SYN should not have any payload

    printf("[receiver] Connection stage complete!\n");
}

void recv_data(FILE* file, int socket_fd) {
    char header[MY_HEADER_SIZE];
    memset(header, 0, MY_HEADER_SIZE);

    char receive_buffer[MY_HEADER_SIZE];
    memset(receive_buffer, 0, MY_HEADER_SIZE);

    // disable timeout
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    init_buffers();

    unsigned long long int next_sequence_num = 0;

    while (recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (struct sockaddr *)&src_addr, &addrlen) >= 0) {
        unsigned long long int sequence_num = get_sequence_num(receive_buffer);
        int payload_length = get_payload_length(receive_buffer);
        enum CONNECTION_STAGE stage = get_conn_stage(receive_buffer);

        // finish detection
        if (stage == FIN) {
            assert(sequence_num == 0); // ACK and SYN should have matching sequence number
            assert(payload_length == 0); // ACK should not have any payload
            break;
        }

        // duplicate detection
        if (sequence_num < next_sequence_num || received[sequence_num % PACKET_BUFFER_SIZE]) {
            // duplicate packets, ignore and receive next
            continue;
        }
        
        // store entire packet in buffer
        memcpy(packet_buffer[sequence_num % PACKET_BUFFER_SIZE], receive_buffer, payload_length + MY_HEADER_SIZE);
        received[sequence_num % PACKET_BUFFER_SIZE] = true;
        written[sequence_num % PACKET_BUFFER_SIZE] = false;

        // matching the expected file, write to file
        if (sequence_num == next_sequence_num) {
            for (int i = sequence_num % PACKET_BUFFER_SIZE; i < PACKET_BUFFER_SIZE; i++) {
                if (!received[i]) {
                    // detect discontinuity and stop writting, waiting for more packets
                    break; 
                } else if (!written[i]) {
                    // write contents to file
                    int write_length = get_payload_length(packet_buffer[i]);
                    fwrite(packet_buffer[i] + MY_HEADER_SIZE, sizeof(char), write_length, file);
                    written[i] = true;
                    received[i] = false; // IMPORTANT: prepare for next buffer iteration
                    next_sequence_num++;
                }
            }
        }  
        
        // send back ack
        create_header(SEND_ACK, 0, next_sequence_num-1, header);
        sendto(socket_fd, header, MY_HEADER_SIZE, 0, (struct sockaddr *)&src_addr, addrlen);
    }
}

// 4-way finish
void finish(int socket_fd) {
    printf("[receiver] Enter finish stage.\n");

    char header[MY_HEADER_SIZE];
    memset(header, 0, MY_HEADER_SIZE);

    char receive_buffer[MY_HEADER_SIZE];
    memset(receive_buffer, 0, MY_HEADER_SIZE);

     // 1. send FIN ACK, finish request is handled in recv_data()
    create_header(FIN_ACK, 0, 0, header);
    sendto(socket_fd, header, MY_HEADER_SIZE, 0, (struct sockaddr *)&src_addr, addrlen);

    // 2. send FIN to server
    create_header(FIN, 0, 1, header);
    sendto(socket_fd, header, MY_HEADER_SIZE, 0, (struct sockaddr *)&src_addr, addrlen);

    // 3. get FIN ACK and confirm info are correct
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));  // set timeout
    while (recvfrom(socket_fd, receive_buffer, MY_HEADER_SIZE, 0, (struct sockaddr*)&src_addr, &addrlen) == -1) {
        // resend on timeout
        sendto(socket_fd, header, MY_HEADER_SIZE, 0, (struct sockaddr *)&src_addr, addrlen);
    }
    confirm_conn_stage(receive_buffer, FIN_ACK);

    unsigned long long int syn_sequence_num = get_sequence_num(receive_buffer);
    int syn_payload_length = get_payload_length(receive_buffer);
    assert(syn_sequence_num == 1); // ACK and SYN should have matching sequence number
    assert(syn_payload_length == 0); // ACK should not have any payload

    printf("[receiver] Finish stage complete!\n");
}


void rrecv(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) 
{
    // Create UDP socket
    int socket_fd;
    struct sockaddr_in client_addr;
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Bind socket to given port
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = INADDR_ANY;
    client_addr.sin_port = htons(myUDPport);
    if (bind(socket_fd, (struct sockaddr *)&client_addr, sizeof(client_addr)) == -1) {
        perror("Binding failed");
        exit(EXIT_FAILURE);
    }

    // wait for connection
    connection(socket_fd);

    // Open destination file
    FILE *file = fopen(destinationFile, "wb"); // write binary
    if (file == NULL) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    printf("[receiver] Receiving data.\n");
    
    recv_data(file, socket_fd);

    printf("[receiver] Finish Receiving data.\n");

    // wait for terminate connection
    finish(socket_fd);

    // Close file and socket
    fclose(file);
    close(socket_fd);
}

int main(int argc, char** argv) {
    // This is a skeleton of a main function.
    // You should implement this function more completely
    // so that one can invoke the file transfer from the
    // command line.

    unsigned short int udpPort;

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);
    char* filename = argv[2];
    unsigned long long int writeRate = 0;

    rrecv(udpPort, filename, writeRate);
}
