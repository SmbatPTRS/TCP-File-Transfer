# TCP File Transfer over Linux Network Namespaces

A small C project that sends a file over TCP between two isolated
network namespaces on a single Linux machine, connected by a virtual
ethernet (veth) pair.

## Architecture

```
 sender-ns (10.0.0.1)                 receiver-ns (10.0.0.2)
 ┌───────────────────┐                ┌───────────────────┐
 │  sender  ──► veth-send ══════════ veth-recv ──► receiver │
 └───────────────────┘                └───────────────────┘
```

- `receiver` listens on `10.0.0.2:5000`, accepts one connection and
  writes every received byte to `receiver/output.txt`.
- `sender` connects to `10.0.0.2:5000` and streams a file.

## Requirements

- Linux with `iproute2` (the `ip` command)
- `gcc` and `make`
- `sudo` rights (creating namespaces needs them)

## Quick start

Run everything from the repository root.

```bash
# 1. Build
make

# 2. Create the virtual network (needs root)
sudo ./scripts/setup-netns.sh

# 3. Terminal A: start the receiver
sudo ip netns exec receiver-ns ./build/receiver

# 4. Terminal B: start the sender
sudo ip netns exec sender-ns ./build/sender

# 5. Check the result
diff examples/input.txt receiver/output.txt && echo "Files match"

# 6. Clean up when done
sudo ./scripts/teardown-netns.sh
```

## Project layout

| Path | Purpose |
|---|---|
| `src/` | C source code |
| `scripts/` | Network environment setup / teardown |
| `examples/` | Sample input file |
| `receiver/` | Output folder written by the receiver |

## Roadmap

- [ ] Send file name and size before the data
- [ ] Error handling for failed reads
- [ ] Support for multiple clients
