using System.Text;

namespace Pvz2Builder;

/// The older games' LAWNSTRINGS.TXT: UTF-16 text where each string is a line "[KEY]" followed by
/// its text (one or more lines), and a blank line before the next key.
public static class StringsText
{
    public static List<(string Key, string Text)> Parse(byte[] data)
    {
        string text = Decode(data);
        var result = new List<(string, string)>();
        string? key = null;
        var lines = new List<string>();
        void Flush()
        {
            if (key == null) return;
            while (lines.Count > 0 && lines[^1].Length == 0) lines.RemoveAt(lines.Count - 1);
            result.Add((key, string.Join("\n", lines)));
            lines.Clear();
        }
        foreach (var raw in text.Split('\n'))
        {
            string line = raw.TrimEnd('\r');
            if (line.Length > 2 && line[0] == '[' && line[^1] == ']' && IsKey(line.AsSpan(1, line.Length - 2)))
            {
                Flush();
                key = line[1..^1];
            }
            else if (key != null) lines.Add(line);
        }
        Flush();
        return result;
    }

    /// The file with `changes` put in: a key it has gets the new text in its place, a new key goes
    /// at the end. Written the way the game's own file is (UTF-16 with a byte order mark, CRLF).
    public static byte[] Write(byte[] original, IReadOnlyDictionary<string, string> changes)
    {
        var entries = Parse(original);
        var done = new HashSet<string>(StringComparer.Ordinal);
        var sb = new StringBuilder();
        foreach (var (k, v) in entries)
        {
            string t = changes.TryGetValue(k, out var c) ? c : v;
            done.Add(k);
            Append(sb, k, t);
        }
        foreach (var (k, v) in changes)
            if (done.Add(k)) Append(sb, k, v);
        bool utf16 = original.Length < 2 || (original[0] == 0xFF && original[1] == 0xFE);
        var body = (utf16 ? Encoding.Unicode : new UTF8Encoding(false)).GetBytes(sb.ToString());
        return utf16 ? [0xFF, 0xFE, .. body] : body;
    }

    static void Append(StringBuilder sb, string key, string text) =>
        sb.Append('[').Append(key).Append("]\r\n").Append(text.Replace("\r\n", "\n").Replace("\n", "\r\n")).Append("\r\n\r\n");

    static string Decode(byte[] data)
    {
        if (data.Length >= 2 && data[0] == 0xFF && data[1] == 0xFE) return Encoding.Unicode.GetString(data, 2, data.Length - 2);
        if (data.Length >= 2 && data[0] == 0xFE && data[1] == 0xFF) return Encoding.BigEndianUnicode.GetString(data, 2, data.Length - 2);
        if (data.Length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) return Encoding.UTF8.GetString(data, 3, data.Length - 3);
        return Encoding.UTF8.GetString(data);
    }

    static bool IsKey(ReadOnlySpan<char> s)
    {
        foreach (char c in s)
            if (!(char.IsAsciiLetterOrDigit(c) || c is '_' or '.' or '-')) return false;
        return true;
    }
}
