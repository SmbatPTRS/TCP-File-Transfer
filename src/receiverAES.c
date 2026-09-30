/*
 * receiverAES.c  -  Client B: receives encrypted frames over TCP, checks the
 *                   authentication tag of every frame, decrypts, and saves
 *                   the file.
 *
 * WIRE FORMAT it expects (must match senderAES.c exactly):
 *
 *   1) Once, at the very start:   [ salt: 8 bytes ]
 *   2) Then one frame per chunk:  [ length: 4 bytes ][ ciphertext: length bytes ][ tag: 16 bytes ]
 *   3) Finally an END frame:      same layout with length = 0 (no ciphertext bytes)
 *
 * The nonce of each frame is rebuilt locally as:
 *   nonce (12 bytes) = [ salt: 8 bytes ][ this side's own frame counter: 4 bytes ]
 *
 * SAFETY RULES this program follows:
 *   - No plaintext is written to the file until its frame passed the tag check.
 *   - The data goes into "<output>.part" and is renamed to the real name only
 *     after a VALID END frame arrived. So the real output file is never
 *     incomplete or unverified.
 *   - A frame whose length field is larger than CHUNK_SIZE is rejected before
 *     we try to read that many bytes.
 */

#include <stdio.h>       // printf, perror, fprintf, snprintf, fopen, fwrite, fclose, remove, rename
#include <string.h>      // memset, memcpy
#include <stdint.h>      // uint32_t, uint16_t, UINT32_MAX
#include <errno.h>       // errno, EINTR
#include <unistd.h>      // read, close
#include <arpa/inet.h>   // sockaddr_in, htons, ntohl, ntohs, inet_pton, inet_ntop
#include <sys/socket.h>  // socket, setsockopt, bind, listen, accept
#include <openssl/evp.h>     // EVP_* decryption functions
#include <openssl/crypto.h>  // OPENSSL_cleanse

#include "receiverAES.h"

/* ---------- Sizes (all in bytes) ----------
 * IMPORTANT: these must be identical in senderAES.c, because both
 * sides must agree on the exact layout of the bytes on the wire. */
#define CHUNK_SIZE       1024   // max plaintext bytes per frame
#define KEY_LEN          32     // AES-256 key
#define SALT_LEN         8      // random per-connection part of the nonce
#define NONCE_LEN        12     // = SALT_LEN + 4 bytes of counter
#define TAG_LEN          16     // GCM authentication tag
#define LENGTH_FIELD_LEN 4      // the "length" field at the start of a frame

#define PATH_MAX_LEN     512    // size of the buffer that holds "<output>.part"

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
 * read_exact: read exactly 'len' bytes from the socket 'fd' into 'buf'.
 *
 * TCP is a stream: one read() may return fewer bytes than asked, because
 * TCP does not keep message boundaries. So we loop until we have them all.
 *
 * Returns 0 on success, -1 on failure. "The other side closed the
 * connection before all bytes arrived" counts as a failure, because a
 * frame that is cut in half is useless.
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * build_nonce: create the 12-byte nonce = salt (8 bytes) + counter (4 bytes).
 * Identical to the sender's version: the counter is written most-significant
 * byte first (big-endian), so both sides always get the same bytes.
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
 * gcm_decrypt: AES-256-GCM decryption + tag check, with AAD.
 *
 *   key              32-byte secret key
 *   nonce            12-byte nonce (same one the sender used for this frame)
 *   aad, aad_len     the bytes that were authenticated but not encrypted
 *                    (here: the 4 length bytes exactly as they arrived)
 *   ciphertext(_len) the encrypted bytes (ciphertext_len may be 0)
 *   tag              the 16-byte tag that arrived with the frame
 *   plaintext        output buffer, gets ciphertext_len bytes
 *
 * Returns the plaintext length, or -1 if the tag check FAILED (or any other
 * error). When it returns -1 the 'plaintext' buffer may contain garbage and
 * must NEVER be used.
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * receiver_run: the whole receiver program (see receiverAES.h).
 * ------------------------------------------------------------------ */
int receiver_run(const char *listen_ip, int port,
                 const char *output_path, const char *key_path)
{
    int exit_code = 1;                 // stays 1 (failure) unless we reach the very end
    int server_fd = -1;                // listening socket, -1 = "not opened yet"
    int client_fd = -1;                // socket of the accepted connection
    FILE *out_file = NULL;             // NULL = "not opened yet"
    int part_file_exists = 0;          // 1 while "<output>.part" exists and is not yet renamed

    char tmp_path[PATH_MAX_LEN];       // will hold "<output_path>.part"

    unsigned char key[KEY_LEN];
    unsigned char salt[SALT_LEN];
    unsigned char nonce[NONCE_LEN];
    unsigned char length_bytes[LENGTH_FIELD_LEN];   // the 4 length bytes exactly as received
    unsigned char ciphertext[CHUNK_SIZE];
    unsigned char tag[TAG_LEN];
    unsigned char plaintext[CHUNK_SIZE];

    uint32_t frame_counter = 0;        // number of the frame we expect next
    uint32_t length_net;               // length as it came from the wire
    uint32_t length;                   // length converted to a normal number
    int plaintext_len;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    char peer_ip[INET_ADDRSTRLEN];     // the sender's IP as text, for the log message
    int reuse = 1;

    // 1. Load the secret key.
    if (load_key(key_path, key) != 0) goto cleanup;

    // 2. Build the name of the temporary file: "<output_path>.part".
    //    snprintf never writes more than sizeof(tmp_path) bytes; it returns
    //    how many characters it WOULD have written, so we can detect a
    //    path that was too long and got cut.
    int written = snprintf(tmp_path, sizeof(tmp_path), "%s.part", output_path);
    if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
        fprintf(stderr, "output path is too long\n");
        goto cleanup;
    }

    // 3. Create a TCP socket (a network endpoint managed by the kernel).
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        goto cleanup;
    }

    // Allow the port to be reused right after the program stops. Without this,
    // restarting the receiver quickly can fail with "Address already in use".
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 4. Fill in the address we want to listen on.
    memset(&server_addr, 0, sizeof(server_addr));        // zero the struct first
    server_addr.sin_family = AF_INET;                    // IPv4
    server_addr.sin_port = htons((uint16_t)port);        // port in network byte order
    if (inet_pton(AF_INET, listen_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IP address: %s\n", listen_ip);
        goto cleanup;
    }

    // 5. Attach the socket to that IP and port.
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        goto cleanup;
    }

    // 6. Start listening. The number is how many finished connections may wait in the queue.
    if (listen(server_fd, 1) < 0) {
        perror("listen failed");
        goto cleanup;
    }
    printf("Receiver: listening on %s:%d ...\n", listen_ip, port);

    // 7. Wait here until a sender connects. accept() returns a NEW socket
    //    (client_fd) that belongs to this one connection.
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept failed");
        goto cleanup;
    }
    inet_ntop(AF_INET, &client_addr.sin_addr, peer_ip, sizeof(peer_ip));
    printf("Receiver: client connected from %s:%d\n", peer_ip, ntohs(client_addr.sin_port));

    // 8. Open the temporary output file ("w" = write, "b" = binary).
    out_file = fopen(tmp_path, "wb");
    if (out_file == NULL) {
        perror("fopen failed");
        goto cleanup;
    }
    part_file_exists = 1;

    // 9. The first 8 bytes of the connection are the salt.
    if (read_exact(client_fd, salt, SALT_LEN) != 0) goto cleanup;

    // 10. Frame loop. It ends only when a valid END frame arrives (break)
    //     or when something goes wrong (goto cleanup).
    for (;;) {

        // 10a. Read the 4-byte length field.
        if (read_exact(client_fd, length_bytes, LENGTH_FIELD_LEN) != 0) goto cleanup;

        // Convert from network byte order (big-endian) to a normal number.
        memcpy(&length_net, length_bytes, LENGTH_FIELD_LEN);
        length = ntohl(length_net);

        // 10b. Sanity checks BEFORE trusting the length. Without the first
        //      one, an attacker could announce a huge length and make us wait for it.
        if (length > CHUNK_SIZE) {
            fprintf(stderr, "frame %u: announced length %u is too large, stopping\n",
                    (unsigned)frame_counter, (unsigned)length);
            goto cleanup;
        }
        // The sender never uses the very last counter value for data, only for END.
        if (length > 0 && frame_counter == UINT32_MAX) {
            fprintf(stderr, "frame counter exhausted, stopping\n");
            goto cleanup;
        }

        // 10c. Read the ciphertext (nothing to read when length is 0) and the tag.
        if (read_exact(client_fd, ciphertext, length) != 0) goto cleanup;
        if (read_exact(client_fd, tag, TAG_LEN) != 0) goto cleanup;

        // 10d. Build the nonce from OUR OWN counter. We never trust a counter
        //      sent by the other side: if a frame was removed, duplicated or
        //      reordered, our counter no longer matches the one the sender
        //      used, so the tag check below fails.
        build_nonce(salt, frame_counter, nonce);

        // 10e. Decrypt AND verify. AAD = the 4 length bytes exactly as received.
        plaintext_len = gcm_decrypt(key, nonce,
                                    length_bytes, LENGTH_FIELD_LEN,
                                    ciphertext, (int)length,
                                    tag, plaintext);
        if (plaintext_len < 0) {
            fprintf(stderr, "frame %u: AUTHENTICATION FAILED (data was changed, "
                            "reordered, replayed, or the key is wrong). Stopping.\n",
                    (unsigned)frame_counter);
            goto cleanup;                // nothing from this frame is written
        }

        // 10f. Length 0 and a valid tag = the authenticated END frame. Done.
        if (length == 0) {
            break;
        }

        // 10g. The frame is verified: only now do we write it to the file.
        if (fwrite(plaintext, 1, (size_t)plaintext_len, out_file) != (size_t)plaintext_len) {
            perror("fwrite failed");
            goto cleanup;
        }

        frame_counter++;                 // expect the next frame number
    }

    // 11. Close the file first (this flushes buffered data to disk and can
    //     report a write error), then give it its real name.
    {
        FILE *f = out_file;
        out_file = NULL;                 // so cleanup does not close it a second time
        if (fclose(f) != 0) {
            perror("fclose failed");
            goto cleanup;
        }
    }
    if (rename(tmp_path, output_path) != 0) {   // atomic on the same filesystem
        perror("rename failed");
        goto cleanup;
    }
    part_file_exists = 0;                // it now has its final name

    printf("Receiver: transfer complete and verified (%u data frames), saved to %s\n",
           (unsigned)frame_counter, output_path);
    exit_code = 0;                       // everything worked

cleanup:
    if (out_file != NULL) fclose(out_file);
    if (part_file_exists) remove(tmp_path);      // never leave an incomplete file behind
    if (client_fd >= 0) close(client_fd);
    if (server_fd >= 0) close(server_fd);
    OPENSSL_cleanse(key, KEY_LEN);               // wipe the key from memory
    return exit_code;
}
