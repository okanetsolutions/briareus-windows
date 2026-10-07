// Test runner only: bounded stdout/stderr, cancellation deadline, no extraction to disk.
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;

public static class ToolProbe {
    public sealed class Result {
        public int Exit;
        public long StdoutBytes;
        public string Stderr;
        public bool Killed;
    }
    private sealed class Capture {
        public Stream Input;
        public long Total;
        public string Text;
        public bool Failed;
        public void Read() {
            try {
                byte[] buffer = new byte[4096];
                using (MemoryStream kept = new MemoryStream()) {
                    int count;
                    while ((count = Input.Read(buffer, 0, buffer.Length)) > 0) {
                        Interlocked.Add(ref Total, count);
                        int retain = (int)Math.Min(count, 65536 - kept.Length);
                        if (retain > 0) kept.Write(buffer, 0, retain);
                    }
                    Text = Encoding.UTF8.GetString(kept.ToArray());
                }
            } catch (IOException) { Failed = true; }
        }
    }
    public static Result Run(string exe, string args) {
        ProcessStartInfo info = new ProcessStartInfo(exe, args);
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        using (Process p = new Process()) {
            p.StartInfo = info;
            p.Start();
            Capture stdout = new Capture { Input = p.StandardOutput.BaseStream };
            Capture stderr = new Capture { Input = p.StandardError.BaseStream };
            Thread a = new Thread(stdout.Read), b = new Thread(stderr.Read);
            a.Start(); b.Start();
            Stopwatch timer = Stopwatch.StartNew();
            bool killed = false;
            while (!p.WaitForExit(10)) {
                if (timer.ElapsedMilliseconds > 30000 || Interlocked.Read(ref stdout.Total) > 65536 || Interlocked.Read(ref stderr.Total) > 65536) {
                    p.Kill(); killed = true; break;
                }
            }
            p.WaitForExit(); a.Join(); b.Join();
            return new Result { Exit = p.ExitCode, StdoutBytes = stdout.Total, Stderr = stderr.Text,
                                Killed = killed || stdout.Failed || stderr.Failed };
        }
    }
}
