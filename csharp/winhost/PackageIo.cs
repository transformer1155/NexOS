// =====================================================================
//  PackageIo.cs  -  PackageInstaller 的宿主文件系统适配器
// ---------------------------------------------------------------------
//  把 PackageInstaller 的 IPackageIo 接到真实的 Windows 目录（WinHost），
//  或 NexOS 的 MKFS（内核侧）。
//
//  MKFS 布局（WinHost 宿主侧映射，见 Backend.cs Dir()）：
//      <FsRoot>/mkfs/            <- fs==0，用户盘
//      <FsRoot>/mkfs/Desktop/    <- fs==3，桌面快捷方式
//      <FsRoot>/sfs/             <- fs==1，只读系统盘
//
//  安装产物一律写 mkfs 与 mkfs/Desktop：
//      mkfs/<app_id>.mex
//      mkfs/<app_id>.icon
//      mkfs/apps.json            <- 注册表 overlay
//      mkfs/Desktop/<Name>.lnk
// =====================================================================
using System;
using System.IO;
using System.Text;

namespace NexOS.Install
{
    // 真实文件系统实现（WinHost 宿主侧）。
    public class HostFileIo : IPackageIo
    {
        public bool FileExists(string path)
        {
            try { return File.Exists(path); } catch { return false; }
        }

        public long FileSize(string path)
        {
            try { return new FileInfo(path).Length; } catch { return -1; }
        }

        public byte[] ReadAll(string path)
        {
            try { return File.ReadAllBytes(path); } catch { return null; }
        }

        // 关键：二进制写入。内核只有 kern_fs_read，没有写原语，所以
        // .mex 的落盘由宿主完成（见 docs/NEXPACK.md §5）。
        public bool WriteAll(string path, byte[] data)
        {
            try
            {
                string dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                    Directory.CreateDirectory(dir);
                File.WriteAllBytes(path, data);
                return true;
            }
            catch { return false; }
        }

        public bool WriteText(string path, string text)
        {
            try
            {
                string dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                    Directory.CreateDirectory(dir);
                // 显式 UTF-8 无 BOM，保证跨语言（Python / 内核）解析一致。
                File.WriteAllText(path, text, new UTF8Encoding(false));
                return true;
            }
            catch { return false; }
        }

        public string ReadText(string path)
        {
            try { return File.Exists(path) ? File.ReadAllText(path) : null; }
            catch { return null; }
        }

        public bool MakeDir(string path)
        {
            try { Directory.CreateDirectory(path); return true; }
            catch { return false; }
        }

        public bool Delete(string path)
        {
            try
            {
                if (File.Exists(path)) { File.Delete(path); return true; }
                if (Directory.Exists(path)) { Directory.Delete(path, true); return true; }
                return false;
            }
            catch { return false; }
        }

        public string[] ListFiles(string dir)
        {
            try
            {
                if (!Directory.Exists(dir)) return new string[0];
                string[] files = Directory.GetFiles(dir);
                string[] subs = Directory.GetDirectories(dir);
                string[] all = new string[files.Length + subs.Length];
                int k = 0;
                for (int i = 0; i < subs.Length; i++) all[k++] = Path.GetFileName(subs[i]);
                for (int i = 0; i < files.Length; i++) all[k++] = Path.GetFileName(files[i]);
                return all;
            }
            catch { return new string[0]; }
        }

        public string Combine(string a, string b) { return Path.Combine(a, b); }
        public string GetFileName(string path) { return Path.GetFileName(path); }
    }
}
