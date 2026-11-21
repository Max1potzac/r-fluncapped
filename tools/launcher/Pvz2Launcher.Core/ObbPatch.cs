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

    /// A texture a mod adds, as the game reads it: `Format` 0 is RGBA8888, 147 ETC1 + A8.
    public sealed record NewTexture(string Path, byte[] Data, int Width, int Height, int Format);

    /// A resource group a mod adds: its ordinary files (part 0) and its textures (part 1).
    public sealed record NewGroup(string Name, IReadOnlyList<(string Path, byte[] Data)> Files, IReadOnlyList<NewTexture> Textures);

    /// A composite group a mod adds, or one the OBB has that a mod adds groups to: its groups,
    /// each for a resolution (0: any).
    public sealed record NewComposite(string Name, IReadOnlyList<(string Group, int Resolution)> Groups);

    /// One of the OBB's textures, RGBA8888 (format 0), as ReadTexture gives it.
    public sealed record Texture(byte[] Rgba, int Width, int Height);

    /// The OBB's texture at `path` ("ATLASES\UI_SEEDPACKETS_768_00.PTX"), or null (with `why`)
    /// when it has none there or it isn't RGBA8888, the one kind a mod's pictures can go into.
    public static Texture? ReadTexture(string obb, Index index, string path, out string why)
    {
        string key = Key(path);
        why = "";
        if (!index.Files.TryGetValue(key, out int g) || g < 0 || g >= index.GroupCount) { why = "the OBB has no such texture"; return null; }
        using var fs = File.OpenRead(obb);
        var packet = Packet.Load(fs, index, g);
        if (packet.Entries.FirstOrDefault(e => e.Name == key && e.Texture) is not { } e) { why = "the OBB has it, but not as a texture"; return null; }
        int width = (int)U32(packet.Header, e.Pos + 24), height = (int)U32(packet.Header, e.Pos + 28);
        int o = (int)(index.GroupOff + g * index.GroupEntry);
        uint ptx = U32(index.Header, o + 200) + U32(packet.Header, e.Pos + 12), ptxEntry = U32(index.Header, 0x5C);
        uint format = ptxEntry >= 16 && ptx < U32(index.Header, 0x54) ? U32(index.Header, (int)(U32(index.Header, 0x58) + ptx * ptxEntry + 12)) : uint.MaxValue;
        if (format != 0 || e.Size != (long)width * height * 4) { why = $"its format is {format}, not RGBA8888 (0)"; return null; }
        var part1 = packet.Part1();
        if (e.Offset + (long)e.Size > part1.Length) { why = "it runs past its group's data"; return null; }
        return new Texture(part1.AsSpan((int)e.Offset, (int)e.Size).ToArray(), width, height);
    }

    /// The patch that puts `files` (by OBB path) into the OBB, as rfu reads it, puts `textures`
    /// (RGBA8888, by OBB path, each the size the OBB's is) in place of the OBB's, and adds `groups`
    /// and `composites`. Files the OBB doesn't have, and groups it has already, are left out, with
    /// a line in `log`.
    public static byte[] Build(string obb, Index index, IReadOnlyDictionary<string, byte[]> files, List<string> log,
        IReadOnlyList<NewGroup>? groups = null, IReadOnlyList<NewComposite>? composites = null, IReadOnlyDictionary<string, byte[]>? textures = null)
    {
        var byGroup = new SortedDictionary<int, Dictionary<string, byte[]>>();
        var picturesByGroup = new Dictionary<int, Dictionary<string, byte[]>>();
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
        foreach (var (path, data) in textures ?? new Dictionary<string, byte[]>())
        {
            string key = Key(path);
            if (!index.Files.TryGetValue(key, out int g) || g < 0 || g >= index.GroupCount)
            {
                log.Add($"{path}: the OBB has no such texture; left out");
                continue;
            }
            if (!byGroup.ContainsKey(g)) byGroup[g] = new(StringComparer.Ordinal);
            if (!picturesByGroup.TryGetValue(g, out var list)) picturesByGroup[g] = list = new(StringComparer.Ordinal);
            list[key] = data;
        }

        var header = (byte[])index.Header.Clone();
        var touched = new List<(int Offset, int Length)>();  // what changed in the header
        var packets = new List<(long Offset, byte[] Bytes)>();
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
            var pictures = picturesByGroup.GetValueOrDefault(g) ?? new(StringComparer.Ordinal);
            foreach (var k in pictures.Keys.ToList())
                if (!packet.Entries.Any(e => e.Name == k && e.Texture && e.Size == pictures[k].Length))
                {
                    log.Add($"{k}: not a texture of its group the size given; left out");
                    pictures.Remove(k);
                }
            if (replace.Count == 0 && pictures.Count == 0) continue;
            var (bytes, part0Size) = packet.Rebuild(replace, pictures);
            Place(ref end, bytes, packets);

            // the group's entry: where its packet is now, and a copy of the packet's header fields
            int o = (int)(index.GroupOff + g * index.GroupEntry);
            W32(header, o + 128, (uint)packets[^1].Offset);
            W32(header, o + 132, (uint)bytes.Length);
            for (int i = 0; i < 9; i++) W32(header, o + 140 + 4 * i, U32(bytes, 0x10 + 4 * i));
            W32(header, o + 160, part0Size);  // the RSB keeps part 0's size here too
            touched.Add((o + 128, 48));
            // its memory pool: room for the header and part 0
            uint pool = U32(header, o + 136);
            if (pool < index.PoolCount)
            {
                int po = (int)(index.PoolOff + pool * index.PoolEntry);
                uint need = U32(bytes, 0x14) + part0Size;
                if (need > U32(header, po + 128)) { W32(header, po + 128, need); touched.Add((po + 128, 4)); }
            }
            log.Add(pictures.Count == 0 ? $"OBB group {packet.Name}: {replace.Count} file(s) replaced"
                : $"OBB group {packet.Name}: {replace.Count} file(s) replaced, pictures put into {pictures.Count} texture(s)");
        }

        var writes = new List<(long Offset, byte[] Bytes)>();
        if ((groups?.Count ?? 0) + (composites?.Count ?? 0) > 0 && Grow(fs, index, header, groups ?? [], composites ?? [], packets, ref end, log) is { } grown)
            writes.Add((0, grown));
        else
            writes.AddRange(touched.Select(t => ((long)t.Offset, header.AsSpan(t.Offset, t.Length).ToArray())));
        if (packets.Count == 0) return [];
        writes.AddRange(packets);
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

    // a packet after the end of the OBB, at a 4 KB boundary
    static void Place(ref long end, byte[] bytes, List<(long Offset, byte[] Bytes)> packets)
    {
        if (end + bytes.Length > uint.MaxValue) throw new FormatException("the patched OBB would pass 4 GB");
        packets.Add((end, bytes));
        end = (end + bytes.Length + Align - 1) / Align * Align;
    }

    /// The RSB's header with `groups` and `composites` added, or null when none could be. Its
    /// tables are written again one after another; the header grows, so the packets it now
    /// covers move after the end of the OBB.
    static byte[]? Grow(FileStream fs, Index index, byte[] header, IReadOnlyList<NewGroup> groups, IReadOnlyList<NewComposite> composites,
        List<(long Offset, byte[] Bytes)> packets, ref long end, List<string> log)
    {
        var t = Tables.Read(header);
        if (t == null)
        {
            log.Add("the OBB's RSB isn't laid out as this launcher knows (version 4), so mods can't add resource groups to it; left out");
            return null;
        }
        var groupIndex = t.GroupMap.ToDictionary(g => g.Name, g => g.Index, StringComparer.OrdinalIgnoreCase);
        var compositeNames = t.CompositeMap.Select(c => c.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var fileNames = t.Files.Select(f => f.Name).ToHashSet(StringComparer.Ordinal);
        int added = 0;
        foreach (var g in groups)
        {
            if (g.Name.Length is 0 or > 127 || g.Name.Any(c => c > 0x7e || c < 0x20)) { log.Add($"group {g.Name}: not a name the RSB can hold; left out"); continue; }
            if (groupIndex.ContainsKey(g.Name)) { log.Add($"group {g.Name}: the OBB has a group by that name already; left out"); continue; }
            var paths = g.Files.Select(f => Key(f.Path)).Concat(g.Textures.Select(x => Key(x.Path))).ToList();
            if (paths.FirstOrDefault(fileNames.Contains) is { } taken) { log.Add($"group {g.Name}: the OBB has {taken} already; group left out"); continue; }
            if (paths.Distinct().Count() != paths.Count) { log.Add($"group {g.Name}: a file is in it twice; left out"); continue; }
            int gi = t.Groups.Count, pi = t.Pools.Count, ti = t.Ptx.Count;
            var bytes = NewPacket(g);
            Place(ref end, bytes, packets);
            uint p0Size = U32(bytes, 0x20), p1Size = U32(bytes, 0x30);

            var entry = new byte[t.GroupEntry];
            Encoding.ASCII.GetBytes(g.Name).CopyTo(entry, 0);
            W32(entry, 128, (uint)packets[^1].Offset);
            W32(entry, 132, (uint)bytes.Length);
            W32(entry, 136, (uint)pi);
            for (int i = 0; i < 9; i++) W32(entry, 140 + 4 * i, U32(bytes, 0x10 + 4 * i));
            W32(entry, 160, p0Size);
            W32(entry, 196, (uint)g.Textures.Count);
            W32(entry, 200, (uint)ti);
            t.Groups.Add(entry);

            var pool = new byte[t.PoolEntry];
            Encoding.ASCII.GetBytes(g.Name + "_AutoPool").CopyTo(pool, 0);
            W32(pool, 128, U32(bytes, 0x14) + p0Size);
            W32(pool, 132, p1Size);
            W32(pool, 136, 1);
            t.Pools.Add(pool);

            foreach (var x in g.Textures)
            {
                var ptx = new byte[t.PtxEntry];
                W32(ptx, 0, (uint)x.Width);
                W32(ptx, 4, (uint)x.Height);
                W32(ptx, 8, (uint)x.Width * 4);
                W32(ptx, 12, (uint)x.Format);
                t.Ptx.Add(ptx);
            }
            foreach (var p in paths) { t.Files.Add((p, gi)); fileNames.Add(p); }
            t.GroupMap.Add((g.Name.ToUpperInvariant(), gi));
            groupIndex[g.Name] = gi;
            log.Add($"OBB group {g.Name} added: {g.Files.Count} file(s), {g.Textures.Count} texture(s)");
            added++;
        }
        foreach (var c in composites)
        {
            if (c.Name.Length is 0 or > 127 || c.Name.Any(ch => ch > 0x7e || ch < 0x20)) { log.Add($"composite group {c.Name}: not a name the RSB can hold; left out"); continue; }
            if (c.Groups.FirstOrDefault(x => !groupIndex.ContainsKey(x.Group)) is { Group: { } missing }) { log.Add($"composite group {c.Name}: no group {missing}; left out"); continue; }
            if (compositeNames.Contains(c.Name))
            {
                // one the OBB has: the groups it hasn't yet put into its free places
                var row = t.Composites[t.CompositeMap.First(m => string.Equals(m.Name, c.Name, StringComparison.OrdinalIgnoreCase)).Index];
                int count = (int)U32(row, 128 + 64 * 16);
                var have = Enumerable.Range(0, count).Select(i => U32(row, 128 + 16 * i)).ToHashSet();
                var more = c.Groups.Where(x => !have.Contains((uint)groupIndex[x.Group])).ToList();
                if (more.Count == 0) continue;
                if (count + more.Count > 64) { log.Add($"composite group {c.Name}: no room for {more.Count} more group(s); left out"); continue; }
                foreach (var x in more)
                {
                    W32(row, 128 + 16 * count, (uint)groupIndex[x.Group]);
                    W32(row, 132 + 16 * count, (uint)x.Resolution);
                    count++;
                }
                W32(row, 128 + 64 * 16, (uint)count);
                log.Add($"OBB composite group {c.Name}: {string.Join(", ", more.Select(x => x.Group))} added to it");
                added++;
                continue;
            }
            if (c.Groups.Count > 64) { log.Add($"composite group {c.Name}: more than 64 groups; left out"); continue; }
            var entry = new byte[t.CompositeEntry];
            Encoding.ASCII.GetBytes(c.Name).CopyTo(entry, 0);
            for (int i = 0; i < c.Groups.Count; i++)
            {
                W32(entry, 128 + 16 * i, (uint)groupIndex[c.Groups[i].Group]);
                W32(entry, 132 + 16 * i, (uint)c.Groups[i].Resolution);
            }
            W32(entry, 128 + 64 * 16, (uint)c.Groups.Count);
            t.CompositeMap.Add((c.Name.ToUpperInvariant(), t.Composites.Count));
            t.Composites.Add(entry);
            compositeNames.Add(c.Name);
            log.Add($"OBB composite group {c.Name} added: {string.Join(", ", c.Groups.Select(x => x.Group))}");
            added++;
        }
        if (added == 0) return null;

        var grown = t.Write(header);
        uint size = U32(grown, 12);
        // packets the header now covers go after the end of the OBB
        for (int g = 0; g < index.GroupCount; g++)
        {
            var entry = t.Groups[g];
            uint off = U32(entry, 128), len = U32(entry, 132);
            if (off >= size) continue;
            var bytes = new byte[len];
            fs.Position = off;
            fs.ReadExactly(bytes);
            Place(ref end, bytes, packets);
            W32(entry, 128, (uint)packets[^1].Offset);
        }
        return t.Write(header);
    }

    // a group's packet: the name table, part 0 stored, then the textures stored
    static byte[] NewPacket(NewGroup g)
    {
        var part0 = new MemoryStream();
        var part1 = new MemoryStream();
        var names = new List<(string, byte[])>();
        foreach (var (path, data) in g.Files)
        {
            part0.SetLength((part0.Length + Align - 1) / Align * Align);
            part0.Position = part0.Length;
            names.Add((Key(path), Payload(0, (uint)part0.Position, (uint)data.Length)));
            part0.Write(data);
        }
        for (int i = 0; i < g.Textures.Count; i++)
        {
            var x = g.Textures[i];
            part1.SetLength((part1.Length + Align - 1) / Align * Align);
            part1.Position = part1.Length;
            names.Add((Key(x.Path), Payload(1, (uint)part1.Position, (uint)x.Data.Length, (uint)i, 0, 0, (uint)x.Width, (uint)x.Height)));
            part1.Write(x.Data);
        }
        part0.SetLength((part0.Length + Align - 1) / Align * Align);
        part1.SetLength((part1.Length + Align - 1) / Align * Align);
        var trie = Trie.Write(names);
        uint headerSize = (uint)((0x5C + trie.Length + Align - 1) / Align * Align), p0 = (uint)part0.Length, p1 = (uint)part1.Length;
        var bytes = new byte[headerSize + p0 + p1];
        W32(bytes, 0, RsgMagic);
        W32(bytes, 4, 4);
        W32(bytes, 0x14, headerSize);
        W32(bytes, 0x18, headerSize);
        W32(bytes, 0x1C, p0);
        W32(bytes, 0x20, p0);
        W32(bytes, 0x28, headerSize + p0);
        W32(bytes, 0x2C, p1);
        W32(bytes, 0x30, p1);
        W32(bytes, 0x48, (uint)trie.Length);
        W32(bytes, 0x4C, 0x5C);
        trie.CopyTo(bytes, 0x5C);
        part0.ToArray().CopyTo(bytes, (int)headerSize);
        part1.ToArray().CopyTo(bytes, (int)(headerSize + p0));
        return bytes;
    }

    static byte[] Payload(params uint[] values)
    {
        var b = new byte[values.Length * 4];
        for (int i = 0; i < values.Length; i++) W32(b, 4 * i, values[i]);
        return b;
    }

    /// The RSB header's tables, version 4 as the game's packer lays it out: the file list, the
    /// group map, composites, the composite map, groups, pools and textures, one after another.
    sealed class Tables
    {
        public required List<(string Name, int Group)> Files;
        public required List<(string Name, int Index)> GroupMap, CompositeMap;
        public required List<byte[]> Composites, Groups, Pools, Ptx;
        public required int CompositeEntry, GroupEntry, PoolEntry, PtxEntry;

        public static Tables? Read(byte[] h)
        {
            if (U32(h, 4) != 4 || U32(h, 0x60) != 0 || U32(h, 0x64) != 0 || U32(h, 0x68) != 0) return null;
            int ce = (int)U32(h, 0x3C), ge = (int)U32(h, 0x30), pe = (int)U32(h, 0x50), te = (int)U32(h, 0x5C);
            if (ce != 128 + 64 * 16 + 4 || ge != 204 || pe != 152 || te != 16) return null;
            var warnings = new List<string>();
            List<(string, int)> Map(int sizeAt, int offAt, string what)
            {
                var list = new List<(string, int)>();
                Trie.Read(h, (int)U32(h, offAt), (int)U32(h, sizeAt), _ => 4, (n, pos) => list.Add((n, (int)U32(h, pos))), warnings, what);
                return list;
            }
            List<byte[]> Rows(int countAt, int offAt, int entry) =>
                Enumerable.Range(0, (int)U32(h, countAt)).Select(i => h.AsSpan((int)U32(h, offAt) + i * entry, entry).ToArray()).ToList();
            var t = new Tables
            {
                Files = Map(0x10, 0x14, "file list"), GroupMap = Map(0x20, 0x24, "group map"), CompositeMap = Map(0x40, 0x44, "composite map"),
                Composites = Rows(0x34, 0x38, ce), Groups = Rows(0x28, 0x2C, ge), Pools = Rows(0x48, 0x4C, pe), Ptx = Rows(0x54, 0x58, te),
                CompositeEntry = ce, GroupEntry = ge, PoolEntry = pe, PtxEntry = te,
            };
            return warnings.Count == 0 ? t : null;
        }

        /// The header again: `h`'s first 0x70 bytes with the tables' places, then the tables, to
        /// a 4 KB boundary.
        public byte[] Write(byte[] h)
        {
            var output = new MemoryStream();
            output.Write(h, 0, 0x70);
            var head = new byte[0x70];
            h.AsSpan(0, 0x70).CopyTo(head);
            void Table(byte[] data, int offAt, int sizeAt)
            {
                W32(head, offAt, (uint)output.Length);
                W32(head, sizeAt, (uint)data.Length);
                output.Write(data);
            }
            void Rows(List<byte[]> rows, int offAt, int countAt)
            {
                W32(head, offAt, (uint)output.Length);
                W32(head, countAt, (uint)rows.Count);
                foreach (var r in rows) output.Write(r);
            }
            Table(Trie.Write(Files.Select(f => (f.Name, Payload((uint)f.Group)))), 0x14, 0x10);
            Table(Trie.Write(GroupMap.Select(g => (g.Name, Payload((uint)g.Index)))), 0x24, 0x20);
            Rows(Composites, 0x38, 0x34);
            Table(Trie.Write(CompositeMap.Select(c => (c.Name, Payload((uint)c.Index)))), 0x44, 0x40);
            Rows(Groups, 0x2C, 0x28);
            Rows(Pools, 0x4C, 0x48);
            Rows(Ptx, 0x58, 0x54);
            uint size = (uint)((output.Length + Align - 1) / Align * Align);
            W32(head, 0x0C, size);
            W32(head, 0x6C, size);
            output.SetLength(size);
            var bytes = output.ToArray();
            head.CopyTo(bytes, 0);
            return bytes;
        }
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
        public required uint Flags, Part1Size;
        public required List<Entry> Entries;

        /// Part 1 (the textures) unpacked.
        public byte[] Part1() => Part1Raw.Length == 0 ? [] : (Flags & 1) != 0 || Part1Raw.Length != Part1Size ? Inflate(Part1Raw, (int)Part1Size, Name) : Part1Raw[..(int)Part1Size];

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
            uint p1Off = U32(packet, 0x28), p1Packed = U32(packet, 0x2C), p1Size = U32(packet, 0x30);
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
            return new Packet { Name = name, Header = header, Part0 = part0, Part1Raw = part1, Flags = flags, Part1Size = p1Size, Entries = entries };
        }

        /// The packet again with some files replaced: part 0 stored, its files in the order they
        /// were, each at a 4 KB boundary. Textures (`pictures`, each the size it was) replaced too
        /// leave part 1 stored as well. Returns it and part 0's size.
        public (byte[] Bytes, uint Part0Size) Rebuild(IReadOnlyDictionary<string, byte[]> replace, IReadOnlyDictionary<string, byte[]>? pictures = null)
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
            var part1 = Part1Raw;
            if (pictures is { Count: > 0 })
            {
                part1 = Part1();
                foreach (var e in Entries.Where(e => e.Texture && pictures.ContainsKey(e.Name)))
                    pictures[e.Name].CopyTo(part1, (int)e.Offset);
                W32(header, 0x10, U32(header, 0x10) & ~1u);  // part 1 stored
                W32(header, 0x2C, (uint)part1.Length);
                W32(header, 0x30, (uint)part1.Length);
            }
            var bytes = new byte[headerSize + p0Size + part1.Length];
            header.CopyTo(bytes, 0);
            part0.ToArray().CopyTo(bytes, (int)headerSize);
            part1.CopyTo(bytes, (int)(headerSize + p0Size));
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
