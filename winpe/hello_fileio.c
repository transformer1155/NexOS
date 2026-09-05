/* hello_fileio.c -- NexOS Win32 subsystem stage-1 demo
 *
 * A REAL 32-bit Windows program (PE32, i386) cross-compiled with
 * i686-w64-mingw32-gcc.  We declare the kernel32 APIs by hand (no
 * <windows.h>, no dllimport) so the compiler emits plain `call _Xxx@n`
 * references that resolve against libkernel32.a's import stubs; the NexOS
 * loader then patches the IAT to point at its own emulation.
 *
 * Exercises:
 *   - reads an existing file from the SFS read-only FS        (file READ)
 *   - writes a new file persisted to the MKFS data FS          (file WRITE)
 *   - spawns a worker thread via CreateThread                  (stage-1)
 *   - emits [HELLO_PE] markers through OutputDebugStringA      (-> serial)
 *
 * Build:
 *   i686-w64-mingw32-gcc -m32 -nostdlib -O2 \
 *       -Wl,--entry=_WinMainCRTStartup -lkernel32 \
 *       -o hello_fileio.exe hello_fileio.c
 */
typedef unsigned long DWORD;
typedef void*        HANDLE;
typedef void*        PVOID;

#define WINAPI __attribute__((stdcall))
#define GENERIC_READ    0x80000000UL
#define GENERIC_WRITE   0x40000000UL
#define OPEN_EXISTING   3
#define CREATE_ALWAYS   2
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define STD_OUTPUT_HANDLE ((DWORD)-11)

typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(void*);

extern WINAPI void   OutputDebugStringA(const char*);
extern WINAPI HANDLE CreateFileA(const char*, DWORD, DWORD, PVOID, DWORD, DWORD, DWORD);
extern WINAPI int    ReadFile(HANDLE, void*, DWORD, DWORD*, PVOID);
extern WINAPI int    WriteFile(HANDLE, const void*, DWORD, DWORD*, PVOID);
extern WINAPI int    CloseHandle(HANDLE);
extern WINAPI HANDLE GetStdHandle(DWORD);
extern WINAPI int    lstrlenA(const char*);
extern WINAPI HANDLE CreateThread(PVOID, DWORD, LPTHREAD_START_ROUTINE, PVOID, DWORD, DWORD*);
extern WINAPI void   ExitProcess(DWORD);

static void emit(const char* s){ OutputDebugStringA(s); }

/* tiny int formatter so we can put a byte count in a marker */
static void emit_num(const char* prefix, int v){
    char tmp[48];
    int i = 0;
    while (prefix[i]) tmp[i++] = prefix[i];
    if (v == 0) tmp[i++] = '0';
    else {
        char rev[16]; int r = 0, n = v;
        while (n > 0){ rev[r++] = (char)('0' + (n % 10)); n /= 10; }
        while (r > 0) tmp[i++] = rev[--r];
    }
    tmp[i++] = '\n';
    tmp[i] = 0;
    emit(tmp);
}

static DWORD WINAPI Worker(PVOID p){
    (void)p;
    OutputDebugStringA("[HELLO_PE] thread worker ran\n");
    return 0;
}

void WinMainCRTStartup(void){
    char buf[512];
    DWORD got, wr, tid;

    emit("[HELLO_PE] start\n");

    /* ---- 1. READ an existing SFS file ---- */
    {
        HANDLE h = CreateFileA("welcome.txt", GENERIC_READ, 0, 0,
                               OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE){
            emit("[HELLO_PE] read FAILED to open welcome.txt\n");
        } else {
            int total = 0;
            while (ReadFile(h, buf, sizeof(buf) - 1, &got, 0) && got > 0){
                buf[got] = 0;
                WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, got, &wr, 0);
                total += (int)got;
            }
            CloseHandle(h);
            emit_num("[HELLO_PE] read welcome.txt OK bytes=", total);
        }
    }

    /* ---- 2. WRITE a new file (persisted to MKFS via kern_fs_create) ---- */
    {
        HANDLE w = CreateFileA("hello.txt", GENERIC_WRITE, 0, 0,
                               CREATE_ALWAYS, 0, 0);
        if (w == INVALID_HANDLE_VALUE){
            emit("[HELLO_PE] write FAILED to create hello.txt\n");
        } else {
            const char* msg = "Hello from a real Windows PE running on NexOS!\r\n";
            DWORD n = (DWORD)lstrlenA(msg);
            WriteFile(w, msg, n, &wr, 0);
            int cc = CloseHandle(w);   // 1 = kern_fs_create() persisted OK
            emit_num("[HELLO_PE] wrote hello.txt close=", cc);
        }
    }

    /* ---- 3. THREAD ---- */
    {
        HANDLE th = CreateThread(0, 0, Worker, 0, 0, &tid);
        if (th == 0) emit("[HELLO_PE] CreateThread FAILED\n");
        else         emit("[HELLO_PE] CreateThread OK\n");
    }

    emit("[HELLO_PE] DONE\n");
    ExitProcess(0);
}
