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

void rrecv(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) 
{
    // Create UDP socket
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Bind socket to given port
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(myUDPport);
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
        perror("Binding failed");
        exit(EXIT_FAILURE);
    }

    // Open destination file
    FILE *file = fopen(destinationFile, "wb");
    if (file == NULL) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    // Receive data and write to file
    unsigned char buffer[MAX_PACKET_SIZE];
    int bytesReceived;
    while (1) {
        socklen_t len = sizeof(cliaddr);
        bytesReceived = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0, (struct sockaddr *)&cliaddr, &len);
        fwrite(buffer, 1, bytesReceived, file);
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

    unsigned short int udpPort;

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);
    filename = argv[2];
    writeRate = 0;

    rrecv(myUBPport, filename, writeRate);
}
