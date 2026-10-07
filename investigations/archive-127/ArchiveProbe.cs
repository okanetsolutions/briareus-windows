// Investigation only. Never use an ACCEPT result as a production capability gate:
// GZipStream does not expose a strict compressed-stream completion/consumption flag.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Threading;

public static class ArchiveProbe {
    public const long CompressedLimit = 300L * 1024 * 1024;
    public const long RawLimit = 512L * 1024 * 1024;
    public const long IndexLimit = 192L * 1024 * 1024;
    public const long FileLimit = 1024L * 1024;
    public const int FileCountLimit = 5000;
    public const int HeaderLimit = 100000;
    private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
    private static readonly uint[] CrcTable = MakeCrcTable();

    private static uint[] MakeCrcTable() {
        uint[] result = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint value = i;
            for (int bit = 0; bit < 8; bit++) value = (value >> 1) ^ ((value & 1) != 0 ? 0xedb88320u : 0u);
            result[i] = value;
        }
        return result;
    }

    public sealed class Member {
        public string Path;
        public long Offset;
        public long Size;
    }

    private static void CheckCancel(string cancel) {
        if (!String.IsNullOrEmpty(cancel) && File.Exists(cancel))
            throw new OperationCanceledException("cancelled");
    }

    // Simulates a response body using a source stream. No full-body allocation;
    // supplied Content-Length is checked independently from observed byte count.
    public static long CopyCompressed(Stream source, string output, long declaredLength,
                                      long limit, string cancel) {
        if (declaredLength > limit) throw new InvalidDataException("compressed declared limit");
        long total = 0;
        using (FileStream dest = new FileStream(output, FileMode.CreateNew, FileAccess.Write)) {
            byte[] buffer = new byte[65536];
            for (;;) {
                CheckCancel(cancel);
                int n = source.Read(buffer, 0, buffer.Length);
                if (n == 0) break;
                if (n > limit - total) throw new InvalidDataException("compressed streaming limit");
                if (declaredLength >= 0 && n > declaredLength - total) throw new InvalidDataException("compressed length mismatch");
                dest.Write(buffer, 0, n);
                total += n;
            }
        }
        if (declaredLength >= 0 && total != declaredLength) throw new InvalidDataException("compressed truncated response body");
        return total;
    }

    // A fixed-size buffer and subtraction checks bound every write, including skipped files.
    // Trailer CRC/ISIZE adds checks, but is NOT proof of DEFLATE termination or no trailing input.
    public static long Inflate(string input, string output, long compressedLimit,
                               long rawLimit, string cancel, int delayMs) {
        using (FileStream source = new FileStream(input, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            if (source.Length > compressedLimit) throw new InvalidDataException("compressed limit");
            if (source.Length < 18) throw new InvalidDataException("gzip too short");
            byte[] header = Read(source, 10);
            if (header[0] != 31 || header[1] != 139 || header[2] != 8 || header[3] != 0)
                throw new InvalidDataException("unsupported gzip header (fallback)");
            source.Position = source.Length - 8;
            byte[] trailer = Read(source, 8);
            source.Position = 0;
            uint crc = 0xffffffff;
            long total = 0;
            using (GZipStream gzip = new GZipStream(source, CompressionMode.Decompress, true))
            using (FileStream dest = new FileStream(output, FileMode.CreateNew, FileAccess.Write)) {
                byte[] buffer = new byte[65536];
                for (;;) {
                    CheckCancel(cancel);
                    int n = gzip.Read(buffer, 0, buffer.Length);
                    if (n == 0) break;
                    if (n > rawLimit - total) throw new InvalidDataException("decompressed limit");
                    for (int i = 0; i < n; i++) crc = (crc >> 8) ^ CrcTable[(crc ^ buffer[i]) & 255];
                    dest.Write(buffer, 0, n);
                    total += n;
                    if (delayMs > 0) Thread.Sleep(delayMs);
                }
            }
            if ((crc ^ 0xffffffff) != Little32(trailer, 0) || (uint)total != Little32(trailer, 4))
                throw new InvalidDataException("gzip trailer CRC/ISIZE");
            return total;
        }
    }

    private static uint Little32(byte[] b, int at) {
        return (uint)b[at] | (uint)b[at + 1] << 8 | (uint)b[at + 2] << 16 | (uint)b[at + 3] << 24;
    }

    private static byte[] Read(Stream s, int count) {
        byte[] b = new byte[count];
        int at = 0;
        while (at < count) {
            int n = s.Read(b, at, count - at);
            if (n == 0) throw new InvalidDataException("truncated tar/gzip field");
            at += n;
        }
        return b;
    }

    private static bool Zero(byte[] b) {
        foreach (byte c in b) if (c != 0) return false;
        return true;
    }

    private static string Text(byte[] b, int at, int count) {
        int end = at;
        while (end < at + count && b[end] != 0) end++;
        for (int i = end; i < at + count; i++)
            if (b[i] != 0) throw new InvalidDataException("tar embedded NUL");
        return Utf8.GetString(b, at, end - at);
    }

    private static long Octal(byte[] b, int at, int count) {
        long value = 0;
        bool ended = false, digit = false;
        for (int i = at; i < at + count; i++) {
            byte c = b[i];
            if (c == 0 || c == 32) { if (digit) ended = true; continue; }
            if (ended || c < 48 || c > 55) throw new InvalidDataException("unsupported tar number");
            if (value > (Int64.MaxValue - 7) / 8) throw new InvalidDataException("tar number overflow");
            value = value * 8 + c - 48;
            digit = true;
        }
        return value;
    }

    // Validate raw AND effective PAX paths. No archive path is ever passed to the filesystem.
    public static string SafePath(string path, bool directory) {
        if (directory && path.EndsWith("/", StringComparison.Ordinal)) path = path.Substring(0, path.Length - 1);
        if (path.Length == 0 || Utf8.GetByteCount(path) > 1024)
            throw new InvalidDataException("path length");
        foreach (char c in path)
            if (c < 32 || c == 127 || "\\:<>\"|?*".IndexOf(c) >= 0)
                throw new InvalidDataException("unsafe path character");
        foreach (string part in path.Split('/')) {
            if (part.Length == 0 || part == "." || part == ".." || part.EndsWith(".") || part.EndsWith(" "))
                throw new InvalidDataException("unsafe path component");
            string name = part.Split('.')[0].ToUpperInvariant();
            if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" || name == "CONIN$" || name == "CONOUT$" ||
                (name.Length == 4 && (name.StartsWith("COM") || name.StartsWith("LPT")) && "123456789\u00b9\u00b2\u00b3".IndexOf(name[3]) >= 0))
                throw new InvalidDataException("Windows device path");
        }
        return path;
    }

    private static Dictionary<string, string> Pax(byte[] b) {
        Dictionary<string, string> result = new Dictionary<string, string>(StringComparer.Ordinal);
        int at = 0;
        while (at < b.Length) {
            int space = at;
            while (space < b.Length && b[space] != 32) space++;
            if (space == b.Length || space == at) throw new InvalidDataException("PAX length");
            int length = 0;
            for (int i = at; i < space; i++) {
                if (b[i] < 48 || b[i] > 57 || length > 65536 / 10) throw new InvalidDataException("PAX length");
                length = length * 10 + b[i] - 48;
            }
            if (length <= space - at + 2 || length > b.Length - at || b[at + length - 1] != 10)
                throw new InvalidDataException("PAX record");
            string record = Utf8.GetString(b, space + 1, at + length - space - 2);
            int eq = record.IndexOf('=');
            if (eq < 1 || result.ContainsKey(record.Substring(0, eq))) throw new InvalidDataException("PAX key");
            result.Add(record.Substring(0, eq), record.Substring(eq + 1));
            at += length;
        }
        return result;
    }

    // Preflight the WHOLE bounded tar before the first candidate file is created.
    // `selected` must be the existing indexer's canonical tree-selected queue.
    public static Member[] ValidateTar(string input, string[] selected, long fileLimit,
                                      long indexLimit, int fileCountLimit, string cancel) {
        HashSet<string> wanted = new HashSet<string>(selected, StringComparer.Ordinal);
        HashSet<string> seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, bool> types = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
        List<Member> accepted = new List<Member>();
        string root = null, pendingPath = null;
        long kept = 0;
        int headers = 0;
        using (FileStream s = new FileStream(input, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            if (s.Length > RawLimit) throw new InvalidDataException("decompressed limit");
            for (;;) {
                CheckCancel(cancel);
                byte[] b = Read(s, 512);
                if (Zero(b)) {
                    if (!Zero(Read(s, 512))) throw new InvalidDataException("second tar end marker");
                    if (pendingPath != null) throw new InvalidDataException("orphan PAX");
                    while (s.Position < s.Length) {
                        CheckCancel(cancel);
                        if (!Zero(Read(s, (int)Math.Min(65536, s.Length - s.Position))))
                            throw new InvalidDataException("nonzero tar trailing data");
                    }
                    if (s.Length % 512 != 0) throw new InvalidDataException("unaligned tar end");
                    break;
                }
                if (++headers > HeaderLimit) throw new InvalidDataException("header count limit");
                long checksum = Octal(b, 148, 8), actual = 0;
                for (int i = 0; i < 512; i++) actual += (i >= 148 && i < 156) ? 32 : b[i];
                if (actual != checksum) throw new InvalidDataException("tar checksum");
                if (Text(b, 257, 6) != "ustar") throw new InvalidDataException("unsupported tar format");
                long size = Octal(b, 124, 12);
                long padded = size + (512 - size % 512) % 512;
                if (padded < size || padded > s.Length - s.Position) throw new InvalidDataException("truncated tar payload");
                char type = (char)b[156];
                string name = Text(b, 0, 100), prefix = Text(b, 345, 155);
                if (prefix.Length != 0) name = prefix + "/" + name;
                name = SafePath(name, type == '5');
                if (Text(b, 157, 100).Length != 0) throw new InvalidDataException("link target");
                if (type == 'g' || type == 'x') {
                    if (pendingPath != null || size > 65536) throw new InvalidDataException("PAX state/limit");
                    Dictionary<string, string> fields = Pax(Read(s, (int)size));
                    foreach (KeyValuePair<string, string> field in fields) {
                        if (type == 'g' && field.Key == "comment") continue;
                        if (type == 'x' && field.Key == "path") { pendingPath = SafePath(field.Value, field.Value.EndsWith("/")); continue; }
                        // Conservative fallback: no GNU longlink/sparse or PAX size/link overrides.
                        throw new InvalidDataException("unsupported PAX field: " + field.Key);
                    }
                    if (!Zero(Read(s, (int)(padded - size)))) throw new InvalidDataException("PAX padding");
                    continue;
                }
                if (type != '0' && type != '\0' && type != '5') throw new InvalidDataException("link/special/unsupported tar entry");
                if (type == '5' && size != 0) throw new InvalidDataException("directory data");
                if (pendingPath != null) { name = SafePath(pendingPath, type == '5'); pendingPath = null; }
                string[] parts = name.Split('/');
                if (root == null) root = parts[0];
                if (root != parts[0] || (parts.Length == 1 && type != '5')) throw new InvalidDataException("GitHub root");
                if (!seen.Add(name)) throw new InvalidDataException("duplicate/case collision");
                bool isDir = type == '5';
                for (int slash = name.IndexOf('/'); slash >= 0; slash = name.IndexOf('/', slash + 1)) {
                    bool parentDir;
                    if (types.TryGetValue(name.Substring(0, slash), out parentDir) && !parentDir)
                        throw new InvalidDataException("file used as parent");
                }
                if (!isDir) foreach (string existing in types.Keys)
                    if (existing.StartsWith(name + "/", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("file replaces parent");
                types.Add(name, isDir);
                string relative = parts.Length > 1 ? name.Substring(root.Length + 1) : "";
                if (!isDir && wanted.Contains(relative)) {
                    if (size > fileLimit || size > indexLimit - kept || accepted.Count >= fileCountLimit)
                        throw new InvalidDataException("file/index/count limit");
                    accepted.Add(new Member { Path = relative, Offset = s.Position, Size = size });
                    kept += size;
                }
                s.Position += size;
                if (!Zero(Read(s, (int)(padded - size)))) throw new InvalidDataException("tar payload padding");
            }
        }
        if (accepted.Count != wanted.Count) throw new InvalidDataException("selected tree path missing: canonical per-file fallback");
        return accepted.ToArray();
    }

    public static void Stage(string tar, string stage, Member[] members, string cancel) {
        Directory.CreateDirectory(stage);
        using (FileStream source = new FileStream(tar, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            byte[] buffer = new byte[65536];
            for (int i = 0; i < members.Length; i++) {
                Member m = members[i];
                source.Position = m.Offset;
                // Generated numeric keys avoid archive path aliases and reparse-point traversal.
                using (FileStream dest = new FileStream(Path.Combine(stage, i.ToString("D5") + ".blob"), FileMode.CreateNew, FileAccess.Write)) {
                    long remaining = m.Size;
                    while (remaining > 0) {
                        CheckCancel(cancel);
                        int n = source.Read(buffer, 0, (int)Math.Min(remaining, buffer.Length));
                        if (n == 0) throw new InvalidDataException("stage truncated");
                        dest.Write(buffer, 0, n);
                        remaining -= n;
                    }
                }
            }
        }
    }
}
