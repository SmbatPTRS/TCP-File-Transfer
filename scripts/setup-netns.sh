#!/usr/bin/env bash
# Recreate this lab from a clean state. Safe to repeat; stops prior lab runs.
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo, or use make up." >&2
    exit 1
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
bash "$script_dir/teardown-netns.sh"
# Clean up partial configuration if any setup command fails.
trap 'bash "$script_dir/teardown-netns.sh"' ERR
ip netns add sender-ns
ip netns add receiver-ns
ip link add veth-send type veth peer name veth-recv
ip link set veth-send netns sender-ns
ip link set veth-recv netns receiver-ns
ip -n sender-ns addr add 10.0.0.1/24 dev veth-send
ip -n receiver-ns addr add 10.0.0.2/24 dev veth-recv
ip -n sender-ns link set lo up
ip -n receiver-ns link set lo up
ip -n sender-ns link set veth-send up
ip -n receiver-ns link set veth-recv up
ip netns exec sender-ns ping -c 2 -W 2 10.0.0.2
trap - ERR
echo "Network ready: sender 10.0.0.1 <-> receiver 10.0.0.2"
