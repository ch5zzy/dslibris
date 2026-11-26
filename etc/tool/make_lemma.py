#!/usr/bin/env python3
"""
Build a language-agnostic .lem lemma table ("DLM1") or StarDict .syn file
from a tab-separated (inflected<TAB>lemma) text file.

Works with lemma lists from Simplemma, UniMorph, Wiktionary/Kaikki, etc.

Usage:
  # Build a standalone .lem sidecar (drop alongside your .ifo, e.g. wordnet.lem):
  python3 make_lemma.py --input en_lemmas.tsv --output Dictionary/wordnet.lem

  # Or bake directly into a StarDict .syn file matched against an existing .idx:
  python3 make_lemma.py --input en_lemmas.tsv --idx Dictionary/wordnet.idx --syn Dictionary/wordnet.syn
"""

import argparse
import struct
import sys


def stardict_sort_key(b: bytes):
    return (b.decode("utf-8", errors="replace").lower().encode("ascii", errors="replace")
            if b.isascii() else bytes(c + 32 if 65 <= c <= 90 else c for c in b), b)


def ascii_lower_bytes(b: bytes) -> bytes:
    return bytes(c + 32 if 65 <= c <= 90 else c for c in b)


def parse_tsv(path: str):
    pairs = set()
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            inflected = parts[0].strip()
            lemma = parts[1].strip()
            if not inflected or not lemma or inflected == lemma:
                continue
            pairs.add((inflected.encode("utf-8"), lemma.encode("utf-8")))
    return sorted(pairs, key=lambda kv: (ascii_lower_bytes(kv[0]), kv[0], kv[1]))


def write_lem(pairs, out_path: str):
    with open(out_path, "wb") as f:
        f.write(b"DLM1")
        f.write(struct.pack("<II", 0, len(pairs)))
        for inflected, lemma in pairs:
            f.write(inflected + b"\x00" + lemma + b"\x00")
    print(f"Wrote {len(pairs)} lemma mappings to {out_path}")


def read_idx_map(idx_path: str, offset_bits: int = 32):
    meta_len = 12 if offset_bits == 64 else 8
    raw = open(idx_path, "rb").read()
    word_to_index = {}
    pos = 0
    idx = 0
    n = len(raw)
    while pos < n:
        nul = raw.find(b"\x00", pos)
        if nul < 0 or nul + 1 + meta_len > n:
            break
        w = raw[pos:nul]
        word_to_index.setdefault(w, idx)
        word_to_index.setdefault(ascii_lower_bytes(w), idx)
        pos = nul + 1 + meta_len
        idx += 1
    return word_to_index


def write_syn(pairs, idx_path: str, syn_path: str, offset_bits: int = 32):
    word_map = read_idx_map(idx_path, offset_bits)
    syn_entries = []
    for inflected, lemma in pairs:
        target = word_map.get(lemma)
        if target is None:
            target = word_map.get(ascii_lower_bytes(lemma))
        if target is not None:
            syn_entries.append((inflected, target))

    syn_entries.sort(key=lambda kv: (ascii_lower_bytes(kv[0]), kv[0], kv[1]))
    with open(syn_path, "wb") as f:
        for inflected, target in syn_entries:
            f.write(inflected + b"\x00" + struct.pack(">I", target))
    print(f"Wrote {len(syn_entries)} StarDict synonyms to {syn_path}")


def main():
    p = argparse.ArgumentParser(description="Build .lem or StarDict .syn from inflected<TAB>lemma TSV.")
    p.add_argument("--input", required=True, help="Input TSV file (inflected<TAB>lemma)")
    p.add_argument("--output", help="Output .lem file path")
    p.add_argument("--idx", help="Existing StarDict .idx path (required when generating --syn)")
    p.add_argument("--syn", help="Output StarDict .syn file path")
    p.add_argument("--idx-bits", type=int, default=32, choices=[32, 64])
    args = p.parse_args()

    if not args.output and not args.syn:
        p.error("Specify --output <.lem> and/or --syn <.syn>.")

    pairs = parse_tsv(args.input)
    if args.output:
        write_lem(pairs, args.output)
    if args.syn:
        if not args.idx:
            p.error("--idx is required when generating --syn.")
        write_syn(pairs, args.idx, args.syn, args.idx_bits)


if __name__ == "__main__":
    main()
