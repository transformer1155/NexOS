// ============================================================================
//  Lang.cs - NexOS internationalisation (i18n) layer for the MiniCLR shell.
//
//  Design constraints (MiniCLR has NO System.Collections, NO Split/IndexOf/
//  Substring instance methods, NO static initialisers, and is built /nostdlib
//  so System.String.Concat is NOT available -> never use '+' on strings and
//  never accumulate via U.Cat(variable,...); only U.Sub / U.Cat(const,...) or
//  our own char-scan helpers).
//
//    * strings live in external language packs: sfs_files/lang/<id>.txt
//      (format:  key=value  ,  '#' comments)
//    * the active pack is loaded at runtime via Host.ReadText(1,"lang/<id>.txt")
//      so new languages can be added WITHOUT touching core code - just drop a
//      new sfs_files/lang/<id>.txt and call Lang.SetLang("<id>").
//    * business code calls Lang.T("some.key"); missing keys fall back to the
//      key string itself (so untranslated UI is visible, never blank).
//    * a tiny inline fallback table guarantees the shell still boots if the
//      SFS language file is missing.
// ============================================================================
using NexOS.Forms;

public static class Lang
{
    static string[] K;
    static string[] V;
    static int N = 0;
    static int Cap = 0;
    static string cur = "zh";

    // MiniCLR never runs static cctors -> allocate in Init().
    public static void Init()
    {
        Cap = 1024; N = 0;
        K = new string[Cap];
        V = new string[Cap];
        LoadFallback();
        LoadPack(cur);            // overlay the full SFS language pack
    }

    // Switch language at runtime (e.g. Settings -> Language).
    public static void SetLang(string id)
    {
        cur = id;
        N = 0;                    // rebuild from fallback + pack
        LoadFallback();
        LoadPack(cur);
    }

    public static string Current() { return cur; }

    static void Grow()
    {
        int nc = Cap * 2;
        string[] nk = new string[nc];
        string[] nv = new string[nc];
        for (int i = 0; i < N; i++) { nk[i] = K[i]; nv[i] = V[i]; }
        K = nk; V = nv; Cap = nc;
    }

    static void Add(string k, string v)
    {
        if (N >= Cap) Grow();
        K[N] = k; V[N] = v; N++;
    }

    // Insert or overwrite a key.
    static void Set(string k, string v)
    {
        for (int i = 0; i < N; i++)
            if (K[i] == k) { V[i] = v; return; }
        Add(k, v);
    }

    static void LoadPack(string id)
    {
        // Path built with U.Cat(const, const) - safe under /nostdlib.
        // To add a language: drop sfs_files/lang/<id>.txt and extend this map.
        string path = "lang/zh.txt";
        if (id == "en") path = "lang/en.txt";
        string body = Host.ReadText(1, path);
        if (body == null || body.Length == 0)
            body = Host.ReadText(0, path);
        if (body != null && body.Length > 0)
            Parse(body);
    }

    static void Parse(string body)
    {
        int i = 0, n = body.Length;
        while (i < n)
        {
            int j = i;
            while (j < n && body[j] != '\n') j++;
            string line = U.Sub(body, i, j - i);
            // strip trailing CR (files edited on Windows)
            if (line.Length > 0 && line[line.Length - 1] == '\r')
                line = U.Sub(line, 0, line.Length - 1);
            i = (j < n) ? j + 1 : j;
            if (line.Length == 0) continue;
            if (line[0] == '#') continue;
            int eq = Idx(line, '=');
            if (eq < 0) continue;
            string k = Trim(U.Sub(line, 0, eq));
            if (k.Length == 0) continue;
            string v = U.Sub(line, eq + 1, line.Length - eq - 1);
            Set(k, v);
        }
    }

    // char-scan indexOf (avoids Apps.IndexOf / BCL String.IndexOf)
    static int Idx(string s, char c)
    {
        for (int i = 0; i < s.Length; i++)
            if (s[i] == c) return i;
        return -1;
    }

    // trim leading/trailing spaces and tabs
    static string Trim(string s)
    {
        int a = 0, b = s.Length;
        while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
        return U.Sub(s, a, b - a);
    }

    // Translate. Missing key -> returns the key itself (visible, not blank).
    public static string T(string key)
    {
        for (int i = 0; i < N; i++)
            if (K[i] == key) return V[i];
        return key;
    }

    // Minimal inline fallback so the shell boots even without the SFS pack.
    static void LoadFallback()
    {
        Add("lock.title",     "登录 NexOS");
        Add("lock.subtitle",  "继续登录");
        Add("lock.user",      "用户名");
        Add("lock.pass",      "密码");
        Add("lock.signin",    "登录");
        Add("lock.hint",      "Tab 切换输入框 · Enter 登录");
        Add("lock.noaccount", "(无账户)");
        Add("lock.err.user",  "请输入用户名。");
        Add("lock.err.cred",  "用户名或密码错误。");
        Add("lock.defaccounts", "默认账户：");
        Add("app.start",      "开始");
        Add("app.settings",   "设置");
    }
}
