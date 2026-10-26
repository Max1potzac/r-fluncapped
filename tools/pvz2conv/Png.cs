using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace Pvz2Conv;

/// Minimal PNG writer: 8-bit RGBA, one IDAT, per-row filter picked by the
/// usual smallest-sum-of-differences rule.
static class Png
{
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
