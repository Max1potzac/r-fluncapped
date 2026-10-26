using System.Diagnostics;
using System.Text;
using Pvz2Conv;

// pvz2conv - unpacks PvZ2 .apk and .obb files and converts RTON, PAM, POPFX to JSON and PTX to PNG.
// Drag an .apk, an .obb, files or folders onto the exe.

var inputs = new List<string>();
string? outDir = null;
bool pause = !Console.IsInputRedirected;
bool overwrite = true;
bool deleteOriginal = false;
var obbOptions = new ObbOptions();

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--out" when i + 1 < args.Length: outDir = args[++i]; break;
        case "--no-pause": pause = false; break;
        case "--skip-existing": overwrite = false; break;
        case "--json-only": case "--converted-only": deleteOriginal = true; break;
        case "--only" when i + 1 < args.Length: obbOptions.Only = args[++i]; break;
        case "--list": obbOptions.ListOnly = true; break;
        case "--no-convert": obbOptions.Convert = false; break;
        case "--help": case "-h": case "/?": PrintUsage(); return 0;
        default:
            if (args[i].StartsWith("--")) { Console.Error.WriteLine($"unknown option {args[i]}"); PrintUsage(); return 2; }
            inputs.Add(args[i]);
            break;
    }
}

if (inputs.Count == 0)
{
    PrintUsage();
    Pause();
    return 2;
}

var apkObbs = new List<string>();   // the OBB each APK given expects
var obbsGiven = new List<string>();
var log = new StringBuilder();
void Log(string line) { lock (log) { Console.WriteLine(line); log.AppendLine(line); } }

int converted = 0, failed = 0, skipped = 0;
var sw = Stopwatch.StartNew();
string? logDir = null;

foreach (var input in inputs)
{
    string full = Path.GetFullPath(input);
    if (Directory.Exists(full))
    {
        logDir ??= outDir ?? full;
        Log($"scanning {full}");
        var files = Directory.EnumerateFiles(full, "*", SearchOption.AllDirectories)
            .Where(f => Converters.KindFromExtension(f) != null)
            .OrderBy(f => f, StringComparer.OrdinalIgnoreCase);
        foreach (var f in files)
            ConvertOne(f, outDir == null ? null : Path.Combine(outDir, Path.GetRelativePath(full, Path.GetDirectoryName(f)!)));
    }
    else if (File.Exists(full) && Apk.LooksLike(full))
    {
        Log($"unpacking {full}");
        logDir ??= outDir ?? Path.Combine(Path.GetDirectoryName(full)!, Path.GetFileNameWithoutExtension(full) + "_extracted");
        try
        {
            var apk = new Apk(outDir, Log);
            if (apk.Run(full)) converted++;
            else failed++;
            if (apk.ObbName != null) apkObbs.Add(apk.ObbName);
        }
        catch (Exception e) when (e is FormatException or IOException or UnauthorizedAccessException or InvalidDataException)
        {
            Log($"FAIL {full}: {e.Message}");
            failed++;
        }
    }
    else if (File.Exists(full) && IsObb(full))
    {
        obbOptions.OutDir = outDir;
        obbOptions.ConvertedOnly = deleteOriginal;
        Log($"unpacking {full}");
        obbsGiven.Add(Path.GetFileName(full));
        try
        {
            var (_, groupFailures) = new Obb(obbOptions, Log).Run(full);
            converted++;
            failed += groupFailures;
            logDir ??= outDir ?? Path.Combine(Path.GetDirectoryName(full)!, Path.GetFileNameWithoutExtension(full) + "_extracted");
        }
        catch (Exception e) when (e is FormatException or IOException or UnauthorizedAccessException or InvalidDataException)
        {
            Log($"FAIL {full}: {e.Message}");
            failed++;
            logDir ??= outDir ?? Path.GetDirectoryName(full);
        }
    }
    else if (File.Exists(full))
    {
        logDir ??= outDir ?? Path.GetDirectoryName(full);
        ConvertOne(full, outDir);
    }
    else
    {
        Log($"not found: {input}");
        failed++;
    }
}

// an APK and an OBB from different versions won't run together
if (apkObbs.Count > 0 && obbsGiven.Count > 0 && !obbsGiven.Any(o => apkObbs.Contains(o, StringComparer.OrdinalIgnoreCase)))
    Log($"warning: the APK expects {string.Join(" or ", apkObbs)}, but the OBB given is {string.Join(", ", obbsGiven)}; check they're the same version");
Log($"done in {sw.Elapsed.TotalSeconds:0.0}s: {converted} converted, {skipped} skipped, {failed} failed");
if (logDir != null)
{
    try { File.WriteAllText(Path.Combine(logDir, "_pvz2conv_log.txt"), log.ToString()); }
    catch (Exception e) { Console.WriteLine($"couldn't write the log: {e.Message}"); }
}
Pause();
return failed == 0 ? 0 : 1;

void ConvertOne(string path, string? targetDir)
{
    byte[] data;
    try { data = File.ReadAllBytes(path); }
    catch (Exception e) { Log($"FAIL {path}: {e.Message}"); failed++; return; }

    // Trust the signature over the extension; fall back to the extension
    // only to give a useful message.
    string? kind = Converters.KindFromSignature(data);
    if (kind == null)
    {
        string? byExt = Converters.KindFromExtension(path);
        if (byExt == "rton" && Rton.LooksEncrypted(data))
            Log($"SKIP {path}: encrypted RTON, not supported yet");
        else if (Path.GetExtension(path).Equals(".ptx", StringComparison.OrdinalIgnoreCase))
            Log($"SKIP {path}: a PTX on its own has no size or format; unpack the .obb with pvz2conv to get PNGs");
        else
            Log($"SKIP {path}: not an RTON, PAM, POPFX or OBB file (signature {Hex(data)})");
        skipped++;
        return;
    }

    string dir = targetDir ?? Path.GetDirectoryName(path)!;
    string outName = Converters.JsonName(Path.GetFileNameWithoutExtension(path), kind);
    string outPath = Path.Combine(dir, outName);

    if (!overwrite && File.Exists(outPath)) { skipped++; return; }

    var warnings = new List<string>();
    try
    {
        byte[] json = Converters.ToJson(data, kind, warnings);
        Directory.CreateDirectory(dir);
        File.WriteAllBytes(Converters.LongPath(outPath), json);
        if (deleteOriginal && !string.Equals(Path.GetFullPath(outPath), path, StringComparison.OrdinalIgnoreCase))
            File.Delete(path);
        converted++;
        Log($"ok   {kind,-5} {path} -> {outName}");
        foreach (var warn in warnings) Log($"     warning: {warn}");
    }
    catch (Exception e) when (e is FormatException or IOException or UnauthorizedAccessException or ArgumentException)
    {
        Log($"FAIL {kind,-5} {path}: {e.Message}");
        failed++;
    }
}

static bool IsObb(string path)
{
    string ext = Path.GetExtension(path).ToLowerInvariant();
    if (ext is ".obb" or ".rsb" or ".smf") return true;
    try
    {
        using var fs = File.OpenRead(path);
        Span<byte> head = stackalloc byte[4];
        return fs.Read(head) == 4 && (Rsb.LooksLike(head) || BitConverter.ToUInt32(head) == 0xDEADFED4);
    }
    catch (IOException) { return false; }
}

static string Hex(byte[] data) =>
    data.Length == 0 ? "empty file" : BitConverter.ToString(data, 0, Math.Min(4, data.Length)).Replace('-', ' ');

void Pause()
{
    if (!pause) return;
    Console.WriteLine("press Enter to close");
    Console.ReadLine();
}

static void PrintUsage()
{
    Console.WriteLine("""
        Usage: pvz2conv [options] <.apk, .obb, file or folder> ...
          An .apk gives "<name>_extracted" with lib\arm64-v8a (the libraries rfu
          loads), assets and _apk_manifest.json (version, library hashes).
          An .obb (or .rsb / .rsb.smf) is unpacked into "<name>_extracted" next to it,
          in the game's own folder layout. On the way, textures (PTX) become PNG and
          RTON, PAM and POPFX files become JSON. _obb_manifest.json lists every group.
          Other files and folders: RTON, PAM and POPFX are converted to JSON next to
          each file. Folders are searched recursively for *.rton, *.pam and *.popfx.
          --out <dir>          write everything under <dir> instead
          --converted-only     don't keep the original of a file that converted
                               (OBB: PTX/RTON/PAM/POPFX aren't written; files: the
                               original is deleted). --json-only does the same.
          --only <text>        OBB: only groups whose name contains <text>
          --list               OBB: list the groups, extract nothing
          --no-convert         OBB: extract files as they are, convert nothing
          --skip-existing      don't overwrite JSON files that already exist
          --no-pause           don't wait for Enter at the end (for scripts)
        """);
}
