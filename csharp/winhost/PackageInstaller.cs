// =====================================================================
//  PackageInstaller.cs  -  .nexpack 应用包安装器
// ---------------------------------------------------------------------
//  对应 VS Code 的 ExtensionInstaller。安装流程（见 docs/NEXPACK.md §5）：
//
//     .nexpack
//        │
//        ├─ 1. 读 ZIP 目录，校验成员名（单段、禁路径穿越）
//        ├─ 2. 读 manifest.json，校验 schema_version / app_id / 字段
//        ├─ 3. 校验 app.mex 魔数 MEX1
//        ├─ 4. 写入 MKFS：<app_id>.mex、<app_id>.icon
//        ├─ 5. 合并注册表 mkfs/apps.overlay.json（overlay，按 app_id 合并）
//        ├─ 6. entry.desktop -> 写 mkfs/Desktop/<Name>.lnk
//        └─ 7. 刷新桌面
//
//  设计要点
//   * 阶段 A（只读校验）失败 => 磁盘零改动；阶段 B 落盘失败 => 尽力回滚。
//   * ZIP 成员必须是 STORE（内核无 inflate）；DEFLATE 包直接拒绝。
//   * 注册表是 **overlay**：读现有 mkfs/apps.overlay.json（若有）+ 内置 apps.json，
//     按 app_id 覆盖/新增，绝不整体覆盖内置应用。
//   * MiniCLR 兼容子集：不用 LINQ / 反射 / try-catch 之外的高级特性。
//     但本文件主要在宿主侧运行（解 ZIP 需要 System.IO.Compression）。
// =====================================================================
using System;

namespace NexOS.Install
{
    // 安装结果码（与 McpResult 风格一致：0 = 成功）
    public static class InstallResult
    {
        public const int OK             = 0;
        public const int ERR_ARG        = 1;   // 参数/路径非法
        public const int ERR_OPEN       = 2;   // 打不开包
        public const int ERR_ZIP        = 3;   // ZIP 结构非法
        public const int ERR_COMPRESSED = 4;   // 成员被压缩（内核无 inflate）
        public const int ERR_MANIFEST   = 5;   // manifest 缺失/非法
        public const int ERR_MEX        = 6;   // app.mex 缺失/魔数错
        public const int ERR_RESERVED   = 7;   // app_id 是内置保留名
        public const int ERR_TOO_BIG    = 8;   // 超过体积上限
        public const int ERR_IO         = 9;   // 写盘失败
        public const int ERR_REGISTRY   = 10;  // 注册表合并失败
    }

    // 安装报告：调用方据此显示 UI / 打印日志。
    public class InstallReport
    {
        public int    code;           // InstallResult.*
        public string message;        // 人类可读说明
        public string app_id;
        public string app_name;
        public string version;
        public string old_version;    // 升级前的版本（新装为 null）
        public int    tools;          // 注册的 MCP 工具数
        public bool   desktop_added;  // 是否新增了桌面快捷方式
        public bool[] rolled_back;    // 是否执行了回滚

        public InstallReport()
        {
            code = InstallResult.OK;
            message = "";
            app_id = "";
            app_name = "";
            version = "";
            old_version = null;
            tools = 0;
            desktop_added = false;
            rolled_back = new bool[1];
        }

        public bool Ok { get { return code == InstallResult.OK; } }

        public string Summary()
        {
            if (!Ok) return "install failed (" + code + "): " + message;
            string s = "installed " + app_id + " " + version;
            if (old_version != null)
                s = "upgraded " + app_id + " " + old_version + " -> " + version;
            if (tools > 0)
                s = s + ", " + tools + " MCP tool(s)";
            if (desktop_added)
                s = s + ", desktop shortcut added";
            return s;
        }
    }

    // =================================================================
    //  NexpackManifest - manifest.json 的内存表示
    // =================================================================
    //  只保留安装器需要的字段。JSON 解析由调用方注入的 IPackageIo 完成，
    //  这样宿主侧可用 System.Text.Json，MiniCLR 侧可换成手写解析器。
    public class NexpackManifest
    {
        public int    schema_version;
        public string app_id;
        public string name;
        public string version;
        public string author;
        public string description;
        public string icon;              // 归档内图标成员名
        public int    entry_kind;        // 传给 Host.OpenApp 的 Kind
        public bool   entry_desktop;
        public bool   entry_taskbar;
        public string ai_mode_required;  // "Fast" / "Balanced" / "Heavy"
        public string[] tool_names;      // MCP 工具名（用于报告/注册）
        public string mcp_server_json;   // 原始 mcp_server 数组 JSON 文本
        public string mcp_access_json;   // 原始 mcp_client_access 数组 JSON 文本

        public NexpackManifest()
        {
            schema_version = 0;
            app_id = "";
            name = "";
            version = "";
            author = "";
            description = "";
            icon = "icon.png";
            entry_kind = 13;
            entry_desktop = true;
            entry_taskbar = false;
            ai_mode_required = "Balanced";
            tool_names = new string[0];
            mcp_server_json = "[]";
            mcp_access_json = "[]";
        }
    }

    public interface IPackageIo
    {
        bool FileExists(string path);
        long FileSize(string path);
        byte[] ReadAll(string path);
        bool WriteAll(string path, byte[] data);
        bool WriteText(string path, string text);
        string ReadText(string path);
        bool MakeDir(string path);
        bool Delete(string path);
        string[] ListFiles(string dir);
        string Combine(string a, string b);
        string GetFileName(string path);
    }

    // =================================================================
    //  PackageInstaller
    // =================================================================
    public class PackageInstaller
    {
        // 与 tools/nexpack.py 保持一致的上限
        public const long MAX_PACKAGE_BYTES = 4L * 1024 * 1024;
        public const long MAX_MEX_BYTES     = 2L * 1024 * 1024;
        public const long MAX_ICON_BYTES    = 64L * 1024;

        const string MEX_MAGIC = "MEX1";     // 4 字节魔数

        // 安装产物注册表文件名。必须与内核 nexos_ai.cpp 的
        // ai_gateway_init 里读的名字一致（见 docs/NEXPACK.md §5.1）。
        // 刻意不叫 "apps.json"：kern_fs_read() 是扁平查找且 MKFS 优先，
        // 同名会遮蔽 SFS 上的系统 manifest。
        public const string OVERLAY_NAME = "apps.overlay.json";

        static readonly string[] RESERVED = new string[]
        {
            "weather", "assistant", "shell", "nexos"
        };

        IPackageIo io;
        string mkfsRoot;      // MKFS 根目录（宿主侧是真实文件夹）
        string desktopDir;    // MKFS/Desktop
        bool force;

        public PackageInstaller(IPackageIo io, string mkfsRoot, string desktopDir)
        {
            this.io = io;
            this.mkfsRoot = mkfsRoot;
            this.desktopDir = desktopDir;
            this.force = false;
        }

        public void SetForce(bool v) { force = v; }

        // -------------------------------------------------------------
        //  公共入口：安装一个 .nexpack
        // -------------------------------------------------------------
        public InstallReport Install(string packagePath)
        {
            InstallReport r = new InstallReport();
            if (packagePath == null || packagePath.Length == 0)
            {
                r.code = InstallResult.ERR_ARG;
                r.message = "package path is empty";
                return r;
            }
            if (!io.FileExists(packagePath))
            {
                r.code = InstallResult.ERR_ARG;
                r.message = "package not found: " + packagePath;
                return r;
            }
            long size = io.FileSize(packagePath);
            if (size <= 0 || size > MAX_PACKAGE_BYTES)
            {
                r.code = InstallResult.ERR_TOO_BIG;
                r.message = "package size " + size + " out of range (max "
                            + MAX_PACKAGE_BYTES + ")";
                return r;
            }

            byte[] pkg = io.ReadAll(packagePath);
            if (pkg == null || pkg.Length == 0)
            {
                r.code = InstallResult.ERR_OPEN;
                r.message = "cannot read package";
                return r;
            }

            // ---- 阶段 A：只读校验 ----------------------------------
            ZipReader zip = null;
            try
            {
                zip = new ZipReader(pkg);
            }
            catch (Exception ex)
            {
                r.code = InstallResult.ERR_ZIP;
                r.message = "bad ZIP: " + ex.Message;
                return r;
            }

            // 1) 成员名必须单段
            for (int i = 0; i < zip.Count; i++)
            {
                string nm = zip.NameAt(i);
                if (!IsSafeMemberName(nm))
                {
                    r.code = InstallResult.ERR_ZIP;
                    r.message = "unsafe member name: " + nm;
                    return r;
                }
            }

            // 2) 必须 STORE，否则内核读不了
            for (int i = 0; i < zip.Count; i++)
            {
                if (zip.MethodAt(i) != 0)
                {
                    r.code = InstallResult.ERR_COMPRESSED;
                    r.message = "member " + zip.NameAt(i) + " is compressed (method "
                                + zip.MethodAt(i) + "); the kernel has no inflate";
                    return r;
                }
            }

            // 3) manifest.json 必需
            int mi = zip.IndexOf("manifest.json");
            if (mi < 0)
            {
                r.code = InstallResult.ERR_MANIFEST;
                r.message = "missing manifest.json";
                return r;
            }
            NexpackManifest mf;
            try
            {
                mf = ManifestParser.Parse(zip.DataAt(mi));
            }
            catch (Exception ex)
            {
                r.code = InstallResult.ERR_MANIFEST;
                r.message = "manifest parse: " + ex.Message;
                return r;
            }
            string bad = ValidateManifest(mf);
            if (bad != null)
            {
                r.code = InstallResult.ERR_MANIFEST;
                r.message = bad;
                return r;
            }
            if (!force && IsReserved(mf.app_id))
            {
                r.code = InstallResult.ERR_RESERVED;
                r.message = "app_id '" + mf.app_id +
                            "' is a built-in reserved name (use force to override)";
                return r;
            }

            // 4) app.mex 必需 + 魔数
            int xi = zip.IndexOf("app.mex");
            if (xi < 0)
            {
                r.code = InstallResult.ERR_MEX;
                r.message = "missing app.mex";
                return r;
            }
            byte[] mex = zip.DataAt(xi);
            string mexBad = ValidateMex(mex);
            if (mexBad != null)
            {
                r.code = InstallResult.ERR_MEX;
                r.message = mexBad;
                return r;
            }

            // 5) icon（可选）
            byte[] icon = null;
            if (mf.icon != null && mf.icon.Length > 0)
            {
                int ii = zip.IndexOf(mf.icon);
                if (ii >= 0)
                {
                    icon = zip.DataAt(ii);
                    if (icon.Length > MAX_ICON_BYTES)
                    {
                        r.code = InstallResult.ERR_TOO_BIG;
                        r.message = "icon too large: " + icon.Length;
                        return r;
                    }
                }
            }

            // ---- 阶段 B：落盘 -------------------------------------
            r.app_id = mf.app_id;
            r.app_name = mf.name;
            r.version = mf.version;
            r.tools = mf.tool_names.Length;

            // 记录旧版本（用于升级报告）
            string oldReg = ReadRegistryRaw();
            r.old_version = FindVersionInRegistry(oldReg, mf.app_id);

            // 已写入的文件，供回滚使用
            string[] written = new string[8];
            int nWritten = 0;

            try
            {
                // 5a) <app_id>.mex
                string mexPath = io.Combine(mkfsRoot, mf.app_id + ".mex");
                if (!io.WriteAll(mexPath, mex))
                {
                    r.code = InstallResult.ERR_IO;
                    r.message = "cannot write " + mexPath;
                    return r;
                }
                written[nWritten++] = mexPath;

                // 5b) <app_id>.icon
                if (icon != null)
                {
                    string iconPath = io.Combine(mkfsRoot, mf.app_id + ".icon");
                    if (io.WriteAll(iconPath, icon)) written[nWritten++] = iconPath;
                }

                // 5c) 合并注册表
                string merged = MergeRegistry(oldReg, mf);
                string regPath = io.Combine(mkfsRoot, OVERLAY_NAME);
                if (!io.WriteText(regPath, merged))
                {
                    r.code = InstallResult.ERR_REGISTRY;
                    r.message = "cannot write registry " + regPath;
                    Rollback(written, nWritten, r);
                    return r;
                }
                written[nWritten++] = regPath;

                // 5d) 桌面快捷方式
                if (mf.entry_desktop)
                {
                    string lnkName = SanitizeFileName(mf.name) + ".lnk";
                    string lnkPath = io.Combine(desktopDir, lnkName);
                    // .lnk 体 = 十进制 Kind（Desktop.ParseKind 解析）
                    bool ok = io.WriteText(lnkPath, IntStr(mf.entry_kind));
                    if (ok)
                    {
                        written[nWritten++] = lnkPath;
                        r.desktop_added = true;
                    }
                }
            }
            catch (Exception ex)
            {
                r.code = InstallResult.ERR_IO;
                r.message = "install failed: " + ex.Message;
                Rollback(written, nWritten, r);
                return r;
            }

            r.code = InstallResult.OK;
            r.message = r.Summary();
            return r;
        }

        // -------------------------------------------------------------
        //  卸载：删除 .mex / .icon / .lnk，并从注册表移除
        // -------------------------------------------------------------
        public InstallReport Uninstall(string app_id)
        {
            InstallReport r = new InstallReport();
            r.app_id = app_id;
            if (app_id == null || app_id.Length == 0)
            {
                r.code = InstallResult.ERR_ARG;
                r.message = "app_id is empty";
                return r;
            }
            if (IsReserved(app_id))
            {
                r.code = InstallResult.ERR_RESERVED;
                r.message = "refusing to uninstall built-in app: " + app_id;
                return r;
            }

            // 先读注册表拿到显示名（用于定位 .lnk）
            string reg = ReadRegistryRaw();
            string dispName = FindNameInRegistry(reg, app_id);

            string mexPath = io.Combine(mkfsRoot, app_id + ".mex");
            string iconPath = io.Combine(mkfsRoot, app_id + ".icon");
            if (io.FileExists(mexPath)) io.Delete(mexPath);
            if (io.FileExists(iconPath)) io.Delete(iconPath);
            if (dispName != null && dispName.Length > 0)
            {
                string lnk = io.Combine(desktopDir,
                                        SanitizeFileName(dispName) + ".lnk");
                if (io.FileExists(lnk)) io.Delete(lnk);
            }

            // 从注册表移除该 app_id
            string merged = RemoveFromRegistry(reg, app_id);
            string regPath = io.Combine(mkfsRoot, OVERLAY_NAME);
            if (!io.WriteText(regPath, merged))
            {
                r.code = InstallResult.ERR_REGISTRY;
                r.message = "cannot update registry";
                return r;
            }

            r.code = InstallResult.OK;
            r.message = "uninstalled " + app_id;
            return r;
        }

        // -------------------------------------------------------------
        //  列出已安装应用（读 mkfs/apps.overlay.json）
        // -------------------------------------------------------------
        public string[] ListInstalled()
        {
            string reg = ReadRegistryRaw();
            return RegistryQuery.AppIds(reg);
        }

        // -------------------------------------------------------------
        //  内部：校验
        // -------------------------------------------------------------
        static bool IsSafeMemberName(string nm)
        {
            if (nm == null || nm.Length == 0) return false;
            if (nm == "." || nm == "..") return false;
            for (int i = 0; i < nm.Length; i++)
            {
                char c = nm[i];
                if (c == '/' || c == '\\') return false;
                if (c < 0x20) return false;
            }
            return true;
        }

        static string ValidateManifest(NexpackManifest m)
        {
            if (m.schema_version != 1)
                return "schema_version must be 1 (got " + m.schema_version + ")";
            if (m.app_id == null || m.app_id.Length == 0)
                return "app_id is missing";
            if (!IsValidAppId(m.app_id))
                return "app_id '" + m.app_id + "' is not a valid identifier";
            if (m.name == null || m.name.Length == 0)
                return "name is missing";
            if (m.version == null || m.version.Length == 0)
                return "version is missing";
            if (!IsValidVersion(m.version))
                return "version '" + m.version + "' is not MAJOR.MINOR.PATCH";
            if (m.author == null || m.author.Length == 0)
                return "author is missing";
            return null;
        }

        static bool IsValidAppId(string s)
        {
            if (s.Length < 2 || s.Length > 32) return false;
            char c0 = s[0];
            if (c0 < 'a' || c0 > 'z') return false;
            for (int i = 1; i < s.Length; i++)
            {
                char c = s[i];
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                if (!ok) return false;
            }
            return true;
        }

        static bool IsValidVersion(string s)
        {
            int dots = 0, digits = 0;
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (c == '.') { dots++; digits = 0; }
                else if (c >= '0' && c <= '9') digits++;
                else return false;
            }
            return dots == 2 && digits > 0;
        }

        static string ValidateMex(byte[] mex)
        {
            if (mex == null || mex.Length == 0) return "app.mex is empty";
            if (mex.Length > MAX_MEX_BYTES)
                return "app.mex too large: " + mex.Length;
            if (mex.Length < 4) return "app.mex is truncated";
            for (int i = 0; i < 4; i++)
            {
                if ((char)mex[i] != MEX_MAGIC[i])
                    return "app.mex has bad magic (expected MEX1)";
            }
            return null;
        }

        bool IsReserved(string id)
        {
            for (int i = 0; i < RESERVED.Length; i++)
                if (RESERVED[i] == id) return true;
            return false;
        }

        // -------------------------------------------------------------
        //  内部：注册表 overlay 合并
        // -------------------------------------------------------------
        string ReadRegistryRaw()
        {
            string p = io.Combine(mkfsRoot, OVERLAY_NAME);
            if (!io.FileExists(p)) return "{\"schema_version\":1,\"apps\":[]}";
            string s = io.ReadText(p);
            if (s == null || s.Length == 0)
                return "{\"schema_version\":1,\"apps\":[]}";
            return s;
        }

        // 把 manifest 转成 apps.json 的一个 app 条目，并合并进现有注册表。
        string MergeRegistry(string existing, NexpackManifest m)
        {
            string entry = ManifestParser.ToRegistryEntry(m);
            return RegistryMerge.Upsert(existing, m.app_id, entry);
        }

        string RemoveFromRegistry(string existing, string app_id)
        {
            return RegistryMerge.Remove(existing, app_id);
        }

        string FindVersionInRegistry(string reg, string app_id)
        {
            return RegistryQuery.Field(reg, app_id, "version");
        }

        string FindNameInRegistry(string reg, string app_id)
        {
            return RegistryQuery.Field(reg, app_id, "name");
        }

        // -------------------------------------------------------------
        //  内部：回滚
        // -------------------------------------------------------------
        void Rollback(string[] written, int n, InstallReport r)
        {
            for (int i = n - 1; i >= 0; i--)
            {
                try { io.Delete(written[i]); } catch { }
            }
            r.rolled_back[0] = true;
        }

        // -------------------------------------------------------------
        //  内部：文件名清洗 / 数字转字符串
        // -------------------------------------------------------------
        public static string SanitizeFileName(string s)
        {
            if (s == null || s.Length == 0) return "App";
            string r = "";
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                bool bad = c == '/' || c == '\\' || c == ':' || c == '*' ||
                           c == '?' || c == '"' || c == '<' || c == '>' ||
                           c == '|' || c < 0x20;
                r = r + (bad ? '_' : c);
            }
            if (r.Length > 48) r = r.Substring(0, 48);
            return r;
        }

        static string IntStr(int v) { return v.ToString(); }
    }
}
