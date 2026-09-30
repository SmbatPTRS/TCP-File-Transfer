/*
 * main.c  -  Entry point. One program, two roles:
 *
 *     ./build/network receiver     run Client B (listens, decrypts, saves)
 *     ./build/network sender       run Client A (reads file, encrypts, sends)
 *
 * All settings live in the #define lines below, in one place.
 */

#include <stdio.h>    // fprintf
#include <string.h>   // strcmp

#include "senderAES.h"
#include "receiverAES.h"

/* ---------- Settings ---------- */
#define PORT               5000
#define RECEIVER_IP        "10.0.0.2"     // the IP inside receiver-ns
#define INPUT_FILE         "/home/smbat/Desktop/Network/data/input.txt"
#define OUTPUT_FILE        "/home/smbat/Desktop/Network/output/output.txt"
#define KEY_FILE           "/home/smbat/Desktop/Network/data/key.bin"

/* Explain how to start the program. */
static void print_usage(const char *program_name)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s receiver    start the receiver (Client B)\n", program_name);
    fprintf(stderr, "  %s sender      start the sender   (Client A)\n", program_name);
}

int main(int argc, char *argv[])
{
    // argv[0] is the program name, argv[1] must be the role. So argc must be 2.
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    // strcmp returns 0 when the two texts are equal.
    if (strcmp(argv[1], "receiver") == 0) {
        return receiver_run(RECEIVER_IP, PORT, OUTPUT_FILE, KEY_FILE);
    }

    if (strcmp(argv[1], "sender") == 0) {
        return sender_run(RECEIVER_IP, PORT, INPUT_FILE, KEY_FILE);
    }

    // Anything else is a mistake.
    fprintf(stderr, "Unknown role: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
