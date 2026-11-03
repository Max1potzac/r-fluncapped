using System.Security.Cryptography;
using System.Text;

namespace Pvz2Conv;

/// Encrypted RTON, as PvZ2 China (PVZ2C) ships nearly all of its RTON files:
/// the bytes 10 00, then the RTON encrypted with Rijndael in CBC mode with a
/// 192-bit block, zero-padded to a whole block. The key is the 32 ASCII hex
/// digits of MD5("com_popcap_pvz2_magento_product_2013_05_05") (that string
/// is in libSrc.so) and the IV is characters 4 to 27 of the same key. .NET's
/// Aes only does 128-bit blocks, hence the small Rijndael below.
static class RtonCrypt
{
    const string Seed = "com_popcap_pvz2_magento_product_2013_05_05";
    const int Nb = 6;                 // block of 6 words = 24 bytes
    const int BlockBytes = Nb * 4;

    static readonly byte[] Key = Encoding.ASCII.GetBytes(Convert.ToHexString(MD5.HashData(Encoding.ASCII.GetBytes(Seed))).ToLowerInvariant());
    static readonly byte[] Iv = Key[4..28];
    static readonly Rijndael Cipher = new(Key, Nb);

    public static bool LooksEncrypted(ReadOnlySpan<byte> data) =>
        data.Length >= 2 + BlockBytes && (data.Length - 2) % BlockBytes == 0 && data[0] == 0x10 && data[1] == 0x00;

    /// Encrypted RTON whose first block decrypts to the RTON signature.
    public static bool IsEncryptedRton(ReadOnlySpan<byte> data)
    {
        if (!LooksEncrypted(data)) return false;
        Span<byte> block = stackalloc byte[BlockBytes];
        Cipher.DecryptBlock(data.Slice(2, BlockBytes), block);
        for (int i = 0; i < 4; i++) block[i] ^= Iv[i];
        return block[0] == 'R' && block[1] == 'T' && block[2] == 'O' && block[3] == 'N';
    }

    /// The RTON inside, with the zero padding after "DONE" taken off.
    public static byte[] Decrypt(ReadOnlySpan<byte> data)
    {
        if (!LooksEncrypted(data)) throw new FormatException("not an encrypted RTON (10 00 and whole 24-byte blocks)");
        var src = data[2..];
        var plain = new byte[src.Length];
        Span<byte> prev = stackalloc byte[BlockBytes];
        Iv.CopyTo(prev);
        for (int o = 0; o < src.Length; o += BlockBytes)
        {
            var dst = plain.AsSpan(o, BlockBytes);
            Cipher.DecryptBlock(src.Slice(o, BlockBytes), dst);
            for (int i = 0; i < BlockBytes; i++) dst[i] ^= prev[i];
            src.Slice(o, BlockBytes).CopyTo(prev);
        }
        int end = plain.Length;
        while (end > 0 && plain[end - 1] == 0) end--;
        // an RTON ends with "DONE"; keep anything after it rather than guess
        if (end >= 4 && plain.AsSpan(end - 4, 4).SequenceEqual("DONE"u8)) return plain[..end];
        return plain;
    }

    /// The other way, for putting edited RTON back into PVZ2C.
    public static byte[] Encrypt(ReadOnlySpan<byte> rton)
    {
        int padded = (rton.Length + BlockBytes - 1) / BlockBytes * BlockBytes;
        var plain = new byte[padded];
        rton.CopyTo(plain);
        var output = new byte[2 + padded];
        output[0] = 0x10;
        Span<byte> prev = stackalloc byte[BlockBytes];
        Span<byte> block = stackalloc byte[BlockBytes];
        Iv.CopyTo(prev);
        for (int o = 0; o < padded; o += BlockBytes)
        {
            for (int i = 0; i < BlockBytes; i++) block[i] = (byte)(plain[o + i] ^ prev[i]);
            var dst = output.AsSpan(2 + o, BlockBytes);
            Cipher.EncryptBlock(block, dst);
            dst.CopyTo(prev);
        }
        return output;
    }
}

/// Rijndael with a 128, 192 or 256-bit block (AES is the 128-bit case), one block at a time.
sealed class Rijndael
{
    static readonly byte[] SBox = new byte[256], InvSBox = new byte[256];
    static readonly byte[] M2 = new byte[256], M3 = new byte[256], M9 = new byte[256], M11 = new byte[256], M13 = new byte[256], M14 = new byte[256];

    static Rijndael()
    {
        for (int x = 0; x < 256; x++)
        {
            M2[x] = Mul((byte)x, 2); M3[x] = Mul((byte)x, 3); M9[x] = Mul((byte)x, 9);
            M11[x] = Mul((byte)x, 11); M13[x] = Mul((byte)x, 13); M14[x] = Mul((byte)x, 14);
        }
        for (int x = 0; x < 256; x++)
        {
            byte b = x == 0 ? (byte)0 : Inverse((byte)x);
            int s = b;
            for (int i = 1; i <= 4; i++) s ^= (byte)((b << i) | (b >> (8 - i)));
            SBox[x] = (byte)(s ^ 0x63);
            InvSBox[SBox[x]] = (byte)x;
        }
    }

    static byte XTime(byte a) => (byte)((a << 1) ^ ((a & 0x80) != 0 ? 0x1B : 0));

    static byte Mul(byte a, byte b)
    {
        byte r = 0;
        while (b != 0) { if ((b & 1) != 0) r ^= a; a = XTime(a); b >>= 1; }
        return r;
    }

    static byte Inverse(byte x)
    {
        for (int y = 1; y < 256; y++) if (Mul(x, (byte)y) == 1) return (byte)y;
        return 0;
    }

    readonly int _nb, _rounds;
    readonly byte[] _roundKeys;   // (rounds + 1) * nb * 4 bytes, column by column
    readonly int[] _shift;        // row shifts for this block size

    public Rijndael(byte[] key, int nb)
    {
        if (nb is not (4 or 6 or 8) || key.Length is not (16 or 24 or 32)) throw new ArgumentException("Rijndael block and key must be 128, 192 or 256 bits");
        int nk = key.Length / 4;
        _nb = nb;
        _rounds = Math.Max(nk, nb) + 6;
        _shift = nb == 8 ? [0, 1, 3, 4] : [0, 1, 2, 3];
        int words = nb * (_rounds + 1);
        _roundKeys = new byte[words * 4];
        key.CopyTo(_roundKeys, 0);
        byte rcon = 1;
        Span<byte> t = stackalloc byte[4];
        for (int i = nk; i < words; i++)
        {
            _roundKeys.AsSpan((i - 1) * 4, 4).CopyTo(t);
            if (i % nk == 0)
            {
                byte first = t[0];
                t[0] = (byte)(SBox[t[1]] ^ rcon); t[1] = SBox[t[2]]; t[2] = SBox[t[3]]; t[3] = SBox[first];
                rcon = XTime(rcon);
            }
            else if (nk > 6 && i % nk == 4)
                for (int j = 0; j < 4; j++) t[j] = SBox[t[j]];
            for (int j = 0; j < 4; j++) _roundKeys[i * 4 + j] = (byte)(_roundKeys[(i - nk) * 4 + j] ^ t[j]);
        }
    }

    void AddRoundKey(Span<byte> s, int round)
    {
        var k = _roundKeys.AsSpan(round * _nb * 4, _nb * 4);
        for (int i = 0; i < s.Length; i++) s[i] ^= k[i];
    }

    // state byte (row r, column c) is s[c * 4 + r]
    void ShiftRows(Span<byte> s, bool inverse)
    {
        Span<byte> row = stackalloc byte[8];
        for (int r = 1; r < 4; r++)
        {
            for (int c = 0; c < _nb; c++) row[c] = s[c * 4 + r];
            for (int c = 0; c < _nb; c++)
            {
                int from = inverse ? (c - _shift[r] + _nb) % _nb : (c + _shift[r]) % _nb;
                s[c * 4 + r] = row[from];
            }
        }
    }

    public void DecryptBlock(ReadOnlySpan<byte> input, Span<byte> output)
    {
        input[..(_nb * 4)].CopyTo(output);
        var s = output[..(_nb * 4)];
        AddRoundKey(s, _rounds);
        for (int round = _rounds - 1; round >= 0; round--)
        {
            ShiftRows(s, inverse: true);
            for (int i = 0; i < s.Length; i++) s[i] = InvSBox[s[i]];
            AddRoundKey(s, round);
            if (round == 0) break;
            for (int c = 0; c < _nb; c++)
            {
                byte a = s[c * 4], b = s[c * 4 + 1], d = s[c * 4 + 2], e = s[c * 4 + 3];
                s[c * 4] = (byte)(M14[a] ^ M11[b] ^ M13[d] ^ M9[e]);
                s[c * 4 + 1] = (byte)(M9[a] ^ M14[b] ^ M11[d] ^ M13[e]);
                s[c * 4 + 2] = (byte)(M13[a] ^ M9[b] ^ M14[d] ^ M11[e]);
                s[c * 4 + 3] = (byte)(M11[a] ^ M13[b] ^ M9[d] ^ M14[e]);
            }
        }
    }

    public void EncryptBlock(ReadOnlySpan<byte> input, Span<byte> output)
    {
        input[..(_nb * 4)].CopyTo(output);
        var s = output[..(_nb * 4)];
        AddRoundKey(s, 0);
        for (int round = 1; round <= _rounds; round++)
        {
            for (int i = 0; i < s.Length; i++) s[i] = SBox[s[i]];
            ShiftRows(s, inverse: false);
            if (round != _rounds)
                for (int c = 0; c < _nb; c++)
                {
                    byte a = s[c * 4], b = s[c * 4 + 1], d = s[c * 4 + 2], e = s[c * 4 + 3];
                    s[c * 4] = (byte)(M2[a] ^ M3[b] ^ d ^ e);
                    s[c * 4 + 1] = (byte)(a ^ M2[b] ^ M3[d] ^ e);
                    s[c * 4 + 2] = (byte)(a ^ b ^ M2[d] ^ M3[e]);
                    s[c * 4 + 3] = (byte)(M3[a] ^ b ^ d ^ M2[e]);
                }
            AddRoundKey(s, round);
        }
    }
}
