// =====================================================================
//  PackageSupport.cs  -  PackageInstaller 的支撑组件
// ---------------------------------------------------------------------
//  这里有四个小模块：
//
//    ZipReader       最小 ZIP 读取器（只解析中央目录 + 按需读 STORE 成员）
//    ManifestParser  manifest.json -> NexpackManifest（不依赖 System.Text.Json）
//    RegistryMerge   按 app_id 的 JSON 数组 upsert/remove（纯字符串操作）
//    RegistryQuery   从注册表 JSON 里取某个 app 的字段
//
//  为什么自己写而不是用 System.Text.Json / System.IO.Compression？
//    * 安装器要能在内部核 MiniCLR 侧运行，那里没有这两个库；
//    * 我们只需要 STORE 成员（无解压），ZIP 中央目录格式固定且简单；
//    * manifest / 注册表都是我们自己的小 schema，手写解析比引入
//      序列化库更可控，也避免「未知字段导致整体失败」。
//
//  注意：宿主侧（WinHost）也复用同一份实现，保证行为一致。
// =====================================================================
using System;

namespace NexOS.Install
{
    // =================================================================
    //  ZipReader - 最小 ZIP 读取器（只支持 STORE）
    // =================================================================
    public class ZipReader
    {
        byte[] d;
        int count;
        string[] names;
        int[] methods;
        int[] offsets;
        int[] sizes;

        public int Count { get { return count; } }
        public string NameAt(int i) { return names[i]; }
        public int MethodAt(int i) { return methods[i]; }

        public ZipReader(byte[] data)
        {
            d = data;
            // 找 End Of Central Directory（从尾部向前找签名 0x06054b50）
            int eocd = -1;
            int limit = data.Length - 22;
            for (int i = limit; i >= 0 && i >= limit - 65536; i--)
            {
                if (i + 4 <= data.Length &&
                    data[i] == 0x50 && data[i + 1] == 0x4b &&
                    data[i + 2] == 0x05 && data[i + 3] == 0x06)
                {
                    eocd = i;
                    break;
                }
            }
            if (eocd < 0) throw new Exception("no End Of Central Directory");

            int total = U16(eocd + 10);
            int cdOffset = (int)U32(eocd + 16);
            count = total;
            names = new string[total];
            methods = new int[total];
            offsets = new int[total];
            sizes = new int[total];

            int p = cdOffset;
            for (int k = 0; k < total; k++)
            {
                if (p + 46 > data.Length) throw new Exception("truncated central directory");
                if (!(data[p] == 0x50 && data[p + 1] == 0x4b &&
                      data[p + 2] == 0x01 && data[p + 3] == 0x02))
                    throw new Exception("bad central directory signature at " + p);

                methods[k] = U16(p + 10);
                int compSize = (int)U32(p + 20);
                int uncompSize = (int)U32(p + 24);
                int nameLen = U16(p + 28);
                int extraLen = U16(p + 30);
                int commentLen = U16(p + 32);
                int localOff = (int)U32(p + 42);

                names[k] = Utf8(data, p + 46, nameLen);
                // STORE 下压缩前后等长；用未压缩大小做边界检查
                sizes[k] = uncompSize > 0 ? uncompSize : compSize;
                offsets[k] = localOff;

                p += 46 + nameLen + extraLen + commentLen;
            }
        }

        public int IndexOf(string name)
        {
            for (int i = 0; i < count; i++)
                if (names[i] == name) return i;
            return -1;
        }

        // 读取成员原始字节（只支持 STORE）。
        public byte[] DataAt(int i)
        {
            int off = offsets[i];
            if (off + 30 > d.Length) throw new Exception("truncated local header");
            if (!(d[off] == 0x50 && d[off + 1] == 0x4b &&
                  d[off + 2] == 0x03 && d[off + 3] == 0x04))
                throw new Exception("bad local header signature");

            int method = U16(off + 8);
            if (method != 0) throw new Exception("member is compressed (method " + method + ")");
            int compSize = (int)U32(off + 18);
            int nameLen = U16(off + 26);
            int extraLen = U16(off + 28);

            int dataStart = off + 30 + nameLen + extraLen;
            int len = compSize > 0 ? compSize : sizes[i];
            if (dataStart + len > d.Length) throw new Exception("member data out of range");

            byte[] r = new byte[len];
            for (int k = 0; k < len; k++) r[k] = d[dataStart + k];
            return r;
        }

        // ---- 小端读取 ------------------------------------------------
        int U16(int o)
        {
            if (o + 2 > d.Length) throw new Exception("read past end");
            return d[o] | (d[o + 1] << 8);
        }

        uint U32(int o)
        {
            if (o + 4 > d.Length) throw new Exception("read past end");
            return (uint)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24));
        }

        // UTF-8 -> string。MiniCLR 的字符串本身就是 UTF-8 字节数组，
        // 这里仍用手写解码以保持宿主/内核行为一致。
        static string Utf8(byte[] b, int off, int len)
        {
            char[] c = new char[len];
            int n = 0;
            int i = off, end = off + len;
            while (i < end)
            {
                int c0 = b[i++];
                if (c0 < 0x80) { c[n++] = (char)c0; }
                else if ((c0 & 0xE0) == 0xC0 && i < end)
                {
                    int c1 = b[i++];
                    c[n++] = (char)(((c0 & 0x1F) << 6) | (c1 & 0x3F));
                }
                else if ((c0 & 0xF0) == 0xE0 && i + 1 < end)
                {
                    int c1 = b[i++], c2 = b[i++];
                    c[n++] = (char)(((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F));
                }
                else if ((c0 & 0xF8) == 0xF0 && i + 2 < end)
                {
                    int c1 = b[i++], c2 = b[i++], c3 = b[i++];
                    int cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                             ((c2 & 0x3F) << 6) | (c3 & 0x3F);
                    // 落在 BMP 内（U+10000 以上用替代对；此处简化处理）
                    if (cp >= 0x10000) cp = '?';
                    c[n++] = (char)cp;
                }
                else { c[n++] = '?'; }
            }
            char[] t = new char[n];
            for (int k = 0; k < n; k++) t[k] = c[k];
            return new string(t, 0, n);
        }
    }

    // =================================================================
    //  ManifestParser - 最小 JSON 解析（只取安装器需要的字段）
    // =================================================================
    public static class ManifestParser
    {
        public static NexpackManifest Parse(byte[] jsonBytes)
        {
            string s = FromUtf8(jsonBytes);
            return Parse(s);
        }

        public static NexpackManifest Parse(string s)
        {
            NexpackManifest m = new NexpackManifest();
            if (s == null) throw new Exception("manifest is empty");

            m.schema_version = (int)JsonNum(s, "schema_version", 0);
            m.app_id = JsonStr(s, "app_id", "");
            m.name = JsonStr(s, "name", "");
            m.version = JsonStr(s, "version", "");
            m.author = JsonStr(s, "author", "");
            m.description = JsonStr(s, "description", "");
            m.icon = JsonStr(s, "icon", "icon.png");
            m.ai_mode_required = JsonStr(s, "ai_mode_required", "Balanced");

            // entry 子对象
            string entry = JsonObj(s, "entry");
            if (entry != null)
            {
                m.entry_kind = (int)JsonNum(entry, "kind", 13);
                m.entry_desktop = JsonBool(entry, "desktop", true);
                m.entry_taskbar = JsonBool(entry, "taskbar", false);
            }

            // mcp_server 数组：保留原始 JSON 供注册表使用，同时抽出 tool_name
            string srv = JsonArr(s, "mcp_server");
            if (srv != null)
            {
                m.mcp_server_json = srv;
                m.tool_names = ExtractToolNames(srv);
            }
            string acc = JsonArr(s, "mcp_client_access");
            if (acc != null) m.mcp_access_json = acc;

            return m;
        }

        // 转成 apps.json 里的一个 app 条目（字段顺序与 apps.json 对齐）
        public static string ToRegistryEntry(NexpackManifest m)
        {
            string s = "{";
            s += "\"app_id\":\"" + Esc(m.app_id) + "\",";
            s += "\"name\":\"" + Esc(m.name) + "\",";
            s += "\"version\":\"" + Esc(m.version) + "\",";
            s += "\"author\":\"" + Esc(m.author) + "\",";
            s += "\"ai_mode_required\":\"" + Esc(m.ai_mode_required) + "\",";
            s += "\"mcp_server\":" + (m.mcp_server_json == null ? "[]" : m.mcp_server_json) + ",";
            s += "\"mcp_client_access\":" + (m.mcp_access_json == null ? "[]" : m.mcp_access_json);
            s += "}";
            return s;
        }

        // ---- 从 mcp_server 数组文本里抽出所有 tool_name --------------
        static string[] ExtractToolNames(string arr)
        {
            string[] tmp = new string[32];
            int n = 0;
            int i = 0;
            while (i < arr.Length)
            {
                int k = IndexOfFrom(arr, "\"tool_name\"", i);
                if (k < 0) break;
                int colon = IndexOfFrom(arr, ":", k);
                if (colon < 0) break;
                int q1 = IndexOfFrom(arr, "\"", colon);
                if (q1 < 0) break;
                int q2 = IndexOfFrom(arr, "\"", q1 + 1);
                if (q2 < 0) break;
                if (n < tmp.Length) tmp[n++] = Sub(arr, q1 + 1, q2 - q1 - 1);
                i = q2 + 1;
            }
            string[] r = new string[n];
            for (int j = 0; j < n; j++) r[j] = tmp[j];
            return r;
        }

        // ---- JSON 取值辅助 ------------------------------------------
        public static string JsonStr(string s, string key, string def)
        {
            int k = IndexOfFrom(s, "\"" + key + "\"", 0);
            if (k < 0) return def;
            int colon = IndexOfFrom(s, ":", k);
            if (colon < 0) return def;
            int i = colon + 1;
            while (i < s.Length && (s[i] == ' ' || s[i] == '\t' ||
                                    s[i] == '\r' || s[i] == '\n')) i++;
            if (i >= s.Length || s[i] != '"') return def;
            i++;
            int start = i;
            bool esc = false;
            string out_s = "";
            while (i < s.Length)
            {
                char c = s[i];
                if (esc)
                {
                    if (c == 'n') out_s = out_s + "\n";
                    else if (c == 't') out_s = out_s + "\t";
                    else if (c == 'r') out_s = out_s + "\r";
                    else out_s = out_s + c;
                    esc = false;
                    i++;
                    continue;
                }
                if (c == '\\') { esc = true; i++; continue; }
                if (c == '"') break;
                out_s = out_s + c;
                i++;
            }
            return out_s;
        }

        public static double JsonNum(string s, string key, double def)
        {
            int k = IndexOfFrom(s, "\"" + key + "\"", 0);
            if (k < 0) return def;
            int colon = IndexOfFrom(s, ":", k);
            if (colon < 0) return def;
            int i = colon + 1;
            while (i < s.Length && (s[i] == ' ' || s[i] == '\t' ||
                                    s[i] == '\r' || s[i] == '\n')) i++;
            int start = i;
            if (i < s.Length && (s[i] == '-' || s[i] == '+')) i++;
            while (i < s.Length)
            {
                char c = s[i];
                if ((c >= '0' && c <= '9') || c == '.') i++;
                else break;
            }
            if (i == start) return def;
            return ParseDouble(Sub(s, start, i - start), def);
        }

        public static bool JsonBool(string s, string key, bool def)
        {
            int k = IndexOfFrom(s, "\"" + key + "\"", 0);
            if (k < 0) return def;
            int colon = IndexOfFrom(s, ":", k);
            if (colon < 0) return def;
            int i = colon + 1;
            while (i < s.Length && (s[i] == ' ' || s[i] == '\t' ||
                                    s[i] == '\r' || s[i] == '\n')) i++;
            if (i + 4 <= s.Length && Sub(s, i, 4) == "true") return true;
            if (i + 5 <= s.Length && Sub(s, i, 5) == "false") return false;
            return def;
        }

        // 取子对象文本 `"key": { ... }`（含大括号配平）
        public static string JsonObj(string s, string key)
        {
            int k = IndexOfFrom(s, "\"" + key + "\"", 0);
            if (k < 0) return null;
            int colon = IndexOfFrom(s, ":", k);
            if (colon < 0) return null;
            int i = colon + 1;
            while (i < s.Length && s[i] != '{') i++;
            if (i >= s.Length) return null;
            int close = MatchBrace(s, i, '{', '}');
            if (close < 0) return null;
            return Sub(s, i, close - i + 1);
        }

        // 取数组文本 `"key": [ ... ]`（含方括号配平）
        public static string JsonArr(string s, string key)
        {
            int k = IndexOfFrom(s, "\"" + key + "\"", 0);
            if (k < 0) return null;
            int colon = IndexOfFrom(s, ":", k);
            if (colon < 0) return null;
            int i = colon + 1;
            while (i < s.Length && s[i] != '[') i++;
            if (i >= s.Length) return null;
            int close = MatchBrace(s, i, '[', ']');
            if (close < 0) return null;
            return Sub(s, i, close - i + 1);
        }

        static int MatchBrace(string s, int start, char o, char c)
        {
            int depth = 0;
            bool inStr = false, esc = false;
            for (int i = start; i < s.Length; i++)
            {
                char ch = s[i];
                if (inStr)
                {
                    if (esc) { esc = false; continue; }
                    if (ch == '\\') { esc = true; continue; }
                    if (ch == '"') inStr = false;
                    continue;
                }
                if (ch == '"') { inStr = true; continue; }
                if (ch == o) depth++;
                else if (ch == c)
                {
                    depth--;
                    if (depth == 0) return i;
                }
            }
            return -1;
        }

        // ---- 字符串工具 ---------------------------------------------
        public static int IndexOfFrom(string s, string pat, int from)
        {
            if (s == null) return -1;
            int n = s.Length - pat.Length;
            for (int i = from; i <= n; i++)
            {
                int k = 0;
                while (k < pat.Length && s[i + k] == pat[k]) k++;
                if (k == pat.Length) return i;
            }
            return -1;
        }

        public static string Sub(string s, int start, int len)
        {
            if (s == null || len <= 0) return "";
            if (start < 0) start = 0;
            if (start + len > s.Length) len = s.Length - start;
            return s.Substring(start, len);
        }

        public static string Esc(string s)
        {
            if (s == null) return "";
            string r = "";
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (c == '"') r = r + "\\\"";
                else if (c == '\\') r = r + "\\\\";
                else if (c == '\n') r = r + "\\n";
                else if (c == '\r') r = r + "\\r";
                else if (c == '\t') r = r + "\\t";
                else r = r + c;
            }
            return r;
        }

        static double ParseDouble(string s, double def)
        {
            // 只需支持非负整数（kind / schema_version），小数用不到。
            double v = 0;
            bool neg = false;
            int i = 0;
            if (i < s.Length && s[i] == '-') { neg = true; i++; }
            bool any = false;
            for (; i < s.Length; i++)
            {
                char c = s[i];
                if (c < '0' || c > '9') break;
                v = v * 10 + (c - '0');
                any = true;
            }
            if (!any) return def;
            return neg ? -v : v;
        }

        public static string FromUtf8(byte[] b)
        {
            if (b == null) return "";
            char[] c = new char[b.Length];
            int n = 0;
            int i = 0;
            while (i < b.Length)
            {
                int c0 = b[i++];
                if (c0 < 0x80) c[n++] = (char)c0;
                else if ((c0 & 0xE0) == 0xC0 && i < b.Length)
                {
                    int c1 = b[i++];
                    c[n++] = (char)(((c0 & 0x1F) << 6) | (c1 & 0x3F));
                }
                else if ((c0 & 0xF0) == 0xE0 && i + 1 < b.Length)
                {
                    int c1 = b[i++], c2 = b[i++];
                    c[n++] = (char)(((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F));
                }
                else if ((c0 & 0xF8) == 0xF0 && i + 2 < b.Length)
                {
                    i += 3;
                    c[n++] = '?';    // 星体平面：本项目不涉及
                }
                else c[n++] = '?';
            }
            char[] t = new char[n];
            for (int k = 0; k < n; k++) t[k] = c[k];
            return new string(t, 0, n);
        }
    }

    // =================================================================
    //  RegistryMerge - 按 app_id 对 apps.json 做 upsert / remove
    // =================================================================
    public static class RegistryMerge
    {
        // 把 entry（一个 JSON 对象文本）插入/替换 apps 数组里同 app_id 的项。
        public static string Upsert(string registryJson, string app_id, string entryJson)
        {
            if (registryJson == null || registryJson.Length == 0)
                registryJson = "{\"schema_version\":1,\"apps\":[]}";

            string apps = ManifestParser.JsonArr(registryJson, "apps");
            if (apps == null)
            {
                // 没有 apps 数组：补一个
                return InsertAppsArray(registryJson, "[" + entryJson + "]");
            }

            // 拆成条目列表
            string[] items;
            int n;
            SplitArray(apps, out items, out n);

            // 找同 app_id 的条目并替换，否则追加
            bool replaced = false;
            string outArr = "[";
            bool firstOut = true;
            for (int i = 0; i < n; i++)
            {
                string it = items[i];
                string id = ManifestParser.JsonStr(it, "app_id", "");
                if (id == app_id)
                {
                    it = entryJson;
                    replaced = true;
                }
                if (!firstOut) outArr += ",";
                outArr += it;
                firstOut = false;
            }
            if (!replaced)
            {
                if (!firstOut) outArr += ",";
                outArr += entryJson;
            }
            outArr += "]";

            return ReplaceAppsArray(registryJson, apps, outArr);
        }

        // 从 apps 数组里移除 app_id 对应的条目。
        public static string Remove(string registryJson, string app_id)
        {
            string apps = ManifestParser.JsonArr(registryJson, "apps");
            if (apps == null) return registryJson;

            string[] items;
            int n;
            SplitArray(apps, out items, out n);

            string outArr = "[";
            bool firstOut = true;
            for (int i = 0; i < n; i++)
            {
                string it = items[i];
                string id = ManifestParser.JsonStr(it, "app_id", "");
                if (id == app_id) continue;
                if (!firstOut) outArr += ",";
                outArr += it;
                firstOut = false;
            }
            outArr += "]";
            return ReplaceAppsArray(registryJson, apps, outArr);
        }

        // 把 JSON 数组文本拆成元素（顶层对象/字符串）
        static void SplitArray(string arr, out string[] items, out int n)
        {
            string[] tmp = new string[64];
            int count = 0;
            int i = 0;
            int len = arr.Length;
            while (i < len)
            {
                // 跳到下一个元素起点
                while (i < len && (arr[i] == '[' || arr[i] == ',' ||
                                   arr[i] == ' ' || arr[i] == '\t' ||
                                   arr[i] == '\r' || arr[i] == '\n')) i++;
                if (i >= len || arr[i] == ']') break;

                int start = i;
                int end;
                if (arr[i] == '{')
                {
                    end = MatchBrace(arr, i, '{', '}');
                    if (end < 0) break;
                    i = end + 1;
                }
                else if (arr[i] == '"')
                {
                    i++;
                    bool esc = false;
                    while (i < len)
                    {
                        if (esc) { esc = false; i++; continue; }
                        if (arr[i] == '\\') { esc = true; i++; continue; }
                        if (arr[i] == '"') { i++; break; }
                        i++;
                    }
                    end = i - 1;
                }
                else
                {
                    while (i < len && arr[i] != ',' && arr[i] != ']') i++;
                    end = i - 1;
                }
                if (count < tmp.Length)
                    tmp[count++] = ManifestParser.Sub(arr, start, end - start + 1);
            }
            items = tmp;
            n = count;
        }

        static int MatchBrace(string s, int start, char o, char c)
        {
            int depth = 0;
            bool inStr = false, esc = false;
            for (int i = start; i < s.Length; i++)
            {
                char ch = s[i];
                if (inStr)
                {
                    if (esc) { esc = false; continue; }
                    if (ch == '\\') { esc = true; continue; }
                    if (ch == '"') inStr = false;
                    continue;
                }
                if (ch == '"') { inStr = true; continue; }
                if (ch == o) depth++;
                else if (ch == c)
                {
                    depth--;
                    if (depth == 0) return i;
                }
            }
            return -1;
        }

        static string ReplaceAppsArray(string registryJson, string oldArr, string newArr)
        {
            int k = ManifestParser.IndexOfFrom(registryJson, oldArr, 0);
            if (k < 0) return registryJson;
            return ManifestParser.Sub(registryJson, 0, k) + newArr +
                   ManifestParser.Sub(registryJson, k + oldArr.Length,
                       registryJson.Length - k - oldArr.Length);
        }

        static string InsertAppsArray(string registryJson, string arr)
        {
            int close = registryJson.LastIndexOf('}');
            if (close < 0) return "{\"schema_version\":1,\"apps\":" + arr + "}";
            string head = ManifestParser.Sub(registryJson, 0, close);
            // 判断是否需要补逗号
            int j = head.Length - 1;
            while (j >= 0 && (head[j] == ' ' || head[j] == '\t' ||
                              head[j] == '\r' || head[j] == '\n')) j--;
            bool needComma = j >= 0 && head[j] != '{' && head[j] != ',';
            return head + (needComma ? "," : "") +
                   "\"apps\":" + arr +
                   ManifestParser.Sub(registryJson, close,
                       registryJson.Length - close);
        }
    }

    // =================================================================
    //  RegistryQuery - 从注册表 JSON 取字段
    // =================================================================
    public static class RegistryQuery
    {
        // 找 app_id 对应条目的文本
        static string FindEntry(string reg, string app_id)
        {
            string apps = ManifestParser.JsonArr(reg, "apps");
            if (apps == null) return null;
            int i = 0;
            while (i < apps.Length)
            {
                int k = ManifestParser.IndexOfFrom(apps, "\"app_id\"", i);
                if (k < 0) break;
                int colon = ManifestParser.IndexOfFrom(apps, ":", k);
                if (colon < 0) break;
                int q1 = ManifestParser.IndexOfFrom(apps, "\"", colon);
                if (q1 < 0) break;
                int q2 = ManifestParser.IndexOfFrom(apps, "\"", q1 + 1);
                if (q2 < 0) break;
                string id = ManifestParser.Sub(apps, q1 + 1, q2 - q1 - 1);
                if (id == app_id)
                {
                    // 找该 app_id 所属的对象范围
                    int objStart = apps.LastIndexOf('{', k);
                    if (objStart < 0) objStart = k;
                    int objEnd = MatchBrace(apps, objStart);
                    if (objEnd < 0) objEnd = apps.Length - 1;
                    return ManifestParser.Sub(apps, objStart, objEnd - objStart + 1);
                }
                i = q2 + 1;
            }
            return null;
        }

        public static string Field(string reg, string app_id, string field)
        {
            string e = FindEntry(reg, app_id);
            if (e == null) return null;
            int k = ManifestParser.IndexOfFrom(e, "\"" + field + "\"", 0);
            if (k < 0) return null;
            return ManifestParser.JsonStr(e, field, null);
        }

        public static string[] AppIds(string reg)
        {
            string apps = ManifestParser.JsonArr(reg, "apps");
            if (apps == null) return new string[0];
            string[] tmp = new string[64];
            int n = 0;
            int i = 0;
            while (i < apps.Length)
            {
                int k = ManifestParser.IndexOfFrom(apps, "\"app_id\"", i);
                if (k < 0) break;
                int colon = ManifestParser.IndexOfFrom(apps, ":", k);
                if (colon < 0) break;
                int q1 = ManifestParser.IndexOfFrom(apps, "\"", colon);
                if (q1 < 0) break;
                int q2 = ManifestParser.IndexOfFrom(apps, "\"", q1 + 1);
                if (q2 < 0) break;
                if (n < tmp.Length)
                    tmp[n++] = ManifestParser.Sub(apps, q1 + 1, q2 - q1 - 1);
                i = q2 + 1;
            }
            string[] r = new string[n];
            for (int j = 0; j < n; j++) r[j] = tmp[j];
            return r;
        }

        static int MatchBrace(string s, int start)
        {
            int depth = 0;
            bool inStr = false, esc = false;
            for (int i = start; i < s.Length; i++)
            {
                char ch = s[i];
                if (inStr)
                {
                    if (esc) { esc = false; continue; }
                    if (ch == '\\') { esc = true; continue; }
                    if (ch == '"') inStr = false;
                    continue;
                }
                if (ch == '"') { inStr = true; continue; }
                if (ch == '{') depth++;
                else if (ch == '}')
                {
                    depth--;
                    if (depth == 0) return i;
                }
            }
            return -1;
        }
    }
}
