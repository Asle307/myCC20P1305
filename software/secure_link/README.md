# Secure HC-05 CLI chat and file demo

`secure_link` runs on Linux KV260 and a Linux laptop. KV260 connects a CP2102
USB/UART adapter to the HC-05, using 230400 baud, 8N1 and no hardware/software
flow control. Laptop connects directly to the paired HC-05's RFCOMM/SPP
socket. Both sides use a separately provisioned 32-byte PSK and OpenSSL for
mutual session authentication. All KV260 application records use FPGA AEAD
through UIO; laptop records use OpenSSL ChaCha20-Poly1305.

Read the [wire/state specification](../../docs/interfaces/secure_link.md) and
[UIO contract](../aead_uio_api.h). This demo does not reconnect, retransmit or
resume a partial file. Start both processes again to establish a fresh session
after a fatal error. Do not infer core performance from Bluetooth transfer
speed; Bluetooth, UART, acknowledgements and host scheduling dominate this path.

## Quick start with the scripts

These defaults match the demonstrated setup: board `debian@192.168.2.132`,
HC-05 `00:25:11:02:85:B9` on RFCOMM channel 1, CP2102 at 230400 baud,
`/dev/uio12`, and the existing `artifacts/secure-chat/link.psk` on the laptop
and `~/Asle/secure-link/link.psk` on the board. Override them with environment
variables listed below if your setup differs. The paired devices and FPGA
prerequisites in this guide still apply.

From the repository root **on the laptop**, copy current source and helpers:

```sh
./software/secure_link/laptop.sh deploy
```

This uses SSH and replaces only the listed source/helper files under
`~/Asle/secure-link`. It excludes PSKs, receive directories, bitstreams and
binaries; it does not run code or change FPGA/driver state on the board. If an
old chat is running, it continues with its existing binary. Quit that session
on both endpoints before starting a new one.

On the **board**:

```sh
cd ~/Asle/secure-link
./secure_link/board.sh check
./secure_link/board.sh start
```

`check` reads FPGA Manager state and device permissions, without UIO register
access. `start` performs the same checks, builds the serial-only binary once on each
start, checks the PSK, and waits for the laptop. Building each time avoids
running an old binary when laptop and board file timestamps differ. It does not establish that
`operating` means the PL clock, correct bitstream or AXI bus is ready; confirm
those separately as described below. An individual stalled MMIO access cannot
be interrupted by the software timeout.

On the **laptop**, within 60 seconds:

```sh
./software/secure_link/laptop.sh start
```

The laptop helper builds the CLI if necessary, checks the existing PSK and
connects to the paired HC-05. It creates the configured receive directory if
needed. On NixOS, missing build dependencies cause it to enter a `nix develop`
environment; Nix may need to download packages. Set `SL_AUTO_NIX=0` to disable
this behavior. Compiler and SSH prompts come from external tools.

Wait for `[OK] Session established` on both ends. Chat, file and demo commands
are unchanged; `/help` lists them. The console uses a simple ASCII frame and status tags:

```text
+------------------------------------------+
|           SECURE LINK / SPP              |
|        ChaCha20-Poly1305  |  PSK         |
+------------------------------------------+
[OK] Session established: mutual authentication and directional keys confirmed.
[CHAT] Peer: hello
[AUTH] REJECTED: authentication failed; no plaintext released.
[REPLAY] REJECTED: sequence already accepted; no plaintext released.
```

`[INFO]` gives setup/status, `[OK]` reports completion, `[CHAT]` identifies
peer text, `[FILE]` shows file progress, `[WAIT]` means an operation is busy,
`[AUTH]` reports a bad tag, `[REPLAY]` a valid old sequence, `[ORDER]` a valid
future sequence, `[REJECT]` a file refusal, and `[ERROR]` a fatal/error
condition. `[WARN]` and `[DEMO]` flag warnings and intentional demo actions.
`[TRACE]` appears only with verbose mode. The application does not log the PSK or derived traffic keys. Terminal echoes,
SSH prompts and compiler diagnostics are produced by external programs and
may not have a status tag.

For a new installation with no PSK, run on the laptop:

```sh
./software/secure_link/laptop.sh keygen
./software/secure_link/laptop.sh deploy
./software/secure_link/laptop.sh copy-key
```

`keygen` refuses to overwrite a local key. `copy-key` checks the local PSK and
uses SSH to create a private temporary file on the board; it checks the length
and atomically links it into place only if `link.psk` does not exist. Never run
`keygen` independently on both endpoints. Existing installations can reuse
their PSK and receive directories; `deploy` and `start` do not change the key.

Run either script with `help` for all commands. Common overrides:

```sh
SL_BLUETOOTH_ADDR=AA:BB:CC:DD:EE:FF SL_CHANNEL=1 ./software/secure_link/laptop.sh start
SL_BOARD_HOST=debian@BOARD_IP ./software/secure_link/laptop.sh deploy
SL_SERIAL=/dev/ttyUSB0 SL_UIO=/dev/uio12 ./secure_link/board.sh start
SL_VERBOSE=1 ./secure_link/board.sh start
```

Both helpers also accept `SL_PSK`, `SL_RECEIVE`, `SL_TIMEOUT_MS` and
`SL_BINARY`; the board helper accepts `SL_BAUD`. Defaults are a 60000 ms
startup/message timeout, 230400 baud and 10 MiB file limit. The helpers do
not automatically reconnect, resend, program the FPGA, load drivers, or
install system packages. `SL_VERBOSE=1` prints nonce, AAD, ciphertext and tag,
so use harmless demonstration data and protect terminal output.

## Build

Run from the repository root. Debian/Ubuntu dependencies for a full Linux
laptop build:

```sh
sudo apt-get update
sudo apt-get install build-essential pkg-config libssl-dev libbluetooth-dev bluez
mkdir -p build/host
cc -std=c99 -O2 -Wall -Wextra -Werror \
    software/secure_link/main.c software/secure_link/protocol.c \
    software/secure_link/transfer.c software/secure_link/transport.c \
    software/aead_uio_api.c $(pkg-config --cflags --libs openssl bluez) \
    -o build/host/secure_link
cc -std=c99 -O2 -Wall -Wextra -Werror \
    software/aead_uio.c software/aead_uio_api.c -o build/host/aead_uio
```

The same sources support a KV260-only serial build without BlueZ development
files. This was compiled against the board's OpenSSL 1.1.1n. It cannot serve
as the RFCOMM laptop endpoint; build without `SL_SERIAL_ONLY` for that role.

```sh
mkdir -p build/host
cc -std=c99 -O2 -Wall -Wextra -Werror -DSL_SERIAL_ONLY \
    software/secure_link/main.c software/secure_link/protocol.c \
    software/secure_link/transfer.c software/secure_link/transport.c \
    software/aead_uio_api.c -lcrypto -o build/host/secure_link
```

For a minimal KV260 image, check before installing anything:

```sh
command -v cc
openssl version
pkg-config --modversion openssl
ls -l /usr/include/openssl/evp.h
```

If missing and system changes are authorized, install `build-essential
pkg-config libssl-dev`. If board DNS or package access is unavailable, record
the failing command and leave hardware acceptance pending. Do not replace
system libraries manually or assume a development machine binary runs on
AArch64. Copy source and compile on the board, inside the operator-approved
workspace. For the 2026-10-30 session that workspace is `~/Asle`; no system
package installation or driver modification was performed.

On NixOS, enter a development shell without adding repository build runners:

```sh
nix develop --impure --expr 'with import <nixpkgs> {}; mkShell { packages = [ gcc pkg-config openssl bluez ]; }'
```

Then run the same compiler/test commands. OpenSSL 1.1.1 or newer and BlueZ
headers/library are required for the full build. No Xilinx BSP is needed.

## Generate and distribute the PSK

Create one random key and deliver the same binary file to both machines over
an independently secured channel (for example authenticated SSH/SCP or a
controlled removable drive). Create private working directories first:

```sh
mkdir -p artifacts/secure-chat/received
chmod 700 artifacts/secure-chat artifacts/secure-chat/received
build/host/secure_link --keygen artifacts/secure-chat/link.psk
build/host/secure_link --check-key artifacts/secure-chat/link.psk
```

`--keygen` creates a new file with mode at most 0600, subject to the process
umask. It rejects a umask that removes owner read permission. The command uses
OpenSSL randomness, refuses an existing
path and never displays the key. Do not generate one independent key on each
machine. Do not paste keys into shell arguments, source, chat or logs.
`--check-key` requires a regular, non-symlink file of exactly 32 raw bytes,
owned by the invoking effective UID, readable by its owner, with no group or
other permission bits. Hex/base64 text files do not meet this contract.

Copy to KV260 with the no-overwrite helper described above:

```sh
SL_BOARD_HOST=debian@BOARD_IP ./software/secure_link/laptop.sh copy-key
```

Check the key again locally on each endpoint. Keep both copies private and
untracked (`*.psk` is ignored). Manual replacement must provision both ends.
The application does not automate distribution, rotation or revocation.
Run as the PSK/receive-directory owner. If privileged UIO access is required,
provision the key and directory for that effective UID rather than weakening
permissions or bypassing key ownership checks.

## Verify the KV260 transport and FPGA first

Check driver presence on **each boot**; the lab previously loaded `cp210x`
temporarily. A past working `/dev/ttyUSB0` does not establish its availability
now. These commands are read-only:

```sh
uname -r
lsmod | grep -E 'cp210x|usbserial'
ls -l /dev/serial/by-id/
ls -l /dev/ttyUSB*
id
```

If the adapter is absent, inspect `dmesg`/`journalctl -k` as permitted. If the
kernel ships the matching module and system changes are authorized, `sudo
modprobe cp210x` may load it. If it is not installed, obtain/build it for the
**running kernel** using the board image's supported procedure; do not reuse
an arbitrary module across kernel versions. Persistent module loading is a
separate system configuration action. Do not assume it survived reboot.
Ensure the user has serial access (commonly `dialout`) and no terminal/ModemManager
client owns the UART. Prefer the stable `/dev/serial/by-id/...` path.

Check UIO and the configured FPGA state:

```sh
cat /sys/class/fpga_manager/fpga0/state
cat /sys/class/uio/uio12/name
cat /sys/class/uio/uio12/maps/map0/addr
cat /sys/class/uio/uio12/maps/map0/size
cat /sys/class/uio/uio12/maps/map0/offset
ls -l /dev/uio12
```

Use the matching **main-branch AXI4-Lite MMIO bitstream**, an enabled PS PL
clock, an `operating` FPGA manager state and the [board prerequisites](../README.md).
`unknown` is not proof of readiness. Resolve it with the board operator before
MMIO access. Device indices may change. The API refuses UIO unless FPGA Manager `fpga0` reports `operating`; it then
checks base `0xa0000000`, map coverage and zero offset and takes an exclusive
advisory lock. `operating` alone does not establish that the bitstream, clock
and bus are ready. Run one
UIO client at a time; other clients must also respect exclusive ownership.
Do not load/reload a bitstream during a client session.

After these prerequisites are established, run the three small independent
RFC checks once, with no benchmark load. In the board source directory:

```sh
./aead_uio --device /dev/uio12 --timeout-ms 5000
```

Require all three PASS results (encrypt, decrypt and bad-tag rejection) before
starting secure-link. A software polling deadline cannot interrupt an MMIO
bus access that never returns because the FPGA/clock/interconnect is not
ready; avoid repeatedly probing a stuck device. Close clients and have the
operator restore the matching FPGA/clock configuration before trying again.

CP2102 UART TX/RX must connect to HC-05 RX/TX with a common ground and suitable
logic levels. The HC-05's **data-mode UART baud** must match 230400; its AT-mode
baud may differ. Configure this using the module's documented AT procedure
outside the application. `--baud` supports 9600, 19200, 38400, 57600, 115200,
230400, 460800 and 921600. The observed setup below used 230400 for chat;
physical file-transfer and disconnect acceptance remain to be completed.

## Pair HC-05 on the laptop

Use Linux BlueZ, an enabled adapter and the module's configured pairing PIN.
The application does not pair or store Bluetooth credentials:

```text
bluetoothctl
power on
agent on
default-agent
scan on
pair AA:BB:CC:DD:EE:FF
info AA:BB:CC:DD:EE:FF
scan off
quit
```

Confirm the intended HC-05 MAC, `Paired: yes`/`Bonded: yes` and Serial Port UUID
`00001101-0000-1000-8000-00805f9b34fb`. Do not connect another serial terminal or
`rfcomm bind` client at the same time. The CLI opens a direct RFCOMM socket;
no `/dev/rfcomm0` binding is needed. Channel defaults to 1; use SDP/service
information for the module and set `--channel N` if different. On the tested
laptop, the paired module was `00:25:11:02:85:B9` (not connected at discovery).

## Run the two endpoints

Create the receive directory yourself, owned by the invoking user, with no
group/other write permission (0700 recommended). Start KV260 first, then
laptop within the handshake deadline. Increase `--timeout-ms` up to 60000
for manual startup; its default is 15000 ms. Keep stdin open in both terminals.

KV260, paths adjusted to the approved workspace:

```sh
cd ~/Asle/secure-link
mkdir -p received
chmod 700 received
./secure_link_kv260 --role kv260 \
    --serial /dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_TGKDS0M2-if00-port0 \
    --baud 230400 --uio /dev/uio12 --psk link.psk --receive received \
    --timeout-ms 60000
```

Laptop, from the repository root:

```sh
build/host/secure_link --role laptop --bluetooth AA:BB:CC:DD:EE:FF \
    --channel 1 --psk artifacts/secure-chat/link.psk \
    --receive artifacts/secure-chat/received --timeout-ms 60000
```

The earlier board snapshot in `~/Asle/secure-link-v1-20261030` predates the
UIO guard and the simplified handshake. Do not run its binaries. Once the board
is responsive and its FPGA configuration is confirmed, copy the current
sources into the approved workspace and rebuild them there. Wait for both
endpoints to print `Session established: mutual authentication and directional
keys confirmed.`

| Command | Action |
|---|---|
| `/msg hello` or a plain UTF-8 line | send one chat line; report authenticated receipt |
| `/file /absolute/path/example.dat` | offer a regular file of 0..10 MiB |
| `/status` | show backend, directional next sequences, file state, pending receipt |
| `/tamper harmless text` | deliver once, then inject a ciphertext-bit modification |
| `/replay harmless text` | deliver once, then replay the accepted encrypted frame |
| `/help` | list commands |
| `/quit` | authenticated close and cleanup |

Commands have no shell expansion or quote processing; `/file ` treats the
remaining line as a literal path (spaces allowed). The basename must be
1..128 ASCII letters/digits/dot/dash/underscore with no leading dot. Chat is
UTF-8 without terminal controls except tab, maximum 4096 bytes; the terminal
input line limit includes a command prefix. Invalid lines are discarded.
Only one file transfer and one locally pending chat are supported. Wait for
completion/receipt before starting the next local operation. File progress
is printed as cumulative bytes. An empty file still requires OFFER/READY and
authenticated END/COMPLETE. Existing destinations are refused, not renamed
or overwritten.

For demos, confirm that the receiver displays the harmless message exactly
once. A modified ciphertext produces `[AUTH] REJECTED`; an accepted frame
replayed intact produces `[REPLAY] REJECTED`. Send another normal chat afterward
to check that the receive counter stayed synchronized. Run both demos in both
directions while no file is active.

On KV260, a bad tag causes a soft reset of the completed UIO operation. The
authenticated Bluetooth session continues, and no tentative plaintext is
released. Older builds may misleadingly print `FAIL AEAD API` and `Session
aborted with SOFT_RESET` for this expected rejection; rebuild to remove those
messages. A genuine UIO error or timeout still ends the application session.

`--verbose` prints nonce, AAD, ciphertext, tag and acceptance/rejection codes.
It never prints keys. Normal chat output and file contents remain sensitive;
use harmless demonstration data and control access to any terminal captures.

## File safety, disconnect and troubleshooting

Temporary files are hidden `.secure-link-*` files of mode 0600 in the pinned
receive directory. Only authenticated chunks are written. The destination
appears only after the authenticated final total matches, using an atomic
hard link that refuses overwrite. Use a filesystem supporting hard links
(e.g. ext4). IO/protocol errors, disconnect, graceful close and SIGINT/SIGTERM
remove incomplete temps (a filesystem refusal is reported explicitly). A process kill/power failure can leave a hidden temp;
stop all clients before manually removing such a file. Never treat it as a
completed received file.

There is no automatic resend or resume. UART may not emit EOF when Bluetooth
drops, so authenticated heartbeats detect inactivity within 30 seconds.
File progress and chat receipt deadlines are also 30 seconds. A partial
record uses `--timeout-ms`. After failure, restart **both** ends; a retained
fully authenticated destination after a lost COMPLETE is refused on another
offer. Verify or remove it manually as appropriate.

| Symptom | Check / action |
|---|---|
| `Invalid PSK file` | exactly 32 raw bytes; same key; correct owner; mode 0600 or 0400; no symlink |
| `Receive directory` failure | directory exists, owned by effective UID, not group/world writable, final component not symlink |
| UART missing after reboot | `cp210x`, `usbserial`, kernel/module compatibility, USB enumeration and cable |
| `Device or resource busy` | another terminal/client or existing exclusive serial/UIO owner |
| RFCOMM refused/no route | power, pairing, MAC, Serial Port service/channel, range, competing connection |
| Handshake timeout | start KV260 first; matching HC-05 UART baud/wiring, data mode; extend startup deadline |
| Handshake authentication fails | same PSK on both ends; restart both after any partial/old handshake |
| UIO mapping fails | rediscover device; base/size/offset; permissions; matching AXI4-Lite configuration |
| UIO appears to hang beyond timeout | stop attempts; hardware bus access may be stuck; restore FPGA/clock with operator |
| File refused | transfer already active, unsafe name, >10 MiB or destination exists |
| Invalid frame/auth errors | line corruption, protocol-version mismatch or deliberate demo; an active file session aborts |
| Publish/IO error | free space, disk permissions, hard-link support, destination race; incomplete temp is deleted |

## Verification and actual hardware status

Local tests require the [fixture environment](../../script/requirements.txt)
and the full OpenSSL/BlueZ build dependencies:

```sh
.venv/bin/python -m unittest discover -s software/tests -v
```

Tests build with C99, `-Wall -Wextra -Werror`, AddressSanitizer and UBSan.
The original host/benchmark slot-model suite remains. API tests use independent
`cryptography` fixtures and verify failed decryption leaves caller output
unchanged and errors do not log tentative plaintext, timeout/reset/recovery, preexisting errors, empty and partial words.
The protocol tests compare a complete wire vector to Python `cryptography`,
run a separately implemented Python peer against each C role, and connect
two real C CLI processes over sockets. A pseudo-terminal verifies serial
230400/8N1/no-flow settings and exclusive access.

Shell-helper tests simulate SSH and compiler operations in temporary directories;
they check path quoting, PSK no-overwrite, source-only deployment, failed builds
and refusal before UIO on unknown FPGA state.

Coverage includes handshake corruption/PSK/confirmation/early-data failures,
fresh keys across restarts, nonce/sequence limits, fragmented input,
malformed/oversized records, simultaneous large sends and blocked writes,
ciphertext/tag/AAD changes, replay/order,
chat receipts and both demos. Files cover zero bytes, multiple chunks, exactly
10 MiB in both directions, unsafe/existing/symlink destinations, oversize,
simultaneous offers, partial disconnect, bad tag/replay, wrong end size,
publish collision, termination and progress timeout despite live heartbeats.
These are software/model results, not FPGA or radio acceptance.

Observed on 2026-10-30:

| Surface | Actual result |
|---|---|
| Local x86-64 Linux, GCC, OpenSSL 3.6.3, BlueZ 5.86 | full CLI compiled; local socket/model tests passed, plus 15 fixture/tool tests |
| Laptop adapter, initial inspection | `hci0`; HC-05 paired/bonded; SPP UUID present; not connected at that time |
| KV260 SSH | AArch64 Debian; operator limited board work to `~/Asle` |
| KV260 build | diagnostic host and serial-only CLI built with `-Wall -Wextra -Werror`, OpenSSL 1.1.1n; no packages installed by the assistant |
| KV260 serial | `/dev/ttyUSB0`, CP2102 stable by-id link; `cp210x` and `usbserial` loaded at inspection; user in `dialout` |
| KV260 UIO metadata | `MY_IP`, `/dev/uio12`, base `0xa0000000`, size `0x10000000`, offset zero; user has access |
| Initial FPGA state and UIO attempt | manager reported `unknown`; an attempted RFC test did not return, and subsequent SSH inspection did not respond |
| Later FPGA setup | operator loaded the bitstream matching SHA-256 `0d70db017d01d3aed4edab1d88005c65259a0926b6bc0c991dd0d301a85b0716`; manager reported `operating` and PL0 clock `199999998 Hz` |
| Later UIO RFC tests | operator reported PASS for encrypt (114 bytes), decrypt with AUTH_OK=1, and bad-tag decrypt with AUTH_OK=0 |
| Later Bluetooth chat | operator's terminal screenshot shows an authenticated SPP session, KV260 `/status` reporting FPGA UIO, and chat delivered in both directions |
| Tamper demo, laptop to KV260 | screenshot shows original text once, then AUTH_OK=0 and `REJECTED`; a subsequent normal chat was delivered |
| Replay demo, laptop to KV260 | screenshot shows original text once, then `REJECTED` after an accepted frame was replayed |
| File transfer, board-to-laptop demos and disconnect | not yet observed on the physical link |

The initial UIO preflight was attempted before `operating` readiness was
established. Its missing return is not a measured cryptographic timeout or a
passing negative test. The software deadline cannot bound a blocked bus load.
The later observations above come from the operator's commands and terminal
screenshot, not an automated measurement. The board binary in that screenshot
predates the local fix for the misleading `FAIL AEAD API` and `Session aborted`
messages on an expected bad-tag rejection. Existing historical board
benchmarks in `software/README.md` do not measure this Bluetooth session.

Remaining hardware acceptance:

1. Confirm chat with multibyte UTF-8 in both directions.
2. Send empty and multi-chunk files each way; compare `sha256sum` on both hosts.
   Send a 10 MiB file only after small transfers are stable; avoid benchmark loops.
3. Run tamper/replay from KV260 to laptop and confirm one delivery then rejection.
4. Disconnect Bluetooth during a file and at idle; check explicit session end,
   temp removal, no partial destination and a new handshake after restart.
5. Record board image, FPGA/clock state, UART baud, HC-05 channel, test sizes,
   hashes and observed results; keep keys and private contents out of reports.
