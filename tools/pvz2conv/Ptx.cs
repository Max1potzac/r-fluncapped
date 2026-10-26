namespace Pvz2Conv;

/// PTX: a raw texture with no header of its own. Width, height, pitch and
/// format live in the RSB's texture table, so a PTX can only be decoded
/// when it comes out of an OBB together with that entry.
/// Every decoder writes RGBA, 4 bytes per pixel, rows top to bottom.
static class Ptx
{
    public static string FormatName(uint format) => format switch
    {
        0 => "RGBA8888",
        1 => "RGBA4444",
        2 => "RGB565",
        3 => "RGBA5551",
        21 => "RGBA4444 tiled",
        22 => "RGB565 tiled",
        23 => "RGBA5551 tiled",
        147 => "ETC1 + A8",
        150 => "ETC2 RGBA8 (EAC alpha)",
        _ => $"format {format}",
    };

    public static bool Supported(uint format) => format is 0 or 1 or 2 or 3 or 21 or 22 or 23 or 147 or 150;

    /// Decodes to RGBA. Throws FormatException for an unknown format or
    /// data too short for the size the RSB gives.
    public static byte[] Decode(ReadOnlySpan<byte> src, int width, int height, uint format, int pitch)
    {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
            throw new FormatException($"bad texture size {width}x{height}");
        var rgba = new byte[width * height * 4];
        switch (format)
        {
            case 0: DecodeRgba8888(src, rgba, width, height, pitch); break;
            case 1 or 2 or 3: Decode16(src, rgba, width, height, format, pitch); break;
            case 21 or 22 or 23: Decode16Tiled(src, rgba, width, height, format - 20); break;
            case 147: DecodeEtc1A8(src, rgba, width, height); break;
            case 150: DecodeEtc2Rgba(src, rgba, width, height); break;
            default: throw new FormatException($"texture format {format} isn't supported yet");
        }
        return rgba;
    }

    static void Need(ReadOnlySpan<byte> src, long bytes, string what)
    {
        if (src.Length < bytes)
            throw new FormatException($"{what} needs {bytes} bytes, the PTX has {src.Length}");
    }

    // 0: bytes R, G, B, A in memory (Android). Rows are pitch bytes apart.
    static void DecodeRgba8888(ReadOnlySpan<byte> src, byte[] dst, int w, int h, int pitch)
    {
        if (pitch < w * 4) pitch = w * 4;
        Need(src, (long)pitch * (h - 1) + w * 4, "RGBA8888");
        for (int y = 0; y < h; y++)
            src.Slice(y * pitch, w * 4).CopyTo(dst.AsSpan(y * w * 4));
    }

    static void Put16(ushort v, uint format, byte[] dst, int o)
    {
        switch (format)
        {
            case 1: // RGBA4444
                dst[o] = (byte)((v >> 12) * 17); dst[o + 1] = (byte)((v >> 8 & 15) * 17);
                dst[o + 2] = (byte)((v >> 4 & 15) * 17); dst[o + 3] = (byte)((v & 15) * 17);
                break;
            case 2: // RGB565
                dst[o] = Expand(v >> 11, 5); dst[o + 1] = Expand(v >> 5 & 63, 6);
                dst[o + 2] = Expand(v & 31, 5); dst[o + 3] = 255;
                break;
            default: // RGBA5551
                dst[o] = Expand(v >> 11, 5); dst[o + 1] = Expand(v >> 6 & 31, 5);
                dst[o + 2] = Expand(v >> 1 & 31, 5); dst[o + 3] = (byte)((v & 1) * 255);
                break;
        }
    }

    static void Decode16(ReadOnlySpan<byte> src, byte[] dst, int w, int h, uint format, int pitch)
    {
        if (pitch < w * 2) pitch = w * 2;
        Need(src, (long)pitch * (h - 1) + w * 2, FormatName(format));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                Put16(BitConverter.ToUInt16(src.Slice(y * pitch + x * 2, 2)), format, dst, (y * w + x) * 4);
    }

    // 21/22/23: the image is cut into 32x32 tiles stored left to right, top
    // to bottom, each tile row by row.
    static void Decode16Tiled(ReadOnlySpan<byte> src, byte[] dst, int w, int h, uint format)
    {
        const int T = 32;
        int tilesX = (w + T - 1) / T, tilesY = (h + T - 1) / T;
        Need(src, (long)tilesX * tilesY * T * T * 2, FormatName(format + 20));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int tile = (y / T) * tilesX + x / T;
                int i = tile * T * T + (y % T) * T + x % T;
                Put16(BitConverter.ToUInt16(src.Slice(i * 2, 2)), format, dst, (y * w + x) * 4);
            }
    }

    static byte Expand(int v, int bits) => (byte)((v << (8 - bits)) | (v >> (2 * bits - 8)));

    static int BlocksX(int w) => (w + 3) / 4;
    static int BlocksY(int h) => (h + 3) / 4;

    // 147: ETC1 blocks for the whole texture, then one alpha byte per pixel.
    static void DecodeEtc1A8(ReadOnlySpan<byte> src, byte[] dst, int w, int h)
    {
        int colorBytes = BlocksX(w) * BlocksY(h) * 8;
        Need(src, colorBytes, "ETC1");
        Span<byte> block = stackalloc byte[64];
        for (int by = 0; by < BlocksY(h); by++)
            for (int bx = 0; bx < BlocksX(w); bx++)
            {
                ulong bits = ReadBE64(src, (by * BlocksX(w) + bx) * 8);
                Etc.DecodeColor(bits, block, allowEtc2: false);
                Store(block, dst, w, h, bx, by, alpha: false);
            }
        var alpha = src[colorBytes..];
        if (alpha.Length >= w * h)
            for (int i = 0; i < w * h; i++) dst[i * 4 + 3] = alpha[i];
        else
            throw new FormatException($"ETC1 + A8 alpha plane needs {w * h} bytes, {alpha.Length} left");
    }

    // 150: 16-byte blocks, an EAC alpha block then an ETC2 colour block.
    static void DecodeEtc2Rgba(ReadOnlySpan<byte> src, byte[] dst, int w, int h)
    {
        Need(src, BlocksX(w) * BlocksY(h) * 16, "ETC2 RGBA8");
        Span<byte> block = stackalloc byte[64];
        for (int by = 0; by < BlocksY(h); by++)
            for (int bx = 0; bx < BlocksX(w); bx++)
            {
                int o = (by * BlocksX(w) + bx) * 16;
                Etc.DecodeColor(ReadBE64(src, o + 8), block, allowEtc2: true);
                Etc.DecodeAlpha(ReadBE64(src, o), block);
                Store(block, dst, w, h, bx, by, alpha: true);
            }
    }

    static ulong ReadBE64(ReadOnlySpan<byte> src, int o) => System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(src.Slice(o, 8));

    // block holds 16 RGBA pixels, row by row
    static void Store(ReadOnlySpan<byte> block, byte[] dst, int w, int h, int bx, int by, bool alpha)
    {
        for (int y = 0; y < 4; y++)
        {
            int py = by * 4 + y;
            if (py >= h) break;
            for (int x = 0; x < 4; x++)
            {
                int px = bx * 4 + x;
                if (px >= w) break;
                int d = (py * w + px) * 4, s = (y * 4 + x) * 4;
                dst[d] = block[s]; dst[d + 1] = block[s + 1]; dst[d + 2] = block[s + 2];
                dst[d + 3] = alpha ? block[s + 3] : (byte)255;
            }
        }
    }
}

/// ETC1 / ETC2 / EAC block decoding as defined in the Khronos texture
/// format spec. A block is a 64-bit big-endian word; pixel (x, y) uses
/// index bit x*4 + y.
static class Etc
{
    static readonly int[,] Etc1Mod =
    {
        { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 },
    };

    static readonly int[] Distance = { 3, 6, 11, 16, 23, 32, 41, 64 };

    static readonly int[,] EacMod =
    {
        { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 },
        { -2, -5, -8, -13, 1, 4, 7, 12 }, { -2, -4, -6, -13, 1, 3, 5, 12 },
        { -3, -6, -8, -12, 2, 5, 7, 11 }, { -3, -7, -9, -11, 2, 6, 8, 10 },
        { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },
        { -2, -6, -8, -10, 1, 5, 7, 9 }, { -2, -5, -8, -10, 1, 4, 7, 9 },
        { -2, -4, -8, -10, 1, 3, 7, 9 }, { -2, -5, -7, -10, 1, 4, 6, 9 },
        { -3, -4, -7, -10, 2, 3, 6, 9 }, { -1, -2, -3, -10, 0, 1, 2, 9 },
        { -4, -6, -8, -9, 3, 5, 7, 8 }, { -3, -5, -7, -9, 2, 4, 6, 8 },
    };

    static int Bits(ulong v, int high, int count) => (int)((v >> (high - count + 1)) & ((1UL << count) - 1));
    static byte Clamp(int v) => (byte)(v < 0 ? 0 : v > 255 ? 255 : v);
    static int X4(int v) => v * 17;
    static int X5(int v) => (v << 3) | (v >> 2);
    static int X6(int v) => (v << 2) | (v >> 4);
    static int X7(int v) => (v << 1) | (v >> 6);
    static int Signed3(int v) => (v << 29) >> 29;

    static int PixelIndex(ulong b, int x, int y)
    {
        int i = x * 4 + y;
        return (int)(((b >> (16 + i)) & 1) << 1 | ((b >> i) & 1));
    }

    static void Set(Span<byte> block, int x, int y, int r, int g, int bl)
    {
        int o = (y * 4 + x) * 4;
        block[o] = Clamp(r); block[o + 1] = Clamp(g); block[o + 2] = Clamp(bl); block[o + 3] = 255;
    }

    public static void DecodeColor(ulong b, Span<byte> block, bool allowEtc2)
    {
        bool diff = (b >> 33 & 1) != 0;
        int r1, g1, b1, r2, g2, b2;
        if (!diff)
        {
            r1 = X4(Bits(b, 63, 4)); r2 = X4(Bits(b, 59, 4));
            g1 = X4(Bits(b, 55, 4)); g2 = X4(Bits(b, 51, 4));
            b1 = X4(Bits(b, 47, 4)); b2 = X4(Bits(b, 43, 4));
        }
        else
        {
            int r = Bits(b, 63, 5), g = Bits(b, 55, 5), bb = Bits(b, 47, 5);
            int rd = r + Signed3(Bits(b, 58, 3)), gd = g + Signed3(Bits(b, 50, 3)), bd = bb + Signed3(Bits(b, 42, 3));
            if (allowEtc2 && (rd < 0 || rd > 31)) { DecodeT(b, block); return; }
            if (allowEtc2 && (gd < 0 || gd > 31)) { DecodeH(b, block); return; }
            if (allowEtc2 && (bd < 0 || bd > 31)) { DecodePlanar(b, block); return; }
            r1 = X5(r); g1 = X5(g); b1 = X5(bb);
            r2 = X5(rd & 31); g2 = X5(gd & 31); b2 = X5(bd & 31);
        }
        bool flip = (b >> 32 & 1) != 0;
        int t1 = Bits(b, 39, 3), t2 = Bits(b, 36, 3);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
            {
                bool second = flip ? y >= 2 : x >= 2;
                int t = second ? t2 : t1;
                int idx = PixelIndex(b, x, y);
                int m = Etc1Mod[t, idx & 1];
                if ((idx & 2) != 0) m = -m;
                if (second) Set(block, x, y, r2 + m, g2 + m, b2 + m);
                else Set(block, x, y, r1 + m, g1 + m, b1 + m);
            }
    }

    static void Paint(ulong b, Span<byte> block, ReadOnlySpan<int> paint)
    {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
            {
                int p = PixelIndex(b, x, y) * 3;
                Set(block, x, y, paint[p], paint[p + 1], paint[p + 2]);
            }
    }

    static void DecodeT(ulong b, Span<byte> block)
    {
        int r1 = X4(Bits(b, 60, 2) << 2 | Bits(b, 57, 2)), g1 = X4(Bits(b, 55, 4)), b1 = X4(Bits(b, 51, 4));
        int r2 = X4(Bits(b, 47, 4)), g2 = X4(Bits(b, 43, 4)), b2 = X4(Bits(b, 39, 4));
        int d = Distance[Bits(b, 35, 2) << 1 | Bits(b, 32, 1)];
        Paint(b, block, stackalloc int[]
        {
            r1, g1, b1,
            r2 + d, g2 + d, b2 + d,
            r2, g2, b2,
            r2 - d, g2 - d, b2 - d,
        });
    }

    static void DecodeH(ulong b, Span<byte> block)
    {
        int r1 = Bits(b, 62, 4), g1 = Bits(b, 58, 3) << 1 | Bits(b, 52, 1), b1 = Bits(b, 51, 1) << 3 | Bits(b, 49, 3);
        int r2 = Bits(b, 46, 4), g2 = Bits(b, 42, 4), b2 = Bits(b, 38, 4);
        int di = Bits(b, 34, 1) << 2 | Bits(b, 32, 1) << 1 | ((r1 << 8 | g1 << 4 | b1) >= (r2 << 8 | g2 << 4 | b2) ? 1 : 0);
        int d = Distance[di];
        r1 = X4(r1); g1 = X4(g1); b1 = X4(b1); r2 = X4(r2); g2 = X4(g2); b2 = X4(b2);
        Paint(b, block, stackalloc int[]
        {
            r1 + d, g1 + d, b1 + d,
            r1 - d, g1 - d, b1 - d,
            r2 + d, g2 + d, b2 + d,
            r2 - d, g2 - d, b2 - d,
        });
    }

    static void DecodePlanar(ulong b, Span<byte> block)
    {
        int ro = X6(Bits(b, 62, 6)), go = X7(Bits(b, 56, 1) << 6 | Bits(b, 54, 6));
        int bo = X6(Bits(b, 48, 1) << 5 | Bits(b, 44, 2) << 3 | Bits(b, 41, 3));
        int rh = X6(Bits(b, 38, 5) << 1 | Bits(b, 32, 1)), gh = X7(Bits(b, 31, 7)), bh = X6(Bits(b, 24, 6));
        int rv = X6(Bits(b, 18, 6)), gv = X7(Bits(b, 12, 7)), bv = X6(Bits(b, 5, 6));
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                Set(block, x, y,
                    (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2,
                    (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
                    (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
    }

    public static void DecodeAlpha(ulong b, Span<byte> block)
    {
        int baseValue = (int)(b >> 56), mult = (int)(b >> 52 & 15), table = (int)(b >> 48 & 15);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
            {
                int i = x * 4 + y;
                int idx = (int)(b >> (45 - 3 * i) & 7);
                block[(y * 4 + x) * 4 + 3] = Clamp(baseValue + EacMod[table, idx] * mult);
            }
    }
}
