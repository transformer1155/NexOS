// Standalone runtime test of the NexOS voice engine's core matching logic.
// This replicates the exact LooseMatch / IsSkip / Lower algorithm and the
// serial frame parser from csharp/NexOS.Forms/Voice.cs and
// .attic64/kernel64.cpp, so the fuzzy matcher can be verified on a normal
// .NET runtime without booting the 64-bit kernel (which triple-faults under
// TCG on this machine inside build_idt() -- unrelated to the voice code).
using System;
using System.Text;

class Program
{
    static bool IsSkip(char c)
    {
        return c == ' ' || c == '\t' || c == ',' || c == '.' || c == '!' ||
               c == '?' || c == ':'  || c == '"' || c == '\'' ||
               c == '。' || c == '，' || c == '！' || c == '？' ||
               c == '：' || c == '“' || c == '”' || c == '、' ||
               c == '（' || c == '）' || c == '·';
    }
    static char Lower(char c)
    {
        if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
        return c;
    }

    // Verbatim copy of Voice.cs LooseMatch (string form).
    static bool LooseMatch(string hay, string needle)
    {
        int hlen = hay.Length, nlen = needle.Length;
        if (nlen == 0) return true;
        if (nlen > hlen) return false;
        int hi = 0;
        while (hi < hlen)
        {
            char hc = hay[hi];
            if (IsSkip(hc)) { hi++; continue; }
            int pp = hi, q = 0, matched = 0;
            while (pp < hlen && q < nlen)
            {
                char a = hay[pp];
                char b = needle[q];
                if (IsSkip(a)) { pp++; continue; }
                if (IsSkip(b)) { q++; continue; }
                if (Lower(a) != Lower(b)) break;
                pp++; q++; matched++;
            }
            if (q >= nlen && matched > 0) return true;
            hi++;
        }
        return false;
    }

    // Verbatim copy of kernel64.cpp serial_voice_poll framing.
    static string ParseFrame(byte[] frame)
    {
        if (frame.Length < 2) return null;
        if (frame[0] != 0x02) return null;
        int n = frame[1];
        if (n <= 0 || n > 127) return null;
        if (frame.Length < 2 + n) return null;
        return Encoding.UTF8.GetString(frame, 2, n);
    }

    static byte[] MakeFrame(string s)
    {
        byte[] b = Encoding.UTF8.GetBytes(s);
        byte[] outp = new byte[2 + b.Length];
        outp[0] = 0x02;
        outp[1] = (byte)(b.Length & 0x7F);
        Array.Copy(b, 0, outp, 2, b.Length);
        return outp;
    }

    // Longest-command-wins selection (mirrors Voice.Drain).
    static string Resolve(string phrase, string[] cmds)
    {
        string best = null; int bestLen = 0;
        foreach (string c in cmds)
            if (LooseMatch(phrase, c) && c.Length > bestLen) { bestLen = c.Length; best = c; }
        return best;
    }

    static int pass = 0, fail = 0;
    static void Check(bool cond, string name)
    {
        if (cond) { pass++; Console.WriteLine("  PASS  " + name); }
        else      { fail++; Console.WriteLine("  FAIL  " + name); }
    }

    static void Main()
    {
        Console.WriteLine("== NexOS voice matcher logic test ==");

        Check(LooseMatch("打开浏览器", "浏览器"),   "中文短语匹配 浏览器");
        Check(LooseMatch("open browser", "browser"), "英文短语匹配 browser");
        Check(LooseMatch("浏览器 Browser", "浏览器"), "双别名命令含 浏览器");
        Check(LooseMatch("浏览器 Browser", "Browser"), "双别名命令含 Browser");
        Check(LooseMatch("Open Browser", "BROWSER"),  "大小写不敏感");
        Check(LooseMatch("打开 浏览器 ！", "浏览器"),  "忽略空格/标点");
        Check(LooseMatch("voice on", "on"),          "子串 on 匹配");

        Check(!LooseMatch("打开计算器", "浏览器"),     "计算器 不应匹配 浏览器");
        Check(!LooseMatch("关闭窗口", "浏览器"),       "关闭窗口 不应匹配 浏览器");
        Check(!LooseMatch("浏览器 Browser", "计算器"), "浏览器 不应匹配 计算器");

        string[] desktopCmds = new string[] { "浏览器", "Browser", "计算器", "Calculator",
                                              "记事本", "Notepad", "设置", "ControlPanel" };
        Check(Resolve("打开浏览器", desktopCmds) == "浏览器", "resolve 打开浏览器 -> 浏览器");
        Check(Resolve("open browser", desktopCmds) == "Browser", "resolve open browser -> Browser");
        Check(Resolve("打开设置", desktopCmds) == "设置",       "resolve 打开设置 -> 设置");
        Check(Resolve("启动计算器", desktopCmds) == "计算器",     "resolve 启动计算器 -> 计算器");
        Check(Resolve("打开记事本 app", desktopCmds) == "记事本", "resolve 打开记事本 app -> 记事本");

        Check(ParseFrame(MakeFrame("打开浏览器")) == "打开浏览器", "解析中文语音帧");
        Check(ParseFrame(MakeFrame("open browser")) == "open browser", "解析英文语音帧");
        Check(ParseFrame(new byte[] { 0x03, 0x01, 0x41 }) == null, "非STX帧被拒");
        Check(ParseFrame(new byte[] { 0x02, 0x05, 0x41 }) == null, "长度越界帧被拒");

        Console.WriteLine("== RESULT: " + pass + " passed, " + fail + " failed ==");
        Environment.Exit(fail == 0 ? 0 : 1);
    }
}
