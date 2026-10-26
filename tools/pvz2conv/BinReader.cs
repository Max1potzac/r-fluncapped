using System.Text;

namespace Pvz2Conv;

/// Little-endian reader over a byte array with bounds-checked reads and
/// error messages that carry the file offset.
sealed class BinReader
{
    readonly byte[] _data;
    public int Pos;

    public BinReader(byte[] data, int pos = 0) { _data = data; Pos = pos; }

    public int Length => _data.Length;
    public bool AtEnd => Pos >= _data.Length;
    public int Remaining => _data.Length - Pos;

    void Need(int n)
    {
        if (Pos + n > _data.Length)
            throw new FormatException($"unexpected end of file at 0x{Pos:X} (needed {n} more bytes)");
    }

    public byte U8() { Need(1); return _data[Pos++]; }
    public sbyte I8() => (sbyte)U8();
    public ushort U16() { Need(2); var v = BitConverter.ToUInt16(_data, Pos); Pos += 2; return v; }
    public short I16() { Need(2); var v = BitConverter.ToInt16(_data, Pos); Pos += 2; return v; }
    public uint U32() { Need(4); var v = BitConverter.ToUInt32(_data, Pos); Pos += 4; return v; }
    public int I32() { Need(4); var v = BitConverter.ToInt32(_data, Pos); Pos += 4; return v; }
    public ulong U64() { Need(8); var v = BitConverter.ToUInt64(_data, Pos); Pos += 8; return v; }
    public long I64() { Need(8); var v = BitConverter.ToInt64(_data, Pos); Pos += 8; return v; }
    public float F32() { Need(4); var v = BitConverter.ToSingle(_data, Pos); Pos += 4; return v; }
    public double F64() { Need(8); var v = BitConverter.ToDouble(_data, Pos); Pos += 8; return v; }

    public byte[] Bytes(int n) { Need(n); var b = _data.AsSpan(Pos, n).ToArray(); Pos += n; return b; }

    public string Utf8(int n) { Need(n); var s = Encoding.UTF8.GetString(_data, Pos, n); Pos += n; return s; }

    /// Unsigned LEB128 (what RTON calls a varint).
    public ulong VarU64()
    {
        ulong result = 0;
        int shift = 0;
        while (true)
        {
            byte b = U8();
            if (shift < 64) result |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return result;
            shift += 7;
            if (shift > 70) throw new FormatException($"varint too long at 0x{Pos:X}");
        }
    }

    public uint VarU32() => (uint)VarU64();
    public long ZigZag64() { ulong v = VarU64(); return (long)(v >> 1) ^ -(long)(v & 1); }
    public int ZigZag32() => (int)ZigZag64();

    /// u16 length + UTF-8 bytes, as used by PAM.
    public string String16() => Utf8(U16());

    /// Zero-terminated UTF-8 string starting at an absolute offset (does not move Pos).
    public string CStringAt(int offset)
    {
        if (offset < 0 || offset >= _data.Length)
            throw new FormatException($"string offset 0x{offset:X} is outside the file");
        int end = Array.IndexOf(_data, (byte)0, offset);
        if (end < 0) end = _data.Length;
        return Encoding.UTF8.GetString(_data, offset, end - offset);
    }
}
