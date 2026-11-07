# Network: TCP File Transfer with AES-256-GCM
# Hello Mr Mirzoyan jan (•ᴗ•)
A small Linux C project demonstrating three ways to transfer a file over TCP:

| Version | Flow | Purpose |
|---|---|---|
| 1. Plain TCP | Sender → receiver | Show file delivery without encryption or authentication |
| 2. Integrated AES-256-GCM | Encrypting sender → decrypting receiver | Protect file contents and verify received frames |
| 3. Separate encoder and decoder | Sender → encoder → decoder → listener | Separate file handling from encryption and decryption |

All three versions are included. The demonstration uses two Linux network
namespaces connected by a virtual cable, so one computer can simulate both ends
of a network. Packet capture shows the difference between plaintext and encrypted
traffic. In version 3, the listener's command-line role is called `receiver`.

## Project structure

```text
Network/
├── Makefile                  Build, network, demonstration, and test commands
├── README.md                 This guide
├── src/
│   ├── sender.c              Version 1: plain TCP sender
│   ├── receiver.c            Version 1: plain TCP receiver
│   ├── main.c                Version 2: entry point and configuration
│   ├── senderAES.c           Version 2: encryption and sending
│   ├── senderAES.h           Encrypting sender interface
│   ├── receiverAES.c         Version 2: verification, decryption, and saving
│   ├── receiverAES.h         Decrypting receiver interface
│   └── relay.c               Version 3: sender, encoder, decoder, and listener
├── scripts/
│   ├── setup-netns.sh        Reset and create the simulated network
│   └── teardown-netns.sh     Stop lab processes and remove the network
├── tests/
│   └── test-relay.py         Automated checks for version 3
├── data/
│   ├── input.txt             Default input for all three demonstrations
│   ├── input.backup.txt      Original sample input backup
│   └── key.bin               Private 32-byte key; generated locally, Git-ignored
├── output/
│   ├── output.txt            Result from version 1 or 2
│   └── relay-output.txt      Result from version 3
└── build/                    Generated executables, objects, and dependencies
```

Generated files appear after preparation or a transfer. The three versions use
four executables:

| Executable | Source | Roles |
|---|---|---|
| `build/sender` | `src/sender.c` | Plain sender |
| `build/receiver` | `src/receiver.c` | Plain receiver |
| `build/network` | `src/main.c`, `src/senderAES.c`, `src/receiverAES.c` | AES sender or receiver |
| `build/relay` | `src/relay.c` | Sender, encoder, decoder, or listener |

## 1. Requirements and preparation

Use Linux with sudo access for network namespaces and packet capture. On
Debian/Ubuntu, install the tools once:

```bash
sudo apt update
sudo apt install build-essential libssl-dev openssl iproute2 iputils-ping tcpdump python3
```

From the project folder:

```bash
cd ~/Desktop/Network
make prepare
```

`make prepare` builds every version, creates `data/` and `output/`, creates a
sample input if missing, and generates a private 32-byte key if missing. Existing
input and key files are preserved. Both encryption endpoints must read the same
key. Keep it private; it is already excluded from Git.

Check the input and key size without printing the key:

```bash
head -n 5 data/input.txt
wc -c data/input.txt data/key.bin
```

Use readable text for the packet-capture demonstration. The key must be 32 bytes.

**If the project is in another directory or belongs to another user:** versions
1 and 2 still contain absolute paths pointing to `/home/smbat/Desktop/Network`.
Update the input path in `src/sender.c` and the file paths in `src/main.c`, then
run `make` again. Version 3 accepts paths as arguments; see configuration below.
Run build commands as your normal user, rather than with sudo.

## 2. Start the simulated network

```bash
make up
make status
```

The setup script creates this layout and checks connectivity with ping:

```text
sender-ns                                   receiver-ns
10.0.0.1 -- veth-send === virtual cable === veth-recv -- 10.0.0.2
```

Each namespace has its own network environment, like a separate computer, while
sharing the host filesystem. Successful setup prints ping replies followed by:

```text
Network ready: sender 10.0.0.1 <-> receiver 10.0.0.2
```

Running `make up` again resets the lab: it stops processes in these two namespaces,
removes the previous network, and recreates it. Reserve the names `sender-ns`,
`receiver-ns`, `veth-send`, and `veth-recv` for this project. Run setup before a
fresh demonstration, not between launching its individual roles. Run it again
after reboot because namespaces do not persist.

## 3. Run and compare all three versions

Open five terminals. In **every terminal**, enter:

```bash
cd ~/Desktop/Network
```

Follow the terminal order in each table. Commands stay in the foreground;
`Listening` means a process is waiting normally. Each role handles one transfer
and exits, so restart the receiving roles before repeating a transfer.

Keep the network up between versions. Stop the packet capture with **Ctrl+C**
before starting the next version's capture.

### Version 1: plain TCP

The sender reads the input and sends its bytes directly. The receiver writes
those bytes to `output/output.txt`.

Run these commands in order, in their respective terminals:

| Terminal | Command | Action |
|---|---|---|
| 1 | `make capture` | Watch TCP port 5000 with tcpdump |
| 2 | `make plain-receiver` | Start the receiver |
| 3 | `make plain-sender` | Send the input file |

After sender and receiver finish, run in terminal 3:

```bash
make check
head -n 5 output/output.txt
```

Expected: `SUCCESS: files match`. Terminal 1 should show recognizable sentences
from `data/input.txt` in the packets. Explain that TCP delivered the file, but
anyone able to capture this traffic can read its contents. This version has no
application-level authentication either.

Press **Ctrl+C in terminal 1**. Continue with version 2.

### Version 2: integrated AES-256-GCM

The sender now reads and encrypts the input. The receiver verifies and decrypts
frames before accepting the completed file.

| Terminal | Command | Action |
|---|---|---|
| 1 | `make capture` | Watch TCP port 5000 again |
| 2 | `make aes-receiver` | Start the decrypting receiver |
| 3 | `make aes-sender` | Encrypt and send the file |

After both programs finish, run in terminal 3:

```bash
make check
head -n 5 output/output.txt
```

Expected: `SUCCESS: files match`, and the receiver reports
`transfer complete and verified`. Successful delivery replaces the output from
version 1. During reception, data goes to `output.txt.part`; after the END frame
verifies, that file is renamed to `output.txt`.

Terminal 1 now shows binary-looking packet contents instead of the original
sentences. Some random printable characters are normal. Explain that the network
carries encrypted chunks and authentication tags, while the saved file contains
the original plaintext.

Press **Ctrl+C in terminal 1**. Continue with version 3.

### Version 3: separate encoder, decoder, and listener

Four processes now divide the work:

```text
sender-ns                                      receiver-ns
Sender → Encoder ───── encrypted TCP ────────→ Decoder → Listener
       plaintext                                     plaintext
       on loopback                                   on loopback
```

- Sender reads the file and sends plaintext locally to the encoder.
- Encoder encrypts and sends frames across the virtual cable.
- Decoder verifies the complete encrypted transfer, then forwards plaintext locally.
- Listener saves the completed plaintext file.

Run these commands in order:

| Terminal | Command | Action |
|---|---|---|
| 1 | `make capture-relay` | Watch encrypted traffic on TCP port 5001 |
| 2 | `make relay-listener` | Listen locally on port 5002 |
| 3 | `make relay-decoder` | Listen on 10.0.0.2:5001; forward to the listener |
| 4 | `make relay-encoder` | Listen locally on port 5000; connect to the decoder |
| 5 | `make relay-sender` | Send `data/input.txt` to the encoder |

Each role should print `complete` and exit. In terminal 5:

```bash
make check-relay
head -n 5 output/relay-output.txt
```

Expected: `SUCCESS: files match`. The capture shows encrypted data. Only encoder
and decoder load the key; sender and listener handle plaintext.

The local address `127.0.0.1` refers to the current namespace. The plaintext links
therefore stay within their respective network environments. Only the encrypted
middle link crosses the veth pair.

Press **Ctrl+C in terminal 1** when finished.

## 4. Run automated tests

```bash
make test
```

These checks exercise version 3 over localhost without requiring namespaces or
sudo. Expected output includes:

```text
PASS small
PASS large
PASS empty
PASS missing
PASS wrong key
PASS truncated encrypted stream
PASS truncated local stream
```

The checks cover matching text/binary/empty files, sample creation for a missing
input, wrong-key rejection, interrupted encrypted reception, and preservation of
an existing output when the final local stream is incomplete. Versions 1 and 2
are verified manually with the demonstrations and `make check` above.

## 5. Stop everything and repeat safely

```bash
make down
make status
```

`make down` stops remaining processes inside the two lab namespaces, including
waiting receivers and tcpdump, deletes the namespaces, and removes the veth pair.
It also removes lab veth interfaces left in the host after interrupted setup.
Terminated foreground commands in other terminals may print an error; this is
expected during cleanup.

Cleanup is safe to repeat: absent lab resources are skipped. Other unrelated
namespaces are left alone. After cleanup, `make status` should no longer list
`sender-ns` or `receiver-ns`.

For a fresh demonstration:

```bash
make up
```

Then launch the chosen version's roles again in the documented order. Network
setup/cleanup preserves input, output, and key files. A failed protected transfer
preserves an existing final output, so always check the receiving program's
success message before comparing files. Abrupt termination can leave a `.part`
file; a later transfer to that path overwrites it.

`make clean` removes build artifacts; it does not stop the network. `make down`
only stops processes inside the lab namespaces. For the optional localhost run
below, use Ctrl+C in each terminal to stop any waiting processes.

## 6. Command and configuration reference

Run `make help` for a quick reminder. Role, capture, and network commands invoke
sudo where required.

| Command | Purpose |
|---|---|
| `make` or `make all` | Build all four executables |
| `make prepare` | Build and create missing default input/key files |
| `make plain`, `make aes`, `make relay` | Build one version |
| `make up`, `make down`, `make status` | Reset/start, stop, or inspect the lab network |
| `make capture`, `make capture-relay` | Capture ports 5000 or 5001 |
| `make check`, `make check-relay` | Compare the input and received file |
| `make test` | Run version 3's automated tests |
| `make clean` | Remove generated objects, dependencies, and executables |

### File paths and settings

| Version | Configuration |
|---|---|
| Plain TCP | IP/port and absolute input path in `src/sender.c`; listening IP/port and relative output path in `src/receiver.c` |
| Integrated AES | `PORT`, `RECEIVER_IP`, `INPUT_FILE`, `OUTPUT_FILE`, and `KEY_FILE` in `src/main.c` |
| Separate encoder/decoder | Command-line arguments; Makefile defaults shown below |

For version 3, the Makefile defaults are `INPUT=data/input.txt`,
`OUTPUT=output/relay-output.txt`, and `KEY=data/key.bin`. To use another input:

```bash
make relay-sender INPUT=data/another.txt
make check-relay INPUT=data/another.txt
```

Pass a custom `OUTPUT` consistently to `relay-listener` and `check-relay`, and a
custom `KEY` consistently to `relay-encoder` and `relay-decoder`. Parent directories
must exist. The version 3 sender creates sample text if its input file is missing.
It also supports binary and empty files.

The executable accepts these arguments directly:

```text
./build/relay sender INPUT ENCODER_IP PORT
./build/relay encoder LISTEN_IP PORT DECODER_IP PORT KEY_FILE
./build/relay decoder LISTEN_IP PORT RECEIVER_IP PORT KEY_FILE
./build/relay receiver OUTPUT LISTEN_IP PORT
```

### Optional version 3 run without namespaces

After `make prepare`, run these in four separate terminals, in this order.
These commands need neither sudo nor `make up`:

```bash
# Terminal 1: listener
./build/relay receiver output/relay-output.txt 127.0.0.1 5002
```

```bash
# Terminal 2: decoder
./build/relay decoder 127.0.0.1 5001 127.0.0.1 5002 data/key.bin
```

```bash
# Terminal 3: encoder
./build/relay encoder 127.0.0.1 5000 127.0.0.1 5001 data/key.bin
```

```bash
# Terminal 4: sender, then comparison
./build/relay sender data/input.txt 127.0.0.1 5000
make check-relay
```

## 7. How the encrypted protocol works

Both protected versions use the same encrypted frame format:

```text
Connection starts with:
    [random salt: 8 bytes]

Each data frame:
    [length: 4 bytes][ciphertext: length bytes][GCM tag: 16 bytes]

Final END frame:
    [length: 0][GCM tag: 16 bytes]
```

Data frames contain at most 1024 plaintext bytes. Length uses network byte order
and is sent in clear, but included as additional authenticated data (AAD), so
changing it invalidates the tag. Oversized frames are rejected before their
contents are read.

Both endpoints reconstruct each frame's nonce locally:

```text
nonce: 12 bytes = random connection salt: 8 bytes + frame counter: 4 bytes
```

The counter starts at zero and advances per data frame. Changing frame order,
duplicating frames, or deleting frames causes authentication failure or a missing
END frame. Counter overflow is prevented. The salt is public; its randomness
reduces the chance of nonce reuse across transfers with the same key.

Version 2 writes verified chunks to `.part` and renames it after END verifies.
Version 3's decoder first stores verified chunks in a temporary file, forwards
them only after END verifies, and closes the temporary file afterward. This needs
disk space for the complete transfer. Its listener separately writes `.part`
and renames it after successful local completion.

Version 3's plaintext links also have framing: a 4-byte length followed by up to
1024 data bytes, ending with zero length. This detects incomplete connections;
it does not authenticate the plaintext links.

## 8. Troubleshooting

| Problem | What to do |
|---|---|
| Namespace does not exist | Run `make up`, including after reboot |
| Connection refused | Start the receiving roles first in the listed order |
| A previous run is still waiting / address in use | Run `make down`, then `make up`, then restart roles |
| Plain receiver cannot immediately bind again | It lacks SO_REUSEADDR; wait about a minute or reset the lab with `make up` |
| Cannot bind to 10.0.0.2 | Check `make status`; reset the network with `make up` |
| Missing input/output path | Run from the project directory; check the absolute paths for versions 1 and 2 |
| Missing or invalid key | Run `make prepare`; both endpoints need the same 32-byte file |
| Authentication failed | Check that both encryption endpoints use the same key and that the stream was not modified |
| Comparison passes after a failed transfer | An older final output may remain; check the receiver's success message too |
| Output belongs to root | Expected for sudo namespace runs; use sudo if you need to edit or remove it |
| Capture is empty in version 3 | Use `make capture-relay` for port 5001 |
| Missing OpenSSL headers | Install `libssl-dev`, then rebuild |

## 9. Limitations

This is an educational project. The protected versions encrypt and authenticate
file data, but have these limits:

- A complete recorded encrypted session can be replayed in a new connection.
- Nonce uniqueness across connections depends on the random 8-byte salt; collisions are possible.
- Keys are shared through a file; there is no key exchange or independent identity verification.
- Addresses, ports, frame sizes, and timing remain visible.
- Transfers are one-way, with one connection per process run and no application timeout.
- Version 3's local plaintext links must stay on trusted hosts, preferably loopback. A process able to reach them can submit plaintext.
- There is no final delivery acknowledgement back to the sender. Confirm receiver/listener success and matching files for end-to-end delivery.

Possible future extensions include receiver challenges against session replay,
a UDP tunnel, two-way communication, and a TUN/AF_PACKET implementation.
