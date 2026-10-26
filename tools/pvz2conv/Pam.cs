using System.Text.Json;

namespace Pvz2Conv;

/// PAM (PopAnim): u32 magic 0xBAF01954, u32 version (1..6), then the
/// animation header, image list, sprite list and main sprite.
/// Coordinates are stored in 1/20 px; matrices in 16.16 fixed point.
/// Output follows the field names used by community tools (Sen / TwinStar)
/// so existing JSON-based editors can read it.
static class Pam
{
    public const uint Magic = 0xBAF01954;

    public static bool LooksLike(byte[] data) =>
        data.Length >= 8 && BitConverter.ToUInt32(data, 0) == Magic;

    // Reflourished's packer cuts the trailing zero bytes off some PAMs (the
    // last frame's colour alpha or coordinate ends early), so the file is read
    // as if those zeros were still there.
    const int TrimmedZeros = 8;

    public static void ToJson(byte[] data, Utf8JsonWriter w, List<string> warnings)
    {
        var padded = new byte[data.Length + TrimmedZeros];
        data.CopyTo(padded, 0);
        var r = new BinReader(padded);
        if (r.U32() != Magic) throw new FormatException("missing PAM signature (54 19 F0 BA)");
        int version = (int)r.U32();
        if (version < 1 || version > 6)
            throw new FormatException($"PAM version {version} is not supported (1 to 6 are)");

        w.WriteStartObject();
        w.WriteNumber("version", version);
        w.WriteNumber("frame_rate", r.U8());
        w.WriteStartArray("position");
        Num(w, r.I16() / 20.0); Num(w, r.I16() / 20.0);
        w.WriteEndArray();
        w.WriteStartArray("size");
        Num(w, r.U16() / 20.0); Num(w, r.U16() / 20.0);
        w.WriteEndArray();

        int imageCount = r.U16();
        w.WriteStartArray("image");
        for (int i = 0; i < imageCount; i++) ReadImage(r, w, version);
        w.WriteEndArray();

        int spriteCount = r.U16();
        w.WriteStartArray("sprite");
        for (int i = 0; i < spriteCount; i++) ReadSprite(r, w, version);
        w.WriteEndArray();

        w.WritePropertyName("main_sprite");
        if (version <= 3 || r.U8() != 0) ReadSprite(r, w, version);
        else w.WriteNullValue();

        w.WriteEndObject();

        if (r.Pos > data.Length)
            warnings.Add($"file ends {r.Pos - data.Length} zero bytes early (trimmed by the packer), read as zeros");
        // other PAMs are zero-padded to a 4 KB boundary instead; only data is worth a warning
        else if (r.Pos < data.Length && Array.FindIndex(data, r.Pos, b => b != 0) >= 0)
            warnings.Add($"{data.Length - r.Pos} trailing bytes after the animation were ignored");
    }

    static void ReadImage(BinReader r, Utf8JsonWriter w, int version)
    {
        w.WriteStartObject();
        w.WriteString("name", r.String16());
        if (version >= 4)
        {
            w.WriteStartArray("size");
            w.WriteNumberValue(r.I16()); w.WriteNumberValue(r.I16());
            w.WriteEndArray();
        }
        w.WriteStartArray("transform");
        if (version == 1)
        {
            // rotation in 1/1000 rad, then translation
            double angle = r.I16() / 1000.0;
            double cos = Math.Cos(angle), sin = Math.Sin(angle);
            Num(w, cos); Num(w, sin); Num(w, -sin); Num(w, cos);
        }
        else
        {
            // 2x2 matrix in 16.16 fixed point, additionally scaled by 20
            for (int k = 0; k < 4; k++) Num(w, r.I32() / 1310720.0);
        }
        Num(w, r.I16() / 20.0); Num(w, r.I16() / 20.0);
        w.WriteEndArray();
        w.WriteEndObject();
    }

    static void ReadSprite(BinReader r, Utf8JsonWriter w, int version)
    {
        w.WriteStartObject();
        if (version >= 4)
        {
            w.WriteString("name", r.String16());
            if (version >= 6) w.WriteString("description", r.String16());
            Num(w, "frame_rate", r.I32() / 65536.0);
        }
        int frameCount = r.U16();
        w.WriteStartArray("work_area");
        if (version >= 5) { w.WriteNumberValue(r.I16()); w.WriteNumberValue(r.I16()); }
        else { w.WriteNumberValue(0); w.WriteNumberValue(frameCount); }
        w.WriteEndArray();

        w.WriteStartArray("frame");
        for (int i = 0; i < frameCount; i++) ReadFrame(r, w, version);
        w.WriteEndArray();
        w.WriteEndObject();
    }

    // frame flags
    const byte FRemoves = 0x01, FAppends = 0x02, FChanges = 0x04, FLabel = 0x08, FStop = 0x10, FCommands = 0x20;

    static int Count(BinReader r)
    {
        int n = r.U8();
        return n == 255 ? r.U16() : n;
    }

    // Frame layout: flags, then removes, appends, changes, label, (stop), commands.
    static void ReadFrame(BinReader r, Utf8JsonWriter w, int version)
    {
        int at = r.Pos;
        byte flags = r.U8();
        if ((flags & 0xC0) != 0)
            throw new FormatException($"frame at 0x{at:X} has unknown flags 0x{flags:X2}");

        w.WriteStartObject();
        w.WriteStartArray("remove");
        if ((flags & FRemoves) != 0)
        {
            int n = Count(r);
            for (int i = 0; i < n; i++)
            {
                int index = r.U16();
                if (index >= 2047) index = (int)r.U32();
                w.WriteStartObject();
                w.WriteNumber("index", index);
                w.WriteEndObject();
            }
        }
        w.WriteEndArray();

        w.WriteStartArray("append");
        if ((flags & FAppends) != 0)
        {
            int n = Count(r);
            for (int i = 0; i < n; i++) ReadAppend(r, w, version);
        }
        w.WriteEndArray();

        w.WriteStartArray("change");
        if ((flags & FChanges) != 0)
        {
            int n = Count(r);
            for (int i = 0; i < n; i++) ReadChange(r, w);
        }
        w.WriteEndArray();

        w.WritePropertyName("label");
        if ((flags & FLabel) != 0) w.WriteStringValue(r.String16()); else w.WriteNullValue();
        w.WriteBoolean("stop", (flags & FStop) != 0);

        w.WriteStartArray("command");
        if ((flags & FCommands) != 0)
        {
            int n = r.U8();
            for (int i = 0; i < n; i++)
            {
                w.WriteStartArray();
                w.WriteStringValue(r.String16());
                w.WriteStringValue(r.String16());
                w.WriteEndArray();
            }
        }
        w.WriteEndArray();

        w.WriteEndObject();
    }

    static void ReadAppend(BinReader r, Utf8JsonWriter w, int version)
    {
        ushort f = r.U16();
        int index = f & 0x07FF;
        if (index == 0x07FF) index = (int)r.U32();
        int resource = r.U8();
        if (version >= 6 && resource == 255) resource = r.U16();
        // optional fields, in file order
        int preloadFrame = (f & 0x2000) != 0 ? r.I16() : 0;
        string? name = (f & 0x1000) != 0 ? r.String16() : null;
        double timeScale = (f & 0x0800) != 0 ? r.I32() / 65536.0 : 1.0;

        w.WriteStartObject();
        w.WriteNumber("index", index);
        w.WriteString("name", name);
        w.WriteNumber("resource", resource);
        w.WriteBoolean("sprite", (f & 0x8000) != 0);
        w.WriteBoolean("additive", (f & 0x4000) != 0);
        w.WriteNumber("preload_frame", preloadFrame);
        Num(w, "time_scale", timeScale);
        w.WriteEndObject();
    }

    static void ReadChange(BinReader r, Utf8JsonWriter w)
    {
        ushort f = r.U16();
        int index = f & 0x03FF;
        if (index == 0x03FF) index = (int)r.U32();

        w.WriteStartObject();
        w.WriteNumber("index", index);

        w.WriteStartArray("transform");
        if ((f & 0x1000) != 0)
        {
            // full 2x2 matrix, 16.16 fixed point
            for (int k = 0; k < 4; k++) Num(w, r.I32() / 65536.0);
        }
        else if ((f & 0x4000) != 0)
        {
            Num(w, r.I16() / 1000.0); // rotation, radians
        }
        if ((f & 0x0800) != 0) { Num(w, r.I32() / 20.0); Num(w, r.I32() / 20.0); }
        else { Num(w, r.I16() / 20.0); Num(w, r.I16() / 20.0); }
        w.WriteEndArray();

        w.WritePropertyName("source_rectangle");
        if ((f & 0x8000) != 0)
        {
            w.WriteStartArray();
            for (int k = 0; k < 4; k++) Num(w, r.I16() / 20.0);
            w.WriteEndArray();
        }
        else w.WriteNullValue();

        w.WritePropertyName("color");
        if ((f & 0x2000) != 0)
        {
            w.WriteStartArray();
            for (int k = 0; k < 4; k++) Num(w, r.U8() / 255.0);
            w.WriteEndArray();
        }
        else w.WriteNullValue();

        w.WritePropertyName("sprite_frame_number");
        if ((f & 0x0400) != 0) w.WriteNumberValue(r.I16()); else w.WriteNullValue();

        w.WriteEndObject();
    }

    static void Num(Utf8JsonWriter w, double v) => w.WriteNumberValue(Math.Round(v, 6));
    static void Num(Utf8JsonWriter w, string name, double v) => w.WriteNumber(name, Math.Round(v, 6));
}
