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
#include <time.h>

// Time related
clock_t prev_time = clock();

int seconds_since_last_clock() {
    int result = difftime(clock(), prev_time);
    prev_time = clock();
    return result;
}

// Handshaking Protocal


// Packet Structure


// Congestion control


// retransmission


//


void rsend(char* hostname, 
            unsigned short int hostUDPport, 
            char* filename, 
            unsigned long long int bytesToTransfer) 
{
    // Create UDP socket
    int sockfd;
    struct sockaddr_in servaddr;
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Fill server information
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET; //IPv4
    servaddr.sin_port = htons(hostUDPport);
    servaddr.sin_addr.s_addr = inet_addr(hostname);

    // Open file to send
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    // Send file data over UDP
    unsigned char buffer[bytesToTransfer];
    int bytesRead;
    while ((bytesRead = fread(buffer, 1, bytesToTransfer, file)) > 0 && bytesToTransfer > 0) {
        sendto(sockfd, buffer, bytesRead, 0, (const struct sockaddr *)&servaddr, sizeof(servaddr));
        bytesToTransfer -= bytesRead;
    }

    // Close file and socket
    fclose(file);
    close(sockfd);
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