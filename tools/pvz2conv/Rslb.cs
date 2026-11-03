namespace Pvz2Conv;

/// "RSLB": the block-LZMA container PvZ2 China (PVZ2C) wraps each of its
/// packages in (assets/*.rsb.smf); the game's own messages call it
/// "block-lzma". Inside is an ordinary RSB.
///   0x00  "RSLB", u32 version (1)
///   0x08  u64 size of the RSB inside
///   0x10  u64 size of everything after the header
///   0x18  u32 block size (32 MB), u32 block count
///   0x20  per block, 36 bytes: u64 offset in the RSB, u64 size there,
///         u64 offset in this file, u32 stored size, 5-byte LZMA props, 3 zero
/// A block is its 5 LZMA props bytes again followed by a raw LZMA stream
/// (no length field), so it decodes to exactly the size its entry gives.
static class Rslb
{
    public const uint Magic = 0x424C5352; // "RSLB"

    public static bool LooksLike(ReadOnlySpan<byte> head) => head.Length >= 4 && BitConverter.ToUInt32(head) == Magic;

    public static void Unwrap(string path, string dest, Action<string> log)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        var r = new BinaryReader(fs);
        if (r.ReadUInt32() != Magic) throw new FormatException("missing RSLB signature");
        uint version = r.ReadUInt32();
        ulong total = r.ReadUInt64();
        r.ReadUInt64();
        uint blockSize = r.ReadUInt32();
        uint count = r.ReadUInt32();
        if (count == 0 || count > 1 << 16 || total > 1UL << 40) throw new FormatException($"implausible RSLB header ({count} blocks, {total} bytes)");
        log($"unwrapping RSLB (block-LZMA) container v{version}: {count} blocks, {total:N0} bytes");

        var blocks = new (ulong Out, ulong OutSize, ulong In, uint InSize, byte[] Props)[count];
        for (int i = 0; i < count; i++)
        {
            blocks[i] = (r.ReadUInt64(), r.ReadUInt64(), r.ReadUInt64(), r.ReadUInt32(), r.ReadBytes(5));
            r.ReadBytes(3);
        }

        using var outFs = File.Create(dest);
        foreach (var (outOff, outSize, inOff, inSize, props) in blocks)
        {
            if (outOff != (ulong)outFs.Position) throw new FormatException($"RSLB block for offset {outOff} is out of order");
            if (inOff + inSize > (ulong)fs.Length) throw new FormatException("RSLB block runs past the end of the file (cut short?)");
            if (outSize > blockSize && blockSize != 0) throw new FormatException($"RSLB block of {outSize} bytes is larger than the block size {blockSize}");
            fs.Position = (long)inOff;
            var head = r.ReadBytes(5);
            if (!head.AsSpan().SequenceEqual(props)) throw new FormatException($"RSLB block at {inOff} doesn't start with its LZMA properties");
            var decoder = new SevenZip.Compression.LZMA.Decoder();
            decoder.SetDecoderProperties(props);
            long before = outFs.Position;
            decoder.Code(fs, outFs, inSize - 5, (long)outSize, null);
            if (outFs.Position - before != (long)outSize) throw new FormatException($"RSLB block at {inOff} gave {outFs.Position - before} bytes, {outSize} expected");
        }
        if ((ulong)outFs.Length != total) throw new FormatException($"RSLB gave {outFs.Length} bytes, the header says {total}");
    }
}
