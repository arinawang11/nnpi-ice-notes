// Minimal iSCSI target with a RAM-backed LUN.
// Purpose: validate the CrystalDiskMark -> Windows iSCSI initiator -> target
// chain on this host before the backend is replaced by the NNPI MEM transport.
// MC/S-capable (multiple connections per session), InitialR2T=Yes, digests
// off, no auth.
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

static class IscsiTarget
{
    const int BlockSize = 512;
    const int MaxBurst = 262144;
    const int MaxRecv = 65536;
    const int MaxOutstandingR2T = 16;
    // The initiator keeps per-target session state; a fresh name is the
    // quickest way to get a clean login after an aborted MC/S session.
    static string TargetName = "iqn.2026-10.local.nnpitest:ram0mcs";
    // MC/S is implemented but off by default: measured slower than one
    // connection on this host, and a target restart with a second connection
    // active leaves the Windows initiator session stuck in reconnect.
    static int MaxConnections = 1;
    // Advertised in SendTargets; the initiator connects back to this address.
    static string Advertise = "127.0.0.1:3260";
    // Optional extra names this instance answers to, so one process can host
    // several sessions without a second loopback address.
    static string[] ExtraTargets;

    static readonly byte[] Empty = new byte[0];
    static byte[] disk;
    static long blockCount;
    // Reads only need to keep writes out while Send() copies the payload into
    // the kernel; multiple readers (one per MC/S connection) may run at once.
    static readonly ReaderWriterLockSlim diskLock = new ReaderWriterLockSlim();

    static int Main(string[] args)
    {
        int port = 3260;
        int mb = 64;
        if (args.Length > 0) port = int.Parse(args[0]);
        if (args.Length > 1) mb = int.Parse(args[1]);
        if (args.Length > 2) TargetName = args[2];
        string extra = Environment.GetEnvironmentVariable("NNPI_ISCSI_NAMES");
        if (!string.IsNullOrEmpty(extra)) ExtraTargets = extra.Split(',');
        Verbose = Environment.GetEnvironmentVariable("NNPI_ISCSI_VERBOSE") == "1";
        Stats = Environment.GetEnvironmentVariable("NNPI_ISCSI_STATS") == "1";
        Trace = Environment.GetEnvironmentVariable("NNPI_ISCSI_TRACE") == "1";
        int mc;
        if (int.TryParse(Environment.GetEnvironmentVariable("NNPI_ISCSI_MAXCONN"), out mc) && mc >= 1 && mc <= 2)
            MaxConnections = mc;
        disk = new byte[(long)mb * 1024L * 1024L];
        blockCount = disk.LongLength / BlockSize;
        // The initiator's discovery portal has no port field, so a second
        // independent target has to live on another loopback address.
        IPAddress bindAddr = IPAddress.Any;
        string bindText = Environment.GetEnvironmentVariable("NNPI_ISCSI_BIND");
        if (!string.IsNullOrEmpty(bindText)) bindAddr = IPAddress.Parse(bindText);
        Advertise = (bindAddr.Equals(IPAddress.Any) ? "127.0.0.1" : bindAddr.ToString()) + ":" + port;
        TcpListener listener = new TcpListener(bindAddr, port);
        listener.Start();
        Console.WriteLine("target={0} bind={1} port={2} size_mib={3} blocks={4}", TargetName, bindAddr, port, mb, blockCount);
        Console.Out.Flush();
        while (true)
        {
            TcpClient client = listener.AcceptTcpClient();
            client.NoDelay = true;
            try { client.Client.SendBufferSize = 1 << 20; client.Client.ReceiveBufferSize = 1 << 20; } catch { }
            Thread t = new Thread(delegate() { Session(client); });
            t.IsBackground = true;
            t.Start();
        }
    }

    sealed class Pdu
    {
        public byte[] B;
        public byte[] D;
        public int Op { get { return B[0] & 0x3f; } }
        public int DataLen { get { return (B[5] << 16) | (B[6] << 8) | B[7]; } }
    }

    // Session state shared by every connection that logged in with the same
    // ISID (MC/S). StatSN stays per connection; CmdSN window and the unit
    // attention are session-scoped per RFC 3720.
    sealed class Ses
    {
        public ushort Tsih;
        public uint ExpCmdSN = 1;
        public uint MaxCmdSN = 256;
        public readonly HashSet<uint> Gaps = new HashSet<uint>();
        public readonly object Gate = new object();
        // RAM LUN contents never survive a target restart. Report a unit
        // attention once per session so Windows drops its cached partition
        // table and re-reads the device instead of trusting stale metadata.
        public bool UaPending = true;
        // Windows identifies a disk by VPD 0x80/0x83. Two LUNs that report
        // the same identifiers are treated as one physical disk, and the
        // second one ends up write-protected in Disk Management, so each
        // session carries the suffix of the target name it logged in to.
        public string Suffix = "0001";
    }

    sealed class Conn
    {
        public int Id;
        public long Cmds;
        public Socket Sock;
        public Ses Ses;
        public uint StatSN = 1;
        public uint R2tSN;
        public uint Ttt = 0x1000;
        public bool LoggedIn;
        public bool GroupTagSent;
        public HashSet<string> Proposed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    }

    static int NextConnId;
    static readonly List<Conn> Active = new List<Conn>();
    static readonly object ActiveGate = new object();

    static string ConnStats()
    {
        lock (ActiveGate)
        {
            StringBuilder sb = new StringBuilder();
            foreach (Conn x in Active) sb.Append(" c").Append(x.Id).Append('=').Append(x.Cmds);
            return sb.ToString();
        }
    }

    static readonly Dictionary<ushort, Ses> SessionsByTsih = new Dictionary<ushort, Ses>();
    static readonly object SessionsGate = new object();
    static ushort NextTsih = 1;

    static Ses FindSession(byte[] b)
    {
        ushort tsih = G16(b, 14);
        lock (SessionsGate)
        {
            Ses s;
            // TSIH != 0 is an additional connection for an existing session
            // (MC/S). TSIH == 0 is always a new session: Windows reuses one
            // ISID across unrelated sessions, so ISID alone cannot identify
            // a session and must not be used as a cache key.
            if (tsih != 0 && SessionsByTsih.TryGetValue(tsih, out s)) return s;
            s = new Ses();
            s.Tsih = NextTsih++;
            if (NextTsih == 0) NextTsih = 1;
            SessionsByTsih[s.Tsih] = s;
            return s;
        }
    }

    // CmdSN is session-wide: commands from different connections may arrive
    // out of order, so hold ExpCmdSN at the first hole and remember the rest.
    static void SessionSeeCommand(Ses s, uint cmdSN)
    {
        lock (s.Gate)
        {
            if (cmdSN == s.ExpCmdSN)
            {
                s.ExpCmdSN++;
                while (s.Gaps.Remove(s.ExpCmdSN)) s.ExpCmdSN++;
            }
            else if (cmdSN > s.ExpCmdSN) s.Gaps.Add(cmdSN);
            if (s.MaxCmdSN < s.ExpCmdSN + 255) s.MaxCmdSN = s.ExpCmdSN + 255;
        }
    }

    // Login PDUs carry CmdSN but do not consume one; the next command keeps
    // the same value.
    static void SessionSeeLogin(Ses s, uint cmdSN)
    {
        lock (s.Gate)
        {
            if (cmdSN > s.ExpCmdSN) s.ExpCmdSN = cmdSN;
            if (s.MaxCmdSN < s.ExpCmdSN + 255) s.MaxCmdSN = s.ExpCmdSN + 255;
        }
    }

    static readonly byte[] SkipBuf = new byte[4096];
    static readonly byte[] PadZero = new byte[4];

    static void Skip(Conn c, int n)
    {
        while (n > 0)
        {
            int k = Math.Min(n, SkipBuf.Length);
            ReadExact(c, SkipBuf, 0, k);
            n -= k;
        }
    }

    static void ReadExact(Conn c, byte[] dst, int off, int n)
    {
        while (n > 0)
        {
            if (c.Sock.Poll(0, SelectMode.SelectRead)) StatPollHit++;
            StatRecvs++;
            int r = c.Sock.Receive(dst, off, n, SocketFlags.None);
            if (r <= 0) throw new System.IO.EndOfStreamException();
            off += r; n -= r;
        }
    }

    static Pdu ReadPdu(Conn c)
    {
        byte[] b = new byte[48];
        ReadExact(c, b, 0, 48);
        int ahs = b[4] * 4;
        int dl = (b[5] << 16) | (b[6] << 8) | b[7];
        if (ahs > 0) Skip(c, ahs);
        byte[] d = Empty;
        if (dl > 0)
        {
            d = new byte[dl];
            ReadExact(c, d, 0, dl);
        }
        int pad = (4 - (dl & 3)) & 3;
        if (pad > 0) Skip(c, pad);
        Pdu p = new Pdu();
        p.B = b; p.D = d;
        return p;
    }

    // The receive loop sends each response itself. A separate sender thread
    // measured slower on loopback: the initiator keeps only one command in
    // flight per connection, so the queue never filled and every PDU paid a
    // thread handoff. MC/S scales with one receive loop per connection.
    static void QueuePdu(Conn c, byte[] b, byte[] d, int off, int len, bool advance)
    {
        uint expCmdSN, maxCmdSN;
        lock (c.Ses.Gate) { expCmdSN = c.Ses.ExpCmdSN; maxCmdSN = c.Ses.MaxCmdSN; }
        SendPdu(c, b, d, off, len, advance, expCmdSN, maxCmdSN);
    }

    static void SendPdu(Conn c, byte[] hdr, byte[] d, int off, int len, bool advance, uint expCmdSN, uint maxCmdSN)
    {
        P32(hdr, 24, c.StatSN);
        if (advance) c.StatSN++;
        P32(hdr, 28, expCmdSN);
        P32(hdr, 32, maxCmdSN);
        int pad = (4 - (len & 3)) & 3;
        if (len <= 8192)
        {
            // Single contiguous buffer, one send: allocation and CLR
            // marshalling of a scatter list cost more than copying 4K.
            byte[] t = new byte[48 + len + pad];
            Buffer.BlockCopy(hdr, 0, t, 0, 48);
            if (len > 0) Buffer.BlockCopy(d, off, t, 48, len);
            StatSends++; StatSendBytes += t.Length;
            c.Sock.Send(t, 0, t.Length, SocketFlags.None);
        }
        else
        {
            List<ArraySegment<byte>> segs = new List<ArraySegment<byte>>(3);
            segs.Add(new ArraySegment<byte>(hdr));
            segs.Add(new ArraySegment<byte>(d, off, len));
            if (pad > 0) segs.Add(new ArraySegment<byte>(PadZero, 0, pad));
            StatSends++; StatSendBytes += 48 + len + pad;
            c.Sock.Send(segs, SocketFlags.None);
        }
    }

    static bool Verbose;
    static bool Stats;
    static bool Trace;
    static long StatCmds;
    static long StatPdus, StatRecvs, StatSends, StatSendBytes, StatPollHit;
    static void Log(string fmt, params object[] a) { if (Verbose) Console.WriteLine(fmt, a); }

    static void P16(byte[] b, int o, ushort v) { b[o] = (byte)(v >> 8); b[o + 1] = (byte)v; }
    static void P32(byte[] b, int o, uint v) { b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v; }
    static void P64(byte[] b, int o, ulong v) { P32(b, o, (uint)(v >> 32)); P32(b, o + 4, (uint)v); }
    static uint G32(byte[] b, int o) { return ((uint)b[o] << 24) | ((uint)b[o + 1] << 16) | ((uint)b[o + 2] << 8) | b[o + 3]; }
    static ulong G64(byte[] b, int o) { return ((ulong)G32(b, o) << 32) | G32(b, o + 4); }
    static ushort G16(byte[] b, int o) { return (ushort)((b[o] << 8) | b[o + 1]); }

    static Dictionary<string, string> ParseKeys(byte[] d)
    {
        Dictionary<string, string> map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string text = Encoding.ASCII.GetString(d);
        foreach (string part in text.Split('\0'))
        {
            int eq = part.IndexOf('=');
            if (eq > 0) map[part.Substring(0, eq)] = part.Substring(eq + 1);
        }
        return map;
    }

    static void Session(TcpClient client)
    {
        Conn c = new Conn();
        c.Sock = client.Client;
        c.Id = Interlocked.Increment(ref NextConnId);
        lock (ActiveGate) Active.Add(c);
        Console.WriteLine("connect id={0} {1}", c.Id, client.Client.RemoteEndPoint);
        try
        {
            while (true)
            {
                Pdu p = ReadPdu(c);
                StatPdus++;
                if (Stats && (StatPdus % 20000) == 0)
                    Console.WriteLine("stats pdus={0} recv={1} send={2} bytes={3} pollhit={4}{5}", StatPdus, StatRecvs, StatSends, StatSendBytes, StatPollHit, ConnStats());
                if (Verbose) Log("{0:HH:mm:ss.fff} pdu op=0x{1:x2} len={2}", DateTime.Now, p.Op, p.D.Length);
                switch (p.Op)
                {
                    case 0x03: Login(c, p); break;
                    case 0x04: Text(c, p); break;
                    case 0x01: Command(c, p); break;
                    case 0x00: NopOut(c, p); break;
                    case 0x06: Logout(c, p); return;
                    case 0x02: TaskMgmt(c, p); break;
                    default:
                        Console.WriteLine("unhandled opcode 0x{0:x2}", p.Op);
                        return;
                }
            }
        }
        catch (Exception e)
        {
            Console.WriteLine("session end: {0}: {1}", e.GetType().Name, e.Message);
            Console.Out.Flush();
        }
        finally
        {
            lock (ActiveGate) Active.Remove(c);
            try { client.Close(); } catch { }
        }
    }

    static void Login(Conn c, Pdu p)
    {
        int csg = (p.B[1] >> 2) & 3;
        int nsg = p.B[1] & 3;
        Dictionary<string, string> keys = ParseKeys(p.D);
        List<string> kv = new List<string>();
        foreach (KeyValuePair<string, string> pair in keys) kv.Add(pair.Key + "=" + pair.Value);
        uint loginCmdSN = G32(p.B, 24);
        uint loginExpStatSN = G32(p.B, 28);
        // Additional MC/S connections log in with the same ISID (and usually
        // the session's TSIH), so the session is keyed by both.
        // One connection runs the whole login handshake (security round, then
        // operational round). Only the first PDU of a connection may create or
        // join a session; later PDUs belong to the session already assigned.
        if (c.Ses == null) c.Ses = FindSession(p.B);
        string reqTarget;
        if (keys.TryGetValue("TargetName", out reqTarget))
        {
            string suffix = "";
            for (int i = reqTarget.Length - 1; i >= 0 && suffix.Length < 8; i--)
            {
                char ch = reqTarget[i];
                if (ch == ':') break;
                suffix = (char.IsLetterOrDigit(ch) ? ch : '_') + suffix;
            }
            if (suffix.Length > 0) c.Ses.Suffix = suffix;
        }
        SessionSeeLogin(c.Ses, loginCmdSN);
        // A reconnect resumes the session's StatSN where the initiator left it.
        if (loginExpStatSN > c.StatSN) c.StatSN = loginExpStatSN;
        Console.WriteLine("login csg={0} nsg={1} t={2} c={3} cmdsn={4} expstatsn={5} tsih={6} keys=[{7}]",
            csg, nsg, (p.B[1] & 0x80) != 0, (p.B[1] & 0x40) != 0,
            loginCmdSN, G32(p.B, 28), c.Ses.Tsih, string.Join(";", kv.ToArray()));
        StringBuilder sb = new StringBuilder();
        bool transit = true;
        bool cont = false;

        if (keys.ContainsKey("SendTargets"))
        {
            AppendTargets(sb);
            sb.Append("TargetPortalGroupTag=1\0");
            nsg = 3;
        }
        else if (csg == 0)
        {
            // Security negotiation -> operational negotiation.
            sb.Append("AuthMethod=None\0");
            cont = false;
            nsg = 1;
        }
        else
        {
            // Operational negotiation: answer each offered key once. Keys the
            // initiator marks Irrelevant on a later round are left alone.
            if (!c.GroupTagSent)
            {
                sb.Append("TargetPortalGroupTag=1\0");
                sb.Append("TargetAlias=NNPI RAM Disk\0");
                c.GroupTagSent = true;
            }
            foreach (KeyValuePair<string, string> pair in keys)
            {
                string key = pair.Key;
                string value = pair.Value;
                if (string.Equals(value, "Irrelevant", StringComparison.OrdinalIgnoreCase)) continue;
                if (string.Equals(value, "NotUnderstood", StringComparison.OrdinalIgnoreCase)) continue;
                if (key.Equals("InitiatorName", StringComparison.OrdinalIgnoreCase)) continue;
                if (key.Equals("SessionType", StringComparison.OrdinalIgnoreCase)) continue;
                if (key.Equals("TargetName", StringComparison.OrdinalIgnoreCase)) continue;
                if (key.Equals("AuthMethod", StringComparison.OrdinalIgnoreCase)) continue;
                if (c.Proposed.Contains(key)) continue;
                string answer = Negotiate(key, value);
                if (answer == null) continue;
                sb.Append(key).Append('=').Append(answer).Append('\0');
                c.Proposed.Add(key);
            }
            nsg = 3;
            c.LoggedIn = true;
        }

        byte[] d = Encoding.ASCII.GetBytes(sb.ToString());
        byte[] b = new byte[48];
        b[0] = 0x23;
        b[1] = (byte)((transit ? 0x80 : 0) | (cont ? 0x40 : 0) | (csg << 2) | (nsg << 0));
        b[2] = 0; b[3] = 0;
        b[5] = (byte)(d.Length >> 16); b[6] = (byte)(d.Length >> 8); b[7] = (byte)d.Length;
        for (int i = 0; i < 6; i++) b[8 + i] = p.B[8 + i]; // ISID echo
        P16(b, 14, c.Ses.Tsih);
        P32(b, 16, G32(p.B, 16)); // ITT echo
        b[36] = 0; b[37] = 0; // status class 0, detail 0
        QueuePdu(c, b, d, 0, d.Length, true);
    }

    static void AppendTargets(StringBuilder sb)
    {
        string[] names = ExtraTargets ?? new string[] { TargetName };
        foreach (string n in names)
        {
            // Each name is paired with its own address: Windows rejects a
            // response that lists several names before the first address.
            sb.Append("TargetName=").Append(n.Trim()).Append('\0');
            sb.Append("TargetAddress=").Append(Advertise).Append(",1\0");
        }
    }

    static string Negotiate(string key, string value)
    {
        switch (key.ToLowerInvariant())
        {
            case "headerdigest": return "None";
            case "datadigest": return "None";
            case "errorrecoverylevel": return "0";
            case "initialr2t": return "Yes";
            case "immediatedata": return "Yes";
            case "maxrecvdatasegmentlength": return "262144";
            case "maxburstlength": return "262144";
            case "firstburstlength": return "65536";
            case "maxconnections":
                {
                    // Never answer above what the initiator offered, and never
                    // above the configured cap (default 1).
                    int v;
                    if (!int.TryParse(value, out v) || v > MaxConnections) v = MaxConnections;
                    if (v < 1) v = 1;
                    return v.ToString();
                }
            case "datapduinorder": return "Yes";
            case "datasequenceinorder": return "Yes";
            case "defaulttime2wait": return "0";
            case "defaulttime2retain": return "60";
            case "maxoutstandingr2t": return "16";
            default: return null;
        }
    }

    static void Text(Conn c, Pdu p)
    {
        Dictionary<string, string> keys = ParseKeys(p.D);
        byte[] d;
        if (keys.ContainsKey("SendTargets"))
        {
            StringBuilder sb = new StringBuilder();
            AppendTargets(sb);
            d = Encoding.ASCII.GetBytes(sb.ToString());
            if (Verbose) Log("sendtargets -> {0}", sb.ToString().Replace('\0', '|'));
        }
        else
            d = new byte[0];
        byte[] b = new byte[48];
        b[0] = 0x24;
        b[1] = 0x80; // F
        b[5] = (byte)(d.Length >> 16); b[6] = (byte)(d.Length >> 8); b[7] = (byte)d.Length;
        P32(b, 16, G32(p.B, 16));
        P32(b, 20, G32(p.B, 20));
        QueuePdu(c, b, d, 0, d.Length, true);
    }

    static void NopOut(Conn c, Pdu p)
    {
        byte[] b = new byte[48];
        b[0] = 0x20;
        b[1] = 0;
        P32(b, 16, 0xffffffff);
        P32(b, 20, G32(p.B, 20)); // echo TTT
        QueuePdu(c, b, null, 0, 0, true);
    }

    static void TaskMgmt(Conn c, Pdu p)
    {
        byte[] b = new byte[48];
        b[0] = 0x22;
        b[2] = 0; // response: function complete
        P32(b, 16, G32(p.B, 16));
        QueuePdu(c, b, null, 0, 0, true);
    }

    static void Logout(Conn c, Pdu p)
    {
        byte[] b = new byte[48];
        b[0] = 0x26;
        b[2] = 0; // response 0
        P32(b, 16, G32(p.B, 16));
        QueuePdu(c, b, null, 0, 0, true);
    }

    static void Response(Conn c, uint itt, byte status, byte[] sense)
    {
        byte[] b = new byte[48];
        b[0] = 0x21;
        // RFC 3720 10.4: byte 1 bit 0 is a constant 1 in the SCSI Response PDU.
        // Windows iScsiPrt rejects a response with this bit clear (event 23).
        b[1] = 0x80;
        b[2] = 0;
        b[3] = status;
        int dl = sense == null ? 0 : sense.Length;
        b[5] = (byte)(dl >> 16); b[6] = (byte)(dl >> 8); b[7] = (byte)dl;
        P32(b, 16, itt);
        if (Verbose) Log("rsp itt={0} status={1} sense={2}", itt, status, dl);
        QueuePdu(c, b, sense, 0, sense == null ? 0 : sense.Length, true);
    }

    static void DataInBatch(Conn c, uint itt, byte[] data, long off, long bytes)
    {
        if (bytes <= MaxRecv)
        {
            // Single PDU: one contiguous buffer, one send. The scatter path
            // allocates a List and an ArraySegment[] per command; at 4K QD32
            // that allocation dominates.
            byte[] b = new byte[48];
            b[0] = 0x25;
            b[1] = 0x81; // F + S
            b[5] = (byte)(bytes >> 16); b[6] = (byte)(bytes >> 8); b[7] = (byte)bytes;
            P32(b, 16, itt);
            P32(b, 20, 0xffffffff);
            P32(b, 36, 0);
            P32(b, 40, 0);
            P32(b, 44, 0);
            QueuePdu(c, b, data, (int)off, (int)bytes, true);
            return;
        }
        // One Send for the whole transfer: a 1 MiB read is 16 headers plus 16
        // payload segments in a single WSASend.
        uint expCmdSN, maxCmdSN;
        lock (c.Ses.Gate) { expCmdSN = c.Ses.ExpCmdSN; maxCmdSN = c.Ses.MaxCmdSN; }
        int count = (int)((bytes + MaxRecv - 1) / MaxRecv);
        List<ArraySegment<byte>> segs = new List<ArraySegment<byte>>(count * 2);
        uint dataSN = 0;
        long pos = 0;
        for (int i = 0; i < count; i++)
        {
            int chunk = (int)Math.Min((long)MaxRecv, bytes - pos);
            bool final = i == count - 1;
            byte[] b = new byte[48];
            b[0] = 0x25;
            // F=0x80, S=0x01 on the last PDU; U stays clear (no residual).
            b[1] = (byte)((final ? 0x80 : 0) | (final ? 0x01 : 0));
            b[5] = (byte)(chunk >> 16); b[6] = (byte)(chunk >> 8); b[7] = (byte)chunk;
            P32(b, 16, itt);
            P32(b, 20, 0xffffffff); // no R2T was used for this command
            P32(b, 36, dataSN++);
            P32(b, 40, (uint)pos); // Buffer Offset within the transfer (10.7.6)
            P32(b, 44, 0);
            // RFC 3720 10.7.3: every PDU carries the current StatSN; only the
            // S PDU advances it, so the increment happens once after the loop.
            P32(b, 24, c.StatSN);
            P32(b, 28, expCmdSN);
            P32(b, 32, maxCmdSN);
            segs.Add(new ArraySegment<byte>(b));
            segs.Add(new ArraySegment<byte>(data, (int)(off + pos), chunk));
            pos += chunk;
        }
        c.StatSN++;
        StatSends++; StatSendBytes += bytes + (long)count * 48;
        c.Sock.Send(segs, SocketFlags.None);
    }

    static void DataIn(Conn c, uint itt, byte[] data, int off, int len, uint dataSN, bool final, bool status, uint residual, uint bufOffset = 0)
    {
        byte[] b = new byte[48];
        b[0] = 0x25;
        b[1] = (byte)((final ? 0x80 : 0) | (status ? 0x01 : 0) | (residual > 0 ? 0x02 : 0));
        b[5] = (byte)(len >> 16); b[6] = (byte)(len >> 8); b[7] = (byte)len;
        P32(b, 16, itt);
        P32(b, 20, 0xffffffff);
        P32(b, 36, dataSN);
        P32(b, 40, bufOffset);
        P32(b, 44, residual);
        QueuePdu(c, b, data, off, len, status);
    }

    static void R2T(Conn c, uint itt, uint ttt, uint offset, uint desired)
    {
        byte[] b = new byte[48];
        b[0] = 0x31;
        b[1] = 0x80; // RFC 3720 10.8: byte 1 bit 0 is a constant 1
        P32(b, 16, itt);
        P32(b, 20, ttt);
        // RFC 3720 10.8.3: StatSN carries the next StatSN but does not advance it.
        P32(b, 36, c.R2tSN++);
        P32(b, 40, offset);
        P32(b, 44, desired);
        QueuePdu(c, b, null, 0, 0, false);
    }

    static void Command(Conn c, Pdu p)
    {
        uint itt = G32(p.B, 16);
        uint cmdSN = G32(p.B, 24);
        long seq = Interlocked.Increment(ref StatCmds);
        Interlocked.Increment(ref c.Cmds);
        if (Trace && seq <= 128) Console.WriteLine("cmd {0} cmdsn={1} t={2:HH:mm:ss.fff}", seq, cmdSN, DateTime.Now);
        SessionSeeCommand(c.Ses, cmdSN);
        bool ua;
        lock (c.Ses.Gate) { ua = c.Ses.UaPending; c.Ses.UaPending = false; }
        if (ua)
        {
            // Fixed-format sense: POWER ON, RESET, OR BUS DEVICE RESET (06/29).
            byte[] sense = new byte[18];
            sense[0] = 0x70;
            sense[2] = 0x06;
            sense[7] = 0x0A;
            sense[12] = 0x29;
            Response(c, itt, 2, sense);
            return;
        }

        byte[] cdb = new byte[16];
        Buffer.BlockCopy(p.B, 32, cdb, 0, 16);
        byte op = cdb[0];
        if (Verbose) Log("cmd op=0x{0:x2} flags=0x{1:x2} expdata={2} imm={3} cdb={4} itt={5} cmdsn={6} expstatsn={7} lun={8}",
            op, p.B[1], G32(p.B, 20), p.D.Length, BitConverter.ToString(cdb), itt, cmdSN, G32(p.B, 28), BitConverter.ToString(p.B, 8, 8));

        switch (op)
        {
            case 0x00: // TEST UNIT READY
                Response(c, itt, 0, null); break;
            case 0x12: // INQUIRY
                Inquiry(c, itt, cdb); break;
            case 0x25: // READ CAPACITY(10)
                {
                    byte[] d = new byte[8];
                    P32(d, 0, (uint)(blockCount - 1));
                    P32(d, 4, BlockSize);
                    DataIn(c, itt, d, 0, 8, 0, true, true, 0);
                }
                break;
            case 0x9e: // SERVICE ACTION IN(16)
                if ((cdb[1] & 0x1f) == 0x10) // READ CAPACITY(16)
                {
                    byte[] d = new byte[32];
                    P64(d, 0, (ulong)(blockCount - 1));
                    P32(d, 8, BlockSize);
                    DataIn(c, itt, d, 0, 32, 0, true, true, 0);
                }
                else Response(c, itt, 0, null);
                break;
            case 0x1a: // MODE SENSE(6)
                {
                    byte[] d = new byte[4];
                    d[0] = 3; d[1] = 0; d[2] = 0; d[3] = 0;
                    uint exp = cdb[4];
                    DataIn(c, itt, d, 0, 4, 0, true, true, exp > 4 ? exp - 4 : 0);
                }
                break;
            case 0x5a: // MODE SENSE(10)
                {
                    byte[] d = new byte[8];
                    d[0] = 0; d[1] = 6; d[2] = 0; d[3] = 0; d[4] = 0; d[5] = 0; d[6] = 0; d[7] = 0;
                    uint exp = (uint)((cdb[7] << 8) | cdb[8]);
                    DataIn(c, itt, d, 0, 8, 0, true, true, exp > 8 ? exp - 8 : 0);
                }
                break;
            case 0xa0: // REPORT LUNS
                {
                    byte[] d = new byte[16];
                    P32(d, 0, 8);
                    uint exp = ((uint)cdb[6] << 24) | ((uint)cdb[7] << 16) | ((uint)cdb[8] << 8) | cdb[9];
                    DataIn(c, itt, d, 0, 16, 0, true, true, exp > 16 ? exp - 16 : 0);
                }
                break;
            case 0x28: // READ(10)
                ReadWrite(c, itt, cdb, false, false, p.D); break;
            case 0x88: // READ(16)
                ReadWrite(c, itt, cdb, false, true, p.D); break;
            case 0x2a: // WRITE(10)
                ReadWrite(c, itt, cdb, true, false, p.D); break;
            case 0x8a: // WRITE(16)
                ReadWrite(c, itt, cdb, true, true, p.D); break;
            case 0x35: // SYNCHRONIZE CACHE(10)
            case 0x91: // SYNCHRONIZE CACHE(16)
                Response(c, itt, 0, null); break;
            case 0x1b: // START STOP UNIT
                Response(c, itt, 0, null); break;
            case 0x2f: // VERIFY(10)
                Response(c, itt, 0, null); break;
            case 0x03: // REQUEST SENSE
                {
                    byte[] d = new byte[18];
                    uint exp = cdb[4];
                    DataIn(c, itt, d, 0, 18, 0, true, true, exp > 18 ? exp - 18 : 0);
                }
                break;
            case 0x42: // UNMAP
            case 0x41: // WRITE SAME(10)
            case 0x93: // WRITE SAME(16)
            case 0x04: // FORMAT UNIT
                Response(c, itt, 0, null); break;
            default:
                if (Verbose) Log("unhandled CDB 0x{0:x2}", op);
                Response(c, itt, 1, null); // CHECK CONDITION
                break;
        }
    }

    static void Inquiry(Conn c, uint itt, byte[] cdb)
    {
        bool evpd = (cdb[1] & 1) != 0;
        byte page = cdb[2];
        List<byte> d = new List<byte>();
        if (!evpd)
        {
            byte[] std = new byte[36];
            std[0] = 0x00;
            std[1] = 0x00;
            std[2] = 0x05;
            std[3] = 0x02;
            std[4] = 31;
            byte[] vendor = Encoding.ASCII.GetBytes("NNPI    ");
            byte[] product = Encoding.ASCII.GetBytes("VIRTUAL DISK    ");
            byte[] rev = Encoding.ASCII.GetBytes("0001");
            Buffer.BlockCopy(vendor, 0, std, 8, 8);
            Buffer.BlockCopy(product, 0, std, 16, 16);
            Buffer.BlockCopy(rev, 0, std, 32, 4);
            d.AddRange(std);
        }
        else if (page == 0x00)
        {
            d.AddRange(new byte[] { 0x00, 0x00, 0x00, 0x03, 0x00, 0x80, 0x83 });
        }
        else if (page == 0x80)
        {
            byte[] s = Encoding.ASCII.GetBytes(("NNPI-" + c.Ses.Suffix).PadRight(16, '0'));
            d.Add(0x00); d.Add(0x80); d.Add(0x00); d.Add((byte)(s.Length + 1));
            d.AddRange(s);
        }
        else if (page == 0x83)
        {
            byte[] id = Encoding.ASCII.GetBytes("NNPI-DISK-" + c.Ses.Suffix);
            d.Add(0x00); d.Add(0x83); d.Add(0x00); d.Add((byte)(4 + id.Length));
            d.Add(0x01); d.Add(0x03); d.Add(0x00); d.Add((byte)id.Length);
            d.AddRange(id);
        }
        else
        {
            Response(c, itt, 1, null);
            return;
        }
        byte[] arr = d.ToArray();
        int expected = (cdb[3] << 8) | cdb[4];
        int alloc = Math.Min(expected, arr.Length);
        DataIn(c, itt, arr, 0, alloc, 0, true, true, (uint)(expected - alloc));
    }

    static void ReadWrite(Conn c, uint itt, byte[] cdb, bool write, bool cdb16, byte[] imm)
    {
        ulong lba;
        uint len;
        if (cdb16)
        {
            lba = G64(cdb, 2);
            len = G32(cdb, 10);
        }
        else
        {
            lba = ((ulong)G32(cdb, 2)) & 0xffffffffUL;
            len = ((uint)cdb[7] << 8) | cdb[8];
        }
        long bytes = (long)len * BlockSize;
        long off = (long)lba * BlockSize;
        if (len == 0) { Response(c, itt, 0, null); return; }
        if (off < 0 || bytes < 0 || off + bytes > disk.LongLength)
        {
            Response(c, itt, 1, null); // CHECK CONDITION
            return;
        }
        if (!write)
        {
            diskLock.EnterReadLock();
            try
            {
                DataInBatch(c, itt, disk, off, bytes);
            }
            finally { diskLock.ExitReadLock(); }
        }
        else
        {
            diskLock.EnterWriteLock();
            try
            {
                int immLen = Math.Min(imm == null ? 0 : imm.Length, (int)bytes);
                if (immLen > 0) Buffer.BlockCopy(imm, 0, disk, (int)off, immLen);
                long received = immLen;
                long requested = received;
                if (Verbose) Log("write lba={0} bytes={1} imm={2} needR2T={3}", lba, bytes, immLen, received < bytes);
                // Pipeline R2T windows: Windows offers MaxOutstandingR2T=16, so a
                // 1 MiB write can have every window in flight instead of stalling
                // on a target round trip per 256 KiB.
                Queue<long> windowEnd = new Queue<long>();
                while (requested < bytes && windowEnd.Count < MaxOutstandingR2T)
                {
                    uint want = (uint)Math.Min((long)MaxBurst, bytes - requested);
                    R2T(c, itt, c.Ttt++, (uint)requested, want);
                    requested += want;
                    windowEnd.Enqueue(requested);
                }
                while (received < bytes)
                {
                    byte[] hb = new byte[48];
                    ReadExact(c, hb, 0, 48);
                    int op = hb[0] & 0x3f;
                    int ahs = hb[4] * 4;
                    int dl = (hb[5] << 16) | (hb[6] << 8) | hb[7];
                    int pad = (4 - (dl & 3)) & 3;
                    if (op == 0x05) // SCSI Data-Out
                    {
                        if (ahs > 0) Skip(c, ahs);
                        if (dl <= 0 || received + dl > bytes) { Response(c, itt, 1, null); return; }
                        // Read the payload straight into the LUN at its final offset.
                        ReadExact(c, disk, (int)(off + received), dl);
                        if (pad > 0) Skip(c, pad);
                        received += dl;
                        while (windowEnd.Count > 0 && received >= windowEnd.Peek()) windowEnd.Dequeue();
                        while (requested < bytes && windowEnd.Count < MaxOutstandingR2T)
                        {
                            uint want = (uint)Math.Min((long)MaxBurst, bytes - requested);
                            R2T(c, itt, c.Ttt++, (uint)requested, want);
                            requested += want;
                            windowEnd.Enqueue(requested);
                        }
                    }
                    else if (op == 0x00) // Nop-Out interleaved by the initiator
                    {
                        if (ahs > 0) Skip(c, ahs);
                        byte[] d = Empty;
                        if (dl > 0) { d = new byte[dl]; ReadExact(c, d, 0, dl); }
                        if (pad > 0) Skip(c, pad);
                        Pdu q = new Pdu(); q.B = hb; q.D = d;
                        NopOut(c, q);
                    }
                    else
                    {
                        if (Verbose) Log("unexpected opcode 0x{0:x2} during write", op);
                        Response(c, itt, 1, null);
                        return;
                    }
                }
            }
            finally { diskLock.ExitWriteLock(); }
            if (Verbose) Log("write done lba={0} bytes={1}", lba, bytes);
            Response(c, itt, 0, null);
        }
    }
}
