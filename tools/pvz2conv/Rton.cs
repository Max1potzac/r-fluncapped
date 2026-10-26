using System.Text;
using System.Text.Json;

namespace Pvz2Conv;

/// RTON ("RTON" + u32 version 1 ... "DONE"): PopCap's binary JSON.
/// The root object has no 0x85 type byte; every object is key/value pairs
/// ended by 0xFF and every array is 0xFD count ... 0xFE.
static class Rton
{
    public static bool LooksLike(byte[] data) =>
        data.Length >= 8 && data[0] == 'R' && data[1] == 'T' && data[2] == 'O' && data[3] == 'N';

    public static bool LooksEncrypted(byte[] data) =>
        data.Length >= 2 && data[0] == 0x10 && data[1] == 0x00;

    public static void ToJson(byte[] data, Utf8JsonWriter w, List<string> warnings)
    {
        if (!LooksLike(data))
        {
            if (LooksEncrypted(data))
                throw new FormatException("this RTON is encrypted (starts with 10 00); decryption is not supported yet");
            throw new FormatException("missing RTON signature");
        }
        var r = new BinReader(data, 4);
        uint version = r.U32();
        if (version != 1) warnings.Add($"unexpected RTON version {version}, decoding anyway");

        var ctx = new Ctx(r, w);
        ctx.ReadObjectBody();

        if (r.Remaining >= 4 && Encoding.ASCII.GetString(data, r.Pos, 4) == "DONE")
            r.Pos += 4;
        else
            warnings.Add($"no DONE trailer at 0x{r.Pos:X}");
        if (!r.AtEnd)
            warnings.Add($"{r.Remaining} trailing bytes after the RTON data were ignored");
    }

    sealed class Ctx
    {
        readonly BinReader r;
        readonly Utf8JsonWriter w;
        readonly List<string> asciiCache = new();
        readonly List<string> utf8Cache = new();

        public Ctx(BinReader r, Utf8JsonWriter w) { this.r = r; this.w = w; }

        public void ReadObjectBody()
        {
            w.WriteStartObject();
            while (true)
            {
                int at = r.Pos;
                byte t = r.U8();
                if (t == 0xFF) break;
                w.WritePropertyName(ReadKey(t, at));
                ReadValue(r.U8(), r.Pos - 1);
            }
            w.WriteEndObject();
        }

        void ReadArray()
        {
            int at = r.Pos;
            if (r.U8() != 0xFD) throw new FormatException($"array at 0x{at:X} has no 0xFD count marker");
            ulong count = r.VarU64();
            w.WriteStartArray();
            for (ulong i = 0; i < count; i++)
                ReadValue(r.U8(), r.Pos - 1);
            at = r.Pos;
            if (r.U8() != 0xFE) throw new FormatException($"array ending at 0x{at:X} has no 0xFE end marker");
            w.WriteEndArray();
        }

        string ReadKey(byte t, int at) => t switch
        {
            0x81 or 0x90 or 0x91 or 0x82 or 0x92 or 0x93 => ReadString(t),
            0x83 => ReadRtid(),
            _ => throw new FormatException($"type 0x{t:X2} at 0x{at:X} can't be an object key"),
        };

        string ReadString(byte t)
        {
            switch (t)
            {
                case 0x81: return r.Utf8((int)r.VarU64());
                case 0x90: { var s = r.Utf8((int)r.VarU64()); asciiCache.Add(s); return s; }
                case 0x91: return Cached(asciiCache, r.VarU64());
                case 0x82: { r.VarU64(); return r.Utf8((int)r.VarU64()); }
                case 0x92: { r.VarU64(); var s = r.Utf8((int)r.VarU64()); utf8Cache.Add(s); return s; }
                case 0x93: return Cached(utf8Cache, r.VarU64());
                default: throw new InvalidOperationException();
            }
        }

        string Cached(List<string> cache, ulong index)
        {
            if (index >= (ulong)cache.Count)
                throw new FormatException($"string cache index {index} out of range (cache has {cache.Count}) at 0x{r.Pos:X}");
            return cache[(int)index];
        }

        // Utf8 string with char count + byte count, used inside RTIDs.
        string Utf8Counted() { r.VarU64(); return r.Utf8((int)r.VarU64()); }

        string ReadRtid()
        {
            int at = r.Pos;
            byte sub = r.U8();
            switch (sub)
            {
                case 0x00: return "RTID(0)";
                case 0x01:
                {
                    uint b = r.VarU32(), a = r.VarU32(), x = r.U32();
                    return $"RTID({a}.{b}.{x:x8}@)";
                }
                case 0x02:
                {
                    string sheet = Utf8Counted();
                    uint b = r.VarU32(), a = r.VarU32(), x = r.U32();
                    return $"RTID({a}.{b}.{x:x8}@{sheet})";
                }
                case 0x03:
                {
                    string sheet = Utf8Counted();
                    string uid = Utf8Counted();
                    return $"RTID({uid}@{sheet})";
                }
                default: throw new FormatException($"unknown RTID subtype 0x{sub:X2} at 0x{at:X}");
            }
        }

        void ReadValue(byte t, int at)
        {
            switch (t)
            {
                case 0x00: w.WriteBooleanValue(false); break;
                case 0x01: w.WriteBooleanValue(true); break;
                case 0x02: w.WriteNullValue(); break;

                case 0x08: w.WriteNumberValue(r.I8()); break;
                case 0x0A: w.WriteNumberValue(r.U8()); break;
                case 0x10: w.WriteNumberValue(r.I16()); break;
                case 0x12: w.WriteNumberValue(r.U16()); break;
                case 0x20: w.WriteNumberValue(r.I32()); break;
                case 0x26: w.WriteNumberValue(r.U32()); break;
                case 0x40: w.WriteNumberValue(r.I64()); break;
                case 0x46: w.WriteNumberValue(r.U64()); break;

                case 0x22: WriteFloat(r.F32()); break;
                case 0x42: WriteDouble(r.F64()); break;

                case 0x24: case 0x28: w.WriteNumberValue(r.VarU32()); break;
                case 0x25: case 0x29: w.WriteNumberValue(r.ZigZag32()); break;
                case 0x44: case 0x48: w.WriteNumberValue(r.VarU64()); break;
                case 0x45: case 0x49: w.WriteNumberValue(r.ZigZag64()); break;

                // "zero" shorthands
                case 0x09: case 0x0B: case 0x11: case 0x13: case 0x21:
                case 0x27: case 0x41: case 0x47:
                    w.WriteNumberValue(0); break;
                case 0x23: case 0x43:
                    w.WriteNumberValue(0.0); break;

                case 0x81: case 0x82: case 0x90: case 0x91: case 0x92: case 0x93:
                    w.WriteStringValue(ReadString(t)); break;

                case 0x83: w.WriteStringValue(ReadRtid()); break;
                case 0x84: w.WriteStringValue("RTID(0)"); break;

                case 0x85: ReadObjectBody(); break;
                case 0x86: ReadArray(); break;

                case 0x87:
                {
                    // $BINARY("string", int)
                    r.U8();
                    string s = r.Utf8((int)r.VarU64());
                    int n = r.ZigZag32();
                    w.WriteStringValue($"$BINARY(\"{s}\", {n})");
                    break;
                }

                default:
                    throw new FormatException($"unknown RTON value type 0x{t:X2} at 0x{at:X}");
            }
        }

        void WriteFloat(float f)
        {
            if (float.IsFinite(f)) w.WriteRawValue(f.ToString("R", System.Globalization.CultureInfo.InvariantCulture).Replace("E", "e"));
            else w.WriteStringValue(f.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }

        void WriteDouble(double d)
        {
            if (double.IsFinite(d)) w.WriteNumberValue(d);
            else w.WriteStringValue(d.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
    }
}
