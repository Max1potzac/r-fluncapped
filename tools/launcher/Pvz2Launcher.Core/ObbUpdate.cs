using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace Pvz2Launcher;

/// Reflourished's automatic OBB updates (1.5.0 and later), done by the launcher because rfu doesn't
/// run the game's Java side, where they live (ObbGate and ObbUpdater). The same steps, with the same
/// files, so the game finds what it expects:
///
/// - The update server's latest.json describes the newest OBB: its time stamp, version, the full
///   OBB and/or a patch from the version before, with sizes and SHA-256s, and "prev", the time stamp
///   of that version before (its description is /prev.json on the same server).
/// - Up to date: obbTimestamp (in the game's OBB folder) holds the newest time stamp, or the OBB's
///   SHA-256 is the newest one's.
/// - Behind: the patches from the OBB's version to the newest are downloaded next to it as
///   main.675.com.ea.game.pvz2_rfl.obb.patch0, .patch1, ... The game applies them itself when it
///   starts (libRFLib's rsbpatch), so rfu's game folder only has to hold them.
/// - An OBB no version on the server matches (too old, or changed by hand): the newest full OBB,
///   then any patches after it. The launcher asks first, since that's a large download.
///
/// The in-game setting (Settings > Automatic OBB Updates, CheckObbUpdates) switches it off, as it
/// does on Android, and so does the launcher's own setting.
public static class ObbUpdate
{
    public const string ReflourishedConfig = "https://rsb.ducnguyen.org/live/latest.json";
    public const int MaxHops = 5;  // how many versions back the walk from the newest goes
    const long SpareSpace = 64L << 20;

    public sealed class Error(string message) : Exception(message);

    /// One version's description, as latest.json and <time>.json give it.
    public sealed record Node(long Time, string Version, string? Full, string? FullSha256, long FullSize,
                              string? Patch, string? PatchSha256, long PatchSize, long? Prev, string Source)
    {
        /// The version before's description: "/<prev>.json" on the same server.
        public string? PrevUrl => Prev is { } p ? new Uri(new Uri(Source), $"/{p}.json").ToString() : null;

        public static Node Parse(string json, string source)
        {
            using var doc = JsonDocument.Parse(json);
            var o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object) throw new Error($"{source} isn't a JSON object");
            string? Str(string k) => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String && v.GetString() is { Length: > 0 } s ? s : null;
            long Long(string k, long fallback) => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out long n) ? n : fallback;
            long prev = Long("prev", 0);
            return new Node(Long("time", 0), Str("version") ?? "?", Str("full"), Str("full_sha256"), Long("full_size", -1),
                            Str("patch"), Str("patch_sha256"), Long("patch_size", -1), prev != 0 ? prev : null, source);
        }
    }

    public abstract record Plan;
    public sealed record UpToDate : Plan;
    /// The patches to get, oldest first.
    public sealed record Patches(IReadOnlyList<Node> Chain) : Plan;
    /// A full OBB to download, then the patches after it, oldest first.
    public sealed record Fresh(Node Base, IReadOnlyList<Node> Chain) : Plan;

    /// The update server for this game, or null for a game without automatic OBB updates.
    public static string? ConfigUrl(GameFolder game) =>
        game.Settings.ObbUpdateUrl is { } url ? (url.Trim() == "" ? null : url.Trim())
        : game.Info?.Package == "com.ea.game.pvz2_rfl" && !game.IsChina ? ReflourishedConfig : null;

    /// The update server an APK's own code knows about (Reflourished 1.5.0+'s ObbUpdater keeps it as
    /// a string in its classes*.dex), or null for an APK that can't fetch its OBB.
    public static string? ConfigUrlInApk(string apk)
    {
        try
        {
            using var zip = System.IO.Compression.ZipFile.OpenRead(apk);
            foreach (var e in zip.Entries)
            {
                if (!e.FullName.StartsWith("classes", StringComparison.Ordinal) || !e.FullName.EndsWith(".dex", StringComparison.Ordinal)) continue;
                using var ms = new MemoryStream();
                using (var s = e.Open()) s.CopyTo(ms);
                if (FindConfigUrl(ms.GetBuffer().AsSpan(0, (int)ms.Length)) is { } url) return url;
            }
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException) { }
        return null;
    }

    /// The first "https://.../latest.json" string in a dex (dex strings end with a NUL).
    public static string? FindConfigUrl(ReadOnlySpan<byte> dex)
    {
        ReadOnlySpan<byte> start = "https://"u8, end = "/latest.json"u8;
        for (int at = 0; at < dex.Length;)
        {
            int i = dex[at..].IndexOf(start);
            if (i < 0) return null;
            i += at;
            int nul = dex[i..].IndexOf((byte)0);
            var str = nul < 0 ? dex[i..] : dex.Slice(i, nul);
            if (str.Length < 300 && str.EndsWith(end)) return System.Text.Encoding.UTF8.GetString(str);
            at = i + start.Length;
        }
        return null;
    }

    /// The folder the game sees as its OBB folder (Android/obb/<package> in rfu's data folder): the
    /// patches and obbTimestamp go there. The OBB itself stays in the game's folder; rfu shows it there.
    public static string StateDir(GameFolder game) =>
        Path.Combine(game.DataDir, "sdcard", "Android", "obb", game.Info?.Package is { Length: > 0 } p ? p : "com.ea.game.pvz2_rfl");

    /// Settings > Automatic OBB Updates in the game (on unless switched off there).
    public static bool GameAllowsUpdates(GameFolder game)
    {
        string prefs = Path.Combine(game.DataDir, "data", "shared_prefs", "PvZ2GameActivity.prefs");
        if (!File.Exists(prefs)) return true;
        foreach (var line in File.ReadLines(prefs))
            if (line.Split('\t') is [_, "CheckObbUpdates", var value]) return value.Trim() != "0";
        return true;
    }

    public static long ReadTimestamp(string stateDir)
    {
        try { return long.Parse(File.ReadAllText(Path.Combine(stateDir, "obbTimestamp")).Trim()); }
        catch (Exception e) when (e is IOException or FormatException or OverflowException or UnauthorizedAccessException) { return 0; }
    }

    public static void WriteTimestamp(string stateDir, long time)
    {
        Directory.CreateDirectory(stateDir);
        File.WriteAllText(Path.Combine(stateDir, "obbTimestamp"), time.ToString());
    }

    /// Forgets which version the OBB is, so the next check looks at the file (a new OBB was put in).
    public static void ForgetTimestamp(GameFolder game)
    {
        try { File.Delete(Path.Combine(StateDir(game), "obbTimestamp")); }
        catch (IOException) { }
    }

    public static string PatchPath(string stateDir, string obbName, int i) => Path.Combine(stateDir, $"{obbName}.patch{i}");

    /// What to do: ObbUpdater.plan. `getJson` fetches a description; `sha256` hashes the OBB.
    public static async Task<(Node Head, Plan Plan)> PlanAsync(string configUrl, string? obb, string stateDir,
        Func<string, Task<string>> getJson, Func<string, Task<string>> sha256, Action<string> log)
    {
        var head = Node.Parse(await getJson(configUrl), configUrl);
        bool have = obb != null && File.Exists(obb);
        if (have && ReadTimestamp(stateDir) == head.Time)
        {
            log($"OBB update: up to date ({head.Version})");
            return (head, new UpToDate());
        }
        string? sha = null;
        if (have)
        {
            log("OBB update: checking the OBB's version...");
            sha = await sha256(obb!);
        }

        var nodes = new List<Node>();
        int full = -1;
        Node? node = head;
        while (node != null && nodes.Count <= MaxHops)
        {
            int i = nodes.Count;
            nodes.Add(node);
            if (sha != null)
            {
                if (node.FullSha256 == null) throw new Error($"version {node.Time} has no full_sha256");
                if (string.Equals(sha, node.FullSha256, StringComparison.OrdinalIgnoreCase))
                {
                    log(i == 0 ? $"OBB update: up to date ({node.Version})" : $"OBB update: the OBB is {node.Version}, {i} behind {head.Version}");
                    return (head, i == 0 ? new UpToDate() : new Patches(Oldest(nodes, i)));
                }
            }
            if (full < 0 && node.Full != null)
            {
                full = i;
                if (sha == null) break;  // nothing to patch from: the newest full OBB will do
            }
            if (node.Patch == null) break;
            node = node.PrevUrl is { } prev ? Node.Parse(await getJson(prev), prev) : null;
        }
        if (full < 0) throw new Error($"no full OBB within {MaxHops} versions of {head.Version}");
        log($"OBB update: {(have ? "the OBB matches no recent version" : "no OBB")}; {nodes[full].Version} in full{(full > 0 ? $" plus {full} patches" : "")}");
        return (head, new Fresh(nodes[full], Oldest(nodes, full)));
    }

    static List<Node> Oldest(List<Node> newestFirst, int count)
    {
        var l = newestFirst.GetRange(0, count);
        l.Reverse();
        return l;
    }

    /// Bytes still to download for a plan.
    public static long DownloadSize(Plan plan) => plan switch
    {
        Patches p => p.Chain.Sum(n => Math.Max(n.PatchSize, 0)),
        Fresh f => Math.Max(f.Base.FullSize, 0) + f.Chain.Sum(n => Math.Max(n.PatchSize, 0)),
        _ => 0,
    };

    /// Does the plan: ObbUpdater.execute. A Fresh plan replaces `obb`; the old one goes to `keepOld`
    /// if that's given.
    public static async Task ExecuteAsync(Node head, Plan plan, string obb, string stateDir, HttpClient http,
        Action<string> log, string? keepOld = null, CancellationToken cancel = default)
    {
        Directory.CreateDirectory(stateDir);
        string name = Path.GetFileName(obb);
        switch (plan)
        {
            case UpToDate:
                if (ReadTimestamp(stateDir) != head.Time) WriteTimestamp(stateDir, head.Time);
                return;
            case Patches p:
                RequireSpace(stateDir, p.Chain.Sum(n => Math.Max(n.PatchSize, 0)));
                CleanStale(stateDir, name, p.Chain.Count);
                await StageAsync(p.Chain, stateDir, name, http, log, cancel);
                break;
            case Fresh f:
                string dir = Path.GetDirectoryName(Path.GetFullPath(obb))!;
                RequireSpace(dir, Math.Max(f.Base.FullSize, 0));
                CleanStale(stateDir, name, f.Chain.Count);
                if (f.Base.Full == null) throw new Error($"version {f.Base.Version} has no full OBB");
                string temp = obb + ".new";
                log($"OBB update: downloading the {f.Base.Version} OBB...");
                await DownloadAsync(http, f.Base.Full, temp, f.Base.FullSize, f.Base.FullSha256, log, cancel);
                if (File.Exists(obb))
                {
                    if (keepOld != null)
                    {
                        Directory.CreateDirectory(keepOld);
                        File.Move(obb, Path.Combine(keepOld, name), true);
                    }
                    else File.Delete(obb);
                }
                File.Move(temp, obb);
                await StageAsync(f.Chain, stateDir, name, http, log, cancel);
                break;
        }
        WriteTimestamp(stateDir, head.Time);
        log(plan is Patches or Fresh { Chain.Count: > 0 }
            ? $"OBB update: ready; the game applies the patches to {head.Version} when it starts"
            : $"OBB update: the OBB is {head.Version}");
    }

    static async Task StageAsync(IReadOnlyList<Node> chain, string stateDir, string name, HttpClient http, Action<string> log, CancellationToken cancel)
    {
        for (int i = 0; i < chain.Count; i++)
        {
            var n = chain[i];
            if (n.Patch == null) throw new Error($"version {n.Version} has no patch");
            log($"OBB update: downloading update {i + 1} of {chain.Count} ({n.Version})...");
            await DownloadAsync(http, n.Patch, PatchPath(stateDir, name, i), n.PatchSize, n.PatchSha256, log, cancel);
        }
    }

    /// Patches and partial downloads left from an earlier plan.
    static void CleanStale(string stateDir, string name, int keep)
    {
        if (!Directory.Exists(stateDir)) return;
        foreach (var f in Directory.EnumerateFiles(stateDir, name + ".*"))
        {
            string rest = Path.GetFileName(f)[(name.Length + 1)..];
            if (rest.EndsWith(".part")) rest = rest[..^5];
            if (rest.StartsWith("patch") && int.TryParse(rest[5..], out int i) && i < keep) continue;
            if (rest is "patch" || rest.StartsWith("patch") || rest == "new")
            {
                try { File.Delete(f); } catch (IOException) { }
            }
        }
    }

    static void RequireSpace(string dir, long need)
    {
        if (need <= 0) return;
        try
        {
            Directory.CreateDirectory(dir);
            long have = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir))!).AvailableFreeSpace;
            if (have < need + SpareSpace) throw new Error($"not enough free space for the OBB update: it needs {Size(need + SpareSpace)}, there's {Size(have)}");
        }
        catch (Exception e) when (e is ArgumentException or IOException or UnauthorizedAccessException) { }
    }

    public static string Size(long bytes) =>
        bytes >= 1L << 30 ? $"{bytes / (double)(1L << 30):0.00} GB" : bytes >= 1L << 20 ? $"{bytes / (double)(1L << 20):0.0} MB" : $"{bytes / 1024.0:0} KB";

    public static async Task<string> Sha256Async(string path, CancellationToken cancel = default)
    {
        await using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, true);
        return Convert.ToHexString(await SHA256.HashDataAsync(f, cancel)).ToLowerInvariant();
    }

    public static async Task<string> GetJsonAsync(HttpClient http, string url, CancellationToken cancel = default)
    {
        RequireHttps(url);
        using var response = await http.GetAsync(url, cancel);
        if (!response.IsSuccessStatusCode) throw new Error($"the update server said HTTP {(int)response.StatusCode} for {url}");
        return await response.Content.ReadAsStringAsync(cancel);
    }

    static void RequireHttps(string url)
    {
        if (!url.StartsWith("https://", StringComparison.OrdinalIgnoreCase)) throw new Error($"refusing non-https URL: {url}");
    }

    /// Downloads to `dest`, resuming a .part left by an earlier try, and checks the size and SHA-256.
    /// A file already there with the right SHA-256 is kept. Five tries when the connection drops.
    public static async Task DownloadAsync(HttpClient http, string url, string dest, long size, string? sha256,
        Action<string> log, CancellationToken cancel = default)
    {
        RequireHttps(url);
        if (File.Exists(dest) && (size < 0 || new FileInfo(dest).Length == size) &&
            (sha256 == null || string.Equals(await Sha256Async(dest, cancel), sha256, StringComparison.OrdinalIgnoreCase)))
        {
            log($"OBB update: already have {Path.GetFileName(dest)}");
            return;
        }
        string part = dest + ".part";
        for (int attempt = 1; ; attempt++)
        {
            try
            {
                await DownloadOnceAsync(http, url, part, size, log, cancel);
                break;
            }
            catch (Exception e) when (e is HttpRequestException or IOException && attempt < 5 && !cancel.IsCancellationRequested)
            {
                log($"OBB update: connection lost, retrying ({attempt} of 5): {e.Message}");
                await Task.Delay(TimeSpan.FromSeconds(2 * attempt), cancel);
            }
        }
        long got = new FileInfo(part).Length;
        if (size >= 0 && got != size)
        {
            File.Delete(part);
            throw new Error($"short download of {url}: got {got} bytes, want {size}");
        }
        if (sha256 != null && !string.Equals(await Sha256Async(part, cancel), sha256, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(part);
            throw new Error($"the download of {Path.GetFileName(dest)} was damaged (SHA-256 mismatch); try again");
        }
        File.Move(part, dest, true);
    }

    static async Task DownloadOnceAsync(HttpClient http, string url, string part, long size, Action<string> log, CancellationToken cancel)
    {
        long have = File.Exists(part) ? new FileInfo(part).Length : 0;
        if (size >= 0 && have > size) { File.Delete(part); have = 0; }
        if (size >= 0 && have == size) return;
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        if (have > 0) request.Headers.Range = new RangeHeaderValue(have, null);
        using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel);
        if (response.StatusCode == HttpStatusCode.RequestedRangeNotSatisfiable && have > 0)
        {
            File.Delete(part);
            throw new IOException("the server wouldn't resume; starting over");
        }
        if (!response.IsSuccessStatusCode) throw new Error($"the update server said HTTP {(int)response.StatusCode} for {url}");
        bool resumed = have > 0 && response.StatusCode == HttpStatusCode.PartialContent;
        if (!resumed) have = 0;
        long total = size >= 0 ? size : (response.Content.Headers.ContentLength is { } len ? len + have : -1);

        await using var input = await response.Content.ReadAsStreamAsync(cancel);
        await using var output = new FileStream(part, resumed ? FileMode.Append : FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, true);
        var buffer = new byte[1 << 20];
        long done = have;
        int lastShown = total > 0 ? (int)(done * 20 / total) : 0;
        for (int n; (n = await input.ReadAsync(buffer, cancel)) > 0;)
        {
            await output.WriteAsync(buffer.AsMemory(0, n), cancel);
            done += n;
            if (total > 0 && (int)(done * 20 / total) is var step && step > lastShown)
            {
                lastShown = step;
                log($"OBB update: {step * 5}% ({Size(done)} of {Size(total)})");
            }
        }
    }
}
