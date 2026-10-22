#!/usr/bin/env python3
"""ChaCha20-Poly1305 AEAD golden vector generator for Verilog/SystemVerilog.

Outputs $readmemh-compatible hex word files and sidecar beat/length metadata.
Reference: RFC 8439, Section 2.8.2. Generic exports plus an explicit 32-bit adapter for this repository's RTL.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
import re
import sys
import shutil
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable

try:
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
    from rich.console import Console
    from rich.panel import Panel
    from rich.progress import BarColumn, Progress, SpinnerColumn, TextColumn
    from rich.table import Table
except ImportError as exc:
    print(f"Dependency missing: {exc}. Install: python -m pip install rich cryptography", file=sys.stderr)
    raise SystemExit(2) from exc

console = Console()
error_console = Console(stderr=True)
ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 65536
MAGIC_FILE = ".cc20p1305-vectorgen"
RFC_TAG = bytes.fromhex("1ae10b594f09e26a7e902ecbd0600691")
RFC_CIPHERTEXT = bytes.fromhex(
    "d31a8d34648e60db7b86afbc53ef7ec2"
    "a4aded51296e08fea9e2b5a736ee62d6"
    "3dbea45e8ca9671282fafb69da92728b"
    "1a71de0a9e060b2905d6a5b67ecd3b36"
    "92ddbd7f2d778b8c9803aee328091b58"
    "fab324e4fad675945585808b4831d7bc"
    "3ff4def08e4b7a9de576d26586cec64b"
    "6116"
)


@dataclass(frozen=True)
class Vector:
    name: str
    key: bytes
    nonce: bytes
    aad: bytes
    plaintext: bytes
    source: str


@dataclass(frozen=True)
class Case:
    vector: Vector
    mode: str
    ciphertext: bytes
    tag: bytes
    received_tag: bytes = b""
    auth_ok: bool = False

    @property
    def slug(self) -> str:
        return f"{self.vector.name}_{'enc' if self.mode == 'encrypt' else 'dec'}"

    @property
    def input_payload(self) -> bytes:
        return self.vector.plaintext if self.mode == "encrypt" else self.ciphertext

    @property
    def output_payload(self) -> bytes:
        return self.ciphertext if self.mode == "encrypt" else self.vector.plaintext


def parse_hex(text: str, *, expected: int | None = None, label: str = "hex") -> bytes:
    s = text.strip()
    if s.lower().startswith("0x"):
        s = s[2:]
    s = re.sub(r"[\s:_-]+", "", s)
    if len(s) % 2:
        raise ValueError(f"{label}: odd number of hex digits")
    try:
        data = bytes.fromhex(s)
    except ValueError as exc:
        raise ValueError(f"{label}: invalid hexadecimal data") from exc
    if expected is not None and len(data) != expected:
        raise ValueError(f"{label}: expected {expected} bytes, got {len(data)}")
    return data


def rfc_vector() -> Vector:
    return Vector(
        name="rfc8439",
        key=bytes(range(0x80, 0xA0)),
        nonce=bytes.fromhex("070000004041424344454647"),
        aad=bytes.fromhex("50515253c0c1c2c3c4c5c6c7"),
        plaintext=(
            b"Ladies and Gentlemen of the class of '99: If I could offer you "
            b"only one tip for the future, sunscreen would be it."
        ),
        source="RFC 8439 section 2.8.2",
    )


def make_random_vector(name: str, aad_size: int, data_size: int, rng: random.Random) -> Vector:
    def draw(n: int) -> bytes:
        # This is repeatable test data, NEVER intended as real encryption key material.
        return bytes(rng.getrandbits(8) for _ in range(n))

    return Vector(name, draw(32), draw(12), draw(aad_size), draw(data_size), "seeded synthetic")


def make_vectors(args: argparse.Namespace) -> list[Vector]:
    if args.profile == "rfc":
        return [rfc_vector()]
    rng = random.Random(args.seed)
    if args.profile == "custom":
        key = parse_hex(args.key_hex, expected=32, label="key") if args.key_hex is not None else bytes(rng.getrandbits(8) for _ in range(32))
        nonce = parse_hex(args.nonce_hex, expected=12, label="nonce") if args.nonce_hex is not None else bytes(rng.getrandbits(8) for _ in range(12))
        if args.aad_hex is not None and args.aad_file is not None:
            raise ValueError("Use either --aad-hex or --aad-file")
        if args.plaintext_hex is not None and args.plaintext_file is not None:
            raise ValueError("Use either --plaintext-hex or --plaintext-file")
        aad = args.aad_file.read_bytes() if args.aad_file else parse_hex(args.aad_hex or "", label="aad")
        data = args.plaintext_file.read_bytes() if args.plaintext_file else parse_hex(args.plaintext_hex or "", label="plaintext")
        name = re.sub(r"[^a-zA-Z0-9_-]", "_", args.case_name).strip("_")
        if not name:
            raise ValueError("Invalid --case-name")
        return [Vector(name, key, nonce, aad, data, "custom")]

    if args.profile == "random":
        return [make_random_vector(f"random_{i:03d}", rng.randrange(args.max_random_bytes + 1),
                                   rng.randrange(args.max_random_bytes + 1), rng)
                for i in range(args.random_count)]
    result = [rfc_vector()]
    sizes = [
        ("empty", 0, 0), ("aad_only", 13, 0), ("data_only", 0, 1),
        ("short_3", 3, 3), ("tail_15", 15, 15),
        ("aligned_16", 16, 16), ("cross_17", 17, 17),
        ("block_63", 13, 63), ("block_64", 16, 64),
        ("block_65", 17, 65), ("multi_128", 32, 128),
        ("multi_129", 33, 129), ("large_1024", 5, 1024),
        ("large_aad", 1024, 0),
    ]
    result += [make_random_vector(name, a, p, rng) for name, a, p in sizes]
    # Exhaustive tails of 16-byte Poly1305 and 64-byte ChaCha20 blocks.
    sizes += [(f"tail_sweep_{n:02d}", n % 17, n) for n in range(65)]
    sizes += [(f"paired_tail_{n:02d}", 16 + n, 64 + n) for n in range(1, 17)]
    sizes += [(f"long_{n}", 65, n) for n in (127, 191, 192, 193, 255, 256, 257, 511, 512, 513, 1023, 4096)]
    result += [make_random_vector(name, a, p, rng) for name, a, p in sizes[14:]]
    result += [Vector("all_zero", bytes(32), bytes(12), bytes(17), bytes(65), "all zero"),
               Vector("all_ff", b"\xff" * 32, b"\xff" * 12, b"\xff" * 17, b"\xff" * 65, "all FF")]
    for i in range(args.random_count):
        a = rng.randrange(args.max_random_bytes + 1)
        p = rng.randrange(args.max_random_bytes + 1)
        result.append(make_random_vector(f"random_{i:03d}", a, p, rng))
    return result


def golden(vector: Vector, mode: str) -> Case:
    if len(vector.key) != 32 or len(vector.nonce) != 12:
        raise ValueError("RFC 8439 requires 32-byte key and 12-byte nonce")
    ciphertext_and_tag = ChaCha20Poly1305(vector.key).encrypt(vector.nonce, vector.plaintext, vector.aad)
    ciphertext, tag = ciphertext_and_tag[:-16], ciphertext_and_tag[-16:]
    # Independently check round-trip through the reference library.
    decoded = ChaCha20Poly1305(vector.key).decrypt(vector.nonce, ciphertext_and_tag, vector.aad)
    if decoded != vector.plaintext:
        raise AssertionError("Reference round-trip failed")
    if vector.source == "RFC 8439 section 2.8.2" and (ciphertext != RFC_CIPHERTEXT or tag != RFC_TAG):
        raise AssertionError("RFC 8439 official ciphertext/tag mismatch")
    return Case(vector, mode, ciphertext, tag, tag if mode == "decrypt" else b"", mode == "decrypt")


def pack_words(data: bytes, nbytes: int, byteorder: str) -> list[int]:
    result = []
    for pos in range(0, len(data), nbytes):
        chunk = data[pos:pos + nbytes].ljust(nbytes, b"\x00")
        result.append(int.from_bytes(chunk, byteorder))
    return result


def mutate(case: Case, field: str, bit: int, name: str | None = None) -> Case:
    aad, ciphertext, received = case.vector.aad, case.ciphertext, case.tag
    value = {"aad": aad, "ciphertext": ciphertext, "tag": received}[field]
    changed = bytearray(value)
    changed[bit // 8] ^= 1 << (bit % 8)
    if field == "aad":
        aad = bytes(changed)
    elif field == "ciphertext":
        ciphertext = bytes(changed)
    else:
        received = bytes(changed)
    cipher = ChaCha20Poly1305(case.vector.key)
    stream = cipher.encrypt(case.vector.nonce, bytes(len(ciphertext)), b"")[:-16]
    plaintext = bytes(a ^ b for a, b in zip(ciphertext, stream))
    computed = cipher.encrypt(case.vector.nonce, plaintext, aad)
    if computed[:-16] != ciphertext:
        raise AssertionError("Ciphertext mutation reference mismatch")
    try:
        cipher.decrypt(case.vector.nonce, ciphertext + received, aad)
    except InvalidTag:
        pass
    else:
        raise AssertionError("Mutation unexpectedly authenticated")
    vector = replace(case.vector, name=name or case.vector.name + "_bad_" + field,
                     aad=aad, plaintext=plaintext, source=f"modified {field} bit {bit}")
    return Case(vector, "decrypt", ciphertext, computed[-16:], received, False)


def emit_rtl(folder: Path, case: Case) -> dict[str, Any]:
    """Native bus adapter: count[2:0], type[8], section-LAST[9]."""
    input_words, input_control, output_words, output_control = [], [], [], []

    def append(data, kind, words, controls, empty=False):
        values = pack_words(data, 4, "little")
        if empty and not data:
            values = [0]
        for index, value in enumerate(values):
            count = max(0, min(4, len(data) - index * 4))
            words.append(value)
            controls.append(count | (kind << 8) | ((index == len(values) - 1) << 9))

    append(case.vector.aad, 1, input_words, input_control, True)
    append(case.input_payload, 0, input_words, input_control, True)
    if case.mode == "decrypt":
        append(case.received_tag, 0, input_words, input_control)
    append(case.output_payload, 0, output_words, output_control)
    if case.mode == "encrypt":
        append(case.tag, 1, output_words, output_control)
    config = [word_hex(int.from_bytes(case.vector.key, "little"), 32),
              word_hex(int.from_bytes(case.vector.nonce, "little"), 12),
              word_hex(int.from_bytes(case.tag, "little"), 16)]
    config += [word_hex(value, 4) for value in (int(case.mode == "encrypt"), int(case.auth_ok),
               len(case.vector.aad), len(case.input_payload), len(input_words), len(output_words))]
    (folder / "rtl_config.hex").write_text("\n".join(config) + "\n", encoding="ascii")
    for name, values in (("input", input_words), ("input_control", input_control),
                         ("expected", output_words), ("expected_control", output_control)):
        write_words(folder / f"rtl_{name}.hex", values, 4)
    return {"word_bits": 32, "byteorder": "little", "mode_encrypt": 1,
            "input_beats": len(input_words), "expected_beats": len(output_words),
            "control": "bits 2:0 byte count, bit 8 AAD(input)/tag(output), bit 9 section LAST; other bits zero",
            "config": "key256, nonce96, tag128, then six word32 lines: encrypt, AUTH_OK, AAD bytes, payload bytes, input beats, output beats",
            "empty_sections": "one zero-byte LAST word for each empty input AAD/payload section",
            "decrypt": "four received tag input words; plaintext output only"}


def word_hex(value: int, nbytes: int) -> str:
    return f"{value:0{nbytes * 2}x}"


def write_words(path: Path, values: Iterable[int], nbytes: int) -> None:
    # Empty memory: zero lines, rather than a fake zero-valued valid beat.
    path.write_text("".join(word_hex(v, nbytes) + "\n" for v in values), encoding="ascii")


def beat_rows(data: bytes, *, phase: str, nbytes: int, byteorder: str) -> list[dict[str, Any]]:
    values = pack_words(data, nbytes, byteorder)
    rows: list[dict[str, Any]] = []
    for idx, value in enumerate(values):
        rows.append({
            "beat_index": idx,
            "phase": phase,
            "data_hex": word_hex(value, nbytes),
            "byte_count": min(nbytes, len(data) - idx * nbytes),
        })
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]], columns: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as fd:
        writer = csv.DictWriter(fd, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def write_json(path: Path, obj: Any) -> None:
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def emit_case(root: Path, case: Case, width_bits: int, byteorder: str) -> dict[str, Any]:
    w = width_bits // 8
    vector = case.vector
    folder = root / case.slug
    folder.mkdir()

    # Key and nonce are single full-width words to load 256-bit and 96-bit ports.
    write_words(folder / "key.hex", [int.from_bytes(vector.key, byteorder)], 32)
    write_words(folder / "nonce.hex", [int.from_bytes(vector.nonce, byteorder)], 12)
    write_words(folder / "aad.hex", pack_words(vector.aad, w, byteorder), w)
    write_words(folder / "input.hex", pack_words(case.input_payload, w, byteorder), w)
    write_words(folder / "expected_output.hex", pack_words(case.output_payload, w, byteorder), w)
    write_words(folder / "expected_tag.hex", [int.from_bytes(case.tag, byteorder)], 16)
    write_words(folder / "expected_tag_words.hex", pack_words(case.tag, w, byteorder), w)
    # Incoming tag is meaningful for decryption, and may need an RTL-specific handshake.
    if case.mode == "decrypt":
        write_words(folder / "input_tag.hex", [int.from_bytes(case.received_tag, byteorder)], 16)
        write_words(folder / "input_tag_words.hex", pack_words(case.received_tag, w, byteorder), w)

    in_aad = beat_rows(vector.aad, phase="aad", nbytes=w, byteorder=byteorder)
    in_data = beat_rows(case.input_payload, phase="payload", nbytes=w, byteorder=byteorder)
    in_rows = in_aad + in_data
    for idx, row in enumerate(in_rows):
        row["beat_index"] = idx
        row["is_aad"] = 1 if row["phase"] == "aad" else 0
        row["phase_last"] = int(row is in_aad[-1] if row["phase"] == "aad" else row is in_data[-1])
        row["packet_last"] = int(idx == len(in_rows) - 1)
    write_csv(
        folder / "input_beats.csv", in_rows,
        ["beat_index", "phase", "data_hex", "byte_count", "is_aad", "phase_last", "packet_last"],
    )
    write_words(folder / "input_stream.hex", [int(r["data_hex"], 16) for r in in_rows], w)
    # Proposed control descriptor for each input beat; the RTL driver must adapt last semantics.
    input_control = [
        int(r["byte_count"]) | (int(r["is_aad"]) << 8)
        | (int(r["phase_last"]) << 9) | (int(r["packet_last"]) << 10)
        for r in in_rows
    ]
    write_words(folder / "input_control.hex", input_control, 4)
    # Two 64-bit byte-length words: AAD and payload, without any artificial word padding.
    write_words(folder / "lengths.hex", [len(vector.aad), len(case.input_payload)], 8)

    out_payload = beat_rows(case.output_payload, phase="payload", nbytes=w, byteorder=byteorder)
    out_tag = beat_rows(case.tag, phase="tag", nbytes=w, byteorder=byteorder) if case.mode == "encrypt" else []
    out_rows = out_payload + out_tag
    for idx, row in enumerate(out_rows):
        row["beat_index"] = idx
        row["is_tag"] = int(row["phase"] == "tag")
    write_csv(folder / "expected_beats.csv", out_rows,
              ["beat_index", "phase", "data_hex", "byte_count", "is_tag"])
    write_words(folder / "expected_stream.hex", [int(r["data_hex"], 16) for r in out_rows], w)
    write_words(folder / "expected_control.hex", [int(r["byte_count"]) | (int(r["is_tag"]) << 8) for r in out_rows], 4)

    metadata = {
        "name": case.slug,
        "source": vector.source,
        "mode": case.mode,
        "word_bits": width_bits,
        "byteorder": byteorder,
        "key_hex_bytes": vector.key.hex(),
        "nonce_hex_bytes": vector.nonce.hex(),
        "aad_hex_bytes": vector.aad.hex(),
        "plaintext_hex_bytes": vector.plaintext.hex(),
        "ciphertext_hex_bytes": case.ciphertext.hex(),
        "tag_hex_bytes": case.tag.hex(),
        "received_tag_hex_bytes": case.received_tag.hex(),
        "expected_auth_ok": int(case.auth_ok),
        "aad_bytes": len(vector.aad),
        "payload_bytes": len(case.input_payload),
        "aad_beats": len(in_aad),
        "input_payload_beats": len(in_data),
        "expected_payload_beats": len(out_payload),
        "tag_beats": len(out_tag),
        "input_total_beats": len(in_rows),
        "expected_total_beats": len(out_rows),
        "padding": "Zero-pad only the unused byte lanes of the final word of each phase; padding is NOT part of AEAD data.",
        "control_encoding": {
            "input_control.hex": "32-bit/line: bits [7:0]=valid byte_count, bit 8=is_aad, bit 9=phase_last, bit 10=packet_last, all higher bits 0",
            "expected_control.hex": "32-bit/line: bits [7:0]=valid byte_count, bit 8=is_tag, all higher bits 0",
            "lengths.hex": "two 64-bit/line values: AAD bytes then input payload bytes",
            "warning": "phase_last/packet_last describe boundaries, NOT direct RTL i_data_last truth values.",
        },
        "signals": {
            "byte_count": "1..word_bytes for valid beats; empty phases have 0 beats.",
            "is_aad": "1 for AAD input beats; 0 for payload input beats.",
            "phase_last": "Metadata only, denotes last beat of the AAD/payload phase.",
            "packet_last": "Metadata only, denotes last data beat in this concatenated AAD+payload listing.",
            "rtl_i_data_last": "NOT fixed by this generator. Derive from your RTL's accepted protocol.",
            "rtl_i_mode": "NOT fixed by this generator. Map mode to the polarity defined by your RTL.",
            "decrypt_tag_input": "Separate tag files; RTL-specific tag input protocol must be implemented by TB.",
        },
        "files": {
            "key": "key.hex", "nonce": "nonce.hex", "aad": "aad.hex",
            "input_payload": "input.hex", "input_stream": "input_stream.hex",
            "input_control": "input_control.hex", "lengths": "lengths.hex",
            "expected_payload": "expected_output.hex", "expected_tag": "expected_tag.hex",
            "expected_tag_words": "expected_tag_words.hex", "expected_stream": "expected_stream.hex",
            "expected_control": "expected_control.hex",
            "input_beats": "input_beats.csv", "expected_beats": "expected_beats.csv",
        },
    }
    if case.mode == "decrypt":
        metadata["files"]["input_tag"] = "input_tag.hex"
        metadata["files"]["input_tag_words"] = "input_tag_words.hex"
    metadata["rtl"] = emit_rtl(folder, case)
    write_json(folder / "case.json", metadata)
    return {
        "name": case.slug, "mode": case.mode, "aad_bytes": len(vector.aad),
        "data_bytes": len(vector.plaintext), "input_beats": len(in_rows),
        "output_beats": len(out_rows), "dir": case.slug, "expected_auth_ok": int(case.auth_ok),
    }


def ensure_output_dir(out: Path, force: bool) -> None:
    if out.exists():
        if not out.is_dir():
            raise ValueError(f"Output path is not a directory: {out}")
        entries = list(out.iterdir())
        if entries and not force:
            raise ValueError(f"Output directory is not empty: {out}; use --force if it is generator-owned")
        if entries and force:
            if not (out / MAGIC_FILE).is_file():
                raise ValueError(f"Refusing to replace a directory not owned by this generator: {out}")
    if out == ROOT or out in ROOT.parents:
        raise ValueError("Output must not replace the repository or one of its ancestors")


def header(title: str, detail: str = "") -> None:
    console.print(Panel.fit(
        f"[bold cyan]CHACHA20[/bold cyan] [bold white]+[/bold white] [bold magenta]POLY1305[/bold magenta]\n"
        f"[bold]{title}[/bold] [dim]{detail}[/dim]",
        border_style="bright_blue", padding=(1, 3),
    ))


def print_cases(rows: list[dict[str, Any]], width_bits: int) -> None:
    table = Table(title=f"Vector Matrix · {width_bits}-bit", border_style="bright_blue", header_style="bold cyan", row_styles=["", "dim"])
    table.add_column("#", style="dim", justify="right")
    table.add_column("Case", style="bold white")
    table.add_column("Mode", justify="center")
    table.add_column("AAD (B)", justify="right")
    table.add_column("Payload (B)", justify="right")
    table.add_column("In beats", justify="right")
    table.add_column("Out beats", justify="right")
    for i, row in enumerate(rows, 1):
        table.add_row(str(i), row["name"],
                      "[green]ENC[/green]" if row["mode"] == "encrypt" else "[yellow]DEC[/yellow]",
                      str(row["aad_bytes"]), str(row["data_bytes"]),
                      str(row["input_beats"]), str(row["output_beats"]))
    console.print(table)


def generate(args: argparse.Namespace) -> int:
    if args.profile == "suite" and args.mode != "both":
        raise ValueError("Suite regression requires --mode both for reset/recovery checks")
    if args.max_random_bytes > MAX_BYTES or args.random_count > 4096:
        raise ValueError("Maximum section length is 65536 bytes; maximum random count is 4096")
    if args.profile == "random" and args.random_count == 0:
        raise ValueError("Random profile requires --random-count > 0")
    if args.random_count < 0 or args.max_random_bytes < 0:
        raise ValueError("--random-count and --max-random-bytes must be >= 0")
    if args.profile != "custom" and any((args.key_hex is not None, args.nonce_hex is not None,
                                          args.aad_hex is not None, args.plaintext_hex is not None,
                                          args.aad_file is not None, args.plaintext_file is not None)):
        raise ValueError("Custom key/AAD/plaintext options require --profile custom")
    vectors = make_vectors(args)
    modes = ["encrypt", "decrypt"] if args.mode == "both" else [args.mode]
    cases = [golden(v, m) for v in vectors for m in modes]
    if args.profile == "suite" and "decrypt" in modes:
        for vector in vectors:
            base = golden(vector, "decrypt")
            cases.append(mutate(base, "tag", 0))
        base = golden(rfc_vector(), "decrypt")
        cases += [mutate(base, "tag", bit, f"rfc_tag_bit_{bit:03d}") for bit in range(128)]
        cases += [mutate(base, kind, 0) for kind in ("aad", "ciphertext")]
    if any(max(len(c.vector.aad), len(c.input_payload)) > MAX_BYTES for c in cases):
        raise ValueError("Case sections must be <= 65536 bytes for the simulation adapter")
    slugs = [c.slug for c in cases]
    if len(slugs) != len(set(slugs)):
        raise ValueError("Duplicate case directory names; use a unique --case-name")
    out = (args.out or ROOT / "dv/vectors" / ("suite" if args.profile == "suite" else "custom")).expanduser().resolve()
    for slug in slugs:
        path = str(out / slug)
        if len(path) > 255 or any(ord(char) < 32 or ord(char) > 126 for char in path):
            raise ValueError("RTL catalog paths require printable ASCII and at most 255 characters")
    ensure_output_dir(out, args.force)
    header("VECTOR GENERATOR", f"RFC 8439 · {args.word_bits}-bit · {args.byteorder}-endian")
    out.parent.mkdir(parents=True, exist_ok=True)
    # Build fully before replacing a previous generated suite, including under --force.
    with tempfile.TemporaryDirectory(prefix=".vectorgen-", dir=out.parent) as temporary:
        stage = Path(temporary) / "export"
        stage.mkdir()
        (stage / MAGIC_FILE).write_text("cc20p1305-vectorgen v1\n", encoding="ascii")
        rows = []
        with Progress(
            SpinnerColumn(), TextColumn("[bold blue]{task.description}"),
            BarColumn(complete_style="cyan"), TextColumn("{task.completed}/{task.total}"),
            console=console, transient=True,
        ) as progress:
            task = progress.add_task("Generating golden vectors", total=len(cases))
            for case in cases:
                rows.append(emit_case(stage, case, args.word_bits, args.byteorder))
                progress.advance(task)
        catalog = "suite_cases.txt" if args.profile == "suite" else "custom_cases.txt"
        (stage / catalog).write_text("".join(str(out / c.slug) + "\n" for c in cases), encoding="ascii")
        coverage = {
            "aad_tails": sorted({(len(v.aad) - 1) % 16 + 1 for v in vectors if v.aad}),
            "payload_tails": sorted({(len(v.plaintext) - 1) % 16 + 1 for v in vectors if v.plaintext}),
            "chacha_tails": sorted({(len(v.plaintext) - 1) % 64 + 1 for v in vectors if v.plaintext}),
            "authentication_rejects": sum(c.mode == "decrypt" and not c.auth_ok for c in cases),
        }
        manifest = {
            "profile": args.profile, "rtl_catalog": catalog, "coverage": coverage,
            "generator": "cc20p1305-vectorgen", "schema_version": 1,
            "algorithm": "IETF ChaCha20-Poly1305 RFC 8439",
            "word_bits": args.word_bits, "byteorder": args.byteorder,
            "seed": args.seed if args.profile != "rfc" else None,
            "cases": rows,
            "notes": [
                "Each .hex has ONE full-width hexadecimal word per line with no 0x prefix.",
                "Input stream concatenates individually word-padded AAD and input payload.",
                "Expected stream is output payload followed by tag for encrypt; plaintext only for decrypt.",
                "Generic empty phase files have zero lines. rtl_* adapter adds mandatory empty section terminators.",
                "rtl_* adapter is always native 32-bit little-endian; generic packing may differ.",
                "Synthetic keys from --seed are for verification ONLY; do not use in production.",
            ],
        }
        write_json(stage / "manifest.json", manifest)
        if out.exists():
            shutil.rmtree(out)
        stage.replace(out)
    print_cases(rows, args.word_bits)
    console.print(f"[bold green]✔ Generated {len(rows)} cases[/bold green] → [cyan]{out}[/cyan]")
    console.print("[dim]Files per case: key, nonce, aad, input, expected_output, expected_tag, beat CSV, JSON metadata.[/dim]")
    return 0


def inspect(args: argparse.Namespace) -> int:
    path = args.path
    if path.is_dir() and (path / "manifest.json").is_file():
        manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
        header("INSPECT MATRIX", str(path))
        print_cases(manifest["cases"], manifest["word_bits"])
        return 0
    if path.is_dir():
        path = path / "case.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    header("INSPECT CASE", data["name"])
    table = Table(box=None)
    table.add_column("Field", style="cyan")
    table.add_column("Value", style="white")
    for k in ("mode", "source", "word_bits", "byteorder", "aad_bytes", "payload_bytes",
              "aad_beats", "input_payload_beats", "expected_payload_beats", "tag_beats"):
        table.add_row(k, str(data[k]))
    console.print(table)
    console.print(f"[dim]Key bytes: {data['key_hex_bytes']}[/dim]")
    console.print(f"[dim]Nonce bytes: {data['nonce_hex_bytes']}[/dim]")
    console.print(f"[green]Expected tag bytes: {data['tag_hex_bytes']}[/green]")
    return 0


def read_word_file(path: Path) -> list[int]:
    words: list[int] = []
    for i, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        line = line.split("//", 1)[0].split("#", 1)[0].strip()
        if not line:
            continue
        line = line.removeprefix("0x").removeprefix("0X")
        if not re.fullmatch(r"[0-9a-fA-F_]+", line):
            raise ValueError(f"Invalid hex at {path}:{i}: {line!r}")
        words.append(int(line.replace("_", ""), 16))
    return words


def compare_file(actual: Path, expected: Path, width_bits: int, label: str) -> bool:
    actual_words = read_word_file(actual)
    expected_words = read_word_file(expected)
    ok = True
    if len(actual_words) != len(expected_words):
        console.print(f"[red]✘ {label}: word count {len(actual_words)} != {len(expected_words)}[/red]")
        ok = False
    mask = (1 << width_bits) - 1
    for i, (got, wanted) in enumerate(zip(actual_words, expected_words)):
        if got != wanted or got > mask:
            console.print(f"[red]✘ {label} word {i}: actual {got:0{width_bits // 4}x}, expected {wanted:0{width_bits // 4}x}[/red]")
            ok = False
            if i > 30:
                console.print("[dim]Further mismatches suppressed.[/dim]")
                break
    if ok:
        console.print(f"[green]✔ {label}: {len(expected_words)} words matched[/green]")
    return ok


def verify(args: argparse.Namespace) -> int:
    folder = args.case
    info = json.loads((folder / "case.json").read_text(encoding="utf-8"))
    header("RTL OUTPUT CHECK", info["name"])
    if not any((args.actual_output, args.actual_tag, args.actual_stream, args.actual_status)):
        args.actual_stream = folder / "actual_stream.hex"
        args.rtl = True
    status = args.actual_status
    if status is None and (folder / "actual_status.hex").is_file():
        status = folder / "actual_status.hex"
    choices = [args.actual_output, args.actual_tag, args.actual_stream, status]
    if not any(choices):
        raise ValueError("Supply --actual-output, --actual-tag, or --actual-stream")
    if args.actual_stream and (args.actual_output or args.actual_tag):
        raise ValueError("Use --actual-stream alone, or --actual-output/--actual-tag separately")
    result = True
    if status:
        got = read_word_file(status)
        result = got == [info["expected_auth_ok"]] and result
        console.print("[green]✔ Authentication status matched[/green]" if got == [info["expected_auth_ok"]]
                      else "[red]✘ Authentication status mismatch[/red]")
    if args.actual_output:
        if args.rtl:
            expected = pack_words(bytes.fromhex(info["ciphertext_hex_bytes"] if info["mode"] == "encrypt"
                                               else info["plaintext_hex_bytes"]), 4, "little")
            got = read_word_file(args.actual_output)
            matched = got == expected
            console.print("[green]✔ Native payload matched[/green]" if matched else "[red]✘ Native payload mismatch[/red]")
            result = matched and result
        else:
            result = compare_file(args.actual_output, folder / "expected_output.hex", info["word_bits"], "Payload") and result
    if args.actual_tag and info["mode"] == "decrypt":
        raise ValueError("Decrypt produces no output tag; check --actual-status instead")
    if args.actual_tag:
        # tag may be emitted in 128-bit single line or normal datapath-size words
        if args.rtl:
            size = 16 if args.tag_format == "single128" else 4
            expected = pack_words(bytes.fromhex(info["tag_hex_bytes"]), size, "little")
            matched = read_word_file(args.actual_tag) == expected
            console.print("[green]✔ Native tag matched[/green]" if matched else "[red]✘ Native tag mismatch[/red]")
            result = matched and result
        else:
            expected_file = folder / ("expected_tag.hex" if args.tag_format == "single128" else "expected_tag_words.hex")
            bits = 128 if args.tag_format == "single128" else info["word_bits"]
            result = compare_file(args.actual_tag, expected_file, bits, "Authentication tag") and result
    if args.actual_stream:
        result = compare_file(args.actual_stream, folder / ("rtl_expected.hex" if args.rtl else "expected_stream.hex"),
                              32 if args.rtl else info["word_bits"], "Output stream") and result
    console.print("[bold green]PASS[/bold green]" if result else "[bold red]FAIL[/bold red]")
    return 0 if result else 1


def selftest(_args: argparse.Namespace) -> int:
    header("SELF TEST", "RFC 8439 section 2.8.2")
    vec = rfc_vector()
    case = golden(vec, "encrypt")
    assert case.ciphertext == RFC_CIPHERTEXT and case.tag == RFC_TAG
    for width in (32, 128):
        for byteorder in ("little", "big"):
            n = width // 8
            packed = pack_words(vec.plaintext, n, byteorder)
            unpacked = b"".join(x.to_bytes(n, byteorder) for x in packed)[:len(vec.plaintext)]
            assert unpacked == vec.plaintext
            for size in (0, 1, n - 1, n, n + 1):
                sample = bytes(range(size))
                result = pack_words(sample, n, byteorder)
                restored = b"".join(x.to_bytes(n, byteorder) for x in result)[:size]
                assert restored == sample
    console.print("[bold green]✔ RFC ciphertext/tag verified; decrypt round-trip OK; word packing OK[/bold green]")
    return 0


def int_auto(s: str) -> int:
    return int(s, 0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cc20p1305-vectorgen",
        description="Golden AEAD ChaCha20-Poly1305 .hex test vectors for RTL testbenches.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    subs = parser.add_subparsers(dest="command", required=True)
    gen = subs.add_parser("generate", help="Generate .hex vector folders and a JSON manifest")
    gen.add_argument("--profile", choices=["suite", "rfc", "custom", "random"], default="suite")
    gen.add_argument("--mode", choices=["encrypt", "decrypt", "both"], default="both")
    gen.add_argument("--word-bits", type=int, choices=[32, 128], default=32)
    gen.add_argument("--byteorder", choices=["little", "big"], default="little")
    gen.add_argument("--out", type=Path, default=None, help="Default: dv/vectors/suite for suite, dv/vectors/custom otherwise")
    gen.add_argument("--force", action="store_true", help="Replace existing generator-owned output")
    gen.add_argument("--seed", type=int_auto, default=0x8439)
    gen.add_argument("--random-count", type=int, default=8, help="Extra suite messages, or message count for random profile")
    gen.add_argument("--max-random-bytes", type=int, default=512)
    gen.add_argument("--case-name", default="custom")
    gen.add_argument("--key-hex", help="32 bytes; custom profile only")
    gen.add_argument("--nonce-hex", help="12 bytes; custom profile only")
    gen.add_argument("--aad-hex", help="Additional authenticated data; custom profile only")
    gen.add_argument("--plaintext-hex", help="Plaintext data; custom profile only")
    gen.add_argument("--aad-file", type=Path, help="Raw binary AAD file; custom profile only")
    gen.add_argument("--plaintext-file", type=Path, help="Raw binary plaintext file; custom profile only")
    gen.set_defaults(func=generate)

    ins = subs.add_parser("inspect", help="Show a Rich table for a manifest folder or case")
    ins.add_argument("path", type=Path)
    ins.set_defaults(func=inspect)

    ver = subs.add_parser("verify", help="Compare DUT output .hex against one generated case")
    ver.add_argument("--case", type=Path, required=True, help="Path to generated case directory")
    ver.add_argument("--actual-output", type=Path, help="DUT payload words")
    ver.add_argument("--actual-tag", type=Path, help="DUT tag words")
    ver.add_argument("--actual-stream", type=Path, help="DUT payload words followed by tag words")
    ver.add_argument("--actual-status", type=Path, help="One word: DUT AUTH_OK at DONE")
    ver.add_argument("--rtl", action="store_true", help="Compare native 32-bit little-endian captures (automatic with no actual-file options)")
    ver.add_argument("--tag-format", choices=["single128", "words"], default="words")
    ver.set_defaults(func=verify)

    test = subs.add_parser("selftest", help="Check golden RFC 8439 reference and word packer")
    test.set_defaults(func=selftest)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (ValueError, OSError, KeyError, AssertionError, json.JSONDecodeError) as exc:
        error_console.print(f"[bold red]ERROR:[/bold red] {exc}", highlight=False)
        return 2
    except KeyboardInterrupt:
        error_console.print("Cancelled.")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
