#!/usr/bin/env bash
# Removes everything created by setup-netns.sh
set -euo pipefail
ip netns del sender-ns   2>/dev/null || true
ip netns del receiver-ns 2>/dev/null || true
echo "Namespaces removed."
