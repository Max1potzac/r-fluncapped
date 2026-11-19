using System.Reflection;
using System.Text.Json;

namespace Pvz2Launcher;

/// The mods that come with the launcher (tools/launcher/mods in the source: its own code injections
/// and a few samples), built into it and written into the shared mods/ folder the first time it
/// starts. A built-in mod that's been deleted stays deleted (mods/.builtin.json remembers which
/// were written); "Restore" writes them all again, over any changes.
public static class BuiltinMods
{
    const string Prefix = "mods/";

    /// Published path in the mod folder -> its bytes, from the launcher's embedded resources.
    static readonly Lazy<Dictionary<string, byte[]>> Files = new(() =>
    {
        var asm = typeof(BuiltinMods).Assembly;
        var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        foreach (var name in asm.GetManifestResourceNames().Where(n => n.StartsWith(Prefix, StringComparison.Ordinal)))
        {
            using var s = asm.GetManifestResourceStream(name)!;
            using var ms = new MemoryStream();
            s.CopyTo(ms);
            files[name[Prefix.Length..].Replace('\\', '/')] = ms.ToArray();
        }
        return files;
    });

    /// The built-in mods' folder names.
    public static IReadOnlySet<string> Folders { get; } = new HashSet<string>(
        Files.Value.Keys.Select(k => k.Split('/')[0]), StringComparer.OrdinalIgnoreCase);

    static string StatePath(string modsDir) => Path.Combine(modsDir, ".builtin.json");

    /// Writes the built-in mods the folder hasn't had yet. Returns the folders written.
    public static List<string> Seed(string modsDir)
    {
        Directory.CreateDirectory(modsDir);
        var written = ReadState(modsDir);
        var added = new List<string>();
        foreach (var folder in Folders.Order(StringComparer.OrdinalIgnoreCase))
        {
            if (written.Contains(folder)) continue;
            if (!Directory.Exists(Path.Combine(modsDir, folder))) Write(modsDir, folder);
            written.Add(folder);
            added.Add(folder);
        }
        if (added.Count > 0) File.WriteAllText(StatePath(modsDir), JsonSerializer.Serialize(written.Order(StringComparer.OrdinalIgnoreCase)));
        return added;
    }

    /// Writes every built-in mod again, as it came with the launcher.
    public static void Restore(string modsDir)
    {
        Directory.CreateDirectory(modsDir);
        foreach (var folder in Folders) Write(modsDir, folder);
        File.WriteAllText(StatePath(modsDir), JsonSerializer.Serialize(Folders.Order(StringComparer.OrdinalIgnoreCase)));
    }

    static void Write(string modsDir, string folder)
    {
        foreach (var (path, bytes) in Files.Value.Where(f => f.Key.StartsWith(folder + "/", StringComparison.OrdinalIgnoreCase)))
        {
            string dest = Path.Combine(modsDir, path.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.WriteAllBytes(dest, bytes);
        }
    }

    static HashSet<string> ReadState(string modsDir)
    {
        try
        {
            if (File.Exists(StatePath(modsDir)))
                return new HashSet<string>(JsonSerializer.Deserialize<List<string>>(File.ReadAllText(StatePath(modsDir))) ?? [], StringComparer.OrdinalIgnoreCase);
        }
        catch (Exception e) when (e is JsonException or IOException) { }
        return new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    }
}
