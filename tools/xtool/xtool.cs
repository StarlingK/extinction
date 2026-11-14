using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

class Entry { public string Path; public uint Sector; public uint Size; public bool Dir; public string Magic; }

static class P
{
    const int SEC = 2048;
    static readonly long[] Parts = { 0L, 0x18300000L, 0xFD90000L, 0x2080000L };
    static readonly byte[] XMAGIC = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

    static int Main(string[] a)
    {
        if (a.Length < 2) { Console.WriteLine("usage: xtool iso|xbe|str ..."); return 2; }
        if (a[0] == "iso") return Iso(a[1], a[2]);
        if (a[0] == "xbe") return Xbe(a[1]);
        if (a[0] == "str") return Str(a[1], a[2], a.Length > 3 ? int.Parse(a[3]) : 6);
        if (a[0] == "riff") return Riff(a[1], a[2], a[3]);
        if (a[0] == "pull") return Pull(a[1], a[2], a[3], a[4]);
        if (a[0] == "zero") return Zero(a[1], a[2], a[3]);
        if (a[0] == "xstat") return XStat(a[1], a[2]);
        if (a[0] == "flat") return Flat(a[1], a[2]);
        if (a[0] == "types") return Types(a[1], a[2], a[3], a[4]);
        if (a[0] == "unpack") return UnpackAll(a[1], a[2], a[3]);
        if (a[0] == "mapscan") return MapScan(a[1], a[2], a[3], a[4]);
        if (a[0] == "extract") return ExtractAll(a[1], a[2], a[3]);
        if (a[0] == "hash") return HashTest(a.Skip(1).ToArray());
        return 2;
    }

    static byte[] ReadAt(FileStream f, long off, int n)
    {
        var b = new byte[n]; f.Seek(off, SeekOrigin.Begin);
        int got = 0; while (got < n) { int r = f.Read(b, got, n - got); if (r <= 0) break; got += r; }
        return b;
    }

    static bool Eq(byte[] b, int off, byte[] m)
    {
        for (int i = 0; i < m.Length; i++) if (b[off + i] != m[i]) return false;
        return true;
    }

    static int Iso(string path, string outDir)
    {
        Directory.CreateDirectory(outDir);
        using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            long part = -1;
            foreach (var p in Parts)
            {
                if (p + 32 * SEC + SEC > f.Length) continue;
                var v = ReadAt(f, p + 32 * SEC, SEC);
                if (Eq(v, 0, XMAGIC) && Eq(v, 0x7EC, XMAGIC)) { part = p; break; }
            }
            if (part < 0) { Console.WriteLine("no XDVDFS volume found at known offsets"); return 1; }
            var vd = ReadAt(f, part + 32 * SEC, SEC);
            uint rootSec = BitConverter.ToUInt32(vd, 20), rootSize = BitConverter.ToUInt32(vd, 24);
            long ft = BitConverter.ToInt64(vd, 28);
            string stamp; try { stamp = DateTime.FromFileTimeUtc(ft).ToString("yyyy-MM-dd HH:mm:ss"); } catch { stamp = "invalid"; }
            Console.WriteLine("image size      : {0}", f.Length);
            Console.WriteLine("game partition  : 0x{0:X} ({1})", part, part == 0x18300000L ? "XGD1" : part == 0xFD90000L ? "XGD2" : part == 0x2080000L ? "XGD3" : "XISO, no video partition");
            Console.WriteLine("root dir        : sector {0}, {1} bytes", rootSec, rootSize);
            Console.WriteLine("volume timestamp: {0} UTC", stamp);

            var list = new List<Entry>();
            Walk(f, part, rootSec, rootSize, "", list);
            foreach (var e in list.Where(x => !x.Dir && x.Size > 0))
            {
                var h = ReadAt(f, part + (long)e.Sector * SEC, (int)Math.Min(16, e.Size));
                e.Magic = BitConverter.ToString(h).Replace("-", " ");
            }
            using (var w = new StreamWriter(Path.Combine(outDir, "files.tsv"), false, new UTF8Encoding(false)))
            {
                w.WriteLine("path\tsize\tsector\tmagic_hex\tmagic_ascii");
                foreach (var e in list.Where(x => !x.Dir).OrderBy(x => x.Path, StringComparer.OrdinalIgnoreCase))
                    w.WriteLine("{0}\t{1}\t{2}\t{3}\t{4}", e.Path, e.Size, e.Sector, e.Magic, Asc(e.Magic));
            }
            var files = list.Where(x => !x.Dir).ToList();
            Console.WriteLine("files           : {0} in {1} directories, {2:N0} bytes", files.Count, list.Count(x => x.Dir), files.Sum(x => (long)x.Size));

            var xbe = files.FirstOrDefault(x => x.Path.Equals("default.xbe", StringComparison.OrdinalIgnoreCase));
            if (xbe == null) { Console.WriteLine("default.xbe not found at root"); return 1; }
            var data = ReadAt(f, part + (long)xbe.Sector * SEC, (int)xbe.Size);
            File.WriteAllBytes(Path.Combine(outDir, "default.xbe"), data);
            Console.WriteLine("default.xbe     : {0} bytes extracted", data.Length);
        }
        return 0;
    }

    static string Asc(string hex)
    {
        if (string.IsNullOrEmpty(hex)) return "";
        var sb = new StringBuilder();
        foreach (var s in hex.Split(' ')) { int c = Convert.ToInt32(s, 16); sb.Append(c >= 32 && c < 127 ? (char)c : '.'); }
        return sb.ToString();
    }

    static void Walk(FileStream f, long part, uint sec, uint size, string prefix, List<Entry> outp)
    {
        if (size == 0) return;
        var d = ReadAt(f, part + (long)sec * SEC, (int)size);
        var seen = new HashSet<int>();
        var st = new Stack<int>(); st.Push(0);
        while (st.Count > 0)
        {
            int o = st.Pop();
            if (o < 0 || o + 14 > d.Length || !seen.Add(o)) continue;
            ushort l = BitConverter.ToUInt16(d, o), r = BitConverter.ToUInt16(d, o + 2);
            if (l == 0xFFFF && r == 0xFFFF) continue;
            uint s = BitConverter.ToUInt32(d, o + 4), z = BitConverter.ToUInt32(d, o + 8);
            byte attr = d[o + 12], n = d[o + 13];
            if (o + 14 + n > d.Length) continue;
            string name = Encoding.ASCII.GetString(d, o + 14, n);
            string full = prefix + name;
            bool isDir = (attr & 0x10) != 0;
            outp.Add(new Entry { Path = full, Sector = s, Size = z, Dir = isDir });
            if (isDir) Walk(f, part, s, z, full + "/", outp);
            if (l != 0) st.Push(l * 4);
            if (r != 0) st.Push(r * 4);
        }
    }

    static byte[] X; static uint Base;
    static uint U(int o) { return BitConverter.ToUInt32(X, o); }
    static string CStr(uint va)
    {
        if (va == 0) return "(none)";
        long o = (long)va - Base; if (o < 0 || o >= X.Length) return string.Format("(va 0x{0:X8} outside headers)", va);
        int e = (int)o; while (e < X.Length && X[e] != 0) e++;
        return Encoding.ASCII.GetString(X, (int)o, e - (int)o);
    }
    static string WStr(uint va)
    {
        if (va == 0) return "(none)";
        long o = (long)va - Base; if (o < 0 || o >= X.Length) return string.Format("(va 0x{0:X8} outside headers)", va);
        int e = (int)o; while (e + 1 < X.Length && (X[e] != 0 || X[e + 1] != 0)) e += 2;
        return Encoding.Unicode.GetString(X, (int)o, e - (int)o);
    }
    static string Ts(uint t) { return new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(t).ToString("yyyy-MM-dd HH:mm:ss") + " UTC"; }

    static int Xbe(string path)
    {
        X = File.ReadAllBytes(path);
        if (Encoding.ASCII.GetString(X, 0, 4) != "XBEH") { Console.WriteLine("not an XBE"); return 1; }
        Base = U(0x104);
        uint ep = U(0x128), th = U(0x158);
        const uint EPR = 0xA8FC57AB, EPD = 0x94859D4B, KTR = 0x5B6D40B6, KTD = 0xEFB1F152;
        uint epRet = ep ^ EPR, epDbg = ep ^ EPD;
        bool retail = epRet >= Base && epRet < Base + U(0x10C);
        bool debug = epDbg >= Base && epDbg < Base + U(0x10C);
        uint entry = retail ? epRet : epDbg;
        uint thunk = th ^ (retail ? KTR : KTD);
        Console.WriteLine("== header");
        Console.WriteLine("file size    : {0}", X.Length);
        Console.WriteLine("key type     : {0}", retail ? "retail" : debug ? "debug" : "unknown");
        Console.WriteLine("base         : 0x{0:X8}", Base);
        Console.WriteLine("image size   : 0x{0:X}", U(0x10C));
        Console.WriteLine("build time   : {0}", Ts(U(0x114)));
        Console.WriteLine("entry point  : 0x{0:X8}", entry);
        Console.WriteLine("kernel thunk : 0x{0:X8}", thunk);
        Console.WriteLine("init flags   : 0x{0:X8}", U(0x124));
        Console.WriteLine("stack commit : 0x{0:X}", U(0x130));
        Console.WriteLine("heap res/com : 0x{0:X} / 0x{1:X}", U(0x134), U(0x138));
        Console.WriteLine("PE size      : 0x{0:X}, PE time {1}", U(0x140), Ts(U(0x148)));
        Console.WriteLine("debug path   : {0}", CStr(U(0x14C)));
        Console.WriteLine("debug file   : {0}", CStr(U(0x150)));
        Console.WriteLine("debug file(w): {0}", WStr(U(0x154)));
        Console.WriteLine("non-kernel imports addr: 0x{0:X8}", U(0x15C));
        Console.WriteLine("TLS addr     : 0x{0:X8}", U(0x12C));

        int c = (int)(U(0x118) - Base);
        Console.WriteLine("== certificate");
        Console.WriteLine("cert time    : {0}", Ts(U(c + 4)));
        uint tid = U(c + 8);
        Console.WriteLine("title id     : 0x{0:X8} ({1}{2}-{3:D3})", tid, (char)(tid >> 24), (char)((tid >> 16) & 0xFF), tid & 0xFFFF);
        Console.WriteLine("title name   : {0}", Encoding.Unicode.GetString(X, c + 0xC, 80).TrimEnd('\0'));
        Console.WriteLine("allowed media: 0x{0:X8}", U(c + 0x9C));
        Console.WriteLine("region       : 0x{0:X8} (1=NA 2=JP 4=RoW)", U(c + 0xA0));
        Console.WriteLine("ratings      : 0x{0:X8}", U(c + 0xA4));
        Console.WriteLine("disc number  : {0}", U(c + 0xA8));
        Console.WriteLine("version      : {0}", U(c + 0xAC));

        Console.WriteLine("== sections");
        int n = (int)U(0x11C), sh = (int)(U(0x120) - Base);
        Console.WriteLine("{0,-10} {1,-10} {2,-10} {3,-10} {4,-10} {5,-9} {6}", "name", "vaddr", "vsize", "raw", "rawsize", "flags", "entropy");
        var secs = new List<uint[]>();
        for (int i = 0; i < n; i++)
        {
            int o = sh + i * 56;
            uint fl = U(o), va = U(o + 4), vs = U(o + 8), ra = U(o + 0xC), rs = U(o + 0x10);
            secs.Add(new[] { va, vs, ra, rs });
            Console.WriteLine("{0,-10} 0x{1:X8} 0x{2:X8} 0x{3:X8} 0x{4:X8} 0x{5:X7} {6:F2}", CStr(U(o + 0x14)), va, vs, ra, rs, fl, Ent(ra, rs));
        }

        Console.WriteLine("== libraries ({0}, kernel lib 0x{1:X8}, xapi lib 0x{2:X8})", U(0x160), U(0x168), U(0x16C));
        int lc = (int)U(0x160), lo = (int)(U(0x164) - Base);
        for (int i = 0; i < lc; i++)
        {
            int o = lo + i * 16;
            string nm = Encoding.ASCII.GetString(X, o, 8).TrimEnd('\0');
            ushort maj = BitConverter.ToUInt16(X, o + 8), min = BitConverter.ToUInt16(X, o + 10), bld = BitConverter.ToUInt16(X, o + 12), fl = BitConverter.ToUInt16(X, o + 14);
            Console.WriteLine("{0,-9} {1}.{2}.{3}  qfe {4}, approved {5}, debug {6}", nm, maj, min, bld, fl & 0x1FFF, (fl >> 13) & 3, (fl >> 15) & 1);
        }

        Console.WriteLine("== kernel imports (ordinals)");
        long tf = -1;
        foreach (var s in secs) if (thunk >= s[0] && thunk < s[0] + s[1]) tf = (long)s[2] + (thunk - s[0]);
        if (tf < 0) Console.WriteLine("thunk table not mapped");
        else
        {
            var ords = new List<uint>();
            for (long o = tf; o + 4 <= X.Length; o += 4)
            {
                uint v = BitConverter.ToUInt32(X, (int)o);
                if (v == 0) break;
                ords.Add(v & 0x7FFFFFFF);
            }
            Console.WriteLine("count {0}: {1}", ords.Count, string.Join(",", ords));
        }
        return 0;
    }

    static double Ent(uint off, uint len)
    {
        if (len == 0 || off >= X.Length) return 0;
        long end = Math.Min((long)off + len, X.Length);
        var h = new long[256];
        for (long i = off; i < end; i++) h[X[i]]++;
        double tot = end - off, e = 0;
        foreach (var c in h) if (c > 0) { double p = c / tot; e -= p * Math.Log(p, 2); }
        return e;
    }

    static string[] Find(string tsv, string name)
    {
        return File.ReadAllLines(tsv).Skip(1).Select(l => l.Split('\t')).First(c => c[0].Equals(name, StringComparison.OrdinalIgnoreCase));
    }

    static int Pull(string iso, string tsv, string name, string outFile)
    {
        var c = Find(tsv, name);
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
            File.WriteAllBytes(outFile, ReadAt(f, 0x18300000L + long.Parse(c[2]) * SEC, (int)long.Parse(c[1])));
        return 0;
    }

    static int Zero(string iso, string tsv, string name)
    {
        var c = Find(tsv, name);
        long nz = 0, first = -1, size = long.Parse(c[1]);
        var h = new long[256];
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            f.Seek(0x18300000L + long.Parse(c[2]) * SEC, SeekOrigin.Begin);
            var b = new byte[1 << 20]; long pos = 0;
            while (pos < size)
            {
                int r = f.Read(b, 0, (int)Math.Min(b.Length, size - pos)); if (r <= 0) break;
                for (int i = 0; i < r; i++) { h[b[i]]++; if (b[i] != 0) { nz++; if (first < 0) first = pos + i; } }
                pos += r;
            }
        }
        double e = 0; foreach (var n in h) if (n > 0) { double p = (double)n / size; e -= p * Math.Log(p, 2); }
        Console.WriteLine("{0}: {1} bytes, nonzero {2}, first nonzero at {3}, entropy {4:F3}", name, size, nz, first, e);
        return 0;
    }

    static int Flat(string xbe, string outFile)
    {
        var d = File.ReadAllBytes(xbe);
        uint b = BitConverter.ToUInt32(d, 0x104), img = BitConverter.ToUInt32(d, 0x10C), hs = BitConverter.ToUInt32(d, 0x108);
        var m = new byte[img];
        Array.Copy(d, 0, m, 0, Math.Min(hs, img));
        int n = (int)BitConverter.ToUInt32(d, 0x11C), sh = (int)(BitConverter.ToUInt32(d, 0x120) - b);
        for (int i = 0; i < n; i++)
        {
            int o = sh + i * 56;
            uint va = BitConverter.ToUInt32(d, o + 4), ra = BitConverter.ToUInt32(d, o + 0xC), rs = BitConverter.ToUInt32(d, o + 0x10);
            Array.Copy(d, ra, m, va - b, rs);
        }
        File.WriteAllBytes(outFile, m);
        Console.WriteLine("flat image: {0} bytes, base 0x{1:X8}", m.Length, b);
        return 0;
    }

    static int XStat(string iso, string tsv)
    {
        var stat = new Dictionary<string, long[]>();
        var hdr = new Dictionary<string, Dictionary<string, int>>();
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
            foreach (var line in File.ReadAllLines(tsv).Skip(1))
            {
                var c = line.Split('\t');
                if (!c[0].EndsWith(".tbx", StringComparison.OrdinalIgnoreCase)) continue;
                long start = 0x18300000L + long.Parse(c[2]) * SEC, end = start + long.Parse(c[1]);
                long pos = start + 12;
                while (pos + 12 <= end)
                {
                    var h = ReadAt(f, pos, 12);
                    uint lsz = BitConverter.ToUInt32(h, 4);
                    long p = pos + 12, le = pos + 8 + lsz;
                    while (p + 8 <= le)
                    {
                        var k = ReadAt(f, p, 8);
                        string id = Fcc(k, 0); uint sz = BitConverter.ToUInt32(k, 4);
                        if (id == "DATX" || id == "PLAX" || id == "TEMX" || id == "DATA" || id == "PLAT" || id == "TEMP")
                        {
                            var b = ReadAt(f, p + 8, (int)Math.Min(sz, 1 << 16));
                            long[] s; if (!stat.TryGetValue(id, out s)) { s = new long[6]; stat[id] = s; hdr[id] = new Dictionary<string, int>(); }
                            uint claim = BitConverter.ToUInt32(b, 0);
                            s[0]++; s[1] += sz; s[2] += claim; if (claim > sz) s[3]++;
                            if (BitConverter.ToUInt16(b, 4) == 0x07FF) s[4]++;
                            var hg = new long[256]; foreach (var x in b) hg[x]++;
                            double e = 0; foreach (var n in hg) if (n > 0) { double q = (double)n / b.Length; e -= q * Math.Log(q, 2); }
                            s[5] += (long)(e * 1000);
                            string tag = BitConverter.ToString(b, 4, 2); int t; hdr[id].TryGetValue(tag, out t); hdr[id][tag] = t + 1;
                        }
                        p += 8 + sz + (sz & 1);
                    }
                    pos += 8 + lsz + (lsz & 1);
                }
            }
        foreach (var kv in stat)
        {
            var s = kv.Value;
            Console.WriteLine("{0}: n={1} bytes={2:N0} sum(first u32)={3:N0} ratio={4:F2} first-u32>size in {5} u16@4==FF07 in {6} avg entropy(first 64K)={7:F2} top u16@4: {8}", kv.Key, s[0], s[1], s[2], (double)s[2] / s[1], s[3], s[4], s[5] / 1000.0 / s[0],
                string.Join(" ", hdr[kv.Key].OrderByDescending(x => x.Value).Take(4).Select(x => x.Key + "x" + x.Value)));
        }
        return 0;
    }

    static uint[] CrcT;
    static uint Crc(byte[] d)
    {
        if (CrcT == null) { CrcT = new uint[256]; for (uint i = 0; i < 256; i++) { uint x = i; for (int k = 0; k < 8; k++) x = (x & 1) != 0 ? 0xEDB88320 ^ (x >> 1) : x >> 1; CrcT[i] = x; } }
        uint c = 0xFFFFFFFF; foreach (var b in d) c = CrcT[(c ^ b) & 0xFF] ^ (c >> 8); return ~c;
    }

    public static byte[] Lzss(byte[] s, int off, int len, out int consumed, out bool sawEnd)
    {
        int claim = BitConverter.ToInt32(s, off), p = off + 4, end = off + len, o = 0;
        var d = new byte[claim];
        sawEnd = false; consumed = 0;
        ushort win = BitConverter.ToUInt16(s, p); p += 2;
        int k = 0; ushort mask = 0; uint w = win;
        while ((w & 0x8000) == 0 && k < 16) { w <<= 1; mask = (ushort)((mask << 1) | 1); k++; }
        while (p + 2 <= end)
        {
            ushort flags = BitConverter.ToUInt16(s, p); p += 2;
            for (ushort bit = 0x8000; bit != 0; bit >>= 1)
            {
                if (p + 2 > end) { consumed = p - off; return Trim(d, o); }
                ushort v = BitConverter.ToUInt16(s, p); p += 2;
                if ((flags & bit) == 0)
                {
                    if (o + 2 > claim) { consumed = p - off; return Trim(d, o); }
                    d[o++] = (byte)v; d[o++] = (byte)(v >> 8);
                    continue;
                }
                if (v == 0) { sawEnd = true; consumed = p - off; return Trim(d, o); }
                int n = (v & mask) * 2, src = o - (v >> k) * 2;
                if (src < 0 || o + n > claim) { consumed = p - off; return Trim(d, o); }
                int dw = n / 4;
                for (int i = 0; i < dw; i++) { int a = BitConverter.ToInt32(d, src); Array.Copy(BitConverter.GetBytes(a), 0, d, o, 4); src += 4; o += 4; }
                for (int i = n & 3; i > 0; i--) d[o++] = d[src++];
            }
        }
        consumed = p - off;
        return Trim(d, o);
    }
    static byte[] Trim(byte[] d, int n) { if (n == d.Length) return d; var r = new byte[n]; Array.Copy(d, r, n); return r; }

    static int ExtractAll(string iso, string tsv, string outDir)
    {
        int n = 0; long total = 0;
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
            foreach (var line in File.ReadAllLines(tsv).Skip(1))
            {
                var c = line.Split('\t');
                if (c[0].Equals("cerbera.tvr", StringComparison.OrdinalIgnoreCase)) continue;
                var o = Path.Combine(outDir, c[0].Replace('/', '\\'));
                Directory.CreateDirectory(Path.GetDirectoryName(o));
                File.WriteAllBytes(o, ReadAt(f, 0x18300000L + long.Parse(c[2]) * SEC, (int)long.Parse(c[1])));
                n++; total += long.Parse(c[1]);
            }
        Console.WriteLine("extracted {0} files, {1:N0} bytes", n, total);
        return 0;
    }

    static int MapScan(string iso, string tsv, string rttiTsv, string outFile)
    {
        var names = new HashSet<string>(File.ReadAllLines(rttiTsv).Skip(1).Select(l => l.Split('\t')[0]));
        var byCrc = new Dictionary<uint, string>();
        foreach (var n in names) byCrc[CrcFF(Encoding.ASCII.GetBytes(n))] = n;
        using (var w = new StreamWriter(outFile, false, new UTF8Encoding(false)))
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
            foreach (var line in File.ReadAllLines(tsv).Skip(1))
            {
                var c = line.Split('\t');
                if (!c[0].EndsWith(".tbx", StringComparison.OrdinalIgnoreCase)) continue;
                var file = ReadAt(f, 0x18300000L + long.Parse(c[2]) * SEC, (int)long.Parse(c[1]));
                var text = new Dictionary<string, int>();
                var crc = new Dictionary<string, int>();
                int pos = 12;
                while (pos + 12 <= file.Length)
                {
                    int lsz = BitConverter.ToInt32(file, pos + 4), p = pos + 12, le = pos + 8 + lsz;
                    while (p + 8 <= le)
                    {
                        string id = Fcc(file, p); int sz = BitConverter.ToInt32(file, p + 4);
                        byte[] body;
                        if (id == "DATX" || id == "PLAX" || id == "TEMX") { int u; bool e; body = Lzss(file, p + 8, sz, out u, out e); }
                        else { body = new byte[sz]; Array.Copy(file, p + 8, body, 0, sz); }
                        ScanBody(body, names, byCrc, text, crc);
                        p += 8 + sz + (sz & 1);
                    }
                    pos += 8 + lsz + (lsz & 1);
                }
                foreach (var k in text.Keys.Union(crc.Keys).OrderBy(x => x, StringComparer.Ordinal))
                {
                    int t, h; text.TryGetValue(k, out t); crc.TryGetValue(k, out h);
                    w.WriteLine("{0}\t{1}\t{2}\t{3}", c[0], k, t, h);
                }
            }
        return 0;
    }

    static void ScanBody(byte[] b, HashSet<string> names, Dictionary<uint, string> byCrc, Dictionary<string, int> text, Dictionary<string, int> crc)
    {
        for (int i = 0; i + 4 <= b.Length; i += 4)
        {
            string n;
            if (byCrc.TryGetValue(BitConverter.ToUInt32(b, i), out n)) { int v; crc.TryGetValue(n, out v); crc[n] = v + 1; }
        }
        int start = -1;
        for (int i = 0; i <= b.Length; i++)
        {
            bool tok = i < b.Length && ((b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= 'a' && b[i] <= 'z') || (b[i] >= '0' && b[i] <= '9') || b[i] == '_');
            if (tok) { if (start < 0) start = i; continue; }
            if (start >= 0)
            {
                int len = i - start;
                if (len >= 5 && len < 80)
                {
                    string s = Encoding.ASCII.GetString(b, start, len);
                    if (names.Contains(s) || s.StartsWith("MiniMap_") || s.StartsWith("MapLoaded"))
                    { int v; text.TryGetValue(s, out v); text[s] = v + 1; }
                }
                start = -1;
            }
        }
    }

    static int UnpackAll(string iso, string tsv, string reportFile)
    {
        var st = new SortedDictionary<string, long[]>();
        var magic = new Dictionary<string, int>();
        var fails = new List<string>();
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
            foreach (var line in File.ReadAllLines(tsv).Skip(1))
            {
                var c = line.Split('\t');
                if (!c[0].EndsWith(".tbx", StringComparison.OrdinalIgnoreCase)) continue;
                var file = ReadAt(f, 0x18300000L + long.Parse(c[2]) * SEC, (int)long.Parse(c[1]));
                int pos = 12;
                while (pos + 12 <= file.Length)
                {
                    int lsz = BitConverter.ToInt32(file, pos + 4), p = pos + 12, le = pos + 8 + lsz;
                    while (p + 8 <= le)
                    {
                        string id = Fcc(file, p); int sz = BitConverter.ToInt32(file, p + 4);
                        if (id == "DATX" || id == "PLAX" || id == "TEMX")
                        {
                            long[] s; if (!st.TryGetValue(id, out s)) { s = new long[7]; st[id] = s; }
                            s[0]++; s[2] += sz;
                            int claim = BitConverter.ToInt32(file, p + 8), used; bool end;
                            byte[] o = null;
                            try { o = Lzss(file, p + 8, sz, out used, out end); }
                            catch (Exception e) { s[6]++; fails.Add(c[0] + " " + id + " exception " + e.GetType().Name); o = null; used = 0; end = false; }
                            if (o != null)
                            {
                                s[3] += o.Length;
                                bool ok = end && o.Length == claim;
                                if (ok) s[1]++;
                                if (!end) s[4]++;
                                if (o.Length != claim) { s[5]++; }
                                if (!ok && fails.Count < 30) fails.Add(string.Format("{0} {1} @{2} packed {3} claim {4} got {5} used {6} end {7}", c[0], id, p, sz, claim, o.Length, used, end));
                                if (o.Length >= 4)
                                {
                                    string key = id + " " + Asc(BitConverter.ToString(o, 0, 4).Replace("-", " "));
                                    int t; magic.TryGetValue(key, out t); magic[key] = t + 1;
                                }
                            }
                        }
                        p += 8 + sz + (sz & 1);
                    }
                    pos += 8 + lsz + (lsz & 1);
                }
            }
        using (var w = new StreamWriter(reportFile, false, new UTF8Encoding(false)))
        {
            foreach (var kv in st) { var s = kv.Value; var t = string.Format("{0}: chunks {1}, verified {2}, packed {3:N0}, unpacked {4:N0}, no end token {5}, size mismatch {6}, exceptions {7}", kv.Key, s[0], s[1], s[2], s[3], s[4], s[5], s[6]); Console.WriteLine(t); w.WriteLine(t); }
            foreach (var x in fails) { Console.WriteLine("FAIL " + x); w.WriteLine("FAIL " + x); }
            foreach (var kv in magic.OrderByDescending(x => x.Value).Take(25)) { var t = string.Format("first4 {0} x{1}", kv.Key, kv.Value); Console.WriteLine(t); w.WriteLine(t); }
        }
        return 0;
    }

    static uint CrcFF(byte[] d) { return ~Crc(d); }

    static int Types(string iso, string tsv, string rttiTsv, string outFile)
    {
        var names = new Dictionary<uint, string>();
        foreach (var l in File.ReadAllLines(rttiTsv).Skip(1))
        {
            var n = l.Split('\t')[0];
            names[CrcFF(Encoding.ASCII.GetBytes(n))] = n;
        }
        var count = new Dictionary<uint, long>();
        var files = new Dictionary<uint, HashSet<string>>();
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
            foreach (var line in File.ReadAllLines(tsv).Skip(1))
            {
                var c = line.Split('\t');
                if (!c[0].EndsWith(".tbx", StringComparison.OrdinalIgnoreCase)) continue;
                long start = 0x18300000L + long.Parse(c[2]) * SEC, end = start + long.Parse(c[1]), pos = start + 12;
                while (pos + 12 <= end)
                {
                    var h = ReadAt(f, pos, 12);
                    uint lsz = BitConverter.ToUInt32(h, 4);
                    long p = pos + 12, le = pos + 8 + lsz;
                    while (p + 8 <= le)
                    {
                        var k = ReadAt(f, p, 8);
                        uint sz = BitConverter.ToUInt32(k, 4);
                        if (Fcc(k, 0) == "TYPE")
                        {
                            var b = ReadAt(f, p + 8, (int)sz);
                            int n = BitConverter.ToInt32(b, 0);
                            for (int i = 0; i < n && 4 + i * 8 + 8 <= b.Length; i++)
                            {
                                uint t = BitConverter.ToUInt32(b, 4 + i * 8);
                                long v; count.TryGetValue(t, out v); count[t] = v + 1;
                                HashSet<string> s; if (!files.TryGetValue(t, out s)) { s = new HashSet<string>(); files[t] = s; }
                                s.Add(c[0]);
                            }
                        }
                        p += 8 + sz + (sz & 1);
                    }
                    pos += 8 + lsz + (lsz & 1);
                }
            }
        using (var w = new StreamWriter(outFile, false, new UTF8Encoding(false)))
        {
            w.WriteLine("hash\tname\tinstances\tfiles");
            foreach (var kv in count.OrderByDescending(x => x.Value))
            {
                string n; names.TryGetValue(kv.Key, out n);
                w.WriteLine("{0:X8}\t{1}\t{2}\t{3}", kv.Key, n ?? "", kv.Value, files[kv.Key].Count);
            }
        }
        Console.WriteLine("distinct types {0}, named {1}, instances {2}, in named types {3}", count.Count, count.Keys.Count(names.ContainsKey), count.Values.Sum(), count.Where(k => names.ContainsKey(k.Key)).Sum(k => k.Value));
        return 0;
    }

    static uint Crc0(byte[] d) { Crc(new byte[0]); uint c = 0; foreach (var b in d) c = CrcT[(c ^ b) & 0xFF] ^ (c >> 8); return c; }
    static uint CrcMsb(byte[] d) { uint c = 0xFFFFFFFF; foreach (var b in d) { c ^= (uint)b << 24; for (int k = 0; k < 8; k++) c = (c & 0x80000000) != 0 ? (c << 1) ^ 0x04C11DB7 : c << 1; } return ~c; }
    static uint Fnv1(byte[] d) { uint h = 2166136261; foreach (var b in d) { h *= 16777619; h ^= b; } return h; }
    static uint Fnv1a(byte[] d) { uint h = 2166136261; foreach (var b in d) { h ^= b; h *= 16777619; } return h; }
    static uint Djb(byte[] d) { uint h = 5381; foreach (var b in d) h = h * 33 + b; return h; }
    static uint Sdbm(byte[] d) { uint h = 0; foreach (var b in d) h = b + (h << 6) + (h << 16) - h; return h; }
    static uint Mul31(byte[] d) { uint h = 0; foreach (var b in d) h = h * 31 + b; return h; }

    static int HashTest(string[] a)
    {
        var targets = new HashSet<uint>(a[0].Split(',').Select(s => Convert.ToUInt32(s, 16)));
        var algs = new Dictionary<string, Func<byte[], uint>> { { "crc32", Crc }, { "crc32-ff", CrcFF }, { "crc32-noinv", Crc0 }, { "crc32-msb", CrcMsb }, { "fnv1", Fnv1 }, { "fnv1a", Fnv1a }, { "djb2", Djb }, { "sdbm", Sdbm }, { "mul31", Mul31 } };
        var cand = new HashSet<string>();
        foreach (var file in a.Skip(1))
            foreach (var l in File.ReadAllLines(file))
            {
                var p = l.Split('\t'); var s = p.Length >= 3 ? p[2] : l;
                cand.Add(s);
            }
        int hits = 0; long tested = 0;
        foreach (var raw in cand)
            foreach (var v in new[] { raw, raw.ToLowerInvariant(), raw.ToUpperInvariant() }.Distinct())
                foreach (var ext in new[] { "", ".tbd", ".tbx", ".TBD", ".TBX" })
                    foreach (var kv in algs)
                    {
                        tested++;
                        uint h = kv.Value(Encoding.ASCII.GetBytes(v + ext));
                        if (targets.Contains(h)) { Console.WriteLine("HIT {0:X8} {1} \"{2}\"", h, kv.Key, v + ext); hits++; }
                    }
        Console.WriteLine("candidates {0}, hashes tested {1}, hits {2}", cand.Count, tested, hits);
        return 0;
    }

    class Agg { public long Count; public long Bytes; public long Min = long.MaxValue; public long Max; public string Sample; }
    static string Fcc(byte[] b, int o) { var s = new StringBuilder(); for (int i = 0; i < 4; i++) { byte c = b[o + i]; s.Append(c >= 32 && c < 127 ? (char)c : '?'); } return s.ToString(); }

    static int Riff(string iso, string tsv, string outDir)
    {
        const long part = 0x18300000L;
        var agg = new SortedDictionary<string, Agg>(StringComparer.Ordinal);
        var perFile = new StringBuilder();
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            foreach (var line in File.ReadAllLines(tsv).Skip(1))
            {
                var c = line.Split('\t');
                if (!c[0].EndsWith(".tbx", StringComparison.OrdinalIgnoreCase) && !c[0].EndsWith(".tvr", StringComparison.OrdinalIgnoreCase)) continue;
                long start = part + long.Parse(c[2]) * SEC, size = long.Parse(c[1]);
                var top = new SortedDictionary<string, long>(StringComparer.Ordinal);
                WalkRiff(f, start, start + size, "", agg, top, 0);
                perFile.Append(c[0]).Append('\t').Append(size).Append('\t');
                perFile.AppendLine(string.Join(" ", top.Select(kv => kv.Key + "x" + kv.Value)));
            }
        }
        File.WriteAllText(Path.Combine(outDir, "riff_files.tsv"), perFile.ToString());
        using (var w = new StreamWriter(Path.Combine(outDir, "riff_chunks.tsv"), false, new UTF8Encoding(false)))
        {
            w.WriteLine("chunk_path\tcount\ttotal_bytes\tmin\tmax\tfirst_bytes");
            foreach (var kv in agg) w.WriteLine("{0}\t{1}\t{2}\t{3}\t{4}\t{5}", kv.Key, kv.Value.Count, kv.Value.Bytes, kv.Value.Min, kv.Value.Max, kv.Value.Sample);
        }
        Console.WriteLine("chunk paths: {0}", agg.Count);
        return 0;
    }

    static void WalkRiff(FileStream f, long pos, long end, string prefix, SortedDictionary<string, Agg> agg, SortedDictionary<string, long> top, int depth)
    {
        while (pos + 8 <= end)
        {
            var h = ReadAt(f, pos, 12);
            string id = Fcc(h, 0);
            uint sz = BitConverter.ToUInt32(h, 4);
            long body = pos + 8, next = body + sz + (sz & 1);
            if (next > end + 1) { Add(agg, prefix + "!BAD(" + id + ")", sz, ""); return; }
            bool cont = (id == "RIFF" || id == "LIST") && sz >= 4;
            string name = cont ? id + ":" + Fcc(h, 8) : id;
            string path = prefix + "/" + name;
            if (depth <= 1) { long t; top.TryGetValue(name, out t); top[name] = t + 1; }
            string sample = "";
            if (!cont && sz > 0)
            {
                var s = ReadAt(f, body, (int)Math.Min(48, sz));
                sample = BitConverter.ToString(s).Replace("-", "") + " |" + Asc(BitConverter.ToString(s).Replace("-", " ")) + "|";
            }
            Add(agg, path, sz, sample);
            if (cont && depth < 8) WalkRiff(f, body + 4, body + sz, path, agg, top, depth + 1);
            pos = next;
        }
    }

    static void Add(SortedDictionary<string, Agg> d, string k, long sz, string sample)
    {
        Agg a; if (!d.TryGetValue(k, out a)) { a = new Agg { Sample = sample }; d[k] = a; }
        a.Count++; a.Bytes += sz; a.Min = Math.Min(a.Min, sz); a.Max = Math.Max(a.Max, sz);
    }

    static int Str(string path, string outp, int min)
    {
        var d = File.ReadAllBytes(path);
        using (var w = new StreamWriter(outp, false, new UTF8Encoding(false)))
        {
            var sb = new StringBuilder();
            for (int i = 0; i <= d.Length; i++)
            {
                byte b = i < d.Length ? d[i] : (byte)0;
                if (b >= 32 && b < 127) sb.Append((char)b);
                else { if (sb.Length >= min) w.WriteLine("{0:X8}\tA\t{1}", i - sb.Length, sb); sb.Length = 0; }
            }
            sb.Length = 0;
            for (int i = 0; i + 1 < d.Length; i += 2)
            {
                if (d[i] >= 32 && d[i] < 127 && d[i + 1] == 0) sb.Append((char)d[i]);
                else { if (sb.Length >= min) w.WriteLine("{0:X8}\tW\t{1}", i - sb.Length * 2, sb); sb.Length = 0; }
            }
        }
        return 0;
    }
}

