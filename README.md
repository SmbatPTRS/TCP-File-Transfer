# Network: Plain TCP vs. AES-256-GCM File Transfer

A small C project on Linux that sends a file from one program (**sender**) to another (**receiver**) over TCP, in two versions:

| Version | Programs | Protection |
|---|---|---|
| **Plain (old)** | `build/sender`, `build/receiver` | None. The data travels as readable text and can be read, changed, faked or cut short. |
| **AES-GCM (new)** | `build/network sender`, `build/network receiver` | Every chunk is encrypted and authenticated with AES-256-GCM (OpenSSL). |

The two versions live side by side so that the weaknesses of the plain version can be **demonstrated in practice** and compared with the protected version.

The programs run inside two Linux **network namespaces** (`sender-ns` and `receiver-ns`) connected by a **veth pair**, so a whole "network" can be simulated on a single computer, and traffic can be captured with `tcpdump`.

---

## Table of contents

1. [Project structure](#1-project-structure)
2. [Requirements](#2-requirements)
3. [Test network setup](#3-test-network-setup)
4. [Create the secret key](#4-create-the-secret-key)
5. [Build](#5-build)
6. [Run the plain (old) version](#6-run-the-plain-old-version)
7. [Run the AES-GCM (new) version](#7-run-the-aes-gcm-new-version)
8. [Demonstrations: vulnerable vs. protected](#8-demonstrations-vulnerable-vs-protected)
9. [How the protected version works](#9-how-the-protected-version-works)
10. [Configuration](#10-configuration)
11. [Troubleshooting](#11-troubleshooting)
12. [Known limitations](#12-known-limitations)

---

## 1. Project structure

```
Network/
├── Makefile              Builds all three programs
├── README.md             This file
├── build/                Compiled programs and object files (created by make)
├── data/
│   ├── input.txt         The file the sender transmits
│   └── key.bin           Secret 32-byte key (YOU create this, see section 4)
├── output/
│   └── output.txt        The file the receiver saves
├── scripts/              Network namespace setup scripts
└── src/
    ├── sender.c          OLD  plain TCP sender
    ├── receiver.c        OLD  plain TCP receiver
    ├── main.c            NEW  entry point: "network sender" / "network receiver"
    ├── senderAES.h       NEW  interface of the encrypting sender
    ├── senderAES.c       NEW  encrypting sender
    ├── receiverAES.h     NEW  interface of the decrypting receiver
    └── receiverAES.c     NEW  decrypting receiver
```

The Makefile builds three separate programs, because the old files and the new `main.c` each contain their own `main()` function and cannot be linked together:

| Program | Built from |
|---|---|
| `build/sender` | `src/sender.c` |
| `build/receiver` | `src/receiver.c` |
| `build/network` | `src/main.c` + `src/senderAES.c` + `src/receiverAES.c` |

## 2. Requirements

- Linux (uses network namespaces, so it will not work on Windows or macOS)
- `gcc` and `make`
- OpenSSL development files (for the new version only)
- `iproute2` (provides the `ip` command, normally already installed)
- `tcpdump` (to watch the traffic)
- `netcat` (optional, only for the "fake sender" demonstration)
- `sudo` rights (namespaces and packet capture need root)

Install everything on Debian or Ubuntu:

```bash
sudo apt update
sudo apt install build-essential libssl-dev tcpdump netcat-openbsd iproute2
```

## 3. Test network setup

The programs expect this layout:

```
 sender-ns (10.0.0.1)                        receiver-ns (10.0.0.2)
 ┌───────────────┐                            ┌───────────────┐
 │ sender        │══ veth-send ═══ veth-recv ═│ receiver      │
 └───────────────┘                            └───────────────┘
```

Use the setup script in `scripts/` if you have one. The namespaces **disappear after a reboot**, so this has to be repeated then.

If you need to create it by hand, this is the equivalent (interface names `veth-send` and `veth-recv` are the ones used in this README):

```bash
sudo ip netns add sender-ns
sudo ip netns add receiver-ns

sudo ip link add veth-send type veth peer name veth-recv
sudo ip link set veth-send netns sender-ns
sudo ip link set veth-recv netns receiver-ns

sudo ip netns exec sender-ns   ip addr add 10.0.0.1/24 dev veth-send
sudo ip netns exec receiver-ns ip addr add 10.0.0.2/24 dev veth-recv

sudo ip netns exec sender-ns   ip link set lo up
sudo ip netns exec receiver-ns ip link set lo up
sudo ip netns exec sender-ns   ip link set veth-send up
sudo ip netns exec receiver-ns ip link set veth-recv up
```

Check that it works:

```bash
sudo ip netns list                                   # shows sender-ns and receiver-ns
sudo ip netns exec sender-ns ping -c 1 10.0.0.2      # must get a reply
sudo ip netns exec receiver-ns ip -br link           # shows the interface name for tcpdump
```

If your interface is not called `veth-recv`, use its real name wherever `tcpdump -i veth-recv` appears below.

## 4. Create the secret key

Only the **new** version needs a key. It is 32 random bytes stored in a file, and **both programs read the same file**. Create it once:

```bash
cd ~/Desktop/Network
head -c 32 /dev/urandom > data/key.bin
chmod 600 data/key.bin
```

Verify it:

```bash
ls -l data/key.bin        # the size must be exactly 32 bytes
```

> **Keep the key secret.** Anyone who has this file can read and forge your transfers. Add it to `.gitignore` so it is never uploaded:
>
> ```bash
> echo "data/key.bin" >> .gitignore
> ```

## 5. Build

```bash
cd ~/Desktop/Network
make clean
make            # builds all three programs
```

Other targets:

```bash
make plain      # only build/sender and build/receiver
make aes        # only build/network
make clean      # remove everything created by make
```

## 6. Run the plain (old) version

Always start from the project folder, because the old receiver uses the relative path `output/output.txt`:

```bash
cd ~/Desktop/Network
```

Use **three terminals**. The receiver must be listening before the sender connects.

| Terminal | Purpose | Command |
|---|---|---|
| 1 | Watch the wire | `sudo ip netns exec receiver-ns tcpdump -i veth-recv -A port 5000` |
| 2 | Receiver | `sudo ip netns exec receiver-ns ./build/receiver` |
| 3 | Sender | `sudo ip netns exec sender-ns ./build/sender` |

Then check the result:

```bash
sha256sum data/input.txt output/output.txt     # the two hashes must be equal
```

Terminal 1 (`tcpdump`) shows the contents of `input.txt` **in readable text**. That is the vulnerability.

Clean up before the next run (the file belongs to root):

```bash
sudo rm -f output/output.txt
```

## 7. Run the AES-GCM (new) version

Same three terminals, only the programs change. **Create the key first** (section 4).

| Terminal | Purpose | Command |
|---|---|---|
| 1 | Watch the wire | `sudo ip netns exec receiver-ns tcpdump -i veth-recv -A port 5000` |
| 2 | Receiver | `sudo ip netns exec receiver-ns ./build/network receiver` |
| 3 | Sender | `sudo ip netns exec sender-ns ./build/network sender` |

Expected output:

```
# receiver
Receiver: listening on 10.0.0.2:5000 ...
Receiver: client connected from 10.0.0.1:<random port>
Receiver: transfer complete and verified (N data frames), saved to /home/smbat/Desktop/Network/output/output.txt

# sender
Sender: connected to 10.0.0.2:5000
Sender: file sent (N data frames + END frame).
```

Check the result:

```bash
sha256sum data/input.txt output/output.txt     # the two hashes must be equal
ls -l output/                                   # only output.txt, no output.txt.part
```

Terminal 1 (`tcpdump`) shows **only unreadable bytes**. Add `-X` to the tcpdump command to see them as hex.

### Get more than one frame

Each frame carries up to 1024 bytes of the file, so a tiny `input.txt` produces just one frame. To see the frame counter at work, use a bigger file:

```bash
cp data/input.txt data/input.backup.txt
for i in $(seq 1 200); do echo "Line $i: Transfer 100 dollars to Anna"; done > data/input.txt
wc -c data/input.txt        # about 7.5 KB, so about 8 data frames
```

Restore the original afterwards:

```bash
cp data/input.backup.txt data/input.txt
```

## 8. Demonstrations: vulnerable vs. protected

Run each test against **both** versions and compare. Delete the output between runs: `sudo rm -f output/output.txt*`.

| Attack | How to run it | Plain version | AES-GCM version |
|---|---|---|---|
| **Reading the data** | `tcpdump -A` during a transfer | File text visible | Only unreadable bytes |
| **Truncation** | Start the receiver and sender, press `Ctrl+C` in the sender terminal mid-transfer (use a big file) | Prints "transfer complete", keeps a partial file | Prints an error, **no** `output.txt` is created |
| **Fake sender** | With the receiver running: `echo "FAKE DATA" \| sudo ip netns exec sender-ns nc 10.0.0.2 5000` | `output.txt` contains `FAKE DATA` | Rejected, nothing saved |
| **Wrong key** | Start the receiver, replace `data/key.bin` with new random bytes, then start the sender | Not applicable | `frame 0: AUTHENTICATION FAILED`, nothing saved |

For the wrong-key test, restore the key afterwards:

```bash
cp data/key.bin data/key.good       # BEFORE changing it
head -c 32 /dev/urandom > data/key.bin
# ... run the test ...
cp data/key.good data/key.bin       # restore
```

## 9. How the protected version works

The sender cuts the file into chunks of at most 1024 bytes and sends each chunk as one **frame**. Every frame is encrypted and authenticated with AES-256-GCM.

### Wire format

```
Once, at the start of the connection:

    [ salt: 8 bytes ]

Then one frame per chunk, and a final END frame:

    [ length: 4 bytes ][ ciphertext: <length> bytes ][ tag: 16 bytes ]
       sent in clear        encrypted chunk            authentication
       (but protected)                                 proof

END frame = same layout with length = 0 (no ciphertext).
```

### The nonce is never sent

Both sides build the same 12-byte nonce for every frame:

```
nonce (12 bytes) = [ salt: 8 bytes ][ frame counter: 4 bytes ]
```

- The **salt** is random, chosen by the sender once per connection. It is not secret. It only makes sure that two connections using the same key do not reuse a nonce.
- The **frame counter** starts at 0 and grows by 1 per frame. Each side counts on its own, so it is never transmitted. If a frame is removed, duplicated or reordered, the counters no longer match, the nonce differs, and the tag check fails.

### What is protected

| Property | How |
|---|---|
| Confidentiality | Chunks are encrypted with AES-256 |
| Integrity and authenticity | Every frame has a GCM tag. Without the key, a valid tag cannot be produced |
| Length field | Sent in clear, but included in the tag as additional authenticated data (AAD) |
| Replay, deletion and reordering inside a connection | The frame counter is part of the nonce |
| Truncated transfers | The file is only accepted after a **valid END frame** arrives |

### Safety rules in the receiver

- Nothing is written to the output before its frame passes the tag check.
- Data goes to `output.txt.part` first. It is renamed to `output.txt` only after the END frame is verified, and deleted if anything fails.
- A frame whose announced length is larger than 1024 bytes is rejected before any attempt to read it.

## 10. Configuration

**New version:** all settings are `#define` lines at the top of `src/main.c`:

| Setting | Default |
|---|---|
| `PORT` | `5000` |
| `RECEIVER_IP` | `10.0.0.2` |
| `INPUT_FILE` | `/home/smbat/Desktop/Network/data/input.txt` |
| `OUTPUT_FILE` | `/home/smbat/Desktop/Network/output/output.txt` |
| `KEY_FILE` | `/home/smbat/Desktop/Network/data/key.bin` |

Change them if your username or folder is different, then run `make`.

**Old version:** the sender has the input path hard-coded in `src/sender.c`, and the receiver writes to the relative path `output/output.txt`.

**Constants that must match** between `senderAES.c` and `receiverAES.c` (they define the wire format): `CHUNK_SIZE`, `KEY_LEN`, `SALT_LEN`, `NONCE_LEN`, `TAG_LEN`, `LENGTH_FIELD_LEN`.

## 11. Troubleshooting

| Message | Cause | Fix |
|---|---|---|
| `openssl/evp.h: No such file or directory` | OpenSSL development files missing | `sudo apt install libssl-dev` |
| `Makefile: missing separator` | Tabs were turned into spaces | Lines under a rule must start with a real TAB |
| `cannot open key file: No such file or directory` | `data/key.bin` does not exist | Section 4 |
| `key file is too short` | The key file is not 32 bytes | Recreate it with `head -c 32 /dev/urandom` |
| `bind failed: Cannot assign requested address` | The namespace has no `10.0.0.2` (setup not run, or a reboot happened) | Section 3 |
| `connect failed: Connection refused` | The receiver is not running yet | Start the receiver first |
| `bind failed: Address already in use` (old receiver) | The previous run is still closing | Wait about a minute. The new receiver avoids this |
| `fopen failed: No such file or directory` (old receiver) | Started from the wrong folder | `cd ~/Desktop/Network` |
| `Permission denied` when deleting `output.txt` | The file belongs to root | `sudo rm output/output.txt` |
| `frame 0: AUTHENTICATION FAILED` | The two sides use different keys | Make sure both read the same `data/key.bin` |

## 12. Known limitations

This is a learning project. The protected version does **not** defend against:

- **Metadata:** IP addresses, ports, the number of frames, their sizes and timing remain visible.
- **Replay of a whole recorded session:** replay *inside* a connection is detected, but an attacker who records an entire connection and sends it again to a new one is accepted. A fix is a fresh random challenge from the receiver at the start of each connection.
- **Shared key file:** anyone who gets `key.bin` can read and forge transfers. Real systems run a key exchange per connection.
- **One direction only:** data flows from sender to receiver. A two-way version needs a different key (or a direction marker in the nonce) for each direction.
- **One connection at a time:** the receiver serves a single sender and then exits.
- **Nonce uniqueness across connections** relies on the 8-byte random salt. A collision is extremely unlikely, but not mathematically impossible.

## Roadmap

| Stage | Status |
|---|---|
| Plain TCP transfer | Done |
| AES-GCM learned in a standalone program | Done |
| Per-chunk encryption over TCP, with counter nonce and END frame | Done |
| Separate Encryptor and Decryptor with a UDP tunnel | Planned |
| Two-way communication | Planned |
| Full-frame (AF_PACKET / TUN) version | Optional |
