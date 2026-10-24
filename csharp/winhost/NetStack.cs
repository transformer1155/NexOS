// ============================================================================
// NexOS WinHost Network Stack// ----------------------------------------------------------------------------
// Three low-level infrastructure subsystems requested by the kernel network
// architect:
//
//   1. McpxBus        - MCP bus anti-sticky-packet transport base.
//                       Strict BIG-ENDIAN Length-Prefix framing over TCP,
//                       heartbeat keepalive, auto-reconnect, and idempotent
//                       state recovery (every request carries a monotonic
//                       seq id; on re-connect the outbound queue is replayed
//                       and duplicate inbound frames are dropped).
//
//   2. SmartDownloader / SmartDownloadRouter
//                       - Host.DownloadFile(url, dest, timeoutMs)
//                       - Files < 5MB take a single-socket fast path.
//                       - Files > 5MB are routed to SmartDownloader which uses
//                         HTTP Range dynamic sharding plus an IDM-style resume
//                         state machine that persists per-slice Offset so an
//                         interrupted download resumes exactly where it left.
//
//   3. MultiPathManager
//                       - Host.DownloadAsync(url, dest) (MCP tool download_async)
//                       - Auto-detects / binds every usable WiFi + NIC socket,
//                         assigns contiguous byte ranges to each path, fetches
//                         in parallel, and stitches the slices. This bypasses
//                         TCP head-of-line blocking and aggregates the physical
//                         bandwidth of all links.
//
// All real I/O lives here in WinHost (.NET 8, full async). The kernel side
// only declares [InternalCall] signatures (see NexOS.Forms/Forms.cs) that the
// MiniCLR runtime resolves to these managed implementations.
// ============================================================================

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

// The rest of the harness compiles with Nullable=enable, but these sources
// follow the kernel's un-annotated style; keep the file warning-clean.
#nullable disable

namespace NexOS.Net
{
    // ========================================================================
    // 1. Big-endian Length-Prefix framing (anti sticky-packet base)
    // ========================================================================
    // Wire format (all multi-byte integers are network / big-endian):
    //   [4 bytes] u32 payload length N  (NOT including the header)
    //   [N bytes] opaque payload (UTF-8 JSON for the MCP bus)
    // A length of 0 is reserved for the heartbeat/pong frame so that a keep
    // alive never collides with a real message yet still counts as a valid
    // frame that exercises the read loop.
    internal static class LengthPrefixFramer
    {
        public const int HeaderSize = 4;
        public const int MaxFrame = 16 * 1024 * 1024; // 16 MB hard ceiling
        public const int HeartbeatLength = 0;          // N == 0 => heartbeat

        public static byte[] Encode(ReadOnlySpan<byte> payload, int lengthMarker = -1)
        {
            int n = lengthMarker < 0 ? payload.Length : lengthMarker;
            if (n > MaxFrame) throw new ArgumentOutOfRangeException(nameof(payload), "frame too large");
            byte[] frame = new byte[HeaderSize + payload.Length];
            frame[0] = (byte)((n >> 24) & 0xFF);
            frame[1] = (byte)((n >> 16) & 0xFF);
            frame[2] = (byte)((n >> 8) & 0xFF);
            frame[3] = (byte)(n & 0xFF);
            if (payload.Length > 0)
                payload.CopyTo(frame.AsSpan(HeaderSize));
            return frame;
        }

        public static byte[] HeartbeatFrame()
        {
            // Length 0 frame.
            return new byte[] { 0, 0, 0, 0 };
        }
    }

    // ========================================================================
    // 1b. McpxBus - resilient MCP transport
    // ========================================================================
    internal sealed class McpxBus : IDisposable
    {
        private readonly string _host;
        private readonly int _port;
        private readonly TimeSpan _heartbeat;
        private readonly TimeSpan _connectTimeout;
        private readonly TimeSpan _reconnectDelay;
        private readonly TimeSpan _replyTimeout = TimeSpan.FromSeconds(30);

        private TcpClient _client;
        private Stream _stream;
        private readonly CancellationTokenSource _cts = new CancellationTokenSource();
        private Task _pump;
        private Task _reconnectTask;

        // Idempotency bookkeeping.
        private long _nextSeq = 1;                       // monotonic request id
        private readonly ConcurrentDictionary<long, PendingReq> _pending = new();
        private readonly ConcurrentQueue<Outbound> _outbox = new(); // replay queue
        private readonly HashSet<long> _seenAcks = new();            // dedup window
        private long _lastSeenAck = 0;

        private int _connected;  // 0/1
        private int _disposed;

        // ---- observability (net_status) -------------------------------
        // Heartbeat round-trip latency, sampled on every heartbeat tick so an
        // AI agent can tell "connected but the link is dying" from "healthy".
        private long _lastRttMs = -1;          // -1 == no sample yet
        private long _minRttMs = -1;
        private long _maxRttMs = -1;
        private long _rttSamples;
        private long _lastHeartbeatAtMs = -1;  // Environment.TickCount of last tick
        private long _connectCount;            // successful (re)connects
        private long _reconnectCount;          // reconnects after the first
        private long _heartbeatsSent;
        private long _heartbeatsMissed;        // ticks with no pong by next tick
        private long _connectedSinceMs = -1;

        private readonly object _sync = new object();
        private readonly ManualResetEventSlim _ready = new ManualResetEventSlim(false);

        public event Action OnConnected;
        public event Action OnDisconnected;

        public bool IsConnected => _connected == 1;

        private sealed class PendingReq
        {
            public long Seq;
            public TaskCompletionSource<string> Tcs;
            public CancellationTokenRegistration Reg;
        }

        private sealed class Outbound
        {
            public long Seq;
            public byte[] Frame;
            public PendingReq Pending;
        }

        public McpxBus(string host, int port,
                       TimeSpan? heartbeat = null,
                       TimeSpan? connectTimeout = null,
                       TimeSpan? reconnectDelay = null)
        {
            _host = host;
            _port = port;
            _heartbeat = heartbeat ?? TimeSpan.FromSeconds(15);
            _connectTimeout = connectTimeout ?? TimeSpan.FromSeconds(5);
            _reconnectDelay = reconnectDelay ?? TimeSpan.FromSeconds(2);
        }

        public void Start()
        {
            if (_disposed == 1) return;
            _reconnectTask = Task.Run(async () => await ReconnectLoop(_cts.Token));
        }

        public async Task WaitUntilReadyAsync(TimeSpan timeout)
        {
            if (IsConnected) return;
            if (!_ready.Wait(timeout))
                throw new TimeoutException("MCP bus did not connect in time");
        }

        // Request/response with idempotency: caller supplies a seq, the frame is
        // queued, and on reconnect the entire outbox (with the same seqs) is
        // replayed so no in-flight request is lost. Duplicate acks are dropped.
        public async Task<string> RequestAsync(string payload, CancellationToken ct = default)
        {
            long seq = Interlocked.Increment(ref _nextSeq);
            var tcs = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
            var pending = new PendingReq
            {
                Seq = seq,
                Tcs = tcs,
            };
            pending.Reg = ct.Register(() => tcs.TrySetCanceled(ct));
            _pending[seq] = pending;

            byte[] frame = BuildRequestFrame(seq, payload);
            var ob = new Outbound { Seq = seq, Frame = frame, Pending = pending };
            _outbox.Enqueue(ob);

            _ready.Wait(_connectTimeout); // block until a socket exists
            await WriteFrameAsync(frame, ct).ConfigureAwait(false);

            using (pending.Reg)
            using (var timeout = new CancellationTokenSource(_replyTimeout))
            using (var linked = CancellationTokenSource.CreateLinkedTokenSource(ct, timeout.Token))
            using (linked.Token.Register(() =>
                   {
                       _pending.TryRemove(seq, out _);
                       tcs.TrySetCanceled(linked.Token);
                   }))
            {
                return await tcs.Task.ConfigureAwait(false);
            }
        }

        // Wire payload of a request frame is [seq: 8 bytes big-endian][json].
        // The peer echoes the seq back so the reply can be matched to its
        // pending request; on reconnect the same frame is replayed verbatim,
        // which makes the whole exchange idempotent.
        private static byte[] BuildRequestFrame(long seq, string payload)
        {
            byte[] body = Encoding.UTF8.GetBytes(payload ?? "");
            var buf = new byte[8 + body.Length];
            buf[0] = (byte)(seq >> 56);
            buf[1] = (byte)(seq >> 48);
            buf[2] = (byte)(seq >> 40);
            buf[3] = (byte)(seq >> 32);
            buf[4] = (byte)(seq >> 24);
            buf[5] = (byte)(seq >> 16);
            buf[6] = (byte)(seq >> 8);
            buf[7] = (byte)seq;
            Buffer.BlockCopy(body, 0, buf, 8, body.Length);
            return LengthPrefixFramer.Encode(buf);
        }

        // Fire-and-forget (still idempotent / replayed) - used for events.
        public async Task SendAsync(string payload, CancellationToken ct = default)
        {
            long seq = Interlocked.Increment(ref _nextSeq);
            byte[] frame = BuildRequestFrame(seq, payload);
            _outbox.Enqueue(new Outbound { Seq = seq, Frame = frame });
            _ready.Wait(_connectTimeout);
            await WriteFrameAsync(frame, ct).ConfigureAwait(false);
        }

        private async Task ReconnectLoop(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    if (_connected == 0)
                    {
                        await ConnectOnce(token).ConfigureAwait(false);
                    }
                }
                catch (OperationCanceledException) { break; }
                catch (Exception ex)
                {
                    Log("mcpx reconnect failed: " + ex.Message);
                }

                if (_connected == 0 && !token.IsCancellationRequested)
                {
                    try { await Task.Delay(_reconnectDelay, token).ConfigureAwait(false); }
                    catch (OperationCanceledException) { break; }
                }
            }
        }

        private async Task ConnectOnce(CancellationToken token)
        {
            var client = new TcpClient();
            using (var cts = new CancellationTokenSource(_connectTimeout))
            using (var linked = CancellationTokenSource.CreateLinkedTokenSource(token, cts.Token))
            {
                try
                {
                    await client.ConnectAsync(_host, _port, linked.Token).ConfigureAwait(false);
                }
                catch
                {
                    client.Dispose();
                    throw;
                }
            }

            // State recovery: when a new socket is established we replay every
            // still-pending outbound frame. The seq ids are preserved so the
            // peer's idempotent handler will ignore duplicates and the local
            // pending Tcs is still attached to receive the (single) response.
            lock (_sync)
            {
                _client = client;
                _stream = client.GetStream();
                _connected = 1;
                _connectedSinceMs = Environment.TickCount;
            }
            long cc = Interlocked.Increment(ref _connectCount);
            if (cc > 1) Interlocked.Increment(ref _reconnectCount);
            _ready.Set();
            OnConnected?.Invoke();
            Log("mcpx connected " + _host + ":" + _port);

            // Replay outbox (oldest first) so ordering is preserved.
            foreach (var ob in _outbox.ToArray())
            {
                try { await WriteFrameAsync(ob.Frame, token).ConfigureAwait(false); }
                catch { /* pump will detect drop and re-replay later */ break; }
            }

            _pump = Task.Run(async () => await ReadLoop(token));
        }

        private async Task ReadLoop(CancellationToken token)
        {
            var buf = new byte[LengthPrefixFramer.HeaderSize];
            try
            {
                while (!token.IsCancellationRequested)
                {
                    // Heartbeat sender.
                    if (_heartbeat > TimeSpan.Zero)
                    {
                        // (handled by separate timer below; loop focuses on reads)
                    }

                    int n = await ReadExactAsync(buf, 0, buf.Length, token).ConfigureAwait(false);
                    if (n == 0) break; // graceful close

                    // Strict big-endian 4-byte length prefix (see
                    // LengthPrefixFramer). Compute as uint so a hostile
                    // 0xFFFFFFFF header cannot alias a negative length.
                    uint ulen = ((uint)buf[0] << 24) | ((uint)buf[1] << 16) | ((uint)buf[2] << 8) | buf[3];
                    if (ulen > (uint)LengthPrefixFramer.MaxFrame)
                    {
                        // Malformed / desync: tear down so we resync on reconnect.
                        break;
                    }
                    int len = (int)ulen;
                    if (len == LengthPrefixFramer.HeartbeatLength)
                    {
                        // Peer heartbeat / our pong target. Record the RTT (this
                        // is the answer to our last ping) and reply so the peer
                        // knows we are alive too.
                        OnPong();
                        try { await WriteFrameAsync(LengthPrefixFramer.HeartbeatFrame(), token).ConfigureAwait(false); } catch { }
                        continue;
                    }
                    if (len > LengthPrefixFramer.MaxFrame)
                    {
                        // Malformed / desync: tear down so we resync on reconnect.
                        throw new IOException("frame length exceeded ceiling");
                    }

                    byte[] payload = new byte[len];
                    await ReadExactAsync(payload, 0, len, token).ConfigureAwait(false);

                    // Parse [seq][json]; seq omitted for unsolicited server pushes.
                    long ackSeq = 0;
                    string json = "";
                    if (len >= 8)
                    {
                        ackSeq = ((long)payload[0] << 56) | ((long)payload[1] << 48)
                               | ((long)payload[2] << 40) | ((long)payload[3] << 32)
                               | ((long)payload[4] << 24) | ((long)payload[5] << 16)
                               | ((long)payload[6] << 8) | payload[7];
                        json = Encoding.UTF8.GetString(payload, 8, len - 8);
                    }
                    else if (len > 0)
                    {
                        json = Encoding.UTF8.GetString(payload);
                    }

                    if (ackSeq != 0)
                    {
                        // Idempotency: drop duplicate acks.
                        bool isDup;
                        lock (_seenAcks)
                        {
                            isDup = ackSeq <= _lastSeenAck && _seenAcks.Contains(ackSeq);
                            if (!isDup)
                            {
                                if (_seenAcks.Count > 1024) _seenAcks.Clear();
                                _seenAcks.Add(ackSeq);
                                if (ackSeq > _lastSeenAck) _lastSeenAck = ackSeq;
                            }
                        }
                        if (!isDup && _pending.TryRemove(ackSeq, out var pending))
                        {
                            // Remove from replay outbox now that it is satisfied.
                            CompactOutbox(ackSeq);
                            pending.Tcs.TrySetResult(json);
                        }
                    }
                    else
                    {
                        // Unsolicited server push (e.g. inbound MCP call).
                        OnServerPush?.Invoke(json);
                    }
                }
            }
            catch (OperationCanceledException) { }
            catch (Exception ex)
            {
                Log("mcpx read loop ended: " + ex.Message);
            }
            finally
            {
                DropConnection();
            }
        }

        public event Action<string> OnServerPush;

        private void CompactOutbox(long satisfiedSeq)
        {
            // Keep outbox small: drop frames whose seq is <= satisfied and that
            // are no longer pending. Cheap O(n) scan over the small queue.
            var keep = new ConcurrentQueue<Outbound>();
            while (_outbox.TryDequeue(out var ob))
            {
                if (ob.Seq > satisfiedSeq || _pending.ContainsKey(ob.Seq))
                    keep.Enqueue(ob);
            }
            while (keep.TryDequeue(out var ob)) _outbox.Enqueue(ob);
        }

        private async Task WriteFrameAsync(byte[] frame, CancellationToken token)
        {
            Stream s;
            lock (_sync) { s = _stream; }
            if (s == null) throw new IOException("not connected");
            await s.WriteAsync(frame, 0, frame.Length, token).ConfigureAwait(false);
            await s.FlushAsync(token).ConfigureAwait(false);
        }

        private async Task<int> ReadExactAsync(byte[] buffer, int offset, int count, CancellationToken token)
        {
            Stream s;
            lock (_sync) { s = _stream; }
            if (s == null) return 0;
            int total = 0;
            while (total < count)
            {
                int r = await s.ReadAsync(buffer, offset + total, count - total, token).ConfigureAwait(false);
                if (r == 0) return total == 0 ? 0 : total; // peer closed
                total += r;
            }
            return total;
        }

        private void DropConnection()
        {
            bool wasUp = _connected == 1;
            lock (_sync)
            {
                _connected = 0;
                try { _stream?.Dispose(); } catch { }
                try { _client?.Dispose(); } catch { }
                _stream = null;
                _client = null;
            }
            _ready.Reset();
            if (wasUp) OnDisconnected?.Invoke();
        }

        // Heartbeat timer started alongside Start().
        private System.Threading.Timer _hbTimer;
        public void StartWithHeartbeat()
        {
            Start();
            _hbTimer = new System.Threading.Timer(_ => HeartbeatTick(), null,
                                 _heartbeat, _heartbeat);
        }

        private async void HeartbeatTick()
        {
            if (_connected != 1) return;

            // A tick that arrives while the previous ping is still unanswered
            // means the peer is unresponsive on a nominally live socket.
            long now = Environment.TickCount;
            long prev = Interlocked.Read(ref _lastHeartbeatAtMs);
            if (prev > 0 && _pingPending == 1)
                Interlocked.Increment(ref _heartbeatsMissed);

            Interlocked.Exchange(ref _lastHeartbeatAtMs, now);
            Interlocked.Exchange(ref _pingSentAtMs, now);
            _pingPending = 1;
            Interlocked.Increment(ref _heartbeatsSent);

            try { await WriteFrameAsync(LengthPrefixFramer.HeartbeatFrame(), _cts.Token).ConfigureAwait(false); }
            catch { /* read loop will tear down */ }
        }

        // Called from the read loop when a heartbeat/pong frame arrives: the
        // peer is alive, so close out the outstanding ping and record the RTT.
        private void OnPong()
        {
            long sent = Interlocked.Read(ref _pingSentAtMs);
            _pingPending = 0;
            if (sent <= 0) return;
            long rtt = Environment.TickCount - sent;
            if (rtt < 0) rtt = 0;                  // TickCount wraps every ~49 days
            Interlocked.Exchange(ref _lastRttMs, rtt);
            Interlocked.Increment(ref _rttSamples);

            long mn = Interlocked.Read(ref _minRttMs);
            if (mn < 0 || rtt < mn) Interlocked.Exchange(ref _minRttMs, rtt);
            long mx = Interlocked.Read(ref _maxRttMs);
            if (mx < 0 || rtt > mx) Interlocked.Exchange(ref _maxRttMs, rtt);
        }

        // Immutable snapshot for the net_status MCP tool. Kept as a plain
        // struct so the JSON shim in Backend.cs needs no locking.
        public BusStatus GetStatus()
        {
            return new BusStatus
            {
                Host = _host,
                Port = _port,
                Connected = _connected == 1,
                LastRttMs = Interlocked.Read(ref _lastRttMs),
                MinRttMs = Interlocked.Read(ref _minRttMs),
                MaxRttMs = Interlocked.Read(ref _maxRttMs),
                RttSamples = Interlocked.Read(ref _rttSamples),
                HeartbeatsSent = Interlocked.Read(ref _heartbeatsSent),
                HeartbeatsMissed = Interlocked.Read(ref _heartbeatsMissed),
                ConnectCount = Interlocked.Read(ref _connectCount),
                ReconnectCount = Interlocked.Read(ref _reconnectCount),
                UptimeMs = _connected == 1 && _connectedSinceMs >= 0
                           ? Environment.TickCount - _connectedSinceMs
                           : -1,
                PendingRequests = _pending.Count,
                OutboxDepth = _outbox.Count,
            };
        }

        private long _pingSentAtMs = -1;
        private int _pingPending;

        // Diagnostic sink. Defaults to the WinHost Host logger; a test harness
        // can swap it for a console writer without dragging in WinForms.
        public static Action<string> LogSink;
        private void Log(string s)
        {
            var sink = LogSink;
            if (sink != null) { sink(s); return; }
            Console.WriteLine("[mcpx] " + s);
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 1) return;
            _cts.Cancel();
            DropConnection();
            try { _hbTimer?.Dispose(); } catch { }
            try { _reconnectTask?.Wait(TimeSpan.FromSeconds(2)); } catch { }
            _cts.Dispose();
        }
    }

    // Point-in-time view of a McpxBus connection, serialised by the
    // net_status MCP tool. RTT fields are -1 until the first pong arrives.
    public sealed class BusStatus
    {
        public string Host;
        public int Port;
        public bool Connected;
        public long LastRttMs;
        public long MinRttMs;
        public long MaxRttMs;
        public long RttSamples;
        public long HeartbeatsSent;
        public long HeartbeatsMissed;
        public long ConnectCount;
        public long ReconnectCount;
        public long UptimeMs;
        public int PendingRequests;
        public int OutboxDepth;
    }

    // Process-wide MCP bus. WinHost had no bus instance before: the AI/MCP
    // surface went straight to the HTTP gateway, so there was nothing for
    // net_status to report on. This is the single place a live McpxBus is
    // created and owned; call Ensure() from startup to bring it up.
    public static class McpBusHost
    {
        private static McpxBus s_bus;
        private static readonly object s_lock = new object();

        // The framed bus is a separate listener from the HTTP gateway
        // (19387); the bus speaks the length-prefixed protocol on 19388.
        public const string DefaultHost = "127.0.0.1";
        public const int DefaultPort = 19388;

        internal static McpxBus Current
        {
            get { lock (s_lock) { return s_bus; } }
        }

        // Start the bus once. Returns the live instance (or null if the
        // caller passed enabled:false). Safe to call repeatedly.
        internal static McpxBus Ensure(bool enabled = true, string host = null, int port = 0)
        {
            if (!enabled) return null;
            lock (s_lock)
            {
                if (s_bus != null) return s_bus;
                s_bus = new McpxBus(host ?? DefaultHost, port > 0 ? port : DefaultPort);
                s_bus.StartWithHeartbeat();
                return s_bus;
            }
        }

        // Status snapshot for the net_status tool. When the bus was never
        // started we still report a well-formed "not connected" answer
        // instead of failing the call.
        public static BusStatus Status()
        {
            var bus = Current;
            if (bus != null) return bus.GetStatus();
            return new BusStatus
            {
                Host = DefaultHost,
                Port = DefaultPort,
                Connected = false,
                LastRttMs = -1,
                MinRttMs = -1,
                MaxRttMs = -1,
                UptimeMs = -1,
            };
        }

        public static void Shutdown()
        {
            lock (s_lock)
            {
                if (s_bus == null) return;
                try { s_bus.Dispose(); } catch { }
                s_bus = null;
            }
        }
    }

    // ========================================================================
    // 2. SmartDownloader + Router (5MB single-thread fast path, >5MB Range)
    // ========================================================================
    public enum DownloadState
    {
        Idle = 0,
        Probing = 1,
        Downloading = 2,
        Paused = 3,
        Completed = 4,
        Failed = 5,
        Resumed = 6
    }

    public sealed class DownloadProgress
    {
        public string Url;
        public string Dest;
        public long Total;
        public long Received;
        public int Slices;
        public int DoneSlices;
        public DownloadState State;
        public string Error;
        public double ThroughputBps;

        public int Percent => Total > 0 ? (int)(Received * 100 / Total) : 0;
    }

    // IDM-style resume record. Persisted to <dest>.resume so an interrupted
    // download can resume per-slice from the exact recorded Offset.
    [Serializable]
    internal sealed class ResumeManifest
    {
        public string Url;
        public string Dest;
        public long Total;
        public int Slices;
        public long[] Offsets;   // bytes already written per slice
        public long[] Lengths;   // full length of each slice
        public string Etag;
        public DateTime SavedAt;
    }

    internal static class SmartDownloadRouter
    {
        public const long FAST_PATH_BYTES = 5L * 1024 * 1024; // < 5MB single socket

        // Live progress registry so Host.DownloadStatus can poll any download
        // started through this router. Keyed by destination path.
        internal static readonly ConcurrentDictionary<string, DownloadProgress> s_progress = new();

        public static async Task<long> DownloadFileAsync(string url, string dest, int timeoutMs, IProgress<DownloadProgress>? progress = null, CancellationToken ct = default)
        {
            // Sink that fans the progress sample out to both the caller's
            // observer and the polled registry.
            var sink = new Progress<DownloadProgress>(p =>
            {
                s_progress[dest] = p;
                progress?.Report(p);
            });

            // Probe size + accept-ranges first.
            using var probe = new HttpClient { Timeout = TimeSpan.FromMilliseconds(Math.Max(2000, timeoutMs)) };
            long size = -1;
            bool canRange = false;
            string etag = null;
            try
            {
                using var head = new HttpRequestMessage(HttpMethod.Head, url);
                using var resp = await probe.SendAsync(head, ct).ConfigureAwait(false);
                if (resp.Content?.Headers?.ContentLength != null)
                    size = resp.Content.Headers.ContentLength.Value;
                canRange = resp.Headers.AcceptRanges.Contains("bytes");
                etag = resp.Headers.ETag?.Tag;
            }
            catch { /* fall through to GET to learn size */ }

            try
            {
                if (size < 0 || size <= FAST_PATH_BYTES || !canRange)
                {
                    return await FastPathAsync(url, dest, timeoutMs, sink, ct).ConfigureAwait(false);
                }
                return await SmartDownloader.DownloadAsync(url, dest, size, etag, timeoutMs, sink, ct).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // SmartDownloader already persisted the failure sample into
                // the registry; rethrow so the caller sees the error.
                throw;
            }
        }

        // Single-socket fast path: one GET, stream straight to disk.
        private static async Task<long> FastPathAsync(string url, string dest, int timeoutMs, IProgress<DownloadProgress> progress, CancellationToken ct)
        {
            var prog = new DownloadProgress { Url = url, Dest = dest, State = DownloadState.Downloading };
            using var client = new HttpClient { Timeout = TimeSpan.FromMilliseconds(timeoutMs <= 0 ? 30000 : timeoutMs) };
            using var resp = await client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            resp.EnsureSuccessStatusCode();
            long total = resp.Content.Headers.ContentLength ?? -1;
            prog.Total = total;
            progress?.Report(prog);

            EnsureDir(dest);
            using var fs = new FileStream(dest, FileMode.Create, FileAccess.Write, FileShare.None, 81920, FileOptions.SequentialScan);
            using var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            var buf = new byte[81920];
            long received = 0;
            var sw = System.Diagnostics.Stopwatch.StartNew();
            int r;
            while ((r = await src.ReadAsync(buf, 0, buf.Length, ct).ConfigureAwait(false)) > 0)
            {
                await fs.WriteAsync(buf, 0, r, ct).ConfigureAwait(false);
                received += r;
                prog.Received = received;
                if (total > 0) prog.ThroughputBps = received / Math.Max(1, sw.Elapsed.TotalSeconds);
                progress?.Report(prog);
            }
            prog.State = DownloadState.Completed;
            prog.Received = received;
            progress?.Report(prog);
            return received;
        }

        internal static void EnsureDir(string dest)
        {
            string dir = Path.GetDirectoryName(dest);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                Directory.CreateDirectory(dir);
        }
    }

    internal static class SmartDownloader
    {
        private const int MAX_SLICES = 16;
        private const int MIN_SLICE_BYTES = 256 * 1024;

        private static readonly ConcurrentDictionary<string, ResumeManifest> s_active = new();

        public static DownloadProgress Peek(string dest)
        {
            // Prefer the in-memory record; fall back to the persisted manifest
            // so a download that was interrupted (or started by an earlier
            // process) still reports its per-slice offsets.
            if (!s_active.TryGetValue(dest, out var m) || m == null)
            {
                string rp = ResumePath(dest);
                if (File.Exists(rp))
                {
                    try { m = JsonResumeLoad(rp); } catch { m = null; }
                }
            }
            if (m != null && m.Offsets != null && m.Lengths != null)
            {
                long rec = 0;
                int done = 0;
                int count = Math.Min(m.Offsets.Length, m.Lengths.Length);
                for (int i = 0; i < count; i++)
                {
                    rec += m.Offsets[i];
                    if (m.Offsets[i] >= m.Lengths[i]) done++;
                }
                return new DownloadProgress
                {
                    Url = m.Url,
                    Dest = m.Dest,
                    Total = m.Total,
                    Received = rec,
                    Slices = m.Slices,
                    DoneSlices = done,
                    State = DownloadState.Downloading,
                };
            }
            return null;
        }

        public static async Task<long> DownloadAsync(string url, string dest, long total, string etag, int timeoutMs, IProgress<DownloadProgress> progress, CancellationToken ct)
        {
            var manifest = LoadOrCreate(url, dest, total, etag);
            s_active[dest] = manifest;
            int slices = manifest.Slices;

            var prog = new DownloadProgress
            {
                Url = url,
                Dest = dest,
                Total = total,
                State = DownloadState.Resumed,
                Slices = slices,
            };
            // Publish the live sample so Host.DownloadStatus(dest) polls real
            // numbers (the tasks mutate prog in place).
            SmartDownloadRouter.s_progress[dest] = prog;
            progress?.Report(prog);

            var tasks = new Task[slices];
            var sw = System.Diagnostics.Stopwatch.StartNew();
            for (int i = 0; i < slices; i++)
            {
                int idx = i;
                tasks[i] = Task.Run(async () =>
                {
                    long start = SliceStart(manifest, idx);
                    long len = manifest.Lengths[idx];
                    long done = manifest.Offsets[idx];
                    long from = start + done;
                    long to = start + len - 1;

                    if (done >= len) return; // slice already complete

                    var opt = new Progress<long>(inc =>
                    {
                        Interlocked.Add(ref prog.Received, inc);
                    });

                    // Resume: open file, seek to slice start + already-written.
                    using var fs = new FileStream(dest, FileMode.OpenOrCreate, FileAccess.Write, FileShare.ReadWrite, 81920, FileOptions.SequentialScan);
                    fs.Seek(from, SeekOrigin.Begin);

                    using var client = new HttpClient { Timeout = TimeSpan.FromMilliseconds(timeoutMs <= 0 ? 30000 : timeoutMs) };
                    var req = new HttpRequestMessage(HttpMethod.Get, url);
                    req.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(from, to);
                    using var resp = await client.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
                    resp.EnsureSuccessStatusCode();

                    using var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
                    var buf = new byte[65536];
                    int r;
                    long local = done;
                    long sinceSave = 0;
                    try
                    {
                        while ((r = await src.ReadAsync(buf, 0, buf.Length, ct).ConfigureAwait(false)) > 0)
                        {
                            await fs.WriteAsync(buf, 0, r, ct).ConfigureAwait(false);
                            local += r;
                            manifest.Offsets[idx] = local;
                            Interlocked.Add(ref prog.Received, r);
                            // Persist the per-slice Offset every ~256KB of progress
                            // so a hard interrupt resumes from close to the edge.
                            sinceSave += r;
                            if (sinceSave >= 0x40000)
                            {
                                sinceSave = 0;
                                Save(manifest);
                            }
                        }
                    }
                    catch
                    {
                        // Cancelled / dropped: record where this slice stopped so
                        // the next attempt resumes here instead of restarting.
                        manifest.Offsets[idx] = local;
                        Save(manifest);
                        throw;
                    }
                    manifest.Offsets[idx] = len; // slice complete
                    Interlocked.Increment(ref prog.DoneSlices);
                    Save(manifest);
                }, ct);
            }

            try
            {
                await Task.WhenAll(tasks).ConfigureAwait(false);
                prog.State = DownloadState.Completed;
                prog.Received = total;
                progress?.Report(prog);
                DeleteResume(dest);
                s_active.TryRemove(dest, out _);
                return total;
            }
            catch (Exception ex)
            {
                prog.State = DownloadState.Failed;
                prog.Error = ex.Message;
                Save(manifest); // persist so a later resume() picks up here
                progress?.Report(prog);
                throw;
            }
        }

        private static long SliceStart(ResumeManifest m, int i)
        {
            long s = 0;
            for (int k = 0; k < i; k++) s += m.Lengths[k];
            return s;
        }

        private static ResumeManifest LoadOrCreate(string url, string dest, long total, string etag)
        {
            string path = ResumePath(dest);
            if (File.Exists(path))
            {
                try
                {
                    var m = JsonResumeLoad(path);
                    if (m != null && m.Url == url && m.Total == total)
                    {
                        // Resume: clamp any over-complete offsets.
                        for (int i = 0; i < m.Offsets.Length; i++)
                            if (m.Offsets[i] > m.Lengths[i]) m.Offsets[i] = m.Lengths[i];
                        return m;
                    }
                }
                catch { /* corrupt manifest -> recreate */ }
            }
            int slices = ComputeSlices(total);
            var m2 = new ResumeManifest
            {
                Url = url,
                Dest = dest,
                Total = total,
                Slices = slices,
                Etag = etag,
                SavedAt = DateTime.UtcNow,
            };
            m2.Offsets = new long[slices];
            m2.Lengths = new long[slices];
            long baseSize = total / slices;
            long rem = total % slices;
            for (int i = 0; i < slices; i++)
                m2.Lengths[i] = baseSize + (i < rem ? 1 : 0);
            Save(m2);
            return m2;
        }

        private static int ComputeSlices(long total)
        {
            if (total <= 0) return 1;
            int slices = (int)(total / (1L * 1024 * 1024)); // ~1MB per slice target
            slices = Math.Max(2, slices);
            slices = Math.Min(MAX_SLICES, slices);
            return slices;
        }

        private static string ResumePath(string dest) => dest + ".resume";

        private static void Save(ResumeManifest m)
        {
            m.SavedAt = DateTime.UtcNow;
            try { File.WriteAllText(ResumePath(m.Dest), JsonResumeSave(m)); }
            catch { }
        }

        private static void DeleteResume(string dest)
        {
            try { File.Delete(ResumePath(dest)); } catch { }
        }

        // Minimal JSON persistence (no external dep) - enough for the manifest.
        private static string JsonResumeSave(ResumeManifest m)
        {
            var sb = new StringBuilder();
            sb.Append("{\"Url\":").Append(Quote(m.Url)).Append(',');
            sb.Append("\"Dest\":").Append(Quote(m.Dest)).Append(',');
            sb.Append("\"Total\":").Append(m.Total).Append(',');
            sb.Append("\"Slices\":").Append(m.Slices).Append(',');
            sb.Append("\"Etag\":").Append(Quote(m.Etag ?? "")).Append(',');
            sb.Append("\"Offsets\":[").Append(string.Join(",", m.Offsets)).Append(']').Append(',');
            sb.Append("\"Lengths\":[").Append(string.Join(",", m.Lengths)).Append(']');
            sb.Append('}');
            return sb.ToString();
        }

        private static ResumeManifest JsonResumeLoad(string path)
        {
            string txt = File.ReadAllText(path);
            // Lightweight parser sufficient for our own emitted shape.
            var m = new ResumeManifest();
            m.Url = Str(txt, "Url");
            m.Dest = Str(txt, "Dest");
            m.Total = Lng(txt, "Total");
            m.Slices = (int)Lng(txt, "Slices");
            m.Etag = Str(txt, "Etag");
            m.Offsets = Arr(txt, "Offsets");
            m.Lengths = Arr(txt, "Lengths");
            if (m.Offsets == null || m.Lengths == null) return null;
            return m;
        }

        private static string Quote(string s) => "\"" + (s ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
        private static string Str(string txt, string key)
        {
            int i = txt.IndexOf("\"" + key + "\"");
            if (i < 0) return "";
            int c = txt.IndexOf(':', i) + 1;
            int q1 = txt.IndexOf('"', c);
            int q2 = txt.IndexOf('"', q1 + 1);
            return txt.Substring(q1 + 1, q2 - q1 - 1);
        }
        private static long Lng(string txt, string key)
        {
            int i = txt.IndexOf("\"" + key + "\"");
            if (i < 0) return 0;
            int c = txt.IndexOf(':', i) + 1;
            int e = c;
            while (e < txt.Length && (char.IsDigit(txt[e]) || txt[e] == '-')) e++;
            return long.TryParse(txt.Substring(c, e - c), out var v) ? v : 0;
        }
        private static long[] Arr(string txt, string key)
        {
            int i = txt.IndexOf("\"" + key + "\"");
            if (i < 0) return null;
            int c = txt.IndexOf('[', i);
            int e = txt.IndexOf(']', c);
            if (e < 0) return null;
            var part = txt.Substring(c + 1, e - c - 1);
            if (string.IsNullOrWhiteSpace(part)) return new long[0];
            var toks = part.Split(',');
            var outv = new long[toks.Length];
            for (int k = 0; k < toks.Length; k++) long.TryParse(toks[k].Trim(), out outv[k]);
            return outv;
        }
    }

    // ========================================================================
    // 3. MultiPathManager - physical bandwidth aggregation over many NICs
    // ========================================================================
    // Detects every operational IPv4 interface (WiFi + Ethernet), opens one
    // socket-bound HttpClient per link, hands each a disjoint byte range of the
    // file, fetches them concurrently, and stitches the slices in order. Because
    // each path is an independent TCP connection the slowest link only throttles
    // its own slice - head-of-line blocking across the others is eliminated and
    // the aggregate throughput approaches the sum of the links.
    internal static class MultiPathManager
    {
        // Live progress registry so Host.DownloadStatus can poll a multipath
        // download by destination path.
        internal static readonly ConcurrentDictionary<string, DownloadProgress> s_progress = new();

        private static void Log(string s) { Console.WriteLine("[multipath] " + s); }

        public static List<NetworkLink> DetectLinks()
        {
            var links = new List<NetworkLink>();
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                var props = ni.GetIPProperties();
                foreach (var ua in props.UnicastAddresses)
                {
                    if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    if (IPAddress.IsLoopback(ua.Address)) continue;
                    // Skip APIPA / link-local.
                    if (ua.Address.GetAddressBytes()[0] == 169) continue;
                    links.Add(new NetworkLink
                    {
                        Name = ni.Name,
                        Friendly = ni.Description,
                        Type = ni.NetworkInterfaceType.ToString(),
                        LocalAddress = ua.Address.ToString(),
                        Gateway = props.GatewayAddresses.Count > 0 ? props.GatewayAddresses[0].Address.ToString() : "",
                    });
                }
            }
            return links;
        }

        public static async Task<long> DownloadAsync(string url, string dest, IProgress<DownloadProgress> progress, CancellationToken ct)
        {
            var links = DetectLinks();
            if (links.Count == 0)
                throw new InvalidOperationException("no usable network interfaces detected");

            // Probe size.
            long total = -1;
            bool canRange = false;
            using (var probe = new HttpClient { Timeout = TimeSpan.FromSeconds(8) })
            using (var head = new HttpRequestMessage(HttpMethod.Head, url))
            {
                try
                {
                    using var resp = await probe.SendAsync(head, ct).ConfigureAwait(false);
                    total = resp.Content?.Headers?.ContentLength ?? -1;
                    canRange = resp.Headers.AcceptRanges.Contains("bytes");
                    if (!canRange) total = -1; // fall back below
                }
                catch { total = -1; }
            }

            SmartDownloadRouter.EnsureDir(dest);

            if (total <= 0)
            {
                // Unknown length or no range support: pick the "best" link and do
                // a single ranged pull to first discover size, else fast path.
                return await SingleLinkFallback(url, dest, links[0], progress, ct).ConfigureAwait(false);
            }

            // Not every detected interface can actually reach this host: VPN /
            // Hyper-V / Docker / VirtualBox adapters are Up and have an IPv4
            // address but have no route to the target. Binding a socket to one
            // of those fails outright, so probe each candidate and keep only
            // the links that really carry a request to this URL.
            var usable = await ProbeReachableLinks(url, links, ct).ConfigureAwait(false);
            if (usable.Count == 0)
                usable = links; // last resort: let the OS route normally

            // Assign contiguous ranges round-robin by link capacity proxy
            // (all equal here, but weighted ready for real metric hooks).
            int n = usable.Count;
            long baseSize = total / n;
            long rem = total % n;

            var prog = new DownloadProgress
            {
                Url = url,
                Dest = dest,
                Total = total,
                State = DownloadState.Downloading,
                Slices = n,
            };
            // Publish the live object: the parallel tasks mutate it in place,
            // so a poller sees byte counts advance without extra plumbing.
            s_progress[dest] = prog;
            progress?.Report(prog);

            var tasks = new Task[n];
            for (int i = 0; i < n; i++)
            {
                int idx = i;
                long start = baseSize * i + Math.Min(rem, i);
                long len = baseSize + (i < rem ? 1 : 0);
                tasks[i] = Task.Run(async () =>
                {
                    var link = usable[idx];
                    try
                    {
                        await FetchSliceAsync(url, dest, start, len, link, prog, ct).ConfigureAwait(false);
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception ex)
                    {
                        // A bound NIC can still fail mid-transfer (link flap,
                        // route change). Retry this slice once on the default
                        // route so one bad adapter cannot sink the download.
                        Log("multipath slice " + idx + " via " + link.LocalAddress +
                            " failed (" + ex.Message + "); retrying on default route");
                        await FetchSliceAsync(url, dest, start, len, null, prog, ct).ConfigureAwait(false);
                    }
                    Interlocked.Increment(ref prog.DoneSlices); // slice fully written
                }, ct);
            }

            try
            {
                await Task.WhenAll(tasks).ConfigureAwait(false);
                prog.State = DownloadState.Completed;
                prog.Received = total;
                progress?.Report(prog);
                return total;
            }
            catch (Exception ex)
            {
                prog.State = DownloadState.Failed;
                prog.Error = ex.Message + " | links=" + string.Join(",", usable.Select(l => l.LocalAddress));
                progress?.Report(prog);
                throw;
            }
        }

        // Write one contiguous byte range of `url` into `dest` at `start`,
        // through `link` when given, else via the OS default route.
        private static async Task FetchSliceAsync(string url, string dest, long start, long len,
                                                  NetworkLink link, DownloadProgress prog,
                                                  CancellationToken ct)
        {
            using var client = link != null ? BindToLink(link) : new HttpClient();
            client.Timeout = TimeSpan.FromMinutes(10);
            var req = new HttpRequestMessage(HttpMethod.Get, url);
            req.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(start, start + len - 1);
            using var resp = await client.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            resp.EnsureSuccessStatusCode();
            using var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            using var fs = new FileStream(dest, FileMode.OpenOrCreate, FileAccess.Write, FileShare.ReadWrite, 81920, FileOptions.WriteThrough);
            fs.Seek(start, SeekOrigin.Begin);
            var buf = new byte[65536];
            int r;
            while ((r = await src.ReadAsync(buf, 0, buf.Length, ct).ConfigureAwait(false)) > 0)
            {
                await fs.WriteAsync(buf, 0, r, ct).ConfigureAwait(false);
                Interlocked.Add(ref prog.Received, r);
            }
        }

        // Keep only the interfaces that can actually carry a request to `url`.
        // Virtual adapters (Hyper-V / Docker / VPN) are Up with a valid IPv4
        // address but have no route to the target, and binding to them makes
        // the connect fail with WSAEADDRNOTAVAIL. A cheap ranged probe per
        // candidate filters them out before any bytes are scheduled.
        private static async Task<List<NetworkLink>> ProbeReachableLinks(
            string url, List<NetworkLink> candidates, CancellationToken ct)
        {
            var results = new NetworkLink[candidates.Count];
            var tasks = new Task[candidates.Count];
            for (int i = 0; i < candidates.Count; i++)
            {
                int idx = i;
                tasks[i] = Task.Run(async () =>
                {
                    try
                    {
                        using var c = BindToLink(candidates[idx]);
                        c.Timeout = TimeSpan.FromSeconds(5);
                        using var req = new HttpRequestMessage(HttpMethod.Get, url);
                        req.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, 0);
                        using var resp = await c.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
                        if (resp.IsSuccessStatusCode) results[idx] = candidates[idx];
                    }
                    catch { /* not reachable through this NIC */ }
                }, ct);
            }
            try { await Task.WhenAll(tasks).ConfigureAwait(false); } catch { }
            var usable = new List<NetworkLink>();
            foreach (var l in results) if (l != null) usable.Add(l);
            return usable;
        }

        private static async Task<long> SingleLinkFallback(string url, string dest, NetworkLink link, IProgress<DownloadProgress> progress, CancellationToken ct)
        {
            // A bound NIC may not reach this host at all (virtual adapter);
            // fall back to the OS default route rather than failing the pull.
            HttpClient client = null;
            if (link != null)
            {
                try { client = BindToLink(link); }
                catch { client = null; }
            }
            client ??= new HttpClient();
            using (client)
            {
                client.Timeout = TimeSpan.FromMinutes(10);
                try
                {
                    return await SingleLinkPull(url, dest, client, progress, ct).ConfigureAwait(false);
                }
                catch when (link != null)
                {
                    // Bound link failed: retry the whole pull on the default route.
                    using var plain = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
                    return await SingleLinkPull(url, dest, plain, progress, ct).ConfigureAwait(false);
                }
            }
        }

        private static async Task<long> SingleLinkPull(string url, string dest, HttpClient client,
                                                       IProgress<DownloadProgress> progress, CancellationToken ct)
        {
            using var resp = await client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            resp.EnsureSuccessStatusCode();
            long total = resp.Content.Headers.ContentLength ?? -1;
            using var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            using var fs = new FileStream(dest, FileMode.Create, FileAccess.Write, FileShare.None, 81920, FileOptions.SequentialScan);
            var buf = new byte[81920];
            long received = 0;
            var sample = new DownloadProgress
            {
                Url = url,
                Dest = dest,
                Total = total,
                State = DownloadState.Downloading,
            };
            s_progress[dest] = sample;
            int r;
            while ((r = await src.ReadAsync(buf, 0, buf.Length, ct).ConfigureAwait(false)) > 0)
            {
                await fs.WriteAsync(buf, 0, r, ct).ConfigureAwait(false);
                received += r;
                sample.Received = received;
                progress?.Report(sample);
            }
            sample.State = DownloadState.Completed;
            sample.Received = received;
            progress?.Report(sample);
            return received;
        }

        // Bind an HttpClient so its sockets egress through a specific local NIC
        // address. SocketsHttpHandler.ConnectCallback is the supported hook for
        // choosing the local endpoint, so each path physically uses its own link.
        private static HttpClient BindToLink(NetworkLink link)
        {
            var local = IPAddress.Parse(link.LocalAddress);
            var sh = new SocketsHttpHandler
            {
                ConnectCallback = async (ctx, token) =>
                {
                    var ep = ctx.DnsEndPoint;
                    // Create the socket for this address family, bind its local
                    // endpoint to the chosen NIC, then connect. Binding before
                    // connect is what forces the egress interface.
                    var socket = new Socket(local.AddressFamily, SocketType.Stream, ProtocolType.Tcp);
                    try
                    {
                        socket.Bind(new IPEndPoint(local, 0));
                        socket.NoDelay = true;
                        await socket.ConnectAsync(new DnsEndPoint(ep.Host, ep.Port), token).ConfigureAwait(false);
                        return new NetworkStream(socket, ownsSocket: true);
                    }
                    catch
                    {
                        socket.Dispose();
                        throw;
                    }
                },
                PooledConnectionLifetime = TimeSpan.FromMinutes(5),
                EnableMultipleHttp2Connections = false,
            };
            var client = new HttpClient(sh, disposeHandler: true);
            client.Timeout = TimeSpan.FromMinutes(10);
            return client;
        }
    }

    public sealed class NetworkLink
    {
        public string Name;
        public string Friendly;
        public string Type;
        public string LocalAddress;
        public string Gateway;
    }
}
