#!/bin/sh
# KV260 helper. Never loads a bitstream or a kernel driver.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
board_dir=$(dirname "$script_dir")
action=${1:-start}
[ "$#" -le 1 ] || { printf '%s\n' '[ERROR] Use one command; see help.' >&2; exit 2; }

binary=${SL_BINARY:-"$board_dir/secure_link_kv260"}
psk=${SL_PSK:-"$board_dir/link.psk"}
receive=${SL_RECEIVE:-"$board_dir/received"}
uio=${SL_UIO:-/dev/uio12}
serial=${SL_SERIAL:-}
baud=${SL_BAUD:-230400}
timeout=${SL_TIMEOUT_MS:-60000}

usage() {
    cat <<'HELP'
[HELP] board.sh [start|build|check|help]
[HELP] start  Check local devices, build once, and wait for the laptop.
[HELP] build  Build the KV260 serial-only binary without opening hardware.
[HELP] check  Inspect FPGA state and device paths without MMIO accesses.
[HELP] Configure with SL_SERIAL, SL_BAUD, SL_UIO, SL_TIMEOUT_MS, SL_PSK,
[HELP] SL_RECEIVE or SL_BINARY. SL_VERBOSE=1 enables trace.
[HELP] This script does not load a bitstream or driver.
HELP
}

build() {
    [ -x "$binary" ] || return 0
    for source in "$board_dir/aead_uio_api.c" "$board_dir/aead_uio_api.h" \
        "$board_dir/aead_uio_internal.h" "$script_dir/main.c" "$script_dir/protocol.c" \
        "$script_dir/protocol.h" "$script_dir/transfer.c" "$script_dir/transfer.h" \
        "$script_dir/transport.c" "$script_dir/transport.h"; do
        if [ ! -f "$source" ]; then
            printf '[ERROR] Missing source: %s\n' "$source" >&2
            return 2
        fi
        [ "$source" -nt "$binary" ] && return 0
    done
    return 1
}

build() {
    mkdir -p "$(dirname "$binary")"
    temporary=$(mktemp "$(dirname "$binary")/.secure-link-build.XXXXXX")
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    printf '%s\n' '[BUILD] Building KV260 serial-only CLI...'
    if ! "${CC:-cc}" -std=c99 -O2 -Wall -Wextra -Werror -DSL_SERIAL_ONLY \
        "$script_dir/main.c" "$script_dir/protocol.c" "$script_dir/transfer.c" \
        "$script_dir/transport.c" "$board_dir/aead_uio_api.c" -lcrypto -o "$temporary"; then
        printf '%s\n' '[ERROR] KV260 build failed; check compiler and OpenSSL development files.' >&2
        return 1
    fi
    mv -f "$temporary" "$binary"
    trap - EXIT HUP INT TERM
    printf '%s\n' '[OK] KV260 CLI built.'
}

check_environment() {
    state=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null || true)
    if [ "$state" != operating ]; then
        printf '%s\n' '[ERROR] FPGA manager is not operating; refusing to access UIO.' >&2
        return 1
    fi
    printf '%s\n' '[OK] FPGA manager: operating.'
    printf '%s\n' '[WARN] Confirm the matching bitstream and PL clock; manager state alone cannot verify the bus.'
    if [ -z "$serial" ]; then
        set -- /dev/serial/by-id/*CP2102*
        if [ "$#" -ne 1 ] || [ ! -c "$1" ]; then
            printf '%s\n' '[ERROR] CP2102 not uniquely found. Check cp210x or set SL_SERIAL.' >&2
            return 1
        fi
        serial=$1
    fi
    if [ ! -c "$serial" ] || [ ! -r "$serial" ] || [ ! -w "$serial" ]; then
        printf '[ERROR] Serial device unavailable or inaccessible: %s\n' "$serial" >&2
        return 1
    fi
    if [ ! -c "$uio" ] || [ ! -r "$uio" ] || [ ! -w "$uio" ]; then
        printf '[ERROR] UIO device unavailable or inaccessible: %s\n' "$uio" >&2
        return 1
    fi
    if [ ! -r "$psk" ]; then
        printf '%s\n' '[ERROR] PSK missing or unreadable. Provision the same key as the laptop.' >&2
        return 1
    fi
    printf '[OK] Serial: %s | UIO: %s | baud: %s\n' "$serial" "$uio" "$baud"
}

case "$action" in
    help|--help|-h) usage ;;
    build) build ;;
    check)
        check_environment
        printf '%s\n' '[INFO] No UIO register access performed by this check.'
        ;;
    start)
        check_environment
        build
        umask 077
        mkdir -p "$receive"
        "$binary" --check-key "$psk"
        printf '%s\n' '[INFO] Waiting for the laptop. Start its script within the timeout.'
        if [ "${SL_VERBOSE:-0}" = 1 ]; then
            exec "$binary" --role kv260 --serial "$serial" --baud "$baud" --uio "$uio" \
                --psk "$psk" --receive "$receive" --timeout-ms "$timeout" --verbose
        fi
        exec "$binary" --role kv260 --serial "$serial" --baud "$baud" --uio "$uio" \
            --psk "$psk" --receive "$receive" --timeout-ms "$timeout"
        ;;
    *) printf '[ERROR] Unknown command: %s\n' "$action" >&2; usage; exit 2 ;;
esac
