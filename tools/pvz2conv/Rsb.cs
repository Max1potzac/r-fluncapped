using System.IO.Compression;
using System.Text;

namespace Pvz2Conv;

/// RSB ("1bsr"), the bundle inside a PvZ2 .obb. Little-endian; a 0x70-byte
/// header of table offsets, then the tables, then one RSG packet per
/// resource group. The layout follows RipeConsole (GPL-3.0) and Sen, checked
/// against a Reflourished OBB; no code was taken from either.
sealed class Rsb
{
    public const uint Magic = 0x72736231; // "1bsr"

    public sealed record Group(int Index, string Name, uint Offset, uint Size, uint Flags, uint Pool, uint TextureCount, uint PtxStart);
    public sealed record Child(uint Group, uint Resolution, string Locale);
    public sealed record Composite(string Name, List<Child> Children);
    public sealed record Texture(uint Width, uint Height, uint Pitch, uint Format, uint AlphaSize, uint Scale);

    public uint Version;
    public uint HeaderSize;
    public readonly List<Group> Groups = new();
    public readonly List<Composite> Composites = new();
    public readonly List<Texture> Textures = new();
    public int GlobalFileCount;

    public static bool LooksLike(ReadOnlySpan<byte> head) => head.Length >= 4 && BitConverter.ToUInt32(head) == Magic;

    /// Reads the header and tables. <paramref name="header"/> must hold at
    /// least the whole header section (its size is at offset 12).
    public static Rsb Parse(byte[] header, List<string> warnings)
    {
        var r = new BinReader(header);
        if (r.U32() != Magic) throw new FormatException("missing RSB signature (1bsr)");
        var rsb = new Rsb { Version = r.U32() };
        r.U32();
        rsb.HeaderSize = r.U32();
        uint globalSize = r.U32(), globalOff = r.U32();
        r.U32(); r.U32();
        uint groupMapSize = r.U32(), groupMapOff = r.U32();
        uint groupCount = r.U32(), groupOff = r.U32(), groupEntry = r.U32();
        uint compCount = r.U32(), compOff = r.U32(), compEntry = r.U32();
        uint compMapSize = r.U32(), compMapOff = r.U32();
        uint poolCount = r.U32(), poolOff = r.U32(), poolEntry = r.U32();
        uint texCount = r.U32(), texOff = r.U32(), texEntry = r.U32();

        if (rsb.Version < 3 || rsb.Version > 4) warnings.Add($"RSB version {rsb.Version} hasn't been tested (4 has)");
        if (rsb.HeaderSize > header.Length) throw new FormatException($"header is {rsb.HeaderSize} bytes but only {header.Length} were read");

        void CheckTable(string what, uint off, uint count, uint entry, uint minEntry)
        {
            if (count == 0) return;
            if (entry < minEntry) throw new FormatException($"{what} entries are {entry} bytes, at least {minEntry} expected");
            if (off + (ulong)count * entry > rsb.HeaderSize) throw new FormatException($"{what} table runs past the header");
        }
        CheckTable("group", groupOff, groupCount, groupEntry, 176);
        CheckTable("composite", compOff, compCount, compEntry, 132);
        CheckTable("texture", texOff, texCount, texEntry, 16);
        CheckTable("pool", poolOff, poolCount, poolEntry, 128);

        // Group names: the descriptor carries one; the group map is the fallback.
        string[] mapNames = new string[groupCount];
        if (groupMapSize > 0)
            Trie.Read(header, (int)groupMapOff, (int)groupMapSize, _ => 4, (name, pos) =>
            {
                uint i = BitConverter.ToUInt32(header, pos);
                if (i < groupCount) mapNames[i] = name;
            }, warnings, "group map");

        for (int i = 0; i < groupCount; i++)
        {
            int o = (int)(groupOff + i * groupEntry);
            string name = CString(header, o, 128);
            if (name.Length == 0) name = mapNames[i] ?? $"group_{i}";
            uint texCountG = 0, ptxStart = 0;
            if (groupEntry >= 204) { texCountG = BitConverter.ToUInt32(header, o + 196); ptxStart = BitConverter.ToUInt32(header, o + 200); }
            uint pool = BitConverter.ToUInt32(header, o + 136);
            // before v3 the texture range sits in the pool entry instead
            if (groupEntry < 204 && pool < poolCount && poolEntry >= 152)
            {
                int p = (int)(poolOff + pool * poolEntry);
                texCountG = BitConverter.ToUInt32(header, p + 144);
                ptxStart = BitConverter.ToUInt32(header, p + 148);
            }
            rsb.Groups.Add(new Group(i, name,
                BitConverter.ToUInt32(header, o + 128), BitConverter.ToUInt32(header, o + 132),
                BitConverter.ToUInt32(header, o + 140), pool, texCountG, ptxStart));
        }

        string[] compNames = new string[compCount];
        if (compMapSize > 0)
            Trie.Read(header, (int)compMapOff, (int)compMapSize, _ => 4, (name, pos) =>
            {
                uint i = BitConverter.ToUInt32(header, pos);
                if (i < compCount) compNames[i] = name;
            }, warnings, "composite map");
        // child entries are 16 bytes in v3+ (group, resolution, locale, reserved), 8 before
        int childSize = rsb.Version >= 3 && compEntry >= 128 + 64 * 16 + 4 ? 16 : 8;
        for (int i = 0; i < compCount; i++)
        {
            int o = (int)(compOff + i * compEntry);
            string name = CString(header, o, 128);
            if (name.Length == 0) name = compNames[i] ?? $"composite_{i}";
            int countPos = o + 128 + 64 * childSize;
            uint n = Math.Min(BitConverter.ToUInt32(header, countPos), 64);
            var children = new List<Child>();
            for (int c = 0; c < n; c++)
            {
                int co = o + 128 + c * childSize;
                string locale = "";
                if (childSize == 16)
                {
                    uint loc = BitConverter.ToUInt32(header, co + 8);
                    // stored as four ASCII bytes, last letter first (e.g. "EN  ")
                    if (loc != 0) locale = Encoding.ASCII.GetString(BitConverter.GetBytes(System.Buffers.Binary.BinaryPrimitives.ReverseEndianness(loc))).Trim('\0', ' ');
                }
                children.Add(new Child(BitConverter.ToUInt32(header, co), BitConverter.ToUInt32(header, co + 4), locale));
            }
            rsb.Composites.Add(new Composite(name, children));
        }

        for (int i = 0; i < texCount; i++)
        {
            int o = (int)(texOff + i * texEntry);
            rsb.Textures.Add(new Texture(
                BitConverter.ToUInt32(header, o), BitConverter.ToUInt32(header, o + 4),
                BitConverter.ToUInt32(header, o + 8), BitConverter.ToUInt32(header, o + 12),
                texEntry >= 20 ? BitConverter.ToUInt32(header, o + 16) : 0,
                texEntry >= 24 ? BitConverter.ToUInt32(header, o + 20) : 0));
        }

        if (globalSize > 0)
        {
            int count = 0;
            Trie.Read(header, (int)globalOff, (int)globalSize, _ => 4, (_, _) => count++, warnings, "file list");
            rsb.GlobalFileCount = count;
        }
        return rsb;
    }

    static string CString(byte[] data, int offset, int max)
    {
        int end = offset;
        while (end < offset + max && data[end] != 0) end++;
        return Encoding.UTF8.GetString(data, offset, end - offset);
    }
}

/// One resource group's packet ("pgsr"): a header, a name table, then
/// part 0 (ordinary files) and part 1 (textures), each optionally compressed.
sealed class Rsg
{
    public const uint Magic = 0x72736770; // "pgsr"

    public sealed record Entry(string Path, bool IsTexture, uint Offset, uint Size, uint TextureIndex, uint Width, uint Height);

    public readonly List<Entry> Entries = new();
    public byte[] Part0 = Array.Empty<byte>(), Part1 = Array.Empty<byte>();
    public string Part0Method = "", Part1Method = "";

    public static Rsg Parse(byte[] packet, List<string> warnings)
    {
        var r = new BinReader(packet);
        if (r.U32() != Magic) throw new FormatException("missing packet signature (pgsr)");
        uint version = r.U32();
        r.U32(); r.U32();
        uint flags = r.U32();
        r.U32(); // header size
        uint p0Off = r.U32(), p0Packed = r.U32(), p0Size = r.U32();
        r.U32();
        uint p1Off = r.U32(), p1Packed = r.U32(), p1Size = r.U32();
        r.Pos = 4 * 18;
        uint mapLen = r.U32(), mapOff = r.U32();
        if (version < 3 || version > 4) warnings.Add($"packet version {version} hasn't been tested (4 has)");

        var rsg = new Rsg();
        if (mapOff + (ulong)mapLen > (ulong)packet.Length) throw new FormatException("name table runs past the packet");
        Trie.Read(packet, (int)mapOff, (int)mapLen, pos => BitConverter.ToUInt32(packet, pos) == 1 ? 4 + 28 : 4 + 8, (name, pos) =>
        {
            uint type = BitConverter.ToUInt32(packet, pos);
            uint off = BitConverter.ToUInt32(packet, pos + 4), size = BitConverter.ToUInt32(packet, pos + 8);
            if (type == 1)
                rsg.Entries.Add(new Entry(name, true, off, size, BitConverter.ToUInt32(packet, pos + 12),
                    BitConverter.ToUInt32(packet, pos + 24), BitConverter.ToUInt32(packet, pos + 28)));
            else
                rsg.Entries.Add(new Entry(name, false, off, size, 0, 0, 0));
        }, warnings, "packet name table");

        // flag bit 2 = part 0 compressed, bit 1 = part 1 compressed (only a hint, see Unpack)
        if (p0Size > 0)
            (rsg.Part0, rsg.Part0Method) = Unpack.Block(Slice(packet, p0Off, p0Packed, "part 0"), (int)p0Size, (flags & 2) != 0);
        if (p1Size > 0)
            (rsg.Part1, rsg.Part1Method) = Unpack.Block(Slice(packet, p1Off, p1Packed, "part 1"), (int)p1Size, (flags & 1) != 0);
        return rsg;
    }

    static ReadOnlySpan<byte> Slice(byte[] packet, uint off, uint len, string what)
    {
        if (off > packet.Length) throw new FormatException($"{what} starts past the end of the packet");
        return packet.AsSpan((int)off, (int)Math.Min(len, (uint)packet.Length - off));
    }
}

/// The name tables in RSB and RSG files are a character trie written depth
/// first: each 4-byte record is one character plus a 24-bit pointer (in
/// units of 4 bytes) to the record holding the alternative character at the
/// same position. A zero character ends a name and is followed by payload.
///
/// Mod repackers write pointers the game never follows (at the end of the
/// table, or back into names already read). Following them blindly is what
/// produced duplicate files under glued-together folder names, so a pointer
/// is only taken when it lands exactly where the alternative must start:
/// right after everything that hangs off the current character.
static class Trie
{
    public static void Read(byte[] buf, int offset, int size, Func<int, int> payloadSize,
        Action<string, int> onName, List<string> warnings, string what)
    {
        if (size <= 0) return;
        if (offset < 0 || offset + (long)size > buf.Length) throw new FormatException($"{what} runs past the data");
        var name = new StringBuilder();
        var bytes = new List<byte>();
        int strays = 0;

        // returns the position just past this subtree
        int Node(int rel, int depth)
        {
            if (depth > 4096) throw new FormatException($"{what} nests too deep");
            while (true)
            {
                if (rel + 4 > size) throw new FormatException($"{what} is cut short at {rel}");
                int p = offset + rel;
                byte c = buf[p];
                int alt = (buf[p + 1] | buf[p + 2] << 8 | buf[p + 3] << 16) * 4;
                int end;
                if (c == 0)
                {
                    int payload = rel + 4;
                    if (payload + 4 > size) throw new FormatException($"{what} payload is cut short at {payload}");
                    onName(Decode(bytes), offset + payload);
                    end = payload + payloadSize(offset + payload);
                }
                else
                {
                    bytes.Add(c);
                    end = Node(rel + 4, depth + 1);
                    bytes.RemoveAt(bytes.Count - 1);
                }
                if (alt == 0 || alt >= size) return end; // none, or the end-of-table pointer some packers write
                if (alt != end) { strays++; return end; }
                rel = alt;
            }
        }

        int last = Node(0, 0);
        if (strays > 0) warnings.Add($"{what}: ignored {strays} pointer(s) that point outside their branch");
        if (last < size && buf.AsSpan(offset + last, size - last).IndexOfAnyExcept((byte)0) >= 0)
            warnings.Add($"{what}: {size - last} bytes after the last name weren't reachable");
    }

    // Names are ASCII in practice; anything else is read as UTF-8, and
    // characters Windows can't put in a file name are replaced later.
    static string Decode(List<byte> bytes) => Encoding.UTF8.GetString(bytes.ToArray());
}

/// Gets at the RSB inside whatever the .obb file actually is.
static class ObbSource
{
    /// Returns a path to a plain RSB: the input itself, or a temporary file
    /// unwrapped from an SMF (zlib) or zip container. tempFile is set when
    /// the caller must delete it.
    public static string Resolve(string path, string tempDir, out string? tempFile, Action<string> log)
    {
        tempFile = null;
        Span<byte> head = stackalloc byte[8];
        using (var fs = File.OpenRead(path)) head = head[..fs.Read(head)];
        uint magic = head.Length >= 4 ? BitConverter.ToUInt32(head) : 0;
        if (magic == Rsb.Magic) return path;

        if (magic == 0xDEADFED4) // SMF: u32 magic, u32 size, zlib stream
        {
            log("unwrapping SMF (zlib) container");
            tempFile = Path.Combine(tempDir, Path.GetFileName(path) + ".rsb.tmp");
            using var fs = File.OpenRead(path);
            fs.Position = 8;
            using var z = new ZLibStream(fs, CompressionMode.Decompress);
            using (var outFs = File.Create(tempFile)) z.CopyTo(outFs);
            return tempFile;
        }

        if (head.Length >= 4 && head[0] == 'P' && head[1] == 'K')
        {
            using var zip = ZipFile.OpenRead(path);
            foreach (var e in zip.Entries.OrderByDescending(e => e.Length))
            {
                using var s = e.Open();
                var sig = new byte[4];
                if (s.ReadAtLeast(sig, 4, throwOnEndOfStream: false) < 4 || BitConverter.ToUInt32(sig) != Rsb.Magic) continue;
                log($"the .obb is a zip; using {e.FullName} inside it");
                tempFile = Path.Combine(tempDir, Path.GetFileName(path) + ".rsb.tmp");
                e.ExtractToFile(tempFile, overwrite: true);
                return tempFile;
            }
            throw new FormatException("zip file with no RSB inside");
        }
        throw new FormatException($"not an RSB/OBB (signature {Convert.ToHexString(head[..Math.Min(4, head.Length)])})");
    }
}
