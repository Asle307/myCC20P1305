# AEAD processing

Implemented architecture for the [32-bit word interface](../interfaces/aead_stream.md).
`aead_cc20_p1305` owns word assembly/serialization, the session FSM, its
128-bit payload buffer, XOR/masking and byte lengths. The arithmetic blocks
retain their interfaces and latency contracts.

## Session order

START captures key, nonce and mode. The core holds configuration throughout
processing and handles one message at a time.

| Phase | Work |
|---|---|
| OTK | ChaCha20 counter 0; low 256 bits start Poly1305 |
| AAD | Assemble 32-bit words into 128-bit blocks; pad final partial block |
| Payload | Assemble block, XOR retained keystream, authenticate ciphertext |
| Lengths | Feed LE64 AAD length and LE64 ciphertext length to Poly1305 |
| Tag | Serialize encrypt tag or assemble and compare received decrypt tag |
| Complete | Pulse DONE after final output transfer/tag check; latch AUTH_OK |

Input order is AAD → payload → received tag (decrypt only). A mismatched input
type stalls. Empty AAD/payload each use one zero-byte LAST word; a nonempty
section ends on its final actual word. Tag is four full little-endian words.

## Word assembly and output hold

The input bank accepts four 32-bit words or a shorter final section. Each word
carries count 0..4 and section-LAST. Valid bytes are placed at increasing byte
offsets in a cleared 128-bit bank, ignoring poisoned unused input bytes.
A complete block holds VALID until the internal controller accepts it. The
next block cannot overwrite it. The running byte count also selects the word
position, because every nonfinal input word has four bytes. The accepting FSM
state supplies section type, so the completed bank needs no separate type flag.
The cleared bank already contains padding zeros; it is not masked a second time.
Reset and accepted START discard partial words.

On output, a two-bit word index selects one of four 32-bit slices of the held
payload/tag. Count is at most four; section-LAST appears only on the final word
of the final block. The index advances only on valid/ready. The payload FSM
and Poly1305 tag handshake advance only on the final word of the held block.
The output and keystream multiplexers use indexed Verilog-2001 part selects.
A shared cipher-state selection serves encryption before MAC and decryption
after MAC; one completion condition updates DONE/auth and returns the FSM to
idle. No additional module hierarchy is introduced by these choices.
There is no whole-message RAM or separate packet-engine module.

## ChaCha20 block reuse

ChaCha20 produces one 64-byte keystream block. Four internal 16-byte payload
blocks use slices `[127:0]`, `[255:128]`, `[383:256]`, `[511:384]` in order.
Each internal block becomes up to four output words. A complete 64-byte payload
therefore uses sixteen external 32-bit words and one ChaCha20 request.

Counter 0 is OTK only; payload counters begin at 1. Keep the current keystream
result held until its fourth slice or the final payload block has been used.
AAD and received tag do not consume keystream.

## Ciphertext feeds MAC in both modes

```text
Encrypt: plaintext  -> XOR -> ciphertext -> output and MAC
Decrypt: ciphertext -> MAC; ciphertext  -> XOR -> plaintext output
```

Retain the payload block until its required consumers have accepted it. In
decrypt, MAC accepts ciphertext before that bank is overwritten with plaintext.
MAC acceptance is sufficient; no accumulator-completion wait is inserted.

Output can stall at any word. Plaintext leaves before authentication finishes;
the caller must drain and privately retain it until DONE/AUTH_OK. An integrating
wrapper must preserve that handshake and authentication contract.

## RFC 8439 framing

```text
AAD || pad16(AAD) || ciphertext || pad16(ciphertext)
    || LE64(aad_length) || LE64(ciphertext_length)
```

Aligned/empty sections add no padding block. A partial section is zero-padded
inside its final 128-bit block. Poly1305 sees full 16-byte framed blocks; only
the lengths block is marked final. Original byte counts exclude padding/tag.

## Timing

Word assembly/serialization contributes to session cycle counts. The core retains ChaCha20's 41-clock result-valid,
paired arithmetic's 69-clock result-valid, or Poly1305's 70/73/72-clock
setup/pair/singleton contracts. CPU/stream stalls add latency. Keep the 200 MHz
target; fresh synthesis/implementation reports are required for timing claims.
