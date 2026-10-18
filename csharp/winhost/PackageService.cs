// =====================================================================
//  PackageService.cs  -  安装器的 WinHost 门面（接到运行中的 shell）
// ---------------------------------------------------------------------
//  PackageInstaller 是纯逻辑（不碰 UI）。本文件把它的宿主依赖接上：
//
//    1. 文件系统 —— HostFileIo，指向 Host.FsRoot 下的 mkfs / mkfs/Desktop
//    2. 桌面刷新 —— 安装成功后调 Desktop.Refresh()（FileRefresh + SyncFromFs）
//    3. 安装入口 —— InstallFromFile(path)：给 shell UI 一个方法调用
//
//  用法（shell UI / 终端命令）：
//      InstallReport r = PackageService.InstallFromFile(@"C:\x\notes.nexpack");
//      if (!r.Ok) ShowError(r.message);
//
//  内核侧（MiniCLR）不走这里：那边没有 ZIP 解包能力，安装由宿主完成，
//  内核只读结果（见 docs/NEXPACK.md §5）。
//
//  命名空间刻意不叫 NexOS.Host —— 那会遮蔽 NexOS.Forms.Host（Backend.cs
//  里的静态类），导致 ShellForm.cs 里 Host.XxxHook 被解析成命名空间成员。
// =====================================================================
using System;
using NexOS.Install;

namespace NexOS.Packages
{
    public static class PackageService
    {
        static PackageInstaller _installer;
        static string _mkfsRoot;
        static string _desktopDir;

        // 用当前的 FsRoot 初始化。FsRoot 为空时退回程序目录。
        public static void Init()
        {
            // 宿主实现是 NexOS.Forms.Host（Backend.cs），FsRoot 是 internal，
            // 但 PackageService 与它同在 WinHost 程序集内，可直接访问。
            string root = NexOS.Forms.Host.FsRoot;
            if (string.IsNullOrEmpty(root)) root = AppContext.BaseDirectory;
            _mkfsRoot = System.IO.Path.Combine(root, "mkfs");
            _desktopDir = System.IO.Path.Combine(_mkfsRoot, "Desktop");
            _installer = new PackageInstaller(new HostFileIo(), _mkfsRoot, _desktopDir);
        }

        static PackageInstaller Inst()
        {
            if (_installer == null) Init();
            return _installer;
        }

        public static void SetForce(bool v) { Inst().SetForce(v); }

        // 安装一个 .nexpack，成功后刷新桌面。
        public static InstallReport InstallFromFile(string path)
        {
            InstallReport r = Inst().Install(path);
            if (r.Ok) RefreshDesktop();
            return r;
        }

        public static InstallReport Uninstall(string app_id)
        {
            InstallReport r = Inst().Uninstall(app_id);
            if (r.Ok) RefreshDesktop();
            return r;
        }

        public static string[] ListInstalled() { return Inst().ListInstalled(); }

        // 桌面刷新：让 Desktop 重新读 MKFS/Desktop，新 .lnk 立刻出现。
        // 这两步与 Desktop.Refresh() 完全一致（FileRefresh + SyncFromFs）。
        public static void RefreshDesktop()
        {
            try
            {
                NexOS.Forms.Host.FileRefresh();
                NexOS.Forms.Desktop.Refresh();
            }
            catch
            {
                // 桌面尚未初始化时（例如无 GUI 的批处理安装）忽略。
            }
        }
    }
}
