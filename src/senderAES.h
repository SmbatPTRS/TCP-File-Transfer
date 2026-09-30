/*
 * senderAES.h  -  Public interface of the encrypting sender (Client A).
 *
 * Only ONE function is exposed. Everything else in senderAES.c is
 * 'static', which means it is private to that file.
 */

#ifndef SENDER_AES_H     // "include guard": prevents this header from being
#define SENDER_AES_H     // processed twice if it is included more than once

/*
 * sender_run: connect to the receiver, encrypt a file in chunks with
 *             AES-256-GCM, and send it.
 *
 *   server_ip   IP address of the receiver, as text (e.g. "10.0.0.2")
 *   port        TCP port of the receiver
 *   input_path  path of the file to send
 *   key_path    path of the file that holds the 32-byte secret key
 *
 * Returns 0 on success, 1 on failure (so main can return it directly
 * as the program's exit code).
 */
int sender_run(const char *server_ip, int port,
               const char *input_path, const char *key_path);

#endif /* SENDER_AES_H */
