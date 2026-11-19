using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using Pvz2Conv;

namespace Pvz2Launcher;

/// Mods' files put into the OBB without changing it: a patch rfu reads the OBB through
/// (--obb-patch, src/core/file_patch.h). The OBB is an RSB of resource groups, each an RSGP
/// packet of files; the packet of every group that gets a new file is written again (its other
/// files as they were, part 0 stored rather than compressed) after the end of the OBB, and the
/// RSB's entries for the group and its memory pool are changed to match (the game reads a stored
/// part 0 as readily as a zlib or zstd one). Only files the OBB
/// already has can be replaced (their names are in the RSB's file list and the resource
/// manifest), and only ordinary files (part 0): not textures.
///
/// Fonts get the names of the font they replace (Fonts.WithNames), so FONTS.RTON's typefaces,
/// which name a font by its family, find the new one.
public static class ObbPatch
{
    const uint RsbMagic = 0x72736231, RsgMagic = 0x72736770;
    const int Align = 0x1000;

    /// The OBB's files, as the RSB lists them ("PRIMEFONTS\BRIANNETOD.TTF"), and their groups.
    public sealed class Index
    {
        public required byte[] Header;
        public required uint GroupOff, GroupEntry, GroupCount, PoolOff, PoolEntry, PoolCount;
        public required Dictionary<string, int> Files;  // upper case, backslashes
        public long Size;
    }

    /// The OBB's path for a file: upper case, with backslashes.
    public static string Key(string path) => path.Replace('/', '\\').TrimStart('\\').ToUpperInvariant();

    public static Index Read(string obb)
    {
        using var fs = File.OpenRead(obb);
        var head = new byte[0x70];
        fs.ReadExactly(head);
        if (U32(head, 0) != RsbMagic) throw new FormatException("the OBB isn't an RSB (no 1bsr)");
        uint version = U32(head, 4);
        if (version is < 3 or > 4) throw new FormatException($"RSB version {version} isn't known (3 and 4 are)");
        uint size = U32(head, 12);
        if (size < 0x70 || size > fs.Length || size > 256 << 20) throw new FormatException("the RSB's header size is wrong");
        var header = new byte[size];
        head.CopyTo(header, 0);
        fs.ReadExactly(header, 0x70, (int)size - 0x70);
        var index = new Index
        {
            Header = header, Size = fs.Length,
            GroupCount = U32(header, 40), GroupOff = U32(header, 44), GroupEntry = U32(header, 48),
            PoolCount = U32(header, 72), PoolOff = U32(header, 76), PoolEntry = U32(header, 80),
            Files = new Dictionary<string, int>(StringComparer.Ordinal),
        };
        if (index.GroupEntry < 176 || index.PoolEntry < 136) throw new FormatException("the RSB's tables aren't laid out as expected");
        var warnings = new List<string>();
        Trie.Read(header, (int)U32(header, 20), (int)U32(header, 16), _ => 4,
            (name, pos) => index.Files[Key(name)] = (int)U32(header, pos), warnings, "file list");
        return index;
    }

    /// The files the OBB has at these paths (for a font's names), read out of their groups.
    public static Dictionary<string, byte[]> ReadFiles(string obb, Index index, IEnumerable<string> paths)
    {
        var found = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        using var fs = File.OpenRead(obb);
        foreach (var group in paths.Select(Key).Where(index.Files.ContainsKey).GroupBy(p => index.Files[p]))
        {
            var packet = Packet.Load(fs, index, group.Key);
            foreach (var path in group)
                if (packet.Entries.FirstOrDefault(e => e.Name == path) is { } e) found[path] = packet.Part0.AsSpan((int)e.Offset, (int)e.Size).ToArray();
        }
        return found;
    }

    /// The patch that puts `files` (by OBB path) into the OBB, as rfu reads it. Files the OBB
    /// doesn't have are left out, with a line in `log`.
    public static byte[] Build(string obb, Index index, IReadOnlyDictionary<string, byte[]> files, List<string> log)
    {
        var byGroup = new SortedDictionary<int, Dictionary<string, byte[]>>();
        foreach (var (path, data) in files)
        {
            string key = Key(path);
            if (!index.Files.TryGetValue(key, out int g) || g < 0 || g >= index.GroupCount)
            {
                log.Add($"{path}: the OBB has no such file; left out");
                continue;
            }
            if (!byGroup.TryGetValue(g, out var list)) byGroup[g] = list = new(StringComparer.Ordinal);
            list[key] = data;
        }

        var writes = new List<(long Offset, byte[] Bytes)>();
        long end = (index.Size + Align - 1) / Align * Align;
        using var fs = File.OpenRead(obb);
        foreach (var (g, replace) in byGroup)
        {
            var packet = Packet.Load(fs, index, g);
            var missing = replace.Keys.Where(k => !packet.Entries.Any(e => e.Name == k && !e.Texture)).ToList();
            foreach (var k in missing)
            {
                log.Add($"{k}: not an ordinary file of its group (a texture?); left out");
                replace.Remove(k);
            }
            if (replace.Count == 0) continue;
            var (bytes, part0Size) = packet.Rebuild(replace);
            writes.Add((end, bytes));

            // the group's entry: where its packet is now, and a copy of the packet's header fields
            int o = (int)(index.GroupOff + g * index.GroupEntry);
            var entry = index.Header.AsSpan(o, 204 <= index.GroupEntry ? 204 : (int)index.GroupEntry).ToArray();
            W32(entry, 128, (uint)end);
            W32(entry, 132, (uint)bytes.Length);
            for (int i = 0; i < 9; i++) W32(entry, 140 + 4 * i, U32(bytes, 0x10 + 4 * i));
            W32(entry, 160, part0Size);  // the RSB keeps part 0's size here too
            writes.Add((o + 128, entry[128..]));
            // its memory pool: room for the header and part 0
            uint pool = U32(entry, 136);
            if (pool < index.PoolCount)
            {
                int po = (int)(index.PoolOff + pool * index.PoolEntry);
                uint need = U32(bytes, 0x14) + part0Size;
                if (need > U32(index.Header, po + 128)) writes.Add((po + 128, BitConverter.GetBytes(need)));
            }
            log.Add($"OBB group {packet.Name}: {replace.Count} file(s) replaced");
            end = (end + bytes.Length + Align - 1) / Align * Align;
            if (end > uint.MaxValue) throw new FormatException("the patched OBB would pass 4 GB");
        }
        if (writes.Count == 0) return [];
        writes.Sort((a, b) => a.Offset.CompareTo(b.Offset));
        long size = Math.Max(index.Size, writes.Max(w => w.Offset + w.Bytes.Length));

        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        w.Write(Encoding.ASCII.GetBytes("RFUPATCH"));
        w.Write(1u);
        w.Write((uint)writes.Count);
        w.Write((ulong)index.Size);
        w.Write((ulong)size);
        foreach (var (offset, bytes) in writes)
        {
            w.Write((ulong)offset);
            w.Write((ulong)bytes.Length);
            w.Write(bytes);
        }
        w.Flush();
        return ms.ToArray();
    }

    /// Applies a patch to a copy of the OBB (for checking one).
    public static void Apply(string obb, byte[] patch, string output)
    {
        File.Copy(obb, output, true);
        using var fs = new FileStream(output, FileMode.Open, FileAccess.ReadWrite);
        var r = new BinaryReader(new MemoryStream(patch));
        if (Encoding.ASCII.GetString(r.ReadBytes(8)) != "RFUPATCH" || r.ReadUInt32() != 1) throw new FormatException("not a patch");
        uint count = r.ReadUInt32();
        long baseSize = (long)r.ReadUInt64(), size = (long)r.ReadUInt64();
        if (baseSize != fs.Length) throw new FormatException("the patch is for another OBB");
        fs.SetLength(size);
        for (int i = 0; i < count; i++)
        {
            long offset = (long)r.ReadUInt64();
            var bytes = r.ReadBytes((int)r.ReadUInt64());
            fs.Position = offset;
            fs.Write(bytes);
        }
    }

    /// One group's packet: its header (with the name table) and its parts.
    sealed class Packet
    {
        public sealed record Entry(string Name, bool Texture, int Pos, uint Offset, uint Size);
        public required string Name;
        public required byte[] Header;    // the packet's header, name table included
        public required byte[] Part0;     // decompressed
        public required byte[] Part1Raw;  // as stored
        public required List<Entry> Entries;

        public static Packet Load(FileStream fs, Index index, int g)
        {
            int o = (int)(index.GroupOff + g * index.GroupEntry);
            string name = Encoding.UTF8.GetString(index.Header, o, 128).TrimEnd('\0');
            int nul = name.IndexOf('\0');
            if (nul >= 0) name = name[..nul];
            long off = U32(index.Header, o + 128);
            uint len = U32(index.Header, o + 132);
            if (off + len > fs.Length) throw new FormatException($"group {name}'s packet runs past the OBB");
            var packet = new byte[len];
            fs.Position = off;
            fs.ReadExactly(packet);
            if (U32(packet, 0) != RsgMagic) throw new FormatException($"group {name} has no packet (pgsr) where the RSB says");
            uint flags = U32(packet, 0x10), headerSize = U32(packet, 0x14);
            uint p0Off = U32(packet, 0x18), p0Packed = U32(packet, 0x1C), p0Size = U32(packet, 0x20);
            uint p1Off = U32(packet, 0x28), p1Packed = U32(packet, 0x2C);
            uint mapLen = U32(packet, 0x48), mapOff = U32(packet, 0x4C);
            if (headerSize > len || p0Off + (long)p0Packed > len || (p1Packed > 0 && p1Off + (long)p1Packed > len))
                throw new FormatException($"group {name}'s packet is cut short");
            var header = packet[..(int)headerSize];
            var entries = new List<Entry>();
            var warnings = new List<string>();
            Trie.Read(header, (int)mapOff, (int)mapLen, pos => U32(header, pos) == 1 ? 4 + 28 : 4 + 8,
                (n, pos) => entries.Add(new Entry(Key(n), U32(header, pos) == 1, pos, U32(header, pos + 4), U32(header, pos + 8))), warnings, $"group {name}'s name table");
            var raw = packet.AsSpan((int)p0Off, (int)p0Packed);
            byte[] part0 = p0Size == 0 ? [] : (flags & 2) != 0 || p0Packed != p0Size ? Inflate(raw, (int)p0Size, name) : raw[..(int)p0Size].ToArray();
            var part1 = p1Packed > 0 ? packet.AsSpan((int)p1Off, (int)p1Packed).ToArray() : [];
            foreach (var e in entries.Where(e => !e.Texture))
                if (e.Offset + (long)e.Size > part0.Length) throw new FormatException($"{e.Name} runs past its group's data");
            return new Packet { Name = name, Header = header, Part0 = part0, Part1Raw = part1, Entries = entries };
        }

        /// The packet again with some files replaced: part 0 stored, its files in the order they
        /// were, each at a 4 KB boundary. Returns it and part 0's size.
        public (byte[] Bytes, uint Part0Size) Rebuild(IReadOnlyDictionary<string, byte[]> replace)
        {
            var header = (byte[])Header.Clone();
            var part0 = new MemoryStream();
            foreach (var e in Entries.Where(e => !e.Texture).OrderBy(e => e.Offset))
            {
                var data = replace.TryGetValue(e.Name, out var r) ? r : Part0.AsSpan((int)e.Offset, (int)e.Size).ToArray();
                part0.SetLength((part0.Length + Align - 1) / Align * Align);
                part0.Position = part0.Length;
                W32(header, e.Pos + 4, (uint)part0.Position);
                W32(header, e.Pos + 8, (uint)data.Length);
                part0.Write(data);
            }
            part0.SetLength((part0.Length + Align - 1) / Align * Align);
            uint p0Size = (uint)part0.Length, headerSize = (uint)header.Length;
            W32(header, 0x10, U32(header, 0x10) & ~2u);  // part 0 stored
            W32(header, 0x18, headerSize);
            W32(header, 0x1C, p0Size);
            W32(header, 0x20, p0Size);
            W32(header, 0x28, headerSize + p0Size);
            var bytes = new byte[headerSize + p0Size + Part1Raw.Length];
            header.CopyTo(bytes, 0);
            part0.ToArray().CopyTo(bytes, (int)headerSize);
            Part1Raw.CopyTo(bytes, (int)(headerSize + p0Size));
            return (bytes, p0Size);
        }

        static byte[] Inflate(ReadOnlySpan<byte> raw, int size, string group)
        {
            // zlib as the game's own packer writes it, zstd as Reflourished's does
            bool zstd = raw.Length >= 4 && raw[0] == 0x28 && raw[1] == 0xB5 && raw[2] == 0x2F && raw[3] == 0xFD;
            var output = new byte[size];
            var input = new MemoryStream(raw.ToArray());
            using Stream z = zstd ? new ZstdSharp.DecompressionStream(input) : new ZLibStream(input, CompressionMode.Decompress);
            int got = 0;
            while (got < size && z.Read(output, got, size - got) is var n && n > 0) got += n;
            if (got != size) throw new FormatException($"group {group}'s data unpacks to {got} bytes, {size} expected");
            return output;
        }
    }

    static uint U32(byte[] b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(at));
    static void W32(byte[] b, int at, uint v) => BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(at), v);
}

/// TrueType / OpenType fonts: the one change the launcher makes to them.
public static class Fonts
{
    /// `font` with the name table of `namesFrom` (the font it replaces), so the game, which finds
    /// a font by its family name, takes it for that one. Throws FormatException when either isn't
    /// a single sfnt font (a .ttc collection isn't).
    public static byte[] WithNames(byte[] font, byte[] namesFrom)
    {
        var tables = Tables(font);
        var names = Tables(namesFrom);
        if (!names.TryGetValue("name", out var name)) throw new FormatException("the game's font has no name table");
        tables["name"] = name;
        uint version = BinaryPrimitives.ReadUInt32BigEndian(font);
        // the sfnt again: the table directory, then each table at a 4-byte boundary
        var tags = tables.Keys.OrderBy(t => t, StringComparer.Ordinal).ToList();
        int n = tags.Count, dirSize = 12 + 16 * n;
        int total = dirSize + tables.Values.Sum(t => (t.Length + 3) & ~3);
        var output = new byte[total];
        BinaryPrimitives.WriteUInt32BigEndian(output, version);
        int pow = 1, log = 0;
        while (pow * 2 <= n) { pow *= 2; log++; }
        BinaryPrimitives.WriteUInt16BigEndian(output.AsSpan(4), (ushort)n);
        BinaryPrimitives.WriteUInt16BigEndian(output.AsSpan(6), (ushort)(pow * 16));
        BinaryPrimitives.WriteUInt16BigEndian(output.AsSpan(8), (ushort)log);
        BinaryPrimitives.WriteUInt16BigEndian(output.AsSpan(10), (ushort)(n * 16 - pow * 16));
        int at = dirSize, headAt = -1;
        for (int i = 0; i < n; i++)
        {
            var data = tables[tags[i]];
            if (tags[i] == "head")
            {
                data = (byte[])data.Clone();
                if (data.Length >= 12) BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(8), 0);  // checkSumAdjustment, set below
                headAt = at;
            }
            int r = 12 + 16 * i;
            Encoding.ASCII.GetBytes(tags[i]).CopyTo(output, r);
            data.CopyTo(output, at);
            BinaryPrimitives.WriteUInt32BigEndian(output.AsSpan(r + 4), Sum(output.AsSpan(at, (data.Length + 3) & ~3)));
            BinaryPrimitives.WriteUInt32BigEndian(output.AsSpan(r + 8), (uint)at);
            BinaryPrimitives.WriteUInt32BigEndian(output.AsSpan(r + 12), (uint)data.Length);
            at += (data.Length + 3) & ~3;
        }
        if (headAt >= 0 && tables["head"].Length >= 12)
            BinaryPrimitives.WriteUInt32BigEndian(output.AsSpan(headAt + 8), 0xB1B0AFBA - Sum(output));
        return output;
    }

    /// A font's family name (name ID 1), or null.
    public static string? Family(byte[] font)
    {
        try
        {
            if (!Tables(font).TryGetValue("name", out var t) || t.Length < 6) return null;
            int count = BinaryPrimitives.ReadUInt16BigEndian(t.AsSpan(2)), strings = BinaryPrimitives.ReadUInt16BigEndian(t.AsSpan(4));
            string? fallback = null;
            for (int i = 0; i < count && 6 + 12 * i + 12 <= t.Length; i++)
            {
                var r = t.AsSpan(6 + 12 * i);
                int platform = BinaryPrimitives.ReadUInt16BigEndian(r), id = BinaryPrimitives.ReadUInt16BigEndian(r[6..]);
                int len = BinaryPrimitives.ReadUInt16BigEndian(r[8..]), off = BinaryPrimitives.ReadUInt16BigEndian(r[10..]);
                if (id != 1 || strings + off + len > t.Length) continue;
                var s = t.AsSpan(strings + off, len);
                if (platform is 0 or 3) return Encoding.BigEndianUnicode.GetString(s);
                fallback ??= Encoding.Latin1.GetString(s);
            }
            return fallback;
        }
        catch (FormatException) { return null; }
    }

    static Dictionary<string, byte[]> Tables(byte[] font)
    {
        if (font.Length < 12) throw new FormatException("too short for a font");
        uint version = BinaryPrimitives.ReadUInt32BigEndian(font);
        if (version == 0x74746366) throw new FormatException("a font collection (.ttc); one font is needed");
        if (version is not (0x00010000 or 0x4F54544F or 0x74727565)) throw new FormatException("not a TrueType or OpenType font");
        int n = BinaryPrimitives.ReadUInt16BigEndian(font.AsSpan(4));
        if (12 + 16 * n > font.Length) throw new FormatException("the font's table directory is cut short");
        var tables = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        for (int i = 0; i < n; i++)
        {
            var r = font.AsSpan(12 + 16 * i);
            string tag = Encoding.ASCII.GetString(r[..4]);
            uint off = BinaryPrimitives.ReadUInt32BigEndian(r[8..]), len = BinaryPrimitives.ReadUInt32BigEndian(r[12..]);
            if (off + (ulong)len > (ulong)font.Length) throw new FormatException($"the font's {tag} table runs past its end");
            tables[tag] = font.AsSpan((int)off, (int)len).ToArray();
        }
        return tables;
    }

    static uint Sum(ReadOnlySpan<byte> data)
    {
        uint sum = 0;
        int i = 0;
        for (; i + 4 <= data.Length; i += 4) sum += BinaryPrimitives.ReadUInt32BigEndian(data[i..]);
        if (i < data.Length)
        {
            Span<byte> last = stackalloc byte[4];
            last.Clear();
            data[i..].CopyTo(last);
            sum += BinaryPrimitives.ReadUInt32BigEndian(last);
        }
        return sum;
    }
}
