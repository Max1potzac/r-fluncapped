using System.IO.Compression;

namespace Pvz2Conv;

/// Decompresses one RSG data block. The packet's compression flags aren't
/// trusted: mod repackers use zstd where the game's own packer used zlib,
/// and pad small compressed blocks to 4 KB so that the stored size equals
/// the real size. The block's own signature decides instead.
static class Unpack
{
    public static string Sniff(ReadOnlySpan<byte> data)
    {
        if (data.Length >= 4 && data[0] == 0x28 && data[1] == 0xB5 && data[2] == 0x2F && data[3] == 0xFD) return "zstd";
        if (data.Length >= 2 && (data[0] & 0x0F) == 8 && (data[0] >> 4) <= 7 && ((data[0] << 8) | data[1]) % 31 == 0) return "zlib";
        if (data.Length >= 3 && data[0] == 0x1F && data[1] == 0x8B && data[2] == 8) return "gzip";
        return "stored";
    }

    /// Returns exactly <paramref name="size"/> bytes and the method that worked.
    public static (byte[] data, string method) Block(ReadOnlySpan<byte> raw, int size, bool flaggedCompressed)
    {
        string kind = Sniff(raw);
        if (kind != "stored")
        {
            try { return (Decode(raw, size, kind), kind); }
            // a stored block can begin with bytes that look like a zlib header
            catch (InvalidDataException) when (raw.Length >= size) { }
        }
        else if (flaggedCompressed && raw.Length != size)
        {
            // flagged compressed and sized like it, but no known header: try raw deflate
            try { return (Decode(raw, size, "deflate"), "deflate"); }
            catch (InvalidDataException) { }
        }
        if (raw.Length < size)
            throw new FormatException($"block holds {raw.Length} bytes, {size} expected, and isn't in a known compressed format");
        return (raw[..size].ToArray(), "stored");
    }

    static byte[] Decode(ReadOnlySpan<byte> raw, int size, string kind)
    {
        var output = new byte[size];
        using var ms = new MemoryStream(raw.ToArray(), writable: false);
        // Read exactly size bytes and stop, so the zero padding some packers
        // put after a zstd frame is never parsed as another frame.
        using Stream s = kind switch
        {
            "zstd" => new ZstdSharp.DecompressionStream(ms),
            "zlib" => new ZLibStream(ms, CompressionMode.Decompress),
            "gzip" => new GZipStream(ms, CompressionMode.Decompress),
            _ => new DeflateStream(ms, CompressionMode.Decompress),
        };
        int got = 0;
        try
        {
            while (got < size)
            {
                int n = s.Read(output, got, size - got);
                if (n == 0) break;
                got += n;
            }
        }
        catch (ZstdSharp.ZstdException e) { throw new InvalidDataException($"zstd: {e.Message}"); }
        if (got != size) throw new InvalidDataException($"{kind} gave {got} bytes, {size} expected");
        return output;
    }
}
