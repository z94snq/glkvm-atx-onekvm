#!/bin/sh
# Build a static glatx-bridge (runs in any container, no libraries needed).
# Uses Docker, so no local compiler is required.
set -e
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" gcc:12-bookworm \
    sh -c 'gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -static -o glatx-bridge main.c core.c && strip glatx-bridge'
ls -l glatx-bridge
