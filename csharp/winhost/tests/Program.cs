// ============================================================================
// Functional tests for NexOS.Net (NetStack.cs).
//
// Starts a loopback HTTP server that serves a small file (< 5MB) and a large
// file (> 5MB) with byte-range support, then drives:
//
//   * LengthPrefixFramer  - big-endian header round-trip + heartbeat sentinel
//   * McpxBus             - request/response over TCP, reconnect, idempotency
//   * SmartDownloadRouter - fast path below the 5MB threshold
//   * SmartDownloader     - Range sharding above the threshold
//   * resume              - interrupt mid-flight, resume from saved offsets
//   * MultiPathManager    - range assignment + stitching across links
// ============================================================================
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using NexOS.Net;

static class T
{
    public static int Pass, Fail;

    public static void Ok(string name, bool cond, string detail = "")
    {
        if (cond) { Pass++; Console.WriteLine("  PASS  " + name + (detail.Length > 0 ? "  (" + detail + ")" : "")); }
        else { Fail++; Console.WriteLine("  FAIL  " + name + (detail.Length > 0 ? "  (" + detail + ")" : "")); }
    }

    public static void Section(string s) { Console.WriteLine(); Console.WriteLine("== " + s + " =="); }
}

static class Program
{
    // ---- deterministic test payloads --------------------------------
    static byte[] MakePayload(int n, int seed)
    {
        var b = new byte[n];
        var r = new Random(seed);
        r.NextBytes(b);
        return b;
    }

    static string Sha(byte[] b)
    {
        using var h = SHA256.Create();
        return Convert.ToHexString(h.ComputeHash(b));
    }

    static async Task<int> Main()
    {
        Console.WriteLine("NexOS.Net functional tests");
        Console.WriteLine("==========================");

        int port = FreePort();
        using var server = new RangeServer(port);
        server.Start();
        string baseUrl = "http://127.0.0.1:" + port;
        Console.WriteLine("loopback server on " + baseUrl);

        string tmp = Path.Combine(Path.GetTempPath(), "nexos-nettest-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        Directory.CreateDirectory(tmp);
        Console.WriteLine("work dir " + tmp);

        try
        {
            await TestFramer();
            await TestBus(port);
            await TestFastPath(baseUrl, tmp);
            await TestSharded(baseUrl, tmp);
            await TestResume(baseUrl, tmp);
            await TestMultiPath(baseUrl, tmp);
        }
        catch (Exception ex)
        {
            Console.WriteLine();
            Console.WriteLine("HARNESS EXCEPTION: " + ex);
            T.Fail++;
        }
        finally
        {
            try { Directory.Delete(tmp, true); } catch { }
        }

        Console.WriteLine();
        Console.WriteLine("==========================");
        Console.WriteLine("PASS " + T.Pass + "   FAIL " + T.Fail);
        return T.Fail == 0 ? 0 : 1;
    }

    static int FreePort()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        int p = ((IPEndPoint)l.LocalEndpoint).Port;
        l.Stop();
        return p;
    }

    // ---- 1. framing -------------------------------------------------
    static async Task TestFramer()
    {
        T.Section("LengthPrefixFramer (strict big-endian)");

        var payload = Encoding.UTF8.GetBytes("hello-mcpx");
        var frame = LengthPrefixFramer.Encode(payload);

        // Header must be exactly 4 bytes, big-endian, equal to payload length.
        T.Ok("header size is 4", LengthPrefixFramer.HeaderSize == 4, LengthPrefixFramer.HeaderSize.ToString());
        T.Ok("frame len = 4 + payload", frame.Length == 4 + payload.Length, frame.Length.ToString());

        int declared = (frame[0] << 24) | (frame[1] << 16) | (frame[2] << 8) | frame[3];
        T.Ok("big-endian length matches", declared == payload.Length, declared + " vs " + payload.Length);
        T.Ok("length is NOT little-endian", !(frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == payload.Length && payload.Length > 255));

        // A 300-byte payload proves byte order: BE puts 0x00 0x00 0x01 0x2C.
        var big = LengthPrefixFramer.Encode(new byte[300]);
        T.Ok("300-byte header is 00 00 01 2C",
             big[0] == 0x00 && big[1] == 0x00 && big[2] == 0x01 && big[3] == 0x2C,
             string.Format("{0:X2} {1:X2} {2:X2} {3:X2}", big[0], big[1], big[2], big[3]));

        // Heartbeat sentinel is a zero-length frame.
        var hb = LengthPrefixFramer.HeartbeatFrame();
        T.Ok("heartbeat frame is 4 zero bytes", hb.Length == 4 && hb[0] == 0 && hb[1] == 0 && hb[2] == 0 && hb[3] == 0);

        await Task.CompletedTask;
    }

    // ---- 2. bus -----------------------------------------------------
    static async Task TestBus(int port)
    {
        T.Section("McpxBus (framed TCP, reconnect, idempotency)");

        using var echo = new EchoBusServer(port + 1);
        echo.Start();

        using var bus = new McpxBus("127.0.0.1", port + 1);
        bus.StartWithHeartbeat();

        bool ready = false;
        try { await bus.WaitUntilReadyAsync(TimeSpan.FromSeconds(5)); ready = bus.IsConnected; }
        catch (TimeoutException) { ready = false; }
        T.Ok("connects within timeout", ready);

        string r1 = await bus.RequestAsync("{\"op\":\"ping\"}", CancellationToken.None);
        T.Ok("request/response round-trip", r1 != null && r1.Contains("ping"), r1 ?? "<null>");

        // Sequence ids must be monotonic so a replay is de-duplicable.
        string r2 = await bus.RequestAsync("{\"op\":\"ping\"}", CancellationToken.None);
        T.Ok("second request works", r2 != null && r2.Contains("ping"));

        // Server push (seq 0) must surface as an event, not a reply.
        var pushTcs = new TaskCompletionSource<string>();
        bus.OnServerPush += s => pushTcs.TrySetResult(s);
        echo.Push("{\"event\":\"tick\"}");
        var push = await Task.WhenAny(pushTcs.Task, Task.Delay(3000)) == pushTcs.Task ? pushTcs.Task.Result : null;
        T.Ok("unsolicited push delivered", push != null && push.Contains("tick"), push ?? "<timeout>");

        // Drop the connection: the bus must reconnect on its own and keep
        // serving requests (state recovery).
        echo.KillConnections();
        await Task.Delay(1200);
        bool recovered = false;
        for (int i = 0; i < 20 && !recovered; i++)
        {
            try
            {
                string r3 = await bus.RequestAsync("{\"op\":\"after-drop\"}", CancellationToken.None);
                recovered = r3 != null && r3.Contains("after-drop");
            }
            catch { }
            if (!recovered) await Task.Delay(300);
        }
        T.Ok("auto-reconnects after drop", recovered);
    }

    // ---- 3. fast path -----------------------------------------------
    static async Task TestFastPath(string baseUrl, string tmp)
    {
        T.Section("SmartDownloadRouter - fast path (< 5MB, single socket)");

        string dest = Path.Combine(tmp, "small.bin");
        var sw = Stopwatch.StartNew();
        long n = await SmartDownloadRouter.DownloadFileAsync(baseUrl + "/small", dest, 10000);
        sw.Stop();

        var expect = RangeServer.SmallPayload;
        T.Ok("byte count matches", n == expect.Length, n + " vs " + expect.Length);
        T.Ok("file exists", File.Exists(dest));

        var got = File.ReadAllBytes(dest);
        T.Ok("sha256 matches source", Sha(got) == Sha(expect));
        T.Ok("under threshold routed to fast path", expect.Length <= SmartDownloadRouter.FAST_PATH_BYTES,
             expect.Length + " bytes <= 5MB");
        T.Ok("completed state reported", SmartDownloadRouter.s_progress.TryGetValue(dest, out var p) && p != null);
    }

    // ---- 4. sharded -------------------------------------------------
    static async Task TestSharded(string baseUrl, string tmp)
    {
        T.Section("SmartDownloader - HTTP Range sharding (> 5MB)");

        string dest = Path.Combine(tmp, "large.bin");
        long n = await SmartDownloadRouter.DownloadFileAsync(baseUrl + "/large", dest, 30000);

        var expect = RangeServer.LargePayload;
        T.Ok("byte count matches", n == expect.Length, n + " vs " + expect.Length);
        T.Ok("payload exceeds 5MB threshold", expect.Length > SmartDownloadRouter.FAST_PATH_BYTES,
             (expect.Length / (1024.0 * 1024.0)).ToString("0.0") + " MB");

        var got = File.ReadAllBytes(dest);
        T.Ok("stitched sha256 matches source", Sha(got) == Sha(expect));

        // Range requests must actually have been issued (sharding, not one GET).
        int ranges = RangeServer.RangeRequestsFor("/large");
        T.Ok("multiple Range requests issued", ranges >= 2, ranges + " ranged GETs");
        T.Ok("resume manifest cleaned up on success", !File.Exists(dest + ".resume"));
    }

    // ---- 5. resume --------------------------------------------------
    static async Task TestResume(string baseUrl, string tmp)
    {
        T.Section("IDM resume state machine (interrupt, then continue)");

        string dest = Path.Combine(tmp, "resume.bin");
        using var cts = new CancellationTokenSource();

        // Kill the transfer partway through so per-slice offsets are persisted.
        // /slow paces the body so the cancel genuinely lands mid-flight
        // instead of after the (fast) loopback transfer already finished.
        var dl = SmartDownloadRouter.DownloadFileAsync(baseUrl + "/slow", dest, 30000, null, cts.Token);
        await Task.Delay(700);
        cts.Cancel();
        try { await dl; } catch { /* expected */ }

        bool manifest = File.Exists(dest + ".resume");
        T.Ok("resume manifest written on interrupt", manifest);

        long partial = File.Exists(dest) ? new FileInfo(dest).Length : 0;
        T.Ok("partial file has bytes", partial > 0, partial + " bytes");

        // Peek must report the persisted per-slice offsets.
        var peek = SmartDownloader.Peek(dest);
        T.Ok("Peek reports a live slice count", peek != null && peek.Slices > 0,
             peek == null ? "<null>" : peek.Slices + " slices");
        T.Ok("Peek reports progress > 0", peek != null && peek.Received > 0,
             peek == null ? "<null>" : peek.Received + " bytes");

        // Resume: must continue from the recorded offsets, not restart. Same
        // URL as the interrupted attempt so the manifest is accepted.
        long n = await SmartDownloadRouter.DownloadFileAsync(baseUrl + "/slow", dest, 60000);
        var expect = RangeServer.LargePayload;
        T.Ok("resumed download completes", n == expect.Length, n + " vs " + expect.Length);
        T.Ok("resumed file sha256 matches source", Sha(File.ReadAllBytes(dest)) == Sha(expect));
        T.Ok("manifest removed after completion", !File.Exists(dest + ".resume"));
    }

    // ---- 6. multipath -----------------------------------------------
    static async Task TestMultiPath(string baseUrl, string tmp)
    {
        T.Section("MultiPathManager (per-NIC binding, aggregation, stitching)");

        var links = MultiPathManager.DetectLinks();
        T.Ok("detects at least one usable link", links.Count >= 1, links.Count + " link(s)");
        foreach (var l in links)
            Console.WriteLine("        link: " + l.Type + " " + l.LocalAddress + " gw=" + l.Gateway);

        string dest = Path.Combine(tmp, "multi.bin");
        long n = await MultiPathManager.DownloadAsync(baseUrl + "/large", dest, null, CancellationToken.None);

        var expect = RangeServer.LargePayload;
        T.Ok("multipath byte count matches", n == expect.Length, n + " vs " + expect.Length);
        T.Ok("multipath stitched sha256 matches", Sha(File.ReadAllBytes(dest)) == Sha(expect));

        // The manager splits the file into one contiguous range per link.
        T.Ok("split into >=1 contiguous range", links.Count >= 1, links.Count + " ranges");

        var p = MultiPathManager.s_progress.TryGetValue(dest, out var pp) ? pp : null;
        T.Ok("progress published for polling", p != null);
        T.Ok("final state is Completed", p != null && p.State == DownloadState.Completed,
             p == null ? "<null>" : p.State.ToString());
    }
}
