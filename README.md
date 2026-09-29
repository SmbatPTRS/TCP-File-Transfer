# TCP File Transfer over Linux Network Namespaces

A small C project that transfers a file over TCP between two isolated Linux network namespaces on the same machine. The namespaces communicate through a virtual Ethernet (`veth`) pair.

## Architecture

| Component | Network namespace | IP address | Role |
|-----------|-------------------|------------|------|
| Sender | `sender-ns` | `10.0.0.1` | Sends the input file |
| Receiver | `receiver-ns` | `10.0.0.2` | Receives and saves the file |

The virtual Ethernet pair connects `veth-send` in the sender namespace to `veth-recv` in the receiver namespace.

- `receiver` listens on `10.0.0.2:5000`, accepts one connection, and writes the received bytes to `output/output.txt`.
- `sender` connects to `10.0.0.2:5000` and sends `data/input.txt`.

## Requirements

- Linux
- `iproute2` (provides the `ip` command)
- `gcc` and `make`
- `sudo` privileges for managing network namespaces
- `tcpdump` for inspecting network traffic (optional)

## Quick Start

Run all commands from the repository root.

### 1. Build the project

```bash
make
```

### 2. Create the virtual network

```bash
sudo ./scripts/setup-netns.sh
```

### 3. Start the receiver

In one terminal, run:

```bash
sudo ip netns exec receiver-ns ./build/receiver
```

### 4. Start the sender

In another terminal, run:

```bash
sudo ip netns exec sender-ns ./build/sender
```

### 5. Verify the transferred file

After the transfer finishes, compare the input and output:

```bash
diff data/input.txt output/output.txt && echo "Files match"
```

If the files are identical, the command prints `Files match`.

### 6. Remove the virtual network

When finished, run:

```bash
sudo ./scripts/teardown-netns.sh
```

## Security Limitations

This project transfers files over plain TCP without encryption or application-level authentication.

### No confidentiality

File contents are transmitted as plaintext. Anyone with the ability to capture traffic on the connection can inspect the transferred data.

### No cryptographic integrity or authentication

TCP provides reliable, ordered delivery and a checksum for detecting accidental corruption. It does not provide cryptographic protection against intentional modification or verify the identity of the sender.

The application therefore cannot establish that a received file came from a trusted sender or was protected against deliberate tampering.

This implementation is intended for learning and local experimentation. It should not be used to transfer sensitive data without additional protection.

## Inspecting Plaintext Traffic

Use `tcpdump` to observe the unencrypted file contents during a transfer.

Build the project and create the virtual network before following these steps.

### 1. Start the packet capture

In Terminal A, run:

```bash
sudo ip netns exec receiver-ns tcpdump -i veth-recv -nn -s 0 -A 'tcp port 5000'
```

The `-A` option displays packet contents as ASCII text, and `-s 0` captures full packets.

### 2. Start the receiver

In Terminal B, run:

```bash
sudo ip netns exec receiver-ns ./build/receiver
```

### 3. Run the sender

In Terminal C, run:

```bash
sudo ip netns exec sender-ns ./build/sender
```

### 4. Inspect the captured output

Look in Terminal A for packets containing application data. If the input file contains readable text, such as `hello from the sender`, that text will appear in the capture output.

File contents may be split across several packets. This demonstrates the lack of encryption; it does not demonstrate a tampering attack.

Press `Ctrl+C` to stop the capture.

## Project Layout

| Path | Purpose |
|------|---------|
| `src/` | C source files |
| `scripts/` | Network namespace setup and teardown scripts |
| `data/` | Sample input file |
| `build/` | Compiled sender and receiver executables |
| `output/` | Files saved by the receiver |

## Roadmap

- [ ] Add authenticated encryption with AES-GCM, including secure key and nonce management
- [ ] Send the file name and size before transferring its contents
- [ ] Improve error handling for failed reads
- [ ] Support multiple clients
