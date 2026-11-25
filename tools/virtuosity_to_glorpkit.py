#!/usr/bin/env python3
"""Convert Virtuosity Drums (CC0) from its Sforzando SFZ bank to .glorpkit.

Usage: virtuosity_to_glorpkit.py --source "/path/to/Virtuosity" --output kit.glorpkit

The full-kit program is flattened (includes + macros), all six mic positions are
kept, stereo mics become L/R mixer channels, and FLAC is transcoded to 24-bit
WAV. The "epic" transposed layers are skipped so they do not collide with the
normal note map.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections import OrderedDict
from pathlib import Path
from xml.sax.saxutils import quoteattr

MIC_TITLES = {
    "kickmic": "Kick Mic",
    "snaremic": "Snare Mic",
    "oh": "OH",
    "mid": "Mid",
    "room": "Room",
    "lofi": "Vintage",
}
MIC_PANS = {"kickmic": 0.0, "snaremic": 0.0, "lofi": 0.0,
            "oh_l": -0.55, "oh_r": 0.55, "mid_l": -0.35, "mid_r": 0.35,
            "room_l": -0.8, "room_r": 0.8}

# key -> (piece, zone, graphic, choke, alt note or None)
KEYMAP = {
    35: ("Kick", "center", "kick", "none", 36),
    36: ("Kick", "center", "kick", "none", 35),
    37: ("Snare", "stickshot", "snare", "none", None),
    38: ("Snare", "center", "snare", "none", None),
    39: ("Snare", "off center", "snare", "none", None),
    40: ("Snare", "rimshot", "snare", "none", None),
    41: ("Low Tom", "center", "ftom", "none", None),
    43: ("Low Tom", "off center", "ftom", "none", None),
    45: ("Low Tom", "rimshot", "ftom", "none", None),
    47: ("Low Tom", "cross stick", "ftom", "none", None),
    48: ("High Tom", "center", "tom1", "none", None),
    50: ("High Tom", "off center", "tom1", "none", None),
    49: ("Crash", "crash", "crash1", "none", None),
    57: ("Crash", "sizzle", "crash1", "none", None),
    51: ("Ride", "ride", "ride", "none", None),
    53: ("Ride", "bell", "ride", "none", None),
    59: ("Flat Ride", "ride", "ride", "none", None),
    55: ("Flat Ride", "crash", "ride", "none", None),
    42: ("Hihat", "closed", "hihat", "hihat", None),
    44: ("Hihat", "pedal", "hihat", "hihat", None),
    46: ("Hihat", "open", "hihat", "hihat", None),
    90: ("Hihat", "half", "hihat", "hihat", None),
    92: ("Hihat", "splash", "hihat", "hihat", None),
    94: ("Hihat", "3/4", "hihat", "hihat", None),
    85: ("Snare", "stickshot alt", "snare", "none", None),
    86: ("Snare", "muted", "snare", "none", None),
    87: ("Snare", "half open", "snare", "none", None),
    88: ("Snare", "cross stick", "snare", "none", None),
    89: ("Low Tom", "muted", "ftom", "none", None),
    91: ("Low Tom", "half open", "ftom", "none", None),
    93: ("Snare", "buzz", "snare", "none", None),
    95: ("Snare", "flam", "snare", "none", None),
    96: ("Snare", "roll", "snare", "none", None),
    # GM percussion
    54: ("Tambourine", "head", "hihat", "none", None),
    56: ("Cowbell", "head", "cowbell", "none", None),
    58: ("Vibraslap", "head", "cowbell", "none", None),
    60: ("Bongo", "high", "snare", "none", None),
    61: ("Bongo", "low", "snare", "none", None),
    62: ("Conga", "muted", "tom2", "none", None),
    63: ("Conga", "open", "tom2", "none", None),
    64: ("Conga", "low", "tom2", "none", None),
    65: ("Timbale", "high", "tom1", "none", None),
    66: ("Timbale", "low", "tom1", "none", None),
    67: ("Agogo", "high", "cowbell", "none", None),
    68: ("Agogo", "low", "cowbell", "none", None),
    69: ("Cabasa", "head", "hihat", "none", None),
    70: ("Shaker", "head", "hihat", "none", None),
    71: ("Whistle", "short", "cowbell", "none", None),
    72: ("Whistle", "long", "cowbell", "none", None),
    73: ("Guiro", "short", "snare", "none", None),
    74: ("Guiro", "long", "snare", "none", None),
    75: ("Claves", "head", "snare", "none", None),
    76: ("Woodblock", "high", "snare", "none", None),
    77: ("Woodblock", "low", "snare", "none", None),
    78: ("Cuica", "muted", "snare", "none", None),
    79: ("Cuica", "open", "snare", "none", None),
    80: ("Triangle", "muted", "hihat", "none", None),
    81: ("Triangle", "open", "hihat", "none", None),
    82: ("Shaker 2", "head", "hihat", "none", None),
    83: ("Jingle Bell", "head", "hihat", "none", None),
    84: ("Bell Tree", "head", "cowbell", "none", None),
}

LAYOUT = {
    "kick": (-155.2, -329.7, 345.0, 1),
    "ftom": (107.2, -272.1, 195.0, 2),
    "tom2": (15.9, -408.0, 155.0, 3),
    "tom1": (-122.6, -420.7, 138.0, 4),
    "snare": (-212.7, -259.5, 180.0, 5),
    "hihat": (-299.6, -404.7, 160.0, 6),
    "ride": (77.7, -507.0, 210.0, 7),
    "crash2": (187.3, -420.8, 205.0, 8),
    "crash1": (-235.0, -502.9, 190.0, 9),
    "cowbell": (-345.0, -150.0, 110.0, 12),
    "china": (330.0, -380.0, 140.0, 15),
}


def strip_comment(line: str) -> str:
    i = line.find("//")
    return line if i < 0 else line[:i]


def expand(text: str, macros: dict, depth: int = 0) -> str:
    if depth > 12 or "$" not in text:
        return text
    out, i = [], 0
    while i < len(text):
        if text[i] != "$":
            out.append(text[i]); i += 1; continue
        j = i + 1
        while j < len(text) and (text[j].isalnum() or text[j] == "_"):
            j += 1
        token = text[i:j]
        replaced = False
        for k in range(len(token), 1, -1):
            cand = token[:k]
            if cand in macros:
                out.append(expand(macros[cand], macros, depth + 1)); out.append(token[k:])
                i = j; replaced = True; break
        if not replaced:
            out.append(token); i = j
    return "".join(out)


def flatten(path: Path, program_dir: Path, macros: dict, out: list, depth: int = 0) -> None:
    if depth > 16:
        return
    for raw in path.read_text(errors="replace").splitlines():
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("#include"):
            m = re.search(r'#include\s+"?([^"]+)"?', line)
            if not m:
                continue
            inc = m.group(1)
            if "_epic_" in inc:
                continue
            inc_path = (program_dir / inc).resolve()
            flatten(inc_path, program_dir, macros, out, depth + 1)
        elif "#define" in line:
            for m in re.finditer(r"#define\s+(\$[A-Za-z0-9_]+)\s*([^#]*)", line):
                macros[m.group(1)] = expand(m.group(2).strip(), macros)
        else:
            out.append(expand(line, macros))


def parse_ops(text: str, ops: dict) -> None:
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i].isspace(): i += 1
        if i >= n: break
        start = i
        while i < n and text[i] != "=" and not text[i].isspace(): i += 1
        key = text[start:i].lower()
        while i < n and text[i].isspace(): i += 1
        if i >= n or text[i] != "=": continue
        i += 1
        while i < n and text[i].isspace(): i += 1
        if i < n and text[i] in "\"'":
            q = text[i]; i += 1; start = i
            while i < n and text[i] != q: i += 1
            value = text[start:i]
            if i < n: i += 1
        else:
            start = i
            while i < n and not text[i].isspace(): i += 1
            value = text[start:i]
        if key:
            ops[key] = value


def parse_regions(lines: list) -> list:
    global_ops, master_ops, group_ops = {}, {}, {}
    regions = []
    for line in lines:
        if line.startswith("<"):
            close = line.find(">")
            if close < 0: continue
            tag = line[1:close].strip().lower()
            rest = line[close + 1:]
            if tag == "region":
                ops = dict(group_ops)
                parse_ops(rest, ops)
                regions.append(ops)
            elif tag == "global":
                global_ops = {}; parse_ops(rest, global_ops)
                master_ops = dict(global_ops); group_ops = dict(global_ops)
            elif tag == "master":
                group_ops = dict(global_ops); parse_ops(rest, group_ops)
                master_ops = dict(group_ops)
            else:
                group_ops = dict(master_ops); parse_ops(rest, group_ops)
        else:
            parse_ops(line, group_ops)
    return regions


def sanitize(value: str) -> str:
    out = []
    for ch in value.lower():
        if ch.isalnum(): out.append(ch)
        elif ch in "-_": out.append(ch)
        elif ch.isspace(): out.append("_")
    return "".join(out).strip("_") or "piece"


def valid_wav(path: Path) -> bool:
    """Reject WAVs whose data chunk runs past the end of the file."""
    try:
        size = path.stat().st_size
        with path.open("rb") as f:
            header = f.read(12)
            if len(header) < 12 or header[:4] != b"RIFF" or header[8:12] != b"WAVE":
                return False
            pos = 12
            while pos + 8 <= size:
                f.seek(pos)
                chunk = f.read(8)
                if len(chunk) < 8:
                    return False
                cid, csize = chunk[:4], int.from_bytes(chunk[4:], "little")
                body = pos + 8
                if cid == b"data":
                    return csize > 0 and body + csize <= size
                pos = body + csize + (csize & 1)
    except Exception:
        return False
    return False


def audio_channels(path: Path, cache: dict) -> int:
    key = str(path)
    if key not in cache:
        try:
            out = subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "a:0",
                                           "-show_entries", "stream=channels", "-of", "csv=p=0", str(path)])
            cache[key] = int(out.decode().strip() or "1")
        except Exception:
            cache[key] = 1
    return cache[key]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    source = Path(args.source).expanduser().resolve()
    program_dir = source / "Programs"
    program = program_dir / "02-full-kit.sfz"
    if not program.is_file():
        print(f"error: {program} not found", file=sys.stderr)
        return 2

    macros, lines = {}, []
    flatten(program, program_dir, macros, lines)
    print(f"flattened {len(lines)} lines, {len(macros)} macros")
    regions = parse_regions(lines)

    channel_cache = {}
    mic_order, mic_seen = [], set()

    def add_mic(name, title):
        if name not in mic_seen:
            mic_seen.add(name)
            mic_order.append((name, title))

    zones = OrderedDict()  # (piece_id, zone) -> dict
    pieces = OrderedDict()  # piece_id -> dict

    def zone_for(piece, zone, graphic, choke, note, alt):
        pid = sanitize(piece)
        zkey = (pid, zone)
        if zkey not in zones:
            zones[zkey] = {"piece": piece, "zone": zone, "graphic": graphic,
                           "choke": choke, "note": note, "alt": alt,
                           "tiers": [], "hits": OrderedDict()}
            pieces.setdefault(pid, {"name": piece, "graphic": graphic, "choke": choke,
                                    "zones": []})
            pieces[pid]["zones"].append(zkey)
        return zones[zkey]

    used = 0
    for ops in regions:
        sample = (ops.get("sample") or "").strip()
        if not sample or sample.startswith("*"):
            continue
        if "hicc21" in ops:  # snares-off variant; keep the default (snares on)
            continue
        try:
            key = int(float(ops.get("key", "0") or 0))
        except ValueError:
            continue
        if key not in KEYMAP:
            continue
        if key == 35:
            # Snare-wires-wrong kick variant; key 36 is the normal kick and the
            # zone's alt-note still covers note 35.
            continue

        sample_path = (program_dir / sample).resolve()
        if not sample_path.is_file():
            continue
        if sample_path.suffix.lower() == ".wav" and not valid_wav(sample_path):
            continue
        rel = sample_path.relative_to(source / "Samples")
        parts = rel.parts
        base_mic = parts[1] if parts[0] == "perc" else parts[0]
        if base_mic not in MIC_TITLES:
            continue

        piece, zone, graphic, choke, alt = KEYMAP[key]
        z = zone_for(piece, zone, graphic, choke, key, alt)

        try:
            lovel = int(float(ops.get("lovel", "0") or 0))
            hivel = int(float(ops.get("hivel", "127") or 127))
            seq = int(float(ops.get("seq_position", "1") or 1))
        except ValueError:
            continue
        tier = (lovel, hivel)
        if tier not in z["tiers"]:
            z["tiers"].append(tier)
        tier_idx = z["tiers"].index(tier)
        rr = max(0, seq - 1)
        hkey = (key, tier_idx, rr)
        if hkey not in z["hits"]:
            z["hits"][hkey] = []

        channels = audio_channels(sample_path, channel_cache)
        if channels >= 2:
            lid, rid = f"{base_mic}_l", f"{base_mic}_r"
            add_mic(lid, MIC_TITLES[base_mic] + " L")
            add_mic(rid, MIC_TITLES[base_mic] + " R")
            entries = [(lid, 0), (rid, 1)]
        else:
            mid = base_mic
            add_mic(mid, MIC_TITLES[base_mic])
            entries = [(mid, 0)]

        for mic, channel in entries:
            if not any(e[0] == mic and e[1] == sample_path for e in z["hits"][hkey]):
                z["hits"][hkey].append((mic, sample_path, channel))
        used += 1

    if not pieces or not mic_order:
        print("error: no usable regions", file=sys.stderr)
        return 1

    print(f"regions used={used} pieces={len(pieces)} zones={len(zones)} mics={len(mic_order)}")
    total = sum(len(s) for z in zones.values() for s in z["hits"].values())
    print(f"mic sample entries={total}")

    output = Path(args.output).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    tmpdir = Path(tempfile.mkdtemp(prefix="virtuosity_"))
    wav_cache = {}

    def transcode(src: Path) -> Path:
        if src in wav_cache:
            return wav_cache[src]
        dst = tmpdir / (src.stem + ".wav")
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(src),
                        "-map_metadata", "-1", "-c:a", "pcm_s24le", str(dst)], check=True)
        wav_cache[src] = dst
        return dst

    try:
        with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
            xml = ['<?xml version="1.0" encoding="UTF-8"?>']
            xml.append('<glorpkit version="2.0" name="Virtuosity Drums" '
                       'author="Versilian Studios / Karoryfer" license="CC0 1.0" '
                       'samplerate="48000" hit-glow-color="#ff8200">')
            xml.append("  <description>Virtuosity Drums, a CC0 jazz drum kit by Versilian "
                       "Studios and Karoryfer, converted from the Sforzando SFZ bank.</description>")
            xml.append("  <mics>")
            for name, title in mic_order:
                pan = MIC_PANS.get(name, 0.0)
                xml.append(f'    <mic name={quoteattr(name)} title={quoteattr(title)} default="1.00" pan="{pan:.3f}" phase="false"/>')
            xml.append("  </mics>")
            xml.append("  <graphics>")
            for g in sorted({p["graphic"] for p in pieces.values()}):
                xml.append(f'    <graphic name={quoteattr(g)} src={quoteattr("builtin:" + g)}/>')
            xml.append("  </graphics>")
            xml.append("</glorpkit>")
            zf.writestr("kit.xml", "\n".join(xml) + "\n")

            layout_use, fallback = {}, 0
            for zi, (pid, p) in enumerate(pieces.items(), start=1):
                g = p["graphic"]
                use = layout_use.get(g, 0)
                layout_use[g] = use + 1
                if g in LAYOUT:
                    x, y, w, z = LAYOUT[g]
                    x += use * 34; y += use * 24; z += use
                else:
                    x = -190 + (fallback % 6) * 62
                    y = -255 - (fallback // 6) * 52
                    w = 125; z = 20 + zi
                    fallback += 1

                ix = ['<?xml version="1.0" encoding="UTF-8"?>']
                ix.append(f'<glorp-instrument name={quoteattr(p["name"])} graphic={quoteattr(g)} '
                          f'output-bus="0" choke-group={quoteattr(p["choke"])} '
                          f'norm-x="{x:.4f}" norm-y="{y:.4f}" norm-w="{w:.4f}" z-order="{z}">')
                for zkey in p["zones"]:
                    zobj = zones[zkey]
                    alt_attr = f' alt-note="{zobj["alt"]}"' if zobj["alt"] is not None else ""
                    ix.append(f'  <zone name={quoteattr(zobj["zone"])} graphic={quoteattr(zobj["graphic"])} '
                              f'note="{zobj["note"]}"{alt_attr}/>')
                ix.append("</glorp-instrument>")
                zf.writestr(f"instruments/{pid}/instrument.xml", "\n".join(ix) + "\n")

                for zkey in p["zones"]:
                    zobj = zones[zkey]
                    tiers = zobj["tiers"]
                    n = max(1, len(tiers))
                    ordered = sorted(zobj["hits"].items())
                    max_rr = max([0] + [k[2] for k in zobj["hits"].keys()])
                    zx = ['<?xml version="1.0" encoding="UTF-8"?>']
                    zx.append(f'<glorp-zone name={quoteattr(zobj["zone"])} power="1.0" '
                              f'round-robin="{max_rr + 1}" velocity-layers="{n}">')
                    written = set()
                    for (key, tier_idx, rr), samples in ordered:
                        power = 0.12 + (0.88 * tier_idx / (n - 1) if n > 1 else 0.88)
                        hit_name = f'{zobj["zone"]}_v{tier_idx + 1}_rr{rr + 1}'
                        for mic, src, channel in samples:
                            wav = transcode(src)
                            arc = wav.name
                            zx.append(f'  <sample name={quoteattr(hit_name)} '
                                      f'file={quoteattr("samples/" + arc)} '
                                      f'mic={quoteattr(mic)} power="{power:.4f}" '
                                      f'velocity="{tier_idx + 1}" round-robin="{rr + 1}"'
                                      + (f' channel="{channel + 1}"' if channel > 0 else "")
                                      + "/>")
                            if (zobj["zone"], arc) not in written:
                                written.add((zobj["zone"], arc))
                                zf.write(wav, f"instruments/{pid}/{zobj['zone']}/samples/{arc}")
                    zx.append("</glorp-zone>")
                    zf.writestr(f"instruments/{pid}/{zobj['zone']}/zone.xml", "\n".join(zx) + "\n")

        print(f"Wrote {output} ({output.stat().st_size / (1024**3):.2f} GiB)")
        return 0
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
