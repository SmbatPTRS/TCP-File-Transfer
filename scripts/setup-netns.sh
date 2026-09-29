#!/usr/bin/env bash
# Creates two network namespaces joined by a veth pair.
# Run with: sudo ./scripts/setup-netns.sh
set -euo pipefail

ip netns del sender-ns   2>/dev/null || true
ip netns del receiver-ns 2>/dev/null || true

ip netns add sender-ns
ip netns add receiver-ns

ip link add veth-send type veth peer name veth-recv

ip link set veth-send netns sender-ns
ip link set veth-recv netns receiver-ns

ip netns exec sender-ns ip addr add 10.0.0.1/24 dev veth-send
ip netns exec sender-ns ip link set veth-send up
ip netns exec sender-ns ip link set lo up

ip netns exec receiver-ns ip addr add 10.0.0.2/24 dev veth-recv
ip netns exec receiver-ns ip link set veth-recv up
ip netns exec receiver-ns ip link set lo up

ip netns exec sender-ns ping -c 2 10.0.0.2
echo "Network ready: sender 10.0.0.1 <-> receiver 10.0.0.2"
