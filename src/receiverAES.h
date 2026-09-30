/*
 * receiverAES.h  -  Public interface of the decrypting receiver (Client B).
 *
 * Only ONE function is exposed. Everything else in receiverAES.c is
 * 'static', which means it is private to that file.
 */

#ifndef RECEIVER_AES_H   // include guard, same purpose as in senderAES.h
#define RECEIVER_AES_H

/*
 * receiver_run: listen for one sender, receive encrypted frames, verify and
 *               decrypt each of them, and save the file.
 *
 *   listen_ip    IP address to listen on, as text (e.g. "10.0.0.2")
 *   port         TCP port to listen on
 *   output_path  where the finished file is saved. While the transfer is in
 *                progress the data goes to "<output_path>.part"; the file is
 *                renamed to output_path only after the END frame is verified.
 *   key_path     path of the file that holds the 32-byte secret key
 *
 * Returns 0 on success, 1 on failure (so main can return it directly
 * as the program's exit code).
 */
int receiver_run(const char *listen_ip, int port,
                 const char *output_path, const char *key_path);

#endif /* RECEIVER_AES_H */
