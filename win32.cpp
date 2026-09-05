// =====================================================================
//  win32.cpp  -  NexOS Win32 subsystem emulation
// ---------------------------------------------------------------------
//  * Simulated Windows registry (HKLM / HKCU / HKCR / HKU / HKCC)
//  * PE32 image loader: sections, base relocations, import resolution
//  * Emulated kernel32 / user32 / gdi32 / advapi32 / msvcrt exports
//  * GDI display list so real Win32 windows render in the NexOS GUI
//
//  The loaded image executes natively: NexOS runs in 32-bit protected
//  mode with flat segments, which is exactly what a PE32 i386 image
//  expects.  Imports are patched to point at the C++ functions below.
// =====================================================================
#include <stdint.h>
#include "win32.h"

extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);
// Kernel file-system write back (MKFS data FS) -- used to make CreateFileA/
// WriteFile/CloseHandle actually persist a file written by a Win32 program.
// Declared WITHOUT extern "C" to match the C++ mangled name in kernel.cpp.
int kern_fs_create(const char* name, const unsigned char* data, int len);

#if defined(__i386__)
#  define WINAPI __attribute__((stdcall))
#  define W32_EXEC 1
#  define W64_EXEC 0
#elif defined(__x86_64__)
//  64-bit guests (PE32+) call the Win32 API shims with the Microsoft x64
//  (Win64) calling convention: RCX/RDX/R8/R9 + 32-byte shadow space.  Make
//  the shims ms_abi so a 64-bit guest can invoke them directly.  Internal
//  calls between shims stay consistent (ms_abi -> ms_abi); calls into
//  ordinary SysV kernel helpers are fine (caller sets up its own ABI).
#  define WINAPI __attribute__((ms_abi))
#  define W32_EXEC 0
#  define W64_EXEC 1
#else
#  define WINAPI
#  define W32_EXEC 0
#  define W64_EXEC 0
#endif

// ---------------------------------------------------------------------
//  Serial debug + host callbacks
// ---------------------------------------------------------------------
#ifdef W32_HOSTTEST
// tools/host_w32_test.cpp compiles this file as an ordinary 32-bit user
// mode program, so the PE32 loader can be exercised without booting.
// Port I/O is privileged there, so the serial debug port becomes a no-op.
static inline void w32_outb(uint16_t, uint8_t){}
#else
static inline void w32_outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
#endif
static void w32_serial(const char* s){ while (*s) w32_outb(0x3F8, (uint8_t)*s++); }
static void w32_serial_hex(uint32_t v){
    const char* H = "0123456789ABCDEF";
    char buf[9];
    for (int i = 0; i < 8; i++) buf[i] = H[(v >> (28 - i*4)) & 0xF];
    buf[8] = 0;
    w32_serial(buf);
}
static void w32_serial_hex64(uint64_t v){
    const char* H = "0123456789ABCDEF";
    char buf[17];
    for (int i = 0; i < 16; i++) buf[i] = H[(v >> (60 - i*4)) & 0xF];
    buf[16] = 0;
    w32_serial(buf);
}
static void w32_serial_byte(uint8_t b){
    const char* H = "0123456789ABCDEF";
    char buf[3]; buf[0] = H[b >> 4]; buf[1] = H[b & 0xF]; buf[2] = 0;
    w32_serial(buf);
}

static int  (*g_reader)(const char*, uint8_t*, int) = 0;
static void (*g_writer)(const char*)                = 0;

static void w32_out(const char* s){ if (g_writer) g_writer(s); }

// ---------------------------------------------------------------------
//  Freestanding string helpers
// ---------------------------------------------------------------------
static int   w_len(const char* s){ int n=0; while (s && s[n]) n++; return n; }
static void  w_cpy(char* d, const char* s){ while ((*d++ = *s++)); }
static void  w_ncpy(char* d, const char* s, int n){ int i=0; for (; i<n-1 && s[i]; i++) d[i]=s[i]; if (n>0) d[i]=0; }
static int   w_cmp(const char* a, const char* b){ while (*a && *a==*b){a++;b++;} return (uint8_t)*a-(uint8_t)*b; }
static char  w_lo(char c){ return (c>='A'&&c<='Z') ? (char)(c+32) : c; }
static int   w_icmp(const char* a, const char* b){
    while (*a && w_lo(*a)==w_lo(*b)) { a++; b++; }
    return (uint8_t)w_lo(*a) - (uint8_t)w_lo(*b);
}
static void  w_cat(char* d, const char* s){ while (*d) d++; while ((*d++ = *s++)); }
static void  w_set(void* d, int v, uint32_t n){ uint8_t* p=(uint8_t*)d; while (n--) *p++=(uint8_t)v; }
static void  w_mov(void* d, const void* s, uint32_t n){
    uint8_t* a=(uint8_t*)d; const uint8_t* b=(const uint8_t*)s; while (n--) *a++=*b++;
}
static void w_num(char* out, uint32_t v){
    char t[12]; int n=0;
    if (!v) { out[0]='0'; out[1]=0; return; }
    while (v) { t[n++] = (char)('0' + v%10); v/=10; }
    int i=0; while (n) out[i++]=t[--n];
    out[i]=0;
}
static void w_hex(char* out, uint32_t v, int digits){
    const char* H="0123456789ABCDEF";
    for (int i=0;i<digits;i++) out[i]=H[(v >> ((digits-1-i)*4)) & 0xF];
    out[digits]=0;
}
// append to a bounded buffer; returns new length
static int w_app(char* buf, int len, int cap, const char* s){
    while (*s && len < cap-1) buf[len++] = *s++;
    buf[len] = 0;
    return len;
}

// =====================================================================
//  1.  Registry
// =====================================================================
constexpr int  REG_MAX_KEYS   = 260;
constexpr int  REG_MAX_VALUES = 360;
constexpr int  REG_DATA_MAX   = 64;

// value types (Win32 constants)
constexpr uint32_t RT_NONE      = 0;
constexpr uint32_t RT_SZ        = 1;
constexpr uint32_t RT_EXPAND_SZ = 2;
constexpr uint32_t RT_BINARY    = 3;
constexpr uint32_t RT_DWORD     = 4;
constexpr uint32_t RT_MULTI_SZ  = 7;

struct RegValue {
    char     name[32];
    uint32_t type;
    uint32_t size;
    uint8_t  data[REG_DATA_MAX];
    int16_t  next;
};
struct RegKey {
    char    name[32];
    int16_t parent, child, sibling, values;
};

static RegKey   g_keys[REG_MAX_KEYS];
static RegValue g_vals[REG_MAX_VALUES];
static int      g_key_n = 0;
static int      g_val_n = 0;
static bool     g_reg_ready = false;

static const char* reg_type_name(uint32_t t){
    switch (t){
        case RT_SZ:        return "REG_SZ";
        case RT_EXPAND_SZ: return "REG_EXPAND_SZ";
        case RT_BINARY:    return "REG_BINARY";
        case RT_DWORD:     return "REG_DWORD";
        case RT_MULTI_SZ:  return "REG_MULTI_SZ";
        default:           return "REG_NONE";
    }
}
static uint32_t reg_type_parse(const char* s){
    if (!s || !s[0])                 return RT_SZ;
    if (!w_icmp(s,"REG_DWORD")     || !w_icmp(s,"dword"))  return RT_DWORD;
    if (!w_icmp(s,"REG_BINARY")    || !w_icmp(s,"binary")) return RT_BINARY;
    if (!w_icmp(s,"REG_EXPAND_SZ") || !w_icmp(s,"expand")) return RT_EXPAND_SZ;
    if (!w_icmp(s,"REG_MULTI_SZ")  || !w_icmp(s,"multi"))  return RT_MULTI_SZ;
    return RT_SZ;
}

static int reg_new_key(const char* name, int parent){
    if (g_key_n >= REG_MAX_KEYS) return -1;
    int i = g_key_n++;
    w_ncpy(g_keys[i].name, name, 32);
    g_keys[i].parent = (int16_t)parent;
    g_keys[i].child = g_keys[i].sibling = g_keys[i].values = -1;
    if (parent >= 0){
        // append to end of the sibling chain (keeps a stable listing order)
        if (g_keys[parent].child < 0) g_keys[parent].child = (int16_t)i;
        else {
            int s = g_keys[parent].child;
            while (g_keys[s].sibling >= 0) s = g_keys[s].sibling;
            g_keys[s].sibling = (int16_t)i;
        }
    }
    return i;
}

static int reg_child(int parent, const char* name){
    if (parent < 0) return -1;
    for (int c = g_keys[parent].child; c >= 0; c = g_keys[c].sibling)
        if (!w_icmp(g_keys[c].name, name)) return c;
    return -1;
}

// Map a root alias to a key index
static int reg_root(const char* n){
    if (!w_icmp(n,"HKEY_CLASSES_ROOT")   || !w_icmp(n,"HKCR")) return 0;
    if (!w_icmp(n,"HKEY_CURRENT_USER")   || !w_icmp(n,"HKCU")) return 1;
    if (!w_icmp(n,"HKEY_LOCAL_MACHINE")  || !w_icmp(n,"HKLM")) return 2;
    if (!w_icmp(n,"HKEY_USERS")          || !w_icmp(n,"HKU"))  return 3;
    if (!w_icmp(n,"HKEY_CURRENT_CONFIG") || !w_icmp(n,"HKCC")) return 4;
    return -1;
}

// Resolve "HKLM\SOFTWARE\..." ; create==true creates missing levels.
static int reg_path(const char* path, bool create){
    if (!path || !path[0]) return -1;
    char comp[32]; int ci = 0;
    const char* p = path;
    int cur = -1;
    bool first = true;
    for (;;){
        char c = *p;
        if (c=='\\' || c=='/' || c==0){
            comp[ci] = 0;
            if (ci){
                if (first){
                    cur = reg_root(comp);
                    if (cur < 0) return -1;
                    first = false;
                } else {
                    int nx = reg_child(cur, comp);
                    if (nx < 0){
                        if (!create) return -1;
                        nx = reg_new_key(comp, cur);
                        if (nx < 0) return -1;
                    }
                    cur = nx;
                }
            }
            ci = 0;
            if (c==0) break;
        } else if (ci < 31) comp[ci++] = c;
        p++;
    }
    return cur;
}

static RegValue* reg_find_val(int key, const char* name){
    if (key < 0) return 0;
    const char* n = name ? name : "";
    for (int v = g_keys[key].values; v >= 0; v = g_vals[v].next)
        if (!w_icmp(g_vals[v].name, n)) return &g_vals[v];
    return 0;
}

static int reg_put_val(int key, const char* name, uint32_t type,
                       const void* data, uint32_t size){
    if (key < 0) return -1;
    if (size > REG_DATA_MAX) size = REG_DATA_MAX;
    RegValue* ex = reg_find_val(key, name);
    if (!ex){
        if (g_val_n >= REG_MAX_VALUES) return -1;
        int i = g_val_n++;
        ex = &g_vals[i];
        w_ncpy(ex->name, name ? name : "", 32);
        ex->next = -1;
        if (g_keys[key].values < 0) g_keys[key].values = (int16_t)i;
        else {
            int s = g_keys[key].values;
            while (g_vals[s].next >= 0) s = g_vals[s].next;
            g_vals[s].next = (int16_t)i;
        }
    }
    ex->type = type;
    ex->size = size;
    w_set(ex->data, 0, REG_DATA_MAX);
    if (data && size) w_mov(ex->data, data, size);
    return 0;
}

static int reg_put_sz(const char* path, const char* name, const char* s){
    int k = reg_path(path, true);
    return reg_put_val(k, name, RT_SZ, s, (uint32_t)w_len(s)+1);
}
static int reg_put_dw(const char* path, const char* name, uint32_t v){
    int k = reg_path(path, true);
    return reg_put_val(k, name, RT_DWORD, &v, 4);
}
static int reg_put_exp(const char* path, const char* name, const char* s){
    int k = reg_path(path, true);
    return reg_put_val(k, name, RT_EXPAND_SZ, s, (uint32_t)w_len(s)+1);
}

// Render a value's data as text
static void reg_val_text(const RegValue* v, char* out, int cap){
    out[0] = 0;
    if (!v) return;
    if (v->type == RT_DWORD){
        uint32_t d = 0; w_mov(&d, v->data, 4);
        char hx[16]; w_hex(hx, d, 8);
        char dec[12]; w_num(dec, d);
        int n = 0;
        n = w_app(out, n, cap, "0x"); n = w_app(out, n, cap, hx);
        n = w_app(out, n, cap, " ("); n = w_app(out, n, cap, dec);
        w_app(out, n, cap, ")");
    } else if (v->type == RT_BINARY){
        int n = 0;
        for (uint32_t i = 0; i < v->size && i < 16; i++){
            char hx[4]; w_hex(hx, v->data[i], 2);
            n = w_app(out, n, cap, hx);
            n = w_app(out, n, cap, " ");
        }
    } else if (v->type == RT_MULTI_SZ){
        int n = 0;
        uint32_t i = 0;
        while (i < v->size && v->data[i]){
            n = w_app(out, n, cap, (const char*)&v->data[i]);
            i += (uint32_t)w_len((const char*)&v->data[i]) + 1;
            if (i < v->size && v->data[i]) n = w_app(out, n, cap, " | ");
        }
    } else {
        w_app(out, 0, cap, (const char*)v->data);
    }
}

// ---------------------------------------------------------------------
//  Registry seeding: a realistic Windows 10 style hive
// ---------------------------------------------------------------------
static void reg_seed(){
    if (g_reg_ready) return;
    g_key_n = g_val_n = 0;
    reg_new_key("HKEY_CLASSES_ROOT",   -1);   // 0
    reg_new_key("HKEY_CURRENT_USER",   -1);   // 1
    reg_new_key("HKEY_LOCAL_MACHINE",  -1);   // 2
    reg_new_key("HKEY_USERS",          -1);   // 3
    reg_new_key("HKEY_CURRENT_CONFIG", -1);   // 4

    // ---- HKLM\HARDWARE ----
    const char* CPU = "HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    reg_put_sz(CPU, "ProcessorNameString", "NexOS Virtual CPU @ 2.40GHz");
    reg_put_sz(CPU, "VendorIdentifier",    "GenuineIntel");
    reg_put_sz(CPU, "Identifier",          "x86 Family 6 Model 142 Stepping 10");
    reg_put_dw(CPU, "~MHz", 2400);
    reg_put_sz("HKLM\\HARDWARE\\DESCRIPTION\\System", "SystemBiosVersion", "NexOS - 20260805");
    reg_put_sz("HKLM\\HARDWARE\\DESCRIPTION\\System\\BIOS", "BaseBoardManufacturer", "NexOS Project");
    reg_put_sz("HKLM\\HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemProductName", "NexOS Virtual Machine");
    reg_put_sz("HKLM\\HARDWARE\\DEVICEMAP\\Serialcomm", "\\Device\\Serial0", "COM1");

    // ---- HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion ----
    const char* NTCV = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    reg_put_sz(NTCV, "ProductName",       "NexOS Windows Compatible Subsystem");
    reg_put_sz(NTCV, "CurrentVersion",    "6.3");
    reg_put_sz(NTCV, "CurrentBuild",      "19045");
    reg_put_sz(NTCV, "CurrentBuildNumber","19045");
    reg_put_sz(NTCV, "BuildLab",          "19045.NexOS.260805-1200");
    reg_put_sz(NTCV, "DisplayVersion",    "22H2");
    reg_put_sz(NTCV, "ReleaseId",         "2009");
    reg_put_sz(NTCV, "EditionID",         "Professional");
    reg_put_sz(NTCV, "InstallationType",  "Client");
    reg_put_sz(NTCV, "RegisteredOwner",   "NexOS User");
    reg_put_sz(NTCV, "RegisteredOrganization", "NexOS Project");
    reg_put_exp(NTCV,"SystemRoot",        "C:\\WINDOWS");
    reg_put_exp(NTCV,"PathName",          "C:\\WINDOWS");
    reg_put_dw(NTCV, "CurrentMajorVersionNumber", 10);
    reg_put_dw(NTCV, "CurrentMinorVersionNumber", 0);
    reg_put_dw(NTCV, "InstallDate", 0x67B00000);

    // ---- HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion ----
    const char* WCV = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion";
    reg_put_sz(WCV, "ProgramFilesDir",    "C:\\Program Files");
    reg_put_sz(WCV, "ProgramFilesDir (x86)", "C:\\Program Files (x86)");
    reg_put_sz(WCV, "CommonFilesDir",     "C:\\Program Files\\Common Files");
    reg_put_sz(WCV, "ProgramFilesPath",   "%ProgramFiles%");
    reg_put_sz(WCV, "DevicePath",         "%SystemRoot%\\inf");
    reg_put_sz(WCV, "MediaPathUnexpanded","%SystemRoot%\\Media");
    reg_put_exp("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                "NexOSTray", "%SystemRoot%\\system32\\minitray.exe");
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer",
               "ShellFolder", "C:\\Users\\Default");
    // ---- HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders ----
    const char* SHELL_LM = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders";
    reg_put_sz(SHELL_LM, "Common Desktop",   "C:\\Users\\Public\\Desktop");
    reg_put_sz(SHELL_LM, "Common Documents", "C:\\Users\\Public\\Documents");
    reg_put_sz(SHELL_LM, "Common Programs",  "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs");
    reg_put_sz(SHELL_LM, "Common Start Menu","C:\\ProgramData\\Microsoft\\Windows\\Start Menu");
    reg_put_sz(SHELL_LM, "Common Startup",   "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");

    // ---- HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NexOS ----
    const char* UNIN = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NexOS";
    reg_put_sz(UNIN, "DisplayName",     "NexOS Kernel");
    reg_put_sz(UNIN, "DisplayVersion",  "0.8.0");
    reg_put_sz(UNIN, "Publisher",       "NexOS Project");
    reg_put_sz(UNIN, "InstallLocation", "C:\\NexOS");
    reg_put_sz(UNIN, "UninstallString", "C:\\NexOS\\uninst.exe");
    reg_put_sz(UNIN, "DisplayIcon",     "C:\\NexOS\\NexOS.exe");
    reg_put_dw(UNIN, "NoModify", 1);
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\.NETFramework", "InstallRoot",
               "C:\\WINDOWS\\Microsoft.NET\\Framework\\");
    reg_put_sz("HKLM\\SOFTWARE\\Classes\\.exe", "", "exefile");
    reg_put_sz("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows", "Managed", "0");

    // ---- HKLM\SYSTEM ----
    const char* SMENV = "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    reg_put_exp(SMENV, "Path", "C:\\WINDOWS\\system32;C:\\WINDOWS;C:\\NexOS");
    reg_put_sz(SMENV, "PATHEXT", ".COM;.EXE;.BAT;.CMD;.PS1");
    reg_put_sz(SMENV, "OS", "Windows_NT");
    reg_put_sz(SMENV, "PROCESSOR_ARCHITECTURE", "x86");
    reg_put_sz(SMENV, "PROCESSOR_IDENTIFIER", "x86 Family 6 Model 142 Stepping 10, GenuineIntel");
    reg_put_sz(SMENV, "NUMBER_OF_PROCESSORS", "1");
    reg_put_exp(SMENV,"TEMP", "C:\\WINDOWS\\TEMP");
    reg_put_exp(SMENV,"TMP",  "C:\\WINDOWS\\TEMP");
    reg_put_exp(SMENV,"windir", "C:\\WINDOWS");
    reg_put_exp(SMENV,"ComSpec", "C:\\WINDOWS\\system32\\cmd.exe");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Control", "SystemBootDevice",
               "\\Device\\HarddiskVolume1");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName",
               "ComputerName", "NexOS-PC");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Control\\Nls\\Language",
               "Default", "0804");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation",
               "TimeZoneKeyName", "China Standard Time");
    reg_put_dw("HKLM\\SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation", "Bias", 0xFFFFFE38);
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\SubSystems",
               "Windows", "%SystemRoot%\\system32\\csrss.exe ObjectDirectory=\\Windows");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\SubSystems",
               "Optional", "Posix");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters",
               "Hostname", "NexOS-pc");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters",
               "Domain", "NexOS.local");
    reg_put_sz("HKLM\\SYSTEM\\CurrentControlSet\\Services\\Disk", "DisplayName", "Disk Driver");
    reg_put_dw("HKLM\\SYSTEM\\Setup", "SystemSetupInProgress", 0);

    // ---- Fonts ----
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
               "Microsoft Sans Serif (TrueType)", "micross.ttf");
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
               "Courier New (TrueType)", "cour.ttf");

    // ---- DirectX / Vulkan placeholders (apps often probe and tolerate absence) ----
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\DirectX", "InstalledVersion", "4.09.00.0904");

    // ---- HKCU ----
    reg_put_exp("HKCU\\Environment", "Path",  "%USERPROFILE%\\bin");
    reg_put_exp("HKCU\\Environment", "TEMP",  "C:\\Users\\User\\AppData\\Local\\Temp");
    reg_put_exp("HKCU\\Environment", "TMP",   "C:\\Users\\User\\AppData\\Local\\Temp");
    reg_put_sz("HKCU\\Volatile Environment", "USERNAME", "User");
    reg_put_sz("HKCU\\Volatile Environment", "USERDOMAIN", "NexOS-PC");
    reg_put_sz("HKCU\\Volatile Environment", "USERPROFILE", "C:\\Users\\User");
    reg_put_sz("HKCU\\Volatile Environment", "LOGONSERVER", "\\\\NexOS-PC");
    reg_put_sz("HKCU\\Volatile Environment", "HOMEDRIVE", "C:");
    reg_put_sz("HKCU\\Volatile Environment", "HOMEPATH", "\\Users\\User");
    reg_put_sz("HKCU\\Volatile Environment", "APPDATA", "C:\\Users\\User\\AppData\\Roaming");

    // ---- HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders ----
    const char* SHELL_CU = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders";
    reg_put_sz(SHELL_CU, "Desktop",   "C:\\Users\\User\\Desktop");
    reg_put_sz(SHELL_CU, "Personal",  "C:\\Users\\User\\Documents");
    reg_put_sz(SHELL_CU, "My Pictures","C:\\Users\\User\\Pictures");
    reg_put_sz(SHELL_CU, "My Music",  "C:\\Users\\User\\Music");
    reg_put_sz(SHELL_CU, "My Video",  "C:\\Users\\User\\Videos");
    reg_put_sz(SHELL_CU, "Programs",  "C:\\Users\\User\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs");
    reg_put_sz(SHELL_CU, "Start Menu","C:\\Users\\User\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu");
    reg_put_sz(SHELL_CU, "Startup",   "C:\\Users\\User\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");

    reg_put_sz("HKCU\\Control Panel\\Desktop", "Wallpaper", "C:\\WINDOWS\\Web\\NexOS.bmp");
    reg_put_sz("HKCU\\Control Panel\\Desktop", "ScreenSaveTimeOut", "600");
    reg_put_sz("HKCU\\Control Panel\\International", "LocaleName", "zh-CN");
    reg_put_sz("HKCU\\Control Panel\\International", "sCountry", "China");
    reg_put_sz("HKCU\\Control Panel\\International", "sShortDate", "yyyy/M/d");
    reg_put_dw("HKCU\\Control Panel\\Colors", "Background", 0x00003C64);
    const char* ADV = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    reg_put_dw(ADV, "Hidden", 1);
    reg_put_dw(ADV, "HideFileExt", 0);
    reg_put_dw(ADV, "ShowSuperHidden", 0);
    reg_put_dw("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               "AppsUseLightTheme", 0);
    reg_put_sz("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", "NexOSApp", "");
    reg_put_sz("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "NexOSUpdate", "");

    // ---- Internet Settings + default search engine (consumed by the NexOS browser) ----
    const char* IS = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    reg_put_dw(IS, "ProxyEnable", 0);
    reg_put_sz(IS, "User Agent",
               "Mozilla/4.0 (compatible; MSIE 10.0; Windows NT 6.3; NexOSBrowser/0.8)");
    reg_put_sz(IS, "Search Engine", "DuckDuckGo");
    const char* SS = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\SearchScopes\\{A1B2C3D4-E5F6-7890-A1B2-C3D4E5F6A7B8}";
    reg_put_sz(SS, "DisplayName", "DuckDuckGo");
    reg_put_sz(SS, "URL", "https://duckduckgo.com/html/?q={searchTerms}");
    reg_put_sz(SS, "SuggestionsURL", "https://duckduckgo.com/ac/?q={searchTerms}");
    reg_put_sz(SS, "FaviconURL", "https://duckduckgo.com/favicon.ico");
    reg_put_dw("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\SearchScopes",
               "DefaultScope", 0xA1B2C3D4);
    const char* SS2 = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\SearchScopes\\{B2C3D4E5-F6A7-8901-B2C3-D4E5F6A7B8C9}";
    reg_put_sz(SS2, "DisplayName", "Bing");
    reg_put_sz(SS2, "URL", "https://www.bing.com/search?q={searchTerms}");
    reg_put_sz("HKCU\\Software\\Microsoft\\Internet Explorer\\Main", "Start Page", "about:home");
    reg_put_sz("HKCU\\Software\\Microsoft\\Internet Explorer\\Main", "Search Page", "about:home");

    // ---- User Shell Folders (per-user, %USERPROFILE% expandable) ----
    const char* USF = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
    reg_put_exp(USF, "Desktop",       "%USERPROFILE%\\Desktop");
    reg_put_exp(USF, "Personal",      "%USERPROFILE%\\Documents");
    reg_put_exp(USF, "My Music",      "%USERPROFILE%\\Music");
    reg_put_exp(USF, "My Pictures",   "%USERPROFILE%\\Pictures");
    reg_put_exp(USF, "My Video",      "%USERPROFILE%\\Videos");
    reg_put_exp(USF, "AppData",       "%USERPROFILE%\\AppData\\Roaming");
    reg_put_exp(USF, "Local AppData", "%USERPROFILE%\\AppData\\Local");
    reg_put_exp(USF, "Cache",         "%USERPROFILE%\\AppData\\Local\\Microsoft\\Windows\\INetCache");
    reg_put_exp(USF, "History",       "%USERPROFILE%\\AppData\\Local\\Microsoft\\Windows\\History");

    // ---- Policies ----
    reg_put_dw("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\Explorer", "NoRun", 0);
    reg_put_dw("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\Explorer", "NoControlPanel", 0);
    reg_put_dw("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System", "EnableLUA", 0);
    reg_put_dw("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", "NoClose", 0);
    reg_put_dw("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", "DisableTaskMgr", 0);

    // ---- RunOnce + App Paths + IFEO (commonly probed by installers/launchers) ----
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
               "NexOSSetup", "%SystemRoot%\\system32\\setup.exe");
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\NexOS.exe",
               "", "C:\\NexOS\\NexOS.exe");
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\NexOS.exe",
               "Path", "C:\\NexOS");
    reg_put_sz("HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\NexOS.exe",
               "ProcessPriority", "Normal");

    // ---- HKCR: file associations ----
    reg_put_sz("HKCR\\.txt", "", "txtfile");
    reg_put_sz("HKCR\\.exe", "", "exefile");
    reg_put_sz("HKCR\\.bat", "", "batfile");
    reg_put_sz("HKCR\\.ps1", "", "Microsoft.PowerShellScript.1");
    reg_put_sz("HKCR\\txtfile", "", "Text Document");
    reg_put_sz("HKCR\\txtfile\\shell\\open\\command", "", "%SystemRoot%\\notepad.exe %1");
    reg_put_sz("HKCR\\exefile", "", "Application");
    reg_put_sz("HKCR\\exefile\\shell\\open\\command", "", "\"%1\" %*");
    reg_put_sz("HKCR\\batfile\\shell\\open\\command", "", "\"%1\" %*");

    // ---- HKU / HKCC ----
    reg_put_sz("HKU\\.DEFAULT\\Control Panel\\International", "LocaleName", "zh-CN");
    reg_put_sz("HKU\\S-1-5-21-1000\\Volatile Environment", "USERNAME", "User");
    reg_put_sz("HKCC\\System\\CurrentControlSet\\Control\\Video", "Resolution", "1024x768x32");

    g_reg_ready = true;
}

// =====================================================================
//  2.  Simulated process environment (%PATH% etc.)
// =====================================================================
struct W32Env { char name[24]; char value[72]; };
static W32Env g_env[24];
static int    g_env_n = 0;

static void env_put(const char* n, const char* v){
    for (int i=0;i<g_env_n;i++)
        if (!w_icmp(g_env[i].name, n)) { w_ncpy(g_env[i].value, v, 72); return; }
    if (g_env_n >= 24) return;
    w_ncpy(g_env[g_env_n].name, n, 24);
    w_ncpy(g_env[g_env_n].value, v, 72);
    g_env_n++;
}
static void env_seed(){
    if (g_env_n) return;
    env_put("ALLUSERSPROFILE", "C:\\ProgramData");
    env_put("APPDATA",         "C:\\Users\\User\\AppData\\Roaming");
    env_put("COMPUTERNAME",    "NexOS-PC");
    env_put("ComSpec",         "C:\\WINDOWS\\system32\\cmd.exe");
    env_put("HOMEDRIVE",       "C:");
    env_put("HOMEPATH",        "\\Users\\User");
    env_put("LOCALAPPDATA",    "C:\\Users\\User\\AppData\\Local");
    env_put("NUMBER_OF_PROCESSORS", "1");
    env_put("OS",              "Windows_NT");
    env_put("Path",            "C:\\WINDOWS\\system32;C:\\WINDOWS;C:\\NexOS");
    env_put("PATHEXT",         ".COM;.EXE;.BAT;.CMD;.PS1");
    env_put("PROCESSOR_ARCHITECTURE", "x86");
    env_put("ProgramData",     "C:\\ProgramData");
    env_put("ProgramFiles",    "C:\\Program Files");
    env_put("PUBLIC",          "C:\\Users\\Public");
    env_put("SystemDrive",     "C:");
    env_put("SystemRoot",      "C:\\WINDOWS");
    env_put("TEMP",            "C:\\Users\\User\\AppData\\Local\\Temp");
    env_put("TMP",             "C:\\Users\\User\\AppData\\Local\\Temp");
    env_put("USERDOMAIN",      "NexOS-PC");
    env_put("USERNAME",        "User");
    env_put("USERPROFILE",     "C:\\Users\\User");
    env_put("windir",          "C:\\WINDOWS");
}
static const char* env_get(const char* n){
    for (int i=0;i<g_env_n;i++) if (!w_icmp(g_env[i].name, n)) return g_env[i].value;
    return 0;
}

// =====================================================================
//  3.  Window / GDI object model
// =====================================================================
typedef int (WINAPI *WNDPROC)(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

struct W32Class {
    char     name[32];
    WNDPROC  proc;
    uint32_t bkbrush;
    bool     used;
};
struct W32Win {
    bool       used, visible, is_msgbox;
    uint32_t   hwnd;
    char       title[48];
    char       cls[32];
    int        x, y, w, h;
    WNDPROC    proc;
    W32DrawCmd cmds[W32_MAX_CMDS];
    int        cmd_n;
    // Child controls (BUTTON / STATIC / ...) created by CreateWindowExA.
    // Real Windows repaints child controls after the parent's WM_PAINT, so
    // we keep a private copy and re-append it inside EndPaint - otherwise
    // BeginPaint's "cmd_n = 0" would wipe them on the very first repaint.
    W32DrawCmd ctl[8];
    int        ctl_n;
    bool       painting;
};

constexpr int W32_MAX_CLASSES = 8;
static W32Class g_cls[W32_MAX_CLASSES];
static W32Win   g_win[W32_MAX_WINDOWS];
static int      g_win_n = 0;

// GDI device contexts
struct W32Dc { bool used; int win; uint32_t textcolor, bkcolor, pencolor, brushcolor; int bkmode; int curx, cury; };
constexpr int W32_MAX_DC = 4;
static W32Dc g_dc[W32_MAX_DC];

// GDI objects (brush / pen)
struct W32Obj { bool used; uint8_t kind; uint32_t color; };   // kind 1=brush 2=pen 3=font
constexpr int W32_MAX_OBJ = 16;
static W32Obj g_obj[W32_MAX_OBJ];

constexpr uint32_t HWND_BASE = 0x00110000;
constexpr uint32_t HDC_BASE  = 0x00220000;
constexpr uint32_t HOBJ_BASE = 0x00330000;

// ---- popup menus (user32: CreatePopupMenu / AppendMenu / TrackPopupMenu)
// TrackPopupMenu() is asynchronous here: it registers a session that the
// NexOS compositor (gui.cpp) renders in its own loop; choosing an item
// delivers WM_COMMAND(id) to the owner window.
struct W32MenuItem { char text[36]; uint32_t id; uint32_t flags; };
struct W32Menu     { bool used; W32MenuItem items[14]; int n; uint32_t h; };
constexpr int      W32_MAX_MENUS = 4;
constexpr uint32_t HMENU_BASE    = 0x00440000;
static W32Menu g_menu[W32_MAX_MENUS];
static int     g_menu_n = 0;

// The one menu currently being shown (TrackPopupMenu session).
static int      g_tp_idx  = -1;
static uint32_t g_tp_hwnd = 0;
static int      g_tp_x = 0, g_tp_y = 0;

static int menu_from_handle(uint32_t h){
    if (h < HMENU_BASE) return -1;
    uint32_t i = h - HMENU_BASE;
    if (i >= (uint32_t)W32_MAX_MENUS || !g_menu[i].used) return -1;
    return (int)i;
}

static int win_from_handle(uint32_t h){
    if (h < HWND_BASE) return -1;
    uint32_t i = h - HWND_BASE;
    if (i >= (uint32_t)W32_MAX_WINDOWS || !g_win[i].used) return -1;
    return (int)i;
}
static int dc_from_handle(uint32_t h){
    if (h < HDC_BASE) return -1;
    uint32_t i = h - HDC_BASE;
    if (i >= (uint32_t)W32_MAX_DC || !g_dc[i].used) return -1;
    return (int)i;
}
static int obj_from_handle(uint32_t h){
    if (h < HOBJ_BASE) return -1;
    uint32_t i = h - HOBJ_BASE;
    if (i >= (uint32_t)W32_MAX_OBJ || !g_obj[i].used) return -1;
    return (int)i;
}
// COLORREF is 0x00BBGGRR -> our compositor uses 0x00RRGGBB
static uint32_t cref_to_rgb(uint32_t c){
    return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

static W32DrawCmd* cmd_push(int wi){
    if (wi < 0 || g_win[wi].cmd_n >= W32_MAX_CMDS) return 0;
    W32DrawCmd* c = &g_win[wi].cmds[g_win[wi].cmd_n++];
    w_set(c, 0, sizeof(W32DrawCmd));
    c->bkcolor = 0xFFFFFFFFu;
    return c;
}

// ---------------------------------------------------------------------
//  Windows messages we emulate
// ---------------------------------------------------------------------
constexpr uint32_t WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_SIZE = 0x0005;
constexpr uint32_t WM_PAINT  = 0x000F, WM_CLOSE   = 0x0010, WM_QUIT = 0x0012;
constexpr uint32_t WM_KEYDOWN = 0x0100, WM_CHAR = 0x0102;
constexpr uint32_t WM_COMMAND = 0x0111;
constexpr uint32_t WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_MOUSEMOVE = 0x0200;

// ---------------------------------------------------------------------
//  Last-error / misc process state
// ---------------------------------------------------------------------
static uint32_t g_last_error = 0;
static uint32_t g_tick       = 0;
static char     g_cmdline[128];
static char     g_modpath[64];
static int      g_exitcode   = 0;
static bool     g_quit       = false;
static int      g_getmsg_n   = 0;

// console capture for the app's stdout
static char g_con[2048];
static int  g_con_len = 0;
static void con_put(const char* s, int n){
    for (int i = 0; i < n && g_con_len < (int)sizeof(g_con)-1; i++) g_con[g_con_len++] = s[i];
    g_con[g_con_len] = 0;
    // Mirror to the host writer (term + serial) so headless tests can
    // assert on Win32 app output (puts / printf / WriteConsoleA).
    if (g_writer && n > 0 && n < 256){
        char tmp[256];
        for (int i = 0; i < n; i++) tmp[i] = s[i];
        tmp[n] = 0;
        g_writer(tmp);
    }
}

// simple bump heap for the app (HeapAlloc / VirtualAlloc / malloc)
static uint8_t* g_apool     = 0;
static uint32_t g_apool_sz  = 0;
static uint32_t g_apool_use = 0;
static void* app_alloc(uint32_t n){
    n = (n + 15u) & ~15u;
    if (!g_apool || g_apool_use + n > g_apool_sz) return 0;
    void* p = g_apool + g_apool_use;
    g_apool_use += n;
    w_set(p, 0, n);
    return p;
}

// =====================================================================
//  4.  kernel32.dll
// =====================================================================
static uint32_t WINAPI K_GetLastError(void){ return g_last_error; }
static void     WINAPI K_SetLastError(uint32_t e){ g_last_error = e; }
static uint32_t WINAPI K_GetStdHandle(uint32_t n){ return 0x00F0 + (n & 0xF); }
static uint32_t WINAPI K_GetCurrentProcessId(void){ return 0x0BAD; }
static uint32_t WINAPI K_GetCurrentThreadId(void){ return 0x0BAE; }
static uint32_t WINAPI K_GetCurrentProcess(void){ return 0xFFFFFFFFu; }
static uint32_t WINAPI K_GetTickCount(void){ g_tick += 16; return g_tick; }
static void     WINAPI K_Sleep(uint32_t ms){ g_tick += ms; }
static uint32_t WINAPI K_GetCommandLineA(void){ return (uint32_t)(uintptr_t)(void*)g_cmdline; }
static uint32_t WINAPI K_GetACP(void){ return 936; }        // GBK
static uint32_t WINAPI K_GetOEMCP(void){ return 936; }
static uint32_t WINAPI K_GetVersion(void){ return 0x0A280000u | 10u; }

static int WINAPI K_WriteConsoleA(uint32_t, const char* buf, uint32_t n, uint32_t* written, void*){
    if (buf && n) con_put(buf, (int)n);
    if (written) *written = n;
    return 1;
}
static void WINAPI K_ExitProcess(uint32_t code){
    g_exitcode = (int)code;
    g_quit = true;
    // Return normally: the loader called the entry point and will regain
    // control when it returns.  A real ExitProcess never returns, but we
    // cannot longjmp out of ring-0 app code safely.
}

// ---------------------------------------------------------------------
//  Threading (stage-1: cooperative / inline execution)
//
//  NexOS does not yet have a pre-emptive scheduler for PE threads, so
//  CreateThread runs the worker inline (synchronously) and returns a fake
//  handle.  This is enough for real Windows programs that only need the
//  API to exist and link (e.g. a worker that prints and returns).  A real
//  scheduler is a later milestone (roadmap P4 stage 3+).
// ---------------------------------------------------------------------
typedef uint32_t (WINAPI *K_THREAD_START)(void*);
static uint32_t WINAPI K_CreateThread(uint32_t, uint32_t, uint32_t start,
                                       uint32_t param, uint32_t, uint32_t* tid){
    if (!start) return 0;
    if (tid) *tid = 1;
    ((K_THREAD_START)(void*)start)((void*)(uintptr_t)param);
    return 0x00E0;   // fake thread handle
}
static void WINAPI K_ExitThread(uint32_t){ /* no-op for stage-1 */ }
static void WINAPI K_OutputDebugStringA(const char* s){ if (s) { w32_serial("[app] "); w32_serial(s); } }

static uint32_t WINAPI K_GetModuleHandleA(const char*){ return 0x00400000u; }
static uint32_t WINAPI K_LoadLibraryA(const char*){ return 0x00400000u; }
static int      WINAPI K_FreeLibrary(uint32_t){ return 1; }
// Real resolution, not a stub: the same table the import fixer walks, so
// a program can bind an optional export (NexOS.dll's AI entry points,
// say) at run time and degrade gracefully when it is absent.
static void* w32_resolve(const char* dll, const char* fn);   // fwd
static uint32_t WINAPI K_GetProcAddress(uint32_t, const char* fn){
    return (uint32_t)(uintptr_t)w32_resolve(0, fn);
}
static uint32_t WINAPI K_GetModuleFileNameA(uint32_t, char* buf, uint32_t sz){
    if (!buf || !sz) return 0;
    w_ncpy(buf, g_modpath, (int)sz);
    return (uint32_t)w_len(buf);
}

static uint32_t WINAPI K_GetProcessHeap(void){ return 0x00A0; }
static void*    WINAPI K_HeapAlloc(uint32_t, uint32_t, uint32_t n){ return app_alloc(n); }
static int      WINAPI K_HeapFree(uint32_t, uint32_t, void*){ return 1; }
static void*    WINAPI K_VirtualAlloc(void*, uint32_t n, uint32_t, uint32_t){ return app_alloc(n); }
static int      WINAPI K_VirtualFree(void*, uint32_t, uint32_t){ return 1; }
static void*    WINAPI K_LocalAlloc(uint32_t, uint32_t n){ return app_alloc(n); }
static void*    WINAPI K_GlobalAlloc(uint32_t, uint32_t n){ return app_alloc(n); }

static uint32_t WINAPI K_GetEnvironmentVariableA(const char* n, char* buf, uint32_t sz){
    const char* v = env_get(n);
    if (!v){ g_last_error = 203; if (buf && sz) buf[0]=0; return 0; }
    if (buf && sz) w_ncpy(buf, v, (int)sz);
    return (uint32_t)w_len(v);
}
static int WINAPI K_SetEnvironmentVariableA(const char* n, const char* v){
    if (!n) return 0; env_put(n, v ? v : ""); return 1;
}
static uint32_t WINAPI K_ExpandEnvironmentStringsA(const char* src, char* dst, uint32_t sz){
    if (!src || !dst || !sz) return 0;
    uint32_t o = 0;
    for (const char* p = src; *p && o < sz-1; ){
        if (*p == '%'){
            const char* e = p+1; char nm[32]; int i=0;
            while (*e && *e != '%' && i < 31) nm[i++] = *e++;
            nm[i] = 0;
            if (*e == '%'){
                const char* v = env_get(nm);
                if (v) { for (const char* q=v; *q && o<sz-1; q++) dst[o++]=*q; }
                p = e+1; continue;
            }
        }
        dst[o++] = *p++;
    }
    dst[o] = 0;
    return o;
}
static int WINAPI K_GetComputerNameA(char* buf, uint32_t* sz){
    const char* n = "NexOS-PC";
    if (buf) w_cpy(buf, n);
    if (sz) *sz = (uint32_t)w_len(n);
    return 1;
}
static int WINAPI K_GetComputerNameExA(int, char* buf, uint32_t* sz){
    return K_GetComputerNameA(buf, sz);
}
static uint32_t WINAPI K_GetSystemDirectoryA(char* buf, uint32_t sz){
    const char* p = "C:\\WINDOWS\\system32";
    if (buf && sz) w_ncpy(buf, p, (int)sz);
    return (uint32_t)w_len(p);
}
static uint32_t WINAPI K_GetWindowsDirectoryA(char* buf, uint32_t sz){
    const char* p = "C:\\WINDOWS";
    if (buf && sz) w_ncpy(buf, p, (int)sz);
    return (uint32_t)w_len(p);
}
static uint32_t WINAPI K_GetTempPathA(uint32_t sz, char* buf){
    const char* p = "C:\\Users\\User\\AppData\\Local\\Temp";
    if (buf && sz) w_ncpy(buf, p, (int)sz);
    return (uint32_t)w_len(p);
}
static uint32_t WINAPI K_GetFullPathNameA(const char* p, uint32_t sz, char* buf, char** file){
    if (!p) return 0;
    if (buf && sz) w_ncpy(buf, p, (int)sz);
    if (file) {
        const char* s = p + w_len(p);
        while (s > p && s[-1] != '\\' && s[-1] != '/') s--;
        *file = (char*)s;
    }
    return (uint32_t)w_len(p);
}
static uint32_t WINAPI K_GetFileAttributesA(const char*){ return 0x80; } // FILE_ATTRIBUTE_NORMAL
static uint32_t WINAPI K_GetDriveTypeA(const char*){ return 3; } // DRIVE_FIXED
static uint32_t WINAPI K_GetLogicalDrives(void){ return 1 << ('C' - 'A'); }

struct W32SysTime { uint16_t y, mo, dow, d, h, mi, s, ms; };
static void WINAPI K_GetLocalTime(W32SysTime* t){
    if (!t) return;
    t->y=2026; t->mo=8; t->dow=3; t->d=5; t->h=12; t->mi=0; t->s=0; t->ms=0;
}
static void WINAPI K_GetSystemTime(W32SysTime* t){ K_GetLocalTime(t); }

struct W32SysInfo { uint32_t oem; uint32_t page; void* minAddr; void* maxAddr; uint32_t mask;
                    uint32_t nproc; uint32_t ptype; uint32_t alloc; uint16_t lvl; uint16_t rev; };
static void WINAPI K_GetSystemInfo(W32SysInfo* si){
    if (!si) return;
    w_set(si, 0, sizeof(*si));
    si->page = 4096; si->nproc = 1; si->ptype = 586; si->alloc = 65536;
    si->lvl = 6; si->rev = 0x8E0A;
    si->minAddr = (void*)0x00010000; si->maxAddr = (void*)0x7FFEFFFF; si->mask = 1;
}
struct W32OsVer { uint32_t size, major, minor, build, platform; char csd[128]; };
static int WINAPI K_GetVersionExA(W32OsVer* v){
    if (!v) return 0;
    v->major = 10; v->minor = 0; v->build = 19045; v->platform = 2;
    w_cpy(v->csd, "NexOS Win32 Subsystem");
    return 1;
}
static int WINAPI K_IsProcessorFeaturePresent(uint32_t){ return 1; }
static void WINAPI K_InitializeCriticalSection(void*){}
static void WINAPI K_EnterCriticalSection(void*){}
static void WINAPI K_LeaveCriticalSection(void*){}
static void WINAPI K_DeleteCriticalSection(void*){}
static uint32_t WINAPI K_SetUnhandledExceptionFilter(void*){ return 0; }

static int WINAPI K_lstrlenA(const char* s){ return w_len(s); }
static char* WINAPI K_lstrcpyA(char* d, const char* s){ if (d && s) w_cpy(d, s); return d; }
static char* WINAPI K_lstrcatA(char* d, const char* s){ if (d && s) w_cat(d, s); return d; }
static int WINAPI K_lstrcmpA(const char* a, const char* b){ return w_cmp(a, b); }
static int WINAPI K_lstrcmpiA(const char* a, const char* b){ return w_icmp(a, b); }

// file APIs backed by the NexOS file systems
static uint8_t  g_fbuf[4096];
static int      g_fsize = 0, g_fpos = 0;

// write session (CREATE_ALWAYS / GENERIC_WRITE): accumulates bytes and is
// flushed to the MKFS data FS by K_CloseHandle via kern_fs_create().
static uint8_t* g_wbuf = 0;
static int      g_wcap = 0, g_wsize = 0;
static char     g_wname[256];
static const uint32_t H_WRITE = 0x00F6;

static uint32_t WINAPI K_CreateFileA(const char* name, uint32_t access, uint32_t,
                                      void*, uint32_t disp, uint32_t, uint32_t){
    if (!name) return 0xFFFFFFFFu;
    // A write session is opened when the caller asks for write access or a
    // creation/disposition that implies writing.  GENERIC_WRITE = 0x40000000.
    bool writing = (access & 0x40000000u) || disp == 1 /*CREATE_NEW*/ ||
                   disp == 2 /*CREATE_ALWAYS*/ || disp == 4 /*OPEN_ALWAYS*/ ||
                   disp == 5 /*TRUNCATE_EXISTING*/;
    if (writing){
        if (!g_wbuf){ g_wbuf = (uint8_t*)app_alloc(8192); g_wcap = 8192; }
        g_wsize = 0;
        int i = 0; for (; name[i] && i < 255; i++) g_wname[i] = name[i];
        g_wname[i] = 0;
        return H_WRITE;
    }
    if (!g_reader) return 0xFFFFFFFFu;
    int r = g_reader(name, g_fbuf, (int)sizeof(g_fbuf));
    if (r < 0) { g_last_error = 2; return 0xFFFFFFFFu; }
    g_fsize = r; g_fpos = 0;
    return 0x00F5;
}
static int WINAPI K_ReadFile(uint32_t h, void* buf, uint32_t n, uint32_t* got, void*){
    if (h != 0x00F5 || !buf) return 0;
    uint32_t avail = (uint32_t)(g_fsize - g_fpos);
    if (n > avail) n = avail;
    w_mov(buf, g_fbuf + g_fpos, n);
    g_fpos += (int)n;
    if (got) *got = n;
    return 1;
}
static uint32_t WINAPI K_GetFileSize(uint32_t, uint32_t*){ return (uint32_t)g_fsize; }
static int WINAPI K_WriteFile(uint32_t h, const void* buf, uint32_t n, uint32_t* written, void*){
    if (h == H_WRITE){
        if (!buf || !n) { if (written) *written = 0; return 1; }
        if (g_wsize + (int)n > g_wcap){
            int nc = g_wcap * 2;
            while (nc < g_wsize + (int)n) nc *= 2;
            uint8_t* nb = (uint8_t*)app_alloc((uint32_t)nc);
            for (int i = 0; i < g_wsize; i++) nb[i] = g_wbuf[i];
            g_wbuf = nb; g_wcap = nc;
        }
        for (uint32_t i = 0; i < n; i++) g_wbuf[g_wsize + (int)i] = ((const uint8_t*)buf)[i];
        g_wsize += (int)n;
        if (written) *written = n;
        return 1;
    }
    if ((h & 0xFFF0) == 0x00F0 && buf && n) con_put((const char*)buf, (int)n);
    if (written) *written = n;
    return 1;
}
static int WINAPI K_CloseHandle(uint32_t h){
    if (h == H_WRITE){
        int rc = kern_fs_create(g_wname, g_wbuf, g_wsize);
        g_wsize = 0;
        return rc >= 0 ? 1 : 0;
    }
    return 1;
}
static uint32_t WINAPI K_FindFirstFileA(const char*, void*){ g_last_error = 2; return 0xFFFFFFFFu; }
static int      WINAPI K_FindNextFileA(uint32_t, void*){ g_last_error = 18; return 0; }
static int      WINAPI K_FindClose(uint32_t h){ return (h != 0xFFFFFFFFu) ? 1 : 0; }
static uint32_t WINAPI K_SetFilePointer(uint32_t h, int32_t d, int32_t* hi, uint32_t m){
    if (h != 0x00F5) { g_last_error = 6; return 0xFFFFFFFFu; }
    int np = g_fpos;
    if (m == 0) np = d + (hi ? *hi : 0);
    else if (m == 1) np += d;
    else if (m == 2) np = g_fsize + d;
    if (np < 0) np = 0;
    if (np > g_fsize) np = g_fsize;
    g_fpos = np;
    if (hi) *hi = 0;
    return (uint32_t)np;
}
static int WINAPI K_MultiByteToWideChar(uint32_t, uint32_t, const char* s, int, uint16_t* out, int cch){
    if (!s) return 0;
    int n = w_len(s);
    if (out && cch > 0){ int i=0; for (; i<n && i<cch-1; i++) out[i]=(uint16_t)(uint8_t)s[i]; out[i]=0; }
    return n + 1;
}
static int WINAPI K_WideCharToMultiByte(uint32_t, uint32_t, const uint16_t* s, int, char* out, int cb, void*, void*){
    if (!s) return 0;
    int n = 0; while (s[n]) n++;
    if (out && cb > 0){ int i=0; for (; i<n && i<cb-1; i++) out[i]=(char)s[i]; out[i]=0; }
    return n + 1;
}

// =====================================================================
//  5.  user32.dll
// =====================================================================
struct W32WndClassA {
    uint32_t style; WNDPROC proc; int cbCls, cbWnd;
    uint32_t hInst, hIcon, hCursor, hbrBackground;
    const char* menu; const char* name;
};
struct W32Rect { int32_t left, top, right, bottom; };
struct W32Msg  { uint32_t hwnd, message, wParam, lParam, time; int32_t px, py; };
struct W32Paint{ uint32_t hdc; int32_t fErase; W32Rect rc; int32_t restore, incUpdate; uint8_t rgb[32]; };

static int cls_find(const char* n){
    if (!n) return -1;
    for (int i=0;i<W32_MAX_CLASSES;i++) if (g_cls[i].used && !w_icmp(g_cls[i].name,n)) return i;
    return -1;
}
static uint16_t WINAPI U_RegisterClassA(const W32WndClassA* c){
    if (!c || !c->name) return 0;
    for (int i=0;i<W32_MAX_CLASSES;i++){
        if (!g_cls[i].used){
            g_cls[i].used = true;
            w_ncpy(g_cls[i].name, c->name, 32);
            g_cls[i].proc = c->proc;
            g_cls[i].bkbrush = c->hbrBackground;
            return (uint16_t)(0xC000 + i);
        }
    }
    return 0;
}
// WNDCLASSEXA has cbSize first, then the same layout
static uint16_t WINAPI U_RegisterClassExA(const uint8_t* p){
    if (!p) return 0;
    return U_RegisterClassA((const W32WndClassA*)(p + 4));
}
static int WINAPI U_UnregisterClassA(const char*, uint32_t){ return 1; }

static uint32_t WINAPI U_CreateWindowExA(uint32_t, const char* cls, const char* title,
                                         uint32_t style, int x, int y, int w, int h,
                                         uint32_t parent, uint32_t menu, uint32_t, void*){
    if (g_win_n >= W32_MAX_WINDOWS) return 0;
    int ci = cls_find(cls);
    // Child controls (BUTTON/STATIC/EDIT) become draw commands on the parent
    int pw = win_from_handle(parent);
    if (ci < 0 && pw >= 0){
        W32DrawCmd d;
        w_set(&d, 0, sizeof(d));
        d.bkcolor = 0xFFFFFFFFu;
        bool btn = cls && !w_icmp(cls, "BUTTON");
        d.kind  = btn ? (uint8_t)W32_CMD_BUTTON : (uint8_t)W32_CMD_TEXT;
        d.x=(int16_t)x; d.y=(int16_t)y; d.w=(int16_t)w; d.h=(int16_t)h;
        d.color = 0x202020;
        d.id = (uint16_t)menu;          // control id for hit-testing
        w_ncpy(d.text, title ? title : "", 48);
        // remember it so every repaint keeps the control alive
        if (g_win[pw].ctl_n < 8) g_win[pw].ctl[g_win[pw].ctl_n++] = d;
        W32DrawCmd* p2 = cmd_push(pw);
        if (p2) *p2 = d;
        return HWND_BASE + 0x1000 + (uint32_t)menu;   // pseudo child handle
    }
    if (ci < 0) return 0;

    int i = -1;
    for (int k=0;k<W32_MAX_WINDOWS;k++) if (!g_win[k].used) { i = k; break; }
    if (i < 0) return 0;
    W32Win& v = g_win[i];
    w_set(&v, 0, sizeof(v));
    v.used = true;
    v.hwnd = HWND_BASE + (uint32_t)i;
    v.proc = g_cls[ci].proc;
    w_ncpy(v.cls,   cls   ? cls   : "", 32);
    w_ncpy(v.title, title ? title : "NexOS Win32 App", 48);
    // CW_USEDEFAULT == 0x80000000
    v.x = (x == (int)0x80000000) ? 80  : x;
    v.y = (y == (int)0x80000000) ? 80  : y;
    v.w = (w == (int)0x80000000 || w <= 0) ? 480 : w;
    v.h = (h == (int)0x80000000 || h <= 0) ? 300 : h;
    if (v.w > 900) v.w = 900;
    if (v.h > 560) v.h = 560;
    v.visible = (style & 0x10000000u) != 0;   // WS_VISIBLE
    if (i + 1 > g_win_n) g_win_n = i + 1;
    if (v.proc) v.proc(v.hwnd, WM_CREATE, 0, 0);
    return v.hwnd;
}
static int WINAPI U_DestroyWindow(uint32_t h){
    int i = win_from_handle(h);
    if (i < 0) return 0;
    if (g_win[i].proc) g_win[i].proc(h, WM_DESTROY, 0, 0);
    g_win[i].used = false;
    return 1;
}
static int WINAPI U_ShowWindow(uint32_t h, int cmd){
    int i = win_from_handle(h);
    if (i < 0) return 0;
    g_win[i].visible = (cmd != 0);
    return 1;
}
static int WINAPI U_UpdateWindow(uint32_t h){
    int i = win_from_handle(h);
    if (i < 0) return 0;
    if (g_win[i].proc) g_win[i].proc(h, WM_PAINT, 0, 0);
    return 1;
}
static int WINAPI U_InvalidateRect(uint32_t h, const W32Rect*, int){
    int i = win_from_handle(h);
    if (i >= 0 && g_win[i].proc) g_win[i].proc(h, WM_PAINT, 0, 0);
    return 1;
}
static int WINAPI U_GetClientRect(uint32_t h, W32Rect* r){
    int i = win_from_handle(h);
    if (!r) return 0;
    r->left = r->top = 0;
    r->right  = (i >= 0) ? g_win[i].w : 480;
    r->bottom = (i >= 0) ? g_win[i].h : 300;
    return 1;
}
static int WINAPI U_GetWindowRect(uint32_t h, W32Rect* r){
    int i = win_from_handle(h);
    if (!r || i < 0) return 0;
    r->left = g_win[i].x; r->top = g_win[i].y;
    r->right = g_win[i].x + g_win[i].w; r->bottom = g_win[i].y + g_win[i].h;
    return 1;
}
static int WINAPI U_SetWindowTextA(uint32_t h, const char* s){
    int i = win_from_handle(h);
    if (i < 0) return 0;
    w_ncpy(g_win[i].title, s ? s : "", 48);
    return 1;
}
static int WINAPI U_IsWindow(uint32_t h){ return win_from_handle(h) >= 0 ? 1 : 0; }
static int WINAPI U_IsWindowVisible(uint32_t h){
    int i = win_from_handle(h); return (i >= 0 && g_win[i].visible) ? 1 : 0;
}
static uint32_t WINAPI U_GetDesktopWindow(void){ return HWND_BASE; }
static uint32_t WINAPI U_GetForegroundWindow(void){
    for (int i = 0; i < W32_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].visible) return HWND_BASE + (uint32_t)i;
    return 0;
}
static int WINAPI U_GetWindowTextA(uint32_t h, char* buf, int sz){
    int i = win_from_handle(h);
    if (i < 0 || !buf || sz <= 0) return 0;
    w_ncpy(buf, g_win[i].title, sz);
    return (int)w_len(buf);
}
static int WINAPI U_GetClassNameA(uint32_t h, char* buf, int sz){
    int i = win_from_handle(h);
    if (i < 0 || !buf || sz <= 0) return 0;
    w_ncpy(buf, g_win[i].cls, sz);
    return (int)w_len(buf);
}
static uint32_t WINAPI U_SetFocus(uint32_t h){ return h; }
static uint32_t WINAPI U_GetFocus(void){ return U_GetForegroundWindow(); }
static int WINAPI U_GetCursorPos(void*){ return 1; }
static int WINAPI U_SetCursorPos(int, int){ return 1; }
static int WINAPI U_ClientToScreen(uint32_t, void*){ return 1; }
static int WINAPI U_ScreenToClient(uint32_t, void*){ return 1; }

static uint32_t WINAPI U_BeginPaint(uint32_t h, W32Paint* ps){
    int wi = win_from_handle(h);
    int di = -1;
    for (int i=0;i<W32_MAX_DC;i++) if (!g_dc[i].used){ di = i; break; }
    if (di < 0) di = 0;
    g_dc[di].used = true; g_dc[di].win = wi;
    g_dc[di].textcolor = 0x00000000; g_dc[di].bkcolor = 0x00FFFFFF;
    g_dc[di].pencolor = 0x00000000;  g_dc[di].brushcolor = 0x00FFFFFF;
    g_dc[di].bkmode = 2; g_dc[di].curx = g_dc[di].cury = 0;
    if (wi >= 0){ g_win[wi].cmd_n = 0; g_win[wi].painting = true; }
    if (ps){
        w_set(ps, 0, sizeof(*ps));
        ps->hdc = HDC_BASE + (uint32_t)di;
        ps->fErase = 1;
        ps->rc.right = (wi >= 0) ? g_win[wi].w : 480;
        ps->rc.bottom= (wi >= 0) ? g_win[wi].h : 300;
    }
    return HDC_BASE + (uint32_t)di;
}
static int WINAPI U_EndPaint(uint32_t h, const W32Paint*){
    int wi = win_from_handle(h);
    if (wi >= 0){
        g_win[wi].painting = false;
        // child controls are drawn on top of whatever the parent painted
        for (int c = 0; c < g_win[wi].ctl_n; c++){
            W32DrawCmd* d = cmd_push(wi);
            if (!d) break;
            *d = g_win[wi].ctl[c];
        }
    }
    for (int i=0;i<W32_MAX_DC;i++) if (g_dc[i].win == wi) g_dc[i].used = false;
    return 1;
}
static uint32_t WINAPI U_GetDC(uint32_t h){ return U_BeginPaint(h, 0); }
static int WINAPI U_ReleaseDC(uint32_t h, uint32_t){ return U_EndPaint(h, 0); }

static int WINAPI U_FillRect(uint32_t hdc, const W32Rect* r, uint32_t brush){
    int di = dc_from_handle(hdc); if (di < 0 || !r) return 0;
    W32DrawCmd* c = cmd_push(g_dc[di].win); if (!c) return 0;
    int oi = obj_from_handle(brush);
    c->kind = W32_CMD_FILLRECT;
    c->x=(int16_t)r->left; c->y=(int16_t)r->top;
    c->w=(int16_t)(r->right-r->left); c->h=(int16_t)(r->bottom-r->top);
    c->color = (oi >= 0) ? cref_to_rgb(g_obj[oi].color) : 0x00F0F0F0;
    return 1;
}
static int WINAPI U_DrawTextA(uint32_t hdc, const char* s, int, W32Rect* r, uint32_t){
    int di = dc_from_handle(hdc); if (di < 0 || !s) return 0;
    W32DrawCmd* c = cmd_push(g_dc[di].win); if (!c) return 0;
    c->kind = W32_CMD_TEXT;
    c->x = (int16_t)(r ? r->left + 4 : 8);
    c->y = (int16_t)(r ? r->top  + 4 : 8);
    c->color = cref_to_rgb(g_dc[di].textcolor);
    c->bkcolor = (g_dc[di].bkmode == 2) ? cref_to_rgb(g_dc[di].bkcolor) : 0xFFFFFFFFu;
    w_ncpy(c->text, s, 48);
    return 8;
}
static int WINAPI U_MessageBoxA(uint32_t, const char* text, const char* cap, uint32_t){
    int i = -1;
    for (int k=0;k<W32_MAX_WINDOWS;k++) if (!g_win[k].used) { i = k; break; }
    if (i < 0) return 1;
    W32Win& v = g_win[i];
    w_set(&v, 0, sizeof(v));
    v.used = v.visible = v.is_msgbox = true;
    v.hwnd = HWND_BASE + (uint32_t)i;
    w_ncpy(v.cls, "#32770", 32);
    w_ncpy(v.title, cap ? cap : "NexOS", 48);
    W32DrawCmd* c = cmd_push(i);
    if (c){ c->kind = W32_CMD_TEXT; c->x = 16; c->y = 20; c->color = 0x101010;
            w_ncpy(c->text, text ? text : "", 48); }
    W32DrawCmd* b = cmd_push(i);
    if (b){ b->kind = W32_CMD_BUTTON; b->x = 130; b->y = 70; b->w = 76; b->h = 26;
            b->color = 0x202020; w_cpy(b->text, "OK"); }
    v.w = 340; v.h = 130; v.x = 200; v.y = 160;
    if (i + 1 > g_win_n) g_win_n = i + 1;
    return 1;    // IDOK
}
static int WINAPI U_MessageBeep(uint32_t){ return 1; }

// ---- user32: popup menus ---------------------------------------------
// Menu-item flag bits we honour (subset of MF_*).
constexpr uint32_t MF_STRING    = 0x0000;
constexpr uint32_t MF_BYCOMMAND = 0x0000;
constexpr uint32_t MF_GRAYED    = 0x0001;
constexpr uint32_t MF_DISABLED  = 0x0002;
constexpr uint32_t MF_CHECKED   = 0x0008;
constexpr uint32_t MF_POPUP     = 0x0010;
constexpr uint32_t MF_BYPOSITION= 0x0400;
constexpr uint32_t MF_SEPARATOR = 0x0800;

static uint32_t WINAPI U_CreatePopupMenu(void){
    int i = -1;
    for (int k = 0; k < W32_MAX_MENUS; k++) if (!g_menu[k].used) { i = k; break; }
    if (i < 0) return 0;
    w_set(&g_menu[i], 0, sizeof(g_menu[i]));
    g_menu[i].used = true;
    g_menu[i].h    = HMENU_BASE + (uint32_t)i;
    if (i + 1 > g_menu_n) g_menu_n = i + 1;
    return g_menu[i].h;
}
static int WINAPI U_AppendMenuA(uint32_t h, uint32_t flags, uint32_t id, const char* text){
    int mi = menu_from_handle(h);
    if (mi < 0 || g_menu[mi].n >= 14) return 0;
    W32MenuItem& it = g_menu[mi].items[g_menu[mi].n++];
    it.id = id; it.flags = flags;
    w_cpy(it.text, (flags & MF_SEPARATOR) ? "" : (text ? text : ""));
    return 1;
}
static int WINAPI U_InsertMenuA(uint32_t h, uint32_t pos, uint32_t flags,
                                uint32_t id, const char* text){
    int mi = menu_from_handle(h);
    if (mi < 0) return 0;
    W32Menu& m = g_menu[mi];
    if (pos > (uint32_t)m.n || m.n >= 14) return 0;
    for (int i = m.n; i > (int)pos; i--) m.items[i] = m.items[i - 1];
    m.items[(int)pos].id = id; m.items[(int)pos].flags = flags;
    w_cpy(m.items[(int)pos].text, (flags & MF_SEPARATOR) ? "" : (text ? text : ""));
    m.n++;
    return 1;
}
static int WINAPI U_DeleteMenu(uint32_t h, uint32_t pos, uint32_t flags){
    int mi = menu_from_handle(h);
    if (mi < 0) return 0;
    W32Menu& m = g_menu[mi];
    if (pos >= (uint32_t)m.n || m.n <= 0) return 0;
    for (uint32_t i = pos; i + 1 < (uint32_t)m.n; i++) m.items[i] = m.items[i + 1];
    m.n--;
    return 1;
}
static int WINAPI U_RemoveMenu(uint32_t h, uint32_t pos, uint32_t flags){
    return U_DeleteMenu(h, pos, flags);
}
static int WINAPI U_DestroyMenu(uint32_t h){
    int mi = menu_from_handle(h);
    if (mi < 0) return 0;
    g_menu[mi].used = false;
    if (g_tp_idx == mi) g_tp_idx = -1;
    return 1;
}
static int WINAPI U_GetMenuItemCount(uint32_t h){
    int mi = menu_from_handle(h);
    return mi < 0 ? -1 : g_menu[mi].n;
}
static int WINAPI U_GetMenuStringA(uint32_t h, uint32_t pos, char* buf, int size, uint32_t flags){
    int mi = menu_from_handle(h);
    if (mi < 0 || !buf || size <= 0) return 0;
    W32Menu& m = g_menu[mi];
    int idx = -1;
    if ((flags & MF_BYPOSITION) == 0) {          // MF_BYCOMMAND: find by id
        for (int i = 0; i < m.n; i++)
            if (m.items[i].id == pos) { idx = i; break; }
    } else {
        if (pos < (uint32_t)m.n) idx = (int)pos;
    }
    if (idx < 0) return 0;
    const char* s = m.items[idx].text;
    int i = 0;
    while (s[i] && i < size - 1) { buf[i] = s[i]; i++; }
    buf[i] = 0;
    return i;
}
static int WINAPI U_EnableMenuItem(uint32_t h, uint32_t id, uint32_t flags){
    int mi = menu_from_handle(h);
    if (mi < 0) return 0;
    W32Menu& m = g_menu[mi];
    for (int i = 0; i < m.n; i++)
        if (m.items[i].id == id){
            m.items[i].flags = (m.items[i].flags & ~(MF_GRAYED | MF_DISABLED))
                             | (flags & (MF_GRAYED | MF_DISABLED));
            return -1;    // MF_ORIGINAL-ish: previous state was "enabled"
        }
    return 0;
}
static int WINAPI U_CheckMenuItem(uint32_t h, uint32_t id, uint32_t flags){
    int mi = menu_from_handle(h);
    if (mi < 0) return 0;
    W32Menu& m = g_menu[mi];
    for (int i = 0; i < m.n; i++)
        if (m.items[i].id == id){
            m.items[i].flags = (m.items[i].flags & ~MF_CHECKED)
                             | (flags & MF_CHECKED);
            return 0;
        }
    return 0;
}
static int WINAPI U_TrackPopupMenu(uint32_t h, uint32_t, int x, int y,
                                   uint32_t, uint32_t hwnd, uint32_t){
    int mi = menu_from_handle(h);
    if (mi < 0) return 0;
    g_tp_idx = mi; g_tp_hwnd = hwnd; g_tp_x = x; g_tp_y = y;
    return 1;    // TRUE: the compositor renders it and posts WM_COMMAND
}
static uint32_t WINAPI U_LoadIconA(uint32_t, const char*){ return 0x901; }
static uint32_t WINAPI U_LoadCursorA(uint32_t, const char*){ return 0x902; }
static int WINAPI U_GetMessageA(W32Msg* m, uint32_t, uint32_t, uint32_t){
    // Deliver a bounded, deterministic message stream: the app paints its
    // window, then receives WM_QUIT so WinMain returns to the loader.
    if (m) w_set(m, 0, sizeof(*m));
    if (g_quit || ++g_getmsg_n > 2) return 0;
    if (m){ m->message = 0x0113; m->hwnd = 0; }   // WM_TIMER, harmless
    return 1;
}
static int WINAPI U_PeekMessageA(W32Msg* m, uint32_t, uint32_t, uint32_t, uint32_t){
    if (m) w_set(m, 0, sizeof(*m));
    return 0;
}
static int WINAPI U_TranslateMessage(const W32Msg*){ return 1; }
static int WINAPI U_DispatchMessageA(const W32Msg* m){
    if (!m) return 0;
    int i = win_from_handle(m->hwnd);
    if (i >= 0 && g_win[i].proc) return g_win[i].proc(m->hwnd, m->message, m->wParam, m->lParam);
    return 0;
}
static void WINAPI U_PostQuitMessage(int code){ g_exitcode = code; g_quit = true; }
static int WINAPI U_DefWindowProcA(uint32_t h, uint32_t msg, uint32_t, uint32_t){
    if (msg == WM_CLOSE) { U_DestroyWindow(h); return 0; }
    if (msg == WM_DESTROY) { g_quit = true; return 0; }
    return 0;
}
static int WINAPI U_PostMessageA(uint32_t, uint32_t, uint32_t, uint32_t){ return 1; }
static int WINAPI U_SendMessageA(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp){
    int i = win_from_handle(h);
    if (i >= 0 && g_win[i].proc) return g_win[i].proc(h, msg, wp, lp);
    return 0;
}
static int WINAPI U_GetSystemMetrics(int idx){
    if (idx == 0) return 1024;
    if (idx == 1) return 768;
    return 0;
}
static int WINAPI U_wsprintfA(char* out, const char* fmt, ...){
    // minimal %s / %d / %x formatter over the stdcall-visible varargs
    if (!out || !fmt) return 0;
    const uint32_t* va = (const uint32_t*)((const uint8_t*)&fmt + sizeof(const char*));
    int o = 0;
    for (const char* p = fmt; *p; p++){
        if (*p != '%') { out[o++] = *p; continue; }
        p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == 's'){ const char* s = (const char*)(*va++); if (s) while (*s) out[o++] = *s++; }
        else if (*p == 'd' || *p == 'u'){ char t[12]; w_num(t, *va++); for (char* q=t; *q; q++) out[o++]=*q; }
        else if (*p == 'x' || *p == 'X'){ char t[12]; w_hex(t, *va++, 8); for (char* q=t; *q; q++) out[o++]=*q; }
        else if (*p == 'c'){ out[o++] = (char)(*va++); }
        else out[o++] = *p;
    }
    out[o] = 0;
    return o;
}

// =====================================================================
//  6.  gdi32.dll
// =====================================================================
static uint32_t WINAPI G_CreateSolidBrush(uint32_t c){
    for (int i=0;i<W32_MAX_OBJ;i++) if (!g_obj[i].used){
        g_obj[i].used = true; g_obj[i].kind = 1; g_obj[i].color = c;
        return HOBJ_BASE + (uint32_t)i;
    }
    return 0;
}
static uint32_t WINAPI G_CreatePen(int, int, uint32_t c){
    for (int i=0;i<W32_MAX_OBJ;i++) if (!g_obj[i].used){
        g_obj[i].used = true; g_obj[i].kind = 2; g_obj[i].color = c;
        return HOBJ_BASE + (uint32_t)i;
    }
    return 0;
}
static int WINAPI G_DeleteObject(uint32_t h){
    int i = obj_from_handle(h); if (i >= 0) g_obj[i].used = false; return 1;
}
static uint32_t WINAPI G_GetStockObject(int i){
    static uint32_t cache[8];
    static const uint32_t colors[8] = {0x00FFFFFF,0x00C0C0C0,0x00808080,0x00000000,
                                       0x00FFFFFF,0x00000000,0x00FFFFFF,0x00F0F0F0};
    int k = i & 7;
    if (!cache[k]) cache[k] = G_CreateSolidBrush(colors[k]);
    return cache[k];
}
static uint32_t WINAPI G_SelectObject(uint32_t hdc, uint32_t h){
    int di = dc_from_handle(hdc); int oi = obj_from_handle(h);
    if (di < 0 || oi < 0) return 0;
    if (g_obj[oi].kind == 1) g_dc[di].brushcolor = g_obj[oi].color;
    else if (g_obj[oi].kind == 2) g_dc[di].pencolor = g_obj[oi].color;
    return h;
}
static uint32_t WINAPI G_SetTextColor(uint32_t hdc, uint32_t c){
    int di = dc_from_handle(hdc); if (di < 0) return 0;
    uint32_t o = g_dc[di].textcolor; g_dc[di].textcolor = c; return o;
}
static uint32_t WINAPI G_SetBkColor(uint32_t hdc, uint32_t c){
    int di = dc_from_handle(hdc); if (di < 0) return 0;
    uint32_t o = g_dc[di].bkcolor; g_dc[di].bkcolor = c; return o;
}
static int WINAPI G_SetBkMode(uint32_t hdc, int m){
    int di = dc_from_handle(hdc); if (di < 0) return 0;
    int o = g_dc[di].bkmode; g_dc[di].bkmode = m; return o;
}
static int WINAPI G_TextOutA(uint32_t hdc, int x, int y, const char* s, int n){
    int di = dc_from_handle(hdc); if (di < 0 || !s) return 0;
    W32DrawCmd* c = cmd_push(g_dc[di].win); if (!c) return 0;
    c->kind = W32_CMD_TEXT; c->x=(int16_t)x; c->y=(int16_t)y;
    c->color = cref_to_rgb(g_dc[di].textcolor);
    c->bkcolor = (g_dc[di].bkmode == 2) ? cref_to_rgb(g_dc[di].bkcolor) : 0xFFFFFFFFu;
    int m = (n > 0 && n < 47) ? n : 47;
    int i = 0; for (; i < m && s[i]; i++) c->text[i] = s[i];
    c->text[i] = 0;
    return 1;
}
static int WINAPI G_Rectangle(uint32_t hdc, int l, int t, int r, int b){
    int di = dc_from_handle(hdc); if (di < 0) return 0;
    W32DrawCmd* f = cmd_push(g_dc[di].win);
    if (f){ f->kind=W32_CMD_FILLRECT; f->x=(int16_t)l; f->y=(int16_t)t;
            f->w=(int16_t)(r-l); f->h=(int16_t)(b-t); f->color=cref_to_rgb(g_dc[di].brushcolor); }
    W32DrawCmd* o = cmd_push(g_dc[di].win);
    if (o){ o->kind=W32_CMD_FRAMERECT; o->x=(int16_t)l; o->y=(int16_t)t;
            o->w=(int16_t)(r-l); o->h=(int16_t)(b-t); o->color=cref_to_rgb(g_dc[di].pencolor); }
    return 1;
}
static int WINAPI G_Ellipse(uint32_t hdc, int l, int t, int r, int b){
    int di = dc_from_handle(hdc); if (di < 0) return 0;
    W32DrawCmd* c = cmd_push(g_dc[di].win); if (!c) return 0;
    c->kind=W32_CMD_ELLIPSE; c->x=(int16_t)l; c->y=(int16_t)t;
    c->w=(int16_t)(r-l); c->h=(int16_t)(b-t); c->color=cref_to_rgb(g_dc[di].brushcolor);
    return 1;
}
static int WINAPI G_MoveToEx(uint32_t hdc, int x, int y, void*){
    int di = dc_from_handle(hdc); if (di < 0) return 0;
    g_dc[di].curx = x; g_dc[di].cury = y; return 1;
}
static int WINAPI G_LineTo(uint32_t hdc, int x, int y){
    int di = dc_from_handle(hdc); if (di < 0) return 0;
    W32DrawCmd* c = cmd_push(g_dc[di].win);
    if (c){ c->kind=W32_CMD_LINE; c->x=(int16_t)g_dc[di].curx; c->y=(int16_t)g_dc[di].cury;
            c->w=(int16_t)(x-g_dc[di].curx); c->h=(int16_t)(y-g_dc[di].cury);
            c->color=cref_to_rgb(g_dc[di].pencolor); }
    g_dc[di].curx = x; g_dc[di].cury = y;
    return 1;
}
static uint32_t WINAPI G_CreateFontA(int,int,int,int,int,uint32_t,uint32_t,uint32_t,
                                     uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,const char*){
    return HOBJ_BASE + 15;
}

struct W32TextMetricA {
    int32_t tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    int32_t tmAveCharWidth, tmMaxCharWidth, tmWeight;
    int8_t  tmItalic, tmUnderlined, tmStruckOut, tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    int8_t  tmPitchAndFamily, tmCharSet;
};
static int WINAPI G_GetTextMetricsA(uint32_t hdc, W32TextMetricA* tm){
    (void)hdc;
    if (!tm) return 0;
    w_set(tm, 0, sizeof(*tm));
    tm->tmHeight = 16; tm->tmAscent = 13; tm->tmDescent = 3;
    tm->tmAveCharWidth = 8; tm->tmMaxCharWidth = 16; tm->tmWeight = 400;
    tm->tmFirstChar = 32; tm->tmLastChar = 126; tm->tmCharSet = 1;
    return 1;
}
struct W32Size { int32_t cx, cy; };
static int WINAPI G_GetTextExtentPoint32A(uint32_t hdc, const char* s, int n, void* sz){
    (void)hdc;
    if (!s || !sz) return 0;
    int len = (n > 0) ? n : (int)w_len(s);
    W32Size* p = (W32Size*)sz;
    p->cx = len * 8; p->cy = 16;
    return 1;
}
static uint32_t WINAPI G_SetPixel(uint32_t hdc, int x, int y, uint32_t c){
    int di = dc_from_handle(hdc); if (di < 0) return 0xFFFFFFFFu;
    W32DrawCmd* cmd = cmd_push(g_dc[di].win); if (!cmd) return 0xFFFFFFFFu;
    cmd->kind = W32_CMD_PIXEL; cmd->x = (int16_t)x; cmd->y = (int16_t)y;
    cmd->color = cref_to_rgb(c);
    return c;
}

// =====================================================================
//  7.  advapi32.dll  -  the registry API surface
// =====================================================================
constexpr uint32_t HKEY_OPEN_BASE = 0x40000000u;

static int hkey_index(uint32_t h){
    if (h == 0x80000000u) return 0;
    if (h == 0x80000001u) return 1;
    if (h == 0x80000002u) return 2;
    if (h == 0x80000003u) return 3;
    if (h == 0x80000005u) return 4;
    if (h >= HKEY_OPEN_BASE && h < HKEY_OPEN_BASE + REG_MAX_KEYS)
        return (int)(h - HKEY_OPEN_BASE);
    return -1;
}
// walk a sub path relative to a key index
static int reg_sub(int base, const char* sub, bool create){
    if (base < 0) return -1;
    if (!sub || !sub[0]) return base;
    char comp[32]; int ci = 0; int cur = base;
    for (const char* p = sub;; p++){
        char c = *p;
        if (c=='\\' || c=='/' || c==0){
            comp[ci] = 0;
            if (ci){
                int nx = reg_child(cur, comp);
                if (nx < 0){
                    if (!create) return -1;
                    nx = reg_new_key(comp, cur);
                    if (nx < 0) return -1;
                }
                cur = nx;
            }
            ci = 0;
            if (!c) break;
        } else if (ci < 31) comp[ci++] = c;
    }
    return cur;
}
static uint32_t WINAPI A_RegOpenKeyExA(uint32_t root, const char* sub, uint32_t, uint32_t, uint32_t* out){
    int b = hkey_index(root);
    int k = reg_sub(b, sub, false);
    if (k < 0) return 2;                      // ERROR_FILE_NOT_FOUND
    if (out) *out = HKEY_OPEN_BASE + (uint32_t)k;
    return 0;
}
static uint32_t WINAPI A_RegOpenKeyA(uint32_t root, const char* sub, uint32_t* out){
    return A_RegOpenKeyExA(root, sub, 0, 0, out);
}
static uint32_t WINAPI A_RegCreateKeyExA(uint32_t root, const char* sub, uint32_t, const char*,
                                         uint32_t, uint32_t, void*, uint32_t* out, uint32_t* disp){
    int b = hkey_index(root);
    int k = reg_sub(b, sub, true);
    if (k < 0) return 5;
    if (out)  *out = HKEY_OPEN_BASE + (uint32_t)k;
    if (disp) *disp = 1;
    return 0;
}
static uint32_t WINAPI A_RegCloseKey(uint32_t){ return 0; }
static uint32_t WINAPI A_RegQueryValueExA(uint32_t key, const char* name, uint32_t*,
                                          uint32_t* type, uint8_t* data, uint32_t* cb){
    int k = hkey_index(key);
    RegValue* v = reg_find_val(k, name);
    if (!v) return 2;
    if (type) *type = v->type;
    uint32_t need = v->size;
    if (!data){ if (cb) *cb = need; return 0; }
    if (cb && *cb < need){ *cb = need; return 234; }   // ERROR_MORE_DATA
    w_mov(data, v->data, need);
    if (cb) *cb = need;
    return 0;
}
static uint32_t WINAPI A_RegSetValueExA(uint32_t key, const char* name, uint32_t,
                                        uint32_t type, const uint8_t* data, uint32_t cb){
    int k = hkey_index(key);
    if (k < 0) return 6;
    return reg_put_val(k, name, type, data, cb) == 0 ? 0u : 5u;
}
static uint32_t WINAPI A_RegDeleteValueA(uint32_t key, const char* name){
    int k = hkey_index(key);
    if (k < 0) return 6;
    int prev = -1;
    for (int v = g_keys[k].values; v >= 0; prev = v, v = g_vals[v].next){
        if (!w_icmp(g_vals[v].name, name ? name : "")){
            if (prev < 0) g_keys[k].values = g_vals[v].next;
            else          g_vals[prev].next = g_vals[v].next;
            return 0;
        }
    }
    return 2;
}
static uint32_t WINAPI A_RegDeleteKeyA(uint32_t root, const char* sub){
    int b = hkey_index(root);
    int k = reg_sub(b, sub, false);
    if (k < 0) return 2;
    int p = g_keys[k].parent;
    if (p < 0) return 5;
    if (g_keys[p].child == k) g_keys[p].child = g_keys[k].sibling;
    else {
        for (int s = g_keys[p].child; s >= 0; s = g_keys[s].sibling)
            if (g_keys[s].sibling == k){ g_keys[s].sibling = g_keys[k].sibling; break; }
    }
    return 0;
}
static uint32_t WINAPI A_RegEnumKeyExA(uint32_t key, uint32_t idx, char* name, uint32_t* cb,
                                       uint32_t*, char*, uint32_t*, void*){
    int k = hkey_index(key);
    if (k < 0) return 6;
    uint32_t n = 0;
    for (int c = g_keys[k].child; c >= 0; c = g_keys[c].sibling, n++){
        if (n == idx){
            if (name && cb) w_ncpy(name, g_keys[c].name, (int)*cb);
            if (cb) *cb = (uint32_t)w_len(g_keys[c].name);
            return 0;
        }
    }
    return 259;   // ERROR_NO_MORE_ITEMS
}
static uint32_t WINAPI A_RegEnumValueA(uint32_t key, uint32_t idx, char* name, uint32_t* ncb,
                                       uint32_t*, uint32_t* type, uint8_t* data, uint32_t* dcb){
    int k = hkey_index(key);
    if (k < 0) return 6;
    uint32_t n = 0;
    for (int v = g_keys[k].values; v >= 0; v = g_vals[v].next, n++){
        if (n == idx){
            if (name && ncb) w_ncpy(name, g_vals[v].name, (int)*ncb);
            if (ncb)  *ncb  = (uint32_t)w_len(g_vals[v].name);
            if (type) *type = g_vals[v].type;
            if (data && dcb && *dcb >= g_vals[v].size) w_mov(data, g_vals[v].data, g_vals[v].size);
            if (dcb)  *dcb  = g_vals[v].size;
            return 0;
        }
    }
    return 259;
}
static uint32_t WINAPI A_RegQueryInfoKeyA(uint32_t key, char*, uint32_t*, uint32_t*,
                                          uint32_t* nkeys, uint32_t*, uint32_t*,
                                          uint32_t* nvals, uint32_t*, uint32_t*, uint32_t*, void*){
    int k = hkey_index(key);
    if (k < 0) return 6;
    uint32_t a=0, b=0;
    for (int c = g_keys[k].child;  c >= 0; c = g_keys[c].sibling) a++;
    for (int v = g_keys[k].values; v >= 0; v = g_vals[v].next)    b++;
    if (nkeys) *nkeys = a;
    if (nvals) *nvals = b;
    return 0;
}
static int WINAPI A_GetUserNameA(char* buf, uint32_t* sz){
    const char* n = "User";
    if (buf) w_cpy(buf, n);
    if (sz) *sz = (uint32_t)w_len(n) + 1;
    return 1;
}

// =====================================================================
//  8.  msvcrt.dll  (cdecl!)
// =====================================================================
static int  C_puts(const char* s){ if (s){ con_put(s, w_len(s)); con_put("\n",1);} return 0; }
static int  C_putchar(int c){ char t=(char)c; con_put(&t,1); return c; }
static void* C_malloc(uint32_t n){ return app_alloc(n); }
static void C_free(void*){}
static int  C_strlen(const char* s){ return w_len(s); }
static char* C_strcpy(char* d, const char* s){ if (d&&s) w_cpy(d,s); return d; }
static int  C_strcmp(const char* a, const char* b){ return w_cmp(a,b); }
static void* C_memset(void* d, int v, uint32_t n){ w_set(d,v,n); return d; }
static void* C_memcpy(void* d, const void* s, uint32_t n){ w_mov(d,s,n); return d; }
static void C_exit(int c){ g_exitcode = c; g_quit = true; }
static int  C_printf(const char* fmt, ...){
    if (!fmt) return 0;
    char tmp[256];
    const uint32_t* va = (const uint32_t*)((const uint8_t*)&fmt + sizeof(const char*));
    int o = 0;
    for (const char* p = fmt; *p && o < 250; p++){
        if (*p != '%'){ tmp[o++] = *p; continue; }
        p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p=='s'){ const char* s=(const char*)(*va++); if (s) while (*s && o<250) tmp[o++]=*s++; }
        else if (*p=='d'||*p=='u'||*p=='i'){ char t[12]; w_num(t,*va++); for (char* q=t;*q&&o<250;q++) tmp[o++]=*q; }
        else if (*p=='x'||*p=='X'){ char t[12]; w_hex(t,*va++,8); for (char* q=t;*q&&o<250;q++) tmp[o++]=*q; }
        else if (*p=='c'){ tmp[o++]=(char)(*va++); }
        else tmp[o++]=*p;
    }
    tmp[o]=0;
    con_put(tmp,o);
    return o;
}

// =====================================================================
//  8b.  Additional commonly-used API stubs (window data, system colors,
//       timers, sync objects, dialogs, images, device caps)
// =====================================================================

// ---- per-hwnd extra-data map (GWLP_WNDPROC/GWLP_USERDATA/GWL_STYLE/GWL_EXSTYLE) ----
struct WndExtra { uint32_t hwnd; uint32_t wndproc; uint32_t user; uint32_t style; uint32_t exstyle; bool used; };
static WndExtra g_wle[64];
static int wle_find(uint32_t hwnd, bool create){
    int empty = -1;
    for (int i = 0; i < 64; i++){
        if (g_wle[i].used && g_wle[i].hwnd == hwnd) return i;
        if (!g_wle[i].used && empty < 0) empty = i;
    }
    if (create && empty >= 0){
        g_wle[empty].used = true; g_wle[empty].hwnd = hwnd;
        g_wle[empty].wndproc = 0; g_wle[empty].user = 0;
        g_wle[empty].style = 0x10000000u; g_wle[empty].exstyle = 0;
        return empty;
    }
    return -1;
}

static uint32_t WINAPI K_GetModuleHandleExA(uint32_t, const char*, uint32_t* out){
    if (out) *out = 0x00400000u; return 1;
}

// ---- user32: window extra data ----
static uint32_t WINAPI U_GetWindowLongA(uint32_t hwnd, int idx){
    int i = wle_find(hwnd, false); if (i < 0) return 0;
    switch (idx){
        case -4:  return g_wle[i].wndproc;   // GWLP_WNDPROC
        case -21: return g_wle[i].user;      // GWLP_USERDATA
        case -20: return g_wle[i].exstyle;   // GWL_EXSTYLE
        case -16: return g_wle[i].style;     // GWL_STYLE
        case 0:   return g_wle[i].user;      // some code uses 0 for userdata
        default:  return 0;
    }
}
static uint32_t WINAPI U_GetWindowLongW(uint32_t hwnd, int idx){ return U_GetWindowLongA(hwnd, idx); }
static uint32_t WINAPI U_SetWindowLongA(uint32_t hwnd, int idx, uint32_t val){
    int i = wle_find(hwnd, true); if (i < 0) return 0;
    switch (idx){
        case -4:  { uint32_t p = g_wle[i].wndproc; g_wle[i].wndproc = val; return p; }
        case -21: { uint32_t p = g_wle[i].user;    g_wle[i].user    = val; return p; }
        case -20: { uint32_t p = g_wle[i].exstyle; g_wle[i].exstyle = val; return p; }
        case -16: { uint32_t p = g_wle[i].style;   g_wle[i].style   = val; return p; }
        case 0:   { uint32_t p = g_wle[i].user;    g_wle[i].user    = val; return p; }
        default:  return 0;
    }
}
static uint32_t WINAPI U_SetWindowLongW(uint32_t hwnd, int idx, uint32_t val){ return U_SetWindowLongA(hwnd, idx, val); }

// ---- user32: system colors ----
static uint32_t sys_color(int i){
    switch (i){
        case 1:  return 0x00C8C8C8; // COLOR_SCROLLBAR
        case 2:  return 0x00808080; // COLOR_BACKGROUND
        case 4:  return 0x00F0F0F0; // COLOR_MENU
        case 5:  return 0x00FFFFFF; // COLOR_WINDOW
        case 6:  return 0x00A0A0A0; // COLOR_WINDOWFRAME
        case 8:  return 0x00000000; // COLOR_WINDOWTEXT
        case 9:  return 0x00FFFFFF; // COLOR_CAPTIONTEXT
        case 13: return 0x00D4E6FF; // COLOR_HIGHLIGHT
        case 15: return 0x00F0F0F0; // COLOR_BTNFACE / 3DFACE
        case 17: return 0x00808080; // COLOR_GRAYTEXT
        case 18: return 0x00000000; // COLOR_BTNTEXT
        case 26: return 0x00412EE6; // COLOR_HOTLIGHT
        default: return 0x00F0F0F0;
    }
}
static uint32_t WINAPI U_GetSysColor(int idx){ return sys_color(idx); }
static uint32_t WINAPI U_GetSysColorBrush(int idx){ return 0x1000u + (uint32_t)idx; } // pseudo brush handle

// ---- user32: timers ----
static uint32_t WINAPI U_SetTimer(uint32_t, uint32_t id, uint32_t, void*){ return id ? id : 1; }
static uint32_t WINAPI U_KillTimer(uint32_t, uint32_t){ return 1; }

// ---- user32: sync objects (events) ----
static uint32_t g_evt = 0x2000;
static uint32_t WINAPI U_CreateEventA(uint32_t, uint32_t, uint32_t, const char*){ return 0x2000u + (g_evt++); }
static uint32_t WINAPI U_SetEvent(uint32_t){ return 1; }
static uint32_t WINAPI U_ResetEvent(uint32_t){ return 1; }
static uint32_t WINAPI U_WaitForSingleObject(uint32_t, uint32_t){ return 0; } // WAIT_OBJECT_0

// ---- user32: dialog helpers ----
static uint32_t WINAPI U_GetDlgItem(uint32_t hwnd, int id){ return (hwnd ^ (uint32_t)id) | 0x80000000u; }
static uint32_t WINAPI U_GetDlgItemTextA(uint32_t, int, char* buf, int n){
    if (buf && n > 0) buf[0] = 0; return 0;
}
static uint32_t WINAPI U_IsDlgButtonChecked(uint32_t, int){ return 0; } // BST_UNCHECKED
static uint32_t WINAPI U_GetWindowTextLengthA(uint32_t){ return 0; }

// ---- user32: window state ----
static uint32_t WINAPI U_EnableWindow(uint32_t, uint32_t b){ return b ? 1 : 0; }
static uint32_t WINAPI U_SetForegroundWindow(uint32_t){ return 1; }
static uint32_t WINAPI U_MessageBoxExA(uint32_t h, const char* t, const char* c, uint32_t, uint16_t){
    return U_MessageBoxA(h, t, c, 0);
}

// ---- user32: wide class registration / unregister (alias to ANSI) ----
static uint32_t WINAPI U_RegisterClassW(const void* wc){ return U_RegisterClassA((const W32WndClassA*)wc); }
static uint32_t WINAPI U_UnregisterClassW(const char* n, uint32_t){ return U_UnregisterClassA(n, 0); }

// ---- user32: image / icon loading ----
static uint32_t WINAPI U_LoadImageA(uint32_t, const char*, uint32_t, int, int, uint32_t){ return 0x3000u; }
static uint32_t WINAPI U_CopyImage(uint32_t h, uint32_t, int, int, uint32_t){ return h; }

// ---- gdi32: device caps / object query ----
static int WINAPI G_GetDeviceCaps(uint32_t, int idx){
    if (idx == 88) return 96;   // LOGPIXELSX
    if (idx == 90) return 96;   // LOGPIXELSY
    if (idx == 8)  return 1024; // HORZRES
    if (idx == 10) return 768;  // VERTRES
    if (idx == 118) return 32;  // BITSPIXEL
    return 0;
}
static uint32_t WINAPI G_GetObjectA(uint32_t, int, void*){ return 1; }

// =====================================================================
//  9.  Export tables
// =====================================================================
struct W32Export { const char* name; void* fn; };
struct W32Dll    { const char* dll; const W32Export* ex; int n; };

#define EXP(n, f) { n, (void*)(f) }

static const W32Export EX_KERNEL32[] = {
    EXP("GetLastError", K_GetLastError), EXP("SetLastError", K_SetLastError),
    EXP("GetStdHandle", K_GetStdHandle), EXP("WriteConsoleA", K_WriteConsoleA),
    EXP("WriteConsoleW", K_WriteConsoleA), EXP("WriteFile", K_WriteFile),
    EXP("ExitProcess", K_ExitProcess), EXP("OutputDebugStringA", K_OutputDebugStringA),
    EXP("GetModuleHandleA", K_GetModuleHandleA), EXP("GetModuleHandleW", K_GetModuleHandleA),
    EXP("LoadLibraryA", K_LoadLibraryA), EXP("FreeLibrary", K_FreeLibrary),
    EXP("GetProcAddress", K_GetProcAddress), EXP("GetModuleFileNameA", K_GetModuleFileNameA),
    EXP("GetModuleHandleExA", K_GetModuleHandleExA),
    EXP("GetProcessHeap", K_GetProcessHeap), EXP("HeapAlloc", K_HeapAlloc),
    EXP("HeapFree", K_HeapFree), EXP("HeapReAlloc", K_HeapAlloc),
    EXP("VirtualAlloc", K_VirtualAlloc), EXP("VirtualFree", K_VirtualFree),
    EXP("LocalAlloc", K_LocalAlloc), EXP("LocalFree", K_HeapFree),
    EXP("GlobalAlloc", K_GlobalAlloc), EXP("GlobalFree", K_HeapFree),
    EXP("GetCommandLineA", K_GetCommandLineA), EXP("GetCommandLineW", K_GetCommandLineA),
    EXP("GetEnvironmentVariableA", K_GetEnvironmentVariableA),
    EXP("SetEnvironmentVariableA", K_SetEnvironmentVariableA),
    EXP("ExpandEnvironmentStringsA", K_ExpandEnvironmentStringsA),
    EXP("GetComputerNameA", K_GetComputerNameA), EXP("GetComputerNameExA", K_GetComputerNameExA),
    EXP("GetSystemDirectoryA", K_GetSystemDirectoryA), EXP("GetWindowsDirectoryA", K_GetWindowsDirectoryA),
    EXP("GetTempPathA", K_GetTempPathA), EXP("GetFullPathNameA", K_GetFullPathNameA),
    EXP("GetFileAttributesA", K_GetFileAttributesA), EXP("GetDriveTypeA", K_GetDriveTypeA),
    EXP("GetLogicalDrives", K_GetLogicalDrives),
    EXP("FindFirstFileA", K_FindFirstFileA), EXP("FindNextFileA", K_FindNextFileA), EXP("FindClose", K_FindClose),
    EXP("SetFilePointer", K_SetFilePointer),
    EXP("GetTickCount", K_GetTickCount), EXP("Sleep", K_Sleep),
    EXP("GetLocalTime", K_GetLocalTime), EXP("GetSystemTime", K_GetSystemTime),
    EXP("GetSystemInfo", K_GetSystemInfo), EXP("GetVersion", K_GetVersion),
    EXP("GetVersionExA", K_GetVersionExA), EXP("GetACP", K_GetACP), EXP("GetOEMCP", K_GetOEMCP),
    EXP("GetCurrentProcessId", K_GetCurrentProcessId), EXP("GetCurrentProcess", K_GetCurrentProcess),
    EXP("GetCurrentThreadId", K_GetCurrentThreadId),
    EXP("IsProcessorFeaturePresent", K_IsProcessorFeaturePresent),
    EXP("InitializeCriticalSection", K_InitializeCriticalSection),
    EXP("EnterCriticalSection", K_EnterCriticalSection),
    EXP("LeaveCriticalSection", K_LeaveCriticalSection),
    EXP("DeleteCriticalSection", K_DeleteCriticalSection),
    EXP("SetUnhandledExceptionFilter", K_SetUnhandledExceptionFilter),
    EXP("lstrlenA", K_lstrlenA), EXP("lstrcpyA", K_lstrcpyA), EXP("lstrcatA", K_lstrcatA),
    EXP("lstrcmpA", K_lstrcmpA), EXP("lstrcmpiA", K_lstrcmpiA),
    EXP("CreateFileA", K_CreateFileA), EXP("ReadFile", K_ReadFile),
    EXP("GetFileSize", K_GetFileSize), EXP("CloseHandle", K_CloseHandle),
    EXP("CreateThread", K_CreateThread), EXP("ExitThread", K_ExitThread),
    EXP("MultiByteToWideChar", K_MultiByteToWideChar),
    EXP("WideCharToMultiByte", K_WideCharToMultiByte),
};
static const W32Export EX_USER32[] = {
    EXP("RegisterClassA", U_RegisterClassA), EXP("RegisterClassExA", U_RegisterClassExA),
    EXP("UnregisterClassA", U_UnregisterClassA),
    EXP("CreateWindowExA", U_CreateWindowExA), EXP("DestroyWindow", U_DestroyWindow),
    EXP("ShowWindow", U_ShowWindow), EXP("UpdateWindow", U_UpdateWindow),
    EXP("InvalidateRect", U_InvalidateRect), EXP("GetClientRect", U_GetClientRect),
    EXP("GetWindowRect", U_GetWindowRect), EXP("SetWindowTextA", U_SetWindowTextA),
    EXP("IsWindow", U_IsWindow), EXP("IsWindowVisible", U_IsWindowVisible),
    EXP("GetDesktopWindow", U_GetDesktopWindow), EXP("GetForegroundWindow", U_GetForegroundWindow),
    EXP("GetWindowTextA", U_GetWindowTextA), EXP("GetClassNameA", U_GetClassNameA),
    EXP("SetFocus", U_SetFocus), EXP("GetFocus", U_GetFocus),
    EXP("GetCursorPos", U_GetCursorPos), EXP("SetCursorPos", U_SetCursorPos),
    EXP("ClientToScreen", U_ClientToScreen), EXP("ScreenToClient", U_ScreenToClient),
    EXP("BeginPaint", U_BeginPaint), EXP("EndPaint", U_EndPaint),
    EXP("GetDC", U_GetDC), EXP("ReleaseDC", U_ReleaseDC),
    EXP("FillRect", U_FillRect), EXP("DrawTextA", U_DrawTextA),
    EXP("MessageBoxA", U_MessageBoxA), EXP("MessageBoxW", U_MessageBoxA),
    EXP("MessageBeep", U_MessageBeep),
    EXP("LoadIconA", U_LoadIconA), EXP("LoadCursorA", U_LoadCursorA),
    EXP("GetMessageA", U_GetMessageA), EXP("PeekMessageA", U_PeekMessageA),
    EXP("TranslateMessage", U_TranslateMessage), EXP("DispatchMessageA", U_DispatchMessageA),
    EXP("PostQuitMessage", U_PostQuitMessage), EXP("DefWindowProcA", U_DefWindowProcA),
    EXP("PostMessageA", U_PostMessageA), EXP("SendMessageA", U_SendMessageA),
    EXP("GetWindowLongA", U_GetWindowLongA), EXP("GetWindowLongW", U_GetWindowLongW),
    EXP("SetWindowLongA", U_SetWindowLongA), EXP("SetWindowLongW", U_SetWindowLongW),
    EXP("GetSysColor", U_GetSysColor), EXP("GetSysColorBrush", U_GetSysColorBrush),
    EXP("SetTimer", U_SetTimer), EXP("KillTimer", U_KillTimer),
    EXP("CreateEventA", U_CreateEventA), EXP("SetEvent", U_SetEvent), EXP("ResetEvent", U_ResetEvent),
    EXP("WaitForSingleObject", U_WaitForSingleObject),
    EXP("GetDlgItem", U_GetDlgItem), EXP("GetDlgItemTextA", U_GetDlgItemTextA),
    EXP("IsDlgButtonChecked", U_IsDlgButtonChecked), EXP("GetWindowTextLengthA", U_GetWindowTextLengthA),
    EXP("EnableWindow", U_EnableWindow), EXP("SetForegroundWindow", U_SetForegroundWindow),
    EXP("MessageBoxExA", U_MessageBoxExA),
    EXP("RegisterClassW", U_RegisterClassW), EXP("UnregisterClassW", U_UnregisterClassW),
    EXP("LoadImageA", U_LoadImageA), EXP("CopyImage", U_CopyImage),
    EXP("GetMessageW", U_GetMessageA),
    EXP("GetSystemMetrics", U_GetSystemMetrics), EXP("wsprintfA", U_wsprintfA),
    // Popup menu editing / display (Win32 API for custom context menus).
    EXP("CreatePopupMenu", U_CreatePopupMenu), EXP("AppendMenuA", U_AppendMenuA),
    EXP("InsertMenuA", U_InsertMenuA), EXP("DeleteMenu", U_DeleteMenu),
    EXP("RemoveMenu", U_RemoveMenu), EXP("DestroyMenu", U_DestroyMenu),
    EXP("GetMenuItemCount", U_GetMenuItemCount), EXP("GetMenuStringA", U_GetMenuStringA),
    EXP("EnableMenuItem", U_EnableMenuItem), EXP("CheckMenuItem", U_CheckMenuItem),
    EXP("TrackPopupMenu", U_TrackPopupMenu),
};
static const W32Export EX_GDI32[] = {
    EXP("CreateSolidBrush", G_CreateSolidBrush), EXP("CreatePen", G_CreatePen),
    EXP("DeleteObject", G_DeleteObject), EXP("GetStockObject", G_GetStockObject),
    EXP("SelectObject", G_SelectObject), EXP("SetTextColor", G_SetTextColor),
    EXP("SetBkColor", G_SetBkColor), EXP("SetBkMode", G_SetBkMode),
    EXP("TextOutA", G_TextOutA), EXP("Rectangle", G_Rectangle), EXP("Ellipse", G_Ellipse),
    EXP("MoveToEx", G_MoveToEx), EXP("LineTo", G_LineTo), EXP("CreateFontA", G_CreateFontA),
    EXP("GetTextMetricsA", G_GetTextMetricsA), EXP("GetTextExtentPoint32A", G_GetTextExtentPoint32A),
    EXP("SetPixel", G_SetPixel),
    EXP("GetDeviceCaps", G_GetDeviceCaps), EXP("GetObjectA", G_GetObjectA),
};
static const W32Export EX_ADVAPI32[] = {
    EXP("RegOpenKeyExA", A_RegOpenKeyExA), EXP("RegOpenKeyA", A_RegOpenKeyA),
    EXP("RegCreateKeyExA", A_RegCreateKeyExA), EXP("RegCloseKey", A_RegCloseKey),
    EXP("RegQueryValueExA", A_RegQueryValueExA), EXP("RegSetValueExA", A_RegSetValueExA),
    EXP("RegDeleteValueA", A_RegDeleteValueA), EXP("RegDeleteKeyA", A_RegDeleteKeyA),
    EXP("RegEnumKeyExA", A_RegEnumKeyExA), EXP("RegEnumValueA", A_RegEnumValueA),
    EXP("RegQueryInfoKeyA", A_RegQueryInfoKeyA), EXP("GetUserNameA", A_GetUserNameA),
};
static const W32Export EX_MSVCRT[] = {
    EXP("puts", C_puts), EXP("putchar", C_putchar), EXP("printf", C_printf),
    EXP("malloc", C_malloc), EXP("free", C_free), EXP("exit", C_exit),
    EXP("strlen", C_strlen), EXP("strcpy", C_strcpy), EXP("strcmp", C_strcmp),
    EXP("memset", C_memset), EXP("memcpy", C_memcpy),
};

// ---------------------------------------------------------------------
//  NexOS.dll -- the non-Windows half of the subsystem
//
//  Everything above imitates Win32.  This one does not: it is NexOS's
//  own local AI engine (ai_engine.cpp) published to native PE programs,
//  which is how the browser's start page answers a question with no
//  network underneath it.  The three entry points forward to kernel.cpp
//  so that the engine has exactly one owner and one "is it up?" flag --
//  the shell's `ai init` and the browser's Ask button drive the same
//  instance.
// ---------------------------------------------------------------------
extern "C" int kern_ai_ready(void);
extern "C" int kern_ai_boot(void);
extern "C" int kern_ai_ask(const char* prompt, char* out, int outsz);

static int WINAPI M_MiniAiReady(void){ return kern_ai_ready(); }
static int WINAPI M_MiniAiInit(void){ return kern_ai_boot(); }
static int WINAPI M_MiniAiAsk(const char* prompt, char* out, int outsz){
    return kern_ai_ask(prompt, out, outsz);
}

// Clipboard bridge so native PE apps (e.g. the 32-bit IE address bar) can
// share the kernel's single g_clipboard with the managed shell and the
// text-mode terminal.  g_clipboard / clipboard_set live in kernel.cpp.
extern char  g_clipboard[256];
extern int   g_clipboard_len;
extern void  clipboard_set(const char* text, int len);
static int  WINAPI M_MiniClipGet(char* buf, int cap){
    if (!buf || cap <= 0) return 0;
    int n = g_clipboard_len; if (n >= cap) n = cap - 1;
    for (int i = 0; i < n; i++) buf[i] = g_clipboard[i];
    buf[n] = 0;
    return n;
}
static void WINAPI M_MiniClipSet(const char* s, int len){
    clipboard_set(s, len);
}

// Synchronous HTTP GET bridge so a native (PE) browser can actually fetch a
// page over the network, instead of only switching between locally-stored
// documents.  Calls the kernel's net_http_get() (net.cpp), which drives the
// whole httpc state machine synchronously to completion, and copies the
// response body (NUL-terminated) into `out`.  Returns the byte count, or -1
// on a bad/busy URL.  The PE guest runs at ring0 with real kernel memory, so
// pointing it at a guest-allocated buffer is safe -- exactly how the managed
// browser reaches the same client through gui_cb_http_get (kernel.cpp:6388).
extern "C" int net_http_get(const char* url, char* out, int outsize); // net.cpp
static int WINAPI M_MiniHttpGet(const char* url, char* out, int outsize){
    if (!url || !out || outsize <= 0) return -1;
    int n = net_http_get(url, out, outsize);
    if (outsize > 0 && n >= 0 && n < outsize) out[n] = 0;
    return n;
}

static const W32Export EX_NexOS[] = {
    EXP("MiniAiReady", M_MiniAiReady),
    EXP("MiniAiInit",  M_MiniAiInit),
    EXP("MiniAiAsk",   M_MiniAiAsk),
    EXP("MiniClipGet", M_MiniClipGet),
    EXP("MiniClipSet", M_MiniClipSet),
    EXP("MiniHttpGet", M_MiniHttpGet),
};

#define NEL(a) ((int)(sizeof(a)/sizeof((a)[0])))
static const W32Dll g_dlls[] = {
    { "kernel32", EX_KERNEL32, NEL(EX_KERNEL32) },
    { "user32",   EX_USER32,   NEL(EX_USER32)   },
    { "gdi32",    EX_GDI32,    NEL(EX_GDI32)    },
    { "advapi32", EX_ADVAPI32, NEL(EX_ADVAPI32) },
    { "msvcrt",   EX_MSVCRT,   NEL(EX_MSVCRT)   },
    { "NexOS",   EX_NexOS,   NEL(EX_NexOS)   },
};

// strip directory + ".dll" suffix, then compare
static bool dll_match(const char* full, const char* base){
    const char* s = full;
    for (const char* p = full; *p; p++) if (*p=='\\' || *p=='/') s = p+1;
    int i = 0;
    while (base[i] && s[i] && w_lo(base[i]) == w_lo(s[i])) i++;
    if (base[i]) return false;
    return (s[i] == 0) || !w_icmp(s + i, ".dll");
}
static void* w32_resolve(const char* dll, const char* fn){
    if (!fn) return 0;
    for (int d = 0; d < NEL(g_dlls); d++){
        if (dll && !dll_match(dll, g_dlls[d].dll)) continue;
        for (int i = 0; i < g_dlls[d].n; i++)
            if (!w_cmp(g_dlls[d].ex[i].name, fn)) return g_dlls[d].ex[i].fn;
    }
    // second pass: ignore the DLL name (apps sometimes import via forwarders)
    for (int d = 0; d < NEL(g_dlls); d++)
        for (int i = 0; i < g_dlls[d].n; i++)
            if (!w_cmp(g_dlls[d].ex[i].name, fn)) return g_dlls[d].ex[i].fn;
    return 0;
}

// =====================================================================
//  10.  PE32 loader
// =====================================================================
static inline uint16_t rd16(const uint8_t* p, uint32_t o){ return (uint16_t)(p[o] | (p[o+1]<<8)); }
static inline uint32_t rd32(const uint8_t* p, uint32_t o){
    return (uint32_t)p[o] | ((uint32_t)p[o+1]<<8) | ((uint32_t)p[o+2]<<16) | ((uint32_t)p[o+3]<<24);
}
static inline uint64_t rd64(const uint8_t* p, uint32_t o){
    return (uint64_t)p[o]       | ((uint64_t)p[o+1]<<8)  | ((uint64_t)p[o+2]<<16) |
           ((uint64_t)p[o+3]<<24) | ((uint64_t)p[o+4]<<32) | ((uint64_t)p[o+5]<<40) |
           ((uint64_t)p[o+6]<<48) | ((uint64_t)p[o+7]<<56);
}
static inline void wr64(uint8_t* p, uint32_t o, uint64_t v){
    for (int i=0;i<8;i++) p[o+i] = (uint8_t)(v >> (8*i));
}

// Forward declaration (defined just below, after win32_run's header parsing).
#if W64_EXEC
static int win64_run(const char* filename, const char* args, int info_only);
#endif

static char     g_report[2200];
static int      g_report_n = 0;
static uint8_t* g_image     = 0;     // kmalloc'd block
static uint8_t* g_base      = 0;     // page-aligned image base
static uint32_t g_entry_rva = 0;

static void rep_flush(void){
    if (g_report_n > 0){ w32_serial(g_report); g_report_n = 0; g_report[0] = 0; }
}
static void rep(const char* s){
    g_report_n = w_app(g_report, g_report_n, (int)sizeof(g_report), s);
    // Flush as soon as a complete line has been buffered so the loader's
    // diagnostics actually reach the serial port -- including the
    // early-rejection branches (bad machine / file not found / oversized)
    // that return right after reporting, where nothing used to flush and
    // the reason for the rejection was silently lost.  Multi-part lines
    // (e.g. "Machine: <hex> (i386)") stay buffered until their '\n'.
    if (g_report_n > 0 && g_report[g_report_n - 1] == '\n') rep_flush();
}
static void rep_num(uint32_t v){ char t[12]; w_num(t, v); rep(t); }
static void rep_hex64(uint64_t v){
    char t[17];
    for (int i=0;i<16;i++){ int d=(int)((v >> (4*(15-i))) & 0xF); t[i] = (char)(d<10 ? ('0'+d) : ('A'-10+d)); }
    t[16] = 0;
    rep("0x"); rep(t);
}
static void rep_hex(uint32_t v){ char t[12]; w_hex(t, v, 8); rep("0x"); rep(t); }

static void w32_free_image(){
    if (g_image){ kfree(g_image); g_image = 0; g_base = 0; }
}

const char* win32_last_report(void){ return g_report; }

// =====================================================================
//  10b. PE32+ (64-bit x86-64) loader
// ---------------------------------------------------------------------
//  Runs only inside the 64-bit long-mode kernel (W64_EXEC == 1).  Parses
//  the PE32+ optional header, maps the image at a 64-bit VA, applies
//  DIR64 base relocations (type 10), resolves imports into the 8-byte
//  IAT, and calls the entry point through an ms_abi function pointer.
// =====================================================================
#if W64_EXEC
static int win64_run(const char* filename, const char* args, int info_only){
    reg_seed(); env_seed();
    g_report_n = 0; g_report[0] = 0;
    g_con_len  = 0; g_con[0] = 0;
    g_quit = false; g_getmsg_n = 0; g_exitcode = 0;
    g_win_n = 0;
    for (int i=0;i<W32_MAX_WINDOWS;i++) g_win[i].used = false;
    for (int i=0;i<W32_MAX_CLASSES;i++) g_cls[i].used = false;
    for (int i=0;i<W32_MAX_DC;i++)      g_dc[i].used  = false;
    for (int i=0;i<W32_MAX_OBJ;i++)     g_obj[i].used = false;

    if (!filename || !g_reader) return -1;
    w_ncpy(g_modpath, filename, 64);
    g_cmdline[0] = 0;
    w_ncpy(g_cmdline, filename, 128);
    if (args && args[0]){ w_cat(g_cmdline, " "); w_ncpy(g_cmdline + w_len(g_cmdline), args,
                                                        128 - w_len(g_cmdline)); }

    // ---- read the file ----
    constexpr uint32_t MAXPE = 192u * 1024u;
    uint8_t* file = (uint8_t*)kmalloc(MAXPE);
    if (!file){ rep("out of memory reading file\n"); return -4; }
    int fsz = g_reader(filename, file, (int)MAXPE);
    if (fsz <= 0){ kfree(file); rep("file not found\n"); return -1; }
    // NOTE: a full buffer means the file is >= MAXPE and we only got a
    // prefix.  We still parse/report the headers (they live in the first
    // page, which we always have) and refuse just before mapping - see the
    // truncation check further down.
    const bool truncated = (fsz >= (int)MAXPE);

    if (fsz < 0x40 || file[0] != 'M' || file[1] != 'Z'){ kfree(file); rep("not an MZ image\n"); return -2; }
    uint32_t nt = rd32(file, 0x3C);
    if (nt + 0xF8 > (uint32_t)fsz || rd32(file, nt) != 0x00004550u){
        kfree(file); rep("no PE signature\n"); return -2;
    }
    uint16_t machine = rd16(file, nt + 4);
    uint16_t nsec    = rd16(file, nt + 6);
    uint16_t optsz   = rd16(file, nt + 20);
    uint32_t opt     = nt + 24;
    uint16_t magic   = rd16(file, opt);
    (void)machine;
    if (magic != 0x20B){ kfree(file); rep("not a PE32+ (x86-64) image\n"); return -3; }

    uint32_t entry_rva = rd32(file, opt + 16);
    uint64_t imgbase   = rd64(file, opt + 24);
    uint32_t sizeimg   = rd32(file, opt + 56);
    uint32_t sizehdr   = rd32(file, opt + 60);
    uint16_t subsys    = rd16(file, opt + 68);
    uint32_t ndir      = rd32(file, opt + 108);
    uint32_t dd        = opt + 112;
    uint32_t imp_rva   = (ndir >  1) ? rd32(file, dd +  1*8) : 0;
    uint32_t rel_rva   = (ndir >  5) ? rd32(file, dd +  5*8) : 0;
    uint32_t rel_sz    = (ndir >  5) ? rd32(file, dd +  5*8 + 4) : 0;
    uint32_t clr_rva   = (ndir > 14) ? rd32(file, dd + 14*8) : 0;

    rep("PE32+ image: "); rep(filename); rep("\n");
    rep("  Machine        : 0x8664 (x86-64)\n");
    rep("  ImageBase      : "); rep_hex64(imgbase); rep("\n");
    rep("  EntryPoint RVA : "); rep_hex(entry_rva); rep("\n");
    rep("  SizeOfImage    : "); rep_num(sizeimg); rep(" bytes\n");
    rep("  Subsystem      : "); rep_num(subsys);
    rep(subsys == 2 ? "  (WINDOWS_GUI)\n" : (subsys == 3 ? "  (WINDOWS_CUI)\n" : "\n"));

    if (clr_rva){ kfree(file); rep("  -> .NET/CLR images are not supported (no .NET runtime in NexOS)\n"); return -3; }
    if (sizeimg == 0 || sizeimg > 64u*1024u*1024u){ kfree(file); rep("  -> bad SizeOfImage\n"); return -3; }
    if (truncated){
        kfree(file);
        rep("  -> image exceeds the loader's 192 KiB limit; only a prefix was\n");
        rep("     read, so mapping it would zero-fill the tail sections\n");
        return -6;
    }

    // ---- map the image (64-bit VA; long mode identity-maps all RAM) ----
    w32_free_image();
    g_image = (uint8_t*)kmalloc(sizeimg + 8192);
    if (!g_image){ kfree(file); rep("  -> out of memory mapping image\n"); return -4; }
    g_base = (uint8_t*)(((uintptr_t)g_image + 4095u) & ~(uintptr_t)4095u);
    w_set(g_base, 0, sizeimg);
    if (sizehdr > (uint32_t)fsz) sizehdr = (uint32_t)fsz;
    w_mov(g_base, file, sizehdr);

    uint32_t sh = nt + 24 + optsz;
    for (uint16_t s = 0; s < nsec; s++){
        uint32_t e   = sh + s*40u;
        if (e + 40 > (uint32_t)fsz) break;
        uint32_t va  = rd32(file, e + 12);
        uint32_t rsz = rd32(file, e + 16);
        uint32_t rp  = rd32(file, e + 20);
        if (va >= sizeimg) continue;
        if (rp + rsz > (uint32_t)fsz) rsz = (rp < (uint32_t)fsz) ? (uint32_t)fsz - rp : 0;
        if (va + rsz > sizeimg) rsz = sizeimg - va;
        if (rsz) w_mov(g_base + va, file + rp, rsz);
    }

    // ---- base relocations (DIR64) ----
    int64_t delta = (int64_t)((uintptr_t)g_base - (uintptr_t)imgbase);
    rep("  Loaded at      : "); rep_hex64((uint64_t)(uintptr_t)g_base);
    rep("  delta="); rep_hex64((uint64_t)delta); rep("\n");
    if (delta != 0){
        // An x86-64 image with no .reloc is not automatically broken.  The
        // AMD64 code model addresses code, strings and globals RIP-relative,
        // so a small freestanding PE frequently needs *zero* fixups and the
        // linker then emits no .reloc at all.  What actually decides whether
        // the image may be moved is IMAGE_FILE_RELOCS_STRIPPED in the COFF
        // characteristics: when that bit is clear the linker is telling us
        // "this image is relocatable", empty fixup table included.  Refusing
        // on a missing section instead of on that bit rejects perfectly good
        // images -- every -nostdlib mingw build lands here.
        const uint16_t chars = rd16(g_base, nt + 22);
        const bool relocs_stripped = (chars & 0x0001u) != 0;
        if (!rel_rva || !rel_sz){
            if (relocs_stripped){
                kfree(file); w32_free_image();
                rep("  -> image is marked RELOCS_STRIPPED and cannot be rebased\n");
                return -3;
            }
            rep("  Relocations    : none required (RIP-relative image)\n");
        } else {
            uint32_t off = 0; uint64_t fixed = 0;
            while (off + 8 <= rel_sz){
                uint32_t page = rd32(g_base, rel_rva + off);
                uint32_t blk  = rd32(g_base, rel_rva + off + 4);
                if (blk < 8 || off + blk > rel_sz) break;
                uint32_t n = (blk - 8) / 2;
                for (uint32_t i = 0; i < n; i++){
                    uint16_t ev = rd16(g_base, rel_rva + off + 8 + i*2);
                    uint32_t ty = ev >> 12, o = ev & 0x0FFF;
                    if (ty == 10){ // IMAGE_REL_BASED_DIR64
                        uint64_t* t = (uint64_t*)(g_base + page + o);
                        *t = (uint64_t)((int64_t)(*t) + delta);
                        fixed++;
                    }
                }
                off += blk;
            }
            rep("  Relocations    : "); rep_num((uint32_t)fixed); rep(" DIR64 fixups applied\n");
        }
    }

    // ---- imports (8-byte IAT) ----
    int missing = 0, resolved = 0;
    if (imp_rva){
        rep("  Imports:\n");
        for (uint32_t d = 0; ; d += 20){
            uint32_t oft  = rd32(g_base, imp_rva + d);
            uint32_t nrva = rd32(g_base, imp_rva + d + 12);
            uint32_t ft   = rd32(g_base, imp_rva + d + 16);
            if (!oft && !nrva && !ft) break;
            if (nrva >= sizeimg) break;
            const char* dll = (const char*)(g_base + nrva);
            rep("    "); rep(dll); rep(" -> ");
            uint32_t rt = oft ? oft : ft;
            uint32_t cnt = 0;
            for (uint32_t k = 0; ; k += 8){
                if (rt + k + 8 > sizeimg) break;
                uint64_t th = rd64(g_base, rt + k);
                if (!th) break;
                void* fn = 0;
                const char* nm = "(ordinal)";
                if (!(th & 0x8000000000000000ULL)){
                    // Import by name: low 63 bits are the RVA of an
                    // IMAGE_IMPORT_BY_NAME (2-byte Hint + ASCII Name).
                    uint64_t nrva = th & 0x7FFFFFFFFFFFFFFFULL;
                    if (nrva + 2 < sizeimg){
                        nm = (const char*)(g_base + nrva + 2);
                        fn = w32_resolve(dll, nm);
                    }
                }
                uint64_t* wp = (uint64_t*)(g_base + ft + k);
                if (fn){ resolved++; *wp = (uint64_t)(uintptr_t)fn; }
                else    { missing++; if (missing <= 8){ rep("\n      MISSING "); rep(nm); } *wp = 0; }
                cnt++;
            }
            rep_num(cnt); rep(" fn\n");
        }
    }
    rep("  Resolved       : "); rep_num((uint32_t)resolved);
    rep(" / missing "); rep_num((uint32_t)missing); rep("\n");
    kfree(file);

    if (missing){
        rep("  -> refusing to execute: unresolved imports would crash the kernel\n");
        w32_free_image();
        return -5;
    }
    g_entry_rva = entry_rva;
    if (info_only){ rep("  (info mode: image loaded but not executed)\n"); return 0; }

    // ---- private heap for the app ----
    if (!g_apool){
        g_apool_sz = 128u * 1024u;
        g_apool = (uint8_t*)kmalloc(g_apool_sz);
        if (!g_apool) g_apool_sz = 0;
    }
    g_apool_use = 0;

    rep("  Executing 64-bit entry point...\n");
    w32_serial("[WIN64] calling PE32+ entry at ");
    w32_serial_hex64((uint64_t)(uintptr_t)(g_base + entry_rva));
    w32_serial("\n");

    typedef int (WINAPI *Entry64)(void);   // WINAPI == ms_abi on x86-64
    Entry64 fn = (Entry64)(void*)(g_base + entry_rva);
    int rc = fn();
    w32_serial("[WIN64] PE32+ entry returned rc=");
    w32_serial_hex64((uint64_t)(uint32_t)rc);
    w32_serial("\n");
    rep("  Exit code      : "); rep_num((uint32_t)(g_quit ? g_exitcode : rc)); rep("\n");

    if (g_con_len){
        rep("--- console output ---\n");
        rep(g_con);
        if (g_con[g_con_len-1] != '\n') rep("\n");
    }
    int wins = 0;
    for (int i = 0; i < W32_MAX_WINDOWS; i++) if (g_win[i].used) wins++;
    rep("  Windows created: "); rep_num((uint32_t)wins); rep("\n");
    return 0;
}
#endif

extern "C" int win32_run(const char* filename, const char* args, int info_only){
    reg_seed(); env_seed();
    g_report_n = 0; g_report[0] = 0;
    g_con_len  = 0; g_con[0] = 0;
    g_quit = false; g_getmsg_n = 0; g_exitcode = 0;
    g_win_n = 0;
    for (int i=0;i<W32_MAX_WINDOWS;i++) g_win[i].used = false;
    for (int i=0;i<W32_MAX_CLASSES;i++) g_cls[i].used = false;
    for (int i=0;i<W32_MAX_DC;i++)      g_dc[i].used  = false;
    for (int i=0;i<W32_MAX_OBJ;i++)     g_obj[i].used = false;

    if (!filename || !g_reader) return -1;
    w_ncpy(g_modpath, filename, 64);
    g_cmdline[0] = 0;
    w_ncpy(g_cmdline, filename, 128);
    if (args && args[0]){ w_cat(g_cmdline, " "); w_ncpy(g_cmdline + w_len(g_cmdline), args,
                                                        128 - w_len(g_cmdline)); }

    // ---- read the file ----
    constexpr uint32_t MAXPE = 192u * 1024u;
    uint8_t* file = (uint8_t*)kmalloc(MAXPE);
    if (!file){ rep("out of memory reading file\n"); return -4; }
    int fsz = g_reader(filename, file, (int)MAXPE);
    if (fsz <= 0){ kfree(file); rep("file not found\n"); return -1; }
    const bool truncated = (fsz >= (int)MAXPE);   // see win64_run

    // ---- validate ----
    if (fsz < 0x40 || file[0] != 'M' || file[1] != 'Z'){ kfree(file); rep("not an MZ image\n"); return -2; }
    uint32_t nt = rd32(file, 0x3C);
    if (nt + 0xF8 > (uint32_t)fsz || rd32(file, nt) != 0x00004550u){
        kfree(file); rep("no PE signature\n"); return -2;
    }
    uint16_t machine = rd16(file, nt + 4);
    uint16_t nsec    = rd16(file, nt + 6);
    uint16_t optsz   = rd16(file, nt + 20);
    uint32_t opt     = nt + 24;
    uint16_t magic   = rd16(file, opt);

    rep("PE32 image: "); rep(filename); rep("\n");
    rep("  Machine        : "); rep_hex(machine);
    rep(machine == 0x014C ? "  (i386)\n" : (machine == 0x8664 ? "  (x86-64)\n" : "  (UNSUPPORTED)\n"));
    rep("  Sections       : "); rep_num(nsec); rep("\n");

    // ---- 64-bit PE32+ (x86-64) ----
    if (machine == 0x8664 && magic == 0x20B){
        kfree(file);
#if W64_EXEC
        rep("  -> routing to 64-bit PE32+ loader\n");
        return win64_run(filename, args, info_only);
#else
        rep("  -> 64-bit PE32+ requires the 64-bit long-mode kernel\n");
        rep("     (boot the UEFI image, or run 'switch64' from the 32-bit kernel)\n");
        return -3;
#endif
    }

    if (machine != 0x014C || magic != 0x010B){
        kfree(file); rep("  -> only 32-bit PE32 i386 images can execute\n"); return -3;
    }
    uint32_t entry_rva = rd32(file, opt + 16);
    uint32_t imgbase   = rd32(file, opt + 28);
    uint32_t sizeimg   = rd32(file, opt + 56);
    uint32_t sizehdr   = rd32(file, opt + 60);
    uint16_t subsys    = rd16(file, opt + 68);
    uint32_t ndir      = rd32(file, opt + 92);
    uint32_t dd        = opt + 96;
    uint32_t imp_rva   = (ndir >  1) ? rd32(file, dd +  1*8) : 0;
    uint32_t rel_rva   = (ndir >  5) ? rd32(file, dd +  5*8) : 0;
    uint32_t rel_sz    = (ndir >  5) ? rd32(file, dd +  5*8 + 4) : 0;
    uint32_t clr_rva   = (ndir > 14) ? rd32(file, dd + 14*8) : 0;

    rep("  ImageBase      : "); rep_hex(imgbase); rep("\n");
    rep("  EntryPoint RVA : "); rep_hex(entry_rva); rep("\n");
    rep("  SizeOfImage    : "); rep_num(sizeimg); rep(" bytes\n");
    rep("  Subsystem      : "); rep_num(subsys);
    rep(subsys == 2 ? "  (WINDOWS_GUI)\n" : (subsys == 3 ? "  (WINDOWS_CUI)\n" : "\n"));

    if (clr_rva){ kfree(file); rep("  -> .NET/CLR images are not supported\n"); return -3; }
    if (sizeimg == 0 || sizeimg > 8u*1024u*1024u){ kfree(file); rep("  -> bad SizeOfImage\n"); return -3; }
    if (truncated){
        kfree(file);
        rep("  -> image exceeds the loader's 192 KiB limit; only a prefix was\n");
        rep("     read, so mapping it would zero-fill the tail sections\n");
        return -6;
    }

    // ---- map the image ----
    w32_free_image();
    g_image = (uint8_t*)kmalloc(sizeimg + 8192);
    if (!g_image){ kfree(file); rep("  -> out of memory mapping image\n"); return -4; }
    g_base = (uint8_t*)((((uintptr_t)g_image) + 4095u) & ~(uintptr_t)4095u);
    w_set(g_base, 0, sizeimg);
    if (sizehdr > (uint32_t)fsz) sizehdr = (uint32_t)fsz;
    w_mov(g_base, file, sizehdr);

    uint32_t sh = nt + 24 + optsz;
    for (uint16_t s = 0; s < nsec; s++){
        uint32_t e   = sh + s*40u;
        if (e + 40 > (uint32_t)fsz) break;
        uint32_t va  = rd32(file, e + 12);
        uint32_t rsz = rd32(file, e + 16);
        uint32_t rp  = rd32(file, e + 20);
        if (va >= sizeimg) continue;
        if (rp + rsz > (uint32_t)fsz) rsz = (rp < (uint32_t)fsz) ? (uint32_t)fsz - rp : 0;
        if (va + rsz > sizeimg) rsz = sizeimg - va;
        if (rsz) w_mov(g_base + va, file + rp, rsz);
    }

    // ---- base relocations ----
    int32_t delta = (int32_t)((uint32_t)(uintptr_t)g_base - imgbase);
    rep("  Loaded at      : "); rep_hex((uint32_t)(uintptr_t)g_base);
    rep("  delta="); rep_hex((uint32_t)delta); rep("\n");
    if (delta != 0){
        if (!rel_rva || !rel_sz){
            kfree(file); w32_free_image();
            rep("  -> image has no .reloc and cannot be rebased\n");
            return -3;
        }
        uint32_t off = 0, fixed = 0;
        while (off + 8 <= rel_sz){
            uint32_t page = rd32(g_base, rel_rva + off);
            uint32_t blk  = rd32(g_base, rel_rva + off + 4);
            if (blk < 8 || off + blk > rel_sz) break;
            uint32_t n = (blk - 8) / 2;
            for (uint32_t i = 0; i < n; i++){
                uint16_t e = rd16(g_base, rel_rva + off + 8 + i*2);
                uint32_t ty = e >> 12, o = e & 0x0FFF;
                if (ty == 3){
                    uint32_t* t = (uint32_t*)(g_base + page + o);
                    *t = (uint32_t)((int32_t)(*t) + delta);
                    fixed++;
                } // type 0 = absolute (padding)
            }
            off += blk;
        }
        rep("  Relocations    : "); rep_num(fixed); rep(" fixups applied\n");
    }

    // ---- imports ----
    int missing = 0, resolved = 0;
    if (imp_rva){
        rep("  Imports:\n");
        w32_serial("[WIN32] imp_rva="); w32_serial_hex(imp_rva); w32_serial("\n");
        for (uint32_t d = 0; ; d += 20){
            uint32_t oft  = rd32(g_base, imp_rva + d);
            uint32_t nrva = rd32(g_base, imp_rva + d + 12);
            uint32_t ft   = rd32(g_base, imp_rva + d + 16);
            w32_serial("[WIN32] desc d="); w32_serial_hex(d);
            w32_serial(" oft="); w32_serial_hex(oft);
            w32_serial(" nrva="); w32_serial_hex(nrva);
            w32_serial(" ft="); w32_serial_hex(ft);
            w32_serial("\n");
            if (!oft && !nrva && !ft) break;
            if (nrva >= sizeimg) break;
            const char* dll = (const char*)(g_base + nrva);
            rep("    "); rep(dll); rep(" -> ");
            uint32_t rt = oft ? oft : ft;
            int cnt = 0;
            for (uint32_t k = 0; ; k += 4){
                if (rt + k + 4 > sizeimg) break;
                uint32_t th = rd32(g_base, rt + k);
                if (!th) break;
                void* fn = 0;
                const char* nm = "(ordinal)";
                if (!(th & 0x80000000u) && th + 2 < sizeimg){
                    nm = (const char*)(g_base + th + 2);
                    fn = w32_resolve(dll, nm);
                }
                if (fn){ resolved++; }
                else {
                    missing++;
                    if (missing <= 6){ rep("\n      MISSING "); rep(nm); }
                }
                uint32_t* wp = (uint32_t*)(g_base + ft + k);
                if (cnt < 2){
                    w32_serial("[WIN32]   write fn="); w32_serial_hex((uint32_t)(uintptr_t)fn);
                    w32_serial(" to "); w32_serial_hex((uint32_t)(uintptr_t)wp);
                    w32_serial(" (was "); w32_serial_hex(*wp); w32_serial(")\n");
                }
                *wp = (uint32_t)(uintptr_t)fn;
                cnt++;
            }
            rep_num((uint32_t)cnt); rep(" fn\n");
        }
    }
    rep("  Resolved       : "); rep_num((uint32_t)resolved);
    rep(" / missing "); rep_num((uint32_t)missing); rep("\n");
    kfree(file);

    if (missing){
        rep("  -> refusing to execute: unresolved imports would crash the kernel\n");
        w32_free_image();
        return -5;
    }
    g_entry_rva = entry_rva;
    if (info_only){ rep("  (info mode: image loaded but not executed)\n"); return 0; }

#if W32_EXEC
    // ---- private heap for the app ----
    if (!g_apool){
        g_apool_sz = 128u * 1024u;
        g_apool = (uint8_t*)kmalloc(g_apool_sz);
        if (!g_apool) g_apool_sz = 0;
    }
    g_apool_use = 0;

    rep("  Executing entry point...\n");
    w32_serial("[WIN32] calling PE entry\n");

    uint32_t gb = (uint32_t)(uintptr_t)g_base;
    uint32_t fn_addr = gb + entry_rva;
    w32_serial("[WIN32] base="); w32_serial_hex(gb);
    w32_serial(" entry="); w32_serial_hex(fn_addr);
    w32_serial(" delta="); w32_serial_hex((uint32_t)delta);
    w32_serial("\n");

    typedef int (WINAPI *EntryFn)(void);
    EntryFn fn = (EntryFn)(void*)(g_base + entry_rva);

    // sanity-check the first few IAT slots before trusting the image
    {
        uint32_t* iat0 = (uint32_t*)(gb + 0x2100);
        w32_serial("[WIN32] IAT[0..3] ");
        for (int i = 0; i < 4; i++){ w32_serial_hex(iat0[i]); w32_serial(" "); }
        w32_serial("\n");
    }

    int rc = fn();
    w32_serial("[WIN32] PE entry returned\n");
    rep("  Exit code      : "); rep_num((uint32_t)(g_quit ? g_exitcode : rc)); rep("\n");
#else
    rep("  (64-bit kernel: PE32 execution disabled)\n");
#endif

    if (g_con_len){
        rep("--- console output ---\n");
        rep(g_con);
        if (g_con[g_con_len-1] != '\n') rep("\n");
    }
    int wins = 0;
    for (int i = 0; i < W32_MAX_WINDOWS; i++) if (g_win[i].used) wins++;
    rep("  Windows created: "); rep_num((uint32_t)wins); rep("\n");
    return 0;
}

// =====================================================================
//  11.  Public API
// =====================================================================
extern "C" void win32_init(int (*reader)(const char*, uint8_t*, int),
                           void (*writer)(const char*)){
    g_reader = reader;
    g_writer = writer;
    reg_seed();
    env_seed();
    (void)w32_out;
}

extern "C" int win32_reg_query(const char* path, const char* value, char* out, int outsz){
    reg_seed();
    int k = reg_path(path, false);
    if (k < 0) return -1;
    int n = 0;
    if (value && value[0]){
        RegValue* v = reg_find_val(k, value);
        if (!v) return -2;
        char tmp[96]; reg_val_text(v, tmp, (int)sizeof(tmp));
        n = w_app(out, n, outsz, "    ");
        n = w_app(out, n, outsz, v->name[0] ? v->name : "(Default)");
        n = w_app(out, n, outsz, "    ");
        n = w_app(out, n, outsz, reg_type_name(v->type));
        n = w_app(out, n, outsz, "    ");
        n = w_app(out, n, outsz, tmp);
        w_app(out, n, outsz, "\n");
        return 1;
    }
    int count = 0;
    for (int v = g_keys[k].values; v >= 0; v = g_vals[v].next){
        char tmp[96]; reg_val_text(&g_vals[v], tmp, (int)sizeof(tmp));
        n = w_app(out, n, outsz, "    ");
        n = w_app(out, n, outsz, g_vals[v].name[0] ? g_vals[v].name : "(Default)");
        n = w_app(out, n, outsz, "    ");
        n = w_app(out, n, outsz, reg_type_name(g_vals[v].type));
        n = w_app(out, n, outsz, "    ");
        n = w_app(out, n, outsz, tmp);
        n = w_app(out, n, outsz, "\n");
        count++;
    }
    return count;
}

extern "C" int win32_reg_list(const char* path, char* out, int outsz){
    reg_seed();
    int k = reg_path(path, false);
    if (k < 0) return -1;
    int n = 0, c = 0;
    for (int s = g_keys[k].child; s >= 0; s = g_keys[s].sibling){
        n = w_app(out, n, outsz, "  [+] ");
        n = w_app(out, n, outsz, g_keys[s].name);
        n = w_app(out, n, outsz, "\n");
        c++;
    }
    return c;
}

static int reg_tree_rec(int k, int depth, int maxd, char* out, int n, int cap){
    for (int s = g_keys[k].child; s >= 0; s = g_keys[s].sibling){
        for (int i = 0; i < depth; i++) n = w_app(out, n, cap, "  ");
        n = w_app(out, n, cap, "+ ");
        n = w_app(out, n, cap, g_keys[s].name);
        n = w_app(out, n, cap, "\n");
        if (depth + 1 < maxd) n = reg_tree_rec(s, depth + 1, maxd, out, n, cap);
        if (n >= cap - 2) break;
    }
    return n;
}
extern "C" int win32_reg_tree(const char* path, char* out, int outsz, int max_depth){
    reg_seed();
    int k = reg_path(path, false);
    if (k < 0) return -1;
    int n = w_app(out, 0, outsz, g_keys[k].name);
    n = w_app(out, n, outsz, "\n");
    reg_tree_rec(k, 1, max_depth, out, n, outsz);
    return 0;
}

extern "C" int win32_reg_set(const char* path, const char* value, const char* type,
                             const char* data){
    reg_seed();
    int k = reg_path(path, true);
    if (k < 0) return -1;
    uint32_t t = reg_type_parse(type);
    if (t == RT_DWORD){
        uint32_t v = 0;
        const char* p = data ? data : "0";
        if (p[0]=='0' && (p[1]=='x'||p[1]=='X')){
            for (p += 2; *p; p++){
                char c = w_lo(*p);
                if (c>='0'&&c<='9') v = v*16 + (uint32_t)(c-'0');
                else if (c>='a'&&c<='f') v = v*16 + (uint32_t)(c-'a'+10);
                else break;
            }
        } else while (*p >= '0' && *p <= '9') v = v*10 + (uint32_t)(*p++ - '0');
        return reg_put_val(k, value, RT_DWORD, &v, 4);
    }
    return reg_put_val(k, value, t, data ? data : "", (uint32_t)w_len(data ? data : "") + 1);
}

extern "C" int win32_reg_delete(const char* path, const char* value){
    reg_seed();
    int k = reg_path(path, false);
    if (k < 0) return -1;
    if (value && value[0]) return (int)A_RegDeleteValueA(HKEY_OPEN_BASE + (uint32_t)k, value) == 0 ? 0 : -2;
    int p = g_keys[k].parent;
    if (p < 0) return -3;
    if (g_keys[p].child == k) g_keys[p].child = g_keys[k].sibling;
    else for (int s = g_keys[p].child; s >= 0; s = g_keys[s].sibling)
        if (g_keys[s].sibling == k){ g_keys[s].sibling = g_keys[k].sibling; break; }
    return 0;
}

extern "C" int win32_reg_key_count(void){ reg_seed(); return g_key_n; }
extern "C" int win32_reg_value_count(void){ reg_seed(); return g_val_n; }

extern "C" const char* win32_env_get(const char* n){ env_seed(); return env_get(n); }
extern "C" int win32_env_list(char* out, int outsz){
    env_seed();
    int n = 0;
    for (int i = 0; i < g_env_n; i++){
        n = w_app(out, n, outsz, "  ");
        n = w_app(out, n, outsz, g_env[i].name);
        n = w_app(out, n, outsz, "=");
        n = w_app(out, n, outsz, g_env[i].value);
        n = w_app(out, n, outsz, "\n");
    }
    return g_env_n;
}

// ---- GUI bridge ------------------------------------------------------
extern "C" int win32_window_count(void){ return g_win_n; }

// Returns 1 when the slot holds a live window, 0 otherwise, so callers can
// simply write:  if (!win32_window_info(i, &wi)) continue;
extern "C" int win32_window_info(int idx, W32WinInfo* o){
    if (idx < 0 || idx >= W32_MAX_WINDOWS || !g_win[idx].used || !o) return 0;
    W32Win& v = g_win[idx];
    o->hwnd = v.hwnd; o->x = v.x; o->y = v.y; o->w = v.w; o->h = v.h;
    w_ncpy(o->title, v.title, 48);
    w_ncpy(o->cls,   v.cls,   32);
    o->is_msgbox = v.is_msgbox ? 1 : 0;
    o->visible   = v.visible   ? 1 : 0;
    return 1;
}
extern "C" int win32_window_cmds(int idx, const W32DrawCmd** out){
    if (idx < 0 || idx >= W32_MAX_WINDOWS || !g_win[idx].used) return 0;
    if (out) *out = g_win[idx].cmds;
    return g_win[idx].cmd_n;
}
extern "C" void win32_window_repaint(int idx){
#if W32_EXEC
    if (idx < 0 || idx >= W32_MAX_WINDOWS || !g_win[idx].used) return;
    if (g_win[idx].proc && !g_win[idx].painting)
        g_win[idx].proc(g_win[idx].hwnd, WM_PAINT, 0, 0);
#else
    (void)idx;
#endif
}
extern "C" int win32_window_dispatch(int idx, uint32_t msg, uint32_t wp, uint32_t lp){
#if W32_EXEC
    if (idx < 0 || idx >= W32_MAX_WINDOWS || !g_win[idx].used) return 0;
    if (!g_win[idx].proc) return 0;
    return g_win[idx].proc(g_win[idx].hwnd, msg, wp, lp);
#else
    (void)idx; (void)msg; (void)wp; (void)lp; return 0;
#endif
}
extern "C" void win32_window_close(int idx){
    if (idx < 0 || idx >= W32_MAX_WINDOWS) return;
    g_win[idx].used = false;
}

// Hit-test a click in client-local coordinates against this window's BUTTON
// controls.  Returns the control id (the menu/resource id passed to
// CreateWindowExA), or 0 if no button was hit.  Coordinates match the
// (x,y) the child control was created with, i.e. relative to the parent's
// client area.
extern "C" int win32_window_button_hit(int idx, int lx, int ly){
#if W32_EXEC
    if (idx < 0 || idx >= W32_MAX_WINDOWS || !g_win[idx].used) return 0;
    for (int c = 0; c < g_win[idx].ctl_n; c++){
        const W32DrawCmd& d = g_win[idx].ctl[c];
        if (d.kind != W32_CMD_BUTTON) continue;
        int w = d.w > 0 ? d.w : 76;
        int h = d.h > 0 ? d.h : 26;
        if (lx >= d.x && lx < d.x + w && ly >= d.y && ly < d.y + h)
            return (int)d.id;
    }
    return 0;
#else
    (void)idx; (void)lx; (void)ly; return 0;
#endif
}

// ---- popup menu bridge (consumed by gui.cpp) --------------------------
extern "C" int win32_menu_active(int* x, int* y, uint32_t* hwnd){
    if (g_tp_idx < 0 || g_tp_idx >= W32_MAX_MENUS || !g_menu[g_tp_idx].used){
        g_tp_idx = -1;
        return 0;
    }
    if (x) *x = g_tp_x;
    if (y) *y = g_tp_y;
    if (hwnd) *hwnd = g_tp_hwnd;
    return 1;
}
extern "C" int win32_menu_item_count(void){
    if (g_tp_idx < 0 || g_tp_idx >= W32_MAX_MENUS) return 0;
    return g_menu[g_tp_idx].n;
}
extern "C" const char* win32_menu_item_text(int i){
    if (g_tp_idx < 0 || g_tp_idx >= W32_MAX_MENUS) return "";
    W32Menu& m = g_menu[g_tp_idx];
    if (i < 0 || i >= m.n) return "";
    return m.items[i].text;
}
extern "C" int win32_menu_item_flags(int i){
    if (g_tp_idx < 0 || g_tp_idx >= W32_MAX_MENUS) return 0;
    W32Menu& m = g_menu[g_tp_idx];
    if (i < 0 || i >= m.n) return 0;
    return (int)m.items[i].flags;
}
extern "C" void win32_menu_choose(int i){
    if (g_tp_idx < 0 || g_tp_idx >= W32_MAX_MENUS) return;
    int  mi = g_tp_idx;
    uint32_t hw = g_tp_hwnd;
    uint32_t id = 0;
    if (i >= 0 && i < g_menu[mi].n) id = g_menu[mi].items[i].id;
    g_tp_idx = -1;
    if (hw && id){
        int wi = win_from_handle(hw);
        if (wi >= 0 && g_win[wi].proc)
            g_win[wi].proc(hw, WM_COMMAND, id, 0);
    }
}
extern "C" void win32_menu_dismiss(void){ g_tp_idx = -1; }
extern "C" void win32_reset(void){
    for (int i=0;i<W32_MAX_WINDOWS;i++) g_win[i].used = false;
    g_win_n = 0;
    w32_free_image();
}
