using System.Text.Encodings.Web;
using System.Text.Json;

namespace Pvz2Conv;

/// RTON / PAM / POPFX to JSON, shared by the file converter and the OBB extractor.
static class Converters
{
    static readonly JsonWriterOptions JsonOptions = new()
    {
        Indented = true,
        // keep game text (accents, CJK, <, >, &) readable instead of \uXXXX escapes
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static string? KindFromSignature(byte[] data)
    {
        if (Rton.LooksLike(data)) return "rton";
        if (Pam.LooksLike(data)) return "pam";
        if (PopFx.LooksLike(data)) return "popfx";
        return null;
    }

    public static string? KindFromExtension(string path) => Path.GetExtension(path).ToLowerInvariant() switch
    {
        ".rton" => "rton",
        ".pam" => "pam",
        ".popfx" => "popfx",
        _ => null,
    };

    /// A JSON file that stands for an RTON (x.json), as opposed to x.pam.json and x.popfx.json.
    public static bool IsRtonJson(string path)
    {
        string name = Path.GetFileName(path).ToLowerInvariant();
        return name.EndsWith(".json") && !name.EndsWith(".pam.json") && !name.EndsWith(".popfx.json") && !name.StartsWith("_");
    }

    // community convention: x.rton -> x.json, x.pam -> x.pam.json, x.popfx -> x.popfx.json
    public static string JsonName(string stem, string kind) => kind switch
    {
        "rton" => stem + ".json",
        "pam" => stem + ".pam.json",
        _ => stem + ".popfx.json",
    };

    public static byte[] ToJson(byte[] data, string kind, List<string> warnings)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, JsonOptions))
        {
            switch (kind)
            {
                case "rton": Rton.ToJson(data, w, warnings); break;
                case "pam": Pam.ToJson(data, w, warnings); break;
                default: PopFx.ToJson(data, w, warnings); break;
            }
        }
        return ms.ToArray();
    }

    // Paths over 260 characters still work on Windows with the \\?\ prefix.
    public static string LongPath(string path) =>
        OperatingSystem.IsWindows() && path.Length >= 248 && !path.StartsWith(@"\\?\") ? @"\\?\" + path : path;
}
