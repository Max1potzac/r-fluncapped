using System.Text.Json;

namespace Pvz2Conv;

/// POPFX ("xfcp" + u32 version 1): compiled PopCap render effect.
/// Layout worked out from COLORIZE_OVERLAY.POPFX; only one sample has been
/// checked, so the raw blocks are always written out in full and only the
/// parts we're sure of get friendly names.
///
///   header: magic, version, 8 x { u32 count, u32 offset, u32 entry_size },
///           u32 string_data_offset
///   block 3 is the string table: entries of { u32 length, u32 type, u32 offset }
///           where offset is relative to string_data_offset and the text is
///           zero-terminated (type 3 marks shader source text).
///   block 8 holds the shaders: { profile, source, ?, ?, entry_point } where
///           profile/source/entry_point are indices into the string table.
static class PopFx
{
    const int BlockCount = 8;
    const int StringBlock = 2;  // zero-based: block_3
    const int ShaderBlock = 7;  // zero-based: block_8

    public static bool LooksLike(byte[] data) =>
        data.Length >= 8 && data[0] == 'x' && data[1] == 'f' && data[2] == 'c' && data[3] == 'p';

    public static void ToJson(byte[] data, Utf8JsonWriter w, List<string> warnings)
    {
        var r = new BinReader(data);
        if (!LooksLike(data)) throw new FormatException("missing POPFX signature (xfcp)");
        r.Pos = 4;
        uint version = r.U32();
        if (version != 1) warnings.Add($"unexpected POPFX version {version}, decoding anyway");

        var blocks = new (int Count, int Offset, int Size)[BlockCount];
        for (int i = 0; i < BlockCount; i++)
        {
            blocks[i] = ((int)r.U32(), (int)r.U32(), (int)r.U32());
            var b = blocks[i];
            if (b.Count < 0 || b.Size < 0 || b.Size % 4 != 0 ||
                (b.Count > 0 && ((long)b.Offset + (long)b.Count * b.Size > data.Length)))
                throw new FormatException($"block {i + 1} descriptor (count {b.Count}, offset 0x{b.Offset:X}, size {b.Size}) doesn't fit the file");
        }
        int stringData = (int)r.U32();

        // string table
        var strings = new List<(int Type, string Text)>();
        {
            var b = blocks[StringBlock];
            var sr = new BinReader(data, b.Offset);
            for (int i = 0; i < b.Count; i++)
            {
                sr.Pos = b.Offset + i * b.Size;
                int length = (int)sr.U32(), type = (int)sr.U32(), offset = (int)sr.U32();
                string text = r.CStringAt(stringData + offset);
                if (System.Text.Encoding.UTF8.GetByteCount(text) != length)
                    warnings.Add($"string {i}: stored length {length} doesn't match the text found");
                strings.Add((type, text));
            }
        }

        string? Str(int index) => index >= 0 && index < strings.Count ? strings[index].Text : null;

        w.WriteStartObject();
        w.WriteNumber("version", version);

        w.WriteStartArray("strings");
        foreach (var (type, text) in strings)
        {
            w.WriteStartObject();
            w.WriteNumber("type", type);
            w.WriteString("text", text);
            w.WriteEndObject();
        }
        w.WriteEndArray();

        // friendly view of the shaders
        w.WriteStartArray("shaders");
        {
            var b = blocks[ShaderBlock];
            if (b.Size == 20)
            {
                for (int i = 0; i < b.Count; i++)
                {
                    var sr = new BinReader(data, b.Offset + i * b.Size);
                    int profile = sr.I32(), source = sr.I32(), u1 = sr.I32(), u2 = sr.I32(), entry = sr.I32();
                    w.WriteStartObject();
                    w.WriteString("profile", Str(profile));
                    w.WriteString("entry_point", Str(entry));
                    w.WriteNumber("unknown_1", u1);
                    w.WriteNumber("unknown_2", u2);
                    w.WriteString("source", Str(source));
                    w.WriteEndObject();
                }
            }
            else if (b.Count > 0)
                warnings.Add($"shader block has entry size {b.Size} (expected 20); only the raw block is written");
        }
        w.WriteEndArray();

        // every block as raw signed 32-bit words, so nothing is lost
        for (int i = 0; i < BlockCount; i++)
        {
            var b = blocks[i];
            w.WriteStartObject($"block_{i + 1}");
            w.WriteNumber("entry_size", b.Size);
            w.WriteStartArray("entries");
            for (int e = 0; e < b.Count; e++)
            {
                var br = new BinReader(data, b.Offset + e * b.Size);
                w.WriteStartArray();
                for (int k = 0; k < b.Size / 4; k++) w.WriteNumberValue(br.I32());
                w.WriteEndArray();
            }
            w.WriteEndArray();
            w.WriteEndObject();
        }

        w.WriteEndObject();
    }
}
