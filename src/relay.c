/* Four roles, connected by TCP. Only encoder -> decoder is encrypted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#define CHUNK_SIZE 1024
#define KEY_LEN 32
#define SALT_LEN 8
#define NONCE_LEN 12
#define TAG_LEN 16
#define LENGTH_FIELD_LEN 4
#define FRAME_MAX (4 + CHUNK_SIZE + TAG_LEN)

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

static int read_exact(int fd, unsigned char *buf, size_t len)
{
    size_t total = 0;                            // how many bytes we already have

    while (total < len) {
        // read into the free part of the buffer, asking only for what is still missing
        ssize_t n = read(fd, buf + total, len - total);

        if (n < 0) {
            if (errno == EINTR) {                // interrupted by a signal, just retry
                continue;
            }
            perror("read failed");
            return -1;
        }
        if (n == 0) {                            // 0 means: the sender closed the connection
            fprintf(stderr, "connection closed before all expected bytes arrived\n");
            return -1;
        }
        total += (size_t)n;                      // n bytes arrived, advance
    }
    return 0;
}

static int gcm_decrypt(const unsigned char *key, const unsigned char *nonce,
                       const unsigned char *aad, int aad_len,
                       const unsigned char *ciphertext, int ciphertext_len,
                       unsigned char *tag, unsigned char *plaintext)
{
    int len = 0;              // bytes produced by the latest OpenSSL call
    int plaintext_len = 0;    // running total of plaintext bytes

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();   // OpenSSL's workspace for this operation
    if (ctx == NULL) return -1;

    // Choose AES-256 in GCM mode (no key or nonce yet).
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto fail;

    // Tell OpenSSL the nonce is 12 bytes long.
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_LEN, NULL) != 1) goto fail;

    // Load the real key and nonce.
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto fail;

    // Feed the AAD (output pointer NULL = "only include it in the tag check").
    // Must be exactly the same bytes, in the same order, as on the sender side.
    if (EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len) != 1) goto fail;

    // Decrypt the data. Skipped when there is nothing to decrypt (END frame).
    if (ciphertext_len > 0) {
        if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1) goto fail;
        plaintext_len = len;
    }

    // Give OpenSSL the tag we RECEIVED. It will compare it with the tag it
    // calculates itself. This must be done BEFORE calling Final.
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, tag) != 1) goto fail;

    // The moment of truth: returns > 0 only if the tags are equal.
    // <= 0 means the data, the length, the nonce or the key was wrong.
    if (EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len, &len) <= 0) goto fail;
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return plaintext_len;

fail:                              // one shared cleanup spot for every error above
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/* Return one connected socket. Listening roles accept one connection and exit. */
static int connection(const char *ip, const char *port_text, int listening)
{
    char *end;
    long port = strtol(port_text, &end, 10);
    if (!*port_text || *end || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", port_text);
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", ip);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    if (!listening) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        perror("connect");
    } else {
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) perror("bind");
        else if (listen(fd, 1) < 0) perror("listen");
        else {
            printf("Listening on %s:%ld\n", ip, port);
            fflush(stdout);
            int peer;
            do { peer = accept(fd, NULL, NULL); } while (peer < 0 && errno == EINTR);
            if (peer < 0) perror("accept");
            close(fd);
            return peer;
        }
    }
    close(fd);
    return -1;
}

static int load_key(const char *path, unsigned char *key)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("key file"); return -1; }
    size_t n = fread(key, 1, KEY_LEN, f);
    int extra = fgetc(f);
    int failed = ferror(f);
    fclose(f);
    if (n != KEY_LEN || extra != EOF || failed) {
        fprintf(stderr, "key must contain exactly 32 bytes\n");
        return -1;
    }
    return 0;
}

/* Local plaintext links use [4-byte length][data], ending with length 0.
   This detects interrupted connections without requiring encryption here. */
static int local_send(int fd, const unsigned char *buf, uint32_t len)
{
    uint32_t net = htonl(len);
    return send_all(fd, (const unsigned char *)&net, 4) || send_all(fd, buf, len);
}

static int local_read(int fd, unsigned char *buf)
{
    uint32_t net;
    if (read_exact(fd, (unsigned char *)&net, 4)) return -1;
    uint32_t len = ntohl(net);
    if (len > CHUNK_SIZE) { fprintf(stderr, "invalid local chunk size\n"); return -1; }
    if (read_exact(fd, buf, len)) return -1;
    return (int)len;
}

static int sender(const char *path, const char *ip, const char *port)
{
    FILE *f = fopen(path, "rb");
    if (!f && errno == ENOENT) {
        f = fopen(path, "wb");
        if (!f) { perror("create input"); return 1; }
        int failed = fputs("Hello from the four-stage sender!\n", f) == EOF;
        if (fclose(f) != 0) failed = 1;
        if (failed) return 1;
        f = fopen(path, "rb");
    }
    if (!f) { perror("input"); return 1; }
    int fd = connection(ip, port, 0), result = 1;
    if (fd < 0) { fclose(f); return 1; }
    unsigned char buf[CHUNK_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        if (local_send(fd, buf, (uint32_t)n) != 0) goto done;
    if (ferror(f)) { perror("input read"); goto done; }
    if (local_send(fd, NULL, 0)) goto done;
    result = 0;
done:
    fclose(f);
    close(fd);
    return result;
}

static int encoder(char **a, const unsigned char *key)
{
    int in = connection(a[0], a[1], 1);
    if (in < 0) return 1;
    int out = connection(a[2], a[3], 0), result = 1;
    if (out < 0) { close(in); return 1; }
    unsigned char salt[SALT_LEN], buf[CHUNK_SIZE];
    uint32_t counter = 0;
    if (RAND_bytes(salt, sizeof(salt)) != 1 || send_all(out, salt, sizeof(salt))) goto done;
    ssize_t n;
    while ((n = local_read(in, buf)) > 0) {
        if (counter == UINT32_MAX || send_frame(out, key, salt, counter, buf, (uint32_t)n)) goto done;
        counter++;
    }
    if (n < 0 || send_frame(out, key, salt, counter, NULL, 0)) goto done;
    result = 0;
done:
    close(in); close(out);
    return result;
}

/* Decoder buffers verified plaintext until END. It contacts the receiver
   only after the entire encrypted transfer is authenticated. */
static int decoder(char **a, const unsigned char *key)
{
    int in = connection(a[0], a[1], 1), out = -1, result = 1;
    if (in < 0) return 1;
    FILE *f = tmpfile();
    if (!f) { perror("temporary file"); close(in); return 1; }
    unsigned char salt[SALT_LEN], nonce[NONCE_LEN], len_bytes[4];
    unsigned char ct[CHUNK_SIZE], plain[CHUNK_SIZE], tag[TAG_LEN];
    uint32_t counter = 0;
    if (read_exact(in, salt, sizeof(salt))) goto done;
    for (;;) {
        uint32_t net;
        if (read_exact(in, len_bytes, 4)) goto done;
        memcpy(&net, len_bytes, 4);
        uint32_t len = ntohl(net);
        if (len > CHUNK_SIZE || (len && counter == UINT32_MAX)) {
            fprintf(stderr, "invalid frame length or exhausted counter\n"); goto done;
        }
        if (read_exact(in, ct, len) || read_exact(in, tag, sizeof(tag))) goto done;
        build_nonce(salt, counter, nonce);
        int n = gcm_decrypt(key, nonce, len_bytes, 4, ct, (int)len, tag, plain);
        if (n < 0) { fprintf(stderr, "authentication failed at frame %u\n", counter); goto done; }
        if (!len) break;
        if (fwrite(plain, 1, (size_t)n, f) != (size_t)n) { perror("temporary write"); goto done; }
        counter++;
    }
    if (fflush(f) || fseek(f, 0, SEEK_SET)) { perror("temporary rewind"); goto done; }
    out = connection(a[2], a[3], 0);
    if (out < 0) goto done;
    size_t n;
    while ((n = fread(plain, 1, sizeof(plain), f)) > 0)
        if (local_send(out, plain, (uint32_t)n)) goto done;
    if (ferror(f) || local_send(out, NULL, 0)) goto done;
    result = 0;
done:
    fclose(f); close(in);
    if (out >= 0) close(out);
    return result;
}

static int receiver(const char *path, const char *ip, const char *port)
{
    int fd = connection(ip, port, 1);
    if (fd < 0) return 1;
    char *part = malloc(strlen(path) + 6);
    if (!part) { close(fd); return 1; }
    sprintf(part, "%s.part", path);
    FILE *f = fopen(part, "wb");
    int result = 1;
    if (!f) { perror("output"); goto done; }
    unsigned char buf[CHUNK_SIZE];
    ssize_t n;
    while ((n = local_read(fd, buf)) > 0)
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { perror("output write"); goto done; }
    if (n < 0) goto done;
    { int err = fclose(f); f = NULL; if (err) { perror("output close"); goto done; } }
    if (rename(part, path)) { perror("output rename"); goto done; }
    result = 0;
done:
    if (f) fclose(f);
    if (result) remove(part);
    free(part); close(fd);
    return result;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int result;
    if (argc == 5 && !strcmp(argv[1], "sender"))
        result = sender(argv[2], argv[3], argv[4]);
    else if (argc == 5 && !strcmp(argv[1], "receiver"))
        result = receiver(argv[2], argv[3], argv[4]);
    else if (argc == 7 && (!strcmp(argv[1], "encoder") || !strcmp(argv[1], "decoder"))) {
        unsigned char key[KEY_LEN] = {0};
        result = load_key(argv[6], key) ? 1 :
                 (!strcmp(argv[1], "encoder") ? encoder(argv + 2, key) : decoder(argv + 2, key));
        OPENSSL_cleanse(key, sizeof(key));
    } else {
        fprintf(stderr, "Usage:\n  %s sender INPUT ENCODER_IP PORT\n"
                "  %s encoder LISTEN_IP PORT DECODER_IP PORT KEY_FILE\n"
                "  %s decoder LISTEN_IP PORT RECEIVER_IP PORT KEY_FILE\n"
                "  %s receiver OUTPUT LISTEN_IP PORT\n", argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    if (!result) printf("%s: complete\n", argv[1]);
    return result;
}
