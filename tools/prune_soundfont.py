#!/usr/bin/env python3
"""Prunes an SF2 SoundFont file to retain only instruments and samples needed by Warlords II.

Reads all R*.XMI and M*.XMI files to discover exact program and drum notes used,
then extracts only those presets, instruments, and audio samples from the input SF2,
re-pointing sample offsets to produce a minimal, high-quality SoundFont.
"""

import glob
import io
import os
import struct
import sys

MT32_TO_GM = [
      0,   1,   0,   2,   4,   4,   5,   3,  16,  17,  18,  16,  16,  19,  20,  21,
      6,   6,   6,   7,   7,   7,   8, 112,  62,  62,  63,  63,  38,  38,  39,  39,
     88,  95,  52,  98,  97,  99,  14,  54, 102,  96,  53, 102,  81, 100,  14,  80,
     48,  48,  49,  45,  41,  40,  42,  42,  43,  46,  45,  24,  25,  28,  27, 104,
     32,  32,  34,  33,  36,  37,  35,  35,  79,  73,  72,  72,  74,  75,  64,  65,
     66,  67,  71,  71,  68,  69,  70,  22,  56,  59,  57,  57,  60,  60,  58,  61,
     61,  11,  11,  98,  14,   9,  14,  13,  12, 107, 107,  77,  78,  78,  76,  76,
     47, 117, 127, 118, 118, 116, 115, 119, 115, 112,  55, 124, 123,   0,  14, 117
]


def scan_xmi(fpath):
    with open(fpath, "rb") as f:
        data = f.read()
    idx = data.find(b"EVNT")
    if idx == -1:
        return set(), set()
    size = struct.unpack(">I", data[idx + 4 : idx + 8])[0]
    evnt = data[idx + 8 : idx + 8 + size]
    pos = 0
    programs = set()
    drums = set()
    is_mt32 = os.path.basename(fpath).upper().startswith("M")

    while pos < len(evnt):
        b = evnt[pos]
        if b < 0x80:
            pos += 1
            continue
        pos += 1
        st = b
        nibble = st & 0xF0
        ch = st & 0x0F
        if st == 0xFF:
            pos += 1
            l = 0
            while pos < len(evnt):
                nb = evnt[pos]
                pos += 1
                l = (l << 7) | (nb & 0x7F)
                if not (nb & 0x80):
                    break
            pos += l
        elif st in (0xF0, 0xF7):
            l = 0
            while pos < len(evnt):
                nb = evnt[pos]
                pos += 1
                l = (l << 7) | (nb & 0x7F)
                if not (nb & 0x80):
                    break
            pos += l
        elif nibble == 0xC0:
            prg = evnt[pos]
            pos += 1
            if is_mt32 and ch != 9 and prg < 128:
                prg = MT32_TO_GM[prg]
            programs.add((ch, prg))
        elif nibble in (0x80, 0xA0, 0xB0, 0xE0):
            pos += 2
        elif nibble == 0x90:
            note = evnt[pos]
            pos += 2
            while pos < len(evnt):
                nb = evnt[pos]
                pos += 1
                if not (nb & 0x80):
                    break
            if ch == 9:
                drums.add(note)

    return programs, drums


def get_required_presets(sound_dir="original/SOUND"):
    melodic = set()
    drum_needed = False
    for p in glob.glob(os.path.join(sound_dir, "[RM]*.XMI")) + glob.glob(
        os.path.join(sound_dir, "[rm]*.xmi")
    ):
        prgs, dr = scan_xmi(p)
        for ch, prg in prgs:
            if ch == 9:
                drum_needed = True
            else:
                melodic.add(prg)
        if dr:
            drum_needed = True
    return melodic, drum_needed


def read_chunks(data, pos, end):
    chunks = {}
    while pos < end:
        cid = data[pos : pos + 4]
        csize = struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        chunks[cid] = (pos + 8, csize)
        pos += 8 + csize + (csize % 2)
    return chunks


def read_list_chunks(data, pos, end):
    subchunks = {}
    while pos < end:
        cid = data[pos : pos + 4]
        csize = struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        subchunks[cid] = (pos + 8, csize)
        pos += 8 + csize + (csize % 2)
    return subchunks


def prune_sf2(src_path, dst_path, required_melodic, keep_drums):
    with open(src_path, "rb") as f:
        data = f.read()

    riff, riff_size, sfbk = struct.unpack("<4sI4s", data[:12])
    assert riff == b"RIFF" and sfbk == b"sfbk"

    top_chunks = {}
    pos = 12
    while pos < riff_size + 8:
        cid = data[pos : pos + 4]
        csize = struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        if cid == b"LIST":
            ltype = data[pos + 8 : pos + 12]
            top_chunks[ltype] = (pos + 12, csize - 4)
        else:
            top_chunks[cid] = (pos + 8, csize)
        pos += 8 + csize + (csize % 2)

    # INFO chunk
    info_start, info_len = top_chunks[b"INFO"]
    info_bytes = data[info_start - 12 : info_start + info_len]

    # sdta chunk
    sdta_start, sdta_len = top_chunks[b"sdta"]
    sdta_sub = read_chunks(data, sdta_start, sdta_start + sdta_len)
    smpl_start, smpl_len = sdta_sub[b"smpl"]
    raw_smpl = data[smpl_start : smpl_start + smpl_len]

    # pdta chunk
    pdta_start, pdta_len = top_chunks[b"pdta"]
    pdta = read_chunks(data, pdta_start, pdta_start + pdta_len)

    # Parse phdr
    phdr_start, phdr_len = pdta[b"phdr"]
    num_presets = phdr_len // 38
    phdrs = []
    for i in range(num_presets):
        offset = phdr_start + i * 38
        name, preset, bank, bag_idx, lib, genre, morph = struct.unpack(
            "<20sHHHIII", data[offset : offset + 38]
        )
        phdrs.append({
            "name": name,
            "preset": preset,
            "bank": bank,
            "bag_idx": bag_idx,
            "lib": lib,
            "genre": genre,
            "morph": morph,
            "orig_idx": i,
        })

    # Parse pbag
    pbag_start, pbag_len = pdta[b"pbag"]
    num_pbags = pbag_len // 4
    pbags = [struct.unpack("<HH", data[pbag_start + i * 4 : pbag_start + i * 4 + 4]) for i in range(num_pbags)]

    # Parse pmod
    pmod_start, pmod_len = pdta[b"pmod"]
    pmods = data[pmod_start : pmod_start + pmod_len]

    # Parse pgen
    pgen_start, pgen_len = pdta[b"pgen"]
    num_pgens = pgen_len // 4
    pgens = [struct.unpack("<HH", data[pgen_start + i * 4 : pgen_start + i * 4 + 4]) for i in range(num_pgens)]

    # Parse inst
    inst_start, inst_len = pdta[b"inst"]
    num_insts = inst_len // 22
    insts = []
    for i in range(num_insts):
        offset = inst_start + i * 22
        name, bag_idx = struct.unpack("<20sH", data[offset : offset + 22])
        insts.append({"name": name, "bag_idx": bag_idx, "orig_idx": i})

    # Parse ibag
    ibag_start, ibag_len = pdta[b"ibag"]
    num_ibags = ibag_len // 4
    ibags = [struct.unpack("<HH", data[ibag_start + i * 4 : ibag_start + i * 4 + 4]) for i in range(num_ibags)]

    # Parse imod
    imod_start, imod_len = pdta[b"imod"]
    imods = data[imod_start : imod_start + imod_len]

    # Parse igen
    igen_start, igen_len = pdta[b"igen"]
    num_igens = igen_len // 4
    igens = [struct.unpack("<HH", data[igen_start + i * 4 : igen_start + i * 4 + 4]) for i in range(num_igens)]

    # Parse shdr
    shdr_start, shdr_len = pdta[b"shdr"]
    num_shdrs = shdr_len // 46
    shdrs = []
    for i in range(num_shdrs):
        offset = shdr_start + i * 46
        name, start, end, start_loop, end_loop, srate, pitch, corr, link, stype = struct.unpack(
            "<20sIIIIIBbHH", data[offset : offset + 46]
        )
        shdrs.append({
            "name": name,
            "start": start,
            "end": end,
            "start_loop": start_loop,
            "end_loop": end_loop,
            "srate": srate,
            "pitch": pitch,
            "corr": corr,
            "link": link,
            "stype": stype,
            "orig_idx": i,
        })

    # Filter presets: bank 0 with preset in required_melodic, or bank 128 (drums)
    kept_preset_indices = []
    for i in range(num_presets - 1):
        p = phdrs[i]
        if p["bank"] == 0 and p["preset"] in required_melodic:
            kept_preset_indices.append(i)
        elif keep_drums and p["bank"] == 128 and p["preset"] == 0:
            kept_preset_indices.append(i)

    # Determine kept instruments from kept presets
    needed_inst_indices = set()
    for pi in kept_preset_indices:
        bag_start = phdrs[pi]["bag_idx"]
        bag_end = phdrs[pi + 1]["bag_idx"]
        for bi in range(bag_start, bag_end):
            gen_start = pbags[bi][0]
            gen_end = pbags[bi + 1][0]
            for gi in range(gen_start, gen_end):
                oper, val = pgens[gi]
                if oper == 41:  # instrument generator
                    needed_inst_indices.add(val)

    kept_inst_indices = sorted(list(needed_inst_indices))
    inst_old_to_new = {old: new for new, old in enumerate(kept_inst_indices)}

    # Determine kept samples from kept instruments
    needed_sample_indices = set()
    for ii in kept_inst_indices:
        bag_start = insts[ii]["bag_idx"]
        bag_end = insts[ii + 1]["bag_idx"]
        for bi in range(bag_start, bag_end):
            gen_start = ibags[bi][0]
            gen_end = ibags[bi + 1][0]
            for gi in range(gen_start, gen_end):
                oper, val = igens[gi]
                if oper == 53:  # sampleID generator
                    needed_sample_indices.add(val)

    kept_shdr_indices = sorted(list(needed_sample_indices))
    shdr_old_to_new = {old: new for new, old in enumerate(kept_shdr_indices)}

    print(f"Keeping {len(kept_preset_indices)} presets, {len(kept_inst_indices)} instruments, {len(kept_shdr_indices)} samples")

    # Build new smpl chunk by extracting and repointing kept samples
    new_smpl_data = bytearray()
    new_shdrs = []
    sample_pos = 0  # sample points (words)

    for si in kept_shdr_indices:
        s = shdrs[si]
        start_bytes = s["start"] * 2
        end_bytes = s["end"] * 2
        chunk = raw_smpl[start_bytes:end_bytes]

        length_samples = s["end"] - s["start"]
        loop_start_rel = s["start_loop"] - s["start"]
        loop_end_rel = s["end_loop"] - s["start"]

        new_start = sample_pos
        new_end = new_start + length_samples
        new_loop_start = new_start + loop_start_rel
        new_loop_end = new_start + loop_end_rel

        new_smpl_data.extend(chunk)
        # Pad with 46 blank sample words as per SoundFont spec
        padding = 46 * 2
        new_smpl_data.extend(b"\x00" * padding)
        sample_pos += length_samples + 46

        new_link = shdr_old_to_new.get(s["link"], 0)
        new_shdrs.append({
            "name": s["name"],
            "start": new_start,
            "end": new_end,
            "start_loop": new_loop_start,
            "end_loop": new_loop_end,
            "srate": s["srate"],
            "pitch": s["pitch"],
            "corr": s["corr"],
            "link": new_link,
            "stype": s["stype"],
        })

    # Add terminal EOS sentinel to shdrs
    new_shdrs.append({
        "name": b"EOS\x00" + b"\x00" * 16,
        "start": 0, "end": 0, "start_loop": 0, "end_loop": 0,
        "srate": 0, "pitch": 0, "corr": 0, "link": 0, "stype": 0,
    })

    # Rebuild instruments
    new_insts = []
    new_ibags = []
    new_igens = []
    new_imods = bytearray()

    for ii in kept_inst_indices:
        inst = insts[ii]
        bag_start = inst["bag_idx"]
        bag_end = insts[ii + 1]["bag_idx"]
        new_inst_bag_start = len(new_ibags)

        for bi in range(bag_start, bag_end):
            gen_start = ibags[bi][0]
            gen_end = ibags[bi + 1][0]
            new_gen_start = len(new_igens)

            for gi in range(gen_start, gen_end):
                oper, val = igens[gi]
                if oper == 53:  # sampleID
                    val = shdr_old_to_new[val]
                new_igens.append((oper, val))

            new_ibags.append((new_gen_start, 0))

        new_insts.append({"name": inst["name"], "bag_idx": new_inst_bag_start})

    # Sentinel EOI and ibag
    new_insts.append({"name": b"EOI\x00" + b"\x00" * 16, "bag_idx": len(new_ibags)})
    new_ibags.append((len(new_igens), 0))

    # Rebuild presets
    new_phdrs = []
    new_pbags = []
    new_pgens = []

    for pi in kept_preset_indices:
        p = phdrs[pi]
        bag_start = p["bag_idx"]
        bag_end = phdrs[pi + 1]["bag_idx"]
        new_preset_bag_start = len(new_pbags)

        for bi in range(bag_start, bag_end):
            gen_start = pbags[bi][0]
            gen_end = pbags[bi + 1][0]
            new_gen_start = len(new_pgens)

            for gi in range(gen_start, gen_end):
                oper, val = pgens[gi]
                if oper == 41:  # instrument
                    val = inst_old_to_new[val]
                new_pgens.append((oper, val))

            new_pbags.append((new_gen_start, 0))

        new_phdrs.append({
            "name": p["name"],
            "preset": p["preset"],
            "bank": p["bank"],
            "bag_idx": new_preset_bag_start,
            "lib": p["lib"],
            "genre": p["genre"],
            "morph": p["morph"],
        })

    # Sentinel EOP and pbag
    new_phdrs.append({
        "name": b"EOP\x00" + b"\x00" * 16,
        "preset": 0, "bank": 0, "bag_idx": len(new_pbags),
        "lib": 0, "genre": 0, "morph": 0,
    })
    new_pbags.append((len(new_pgens), 0))

    # Serialize pdta subchunks
    def pack_subchunk(cid, data_bytes):
        return cid + struct.pack("<I", len(data_bytes)) + data_bytes + (b"\x00" if len(data_bytes) % 2 else b"")

    phdr_data = bytearray()
    for p in new_phdrs:
        phdr_data.extend(struct.pack("<20sHHHIII", p["name"], p["preset"], p["bank"], p["bag_idx"], p["lib"], p["genre"], p["morph"]))

    pbag_data = bytearray()
    for b in new_pbags:
        pbag_data.extend(struct.pack("<HH", b[0], b[1]))

    pmod_data = bytearray(b"\x00" * 10)  # Empty sentinel

    pgen_data = bytearray()
    for g in new_pgens:
        pgen_data.extend(struct.pack("<HH", g[0], g[1]))
    if len(pgen_data) == 0:
        pgen_data.extend(b"\x00\x00\x00\x00")

    inst_data = bytearray()
    for inst in new_insts:
        inst_data.extend(struct.pack("<20sH", inst["name"], inst["bag_idx"]))

    ibag_data = bytearray()
    for b in new_ibags:
        ibag_data.extend(struct.pack("<HH", b[0], b[1]))

    imod_data = bytearray(b"\x00" * 10)  # Empty sentinel

    igen_data = bytearray()
    for g in new_igens:
        igen_data.extend(struct.pack("<HH", g[0], g[1]))
    if len(igen_data) == 0:
        igen_data.extend(b"\x00\x00\x00\x00")

    shdr_data = bytearray()
    for s in new_shdrs:
        shdr_data.extend(struct.pack("<20sIIIIIBbHH", s["name"], s["start"], s["end"], s["start_loop"], s["end_loop"], s["srate"], s["pitch"], s["corr"], s["link"], s["stype"]))

    pdta_body = (
        pack_subchunk(b"phdr", phdr_data)
        + pack_subchunk(b"pbag", pbag_data)
        + pack_subchunk(b"pmod", pmod_data)
        + pack_subchunk(b"pgen", pgen_data)
        + pack_subchunk(b"inst", inst_data)
        + pack_subchunk(b"ibag", ibag_data)
        + pack_subchunk(b"imod", imod_data)
        + pack_subchunk(b"igen", igen_data)
        + pack_subchunk(b"shdr", shdr_data)
    )
    pdta_chunk = b"LIST" + struct.pack("<I", len(pdta_body) + 4) + b"pdta" + pdta_body

    # Serialize sdta
    smpl_chunk = pack_subchunk(b"smpl", new_smpl_data)
    sdta_chunk = b"LIST" + struct.pack("<I", len(smpl_chunk) + 4) + b"sdta" + smpl_chunk

    # Complete RIFF sfbk
    total_riff_body = info_bytes + sdta_chunk + pdta_chunk
    out_data = b"RIFF" + struct.pack("<I", len(total_riff_body) + 4) + b"sfbk" + total_riff_body

    os.makedirs(os.path.dirname(os.path.abspath(dst_path)), exist_ok=True)
    with open(dst_path, "wb") as f:
        f.write(out_data)

    src_mb = len(data) / (1024 * 1024)
    dst_mb = len(out_data) / (1024 * 1024)
    print(f"Pruned SoundFont: {src_path} ({src_mb:.1f} MB) -> {dst_path} ({dst_mb:.2f} MB)")


def main():
    sf2_in = sys.argv[1] if len(sys.argv) > 1 else "/tmp/w2_sf2/GeneralUser_GS.sf2"
    sf2_out = sys.argv[2] if len(sys.argv) > 2 else "original/SOUND/warlords2.sf2"
    sound_dir = sys.argv[3] if len(sys.argv) > 3 else "original/SOUND"

    if not os.path.exists(sf2_in):
        sys.exit(f"Error: input soundfont {sf2_in} does not exist.")

    melodic, drum_needed = get_required_presets(sound_dir)
    prune_sf2(sf2_in, sf2_out, melodic, drum_needed)


if __name__ == "__main__":
    main()
