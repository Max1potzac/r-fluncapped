using System.Collections.Concurrent;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Pvz2Conv;

sealed class ObbOptions
{
    public string? OutDir;
    public string? Only;          // only groups whose name contains this
    public bool ListOnly;
    public bool ConvertedOnly;    // don't write PTX / RTON / PAM / POPFX that converted
    public bool Convert = true;   // PTX -> PNG, RTON/PAM/POPFX -> JSON
}

/// Extracts every resource group of an OBB into the game's own folder
/// layout (ATLASES, IMAGES, PACKAGES, SOUNDBANKS, ...), converting files on
/// the way so the originals never have to touch the disk.
sealed class Obb
{
    readonly Action<string> _log;
    readonly ObbOptions _opt;
    readonly ConcurrentDictionary<string, ulong> _written = new(StringComparer.OrdinalIgnoreCase);
    int _files, _png, _json, _kept, _shared, _failed, _groupsDone, _groupsSkipped, _groupsPastEnd;

    public Obb(ObbOptions opt, Action<string> log) { _opt = opt; _log = log; }

    public (int ok, int failed) Run(string obbPath)
    {
        string outDir = _opt.OutDir ?? Path.Combine(Path.GetDirectoryName(obbPath)!, Path.GetFileNameWithoutExtension(obbPath) + "_extracted");
        Directory.CreateDirectory(outDir);
        string rsbPath = ObbSource.Resolve(obbPath, outDir, out var temp, _log);
        try { return RunRsb(rsbPath, outDir); }
        finally { if (temp != null) File.Delete(temp); }
    }

    (int, int) RunRsb(string rsbPath, string outDir)
    {
        using var fs = new FileStream(rsbPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.RandomAccess);
        var head = new byte[16];
        fs.ReadExactly(head);
        uint headerSize = BitConverter.ToUInt32(head, 12);
        if (headerSize < 0x70 || headerSize > fs.Length) throw new FormatException($"header size {headerSize} doesn't fit a {fs.Length}-byte file");
        var header = new byte[headerSize];
        fs.Position = 0;
        fs.ReadExactly(header);

        var warnings = new List<string>();
        var rsb = Rsb.Parse(header, warnings);
        foreach (var w in warnings) _log($"     warning: {w}");
        _log($"RSB v{rsb.Version}: {rsb.Groups.Count} groups, {rsb.Composites.Count} composites, {rsb.Textures.Count} textures, {rsb.GlobalFileCount} files listed");

        var groups = rsb.Groups.Where(g => _opt.Only == null || g.Name.Contains(_opt.Only, StringComparison.OrdinalIgnoreCase)).ToList();
        var results = new GroupResult?[rsb.Groups.Count];
        bool headerOnly = fs.Length <= headerSize;
        if (headerOnly) _log("the file is only the header: listing groups, nothing to extract");

        if (_opt.ListOnly || headerOnly)
        {
            foreach (var g in groups)
                _log($"     {g.Name}  ({g.Size} bytes, {g.TextureCount} textures){(g.Offset + (long)g.Size > fs.Length ? "  [not in file]" : "")}");
        }
        else
        {
            var gate = new object();
            Parallel.ForEach(groups, new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, Environment.ProcessorCount) }, g =>
            {
                if (g.Size == 0) { Interlocked.Increment(ref _groupsSkipped); return; }
                if (g.Offset + (long)g.Size > fs.Length)
                {
                    Interlocked.Increment(ref _groupsSkipped);
                    Interlocked.Increment(ref _groupsPastEnd);
                    return;
                }
                byte[] packet = new byte[g.Size];
                lock (gate) { fs.Position = g.Offset; fs.ReadExactly(packet); }
                try { results[g.Index] = ExtractGroup(rsb, g, packet, outDir); Interlocked.Increment(ref _groupsDone); }
                catch (Exception e) when (e is FormatException or InvalidDataException or IOException or ArgumentException or UnauthorizedAccessException)
                {
                    Interlocked.Increment(ref _failed);
                    _log($"FAIL group {g.Name}: {e.Message}");
                }
            });
        }

        if (_groupsPastEnd > 0)
            _log($"SKIP {_groupsPastEnd} groups: the file ends before them (an incomplete or cut-short .obb)");
        WriteManifest(Path.Combine(outDir, "_obb_manifest.json"), rsb, results);
        _log($"groups: {_groupsDone} extracted, {_groupsSkipped} skipped, {_failed} failed");
        _log($"files: {_files} written ({_png} PNG, {_json} JSON, {_kept} kept as-is), {_shared} identical copies shared between groups");
        _log($"output: {outDir}");
        return (_groupsDone, _failed);
    }

    sealed record FileResult(string Path, uint Size, string Output, string? Texture);
    sealed record GroupResult(string Part0, string Part1, List<FileResult> Files);

    GroupResult ExtractGroup(Rsb rsb, Rsb.Group g, byte[] packet, string outDir)
    {
        var warnings = new List<string>();
        var rsg = Rsg.Parse(packet, warnings);
        var files = new List<FileResult>();
        foreach (var e in rsg.Entries)
        {
            byte[] part = e.IsTexture ? rsg.Part1 : rsg.Part0;
            if (e.Offset + (long)e.Size > part.Length)
            {
                warnings.Add($"{e.Path}: runs past the end of its part, skipped");
                Interlocked.Increment(ref _failed);
                continue;
            }
            var data = new ReadOnlySpan<byte>(part, (int)e.Offset, (int)e.Size);
            string rel = SafeRelative(e.Path);
            if (e.Path.Split('\\', '/').Any(p => p.Length > MaxName))
                warnings.Add($"{e.Path}: name too long for Windows, written as {rel}");
            string output;
            string? texInfo = null;
            try
            {
                if (e.IsTexture)
                    (output, texInfo) = WriteTexture(rsb, g, e, data, rel, outDir, warnings);
                else
                    output = WriteResident(data, rel, outDir, warnings);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
            {
                // one file that can't be written doesn't stop the rest of its group
                warnings.Add($"{e.Path}: couldn't be written ({ex.Message})");
                Interlocked.Increment(ref _failed);
                continue;
            }
            files.Add(new FileResult(e.Path.Replace('\\', '/'), e.Size, output, texInfo));
        }
        string summary = $"ok   group {g.Name}: {rsg.Entries.Count} files" +
            (rsg.Part0Method != "" ? $", part 0 {rsg.Part0Method}" : "") + (rsg.Part1Method != "" ? $", part 1 {rsg.Part1Method}" : "");
        lock (_log) { _log(summary); foreach (var w in warnings) _log($"     warning: {w}"); }
        return new GroupResult(rsg.Part0Method, rsg.Part1Method, files);
    }

    (string, string?) WriteTexture(Rsb rsb, Rsb.Group g, Rsg.Entry e, ReadOnlySpan<byte> data, string rel, string outDir, List<string> warnings)
    {
        uint index = g.PtxStart + e.TextureIndex;
        Rsb.Texture? t = index < rsb.Textures.Count ? rsb.Textures[(int)index] : null;
        int w = (int)(t?.Width ?? e.Width), h = (int)(t?.Height ?? e.Height);
        string info = t == null ? $"{w}x{h}, no texture entry {index}" : $"{w}x{h} {Ptx.FormatName(t.Format)}";
        if (_opt.Convert && t != null && Ptx.Supported(t.Format))
        {
            try
            {
                byte[] rgba = Ptx.Decode(data, w, h, t.Format, (int)t.Pitch);
                string png = Path.ChangeExtension(rel, ".png");
                Write(outDir, png, Png.Encode(rgba, w, h));
                Interlocked.Increment(ref _png);
                if (!_opt.ConvertedOnly) Write(outDir, rel, data);
                return (png.Replace('\\', '/'), info);
            }
            catch (FormatException ex) { warnings.Add($"{e.Path}: {ex.Message}; kept the PTX"); }
        }
        else if (_opt.Convert)
            warnings.Add($"{e.Path}: {info} can't be converted yet; kept the PTX");
        Write(outDir, rel, data);
        Interlocked.Increment(ref _kept);
        return (rel.Replace('\\', '/'), info);
    }

    string WriteResident(ReadOnlySpan<byte> data, string rel, string outDir, List<string> warnings)
    {
        if (_opt.Convert)
        {
            var bytes = data.ToArray();
            string? kind = Converters.KindFromSignature(bytes);
            if (kind != null)
            {
                var convWarnings = new List<string>();
                try
                {
                    byte[] json = Converters.ToJson(bytes, kind, convWarnings);
                    string jsonRel = Path.Combine(Path.GetDirectoryName(rel) ?? "", Converters.JsonName(Path.GetFileNameWithoutExtension(rel), kind));
                    Write(outDir, jsonRel, json);
                    Interlocked.Increment(ref _json);
                    foreach (var w in convWarnings) warnings.Add($"{rel}: {w}");
                    if (!_opt.ConvertedOnly) Write(outDir, rel, data);
                    return jsonRel.Replace('\\', '/');
                }
                catch (Exception ex) when (ex is FormatException or ArgumentException or InvalidOperationException)
                {
                    warnings.Add($"{rel}: {kind} conversion failed ({ex.Message}); kept the original");
                }
            }
        }
        Write(outDir, rel, data);
        Interlocked.Increment(ref _kept);
        return rel.Replace('\\', '/');
    }

    void Write(string outDir, string rel, ReadOnlySpan<byte> data)
    {
        // The same file can sit in several groups; write it once.
        ulong hash = Hash(data);
        if (!_written.TryAdd(rel, hash))
        {
            if (_written[rel] == hash) { Interlocked.Increment(ref _shared); return; }
            _log($"     warning: {rel} appears in more than one group with different contents; kept the first");
            return;
        }
        string path = Path.Combine(outDir, rel);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(Converters.LongPath(path), data.ToArray());
        Interlocked.Increment(ref _files);
    }

    static ulong Hash(ReadOnlySpan<byte> data)
    {
        ulong h = 14695981039346656037UL ^ (ulong)data.Length;
        foreach (byte b in data) h = (h ^ b) * 1099511628211UL;
        return h;
    }

    // Game paths use backslashes; keep them inside the output folder and
    // replace characters Windows doesn't allow.
    static string SafeRelative(string gamePath)
    {
        var parts = gamePath.Split('\\', '/').Where(p => p.Length > 0 && p != "." && p != "..")
            .Select(p => string.Concat(p.Select(c => c < 32 || "<>:\"|?*".Contains(c) ? '_' : c)).TrimEnd('.', ' '))
            .Where(p => p.Length > 0)
            .Select(Shorten);
        string rel = Path.Combine(parts.ToArray());
        return rel.Length == 0 ? "_unnamed" : rel;
    }

    // Windows allows 255 characters per name. Longer ones keep their start
    // and extension plus a hash of the full name, so they stay unique; the
    // manifest maps them back to the game's name.
    const int MaxName = 120;

    static string Shorten(string name)
    {
        if (name.Length <= MaxName) return name;
        string ext = Path.GetExtension(name);
        if (ext.Length > 16) ext = "";
        string hash = Hash(System.Text.Encoding.UTF8.GetBytes(name)).ToString("x16");
        return name[..(MaxName - ext.Length - 18)] + "~" + hash + ext;
    }

    static void WriteManifest(string path, Rsb rsb, GroupResult?[] results)
    {
        using var fs = File.Create(path);
        using var w = new Utf8JsonWriter(fs, new JsonWriterOptions { Indented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
        w.WriteStartObject();
        w.WriteNumber("rsb_version", rsb.Version);
        w.WriteStartArray("composites");
        foreach (var c in rsb.Composites)
        {
            w.WriteStartObject();
            w.WriteString("name", c.Name);
            w.WriteStartArray("groups");
            foreach (var ch in c.Children)
            {
                w.WriteStartObject();
                w.WriteString("group", ch.Group < rsb.Groups.Count ? rsb.Groups[(int)ch.Group].Name : $"#{ch.Group}");
                if (ch.Resolution != 0) w.WriteNumber("resolution", ch.Resolution);
                if (ch.Locale != "") w.WriteString("locale", ch.Locale);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteEndObject();
        }
        w.WriteEndArray();
        w.WriteStartArray("groups");
        foreach (var g in rsb.Groups)
        {
            var r = results[g.Index];
            w.WriteStartObject();
            w.WriteString("name", g.Name);
            w.WriteNumber("pool", g.Pool);
            w.WriteNumber("flags", g.Flags);
            w.WriteBoolean("extracted", r != null);
            if (r != null)
            {
                if (r.Part0 != "") w.WriteString("part0", r.Part0);
                if (r.Part1 != "") w.WriteString("part1", r.Part1);
                w.WriteStartArray("files");
                foreach (var f in r.Files)
                {
                    w.WriteStartObject();
                    w.WriteString("path", f.Path);
                    w.WriteNumber("size", f.Size);
                    w.WriteString("output", f.Output);
                    if (f.Texture != null) w.WriteString("texture", f.Texture);
                    w.WriteEndObject();
                }
                w.WriteEndArray();
            }
            w.WriteEndObject();
        }
        w.WriteEndArray();
        w.WriteEndObject();
    }
}
