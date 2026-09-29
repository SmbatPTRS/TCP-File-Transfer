#include <stdio.h>      // for printf, perror, fopen, fwrite, fclose
#include <stdlib.h>     // for exit()
#include <string.h>     // for memset()
#include <unistd.h>     // for close(), read()
#include <arpa/inet.h>  // for sockaddr_in, htons, inet_pton, etc.
#include <sys/socket.h> // for socket(), bind(), listen(), accept()

#define PORT 5000
#define BUFFER_SIZE 1024
#define LISTEN_IP "10.0.0.2"   // the receiver's own IP inside receiver-ns

int main(void) {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // 1. Create a TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(1);
    }

    // 2. Fill in the address structure: which IP/port do we bind to?
    memset(&server_addr, 0, sizeof(server_addr));       // zero out the struct first
    server_addr.sin_family = AF_INET;                    // IPv4
    server_addr.sin_port = htons(PORT);                  // port, converted to network byte order
    inet_pton(AF_INET, LISTEN_IP, &server_addr.sin_addr); // convert "10.0.0.2" string into binary form

    // 3. Bind the socket to that IP and port
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(1);
    }

    // 4. Put the socket into listening mode, allow 1 pending connection in queue
    if (listen(server_fd, 1) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(1);
    }

    printf("Receiver: listening on %s:%d ...\n", LISTEN_IP, PORT);

    // 5. Block here until a client connects; get a NEW socket for that specific connection
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept failed");
        close(server_fd);
        exit(1);
    }

    printf("Receiver: client connected.\n");

    // 6. Open the output file where we'll write whatever bytes we receive
    FILE *out_file = fopen("output/output.txt", "wb");
    if (out_file == NULL) {
        perror("fopen failed");
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    // 7. Read from the socket in a loop until the sender closes the connection
    ssize_t bytes_received;
    while ((bytes_received = read(client_fd, buffer, BUFFER_SIZE)) > 0) {
        fwrite(buffer, 1, bytes_received, out_file);
    }

    printf("Receiver: transfer complete, file saved.\n");

    // 8. Clean up: close file and both sockets
    fclose(out_file);
    close(client_fd);
    close(server_fd);

    return 0;
}
