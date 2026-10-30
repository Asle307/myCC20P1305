#!/bin/sh
# Laptop helper. Does not program the FPGA or replace existing PSKs.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
software_dir=$(dirname "$script_dir")
project_dir=$(dirname "$software_dir")
script_path="$script_dir/laptop.sh"
action=${1:-start}
[ "$#" -le 1 ] || { printf '%s\n' '[ERROR] Use one command; see help.' >&2; exit 2; }

binary=${SL_BINARY:-"$project_dir/build/host/secure_link"}
psk=${SL_PSK:-"$project_dir/artifacts/secure-chat/link.psk"}
receive=${SL_RECEIVE:-"$project_dir/artifacts/secure-chat/received"}
address=${SL_BLUETOOTH_ADDR:-00:25:11:02:85:B9}
channel=${SL_CHANNEL:-1}
timeout=${SL_TIMEOUT_MS:-60000}
board=${SL_BOARD_HOST:-debian@192.168.2.132}

usage() {
    cat <<'HELP'
[HELP] laptop.sh [start|build|check|deploy|keygen|copy-key|help]
[HELP] start     Build if needed and connect to the paired HC-05.
[HELP] build     Build the laptop binary only.
[HELP] check     Build if needed and check the local PSK.
[HELP] deploy    Copy source and scripts to ~/Asle/secure-link on the board.
[HELP] keygen    Create a new local 32-byte PSK; never replaces a file.
[HELP] copy-key  Copy the existing PSK to the board; never replaces a file.
[HELP] Configure with SL_BLUETOOTH_ADDR, SL_CHANNEL, SL_TIMEOUT_MS,
[HELP] SL_PSK, SL_RECEIVE, SL_BOARD_HOST or SL_BINARY. SL_VERBOSE=1 enables trace.
[HELP] NixOS: when development libraries are missing, start may use nix develop.
HELP
}

needs_build() {
    [ -x "$binary" ] || return 0
    for source in "$software_dir/aead_uio_api.c" "$software_dir/aead_uio_api.h" \
        "$software_dir/aead_uio_internal.h" "$script_dir/main.c" "$script_dir/protocol.c" \
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
    if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists openssl bluez; then
        if [ "${SL_AUTO_NIX:-1}" = 1 ] && [ "${SL_IN_NIX_SHELL:-0}" != 1 ] && command -v nix >/dev/null 2>&1; then
            printf '%s\n' '[BUILD] Opening a Nix development environment (packages may be downloaded).'
            export SL_IN_NIX_SHELL=1
            exec nix develop --impure --expr 'with import <nixpkgs> {}; mkShell { packages = [ gcc pkg-config openssl bluez ]; }' -c "$script_path" "$action"
        fi
        printf '%s\n' '[ERROR] Missing pkg-config, OpenSSL or BlueZ development files.' >&2
        return 1
    fi
    mkdir -p "$(dirname "$binary")"
    temporary=$(mktemp "$(dirname "$binary")/.secure-link-build.XXXXXX")
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    printf '%s\n' '[BUILD] Building laptop CLI...'
    if ! "${CC:-cc}" -std=c99 -O2 -Wall -Wextra -Werror \
        "$script_dir/main.c" "$script_dir/protocol.c" "$script_dir/transfer.c" \
        "$script_dir/transport.c" "$software_dir/aead_uio_api.c" \
        $(pkg-config --cflags --libs openssl bluez) -o "$temporary"; then
        printf '%s\n' '[ERROR] Laptop build failed.' >&2
        return 1
    fi
    mv -f "$temporary" "$binary"
    trap - EXIT HUP INT TERM
    printf '%s\n' '[OK] Laptop CLI built.'
}

ensure_build() {
    if needs_build; then
        build
    else
        status=$?
        [ "$status" -eq 1 ] || return "$status"
    fi
}

case "$action" in
    help|--help|-h) usage ;;
    build) build ;;
    check)
        ensure_build
        "$binary" --check-key "$psk"
        printf '[INFO] HC-05: %s, RFCOMM channel %s\n' "$address" "$channel"
        ;;
    keygen)
        ensure_build
        umask 077
        mkdir -p "$(dirname "$psk")"
        "$binary" --keygen "$psk"
        ;;
    copy-key)
        ensure_build
        "$binary" --check-key "$psk"
        case "$board" in -*) printf '%s\n' '[ERROR] Invalid board host.' >&2; exit 2;; esac
        printf '%s\n' '[INFO] Copying PSK via SSH; an existing board key will not be replaced.'
        if ! ssh "$board" 'set -eu; umask 077; temporary=$(mktemp ~/Asle/secure-link/.psk.XXXXXX); trap '\''rm -f "$temporary"'\'' EXIT HUP INT TERM; cat > "$temporary"; [ "$(wc -c < "$temporary")" -eq 32 ]; ln -T "$temporary" ~/Asle/secure-link/link.psk' < "$psk"; then
            printf '%s\n' '[ERROR] PSK copy failed; check SSH, board directory and existing key.' >&2
            exit 1
        fi
        printf '%s\n' '[OK] PSK copied. Check it on the board before starting.'
        ;;
    deploy)
        case "$board" in -*) printf '%s\n' '[ERROR] Invalid board host.' >&2; exit 2;; esac
        archive=$(mktemp "${TMPDIR:-/tmp}/secure-link-source.XXXXXX")
        trap 'rm -f "$archive"' EXIT HUP INT TERM
        tar -C "$software_dir" -cf "$archive" \
            aead_uio.c aead_uio_api.c aead_uio_api.h aead_uio_internal.h \
            secure_link/main.c secure_link/protocol.c secure_link/protocol.h \
            secure_link/transfer.c secure_link/transfer.h secure_link/transport.c \
            secure_link/transport.h secure_link/laptop.sh secure_link/board.sh secure_link/README.md
        printf '%s\n' '[INFO] Copying source to the board via SSH; keys and received files are excluded.'
        if ! ssh "$board" 'mkdir -p ~/Asle/secure-link && cd ~/Asle/secure-link && tar -xf -' < "$archive"; then
            printf '%s\n' '[ERROR] Deployment failed; rerun deploy to complete the source copy.' >&2
            exit 1
        fi
        printf '%s\n' '[OK] Source copied. On the board run: ./secure_link/board.sh start'
        ;;
    start)
        ensure_build
        umask 077
        mkdir -p "$receive"
        "$binary" --check-key "$psk"
        printf '[INFO] Connecting to %s on RFCOMM channel %s.\n' "$address" "$channel"
        if [ "${SL_VERBOSE:-0}" = 1 ]; then
            exec "$binary" --role laptop --bluetooth "$address" --channel "$channel" \
                --psk "$psk" --receive "$receive" --timeout-ms "$timeout" --verbose
        fi
        exec "$binary" --role laptop --bluetooth "$address" --channel "$channel" \
            --psk "$psk" --receive "$receive" --timeout-ms "$timeout"
        ;;
    *) printf '[ERROR] Unknown command: %s\n' "$action" >&2; usage; exit 2 ;;
esac
