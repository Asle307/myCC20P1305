# Secure Bluetooth byte-stream protocol v1

The Linux C application in `software/secure_link/` carries UTF-8 chat and files
between a KV260 (role 0, responder) and a paired HC-05 SPP laptop (role 1,
initiator). KV260 application AEAD uses the existing AXI4-Lite core through
`aead_uio_api.h`; laptop application AEAD and both handshakes use OpenSSL.
The production CLI has no software fallback for KV260. Test builds explicitly
use socket pairs and software AEAD for both roles. No RTL changes are needed.

This is a PSK demo between provisioned trusted endpoints. Bluetooth pairing
is a transport prerequisite, not the application authentication mechanism.
Key provisioning, rotation and revocation are manual. Anyone holding the PSK
can impersonate either endpoint. There is no forward secrecy: later PSK
compromise exposes captured sessions. This protocol is not a claim of a
standardized or independently audited secure transport.

## Session establishment

The PSK is an unencoded 32-byte random file. Each process checks a regular,
non-symlink file, exactly 32 bytes, owned by its effective UID, owner-readable,
with no group/other permissions. It never prints the PSK or traffic keys.

All handshake headers are eight bytes: ASCII `SLH1`, message type (one byte),
role (one byte), then two zero bytes. Challenges are 32 fresh bytes from
OpenSSL `RAND_bytes`; RNG failure aborts. Lengths below include the header.
The fixed transcript `T` is the 40-byte client hello followed by the 40-byte
server hello (without its MAC), in this order.

| Order | Direction | Type | Body | Total bytes |
|---|---|---|---|---|
| 1 | laptop to KV260 | 1 | client challenge | 40 |
| 2 | KV260 to laptop | 2 | server challenge, server MAC | 72 |
| 3 | laptop to KV260 | 3 | client MAC | 40 |
| 4 | KV260 to laptop | 4 | finished MAC | 40 |

`MAC(K, L) = HMAC-SHA-256(K, ASCII("secure-link-v1 ") || ASCII(L) || T)`.
There are no implicit NULs or separators. Types 2/3 use the PSK and labels
`server`/`client`. Verification uses constant-time comparison. Both sides
validate exact message types, roles and reserved bytes; role labels prevent
reflection.

Derive each 32-byte traffic key with HKDF-SHA-256, IKM=PSK, salt=SHA-256(T),
and info equal to exactly one of these ASCII strings:

- `secure-link-v1 kv260 to laptop` (direction 0)
- `secure-link-v1 laptop to kv260` (direction 1)

Type 4 uses the direction-0 key and label `finished`. KV260 enables
application data after verifying the client MAC and sending finished; laptop
enables it after verifying finished. Stream ordering puts finished before any
KV260 application record. Neither accepts early application records. Any
handshake failure closes the session and clears software key material.
Each fixed handshake message has its own complete-operation timeout.

## Application record

No native Verilog word framing crosses Bluetooth. The byte stream contains a
24-byte header, `length` ciphertext bytes and a 16-byte Poly1305 tag. The
entire header, exactly as transmitted, is AEAD AAD. Integers are unsigned,
network byte order. Buffers are fixed, with a 4096-byte payload limit checked
before reading the body. A malformed header terminates the session; the
receiver does not search arbitrary bytes for a new magic value.

| Offset | Size | Meaning |
|---|---|---|
| 0 | 4 | ASCII `SLF1` |
| 4 | 1 | version 1 |
| 5 | 1 | message type below |
| 6 | 1 | direction: sender role 0 or 1 |
| 7 | 1 | zero |
| 8 | 8 | directional sequence, starting at zero |
| 16 | 4 | ciphertext/plaintext byte length, 0..4096 |
| 20 | 4 | zero |

Nonce = four zero bytes followed by the eight network-order sequence bytes.
Separate directional keys permit the same numeric sequence in each direction.
Sequence counters never wrap; UINT64_MAX is refused. A successful encryption
consumes its sequence even if writing the frame fails, and that session then
ends. New process runs negotiate new traffic keys from fresh challenges.

Decrypt into private memory, verify the tag, then require exactly the next
receive sequence before exposing a frame. Failed authentication, replay or
out-of-order sequence leaves the caller's plaintext and receive counter
unchanged. At idle, such records are reported and discarded; more than eight
consecutive rejections closes the session. The CLI displays `[AUTH]` for tag
failure, `[REPLAY]` for an authenticated earlier sequence and `[ORDER]` for an
authenticated future sequence. These are local diagnostic results; the wire
format and failure behavior are unchanged. Any rejection while transferring
a file aborts the session and removes its temporary file. Hardware/backend
failures are fatal. Invalid direction, unsupported version/type, reserved
bits, excessive length or invalid authenticated application state is fatal.

| Type | Name | Plaintext body |
|---|---|---|
| 1 | CHAT | 1..4096 UTF-8 bytes, without terminal control characters except tab |
| 2 | OFFER | u64 total size, followed by 1..128 filename bytes |
| 3 | READY | empty |
| 4 | REFUSE | empty |
| 5 | CHUNK | 1..4096 file bytes |
| 6 | ACK | u64 cumulative accepted file bytes |
| 7 | END | u64 final total size |
| 8 | COMPLETE | u64 final total size |
| 9 | RECEIPT | u64 accepted CHAT sequence |
| 10 | CLOSE | empty |
| 11 | PING | empty |
| 12 | PONG | empty |

All of these, including acknowledgements and close, use AEAD. Only one local
chat awaits a receipt at once. No message is automatically resent. `/tamper`
and `/replay` first send a normal CHAT, wait for its authenticated RECEIPT,
then deliberately inject a modified copy or exact replay of that accepted
wire frame. Tamper flips the first ciphertext byte and leaves the original
tag. The demo does not perform another encryption with a used nonce, does
not advance either sequence for the injected copy, and does not deliver the
replayed text twice. These injections are explicit demo actions.

## File state and publication

Maximum total size is 10 MiB (10,485,760 bytes), including zero-byte files.
The sender snapshots a regular non-symlink file's size and rejects a size
change/truncation while reading. In-place content edits of unchanged size
are outside the snapshot guarantee; avoid modifying an outgoing file.
Only one file transfer can be active across the link. A busy endpoint refuses
incoming offers; simultaneous offers are both refused without deadlock.

| Current state | Event | Action / next state |
|---|---|---|
| IDLE | local file | OFFER / TX_OFFER |
| IDLE | safe OFFER, destination absent | create private temp, READY / RX |
| TX_OFFER | READY | first CHUNK / TX_ACK; or END / TX_COMPLETE if empty |
| TX_OFFER | REFUSE | close source / IDLE |
| TX_ACK | matching ACK | next CHUNK / TX_ACK; or END / TX_COMPLETE |
| TX_COMPLETE | matching COMPLETE | close source, report delivery / IDLE |
| RX | CHUNK within declared size | write authenticated bytes to temp, ACK / RX |
| RX | END matching declared and received size | publish, COMPLETE / IDLE |
| any | protocol, IO, progress timeout or disconnect | close session, delete temp |

Filenames use ASCII letters, digits, dot, dash and underscore, cannot start
with a dot and cannot contain separators, NULs or path traversal. The receive
directory must already exist, belong to the invoking user and have no group
or world write access; its final path component cannot be a symlink. The
process pins it with an open directory FD. Existing destinations, including
symlinks, are refused. Random hidden temporary names are created relative to
that FD with O_EXCL/O_NOFOLLOW and mode 0600.

Only authenticated chunks enter the temporary file. END must authenticate and
confirm both advertised and received totals. The receiver fsyncs and closes
the temp, then publishes it with `linkat` and unlinks the temporary name;
link creation atomically refuses any destination that appeared meanwhile.
It fsyncs the directory before sending COMPLETE. This requires a filesystem
supporting same-directory hard links (e.g. ext4); failure aborts. A lost COMPLETE
may leave a fully authenticated published file at the receiver while the
sender reports failure; it is never overwritten or resumed. No partial file
is published. Graceful exit, SIGINT/SIGTERM, IO and protocol errors remove
temporary files. SIGKILL, power loss or process crashes can leave hidden 0600
files; after stopping all clients the owner may remove those manually.

## Stream and failure handling

The nonblocking UI loop accumulates header/body fragments and can receive
while stdin is incomplete or output is waiting to be written. Outgoing frames
use a bounded queue of eight; a full queue or a send deadline ends the session.
It handles coalesced records by reading each bounded record in turn. Writes and
handshake reads handle short operations,
EINTR, EAGAIN, EOF and a monotonic complete-operation deadline. Default is
15 seconds (`--timeout-ms 100..60000`). The hardware API uses that timeout
independently per core operation and interleaves input feeding/output draining.
A started failed core operation is soft-reset. Preexisting busy/error is not
reset. Before opening UIO, the API requires the FPGA Manager `fpga0` state
`operating`; this does not establish the correct bitstream, clock or AXI bus
health. Software deadlines cannot interrupt a stalled individual MMIO access.
FPGA reset does not promise hardware key-register zeroization.

PING every five seconds and an authenticated-receive timeout of 30 seconds
detect an idle UART whose Bluetooth connection disappeared without a serial
EOF. Pending chat receipts and file progress also have 30-second deadlines;
heartbeats do not extend those deadlines. Partial frames use the configurable
frame timeout. There is no reconnect, application retransmission or partial
file resume. Restart both endpoints after a fatal error.

See the [operator guide](../../software/secure_link/README.md) for provisioning,
Bluetooth/driver checks, execution and the actual validation status.
