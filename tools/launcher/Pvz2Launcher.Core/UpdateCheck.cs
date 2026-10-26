using System.IO.Compression;
using System.Net;
using System.Text;
using System.Text.Json;

namespace Pvz2Launcher;

/// Is there a new version of the game? The game's own answer: its CDN content has a
/// ForceUpdateConfigData sheet whose MinVersion is the oldest version the server still wants to
/// see, the one the game itself compares against before telling a phone to update. A MinVersion
/// above the APK's version (the receiver's) is an update. New events, Piñata Parties and Penny's
/// Challenges arrive in the same CDN content without changing MinVersion, so they never count.
public static class UpdateCheck
{
    public enum Outcome { UpToDate, UpdateAvailable, Unknown }

    public sealed record Result(Outcome Outcome, string Message, string? MinVersion = null);

    /// Downloads the CDN archive for the game's version (only when it changed since last time) and
    /// reads MinVersion from it. Falls back to the archive the game itself downloaded last.
    public static async Task<Result> RunAsync(GameFolder game, HttpClient http, CancellationToken cancel = default)
    {
        string? receiver = game.Info?.VersionName;
        if (string.IsNullOrEmpty(receiver)) return new Result(Outcome.Unknown, "the APK's version isn't known yet");
        string? template = game.Settings.CdnUrl;
        if (string.IsNullOrWhiteSpace(template)) return new Result(Outcome.Unknown, "no CDN address set for this game, so no update check");

        string url = template.Replace("{version}", receiver);
        string cacheDir = Path.Combine(game.WorkDir, "cdn-check");
        Directory.CreateDirectory(cacheDir);
        string archive = Path.Combine(cacheDir, "cdn.tar"), etagPath = Path.Combine(cacheDir, "etag.txt");

        string source;
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            if (File.Exists(archive) && File.Exists(etagPath)) request.Headers.TryAddWithoutValidation("If-None-Match", File.ReadAllText(etagPath));
            using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel);
            if (response.StatusCode == HttpStatusCode.NotFound)
                return new Result(Outcome.UpdateAvailable, $"the CDN has no content for version {receiver} any more, which usually means a newer version is out");
            if (response.StatusCode != HttpStatusCode.NotModified)
            {
                response.EnsureSuccessStatusCode();
                string temp = archive + ".part";
                await using (var file = File.Create(temp))
                    await response.Content.CopyToAsync(file, cancel);
                File.Move(temp, archive, true);
                if (response.Headers.ETag != null) File.WriteAllText(etagPath, response.Headers.ETag.ToString());
                else File.Delete(etagPath);
            }
            source = "the CDN";
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException)
        {
            // offline: the archive the game downloaded on its last run, or our own last copy
            string gameCopy = Path.Combine(game.DataDir, "external", "files", "No_Backup", Mods.CdnFolderName(game), "tar.full");
            if (File.Exists(gameCopy)) archive = gameCopy;
            else if (!File.Exists(archive)) return new Result(Outcome.Unknown, $"couldn't reach the CDN ({e.Message})");
            source = "the last downloaded CDN content (the CDN couldn't be reached)";
        }

        string? min;
        try { min = MinVersionFromArchive(archive); }
        catch (Exception e) when (e is IOException or InvalidDataException or FormatException)
        {
            return new Result(Outcome.Unknown, $"couldn't read the CDN content: {e.Message}");
        }
        if (min == null) return new Result(Outcome.Unknown, $"{source} has no ForceUpdateConfigData, so the version can't be compared");
        return CompareVersions(min, receiver) > 0
            ? new Result(Outcome.UpdateAvailable, $"the game wants version {min} or newer; you have {receiver}", min)
            : new Result(Outcome.UpToDate, $"version {receiver} is current (the CDN asks for {min} or newer)", min);
    }

    /// MinVersion from the first ForceUpdateConfigData in a tar (or .tar.gz) of JSON and RTON files.
    public static string? MinVersionFromArchive(string path)
    {
        using var file = File.OpenRead(path);
        Stream s = file;
        int b0 = file.ReadByte(), b1 = file.ReadByte();
        file.Position = 0;
        if (b0 == 0x1F && b1 == 0x8B) s = new GZipStream(file, CompressionMode.Decompress);
        using (s)
        {
            foreach (var (name, data) in Tar.Read(s))
                if (MinVersionFromFile(data) is { } v) return v;
        }
        return null;
    }

    static readonly byte[] Marker = Encoding.ASCII.GetBytes("ForceUpdateConfigData");

    /// MinVersion from one JSON or RTON property sheet, if it has a ForceUpdateConfigData.
    public static string? MinVersionFromFile(byte[] data)
    {
        if (data.AsSpan().IndexOf(Marker) < 0) return null;
        byte[] json = data;
        if (Pvz2Conv.Rton.LooksLike(data))
        {
            using var ms = new MemoryStream();
            using (var w = new Utf8JsonWriter(ms)) Pvz2Conv.Rton.ToJson(data, w, []);
            json = ms.ToArray();
        }
        try
        {
            using var doc = JsonDocument.Parse(json, new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
            return Find(doc.RootElement);
        }
        catch (JsonException) { return null; }
    }

    // { "objclass": "ForceUpdateConfigData", "objdata": { "MinVersion": "1.4.2" } }, wherever it is
    static string? Find(JsonElement e)
    {
        if (e.ValueKind == JsonValueKind.Object)
        {
            if (e.TryGetProperty("objclass", out var cls) && cls.ValueKind == JsonValueKind.String && cls.GetString() == "ForceUpdateConfigData" &&
                e.TryGetProperty("objdata", out var data) && data.ValueKind == JsonValueKind.Object &&
                data.TryGetProperty("MinVersion", out var min) && min.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(min.GetString()))
                return min.GetString()!.Trim();
            foreach (var p in e.EnumerateObject())
                if (Find(p.Value) is { } v) return v;
        }
        else if (e.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in e.EnumerateArray())
                if (Find(item) is { } v) return v;
        }
        return null;
    }

    /// Compares "1.4.2"-style versions number by number (missing numbers are 0, text after a
    /// number is ignored): negative when a is older.
    public static int CompareVersions(string a, string b)
    {
        static int[] Parts(string v) => v.Split('.').Select(p => int.TryParse(new string(p.TakeWhile(char.IsAsciiDigit).ToArray()), out int n) ? n : 0).ToArray();
        int[] x = Parts(a), y = Parts(b);
        for (int i = 0; i < Math.Max(x.Length, y.Length); i++)
        {
            int c = (i < x.Length ? x[i] : 0).CompareTo(i < y.Length ? y[i] : 0);
            if (c != 0) return c;
        }
        return 0;
    }
}

/// Reads the regular files of a tar archive (ustar and GNU long names), one at a time.
static class Tar
{
    public static IEnumerable<(string Name, byte[] Data)> Read(Stream s)
    {
        var header = new byte[512];
        string? longName = null;
        while (ReadExactly(s, header))
        {
            if (header.All(b => b == 0)) yield break;
            string name = CString(header, 0, 100);
            string prefix = CString(header, 345, 155);
            if (prefix.Length > 0 && header[257] == 'u') name = prefix + "/" + name;
            long size = Octal(header, 124, 12);
            char type = (char)header[156];
            if (size > int.MaxValue) throw new InvalidDataException($"tar entry {name} is too big");
            var data = new byte[size];
            if (!ReadExactly(s, data)) throw new InvalidDataException("tar archive cut short");
            long pad = (512 - size % 512) % 512;
            if (pad > 0 && !ReadExactly(s, new byte[pad])) yield break;
            if (type == 'L') { longName = Encoding.UTF8.GetString(data).TrimEnd('\0'); continue; }
            if (longName != null) { name = longName; longName = null; }
            if (type is '0' or '\0') yield return (name, data);
        }
    }

    static bool ReadExactly(Stream s, byte[] buf)
    {
        int got = 0;
        while (got < buf.Length)
        {
            int n = s.Read(buf, got, buf.Length - got);
            if (n == 0) return false;
            got += n;
        }
        return true;
    }

    static string CString(byte[] b, int at, int len)
    {
        int end = Array.IndexOf(b, (byte)0, at, len);
        return Encoding.UTF8.GetString(b, at, (end < 0 ? at + len : end) - at);
    }

    static long Octal(byte[] b, int at, int len)
    {
        if ((b[at] & 0x80) != 0)
        {
            // GNU base-256 for big sizes
            long v = b[at] & 0x7F;
            for (int i = 1; i < len; i++) v = v << 8 | b[at + i];
            return v;
        }
        long r = 0;
        foreach (char c in CString(b, at, len).Trim())
            if (c is >= '0' and <= '7') r = r * 8 + (c - '0');
        return r;
    }
}
