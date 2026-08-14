"""
Survey every STRM (streamed audio) file on a GameCube disc image and report its
loop geometry.

Motivation: JASAramStream stages a STRM through a ring of fixed-size ARAM blocks.
A file loop whose start is not block-aligned forces the mixer to restart decoding
in the middle of a block (JASAramStream::updateChannel sets mLoopStartSample to
`loop_start % block_samples + n * block_samples`), which is the awkward case for
ADPCM history handling. This tool partitions the disc's streams into
block-aligned and non-block-aligned loops so that suspect tracks can be picked
out without playing the game.

Only plain ISO/GCM images are supported (no RVZ/WIA/CISO/GCZ).

Usage:
    python3 tools/strm_survey.py orig/GZ2E01/GZ2E01.iso
    python3 tools/strm_survey.py GZ2E01.iso --csv streams.csv
    python3 tools/strm_survey.py GZ2E01.iso --only-misaligned
"""

import argparse
import csv
import struct
import sys

# GameCube disc header fields.
FST_OFFSET_ADDR = 0x424
FST_SIZE_ADDR = 0x428
FST_ENTRY_SIZE = 12

# JASAramStream::Header, see libs/JSystem/include/JSystem/JAudio2/JASAramStream.h
STRM_TAG = 0x5354524D  # 'STRM'
STRM_HEADER_SIZE = 0x40
STREAM_FORMAT_ADPCM4 = 0
STREAM_FORMAT_PCM16 = 1

# JAU_JASInitializer: aramBlockSize_, see libs/JSystem/src/JAudio2/JAUInitializer.cpp
DEFAULT_BLOCK_SIZE = 0x2760

# JAUStreamStaticAramMgr reserves 0x14 blocks over aramChannelNum_ == 2 channels,
# so mAramBlocksPerChannel is 10 and mBufCount is one less. See Z2AudioMgr.cpp.
DEFAULT_ARAM_BLOCKS_PER_CHANNEL = 10


class Stream(object):
    """Parsed STRM header plus the derived loop geometry we care about."""

    def __init__(self, path, size, header):
        (
            self.tag,
            self.sound_block_size,
            self.format,
            self.bits,
            self.channels,
            self.loop,
            self.sample_rate,
            self.sample_count,
            self.loop_start,
            self.loop_end,
            self.block_size,
        ) = struct.unpack_from(">IIHHHHiIiiI", header, 0)
        self.volume = header[0x28]
        self.path = path
        self.size = size

    def block_samples(self):
        """JASAramStream::getBlockSamples()"""
        if self.format == STREAM_FORMAT_ADPCM4:
            return (self.block_size << 4) // 9
        return self.block_size >> 1

    def format_name(self):
        if self.format == STREAM_FORMAT_ADPCM4:
            return "ADPCM4"
        if self.format == STREAM_FORMAT_PCM16:
            return "PCM16"
        return "?%d" % self.format

    def loop_start_phase(self):
        """Offset of the loop start within its block. 0 means block-aligned."""
        return self.loop_start % self.block_samples()

    def loop_end_phase(self):
        return self.loop_end % self.block_samples()

    def loop_end_block(self):
        """The `loop_end_block` of JASAramStream::load()."""
        return (self.loop_end - 1) // self.block_samples()

    def loop_start_block(self):
        return self.loop_start // self.block_samples()

    def is_misaligned(self):
        """True when the file loop restarts mid-block."""
        return bool(self.loop) and self.loop_start_phase() != 0

    def loop_seconds(self):
        if not self.sample_rate:
            return 0.0
        return (self.loop_end - self.loop_start) / float(self.sample_rate)

    def warnings(self, blocks_per_channel):
        """Reproduce the JUT_WARN conditions JASAramStream::headerLoad would hit."""
        out = []
        buf_count = blocks_per_channel - 1
        if buf_count < 3:
            out.append("Too few Buffer-Size")
        if self.loop and self.loop_end_block() <= buf_count:
            out.append("Too few samples for Loop-buffer")
        if self.block_size != DEFAULT_BLOCK_SIZE:
            out.append("block_size != 0x%X" % DEFAULT_BLOCK_SIZE)
        if self.loop and self.loop_start_block() >= self.loop_end_block():
            # JUT_ASSERT(537, loop_start_block < loop_end_block) in load().
            out.append("loop_start_block >= loop_end_block")
        return out


def read_fst(fp):
    """Yield (path, offset, size) for every file in the disc image."""
    fp.seek(FST_OFFSET_ADDR)
    fst_offset, fst_size = struct.unpack(">II", fp.read(8))
    if fst_offset == 0 or fst_size == 0:
        raise ValueError("no FST found - is this a plain ISO/GCM image?")

    fp.seek(fst_offset)
    fst = fp.read(fst_size)
    if len(fst) < FST_ENTRY_SIZE:
        raise ValueError("truncated FST")

    entry_count = struct.unpack_from(">I", fst, 8)[0]
    strings = fst[entry_count * FST_ENTRY_SIZE:]

    def name_of(index):
        if index == 0:
            return ""
        name_offset = struct.unpack_from(">I", fst, index * FST_ENTRY_SIZE)[0] & 0xFFFFFF
        end = strings.find(b"\0", name_offset)
        return strings[name_offset:end].decode("shift-jis", "replace")

    # Walk iteratively; `dir_ends` tracks the exclusive end index of each open
    # directory so we know when to pop back out of it.
    path_parts = []
    dir_ends = [entry_count]
    for index in range(1, entry_count):
        while len(dir_ends) > 1 and index >= dir_ends[-1]:
            dir_ends.pop()
            path_parts.pop()

        flags, offset, length = struct.unpack_from(">III", fst, index * FST_ENTRY_SIZE)
        is_dir = (flags >> 24) & 1
        name = name_of(index)
        if is_dir:
            path_parts.append(name)
            dir_ends.append(length)
        else:
            yield "/".join(path_parts + [name]), offset, length


def find_streams(iso_path):
    """Return every file on the disc whose first four bytes are 'STRM'."""
    streams = []
    with open(iso_path, "rb") as fp:
        entries = list(read_fst(fp))
        # Reading in disc order keeps this to one forward pass over the image.
        for path, offset, size in sorted(entries, key=lambda e: e[1]):
            if size < STRM_HEADER_SIZE:
                continue
            fp.seek(offset)
            header = fp.read(STRM_HEADER_SIZE)
            if len(header) < STRM_HEADER_SIZE:
                continue
            if struct.unpack_from(">I", header, 0)[0] != STRM_TAG:
                continue
            streams.append(Stream(path, size, header))
    streams.sort(key=lambda s: s.path)
    return streams


COLUMNS = [
    "path",
    "format",
    "channels",
    "sample_rate",
    "loop",
    "loop_start",
    "loop_end",
    "loop_samples",
    "loop_seconds",
    "block_samples",
    "loop_start_phase",
    "loop_end_phase",
    "loop_start_block",
    "loop_end_block",
    "misaligned",
    "warnings",
]


def row_for(stream, blocks_per_channel):
    return {
        "path": stream.path,
        "format": stream.format_name(),
        "channels": stream.channels,
        "sample_rate": stream.sample_rate,
        "loop": int(bool(stream.loop)),
        "loop_start": stream.loop_start,
        "loop_end": stream.loop_end,
        "loop_samples": stream.loop_end - stream.loop_start,
        "loop_seconds": "%.2f" % stream.loop_seconds(),
        "block_samples": stream.block_samples(),
        "loop_start_phase": stream.loop_start_phase(),
        "loop_end_phase": stream.loop_end_phase(),
        "loop_start_block": stream.loop_start_block(),
        "loop_end_block": stream.loop_end_block(),
        "misaligned": int(stream.is_misaligned()),
        "warnings": "; ".join(stream.warnings(blocks_per_channel)),
    }


def print_table(rows):
    shown = [
        "path",
        "format",
        "sample_rate",
        "loop",
        "loop_start",
        "loop_end",
        "loop_start_phase",
        "loop_seconds",
        "warnings",
    ]
    widths = {}
    for name in shown:
        widths[name] = max([len(name)] + [len(str(r[name])) for r in rows])
    header = "  ".join(name.ljust(widths[name]) for name in shown)
    print(header)
    print("-" * len(header))
    for row in rows:
        print("  ".join(str(row[name]).ljust(widths[name]) for name in shown))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("iso", help="path to a plain ISO/GCM disc image")
    parser.add_argument("--csv", metavar="FILE", help="also write the full table as CSV")
    parser.add_argument("--only-misaligned", action="store_true", help="list only streams whose loop start is not block-aligned")
    parser.add_argument("--grep", metavar="TEXT", help="list only streams whose path contains TEXT")
    parser.add_argument("--blocks-per-channel", type=int, default=DEFAULT_ARAM_BLOCKS_PER_CHANNEL, help="mAramBlocksPerChannel to assume when reproducing headerLoad warnings (default: %(default)s)")
    args = parser.parse_args(argv)

    try:
        streams = find_streams(args.iso)
    except (OSError, ValueError) as err:
        print("error: %s" % err, file=sys.stderr)
        return 1

    if not streams:
        print("no STRM files found in %s" % args.iso, file=sys.stderr)
        return 1

    rows = [row_for(s, args.blocks_per_channel) for s in streams]

    if args.csv:
        with open(args.csv, "w", newline="") as fp:
            writer = csv.DictWriter(fp, fieldnames=COLUMNS)
            writer.writeheader()
            writer.writerows(rows)

    shown = rows
    if args.grep:
        shown = [r for r in shown if args.grep.lower() in r["path"].lower()]
    if args.only_misaligned:
        shown = [r for r in shown if r["misaligned"]]

    if shown:
        print_table(shown)
    else:
        print("(no streams matched)")

    looping = [r for r in rows if r["loop"]]
    misaligned = [r for r in rows if r["misaligned"]]
    flagged = [r for r in rows if r["warnings"]]
    print()
    print("%d STRM files, %d looping, %d with a non-block-aligned loop start, %d with headerLoad warnings" % (len(rows), len(looping), len(misaligned), len(flagged)))
    if args.csv:
        print("full table written to %s" % args.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
