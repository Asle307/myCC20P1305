# Cryptographic references

These eight saved PDFs are the complete cryptographic reference collection on
`main`. [SHA-256 checksums](reference-checksums.sha256) identify their exact
contents. No other protocol paper is added to the core bibliography.

| Document | Use |
|---|---|
| [A Hardware Perspective on the ChaCha Ciphers](<references/A Hardware Perspective on the ChaCha Ciphers.pdf>) | ChaCha hardware architecture context |
| [ChaCha, a variant of Salsa20](<references/ChaCha, a variant of Salsa20.pdf>) | Original ChaCha cipher and quarter-round definition |
| [ChaCha20–Poly1305 Authenticated Encryption with AdditionalData for Transport Layer Security 1.3](<references/ChaCha20–Poly1305 Authenticated Encryption with AdditionalData for Transport Layer Security 1.3.pdf>) | AEAD hardware and transport-layer integration context |
| [Design of an Integrated Cryptographic SoC Architecture for Resource-Constrained Devices](<references/Design of an Integrated Cryptographic SoC Architecture for Resource-Constrained Devices.pdf>) | Integrated cryptographic SoC architecture context |
| [High-Throughput Low-Area Hardware Design of Authenticated Encryption with Associated Data Cryptosystem that Uses ChaCha20 and Poly1305](<references/High-Throughput Low-Area Hardware Design of Authenticated Encryption with Associated Data Cryptosystem that Uses ChaCha20 and Poly1305.pdf>) | AEAD arithmetic architecture and FPGA/ASIC implementation context |
| [RFC 8439 - ChaCha20 and Poly1305 for IETF Protocols](<references/RFC 8439 - ChaCha20 and Poly1305 for IETF Protocols.pdf>) | Normative ChaCha20/Poly1305/AEAD behavior, byte order and test vectors |
| [Research on Hardware Primitives for Root-of-Trust in Secure Systems](<references/Research on Hardware Primitives for Root-of-Trust in Secure Systems.pdf>) | Hardware security primitives and integrated AEAD context |
| [The Poly1305-AES message-authentication code](<references/The Poly1305-AES message-authentication code.pdf>) | Original MAC arithmetic; its AES key generation differs from RFC 8439 |

Use each document's own device, workload and implementation flow when reading
its performance numbers. Standalone cipher, native AEAD and software-driven
peripheral rates are different measurement boundaries. Published results do
not establish this RTL's resource use or Fmax; those require fresh reports.

Interface branches keep this collection unchanged and cite their additional
lab and AMD/Xilinx interface manuals in their integration reference guide.
