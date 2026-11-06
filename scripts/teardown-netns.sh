#!/usr/bin/env bash
# Stop this lab and remove its namespaces and virtual cable. Safe to repeat.
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo, or use make down." >&2
    exit 1
fi
for ns in sender-ns receiver-ns; do
    if ip netns list | awk '{print $1}' | grep -Fxq "$ns"; then
        # Stop processes first: otherwise a process can keep a deleted
        # namespace (and its interfaces) alive.
        pids=$(ip netns pids "$ns")
        if [[ -n "$pids" ]]; then
            kill $pids 2>/dev/null || true
            for attempt in {1..20}; do
                [[ -z "$(ip netns pids "$ns")" ]] && break
                sleep 0.1
            done
            pids=$(ip netns pids "$ns")
            [[ -z "$pids" ]] || kill -KILL $pids 2>/dev/null || true
        fi
        ip netns del "$ns"
    fi
done
# Also handle an interrupted setup that left a veth in the host namespace.
for dev in veth-send veth-recv; do
    if ip link show dev "$dev" >/dev/null 2>&1; then
        ip link del dev "$dev"
    fi
done
echo "Lab stopped: namespaces and veth interfaces removed."
