// Loopback HTTP servers used by the functional harness.
//
// RangeServer  - serves /small (<5MB, no ranges needed) and /large (>5MB,
//                honouring Range: bytes=a-b so sharding can be verified).
//                Also counts ranged GETs so the test can prove sharding
//                actually happened rather than one full-body GET.
// EchoBusServer - framed TCP echo used to drive McpxBus: replies to every
//                request frame with the same JSON, can emit an unsolicited
//                push (seq 0), and can drop live connections on demand.
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

// ============================================================================
//  RangeServer
// ============================================================================
sealed class RangeServer : IDisposable
{
    public static readonly byte[] SmallPayload = BuildSmall();
    public static readonly byte[] LargePayload = BuildLarge();

    static byte[] BuildSmall()
    {
        // ~1.5 MB: comfortably under the 5MB fast-path threshold.
        var b = new byte[1536 * 1024];
        new Random(1234).NextBytes(b);
        return b;
    }

    static byte[] BuildLarge()
    {
        // ~12 MB: comfortably over the threshold so sharding kicks in.
        var b = new byte[12 * 1024 * 1024];
        new Random(5678).NextBytes(b);
        return b;
    }

    static readonly Dictionary<string, int> s_ranges = new Dictionary<string, int>();
    static readonly object s_lock = new object();

    public static int RangeRequestsFor(string path)
    {
        lock (s_lock) { return s_ranges.TryGetValue(path, out var n) ? n : 0; }
    }

    readonly HttpListener _listener = new HttpListener();
    readonly int _port;
    Thread _thread;
    volatile bool _stop;

    public RangeServer(int port)
    {
        _port = port;
        _listener.Prefixes.Add("http://127.0.0.1:" + port + "/");
    }

    public void Start()
    {
        _listener.Start();
        _thread = new Thread(Loop) { IsBackground = true };
        _thread.Start();
    }

    void Loop()
    {
        while (!_stop)
        {
            HttpListenerContext ctx;
            try { ctx = _listener.GetContext(); }
            catch { break; }
            try { Handle(ctx); }
            catch { try { ctx.Response.Abort(); } catch { } }
        }
    }

    void Handle(HttpListenerContext ctx)
    {
        string path = ctx.Request.Url.AbsolutePath;
        byte[] body;
        // "/slow" serves the large payload at a deliberately throttled rate so
        // the resume test can cancel a transfer that is genuinely mid-flight.
        bool slow = path == "/slow";
        if (path == "/small") body = SmallPayload;
        else if (path == "/large" || slow) body = LargePayload;
        else { ctx.Response.StatusCode = 404; ctx.Response.Close(); return; }

        var res = ctx.Response;
        res.Headers["Accept-Ranges"] = "bytes";
        res.Headers["ETag"] = "\"" + body.Length.ToString("x") + "\"";
        res.ContentType = "application/octet-stream";

        // HEAD: advertise the length so the router can route on size alone.
        if (ctx.Request.HttpMethod == "HEAD")
        {
            res.ContentLength64 = body.Length;
            res.StatusCode = 200;
            res.Close();
            return;
        }

        string range = ctx.Request.Headers["Range"];
        if (!string.IsNullOrEmpty(range) && range.StartsWith("bytes="))
        {
            lock (s_lock)
            {
                s_ranges[path] = (s_ranges.TryGetValue(path, out var n) ? n : 0) + 1;
            }

            string spec = range.Substring(6);
            int dash = spec.IndexOf('-');
            long from = 0, to = body.Length - 1;
            if (dash > 0 && spec.Substring(0, dash).Length > 0)
                long.TryParse(spec.Substring(0, dash), out from);
            string tail = dash >= 0 && dash + 1 < spec.Length ? spec.Substring(dash + 1) : "";
            if (tail.Length > 0) long.TryParse(tail, out to);
            if (to >= body.Length) to = body.Length - 1;
            if (from < 0) from = 0;
            if (from > to) { res.StatusCode = 416; res.Close(); return; }

            long len = to - from + 1;
            res.StatusCode = 206;
            res.ContentLength64 = len;
            res.Headers["Content-Range"] = "bytes " + from + "-" + to + "/" + body.Length;
            WriteBody(res, body, (int)from, (int)len, slow);
            res.Close();
            return;
        }

        res.StatusCode = 200;
        res.ContentLength64 = body.Length;
        WriteBody(res, body, 0, body.Length, slow);
        res.Close();
    }

    // Stream the slice, optionally pacing it so the client stays mid-transfer.
    static void WriteBody(HttpListenerResponse res, byte[] body, int from, int len, bool slow)
    {
        const int CHUNK = 64 * 1024;
        int off = from, left = len;
        while (left > 0)
        {
            int n = Math.Min(CHUNK, left);
            try { res.OutputStream.Write(body, off, n); res.OutputStream.Flush(); }
            catch { return; }
            off += n; left -= n;
            if (slow) Thread.Sleep(25);
        }
    }

    public void Dispose()
    {
        _stop = true;
        try { _listener.Stop(); } catch { }
        try { _listener.Close(); } catch { }
    }
}

// ============================================================================
//  EchoBusServer - framed TCP peer for McpxBus
// ============================================================================
sealed class EchoBusServer : IDisposable
{
    readonly TcpListener _listener;
    readonly int _port;
    readonly List<TcpClient> _clients = new List<TcpClient>();
    readonly object _lock = new object();
    volatile bool _stop;

    public EchoBusServer(int port)
    {
        _port = port;
        _listener = new TcpListener(IPAddress.Loopback, port);
    }

    public void Start()
    {
        _listener.Start();
        Task.Run(AcceptLoop);
    }

    async Task AcceptLoop()
    {
        while (!_stop)
        {
            TcpClient c;
            try { c = await _listener.AcceptTcpClientAsync().ConfigureAwait(false); }
            catch { break; }
            lock (_lock) { _clients.Add(c); }
            _ = Task.Run(() => Serve(c));
        }
    }

    async Task Serve(TcpClient c)
    {
        try
        {
            var s = c.GetStream();
            var hdr = new byte[4];
            while (!_stop)
            {
                if (!await ReadExact(s, hdr, 4).ConfigureAwait(false)) break;
                uint ulen = ((uint)hdr[0] << 24) | ((uint)hdr[1] << 16) | ((uint)hdr[2] << 8) | hdr[3];
                if (ulen == 0) continue;                    // heartbeat
                if (ulen > 16 * 1024 * 1024) break;
                var payload = new byte[ulen];
                if (!await ReadExact(s, payload, (int)ulen).ConfigureAwait(false)) break;

                // Reply with [seq][json]: echo the request's seq back so the
                // bus can match the ack to its pending request. This mirrors
                // the real MCP bus protocol (seq 0 == unsolicited push).
                if (ulen >= 8)
                {
                    await WriteFrame(s, payload).ConfigureAwait(false);
                }
            }
        }
        catch { }
    }

    static async Task<bool> ReadExact(Stream s, byte[] buf, int len)
    {
        int off = 0;
        while (off < len)
        {
            int n = await s.ReadAsync(buf, off, len - off).ConfigureAwait(false);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }

    static async Task WriteFrame(Stream s, byte[] payload)
    {
        var frame = new byte[4 + payload.Length];
        frame[0] = (byte)(payload.Length >> 24);
        frame[1] = (byte)(payload.Length >> 16);
        frame[2] = (byte)(payload.Length >> 8);
        frame[3] = (byte)payload.Length;
        Buffer.BlockCopy(payload, 0, frame, 4, payload.Length);
        await s.WriteAsync(frame, 0, frame.Length).ConfigureAwait(false);
        await s.FlushAsync().ConfigureAwait(false);
    }

    // Unsolicited server push: seq 0 followed by JSON (no pending request).
    public void Push(string json)
    {
        var body = Encoding.UTF8.GetBytes(json);
        var payload = new byte[8 + body.Length];
        // seq 0 in the first 8 big-endian bytes
        Buffer.BlockCopy(body, 0, payload, 8, body.Length);
        lock (_lock)
        {
            foreach (var c in _clients)
            {
                try { WriteFrame(c.GetStream(), payload).GetAwaiter().GetResult(); }
                catch { }
            }
        }
    }

    // Simulate a network drop so the bus's reconnect path is exercised.
    public void KillConnections()
    {
        lock (_lock)
        {
            foreach (var c in _clients) { try { c.Close(); } catch { } }
            _clients.Clear();
        }
    }

    public void Dispose()
    {
        _stop = true;
        KillConnections();
        try { _listener.Stop(); } catch { }
    }
}
