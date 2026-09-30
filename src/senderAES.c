/*
 * senderAES.c  -  Client A: reads a file, encrypts it in chunks with
 *                 AES-256-GCM, and sends it to the receiver over TCP.
 *
 * WIRE FORMAT of what this program sends:
 *
 *   1) Once, at the very start:   [ salt: 8 bytes ]
 *   2) Then one frame per chunk:  [ length: 4 bytes ][ ciphertext: length bytes ][ tag: 16 bytes ]
 *   3) Finally an END frame:      same layout with length = 0 (no ciphertext bytes)
 *
 * The nonce of each frame is NOT sent. It is rebuilt on both sides as:
 *   nonce (12 bytes) = [ salt: 8 bytes ][ frame counter: 4 bytes ]
 *
 * The 4-byte length field is sent in clear, but it is fed to GCM as
 * "additional authenticated data" (AAD), so any change to it breaks the tag.
 */

#include <stdio.h>       // printf, perror, fprintf, fopen, fread, fclose, ferror
#include <string.h>      // memset, memcpy
#include <stdint.h>      // uint32_t, uint16_t, UINT32_MAX (integers of exact size)
#include <errno.h>       // errno, EINTR
#include <signal.h>      // signal, SIGPIPE, SIG_IGN
#include <unistd.h>      // write, close
#include <arpa/inet.h>   // sockaddr_in, htons, htonl, inet_pton
#include <sys/socket.h>  // socket, connect
#include <openssl/evp.h>     // EVP_* encryption functions
#include <openssl/rand.h>    // RAND_bytes
#include <openssl/crypto.h>  // OPENSSL_cleanse

#include "senderAES.h"

/* ---------- Sizes (all in bytes) ----------
 * IMPORTANT: these must be identical in receiverAES.c, because both
 * sides must agree on the exact layout of the bytes on the wire. */
#define CHUNK_SIZE       1024   // max plaintext bytes per frame
#define KEY_LEN          32     // AES-256 key
#define SALT_LEN         8      // random per-connection part of the nonce
#define NONCE_LEN        12     // = SALT_LEN + 4 bytes of counter
#define TAG_LEN          16     // GCM authentication tag
#define LENGTH_FIELD_LEN 4      // the "length" field at the start of a frame
#define FRAME_MAX (LENGTH_FIELD_LEN + CHUNK_SIZE + TAG_LEN)  // biggest possible frame

/* ------------------------------------------------------------------
 * load_key: read KEY_LEN bytes from a file into 'key'.
 * Returns 0 on success, -1 on failure.
 * ------------------------------------------------------------------ */
static int load_key(const char *path, unsigned char *key)
{
    FILE *f = fopen(path, "rb");                 // open in binary mode
    if (f == NULL) {
        perror("cannot open key file");
        return -1;
    }

    size_t got = fread(key, 1, KEY_LEN, f);      // try to read 32 bytes
    fclose(f);                                   // close before checking, so it is closed on every path

    if (got != KEY_LEN) {                        // file was shorter than 32 bytes
        fprintf(stderr, "key file is too short (need %d bytes)\n", KEY_LEN);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * send_all: write exactly 'len' bytes to the socket 'fd'.
 * A single write() may send fewer bytes than requested, so we loop
 * until everything has been sent.
 * Returns 0 on success, -1 on failure.
 * ------------------------------------------------------------------ */
static int send_all(int fd, const unsigned char *data, size_t len)
{
    size_t total_sent = 0;                       // how many bytes are already sent

    while (total_sent < len) {
        // send the remaining part: start after what is already sent
        ssize_t n = write(fd, data + total_sent, len - total_sent);

        if (n < 0) {
            if (errno == EINTR) {                // interrupted by a signal, just retry
                continue;
            }
            perror("write failed");
            return -1;
        }
        total_sent += (size_t)n;                 // n bytes went out, advance
    }
    return 0;
}

/* ------------------------------------------------------------------
 * build_nonce: create the 12-byte nonce = salt (8 bytes) + counter (4 bytes).
 * The counter is written most-significant byte first (big-endian), so
 * both sides get the same bytes on any kind of computer.
 * ------------------------------------------------------------------ */
static void build_nonce(const unsigned char *salt, uint32_t counter,
                        unsigned char *nonce)
{
    memcpy(nonce, salt, SALT_LEN);                            // bytes 0..7  = salt

    nonce[SALT_LEN + 0] = (unsigned char)(counter >> 24);     // byte 8  = highest counter byte
    nonce[SALT_LEN + 1] = (unsigned char)(counter >> 16);     // byte 9
    nonce[SALT_LEN + 2] = (unsigned char)(counter >> 8);      // byte 10
    nonce[SALT_LEN + 3] = (unsigned char)(counter);           // byte 11 = lowest counter byte
}

/* ------------------------------------------------------------------
 * gcm_encrypt: AES-256-GCM encryption with additional authenticated data.
 *
 *   key             32-byte secret key
 *   nonce           12-byte nonce (must never repeat with the same key)
 *   aad, aad_len    bytes that are NOT encrypted but ARE covered by the tag
 *   plaintext(_len) data to encrypt (plaintext_len may be 0)
 *   ciphertext      output buffer, gets plaintext_len bytes
 *   tag             output buffer, gets 16 bytes
 *
 * Returns the ciphertext length, or -1 on error.
 * ------------------------------------------------------------------ */
static int gcm_encrypt(const unsigned char *key, const unsigned char *nonce,
                       const unsigned char *aad, int aad_len,
                       const unsigned char *plaintext, int plaintext_len,
                       unsigned char *ciphertext, unsigned char *tag)
{
    int len = 0;              // bytes produced by the latest OpenSSL call
    int ciphertext_len = 0;   // running total of ciphertext bytes

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();   // OpenSSL's workspace for this operation
    if (ctx == NULL) return -1;

    // Choose AES-256 in GCM mode (no key or nonce yet).
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto fail;

    // Tell OpenSSL the nonce is 12 bytes long.
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_LEN, NULL) != 1) goto fail;

    // Load the real key and nonce.
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto fail;

    // Feed the AAD. Output pointer NULL means: "do not encrypt this,
    // just include it in the tag calculation". Must come BEFORE the plaintext.
    if (EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len) != 1) goto fail;

    // Encrypt the data. Skipped when there is nothing to encrypt (END frame).
    if (plaintext_len > 0) {
        if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) != 1) goto fail;
        ciphertext_len = len;
    }

    // Finish the operation (writes 0 bytes in GCM, but completes the tag).
    if (EVP_EncryptFinal_ex(ctx, ciphertext + ciphertext_len, &len) != 1) goto fail;
    ciphertext_len += len;

    // Copy the finished 16-byte tag into 'tag'. Must come AFTER Final.
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) != 1) goto fail;

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext_len;

fail:                              // one shared cleanup spot for every error above
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/* ------------------------------------------------------------------
 * send_frame: encrypt one chunk and send it as one frame:
 *      [ length (4) ][ ciphertext (length) ][ tag (16) ]
 *
 *   sock_fd        connected socket
 *   key            the 32-byte key
 *   salt           the 8-byte per-connection salt
 *   counter        frame number, used to build the nonce
 *   plaintext      chunk bytes (may be NULL when plaintext_len is 0)
 *   plaintext_len  0..CHUNK_SIZE  (0 = the END frame)
 *
 * Returns 0 on success, -1 on failure.
 * ------------------------------------------------------------------ */
static int send_frame(int sock_fd, const unsigned char *key,
                      const unsigned char *salt, uint32_t counter,
                      const unsigned char *plaintext, uint32_t plaintext_len)
{
    unsigned char frame[FRAME_MAX];   // the whole frame is assembled here
    unsigned char nonce[NONCE_LEN];

    if (plaintext_len > CHUNK_SIZE) {
        fprintf(stderr, "chunk too large\n");
        return -1;
    }

    // 1. Nonce for this frame.
    build_nonce(salt, counter, nonce);

    // 2. Put the length at the start of the frame, in network byte order.
    uint32_t length_net = htonl(plaintext_len);
    memcpy(frame, &length_net, LENGTH_FIELD_LEN);

    // 3. Encrypt. AAD = the 4 length bytes we just wrote.
    //    Ciphertext goes right after the length field.
    //    Tag goes right after the ciphertext.
    int ct_len = gcm_encrypt(key, nonce,
                             frame, LENGTH_FIELD_LEN,
                             plaintext, (int)plaintext_len,
                             frame + LENGTH_FIELD_LEN,
                             frame + LENGTH_FIELD_LEN + plaintext_len);
    if (ct_len != (int)plaintext_len) {
        fprintf(stderr, "encryption failed\n");
        return -1;
    }

    // 4. Send the complete frame in one go.
    return send_all(sock_fd, frame, LENGTH_FIELD_LEN + plaintext_len + TAG_LEN);
}

/* ------------------------------------------------------------------
 * sender_run: the whole sender program (see senderAES.h).
 * ------------------------------------------------------------------ */
int sender_run(const char *server_ip, int port,
               const char *input_path, const char *key_path)
{
    int exit_code = 1;                 // stays 1 (failure) unless we reach the very end
    int sock_fd = -1;                  // -1 = "not opened yet"
    FILE *in_file = NULL;              // NULL = "not opened yet"

    unsigned char key[KEY_LEN];
    unsigned char salt[SALT_LEN];
    unsigned char buffer[CHUNK_SIZE];  // holds one chunk read from the file
    uint32_t frame_counter = 0;        // number of the next frame
    size_t bytes_read;
    struct sockaddr_in server_addr;

    // If the receiver closes the connection, write() should return an error
    // instead of killing the whole program with a SIGPIPE signal.
    signal(SIGPIPE, SIG_IGN);

    // 1. Load the secret key.
    if (load_key(key_path, key) != 0) goto cleanup;

    // 2. Create a TCP socket.
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket failed");
        goto cleanup;
    }

    // 3. Fill in the receiver's address and connect.
    memset(&server_addr, 0, sizeof(server_addr));        // zero the struct first
    server_addr.sin_family = AF_INET;                    // IPv4
    server_addr.sin_port = htons((uint16_t)port);        // port in network byte order
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IP address: %s\n", server_ip);
        goto cleanup;
    }

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect failed");
        goto cleanup;
    }
    printf("Sender: connected to %s:%d\n", server_ip, port);

    // 4. Create this connection's random salt and send it in clear.
    //    (The salt is not secret. It only has to be unique per connection.)
    if (RAND_bytes(salt, SALT_LEN) != 1) {
        fprintf(stderr, "could not generate random salt\n");
        goto cleanup;
    }
    if (send_all(sock_fd, salt, SALT_LEN) != 0) goto cleanup;

    // 5. Open the input file.
    in_file = fopen(input_path, "rb");
    if (in_file == NULL) {
        perror("fopen failed");
        goto cleanup;
    }

    // 6. Read the file chunk by chunk; send each chunk as one encrypted frame.
    while ((bytes_read = fread(buffer, 1, CHUNK_SIZE, in_file)) > 0) {

        // The last counter value is reserved for the END frame, so a
        // nonce is never reused even in the most extreme case.
        if (frame_counter == UINT32_MAX) {
            fprintf(stderr, "too many frames for one connection\n");
            goto cleanup;
        }

        if (send_frame(sock_fd, key, salt, frame_counter,
                       buffer, (uint32_t)bytes_read) != 0) goto cleanup;

        frame_counter++;
    }

    // fread returns 0 both at the end of the file AND on a read error.
    // If it was an error, do NOT send the END frame, so the receiver
    // knows the transfer is incomplete.
    if (ferror(in_file)) {
        fprintf(stderr, "error while reading the input file\n");
        goto cleanup;
    }

    // 7. Send the END frame: a normal frame with 0 bytes of data.
    if (send_frame(sock_fd, key, salt, frame_counter, NULL, 0) != 0) goto cleanup;

    printf("Sender: file sent (%u data frames + END frame).\n", (unsigned)frame_counter);
    exit_code = 0;                     // everything worked

cleanup:
    if (in_file != NULL) fclose(in_file);
    if (sock_fd >= 0) close(sock_fd);
    OPENSSL_cleanse(key, KEY_LEN);     // wipe the key from memory
    return exit_code;
}
