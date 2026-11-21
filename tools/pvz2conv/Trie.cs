using System.Text;

namespace Pvz2Conv;

/// The name tables in RSB and RSG files are a character trie written depth
/// first: each 4-byte record is one character plus a 24-bit pointer (in
/// units of 4 bytes) to the record holding the alternative character at the
/// same position. A zero character ends a name and is followed by payload.
///
/// Mod repackers write pointers the game never follows: at the end of the
/// table, back into names already read, or copies of an enclosing
/// character's pointer. Following them blindly is what produced duplicate
/// files under glued-together folder names, so a pointer is only taken when
/// it lands exactly where the alternative must start (right after everything
/// that hangs off the current character) and no enclosing character owns it.
static class Trie
{
    public static void Read(byte[] buf, int offset, int size, Func<int, int> payloadSize,
        Action<string, int> onName, List<string> warnings, string what)
    {
        if (size <= 0) return;
        if (offset < 0 || offset + (long)size > buf.Length) throw new FormatException($"{what} runs past the data");
        var bytes = new List<byte>();
        // alternatives already promised by enclosing characters, innermost last
        var claimed = new List<int>();
        int strays = 0;

        // returns the position just past this subtree
        int Node(int rel, int depth)
        {
            if (depth > 4096) throw new FormatException($"{what} nests too deep");
            while (true)
            {
                if (rel + 4 > size) throw new FormatException($"{what} is cut short at {rel}");
                int p = offset + rel;
                byte c = buf[p];
                int alt = (buf[p + 1] | buf[p + 2] << 8 | buf[p + 3] << 16) * 4;
                int end;
                if (c == 0)
                {
                    int payload = rel + 4;
                    if (payload + 4 > size) throw new FormatException($"{what} payload is cut short at {payload}");
                    onName(Decode(bytes), offset + payload);
                    end = payload + payloadSize(offset + payload);
                }
                else
                {
                    bool owns = alt != 0 && alt < size;
                    if (owns) claimed.Add(alt);
                    bytes.Add(c);
                    end = Node(rel + 4, depth + 1);
                    bytes.RemoveAt(bytes.Count - 1);
                    if (owns) claimed.RemoveAt(claimed.Count - 1);
                }
                if (alt == 0 || alt >= size) return end; // none, or the end-of-table pointer some packers write
                // Packers like Reflourished's give the last alternative in a run
                // the pointer of an enclosing character. It lands on the right
                // spot but belongs to the outer branch; taking it here glues
                // the outer names onto this prefix.
                if (claimed.Contains(alt)) return end;
                if (alt != end) { strays++; return end; }
                rel = alt;
            }
        }

        int last = Node(0, 0);
        if (strays > 0) warnings.Add($"{what}: ignored {strays} pointer(s) that point outside their branch");
        if (last < size && buf.AsSpan(offset + last, size - last).IndexOfAnyExcept((byte)0) >= 0)
            warnings.Add($"{what}: {size - last} bytes after the last name weren't reachable");
    }

    /// Writes a table that Read reads back: names (with their payloads) in byte order, each
    /// character's pointer leading to the next character at its position, the way the game's own
    /// packer lays them out.
    public static byte[] Write(IEnumerable<(string Name, byte[] Payload)> names)
    {
        var output = new List<byte>();
        void Node(List<(byte[] Tail, byte[] Payload)> items)
        {
            var branches = items.GroupBy(i => i.Tail.Length == 0 ? (byte)0 : i.Tail[0]).OrderBy(b => b.Key).ToList();
            for (int k = 0; k < branches.Count; k++)
            {
                int at = output.Count;
                output.AddRange([branches[k].Key, 0, 0, 0]);
                if (branches[k].Key == 0) output.AddRange(branches[k].First().Payload);
                else Node(branches[k].Select(i => (i.Tail[1..], i.Payload)).ToList());
                if (k + 1 < branches.Count)
                {
                    int alt = output.Count / 4;
                    output[at + 1] = (byte)alt; output[at + 2] = (byte)(alt >> 8); output[at + 3] = (byte)(alt >> 16);
                }
            }
        }
        var list = names.Select(n => (Encoding.UTF8.GetBytes(n.Name), n.Payload)).ToList();
        if (list.Count > 0) Node(list);
        return output.ToArray();
    }

    // Names are ASCII in practice; anything else is read as UTF-8, and
    // characters Windows can't put in a file name are replaced later.
    static string Decode(List<byte> bytes) => Encoding.UTF8.GetString(bytes.ToArray());
}
