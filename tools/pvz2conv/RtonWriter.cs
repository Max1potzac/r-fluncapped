using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Pvz2Conv;

/// JSON back to RTON, the reverse of Rton.ToJson: what pvz2conv writes (and what Sen and other
/// community tools write) goes back to the binary the game reads. Strings are written through
/// the string cache (0x90/0x91 for ASCII, 0x92/0x93 for the rest), "RTID(...)" strings as RTIDs,
/// whole numbers as varints, and decimals as float32 when that loses nothing, else float64.
static class RtonWriter
{
    public static byte[] FromJson(ReadOnlySpan<byte> json)
    {
        using var doc = JsonDocument.Parse(json.ToArray(), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
        if (doc.RootElement.ValueKind != JsonValueKind.Object)
            throw new FormatException("an RTON file's root must be a JSON object");
        var ctx = new Ctx();
        ctx.Out.Write("RTON"u8);
        ctx.Out.Write(BitConverter.GetBytes(1u));
        ctx.ObjectBody(doc.RootElement);
        ctx.Out.Write("DONE"u8);
        return ctx.Out.ToArray();
    }

    sealed class Ctx
    {
        public readonly MemoryStream Out = new();
        readonly Dictionary<string, int> _ascii = new(StringComparer.Ordinal);
        readonly Dictionary<string, int> _utf8 = new(StringComparer.Ordinal);

        public void ObjectBody(JsonElement obj)
        {
            foreach (var p in obj.EnumerateObject())
            {
                if (TryRtid(p.Name, out _)) Rtid(p.Name, key: true);
                else String(p.Name);
                Value(p.Value);
            }
            Out.WriteByte(0xFF);
        }

        void Value(JsonElement v)
        {
            switch (v.ValueKind)
            {
                case JsonValueKind.False: Out.WriteByte(0x00); break;
                case JsonValueKind.True: Out.WriteByte(0x01); break;
                case JsonValueKind.Null: Out.WriteByte(0x02); break;
                case JsonValueKind.Number: Number(v); break;
                case JsonValueKind.String: StringValue(v.GetString()!); break;
                case JsonValueKind.Object: Out.WriteByte(0x85); ObjectBody(v); break;
                case JsonValueKind.Array:
                    Out.WriteByte(0x86);
                    Out.WriteByte(0xFD);
                    VarU(v.GetArrayLength());
                    foreach (var e in v.EnumerateArray()) Value(e);
                    Out.WriteByte(0xFE);
                    break;
                default: throw new FormatException($"can't write a JSON {v.ValueKind} as RTON");
            }
        }

        void Number(JsonElement v)
        {
            string raw = v.GetRawText();
            bool whole = raw.IndexOfAny(['.', 'e', 'E']) < 0;
            if (whole && v.TryGetInt64(out long l))
            {
                if (l == 0) Out.WriteByte(0x21);
                else if (l is > 0 and <= int.MaxValue) { Out.WriteByte(0x24); VarU((ulong)l); }
                else if (l is < 0 and >= int.MinValue) { Out.WriteByte(0x25); VarU(ZigZag(l)); }
                else if (l > 0) { Out.WriteByte(0x44); VarU((ulong)l); }
                else { Out.WriteByte(0x45); VarU(ZigZag(l)); }
                return;
            }
            if (whole && v.TryGetUInt64(out ulong u)) { Out.WriteByte(0x46); Out.Write(BitConverter.GetBytes(u)); return; }
            double d = v.GetDouble();
            if (d == 0) { Out.WriteByte(0x23); return; }
            // float32 when it reads back as the same number (the way Rton.ToJson printed it)
            float f = (float)d;
            if (float.IsFinite(f) && double.Parse(f.ToString("R", CultureInfo.InvariantCulture), CultureInfo.InvariantCulture) == d)
            {
                Out.WriteByte(0x22);
                Out.Write(BitConverter.GetBytes(f));
            }
            else
            {
                Out.WriteByte(0x42);
                Out.Write(BitConverter.GetBytes(d));
            }
        }

        void StringValue(string s)
        {
            if (s == "RTID(0)") { Out.WriteByte(0x84); return; }
            if (TryRtid(s, out _)) { Rtid(s, key: false); return; }
            if (TryBinary(s, out string bin, out int n))
            {
                Out.WriteByte(0x87);
                Out.WriteByte(0x00);
                byte[] b = Encoding.UTF8.GetBytes(bin);
                VarU(b.Length);
                Out.Write(b);
                VarU(ZigZag(n));
                return;
            }
            String(s);
        }

        void String(string s)
        {
            bool ascii = s.All(c => c < 0x80);
            var cache = ascii ? _ascii : _utf8;
            if (cache.TryGetValue(s, out int index))
            {
                Out.WriteByte(ascii ? (byte)0x91 : (byte)0x93);
                VarU(index);
                return;
            }
            cache[s] = cache.Count;
            byte[] b = Encoding.UTF8.GetBytes(s);
            if (ascii) Out.WriteByte(0x90);
            else { Out.WriteByte(0x92); VarU(CharCount(s)); }
            VarU(b.Length);
            Out.Write(b);
        }

        // RTID(0), RTID(uid@Sheet), RTID(a.b.hex@Sheet), RTID(a.b.hex@)
        void Rtid(string s, bool key)
        {
            TryRtid(s, out string inner);
            Out.WriteByte(0x83);
            if (inner == "0") { Out.WriteByte(0x00); return; }
            int at = inner.LastIndexOf('@');
            string id = inner[..at], sheet = inner[(at + 1)..];
            var parts = id.Split('.');
            if (parts.Length == 3 && uint.TryParse(parts[0], out uint a) && uint.TryParse(parts[1], out uint b) &&
                uint.TryParse(parts[2], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint x))
            {
                if (sheet == "") Out.WriteByte(0x01);
                else { Out.WriteByte(0x02); Counted(sheet); }
                VarU(b);
                VarU(a);
                Out.Write(BitConverter.GetBytes(x));
                return;
            }
            Out.WriteByte(0x03);
            Counted(sheet);
            Counted(id);
        }

        void Counted(string s)
        {
            byte[] b = Encoding.UTF8.GetBytes(s);
            VarU(CharCount(s));
            VarU(b.Length);
            Out.Write(b);
        }

        static bool TryRtid(string s, out string inner)
        {
            inner = "";
            if (!s.StartsWith("RTID(", StringComparison.Ordinal) || !s.EndsWith(')')) return false;
            inner = s[5..^1];
            return inner == "0" || inner.Contains('@');
        }

        static bool TryBinary(string s, out string value, out int n)
        {
            value = ""; n = 0;
            const string head = "$BINARY(\"";
            if (!s.StartsWith(head, StringComparison.Ordinal) || !s.EndsWith(')')) return false;
            int quote = s.LastIndexOf("\", ", StringComparison.Ordinal);
            if (quote < head.Length) return false;
            value = s[head.Length..quote];
            return int.TryParse(s[(quote + 3)..^1], NumberStyles.Integer, CultureInfo.InvariantCulture, out n);
        }

        static int CharCount(string s)
        {
            int n = 0;
            foreach (var _ in s.EnumerateRunes()) n++;
            return n;
        }

        static ulong ZigZag(long v) => (ulong)((v << 1) ^ (v >> 63));

        void VarU(long v) => VarU((ulong)v);
        void VarU(ulong v)
        {
            while (v >= 0x80) { Out.WriteByte((byte)(v | 0x80)); v >>= 7; }
            Out.WriteByte((byte)v);
        }
    }
}
