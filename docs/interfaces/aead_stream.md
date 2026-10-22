# AEAD 32-bit word interface

The top `aead_cc20_p1305` accepts and produces little-endian 32-bit words.
ChaCha20 and Poly1305 retain their internal 512-bit and 128-bit datapaths.

## Ports

One clock domain; synchronous active-low reset.

| Group | Buses | Scalar signals |
|---|---|---|
| Clock/reset | — | `i_clk`, `i_rst_n` |
| Configuration | `i_key[255:0]`, `i_nonce[95:0]` | `i_start`, `i_mode`, `o_start_ready` |
| Input | `i_data[31:0]`, `i_data_byte_count[2:0]` | `i_is_aad`, `i_data_valid`, `o_data_ready`, `i_data_last` |
| Output | `o_data[31:0]`, `o_data_byte_count[2:0]` | `o_is_tag`, `o_data_last`, `o_data_valid`, `i_data_ready` |
| Completion | — | `o_done`, `o_auth_ok` |

`i_mode=1` encrypts; zero decrypts. Byte 0 is `[7:0]`. Configuration transfers
on a rising edge with `i_start && o_start_ready`; the core captures it once.
Hold START/configuration until accepted. A new session cannot start on the
same edge that completes the previous one.

## Word transfers and sections

A word transfers on `valid && ready`. Hold data, count, type and LAST stable
under backpressure. Invalid output values are unspecified while VALID is low.

| Mode | Input order |
|---|---|
| Encrypt | AAD → plaintext |
| Decrypt | AAD → ciphertext → received tag |

AAD uses `i_is_aad=1`; payload/tag use zero. LAST ends each section, not every
internal Poly1305 block. A mismatched type stalls without capturing a word for
a later section. The caller supplies legal framing:

| Section | Nonfinal word | Final word |
|---|---|---|
| AAD/payload | count=4, LAST=0 | count=1..4, LAST=1 |
| Empty AAD/payload | None | count=0, LAST=1 |
| Received decrypt tag | Three words, count=4, LAST=0 | Fourth word, count=4, LAST=1 |

Unused input bytes are ignored. Nonempty sections end on the last actual word;
there is no extra empty terminator. An empty AAD word is mandatory, and empty
decrypt payload still precedes the four received-tag words.

The core assembles up to four words into one 128-bit block, zero-pads final
partial blocks and forwards them to its existing MAC/cipher controller. It
holds a completed block until its consumer is ready; another block is not
queued. Reset/START clear partial input and output word indices.

## Output and completion

Payload output has `o_is_tag=0`, count=1..4, and LAST only on the final payload
word. Unused bytes of that word are zero. Encryption then emits four tag words,
count=4 and `o_is_tag=1`; LAST marks only the fourth. Both the final ciphertext
word and final tag word have LAST=1, ending their respective sections.

Decryption emits plaintext only. Empty payload emits no payload word;
encryption still emits its tag and decryption still checks the received tag.
Output is serialized from a held 128-bit block. The internal controller
advances only after the last word of that block transfers.

`o_done` pulses for one clock when all encryption output has transferred, or
all plaintext has transferred and the decrypt tag has been checked.
`o_auth_ok` updates at completion and remains until START/reset. Encrypt
always reports auth=0; decrypt auth=1 means authentication passed.

Plaintext is tentative until DONE with auth=1. A consumer must drain plaintext
while feeding ciphertext/tag, retain it privately, and discard it on auth=0.
Waiting to read all output until DONE can deadlock under backpressure.

Reset aborts the session and clears control/valid/done/auth. It does not promise
to erase all key/datapath registers. The caller guarantees section order/counts;
there is no core framing-error handler or timeout. Use a unique nonce per key
and message. Payload is at most `(2^32-1)*64` bytes and AAD fits unsigned 64 bits.
