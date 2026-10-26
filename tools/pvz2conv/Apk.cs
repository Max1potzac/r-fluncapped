using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Pvz2Conv;

/// Takes from the game's APK what rfu needs: the arm64 native libraries and
/// the assets folder, plus the version from AndroidManifest.xml. The Java
/// side (dex files, res/) isn't used by rfu and is left in the APK.
sealed class Apk
{
    public const string LibDir = "lib/arm64-v8a/";

    // what rfu loads; the rest (crashlytics) is extracted but optional
    static readonly string[] Required = ["libPVZ2.so", "libRFLib.so", "libc++_shared.so", "libNimble.so"];

    readonly Action<string> _log;
    readonly string? _outDir;

    /// main.<version code>.<package>.obb, once Run has read the manifest
    public string? ObbName { get; private set; }

    public Apk(string? outDir, Action<string> log) { _outDir = outDir; _log = log; }

    public static bool LooksLike(string path)
    {
        try
        {
            using var fs = File.OpenRead(path);
            if (fs.ReadByte() != 'P' || fs.ReadByte() != 'K') return false;
            fs.Position = 0;
            using var zip = new ZipArchive(fs, ZipArchiveMode.Read);
            return zip.GetEntry("AndroidManifest.xml") != null || zip.Entries.Any(IsInnerApk);
        }
        catch (InvalidDataException) { return false; }
        catch (IOException) { return false; }
    }

    static bool IsInnerApk(ZipArchiveEntry e) => e.FullName.EndsWith(".apk", StringComparison.OrdinalIgnoreCase);

    /// Returns false when the APK can't run under rfu (no arm64 libraries or
    /// one of the required ones missing).
    public bool Run(string apkPath)
    {
        string outDir = _outDir ?? Path.Combine(Path.GetDirectoryName(apkPath)!, Path.GetFileNameWithoutExtension(apkPath) + "_extracted");
        using var zip = ZipFile.OpenRead(apkPath);

        var manifestEntry = zip.GetEntry("AndroidManifest.xml");
        if (manifestEntry == null)
            throw new FormatException("this is an APK bundle (XAPK / APKS); open it as a zip and drop the base .apk and the .obb instead: " +
                string.Join(", ", zip.Entries.Where(IsInnerApk).Select(e => e.FullName)));

        var info = ReadManifest(manifestEntry);
        _log($"APK {info.Package} version {info.VersionName} (code {info.VersionCode}), Android {info.MinSdk}+");
        if (info.Package != "" && info.VersionCode != "")
        {
            ObbName = $"main.{info.VersionCode}.{info.Package}.obb";
            _log($"     its OBB is {ObbName}");
        }

        var abis = zip.Entries.Where(e => e.FullName.StartsWith("lib/") && e.FullName.Count(c => c == '/') == 2)
            .Select(e => e.FullName.Split('/')[1]).Distinct().Order().ToList();
        if (!abis.Contains("arm64-v8a"))
        {
            _log($"FAIL no arm64-v8a libraries (this APK has {(abis.Count == 0 ? "none" : string.Join(", ", abis))}); rfu needs the 64-bit build");
            WriteManifest(outDir, info, abis, [], []);
            return false;
        }

        var libs = new List<(string Name, long Size, string Sha256)>();
        var assets = new List<(string Path, long Size)>();
        int dex = 0;
        foreach (var e in zip.Entries)
        {
            if (e.FullName.EndsWith('/')) continue;
            if (e.FullName.StartsWith(LibDir) && e.FullName.IndexOf('/', LibDir.Length) < 0)
            {
                string dest = Path.Combine(outDir, "lib", "arm64-v8a", e.Name);
                libs.Add((e.Name, e.Length, Extract(e, dest)));
            }
            else if (e.FullName.StartsWith("assets/"))
            {
                // the Java side's own code isn't needed by rfu
                if (e.FullName.EndsWith(".dex", StringComparison.OrdinalIgnoreCase)) { dex++; continue; }
                Extract(e, Path.Combine(outDir, SafeRelative(e.FullName)));
                assets.Add((e.FullName["assets/".Length..], e.Length));
            }
        }

        foreach (var l in libs) _log($"ok   {LibDir}{l.Name} ({l.Size:N0} bytes)");
        _log($"ok   {assets.Count} files in assets/{(dex > 0 ? $" ({dex} .dex left out)" : "")}");
        var missing = Required.Where(r => !libs.Any(l => l.Name == r)).ToList();
        if (missing.Count > 0) _log($"FAIL rfu needs {string.Join(", ", missing)}, which this APK doesn't have");

        WriteManifest(outDir, info, abis, libs, assets);
        _log($"output: {outDir}");
        _log($"     run the game with: rfu --lib-dir \"{Path.Combine(outDir, "lib", "arm64-v8a")}\"");
        return missing.Count == 0;
    }

    sealed record Info(string Package, string VersionCode, string VersionName, string MinSdk, string TargetSdk);

    static Info ReadManifest(ZipArchiveEntry entry)
    {
        using var s = entry.Open();
        using var ms = new MemoryStream();
        s.CopyTo(ms);
        var elements = Axml.Read(ms.ToArray());
        string Get(string element, string attr) =>
            elements.FirstOrDefault(e => e.Name == element)?.Attributes.GetValueOrDefault(attr) ?? "";
        return new Info(Get("manifest", "package"), Get("manifest", "versionCode"), Get("manifest", "versionName"),
            Get("uses-sdk", "minSdkVersion"), Get("uses-sdk", "targetSdkVersion"));
    }

    // Writes the entry and returns its SHA-256.
    static string Extract(ZipArchiveEntry e, string dest)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
        using var src = e.Open();
        using var dst = File.Create(Converters.LongPath(dest));
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buf = new byte[1 << 16];
        int n;
        while ((n = src.Read(buf)) > 0) { dst.Write(buf, 0, n); sha.AppendData(buf, 0, n); }
        return Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant();
    }

    // zip entry names come from the file: keep them inside the output folder
    static string SafeRelative(string name) =>
        Path.Combine(name.Split('/').Where(p => p is not ("" or "." or "..")).ToArray());

    void WriteManifest(string outDir, Info info, List<string> abis, List<(string Name, long Size, string Sha256)> libs, List<(string Path, long Size)> assets)
    {
        Directory.CreateDirectory(outDir);
        using var fs = File.Create(Path.Combine(outDir, "_apk_manifest.json"));
        string? obbName = ObbName;
        using var w = new Utf8JsonWriter(fs, new JsonWriterOptions { Indented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
        w.WriteStartObject();
        w.WriteString("package", info.Package);
        w.WriteString("version_name", info.VersionName);
        w.WriteString("version_code", info.VersionCode);
        w.WriteString("min_sdk", info.MinSdk);
        w.WriteString("target_sdk", info.TargetSdk);
        w.WriteString("obb", obbName);
        w.WriteStartArray("abis");
        foreach (var a in abis) w.WriteStringValue(a);
        w.WriteEndArray();
        w.WriteStartArray("libraries");
        foreach (var l in libs)
        {
            w.WriteStartObject();
            w.WriteString("name", l.Name);
            w.WriteNumber("size", l.Size);
            w.WriteString("sha256", l.Sha256);
            w.WriteEndObject();
        }
        w.WriteEndArray();
        w.WriteStartArray("assets");
        foreach (var a in assets)
        {
            w.WriteStartObject();
            w.WriteString("path", a.Path);
            w.WriteNumber("size", a.Size);
            w.WriteEndObject();
        }
        w.WriteEndArray();
        w.WriteEndObject();
    }
}
