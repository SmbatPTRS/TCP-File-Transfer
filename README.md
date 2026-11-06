# Step-by-step demonstration: all three versions

Start here for a classroom demonstration. The detailed reference follows this guide.

| Version | Flow | What to demonstrate |
|---|---|---|
| Plain TCP | Sender → receiver | File arrives, but packet capture reveals the text |
| Integrated AES-256-GCM | Encrypting sender → decrypting receiver | File arrives, network contents are encrypted |
| Separate encoder/decoder | Sender → encoder → decoder → listener | File handling and encryption run in separate processes |

All three use TCP. In the third version, the listener's command-line role is named
`receiver`. Encryption protects the encoder-to-decoder link; the other two links
carry plaintext locally inside each namespace.

## 1. Install requirements (first time only)

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install build-essential libssl-dev openssl iproute2 iputils-ping tcpdump python3
```

## 2. Prepare files and programs

```bash
cd ~/Desktop/Network
make prepare
```

This builds all versions, creates `data/` and `output/`, creates a sample
`data/input.txt` if missing, and creates a private 32-byte `data/key.bin` if
missing. Existing input and key files are preserved. Both encryption endpoints
read the same key file. Do not display or share the key.

Inspect the input so you know what to look for in the capture:

```bash
head -n 5 data/input.txt
wc -c data/input.txt
```

Use readable text for this demonstration. The old versions have paths pointing
to `/home/smbat/Desktop/Network`; on another computer, update the paths in
`src/sender.c` and `src/main.c`, then rebuild. The third version accepts paths
as arguments and does not need those source changes.

## 3. Create the simulated network

```bash
make up
make status
```

`make up` creates this layout and checks it with ping:

```text
sender-ns (10.0.0.1) -- veth-send === veth-recv -- receiver-ns (10.0.0.2)
```

A namespace acts like a separate computer's network environment. The veth pair
is its virtual cable. The namespaces share the host filesystem.

**Running `make up` twice is supported.** Each run stops processes in these two
lab namespaces, removes the previous network, and recreates it. Use it before
a fresh demonstration, not between launching the individual roles. These names
and interface names are reserved for this lab; do not use them for other work.
Namespaces disappear after reboot, so run `make up` again then.

## 4. Open five terminals

In every terminal:

```bash
cd ~/Desktop/Network
```

Commands below stay in the foreground. A receiver showing `Listening` is waiting
normally. Start each role in the specified order. Each transfer handles one file
and exits; restart the roles to send another file.

## 5. Version 1 — plain TCP

Say: “The first version sends file bytes directly, without encryption.”

Run these in order:

| Terminal | Command | Purpose |
|---|---|---|
| 1 | `make capture` | Watch TCP port 5000 as readable text |
| 2 | `make plain-receiver` | Wait for a sender and save incoming bytes |
| 3 | `make plain-sender` | Read `data/input.txt` and send it |

After sender and receiver finish, in terminal 3:

```bash
make check
head -n 5 output/output.txt
```

Expected: `SUCCESS: files match`. In terminal 1, point out recognizable sentences
from the input in packet contents. TCP delivered the data, but provided no
application encryption or authentication.

Press **Ctrl+C in terminal 1** to stop capture before moving on. Leave the network up.

## 6. Version 2 — integrated AES-256-GCM

Say: “The sender now encrypts, and the receiver authenticates and decrypts.”

Run these in order:

| Terminal | Command | Purpose |
|---|---|---|
| 1 | `make capture` | Watch the same TCP port 5000 |
| 2 | `make aes-receiver` | Wait for encrypted frames |
| 3 | `make aes-sender` | Encrypt and send the input |

After completion, in terminal 3:

```bash
make check
head -n 5 output/output.txt
```

Expected: matching files, and the receiver reports `transfer complete and verified`.
This run replaces the previous `output/output.txt` after successful verification.
The capture now contains binary-looking data instead of the original sentences.
Random printable characters may still appear.

The file is split into chunks of at most 1024 bytes. AES-GCM encrypts each chunk
and produces an authentication tag. A random salt and frame counter form the
nonce; the length is authenticated too. The receiver verifies each frame and
requires an authenticated END frame before renaming `output.txt.part` to
`output.txt`. An interrupted encrypted transfer is rejected.

Press **Ctrl+C in terminal 1**. Leave the network up.

## 7. Version 3 — separate encoder, decoder, and listener

Say: “Now the file programs and cryptography programs have separate jobs.”

```text
sender-ns                                      receiver-ns
Sender → Encoder ───── encrypted TCP ────────→ Decoder → Listener
       local plaintext                               local plaintext
```

Run these in order, each in its own terminal:

| Terminal | Command | Purpose |
|---|---|---|
| 1 | `make capture-relay` | Watch encrypted TCP port 5001 across the virtual cable |
| 2 | `make relay-listener` | Listen on local port 5002 and save the output |
| 3 | `make relay-decoder` | Listen on 10.0.0.2:5001, verify/decrypt, then forward locally |
| 4 | `make relay-encoder` | Listen on local port 5000, encrypt, then send to the decoder |
| 5 | `make relay-sender` | Read the same input and send it to the encoder |

Each role should print `complete` and exit. In terminal 5:

```bash
make check-relay
head -n 5 output/relay-output.txt
```

Expected: `SUCCESS: files match`. The capture again shows encrypted data.
The decoder buffers verified plaintext in a temporary file until the encrypted
END frame verifies, then forwards it to the listener. The listener writes a
`.part` file and renames it after a complete local transfer.

The sender and listener do not load keys. Only encoder and decoder do.
`127.0.0.1` means the local namespace, so the plaintext links stay inside their
respective network environments. The network-facing encrypted link connects
10.0.0.1 to 10.0.0.2.

Press **Ctrl+C in terminal 1**.

## 8. Run automated checks

```bash
make test
```

Expected: seven `PASS` lines for small, large/binary, empty, automatically created
input, wrong key, truncated encrypted stream, and truncated local stream.
These tests cover the third version and run on localhost without namespaces.

## 9. Stop everything and remove the network

```bash
make down
```

This stops any remaining processes inside `sender-ns` and `receiver-ns`, including
waiting receivers and packet captures, deletes both namespaces, and removes the
veth pair. It also handles veth interfaces left in the host after partial setup.
Other terminals may report that their processes were terminated; that is expected.

**It is safe to run `make down` again.** Already absent lab resources are skipped.
To check:

```bash
make status
```

The two lab namespaces should no longer appear. Other unrelated namespaces, if
present, are left alone. Start a new demonstration with `make up`, then run roles
again in the documented order. The network commands do not delete keys or files.
`make clean` removes compiled programs; it does not stop the network.

`make down` manages namespace-based runs from this guide. If you run the optional
localhost-only commands from the reference below, stop those foreground programs
with Ctrl+C in their terminals.

## 10. Command reference and common problems

Run `make help` for the command summary. All role, capture, and network targets
invoke sudo where needed; do not run the build itself with sudo.

For the third version, customize paths consistently, for example:

```bash
make relay-sender INPUT=data/another.txt
make check-relay INPUT=data/another.txt
```

`INPUT`, `OUTPUT`, and `KEY` are Makefile variables for the third version. Pass the
same `OUTPUT` to listener/check, and the same `KEY` to encoder/decoder. The older
versions retain their source-defined settings.

| Problem | What to do |
|---|---|
| Namespace does not exist | Run `make up` before starting roles |
| Connection refused | Start the receiving roles first, in the listed order |
| Address already in use / previous run left waiting | Run `make down`, then `make up`, then restart roles |
| Plain receiver cannot immediately rebind after a run | The old receiver lacks SO_REUSEADDR; wait about a minute or recreate the lab with `make up` |
| Wrong output when checking | Confirm the receiver reported success; a failed protected transfer preserves an older output |
| Authentication failed | Ensure encoder/sender and decoder/receiver use the same 32-byte key |
| Output file belongs to root | Expected for sudo namespace runs; use sudo if you need to edit/remove it |
| Port 5000 capture is empty during version 3 | Use `make capture-relay`, which captures port 5001 |

The protected versions are learning examples: whole-session replay is possible,
random salts provide probabilistic nonce uniqueness, and there is no key exchange.
The third version has no final delivery acknowledgement back to the sender, so
show listener success and matching files to confirm end-to-end delivery.

---

# Detailed project reference

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


## Third version: separate sender, encoder, decoder, receiver

This version keeps file handling and encryption in separate processes:

```
sender --plain TCP--> encoder --AES-256-GCM over TCP--> decoder --plain TCP--> receiver
```

Build with `make` or `make relay`. All four roles use `build/relay`.
The original plain and AES programs are still available.

### Quick local test

From the project directory, create the output directory and a key once:

```bash
mkdir -p output
# Run only if you do not already have data/key.bin:
(umask 077; openssl rand -out data/key.bin 32)
```

Keep this key private. Only encoder and decoder need it; both must use the
same exact 32-byte key. No key is embedded in the source code.

Start these commands in four terminals, in the order shown:

```bash
# 1. Receiver: writes the finished plaintext file
./build/relay receiver output/relay-output.txt 127.0.0.1 5002

# 2. Decoder: listens for encrypted data, then forwards verified plaintext
./build/relay decoder 127.0.0.1 5001 127.0.0.1 5002 data/key.bin

# 3. Encoder: accepts plaintext from sender and encrypts it
./build/relay encoder 127.0.0.1 5000 127.0.0.1 5001 data/key.bin

# 4. Sender: reads any file; creates a short sample if this path is missing
./build/relay sender data/relay-input.txt 127.0.0.1 5000
```

The input parent directory must exist. The receiver's output directory must
also exist. Any file type is supported, including binary and empty files.

Compare the result:

```bash
cmp data/relay-input.txt output/relay-output.txt
# No output means they match.
```

Each role handles one transfer and exits. Restart the roles for another transfer.
Addresses, ports, input/output paths, and key path are command-line arguments:

```
relay sender INPUT ENCODER_IP PORT
relay encoder LISTEN_IP PORT DECODER_IP PORT KEY_FILE
relay decoder LISTEN_IP PORT RECEIVER_IP PORT KEY_FILE
relay receiver OUTPUT LISTEN_IP PORT
```

### Using the existing two namespaces

Run receiver and decoder inside `receiver-ns`, and encoder and sender inside
`sender-ns`. Use the same commands above with these changes:

- Receiver: prefix with `sudo ip netns exec receiver-ns`.
- Decoder: same prefix; listen on `10.0.0.2` instead of `127.0.0.1`.
  Keep its receiver destination `127.0.0.1:5002`.
- Encoder: prefix with `sudo ip netns exec sender-ns`; keep its listening
  address `127.0.0.1`, and set its decoder destination to `10.0.0.2:5001`.
- Sender: prefix with `sudo ip netns exec sender-ns`; keep `127.0.0.1:5000`.

Only the encrypted middle link then crosses the veth pair. Capture port 5001
to inspect that link.

### Simple protocol and limitations

The encrypted middle link uses the existing AES version's wire format:
8-byte random salt, then frames containing a 4-byte length, ciphertext, and
16-byte GCM tag. The nonce is salt plus a 32-bit counter. Length is authenticated,
and a verified zero-length END frame marks completion.

The decoder spools authenticated chunks into a temporary file, and forwards
plaintext only after END verifies. The temporary file is removed when closed.
The receiver writes `OUTPUT.part` and renames it only after receiving the local
completion marker; a detected failure removes the partial file and preserves any
previous output. This requires temporary disk space for the complete transfer.

Local plaintext links use a 4-byte length followed by up to 1024 data bytes;
a zero length marks completion. This detects incomplete streams but does not
authenticate local traffic. Keep those links on trusted hosts, preferably bound
to loopback as above. A malicious process able to reach them can submit plaintext.

As with the original AES version, whole-session replay is possible and nonce
uniqueness depends on random salts. There is no timeout, concurrency, key exchange,
or final acknowledgement back to the sender. Sender success means its local send
completed; check receiver success and compare files to confirm delivery. Abrupt
process termination can leave the receiver's `.part` file.

Run the automated local checks (Python 3, no extra packages):

```bash
python3 tests/test-relay.py
```

Checks cover text, binary, empty and automatically created input files, wrong
keys, truncated encrypted streams, and incomplete receiver streams.
