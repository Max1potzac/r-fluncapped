using System.Text;

namespace Pvz2Conv;

/// Android's binary XML (the compiled AndroidManifest.xml in an APK): a chain
/// of chunks (u16 type, u16 header size, u32 size). Only what the manifest
/// needs is read: the string pool, the resource id map and start elements
/// with their attributes.
static class Axml
{
    public sealed record Element(string Name, Dictionary<string, string> Attributes);

    // attribute resource ids, for manifests whose attribute names were stripped
    static readonly Dictionary<uint, string> KnownIds = new()
    {
        [0x0101021b] = "versionCode",
        [0x0101021c] = "versionName",
        [0x0101020c] = "minSdkVersion",
        [0x01010270] = "targetSdkVersion",
        [0x01010003] = "name",
    };

    public static List<Element> Read(byte[] data)
    {
        var r = new BinReader(data);
        if (r.U16() != 0x0003) throw new FormatException("not a binary XML file");
        r.U16();
        uint total = r.U32();
        int end = (int)Math.Min(total, (uint)data.Length);

        string[] strings = [];
        uint[] ids = [];
        var elements = new List<Element>();
        while (r.Pos + 8 <= end)
        {
            int start = r.Pos;
            ushort type = r.U16();
            ushort headerSize = r.U16();
            uint size = r.U32();
            if (size < 8 || start + size > end) throw new FormatException($"chunk at 0x{start:X} has a bad size");
            switch (type)
            {
                case 0x0001: strings = StringPool(data, start); break;
                case 0x0180:
                    ids = new uint[(size - headerSize) / 4];
                    for (int i = 0; i < ids.Length; i++) ids[i] = BitConverter.ToUInt32(data, start + headerSize + 4 * i);
                    break;
                case 0x0102: elements.Add(StartElement(data, start, headerSize, strings, ids)); break;
            }
            r.Pos = start + (int)size;
        }
        return elements;
    }

    static string[] StringPool(byte[] data, int start)
    {
        var r = new BinReader(data, start + 8);
        int count = (int)r.U32();
        r.U32(); // styles
        bool utf8 = (r.U32() & 0x100) != 0;
        int stringsStart = start + (int)r.U32();
        r.U32();
        var result = new string[count];
        for (int i = 0; i < count; i++)
        {
            int p = stringsStart + (int)BitConverter.ToUInt32(data, start + 28 + 4 * i);
            if (utf8)
            {
                Len8(data, ref p); // length in characters
                int bytes = Len8(data, ref p);
                result[i] = Encoding.UTF8.GetString(data, p, bytes);
            }
            else
            {
                int chars = BitConverter.ToUInt16(data, p); p += 2;
                if ((chars & 0x8000) != 0) { chars = ((chars & 0x7FFF) << 16) | BitConverter.ToUInt16(data, p); p += 2; }
                result[i] = Encoding.Unicode.GetString(data, p, chars * 2);
            }
        }
        return result;
    }

    static int Len8(byte[] data, ref int p)
    {
        int n = data[p++];
        if ((n & 0x80) != 0) n = ((n & 0x7F) << 8) | data[p++];
        return n;
    }

    static Element StartElement(byte[] data, int start, int headerSize, string[] strings, uint[] ids)
    {
        var r = new BinReader(data, start + headerSize);
        r.U32(); // namespace
        string name = Str(strings, r.U32());
        int attrStart = r.U16(), attrSize = r.U16(), attrCount = r.U16();
        var attrs = new Dictionary<string, string>();
        for (int i = 0; i < attrCount; i++)
        {
            var a = new BinReader(data, start + headerSize + attrStart + i * attrSize);
            a.U32();
            uint nameIndex = a.U32();
            uint raw = a.U32();
            a.U16(); a.U8();
            byte dataType = a.U8();
            uint value = a.U32();
            string attr = Str(strings, nameIndex);
            if ((attr == "" || attr.StartsWith('#')) && nameIndex < ids.Length && KnownIds.TryGetValue(ids[nameIndex], out var known))
                attr = known;
            attrs[attr] = dataType switch
            {
                0x03 => Str(strings, value),
                0x01 => $"@0x{value:x8}",
                0x10 => ((int)value).ToString(),
                0x11 => $"0x{value:x}",
                0x12 => value != 0 ? "true" : "false",
                _ => raw != 0xFFFFFFFF ? Str(strings, raw) : $"0x{value:x}",
            };
        }
        return new Element(name, attrs);
    }

    static string Str(string[] strings, uint i) => i < strings.Length ? strings[i] : "";
}
