#include <stdio.h>      // for printf, perror, fopen, fwrite,...
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
    /*
    struct sockaddr_in {
    sa_family_t    sin_family;   // which kind of address (IPv4)
    in_port_t      sin_port;     // the port number (16 bits)
    struct in_addr sin_addr;     // the IP address (32 bits)
    unsigned char  sin_zero[8];  // padding, unused
    };
    */
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // 1. Create a TCP socket
    //This asks the kernel: "create me a network endpoint."
    //SOCK_STREAM --> A reliable, ordered stream of bytes (this means TCP).
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(1);
    }

    // 2. Fill in the address structure: which IP/port do we bind to?
    memset(&server_addr, 0, sizeof(server_addr));       // zero out the struct first
    server_addr.sin_family = AF_INET;                    // IPv4
    server_addr.sin_port = htons(PORT);                  // port, converted to network byte order
    
    //"10.0.0.2" needs to become 4 raw bytes
    // AF_INET -> IPv4 address
    //LISTEN_IP	-> The text to convert
    //&server_addr.sin_add -> Where to store the 4 result bytes
    inet_pton(AF_INET, LISTEN_IP, &server_addr.sin_addr); 




    // 3. Bind the socket to that IP and port
    //bind attaches the phone (server_fd) to the number (10.0.0.2:5000).
    //const struct sockaddr *x->location in memory of a form containing the IP and port
    //How many bytes long that form is
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(1);
    }




    // 4. Put the socket into listening mode, allow 1 pending connection in queue
    //server_fd -> is int, tells Which socket should start listening
    //How many finished connections may wait in the queue
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
    //w = write, b = binary
    FILE *out_file = fopen("output/output.txt", "wb");
    if (out_file == NULL) {
        perror("fopen failed");
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    // 7. Read from the socket in a loop until the sender closes the connection
    // ssize_t read(int fd, void *buf, size_t count);
    ssize_t bytes_received;
    while ((bytes_received = read(client_fd, buffer, BUFFER_SIZE)) > 0) {
    
        //The address of the data to write (buffer)
        //The size of one item in bytes (1)
        //How many items to write (bytes_received)
        //The open file to write into (out_file)
        fwrite(buffer, 1, bytes_received, out_file);
    }

    printf("Receiver: transfer complete, file saved.\n");

    // 8. Clean up: close file and both sockets
    fclose(out_file);
    close(client_fd);
    close(server_fd);

    return 0;
}
