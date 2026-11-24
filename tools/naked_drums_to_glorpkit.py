#!/usr/bin/env python3
"""
naked_drums_to_glorpkit.py

Convert the Wilkinson Audio "Naked Drums" multi-mic SFZ library into a
single multi-mic .glorpkit.

The library is not a flat SFZ file: it uses #include and #define macros,
and the multi-mic version is split across 16 SFZ files under Multi/.
This tool resolves the includes/macros, reads the non-variable articulation
set, merges the same hit across the 16 mic files, transcodes FLAC to 24-bit
WAV (the Glorp sample cache reads WAV), and writes a GlorpKit v2 archive.

Example:
    ./naked_drums_to_glorpkit.py \
        --source "/path/to/Wilkinson Audio/Naked Drums" \
        --output "$HOME/.config/SuiteDrumsGlorp/kits/Naked Drums.glorpkit"

The source samples are CC-BY 4.0 by Wilkinson Audio; the converted kit keeps
that attribution and license in kit.xml.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections import OrderedDict
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr


def strip_comment(line: str) -> str:
    i = line.find("//")
    return line if i < 0 else line[:i]


def expand_macros(text: str, macros: dict, depth: int = 0) -> str:
    if depth > 12 or "$" not in text:
        return text
    out = []
    i = 0
    while i < len(text):
        if text[i] != "$":
            out.append(text[i])
            i += 1
            continue
        j = i + 1
        while j < len(text) and (text[j].isalnum() or text[j] == "_"):
            j += 1
        token = text[i:j]
        replaced = False
        # Longest defined prefix: fixes $MIC_$ART_$VEL1_$RR.
        for k in range(len(token), 1, -1):
            cand = token[:k]
            if cand in macros:
                out.append(expand_macros(macros[cand], macros, depth + 1))
                out.append(token[k:])
                i = j
                replaced = True
                break
        if not replaced:
            out.append(token)
            i = j
    return "".join(out)


def preprocess(path: Path, root_dir: Path, macros: dict, stack: set) -> list[str]:
    path = path.resolve()
    if path in stack:
        return []
    stack.add(path)
    out = []
    for raw in path.read_text(errors="replace").splitlines():
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("#include"):
            m = re.match(r'#include\s+"?([^"]+)"?', line)
            if m:
                inc = (root_dir / m.group(1)).resolve()
                out.extend(preprocess(inc, root_dir, macros, stack))
        elif "#define" in line:
            for m in re.finditer(r"#define\s+(\$[A-Za-z0-9_]+)\s*([^#]*)", line):
                key = m.group(1)
                macros[key] = expand_macros(m.group(2).strip(), macros)
        else:
            out.append(expand_macros(line, macros))
    stack.remove(path)
    return out


def parse_ops(text: str, ops: dict) -> None:
    i = 0
    n = len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        start = i
        while i < n and text[i] != "=" and not text[i].isspace():
            i += 1
        key = text[start:i].lower()
        while i < n and text[i].isspace():
            i += 1
        if i >= n or text[i] != "=":
            continue
        i += 1
        while i < n and text[i].isspace():
            i += 1
        if i < n and text[i] in "\"'":
            q = text[i]
            i += 1
            start = i
            while i < n and text[i] != q:
                i += 1
            value = text[start:i]
            if i < n:
                i += 1
        else:
            start = i
            while i < n and not text[i].isspace():
                i += 1
            value = text[start:i]
        if key:
            ops[key] = value


def parse_sfz(sfz_path: Path) -> tuple[list[dict], dict]:
    root_dir = sfz_path.parent
    macros: dict = {}
    lines = preprocess(sfz_path, root_dir, macros, set())

    global_ops: dict = {}
    group_ops: dict = {}
    regions: list[dict] = []
    current: dict | None = None
    master_label = ""
    default_path = ""

    def keep_master(ops: dict) -> None:
        nonlocal master_label, default_path
        if "master_label" in ops:
            master_label = ops["master_label"]
            global_ops["master_label"] = master_label
        if "default_path" in ops:
            default_path = ops["default_path"]

    for line in lines:
        if line.startswith("<"):
            close = line.find(">")
            if close < 0:
                continue
            tag = line[1:close].strip().lower()
            rest = line[close + 1:]
            if tag == "region":
                if current is not None:
                    regions.append(current)
                current = dict(group_ops)
                if master_label:
                    current["master_label"] = master_label
                parse_ops(rest, current)
            else:
                if current is not None:
                    regions.append(current)
                    current = None
                if tag == "global":
                    global_ops = {}
                    parse_ops(rest, global_ops)
                    if master_label:
                        global_ops["master_label"] = master_label
                    group_ops = dict(global_ops)
                else:
                    group_ops = dict(global_ops)
                    parse_ops(rest, group_ops)
                    keep_master(group_ops)
                    if "default_path" in group_ops:
                        global_ops["default_path"] = group_ops["default_path"]
        elif current is not None:
            parse_ops(line, current)
        else:
            parse_ops(line, group_ops)
            keep_master(group_ops)
            if "default_path" in group_ops:
                global_ops["default_path"] = group_ops["default_path"]

    if current is not None:
        regions.append(current)
    return regions, {"default_path": default_path, "master_label": master_label}


def sanitize(value: str) -> str:
    out = []
    for ch in value.lower():
        if ch.isalnum():
            out.append(ch)
        elif ch in "-_":
            out.append(ch)
        elif ch.isspace():
            out.append("_")
    return "".join(out).strip("_") or "piece"


def map_master(label: str) -> str:
    raw = (label or "").strip()
    low = raw.lower().replace(" ", "").replace("$lab_", "")
    if low.startswith("snr") or low.startswith("snare"):
        if low.endswith("1"):
            return "Snare 1"
        if low.endswith("2"):
            return "Snare 2"
        return "Snare"
    table = {
        "kick": "Kick",
        "snare": "Snare",
        "tom1": "Tom 1",
        "tom2": "Tom 2",
        "tom3": "Tom 3",
        "tom4": "Tom 4",
        "tom5": "Tom 5",
        "hats": "Hihat",
        "hh": "Hihat",
        "ride": "Ride",
        "rd": "Ride",
        "crash1": "Crash 1",
        "crash2": "Crash 2",
        "crs1": "Crash 1",
        "crs2": "Crash 2",
        "splash1": "Splash 1",
        "splash2": "Splash 2",
        "spl1": "Splash 1",
        "spl2": "Splash 2",
        "china1": "China 1",
        "china2": "China 2",
        "chn1": "China 1",
        "chn2": "China 2",
    }
    return table.get(low, raw.title() if raw else "Percussion")


def map_zone(label: str) -> str:
    low = (label or "").strip().lower()
    table = {
        "half": "semi",
        "center": "center",
        "sidestick": "sidestick",
        "tight": "tight",
        "closed": "closed",
        "pedal": "pedal",
        "open": "open",
        "bow": "bow",
        "bell": "bell",
        "edge": "edge",
        "choke": "choke",
    }
    return table.get(low, low or "center")


def graphic_for(piece: str, zone: str) -> str:
    p = piece.lower()
    if "kick" in p:
        return "kick"
    if "snare" in p:
        return "snare"
    if "hihat" in p or "hat" in p:
        return "hihat"
    if "floor" in p or p in ("tom4", "tom5", "tom 4", "tom 5"):
        return "ftom"
    if "tom" in p:
        return "tom2"
    if "ride" in p:
        return "ride"
    if "crash2" in p or "crash 2" in p:
        return "crash2"
    if "crash" in p or "splash" in p or "china" in p:
        return "crash1"
    return "snare"


def sample_channels(path: Path, cache: dict) -> int:
    key = str(path)
    if key in cache:
        return cache[key]
    try:
        out = subprocess.check_output(["soxi", "-c", str(path)], stderr=subprocess.DEVNULL)
        count = int(out.decode().strip() or "1")
    except Exception:
        count = 1
    cache[key] = count
    return count


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, help="Path to the 'Naked Drums' folder")
    ap.add_argument("--output", required=True, help="Output .glorpkit path")
    ap.add_argument("--dry-run", action="store_true", help="Parse and report without writing")
    ap.add_argument("--only-mics", default="", help="Comma-separated Multi SFZ stems for a test subset")
    args = ap.parse_args()

    source = Path(args.source).expanduser().resolve()
    multi_dir = source / "Multi"
    samples_dir = source / "Samples"
    if not multi_dir.is_dir() or not samples_dir.is_dir():
        print(f"error: expected Multi/ and Samples/ under {source}", file=sys.stderr)
        return 2

    wanted = {x.strip() for x in args.only_mics.split(",") if x.strip()}
    sfz_files = sorted(multi_dir.glob("*.sfz"))
    if wanted:
        sfz_files = [f for f in sfz_files if f.stem in wanted]
    if not sfz_files:
        print("error: no matching Multi/*.sfz files", file=sys.stderr)
        return 2

    channel_cache: dict = {}
    mic_order: list[tuple[str, str]] = []  # (name, title)
    mic_seen: set[str] = set()

    def add_mic(name: str, title: str) -> None:
        if name not in mic_seen:
            mic_seen.add(name)
            mic_order.append((name, title))

    def mic_for(base: str, sample: Path) -> list[tuple[str, str, int]]:
        """Return (mic_name, mic_title, file_channel) for a sample."""
        if base == "Snare Tops":
            name = sample.name
            if "TopOne" in name:
                add_mic("snare_top_1", "Snare Top 1")
                return [("snare_top_1", "Snare Top 1", 0)]
            if "TopTwo" in name:
                add_mic("snare_top_2", "Snare Top 2")
                return [("snare_top_2", "Snare Top 2", 0)]

        channels = sample_channels(sample, channel_cache)
        if channels >= 2:
            lid = sanitize(base) + "_l"
            rid = sanitize(base) + "_r"
            add_mic(lid, base + " L")
            add_mic(rid, base + " R")
            return [(lid, base + " L", 0), (rid, base + " R", 1)]

        mid = sanitize(base)
        add_mic(mid, base)
        return [(mid, base, 0)]

    # Load every region from every mic file.
    raw_regions: list[dict] = []
    for sfz in sfz_files:
        base = sfz.stem
        regions, globals_ = parse_sfz(sfz)
        for r in regions:
            sample = (r.get("sample") or "").strip()
            if not sample or sample.startswith("*"):
                continue
            if r.get("sw_last", "") == "13":
                continue  # variable hi-hat mode: keep the non-variable set
            try:
                key = int(float(r.get("key", "0") or 0))
            except ValueError:
                continue
            if key <= 13 or key >= 60:
                continue

            sample_path = Path(sample)
            if not sample_path.is_absolute():
                default_path = (globals_.get("default_path") or "").strip()
                sample_path = (sfz.parent / default_path / sample_path).resolve()
            if not sample_path.is_file():
                continue

            raw_regions.append(
                {
                    "base": base,
                    "key": key,
                    "lovel": int(float(r.get("lovel", "0") or 0)),
                    "hivel": int(float(r.get("hivel", "127") or 127)),
                    "seq": int(float(r.get("seq_position", "1") or 1)),
                    "master": r.get("master_label", ""),
                    "group": r.get("group_label", ""),
                    "sample": sample_path,
                }
            )

    if not raw_regions:
        print("error: no usable regions found", file=sys.stderr)
        return 1

    # Build pieces/zones and merge the same hit across mics.
    pieces: "OrderedDict[str, dict]" = OrderedDict()
    zone_keys: dict[tuple[str, str], str] = {}
    hits: dict[tuple[str, str, int, int, int], dict] = {}

    for r in raw_regions:
        piece = map_master(r["master"])
        zone = map_zone(r["group"])
        if zone == "choke":
            # Choke articulations share the cymbal's note in the SFZ. The plugin
            # already chokes from polyphonic aftertouch and closed hi-hats, so
            # skip these duplicate-note zones or they steal the main hit.
            continue
        piece_id = sanitize(piece)
        zkey = (piece_id, zone)

        if piece_id not in pieces:
            pieces[piece_id] = {
                "id": piece_id,
                "name": piece,
                "graphic": graphic_for(piece, zone),
                "choke": "hihat" if "hihat" in piece.lower() else "none",
                "zones": OrderedDict(),
            }
        p = pieces[piece_id]
        if zone not in p["zones"]:
            p["zones"][zone] = {
                "name": zone,
                "graphic": graphic_for(piece, zone),
                "note": r["key"],
                "hits": OrderedDict(),
            }
            zone_keys[zkey] = zone

        # Merge by velocity tier + round robin. Tiers are assigned per zone.
        zone_obj = p["zones"][zone]
        tier_key = (r["lovel"], r["hivel"])
        if tier_key not in zone_obj.setdefault("tiers", []):
            zone_obj["tiers"].append(tier_key)
        tier = zone_obj["tiers"].index(tier_key)
        rr = max(0, r["seq"] - 1)
        hkey = (piece_id, zone, r["key"], tier, rr)
        if hkey not in hits:
            hits[hkey] = {
                "name": f"{zone}_v{tier + 1}_rr{rr + 1}",
                "power": 1.0,
                "velocity": tier,
                "roundrobin": rr,
                "samples": [],
            }
        for mic_name, mic_title, channel in mic_for(r["base"], r["sample"]):
            entry = (mic_name, r["sample"], channel)
            if entry not in hits[hkey]["samples"]:
                hits[hkey]["samples"].append(entry)

    # Finalize tier counts, powers, and hit names.
    for (piece_id, zone, key, tier, rr), hit in hits.items():
        z = pieces[piece_id]["zones"][zone]
        tiers = z["tiers"]
        n = max(1, len(tiers))
        hit["power"] = 0.12 + (0.88 * tier / (n - 1) if n > 1 else 0.88)
        z["hits"][(key, tier, rr)] = hit

    for p in pieces.values():
        for z in p["zones"].values():
            z["num_vel"] = max(1, len(z.get("tiers", [])))
            z["num_rr"] = max([1] + [h["roundrobin"] + 1 for h in z["hits"].values()])
            z["hits"] = [h for _, h in sorted(z["hits"].items())]

    total_samples = sum(
        len(h["samples"]) for p in pieces.values() for z in p["zones"].values() for h in z["hits"]
    )
    print(f"Source: {source}")
    print(f"Mic files: {len(sfz_files)}")
    print(f"Mics: {len(mic_order)}")
    print(f"Pieces: {len(pieces)}")
    print(f"Zones: {sum(len(p['zones']) for p in pieces.values())}")
    print(f"Hit groups: {len(hits)}")
    print(f"Mic sample entries: {total_samples}")

    if args.dry_run:
        print("Mics:")
        for name, title in mic_order:
            print(f"  {name}: {title}")
        print("Pieces:")
        for p in pieces.values():
            print(f"  {p['name']} ({p['id']}) -> " + ", ".join(p["zones"].keys()))
            for zn, z in p["zones"].items():
                print(f"      {zn}: vel={z['num_vel']} rr={z['num_rr']} hits={len(z['hits'])}")
        print("Dry run: no archive written.")
        return 0

    output = Path(args.output).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    tmpdir = Path(tempfile.mkdtemp(prefix="naked_drums_"))
    wav_cache: dict[Path, Path] = {}

    def transcode(src: Path) -> Path:
        if src in wav_cache:
            return wav_cache[src]
        out = tmpdir / (src.stem.replace("/", "_") + ".wav")
        subprocess.run(["sox", str(src), "-b", "24", str(out)], check=True)
        wav_cache[src] = out
        return out

    try:
        with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
            # kit.xml
            xml = ['<?xml version="1.0" encoding="UTF-8"?>']
            xml.append(
                '<glorpkit version="2.0" name="Naked Drums" '
                'author="Wilkinson Audio / SFZ conversion by kinwie" '
                'license="CC-BY 4.0" samplerate="44100" hit-glow-color="#ff8200">'
            )
            xml.append(
                "  <description>Multi-mic Naked Drums converted from the Wilkinson Audio "
                "SFZ library. 16 mic channels, 5 velocity layers and 10 round robins.</description>"
            )
            xml.append("  <mics>")
            for name, title in mic_order:
                xml.append(f'    <mic name={quoteattr(name)} title={quoteattr(title)} default="1.00"/>')
            xml.append("  </mics>")
            graphics = sorted({p["graphic"] for p in pieces.values()})
            xml.append("  <graphics>")
            for g in graphics:
                xml.append(f'    <graphic name={quoteattr(g)} src={quoteattr("builtin:" + g)}/>')
            xml.append("  </graphics>")
            xml.append("</glorpkit>")
            zf.writestr("kit.xml", "\n".join(xml) + "\n")

            # Pieces
            default_layout = {
                "kick": (-155.2, -329.7, 345.0, 1),
                "ftom": (107.2, -272.1, 195.0, 2),
                "tom2": (15.9, -408.0, 155.0, 3),
                "tom1": (-122.6, -420.7, 138.0, 4),
                "snare": (-212.7, -259.5, 180.0, 5),
                "hihat": (-299.6, -404.7, 160.0, 6),
                "ride": (77.7, -507.0, 210.0, 7),
                "crash2": (187.3, -420.8, 205.0, 8),
                "crash1": (-235.0, -502.9, 190.0, 9),
            }
            layout_use: dict[str, int] = {}
            fallback = 0
            for zi, (piece_id, p) in enumerate(pieces.items(), start=1):
                g = p["graphic"]
                use = layout_use.get(g, 0)
                layout_use[g] = use + 1
                if g in default_layout:
                    x, y, w, z = default_layout[g]
                    x += use * 34
                    y += use * 24
                    z += use
                else:
                    x = -190 + (fallback % 6) * 62
                    y = -255 - (fallback // 6) * 52
                    w = 125
                    z = 20 + zi
                    fallback += 1
                ix = ['<?xml version="1.0" encoding="UTF-8"?>']
                ix.append(
                    f'<glorp-instrument name={quoteattr(p["name"])} graphic={quoteattr(g)} '
                    f'output-bus="0" choke-group={quoteattr(p["choke"])} '
                    f'norm-x="{x:.4f}" norm-y="{y:.4f}" norm-w="{w:.4f}" z-order="{z}">'
                )
                for zone, zobj in p["zones"].items():
                    ix.append(
                        f'  <zone name={quoteattr(zone)} graphic={quoteattr(zobj["graphic"])} '
                        f'note="{zobj["note"]}"/>'
                    )
                ix.append("</glorp-instrument>")
                zf.writestr(f"instruments/{piece_id}/instrument.xml", "\n".join(ix) + "\n")

                for zone, zobj in p["zones"].items():
                    zx = ['<?xml version="1.0" encoding="UTF-8"?>']
                    zx.append(
                        f'<glorp-zone name={quoteattr(zone)} power="1.0" '
                        f'round-robin="{zobj["num_rr"]}" velocity-layers="{zobj["num_vel"]}">'
                    )
                    written: set[str] = set()
                    for hit in zobj["hits"]:
                        for mic_name, src, channel in hit["samples"]:
                            wav = transcode(src)
                            arc_name = wav.name
                            zx.append(
                                f'  <sample name={quoteattr(hit["name"])} '
                                f'file={quoteattr("samples/" + arc_name)} '
                                f'mic={quoteattr(mic_name)} power="{hit["power"]:.4f}" '
                                f'velocity="{hit["velocity"] + 1}" '
                                f'round-robin="{hit["roundrobin"] + 1}"'
                                + (f' channel="{channel + 1}"' if channel > 0 else "")
                                + "/>"
                            )
                            if arc_name not in written:
                                written.add(arc_name)
                                zf.write(wav, f"instruments/{piece_id}/{zone}/samples/{arc_name}")
                    zx.append("</glorp-zone>")
                    zf.writestr(f"instruments/{piece_id}/{zone}/zone.xml", "\n".join(zx) + "\n")

        print(f"Wrote {output} ({output.stat().st_size / (1024**3):.2f} GiB)")
        return 0
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
