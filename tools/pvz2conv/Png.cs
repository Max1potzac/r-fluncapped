using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace Pvz2Conv;

/// Minimal PNG writer: 8-bit RGBA, one IDAT, per-row filter picked by the
/// usual smallest-sum-of-differences rule. And a reader for 8-bit PNGs of any
/// colour type (not interlaced), giving RGBA.
static class Png
{
    /// A PNG's pixels as RGBA. Throws FormatException for what it can't read.
    public static (byte[] Rgba, int Width, int Height) Decode(byte[] png)
    {
        if (png.Length < 8 || !png.AsSpan(0, 8).SequenceEqual(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }))
            throw new FormatException("not a PNG");
        int width = 0, height = 0, depth = 0, type = -1, interlace = 0;
        byte[]? palette = null, trns = null;
        var idat = new MemoryStream();
        for (int at = 8; at + 12 <= png.Length;)
        {
            int len = BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(at));
            string kind = Encoding.ASCII.GetString(png, at + 4, 4);
            if (len < 0 || at + 12L + len > png.Length) throw new FormatException("the PNG is cut short");
            var data = png.AsSpan(at + 8, len);
            switch (kind)
            {
                case "IHDR":
                    width = BinaryPrimitives.ReadInt32BigEndian(data);
                    height = BinaryPrimitives.ReadInt32BigEndian(data[4..]);
                    depth = data[8]; type = data[9]; interlace = data[12];
                    break;
                case "PLTE": palette = data.ToArray(); break;
                case "tRNS": trns = data.ToArray(); break;
                case "IDAT": idat.Write(data); break;
            }
            if (kind == "IEND") break;
            at += 12 + len;
        }
        if (depth != 8) throw new FormatException($"the PNG has {depth}-bit channels (8-bit ones are read)");
        if (interlace != 0) throw new FormatException("the PNG is interlaced");
        if (width <= 0 || height <= 0 || (long)width * height > 1 << 26) throw new FormatException("the PNG's size is wrong");
        int channels = type switch { 0 => 1, 2 => 3, 3 => 1, 4 => 2, 6 => 4, _ => throw new FormatException($"PNG colour type {type} isn't known") };
        if (type == 3 && palette == null) throw new FormatException("the PNG has no palette");
        int stride = width * channels;
        var raw = new byte[(stride + 1) * height];
        idat.Position = 0;
        using (var z = new ZLibStream(idat, CompressionMode.Decompress))
        {
            int got = 0;
            while (got < raw.Length && z.Read(raw, got, raw.Length - got) is var n && n > 0) got += n;
            if (got != raw.Length) throw new FormatException("the PNG's data is cut short");
        }
        var pixels = new byte[stride * height];
        for (int y = 0; y < height; y++)
        {
            int f = raw[y * (stride + 1)];
            var src = raw.AsSpan(y * (stride + 1) + 1, stride);
            var row = pixels.AsSpan(y * stride, stride);
            var prev = y > 0 ? pixels.AsSpan((y - 1) * stride, stride) : Span<byte>.Empty;
            for (int i = 0; i < stride; i++)
            {
                int a = i >= channels ? row[i - channels] : 0, b = y > 0 ? prev[i] : 0, c = i >= channels && y > 0 ? prev[i - channels] : 0;
                row[i] = (byte)(src[i] + f switch { 0 => 0, 1 => a, 2 => b, 3 => (a + b) / 2, 4 => Paeth(a, b, c), _ => throw new FormatException($"PNG row filter {f} isn't known") });
            }
        }
        var rgba = new byte[width * height * 4];
        for (int p = 0; p < width * height; p++)
        {
            var s = pixels.AsSpan(p * channels, channels);
            var d = rgba.AsSpan(p * 4, 4);
            switch (type)
            {
                case 0: d[0] = d[1] = d[2] = s[0]; d[3] = trns is { Length: >= 2 } && trns[1] == s[0] && trns[0] == 0 ? (byte)0 : (byte)255; break;
                case 2:
                    s.CopyTo(d);
                    d[3] = trns is { Length: >= 6 } && trns[1] == s[0] && trns[3] == s[1] && trns[5] == s[2] ? (byte)0 : (byte)255;
                    break;
                case 3:
                    int i = s[0];
                    if (3 * i + 2 >= palette!.Length) throw new FormatException("a PNG pixel is past its palette");
                    d[0] = palette[3 * i]; d[1] = palette[3 * i + 1]; d[2] = palette[3 * i + 2];
                    d[3] = trns != null && i < trns.Length ? trns[i] : (byte)255;
                    break;
                case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
                default: s.CopyTo(d); break;
            }
        }
        return (rgba, width, height);
    }

    public static byte[] Encode(byte[] rgba, int width, int height)
    {
        int stride = width * 4;
        var filtered = new byte[(stride + 1) * height];
        var candidate = new byte[stride];
        for (int y = 0; y < height; y++)
        {
            var row = rgba.AsSpan(y * stride, stride);
            var prev = y > 0 ? rgba.AsSpan((y - 1) * stride, stride) : Span<byte>.Empty;
            var outRow = filtered.AsSpan(y * (stride + 1) + 1, stride);
            long best = long.MaxValue;
            for (int f = 0; f <= 4; f++)
            {
                long score = Filter(f, row, prev, candidate);
                if (score < best)
                {
                    best = score;
                    filtered[y * (stride + 1)] = (byte)f;
                    candidate.CopyTo(outRow);
                }
            }
        }

        using var idat = new MemoryStream();
        using (var z = new ZLibStream(idat, CompressionLevel.Fastest, leaveOpen: true))
            z.Write(filtered);

        using var png = new MemoryStream();
        png.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
        var ihdr = new byte[13];
        BinaryPrimitives.WriteInt32BigEndian(ihdr, width);
        BinaryPrimitives.WriteInt32BigEndian(ihdr.AsSpan(4), height);
        ihdr[8] = 8; ihdr[9] = 6; // 8 bits per channel, RGBA
        Chunk(png, "IHDR", ihdr);
        Chunk(png, "IDAT", idat.ToArray());
        Chunk(png, "IEND", Array.Empty<byte>());
        return png.ToArray();
    }

    static long Filter(int f, ReadOnlySpan<byte> row, ReadOnlySpan<byte> prev, Span<byte> dst)
    {
        int n = row.Length;
        bool top = prev.IsEmpty;
        switch (f)
        {
            case 0: row.CopyTo(dst); break;
            case 1:
                for (int i = 0; i < n; i++) dst[i] = (byte)(row[i] - (i >= 4 ? row[i - 4] : 0));
                break;
            case 2:
                if (top) row.CopyTo(dst);
                else for (int i = 0; i < n; i++) dst[i] = (byte)(row[i] - prev[i]);
                break;
            case 3:
                for (int i = 0; i < n; i++)
                    dst[i] = (byte)(row[i] - (((i >= 4 ? row[i - 4] : 0) + (top ? 0 : prev[i])) >> 1));
                break;
            default:
                for (int i = 0; i < n; i++)
                {
                    int a = i >= 4 ? row[i - 4] : 0, b = top ? 0 : prev[i], c = i >= 4 && !top ? prev[i - 4] : 0;
                    dst[i] = (byte)(row[i] - Paeth(a, b, c));
                }
                break;
        }
        long sum = 0;
        foreach (byte v in dst) sum += v < 128 ? v : 256 - v;
        return sum;
    }

    static int Paeth(int a, int b, int c)
    {
        int p = a + b - c, pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    }

    static void Chunk(Stream s, string type, byte[] data)
    {
        Span<byte> len = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(len, data.Length);
        s.Write(len);
        var typeBytes = Encoding.ASCII.GetBytes(type);
        s.Write(typeBytes);
        s.Write(data);
        uint crc = Crc32(typeBytes, 0xFFFFFFFF);
        crc = Crc32(data, crc) ^ 0xFFFFFFFF;
        BinaryPrimitives.WriteUInt32BigEndian(len, crc);
        s.Write(len);
    }

    static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n =>
    {
        uint c = (uint)n;
        for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();

    static uint Crc32(byte[] data, uint crc)
    {
        foreach (byte b in data) crc = CrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc;
    }
}
