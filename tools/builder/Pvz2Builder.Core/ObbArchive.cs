using Pvz2Conv;

namespace Pvz2Builder;

/// A game's OBB (an RSB of resource groups), read in place: no unpacking. Files are found by
/// their OBB path ("PACKAGES\PLANTTYPES.RTON"); a group's packet is read and decompressed the
/// first time one of its files is asked for, and the last few are kept.
public sealed class ObbArchive : IDisposable
{
    readonly FileStream _fs;
    readonly Rsb _rsb;
    readonly Dictionary<string, int> _files = new(StringComparer.Ordinal);
    readonly LinkedList<(int Group, Rsg Packet)> _recent = new();
    readonly object _lock = new();
    readonly string? _temp;
    const int Kept = 12;

    public string Path { get; }
    public int GroupCount => _rsb.Groups.Count;
    public IReadOnlyCollection<string> Files => _files.Keys;

    ObbArchive(string path, string rsbPath, string? temp)
    {
        Path = path;
        _temp = temp;
        _fs = new FileStream(rsbPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        var head = new byte[16];
        _fs.ReadExactly(head);
        uint size = BitConverter.ToUInt32(head, 12);
        if (size < 0x70 || size > _fs.Length || size > 256 << 20) throw new FormatException("the RSB's header size is wrong");
        var header = new byte[size];
        _fs.Position = 0;
        _fs.ReadExactly(header);
        var warnings = new List<string>();
        _rsb = Rsb.Parse(header, warnings);
        Trie.Read(header, (int)BitConverter.ToUInt32(header, 20), (int)BitConverter.ToUInt32(header, 16), _ => 4,
            (name, pos) => _files[Key(name)] = (int)BitConverter.ToUInt32(header, pos), warnings, "file list");
    }

    /// Opens an .obb: a plain RSB, or one wrapped in a zip or SMF (unwrapped to a temporary file).
    public static ObbArchive Open(string path)
    {
        string dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "pvz2builder");
        Directory.CreateDirectory(dir);
        string rsb = ObbSource.Resolve(path, dir, out var temp, _ => { });
        try { return new ObbArchive(path, rsb, temp); }
        catch { if (temp != null) File.Delete(temp); throw; }
    }

    /// The OBB's spelling of a path: upper case, backslashes.
    public static string Key(string path) => path.Replace('/', '\\').TrimStart('\\').ToUpperInvariant();

    public bool Has(string path) => _files.ContainsKey(Key(path));

    /// The name of the resource group a file is in.
    public string? GroupOf(string path) => _files.TryGetValue(Key(path), out int g) && g < _rsb.Groups.Count ? _rsb.Groups[g].Name : null;

    /// An ordinary file's bytes, or null when the OBB has no such file.
    public byte[]? Read(string path)
    {
        string key = Key(path);
        if (!_files.TryGetValue(key, out int g)) return null;
        var packet = Packet(g);
        var e = packet.Entries.FirstOrDefault(x => Key(x.Path) == key && !x.IsTexture);
        if (e == null || e.Offset + (long)e.Size > packet.Part0.Length) return null;
        return packet.Part0.AsSpan((int)e.Offset, (int)e.Size).ToArray();
    }

    /// A texture (an atlas, "ATLASES\PLANTPEASHOOTER_768_00.PTX") decoded to RGBA, or null when
    /// the OBB has none there or its format can't be decoded (ASTC, PVRTC).
    public Picture? ReadTexture(string path, out string why)
    {
        why = "";
        string key = Key(path);
        if (!_files.TryGetValue(key, out int g)) { why = "the OBB has no such texture"; return null; }
        var packet = Packet(g);
        var e = packet.Entries.FirstOrDefault(x => Key(x.Path) == key && x.IsTexture);
        if (e == null) { why = "the OBB has it, but not as a texture"; return null; }
        var group = _rsb.Groups[g];
        uint index = group.PtxStart + e.TextureIndex;
        if (index >= _rsb.Textures.Count) { why = "it has no texture entry"; return null; }
        var t = _rsb.Textures[(int)index];
        if (!Ptx.Supported(t.Format)) { why = $"its format ({Ptx.FormatName(t.Format)}) can't be shown"; return null; }
        if (e.Offset + (long)e.Size > packet.Part1.Length) { why = "it runs past its group's data"; return null; }
        try
        {
            var rgba = Ptx.Decode(packet.Part1.AsSpan((int)e.Offset, (int)e.Size), (int)t.Width, (int)t.Height, t.Format, (int)t.Pitch);
            return new Picture((int)t.Width, (int)t.Height, rgba);
        }
        catch (FormatException ex) { why = ex.Message; return null; }
    }

    Rsg Packet(int g)
    {
        lock (_lock)
        {
            for (var n = _recent.First; n != null; n = n.Next)
                if (n.Value.Group == g)
                {
                    _recent.Remove(n);
                    _recent.AddFirst(n);
                    return n.Value.Packet;
                }
            var group = _rsb.Groups[g];
            var bytes = new byte[group.Size];
            _fs.Position = group.Offset;
            _fs.ReadExactly(bytes);
            var packet = Rsg.Parse(bytes, []);
            _recent.AddFirst((g, packet));
            while (_recent.Count > Kept) _recent.RemoveLast();
            return packet;
        }
    }

    public void Dispose()
    {
        _fs.Dispose();
        if (_temp != null) try { File.Delete(_temp); } catch (IOException) { }
    }
}
