"""
Probe what a streamed BGM's file loop point actually costs in decoded samples.

The dusk mixer restarts a looping stream by rewinding to the nearest ADPCM frame
boundary at or before `loop_start`, decoding from there, and discarding the
leading `loop_start % 16` samples (ReadChannelSamplesChunk in
src/dusk/audio/DuskDsp.cpp; `mSamplesPerBlock` is 16, the ADPCM frame, not the
17920-sample ARAM block). It seeds that decode with the ADPCM history it happens
to be holding -- `mpLast`/`mpPenult`, reloaded in FillDecodeBuf -- which are the
continuation values from an ARAM *block* header and are therefore only correct at
a block boundary. Whenever `loop_start` is not block-aligned, the seed applied at
a mid-block frame is wrong.

ADPCM is an IIR predictor, so a wrong seed produces an error that decays. The
question this tool answers is how much of that error survives into the samples
that are actually *emitted* -- i.e. past the at-most-15-sample discarded head.
An error that persists for hundreds of samples at every loop iteration is an
audible click; one that dies within the discarded head is a non-issue.

Usage:
    python3 tools/strm_loop_probe.py orig/GZ2E01/GZ2E01.iso Audiores/Stream/fairy.ast
    python3 tools/strm_loop_probe.py orig/GZ2E01/GZ2E01.iso --all-looping
"""

import argparse
import struct
import sys

from strm_survey import (
    STRM_HEADER_SIZE,
    Stream,
    read_fst,
)

ADPCM_FRAME_SIZE = 9
SAMPLES_PER_FRAME = 16
BLOCK_HEADER_SIZE = 0x20
CHANNEL_MAX = 6

# src/dusk/audio/Adpcm.cpp
COEF0 = [
    0x0000, 0x0800, 0x0000, 0x0400,
    0x1000, 0x0E00, 0x0C00, 0x1200,
    0x1068, 0x12C0, 0x1400, 0x0800,
    0x0400, 0xFC00, 0xFC00, 0xF800,
]
COEF1 = [
    0x0000, 0x0000, 0x0800, 0x0400,
    0xF800, 0xFA00, 0xFC00, 0xF600,
    0xF738, 0xF704, 0xF400, 0xF800,
    0xFC00, 0x0400, 0x0000, 0x0000,
]


def s16(value):
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def clamp16(value):
    if value > 0x7FFF:
        return 0x7FFF
    if value < -0x8000:
        return -0x8000
    return value


def decode_adpcm4(data, count, hist2, hist1):
    """Port of dusk::audio::Adpcm4ToPcm16. Returns (samples, hist2, hist1)."""
    out = []
    for base in range(0, len(data), ADPCM_FRAME_SIZE):
        header = data[base]
        scale = 1 << (header >> 4)
        coef_index = header & 0xF
        coef0 = s16(COEF0[coef_index])
        coef1 = s16(COEF1[coef_index])
        for index in range(SAMPLES_PER_FRAME):
            byte = data[base + 1 + index // 2]
            nibble = byte >> 4 if index % 2 == 0 else byte & 0xF
            if nibble & 0x8:
                nibble -= 0x10
            sample = clamp16((((nibble * scale) << 11) + (coef0 * hist1 + coef1 * hist2)) >> 11)
            hist2 = hist1
            hist1 = sample
            out.append(sample)
            if len(out) == count:
                return out, hist2, hist1
    return out, hist2, hist1


class StreamFile(object):
    """Random access to one STRM's blocks inside a disc image."""

    def __init__(self, fp, offset, size, header):
        self.fp = fp
        self.offset = offset
        self.size = size
        self.info = header
        self.channel_bytes = header.block_size
        self.stride = self.channel_bytes * header.channels + BLOCK_HEADER_SIZE

    def block_count(self):
        return (self.size - STRM_HEADER_SIZE) // self.stride

    def read_block(self, index, channel=0):
        """Return (adpcm_bytes, mpLast, mpPenult) for one block of one channel.

        mpLast/mpPenult are the continuation values stored in the *block header*,
        which describe the state entering this block.
        """
        base = self.offset + STRM_HEADER_SIZE + index * self.stride
        self.fp.seek(base)
        raw = self.fp.read(self.stride)
        if len(raw) < BLOCK_HEADER_SIZE:
            raise ValueError("block %d is past the end of the file" % index)
        tag, block_size = struct.unpack_from(">II", raw, 0)
        if tag != 0x424C434B:  # 'BLCK'
            raise ValueError("block %d has tag %08X, expected 'BLCK'" % (index, tag))
        conts = struct.unpack_from(">%dh" % (CHANNEL_MAX * 2), raw, 8)
        mp_last = conts[channel * 2]
        mp_penult = conts[channel * 2 + 1]
        start = BLOCK_HEADER_SIZE + block_size * channel
        return raw[start:start + block_size], mp_last, mp_penult


def find_stream(fp, entries, path):
    for entry_path, offset, size in entries:
        if entry_path.lower() == path.lower() or entry_path.lower().endswith("/" + path.lower()):
            fp.seek(offset)
            header = fp.read(STRM_HEADER_SIZE)
            if struct.unpack_from(">I", header, 0)[0] != 0x5354524D:
                raise ValueError("%s is not a STRM file" % entry_path)
            return StreamFile(fp, offset, size, Stream(entry_path, size, header))
    raise ValueError("no such file on the disc: %s" % path)


MEASURE_SAMPLES = 4096


def probe(stream, channel=0, verbose=True):
    """Measure how long a wrong ADPCM seed survives into the emitted region.

    Returns a dict of findings for the stream's loop restart.
    """
    info = stream.info
    block_samples = info.block_samples()
    phase = info.loop_start % block_samples
    loop_block = info.loop_start // block_samples

    data, mp_last, mp_penult = stream.read_block(loop_block, channel)

    # Decode the loop-start block from its own boundary with its own continuation
    # values. This is ground truth: what the stream is supposed to sound like.
    truth, _, _ = decode_adpcm4(data, block_samples, mp_penult, mp_last)

    # dusk restarts at the ADPCM frame boundary at or before loop_start, and
    # discards `loop_start % SAMPLES_PER_FRAME` samples from that frame.
    frame_index = phase // SAMPLES_PER_FRAME
    head = phase % SAMPLES_PER_FRAME
    frame_offset = frame_index * ADPCM_FRAME_SIZE
    tail = data[frame_offset:]
    count = min(MEASURE_SAMPLES + head, block_samples - frame_index * SAMPLES_PER_FRAME)

    # The correct seed at that frame: the state after decoding everything before it.
    if frame_index == 0:
        correct_seed = (mp_penult, mp_last)
    else:
        correct_seed = (truth[frame_index * SAMPLES_PER_FRAME - 2],
                        truth[frame_index * SAMPLES_PER_FRAME - 1])

    # What dusk actually seeds with: the block-header continuation pair, applied
    # at a mid-block frame. Also try neighbouring blocks' pairs, standing in for
    # "whatever the ring happened to hold", and the zero seed as a worst case.
    candidates = [("this block's header", mp_penult, mp_last)]
    total_blocks = stream.block_count()
    for other in (loop_block + 1, loop_block - 1):
        if 0 <= other < total_blocks and other != loop_block:
            _, other_last, other_penult = stream.read_block(other, channel)
            candidates.append(("block %d header" % other, other_penult, other_last))
    candidates.append(("zero seed", 0, 0))

    reference, _, _ = decode_adpcm4(tail, count, correct_seed[0], correct_seed[1])
    emitted_ref = reference[head:]
    peak = max((abs(v) for v in emitted_ref), default=0) or 1

    results = []
    for name, hist2, hist1 in candidates:
        wrong, _, _ = decode_adpcm4(tail, count, hist2, hist1)
        emitted = wrong[head:]
        limit = min(len(emitted), len(emitted_ref))
        errors = [abs(emitted[i] - emitted_ref[i]) for i in range(limit)]
        converged = None
        for index in range(limit - 1, -1, -1):
            if errors[index] != 0:
                converged = index + 1
                break
        results.append({
            "seed": name,
            "converged_after": converged,
            "first_error": errors[0] if errors else 0,
            "max_error": max(errors) if errors else 0,
        })

    finding = {
        "path": info.path,
        "channels": info.channels,
        "loop_start": info.loop_start,
        "loop_end": info.loop_end,
        "block_samples": block_samples,
        "loop_block": loop_block,
        "phase": phase,
        "frame_index": frame_index,
        "head": head,
        "peak": peak,
        "results": results,
    }

    if verbose:
        print("%s  (channel %d)" % (info.path, channel))
        print("  loop_start %d -> block %d, frame %d of the block, discarded head = %d samples" % (
            info.loop_start, loop_block, frame_index, head))
        print("  block-aligned loop: %s" % ("yes" if phase == 0 else "no"))
        print("  peak amplitude just after the loop point: %d" % peak)
        for res in finding["results"]:
            converged = "clean" if res["converged_after"] is None else "%d samples (%.1f ms)" % (
                res["converged_after"], 1000.0 * res["converged_after"] / info.sample_rate)
            print("    %-22s error persists %-22s first %6d, peak %6d (%5.1f%% of signal)" % (
                res["seed"], converged, res["first_error"], res["max_error"],
                100.0 * res["max_error"] / peak))
        print()

    return finding


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("iso", help="path to a plain ISO/GCM disc image")
    parser.add_argument("path", nargs="?", help="STRM path on the disc, e.g. Audiores/Stream/fairy.ast")
    parser.add_argument("--all-looping", action="store_true", help="probe every looping stream and summarise")
    parser.add_argument("--channel", type=int, default=0, help="which audio channel to decode (default: %(default)s)")
    args = parser.parse_args(argv)

    if not args.path and not args.all_looping:
        parser.error("give a STRM path or --all-looping")

    with open(args.iso, "rb") as fp:
        entries = list(read_fst(fp))

        if args.path:
            probe(find_stream(fp, entries, args.path), args.channel)
            return 0

        findings = []
        for path, offset, size in sorted(entries, key=lambda e: e[1]):
            if size < STRM_HEADER_SIZE:
                continue
            fp.seek(offset)
            header = fp.read(STRM_HEADER_SIZE)
            if len(header) < STRM_HEADER_SIZE or struct.unpack_from(">I", header, 0)[0] != 0x5354524D:
                continue
            info = Stream(path, size, header)
            if not info.loop:
                continue
            findings.append(probe(StreamFile(fp, offset, size, info), args.channel, verbose=False))

    # The seed dusk actually uses is the first candidate: this block's header pair.
    for finding in findings:
        finding["actual"] = finding["results"][0]
    findings.sort(key=lambda f: f["actual"]["max_error"] / float(f["peak"]), reverse=True)

    print("%-42s %8s %9s %9s %9s %8s" % ("path", "aligned", "persists", "peak err", "signal", "% signal"))
    print("-" * 92)
    survivors = 0
    for finding in findings:
        actual = finding["actual"]
        if actual["max_error"]:
            survivors += 1
        persists = "clean" if actual["converged_after"] is None else str(actual["converged_after"])
        print("%-42s %8s %9s %9d %9d %7.1f%%" % (
            finding["path"],
            "yes" if finding["phase"] == 0 else "no",
            persists,
            actual["max_error"],
            finding["peak"],
            100.0 * actual["max_error"] / finding["peak"]))
    print()
    print("%d looping streams probed; %d emit a wrong-prediction error at the loop restart" % (len(findings), survivors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
