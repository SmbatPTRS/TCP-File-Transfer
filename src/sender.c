#include <stdio.h>      // for printf, perror, fopen, fread, fclose
#include <stdlib.h>     // for exit()
#include <string.h>     // for memset()
#include <unistd.h>     // for close(), write()
#include <arpa/inet.h>  // for sockaddr_in, htons, inet_pton
#include <sys/socket.h> // for socket(), connect()

#define PORT 5000
#define BUFFER_SIZE 1024
#define SERVER_IP "10.0.0.2"   // the receiver's IP, inside receiver-ns

int main(void) {
    int sock_fd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    // 1. Create a TCP socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket failed");
        exit(1);
    }

    // 2. Fill in the address of the machine we want to connect to
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    // 3. Actively connect to the receiver
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect failed");
        close(sock_fd);
        exit(1);
    }

    printf("Sender: connected to %s:%d\n", SERVER_IP, PORT);

    // 4. Open the local file we want to transmit
    FILE *in_file = fopen("/home/smbat/Desktop/Network/data/input.txt", "rb");
    if (in_file == NULL) {
        perror("fopen failed");
        close(sock_fd);
        exit(1);
    }

    // 5. Read the file in chunks, and push each chunk into the socket
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, in_file)) > 0) {
        ssize_t bytes_sent = write(sock_fd, buffer, bytes_read);
        if (bytes_sent < 0) {
            perror("write failed");
            break;
        }
    }

    printf("Sender: file sent.\n");

    // 6. Clean up: close file and socket
    fclose(in_file);
    close(sock_fd);

    return 0;
}
