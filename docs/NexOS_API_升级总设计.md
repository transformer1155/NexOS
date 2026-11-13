# NexOS.Forms → NexOS.OS API 升级总设计（L0–L14 全量契约）

> 本文档为最终交付契约：逐层给出托管签名、内核 C 签名、语义、所属 Cap、数据结构、ring-0 入口、manifest 示例、自测行。
> MiniCLR 硬约束贯穿全部托管代码：无 .cctor 依赖（除非 L13 显式启用/lint）、数组仅 `int[]` 与 `reference[]`、无浮点/泛型/接口/try-catch/Format/BCL、Paint() 内禁止分配持久对象、多返回值拆标量 icall、InternalCall 按名+签名绑定并版本化、字符串重建用 `StrSub` 单次分配。

## 全局约定

- 错误码：所有失败 icall **必须** `set_last_error(Err.X)` 后返回失败值；返回值只承载"成功/数据"，错误一律走 `Host.LastError`。**禁止用 -1 泛指错误**（句柄 0 为无效句柄，其余 -1 仅作为个别 API 文档明确的哨兵，均同时 set_last_error）。
- 句柄：类型 `h_t`（32 位打包：kind:8 | slot:16 | gen:8）。托管侧为 `long`（int 可表达，但留 long 以便 ABI 演进）。**禁止裸 int 句柄**。
- Cap 常量（`Host.HasCap` 探测）：CAP_ERR=0, CAP_HANDLE=1, CAP_PERM=2, CAP_PROC=3, CAP_THREAD=4, CAP_SYNC=5, CAP_JOB=6, CAP_FS=7, CFG_MEM=8, CAP_IPC=9, CAP_NET=10, CAP_DEV=11, CAP_CLOCK=12, CAP_GFX=13, CAP_PKG=14, CAP_LOG=15, CAP_CFG=16, CAP_CLR=17, CAP_DBG=18。数值即 ABI，冻结。
- ABI 版本：`NEX_ABI_VERSION = 1`。任何 InternalCall 签名变更必须 `NEX_ABI_VERSION++` 并更新 `tools/abi_snapshot.txt`（L14 的 `make abi-check` 拦截）。
- 文件分块约定：`【托管侧】` `【内核侧】` `【宿主侧】`。

---

# L0 基础契约：错误通道 / 句柄抽象 / ABI 版本化

### L0.1 统一错误通道
【内核侧】
```c
// kernel64.cpp / src/err.c
int  k_set_last_error(int code);      // 写 per-进程 err（slot0 壳亦然）；返回旧值
int  k_get_last_error(void);          // 读并不清除；清除用 k_clear_last_error()
void k_clear_last_error(void);
// icall 注册表（mforms.cpp）
static void icall_set_last_error(uint32_t* args) { k_set_last_error((int)args[0]); args[0]=1; }
static void icall_get_last_error(uint32_t* args) { args[0]=(uint32_t)k_get_last_error(); }
static void icall_clear_last_error(uint32_t* args){ k_clear_last_error(); }
```
语义：错误随进程上下文（token）保存，`CreateProcess` 后子进程 err 归零。
【托管侧】
```csharp
public static class Err {              // 常量码，全部 const（MiniCLR 友好）
    public const int OK=0, NoEntry=1, BadArg=2, BadHandle=3, PermDenied=4,
        NameExists=5, NotFound=6, NoSpace=7, Busy=8, Broken=9, Timeout=10,
        TooBig=11, NoMem=12, NotSupported=13, IoError=14, WouldBlock=15,
        CrossDev=16, NotDir=17, IsDir=18, Quota=19, Deadlk=20, Stale=21;
}
public class Host {
    [MethodImpl(InternalCall)] public static extern void SetLastError(int code);
    [MethodImpl(InternalCall)] public static extern int  LastError();      // 一行语义：读当前进程最近错误码
    [MethodImpl(InternalCall)] public static extern void ClearError();
}
```
【宿主侧】winhost 映射：`host.LastError` 转发到同语义桩，保证托管代码二进制同 ABI。

### L0.2 句柄抽象：全局句柄表
【内核侧】
```c
typedef struct {                       // 全局句柄表项
    uint8_t  kind;                     // HK_*
    uint16_t slot;                     // kind 内子索引
    uint8_t  gen;                      // 换代计数：边界变更/关闭 → gen++
    uint32_t owner;                    // owner token id；0=kernel
    void*    ptr;                      // 指向真实对象
    uint32_t rights;                   // 位集：R=1 W=2 X=4 CTRL=8
} hentry_t;
#define HK_MAX 512
typedef uint32_t h_t;                  // 打包：kind:8 | slot:16 | gen:8
h_t   handle_alloc(uint8_t kind, uint16_t slot, uint32_t owner, void* ptr, uint32_t rights);
void* handle_resolve(h_t h, uint8_t kind, uint32_t owner);   // kind/owner 不符或 gen 不符 → NULL+set_last_error
int   handle_close(h_t h, uint32_t owner);                   // 关闭后 gen++ → 旧句柄永久失效
void  handle_gen_bump_kind(uint8_t kind, uint16_t slot);     // 边界换代失效专用入口
// 错误语义：resolve 失败 set_last_error(Err.BadHandle) 或 Err.Stale（gen 不匹配）
```
语义：`handle_resolve` 是**唯一**合法的句柄→对象路径；内核内部一律持有 `hentry_t*`，不回传裸指针给托管侧。
【托管侧】
```csharp
public struct Handle {                 // 只读值类型，无 .cctor
    public long Raw;
    public int  Kind() { return (int)(Raw & 0xFF); }
    public int  Gen()  { return (int)((Raw >> 24) & 0xFF); }
    public bool Valid() { return Raw != 0; }
    public static Handle None() { Handle h; h.Raw = 0; return h; }
}
public partial class Host {
    [MethodImpl(InternalCall)] static extern long HandleAlloc(int kind, int slot, int rights);
    [MethodImpl(InternalCall)] static extern int  HandleClose(long h);
}
```
语义：托管侧不允许从整数拼句柄，只能从分配 API 获得。
Cap：CAP_HANDLE。自测：`Host.ClearError(); Host.HandleClose(0xDeadBeef); Host.Log("L0.2 err ok=" + (Host.LastError()==Err.BadHandle));`

### L0.3 ABI 版本化与能力探测
【内核侧】
```c
uint32_t k_api_version(void);          // 返回 NEX_ABI_VERSION
uint32_t k_caps_mask(void);            // 位图：bit n = CAP n
```
【托管侧】
```csharp
public static class Cap { public const int Err=0, Handle=1, Perm=2, Proc=3, Thread=4,
    Sync=5, Job=6, Fs=7, Mem=8, Ipc=9, Net=10, Dev=11, Clock=12, Gfx=13,
    Pkg=14, Log=15, Cfg=16, Clr=17, Dbg=18; }
public partial class Host {
    [MethodImpl(InternalCall)] public static extern int  ApiVersion();
    [MethodImpl(InternalCall)] public static extern bool HasCap(int cap);
    [MethodImpl(InternalCall)] public static extern bool Is64Bit();   // 32 位镜像守门用
}
```
语义：调用方在任何可选 Cap 的 icall 之前必须 `HasCap`；32 位镜像对 SetTextGamma/AudioPlay 必须先 `HasCap(Cap.Gfx)||Is64Bit()`（反面清单强制）。
自测：`Host.Log("L0.3 abi ok=" + (Host.ApiVersion() >= 1 && Host.HasCap(Cap.Err)));`

---

# L1 安全权限：token / boundary / ICALL_GUARD

### L1.1 数据结构
【内核侧】
```c
typedef struct {                       // token_t：进程身份
    uint32_t id;                       // 0=kernel, 1=init
    uint32_t uid;                      // 所有者 uid
    uint32_t parent;                   // 父 token id（0=无）
    uint64_t syscall_mask;             // 允许的 syscall 类位（bit n = syscall 类 n）
    uint32_t caps_mask;                // 允许的 Cap 位图（HasCap 实际按此过滤）
    uint32_t flags;                    // TOK_PRIV_R0=1 可改边界
} token_t;

#define FS_MAXVOL 8
#define FS_MAXPREFIX 16
typedef struct {                       // boundary_t：进程边界（不可外泄指针）
    uint32_t fs_vol_mask;              // 允许的卷位图（bit v = 卷 v）
    struct { uint32_t vol; char prefix[64]; uint32_t mode; } prefixes[FS_MAXPREFIX];
                                       // mode: FSB_RO=1 | FSB_RW=2 | FSB_NONE=0；rw=读写
    int      n_prefixes;
    uint64_t fs_quota_bytes, fs_used_bytes;
    uint32_t dev_class_mask;           // 允许的设备类位图
    uint32_t dev_id_whitelist[16]; int n_dev_ids;   // 设备 id 白名单（-1 结尾语义禁用，用计数）
    uint32_t net_port_lo, net_port_hi;              // 0/0=禁网
    uint32_t net_cidr[8]; int n_cidr;  // 打包 a/b/n，n=0=允许任意（在端口允许前提下）
} boundary_t;
```
【托管侧】`Perm` 只读门面（**无任何 SetBound\*/SetToken/SetCaps**，反面清单）：
```csharp
public static class Perm {
    [MethodImpl(InternalCall)] public static extern int  Uid();
    [MethodImpl(InternalCall)] public static extern int  FsVolMask();
    [MethodImpl(InternalCall)] public static extern int  FsPrefixCount();
    [MethodImpl(InternalCall)] public static extern void FsPrefixAt(int i, out_dummy_ok); // 拆标量：
    [MethodImpl(InternalCall)] public static extern int  FsPrefixVol(int i);
    [MethodImpl(InternalCall)] public static extern int  FsPrefixMode(int i);
    [MethodImpl(InternalCall)] public static extern bool FsCanPath(string path, int mode); // 主动检查
    [MethodImpl(InternalCall)] public static extern bool DevAllowed(int devClass, int devId);
    [MethodImpl(InternalCall)] public static extern bool NetPortAllowed(int port);
    [MethodImpl(InternalCall)] public static extern bool NetAddrAllowed(int ipBe);  // 大端 IPv4
    [MethodImpl(InternalCall)] public static extern bool SyscallAllowed(int cls);
    public static bool Can(string path) { return FsCanPath(path, 1); }   // 只读探测
}
```

### L1.2 前缀匹配：必须校验段边界（硬指标）
【内核侧】
```c
// 返回 1 当 path 在 prefix 目录“之内”（prefix 本身也算命中）
// /home/user1 不匹配 /home/user10；/home/user1 与 /home/user1 精确相等 → 命中
static int bnd_prefix_hit(const char* path, const char* prefix) {
    size_t n = kstrlen(prefix);
    if (kstrncmp(path, prefix, n) != 0) return 0;
    char c = path[n];
    return (c == 0 || c == '/');
}
```
任何 fs/net/dev icall 中的授权检查**只准调用此函数**，禁止各自手写 startswith。
执行点：`fs_open/fs_stat/fs_dir_iter/fs_delete/fs_rename/fs_write…`（L3 全部入口）、`dev_open`、`sock_bind/sock_connect`、`exec_*`。

### L1.3 ICALL_GUARD
【内核侧】
```c
#define ICALL_GUARD(cap, cls)                                  \
    do { token_t* tok__ = k_cur_token();                       \
         if (!tok__ || !(tok__->caps_mask & (1u<<(cap))))      \
             { k_set_last_error(Err_PermDenied); return; }     \
         if (!(tok__->syscall_mask & (1ull<<(cls))))           \
             { k_set_last_error(Err_PermDenied); return; }     \
    } while(0)
```
每个 L2–L12 新 icall 函数体第一行（除 L0 三个探针 icall 外）必须带 GUARD。

### L1.4 ring-0 入口与换代失效
边界仅三个入口可改（全部内核态、不可被 GUARD 外的托管调用触达）：
1. **manifest 加载**：`apms_manifest_apply(boundary_t* b)`（apps.man 增补 `bound` 段时）；
2. **特权 shell**：`cmd_bound_set(...)`（ring-0 shell 命令，校验 `TOK_PRIV_R0`）；
3. **内核 API**：`k_boundary_set(uint32_t token_id, const boundary_t* b)`。

```c
int k_boundary_set(uint32_t token_id, const boundary_t* b) {
    // 校验：只降不升由调用方（CreateProcess）保证；此处做完全替换
    boundary_t* cur = k_boundary_of(token_id);
    if (!cur) { k_set_last_error(Err_NoEntry); return 0; }
    *cur = *b;
    handle_gen_bump_all_owned(token_id);   // 关键：换代失效——该 token 名下所有句柄 gen++，旧句柄立即 Stale
    return 1;
}
```
**CreateProcess 边界只降不升**（硬指标）：
```c
int boundary_subset(const boundary_t* child, const boundary_t* parent) {
    if ((child->fs_vol_mask & ~parent->fs_vol_mask)) return 0;
    for (int i=0;i<child->n_prefixes;i++) {
        int ok=0;
        for (int j=0;j<parent->n_prefixes;j++)
            if (child->prefixes[i].vol==parent->prefixes[j].vol &&
                bnd_prefix_hit(parent->prefixes[j].prefix, child->prefixes[i].prefix) &&
                (child->prefixes[i].mode & ~parent->prefixes[i].mode)==0) ok=1;
        if (!ok) return 0;              // 子前缀必须落在父前缀内且权限不升
    }
    if (child->fs_quota_bytes > parent->fs_quota_bytes) return 0;
    if (child->dev_class_mask & ~parent->dev_class_mask) return 0;
    if (child->net_port_lo < parent->net_port_lo || child->net_port_hi > parent->net_port_hi) return 0;
    if (child->syscall_mask & ~parent->syscall_mask) return 0;
    return 1;
}
```
失败 → `set_last_error(Err_PermDenied)`，`CreateProcess` 拒绝。
【manifest 示例行】
```
# kind|显示名|mex|图标色|字母|说明
21|受限计算器|calc_mex|4|C|bound:vol=1,prefix=/data/calc:rw,dev=0,port=0/0
```
【自测】`Host.Log("L1 ok deny=" + (Perm.FsCanPath("/home/user10/x",1)==false) + " hit=" + Perm.FsCanPath("/home/user1/x",1));`

---

# L2 进程/线程/并发/Job/定时器

【内核侧】签名全集（每个入口 ICALL_GUARD(CAP_PROC/THREAD/SYNC/JOB, cls)）：
```c
// 进程
h_t   k_proc_create(const char* image, const char* entry, boundary_t* child_bnd, uint32_t flags);
int   k_proc_wait(h_t h, int timeout_ms, int* exit_code_out);   // 多返回值拆两个 icall：k_proc_wait_code
int   k_proc_kill(h_t h, int code);
int   k_proc_list(int index, uint32_t* tok_out, int* state_out, char* name_out, int name_cap);
int   k_proc_exit_code(h_t h);                                  // wait 后取码（拆标量）
// 线程：每线程独立栈，调度器协作式（内核无 IRQ 抢占，与 AiAsync 同款 re-arm 模型）
h_t   k_thread_create(h_t proc, uint32_t entry_ip, uint32_t stack_sz, void* arg);
void  k_thread_yield(void);
int   k_thread_exit(int code);
h_t   k_thread_self(void);
// 同步原语（全部内核对象、经句柄表）
h_t   k_mutex_new(void); int k_mutex_lock(h_t h, int ms); int k_mutex_unlock(h_t h);
h_t   k_sem_new(int init, int max); int k_sem_wait(h_t h,int ms); int k_sem_post(h_t h);
h_t   k_event_new(void);   int k_event_wait(h_t h,int ms,int clear); int k_event_set(h_t h);
h_t   k_cond_new(h_t mutex); int k_cond_wait(h_t h,int ms); int k_cond_signal(h_t h,int all);
int   k_atomic_add(volatile int* p, int v);   // 单核下关调度实现
// 通用异步 Job（重写 Ai/Download 为 Job 封装）
h_t   k_job_start(uint8_t kind, void* ctx);    // kind: JOB_AI / JOB_DL / JOB_WALK / JOB_PKG
int   k_job_tick(int budget_ms);               // 每帧调用（mforms_paint_overlay 已有 ai_async_tick → 改调此）
int   k_job_status(h_t h);                     // JOB_RUN/JOB_DONE/JOB_ERR/JOB_CANCEL
int   k_job_progress(h_t h, int* total_out);   // 拆标量：k_job_progress_total
int   k_job_cancel(h_t h);
// 定时器
h_t   k_timer_new(int ms, int periodic);
int   k_timer_poll(h_t h);                     // 到期返回 1
```
托管侧：
```csharp
public static class Proc {
    [MethodImpl(InternalCall)] public static extern long Create(string image, string entry);
    [MethodImpl(InternalCall)] public static extern int  Wait(long h, int ms);
    [MethodImpl(InternalCall)] public static extern int  ExitCode(long h);
    [MethodImpl(InternalCall)] public static extern int  Kill(long h, int code);
    [MethodImpl(InternalCall)] public static extern int  ListCount();
    [MethodImpl(InternalCall)] public static extern string ListName(int i);
    [MethodImpl(InternalCall)] public static extern int  ListState(int i);
}
public static class Thread {
    [MethodImpl(InternalCall)] public static extern long Create(long procH, int entry, int stackSz);
    [MethodImpl(InternalCall)] public static extern void Yield();
    [MethodImpl(InternalCall)] public static extern int  Self();
}
public static class Sync {
    [MethodImpl(InternalCall)] public static extern long MutexNew();
    [MethodImpl(InternalCall)] public static extern int  Lock(long h, int ms);
    [MethodImpl(InternalCall)] public static extern int  Unlock(long h);
    [MethodImpl(InternalCall)] public static extern long SemNew(int init, int max);
    [MethodImpl(InternalCall)] public static extern int  SemWait(long h, int ms);
    [MethodImpl(InternalCall)] public static extern int  SemPost(long h);
    [MethodImpl(InternalCall)] public static extern long EventNew();
    [MethodImpl(InternalCall)] public static extern int  EventWait(long h, int ms, int clear);
    [MethodImpl(InternalCall)] public static extern int  EventSet(long h);
    [MethodImpl(InternalCall)] public static extern long CondNew(long mutex);
    [MethodImpl(InternalCall)] public static extern int  CondWait(long h, int ms);
    [MethodImpl(InternalCall)] public static extern int  CondSignal(long h, int all);
    [MethodImpl(InternalCall)] public static extern int  AtomicAdd(int cls, int idx, int v); // 内核侧原子槽位，避免裸指针跨 ABI
}
public static class Job {
    public const int Run=0, Done=1, Err=2, Cancel=3;
    [MethodImpl(InternalCall)] public static extern long Start(int kind, int ctxInt);
    [MethodImpl(InternalCall)] public static extern int  Status(long h);
    [MethodImpl(InternalCall)] public static extern int  Progress(long h);
    [MethodImpl(InternalCall)] public static extern int  ProgressTotal(long h);
    [MethodImpl(InternalCall)] public static extern int  Cancel(long h);
}
public static class Timer {
    [MethodImpl(InternalCall)] public static extern long New(int ms, int periodic);
    [MethodImpl(InternalCall)] public static extern int  Poll(long h);
}
```
【宿主侧】winhost 中 Ai/Download 原有逻辑改为 `Job.Start(JOB_AI/JOB_DL, ctx)` 封装，旧 `Host.AiStart/AiPoll` 保留为兼容薄壳。
自测：`long m=Sync.MutexNew(); Sync.Lock(m,0); Sync.Unlock(m); Host.Log("L2 ok=" + (Sync.Lock(m,0)==1));`

---

# L3 文件系统

【内核侧】
```c
// VFS
int   k_vfs_mount(int vol, const char* mnt_point, uint8_t fstype);   // SFS/FAT
int   k_vfs_resolve(const char* path, char* out, int out_cap);
// 解析规则：/ → 当前卷根；.. 逐段弹出，禁越过挂载点根（弹过头 → Err.NoEntry）；
// 结果是规范绝对路径，无重复 '/'、无 '.'。
int   k_vfs_umount(const char* mnt_point);
// 元数据
int   k_fs_chmod(const char* path, uint32_t mode);
int   k_fs_chown(const char* path, uint32_t uid);
int   k_fs_stat2(const char* path, uint32_t* mode_out);  // 拆标量：k_fs_stat_uid / k_fs_stat_size / k_fs_stat_mtime
// 打开（原子语义）
h_t   k_fs_open(const char* path, uint32_t flags);
      // flags: O_RD=1 O_WR=2 O_APPEND=4 O_TRUNC=8 O_CREAT=16 O_EXCL=32 O_NONBLOCK=64
      // O_CREAT|O_EXCL 且已存在 → set_last_error(Err_NameExists) 返回 0（硬指标）
int   k_fs_read(h_t h, void* buf, int len);   int k_fs_write(h_t h, const void* buf, int len);
int   k_fs_seek(h_t h, int off, int whence);  int k_fs_size(h_t h);
int   k_fs_rename(const char* a, const char* b);       // 跨卷 → Err.CrossDev
int   k_fs_link(const char* t, const char* l);         // 硬链（同卷）
int   k_fs_symlink(const char* t, const char* l);      // 符号链（记录目标，resolve 时跟随一次）
int   k_fs_symlink_target(const char* l, char* out, int cap);
int   k_fs_lock(h_t h, int op);                        // op: 1=LOCK_SH 2=LOCK_EX 0=UNLOCK；冲突 → Err.Busy
int   k_fs_unlink(const char* path);
// 目录
int   k_fs_mkdir(const char* p); int k_fs_rmdir(const char* p);
h_t   k_fs_dir_open(const char* p);
int   k_fs_dir_read(h_t h, char* out, int cap);        // 逐项；末尾返回 0
int   k_fs_glob(const char* pattern, int index, char* out, int cap);  // index 遍历，仅支持 * 与 ?（单层目录）
h_t   k_walk_start(const char* root);                  // 异步递归遍历 Job（JOB_WALK）
int   k_walk_next(h_t h, char* out, int cap, int* is_dir_out);
```
托管侧：
```csharp
public enum-class 不用（无接口/枚举 ABI 顾虑） → const 类：
public static class FileFlags { public const int RD=1, WR=2, Append=4, Trunc=8,
    Creat=16, Excl=32, NonBlock=64; }
public static class File {
    [MethodImpl(InternalCall)] public static extern long Open(string path, int flags);
    [MethodImpl(InternalCall)] public static extern int  Read(long h, byte[] buf, int len);
    [MethodImpl(InternalCall)] public static extern int  Write(long h, byte[] buf, int len);
    [MethodImpl(InternalCall)] public static extern int  Seek(long h, int off, int whence);
    [MethodImpl(InternalCall)] public static extern int  Size(long h);
    [MethodImpl(InternalCall)] public static extern int  Lock(long h, int op);
    [MethodImpl(InternalCall)] public static extern int  Close(long h);
    [MethodImpl(InternalCall)] public static extern int  Rename(string a, string b);
    [MethodImpl(InternalCall)] public static extern int  Link(string t, string l);
    [MethodImpl(InternalCall)] public static extern int  SymLink(string t, string l);
    [MethodImpl(InternalCall)] public static extern string SymTarget(string l);   // StrSub 单次分配
    [MethodImpl(InternalCall)] public static extern int  Unlink(string p);
    [MethodImpl(InternalCall)] public static extern int  MkDir(string p);
    [MethodImpl(InternalCall)] public static extern int  RmDir(string p);
}
public static class Fs {
    [MethodImpl(InternalCall)] public static extern int  Mount(int vol, string mnt, int fstype);
    [MethodImpl(InternalCall)] public static extern int  Umount(string mnt);
    [MethodImpl(InternalCall)] public static extern int  Chmod(string p, int mode);
    [MethodImpl(InternalCall)] public static extern int  Chown(string p, int uid);
    [MethodImpl(InternalCall)] public static extern int  ModeOf(string p);
    [MethodImpl(InternalCall)] public static extern int  UidOf(string p);
    [MethodImpl(InternalCall)] public static extern int  SizeOf(string p);
    [MethodImpl(InternalCall)] public static extern int  MtimeOf(string p);
    [MethodImpl(InternalCall)] public static extern long DirOpen(string p);
    [MethodImpl(InternalCall)] public static extern string DirNext(long h);      // null 结束（StrSub）
    [MethodImpl(InternalCall)] public static extern string GlobAt(string pattern, int i);
    [MethodImpl(InternalCall)] public static extern long WalkStart(string root);
    [MethodImpl(InternalCall)] public static extern string WalkNext(long h);     // DirNext 语义
    [MethodImpl(InternalCall)] public static extern int  WalkIsDir(long h);
}
```
托管侧约定：`File.Open` 返回 0 时调用方查 `Host.LastError`；所有 string 返回用 `StrSub` 一次分配，null 用 0 句柄语义（`Err.NoEntry`）。
自测：`long f=File.Open("/data/t.txt", FileFlags.Creat|FileFlags.Excl); int e1=Host.LastError(); long g=File.Open("/data/t.txt", FileFlags.Creat|FileFlags.Excl); Host.Log("L3 ok=" + (f!=0 && g==0 && e1==Err.OK && Host.LastError()==Err.NameExists)); File.Unlink("/data/t.txt");`

---

# L12 配置存储（提前至第 4 位执行）

【内核侧】
```c
h_t   k_cfg_open(const char* name);        // name 归一化为 /data/cfg/<name>.cfg；句柄缓存整文件
int   k_cfg_get(h_t h, const char* key, char* out, int cap);   // 不存在 → Err.NoEntry
int   k_cfg_set(h_t h, const char* key, const char* val);      // 写入内存副本
int   k_cfg_del(h_t h, const char* key);
int   k_cfg_keys(h_t h, int index, char* out, int cap);
int   k_cfg_flush(h_t h);                  // 原子落盘：写 tmp → rename
```
托管侧：
```csharp
public static class Cfg {
    [MethodImpl(InternalCall)] public static extern long   Open(string name);
    [MethodImpl(InternalCall)] public static extern string Get(long h, string key);   // null=无此键
    [MethodImpl(InternalCall)] public static extern int    Set(long h, string key, string val);
    [MethodImpl(InternalCall)] public static extern int    Del(long h, string key);
    [MethodImpl(InternalCall)] public static extern string KeyAt(long h, int i);
    [MethodImpl(InternalCall)] public static extern int    Flush(long h);
}
```
【宿主侧】迁移：`Shell::Init` 中一次性把 `Theme` 设置（原内存态）与 `apps.json` 的 `assoc.cfg` 键值搬入 `Cfg.Open("shell")` / `Cfg.Open("assoc")` 并 `Flush`；旧内存态仅作缺省值来源。
自测：`long c=Cfg.Open("t"); Cfg.Set(c,"k","v"); Cfg.Flush(c); long d=Cfg.Open("t"); Host.Log("L12 ok=" + (Cfg.Get(d,"k")=="v"));`

### L12 落地记录（实现与契约的对齐说明）

**Cap / 执行点**：`CAP_CFG=16`，由 `osapi_ic_caps_mask()` 在实际落地后置位，`Host.HasCap(Cap.Cfg)` 可探测。9 个 icall 首行一律 `ICALL_GUARD(CAP_CFG, SCL_MISC)`——同时检查 `caps_mask` 与 `syscall_mask`，失败 `set_last_error(Err.PermDenied)` 并返回 0。该宏在 `osapi.h` 中统一定义：L1.3 的契约由此落地，L12 是首个完整使用者，L4–L11 直接复用。**收尾待办**：L2/L3 的存量 icall 尚未逐个补挂 GUARD（全量 17 条验收第 7 条要求 L2–L12 全覆盖）。

**权限改动**：无新增 ring-0 入口，L12 不改边界。权限检查沿用两个执行点：
- `osapi_cfg_open_c()` → `osapi_fs_path_allowed(token, "/data/cfg/<name>.cfg", FSB_RO)`；
- `osapi_cfg_flush_c()` → 同路径 `FSB_RW`（只读 token 可读配置但不能落盘）。

两者最终都走唯一的段边界函数 `osapi_prefix_hit()`，无手写 startswith。逻辑名用 `[A-Za-z0-9_.-]` 白名单且拒绝 `.` / `..`，保证归一化后不会越出 `/data/cfg/` 段。

**数据结构 / 只读访问器**：C 侧 `CfgEntry{key[32],val[96]}` + `OsCfg{used,dirty,name[16],path[64],e[48],n,owner}`（`osapi.h`）；托管侧不暴露可变结构，只给标量只读访问器 `Cfg.Count/Dirty/KeyAt/Get` 与 `Cfg.Wrap(int) -> Handle`。

**manifest 示例行**（受限应用只允许读自己的配置目录）：
```
21|受限记事本|notepad_mex|4|N|bound:vol=1,prefix=/data/cfg:rw,dev=0,port=0/0
```

**ABI 偏差（必须记录）**：契约里句柄写作 `long`，但 MiniCLR 是 32 位栈机（`clr_icall_fn = int32_t(*)(int32_t* args)`，每个参数占一个 32 位 slot；`conv.i8` 被截断），用 `long` 会让 a[] 索引错位。因此 ABI v1 中 `Cfg.Open` 与 `File.Open`/`Sync.MutexNew` 保持一致，返回打包 `h_t` 的 `int`（0 = 无效句柄），并由 `Handle` 只读结构包装。若将来 MiniCLR 支持 64 位栈槽，`NEX_ABI_VERSION++` 时再统一升级为 `long`。

**存储映射**：64 位 MKFS 是 flat 表（`FS_NAME_LEN=20`），故逻辑路径 `/data/cfg/<name>.cfg` 物理落为 basename `<name>.cfg`；Flush 先写 `<name>.tmp`，再 `nexos_mcp_fs_rename(tmp, cfg)`（`mkfs.create` 覆盖 + `remove`），中途崩溃仍保留旧 `.cfg`。`Close` 自动 Flush，且 `gen++` 让旧句柄解析为 `Err.Stale`。

**自测**：内核侧 `osapi_ic_cfg_self_test()` 直接调纯 C API（不经 `clr_new_str`，避免 CLR 上下文外假绿），覆盖 NoEntry、Set/Get、Count/KeyAt、Dirty、Flush 落盘与 tmp 回收、重开持久化、Del、超长键、坏名、换代失效；托管侧 `Shell.Init` 打印 `[SHELL] osapi L12 cfg ok=1`。

---

# L4 内存

【内核侧】
```c
void* k_mem_alloc(uint32_t sz);            // 持久真堆；托管不持指针，只持句柄
h_t   k_heap_alloc(uint32_t sz);           // 句柄化版本（HK_HEAP）
int   k_heap_free(h_t h);                  // free 后 gen++ → 悬空防护（硬指标"句柄防悬空"）
h_t   k_heap_realloc(h_t h, uint32_t sz);  // 可换 slot，返回新句柄，旧句柄失效
h_t   k_arena_new(uint32_t cap);
void* k_arena_ptr(h_t h);                  // 内核内部专用
int   k_arena_mark(h_t h);
int   k_arena_release(h_t h, int mark);    // 回滚到 mark
h_t   k_shm_create(const char* name, uint32_t sz);
h_t   k_shm_open(const char* name);
void* k_shm_map(h_t h, uint32_t off, uint32_t len);   // 仅 ring-0 线程（内核侧 mapping 记账）
int   k_mmap(int what, uint32_t addr, uint32_t len);  // MMIO/帧缓冲 映射记账，受 dev_class_mask GUARD
```
托管侧：
```csharp
public static class Mem {
    [MethodImpl(InternalCall)] public static extern long Alloc(uint32_ok);   // 用 int 参数（MiniCLR 无 uint）：
    [MethodImpl(InternalCall)] public static extern long HeapAlloc(int sz);
    [MethodImpl(InternalCall)] public static extern int  HeapFree(long h);
    [MethodImpl(InternalCall)] public static extern long HeapRealloc(long h, int sz);
    [MethodImpl(InternalCall)] public static extern long ArenaNew(int cap);
    [MethodImpl(InternalCall)] public static extern int  ArenaMark(long h);
    [MethodImpl(InternalCall)] public static extern int  ArenaRelease(long h, int mark);
    [MethodImpl(InternalCall)] public static extern long ShmCreate(string name, int sz);
    [MethodImpl(InternalCall)] public static extern long ShmOpen(string name);
    [MethodImpl(InternalCall)] public static extern int  ShmSize(long h);
}
```
语义：托管侧永远只见句柄，不见地址（无指针暴露，防悬空）。Paint() 内的临时对象走 CLR 帧回卷堆，与本层真堆无关——两条内存轨互不污染。
自测：`long h=Mem.HeapAlloc(64); h=Mem.HeapRealloc(h,128); int ok=Mem.HeapFree(h)==1 && Mem.HeapFree(h)==0 && Host.LastError()==Err.BadHandle; Host.Log("L4 ok="+ok);`

### L4 落地记录（第 5 个执行层，已实机验证）

**ABI 偏差（与 L12 同源）**：契约里的 `long` 句柄在实现中统一为打包 `h_t` 的 `int`（`kind:8|slot:16|gen:8`）。MiniCLR 是 32 位栈机，`int32_t(*)(int32_t* args)` 的参数槽固定 4 字节，`long` 会让参数索引错位。升级需 `NEX_ABI_VERSION++`。托管侧 `Mem.Wrap(int)` 提供只读 `Handle` 视图；**任何 icall 都不返回地址**，`osapi_mem_ptr_c()` 是 `extern` 但仅供 ring-0 内核内部使用，未注册成 icall。

**数据结构**（`osapi.h`）：`OsMemBlock{used,kind,gen,handle_gen,owner,size,used_bytes,base,backing,name}` × 64、`OsShmSeg{used,name,base,size,refs}` × 16、`OsMemMap{used,what,addr,len,owner}` × 16。`base` 来自 64 位内核 `kmalloc()` 的持久堆（16 MiB @0x500000，`heap_init()` 在 K64-7 完成，早于 `mforms_init()`），与 `Paint()` 用的 CLR 帧回卷堆是两条互不污染的内存轨。`MEM_BLOCK_CAP=1 MiB` 单块上限。

**双层 gen 防悬空（本层核心硬指标，验收第 2 条）**：
1. `osapi_handle_close()` 在 free/realloc/shm-close/arena-free 时把全局句柄表项的 `gen++`；
2. 每个 `OsMemBlock` 自存 `gen` 与 `handle_gen`（分配时记录 `hgen(h)`），`mem_from()` 在 `osapi_handle_resolve()` 之后**再比一次** `b->handle_gen != hgen(h)` → `ERR_STALE`。
   即使全局表因表项复用顺序意外交回一个 `(kind,slot,gen)` 完全相同的三元组，第二道也会拒绝旧句柄。`HeapRealloc` 先关旧句柄（两处 gen 同时前进）再发新句柄，因此**旧句柄永久失效**。

**纯 C 自测 `osapi_ic_mem_self_test()`**：全函数不出现 `clr_str` / `clr_new_str`，`mforms_init()` 里连续调用两次以证明幂等且不是"假绿"。断言清单：
- `HeapFree` 前读 `osapi_mem_slot_gen_c(slot)`，free 后断言 **数值 +1**（真断言 gen++，不是只看返回值）；
- 同槽重新分配后断言 `hgen(new) != hgen(old)`，且**旧 `h_t` 必须报 `ERR_STALE`**（拒绝「只是没找到」的 `ERR_BADHANDLE`）；串口打印 `[OSAPI] L4 gen++ proof: slot=... slotgen 1->2 handlegen 1->2 stale=21`；
- free 后立即再 free → 0 + `ERR_BADHANDLE/STALE`；越界 `HeapWrite` → `ERR_BADARG`；
- `HeapRealloc(64→128)` 保留前 4 字节内容、旧句柄失效、新 size=128；
- Arena mark/release 回滚到 33、越界 mark 拒绝、超额分配 `ERR_NOSPACE`、free 后旧句柄失效；
- Shm 重复 create `ERR_NAMEEXISTS`、两个句柄指向同一 `base`、关第一个后第二个仍存活、最后一个关闭后段销毁（`ShmOpen` → `ERR_NOTFOUND`）、旧句柄失效；
- Mmap 重叠 `ERR_BUSY`、非法 what `ERR_BADARG`、临时撤掉 `dev_class_mask` 的 MMIO 位后断言 `ERR_PERMDENIED`（生产路径仍只能由 ring-0 改 boundary）、重复 unmap `ERR_NOTFOUND`；
- 构造降级子 token 分别撤掉 `SCL_MEM` 与 `CAP_MEM`，断言 `ICALL_GUARD` **两道门**都返回 `ERR_PERMDENIED`；
- 收尾断言 `live` 块数与 mapping 数回到基线（无泄漏）。

**Cap / 执行点**：`CAP_MEM=8`、`SCL_MEM=6`；20 个 icall 首行一律 `ICALL_GUARD(CAP_MEM, SCL_MEM)`。`caps_mask()` 已加 `1<<CAP_MEM`，`Host.HasCap(Cap.Mem)` 可探测。Mmap 额外走 `osapi_dev_allowed()` 的 `dev_class_mask` 门。`MM_WHAT_FB=1→dev_class 0`、`MM_WHAT_MMIO=2→dev_class 2`；内核本就 identity-map，故 Mmap **只记账**（范围/重叠/权限），不改页表。manifest 示例行：`bound:vol=1,prefix=/data/cfg:rw,dev=0xff,port=0/0`。

**返回值约定补充**：`ArenaAlloc` 的 0 是**合法字节偏移**，失败返回 -1（其余失败路径仍返回 0 并 `set_last_error`）；`HeapPeek` 成功返回 0..255、失败 -1。所有失败均写 per-token 错误通道。

**实机证据**：`[OSAPI] mem self-test fails=0`（×2）、`[OSAPI] L4 gen++ proof: ...`、`[SHELL] osapi L4 mem ok=1`；`MAKE_EXIT=0`、`k64 undefined: 0 (clean)`、VDI `IN SYNC`。

---

# L5 IPC：管道 / 消息队列 / Socket / PostMessage

【内核侧】
```c
h_t   k_pipe_new(int cap_bytes, int named, const char* name);   // named=1 注册到 /dev/pipe/<name>
h_t   k_pipe_open(const char* name);                            // 打开已存在命名管道
int   k_pipe_write(h_t h, const void* buf, int len);            // 满且非阻塞 → Err.WouldBlock
int   k_pipe_read(h_t h, void* buf, int len);                   // 空且非阻塞 → Err.WouldBlock
h_t   k_msgq_new(const char* name, int max_msg, int msg_sz);
int   k_msgq_send(h_t h, const void* msg); int k_msgq_recv(h_t h, void* out);
// socket 栈（IPv4，内核 lwIP 式精简栈）
h_t   k_sock_new(int kind);            // SOCK_TCP=1 SOCK_UDP=2 SOCK_TLS=3（TLS=L9 证书栈就绪后启用）
int   k_sock_bind(h_t h, uint32_t ip_be, int port);   // port 受 boundary net_port 检查
int   k_sock_listen(h_t h, int backlog);
h_t   k_sock_accept(h_t h);                            // 阻塞走 Job；同步版 → Err.WouldBlock
int   k_sock_connect(h_t h, uint32_t ip_be, int port); // 地址受 net_cidr 检查（bnd_net_addr_allowed）
int   k_sock_send(h_t h, const void* b, int n); int k_sock_recv(h_t h, void* b, int n);
int   k_sock_close(h_t h);
h_t   k_sock_select(h_t* hs, int n, int ms);           // 就绪句柄返回；内核侧数组
// 跨进程窗口消息
int   k_wm_post(uint32_t dst_app_id, uint32_t msg, uint32_t wparam, uint32_t lparam);
      // 投入目标进程消息队列；目标轮询 k_wm_peek
int   k_wm_peek(uint32_t* msg_out, uint32_t* wp_out, uint32_t* lp_out);  // 拆 3 个 icall 取参亦可
```
托管侧：
```csharp
public static class Pipe {
    [MethodImpl(InternalCall)] public static extern long New(int capBytes, int named, string name);
    [MethodImpl(InternalCall)] public static extern long Open(string name);
    [MethodImpl(InternalCall)] public static extern int  Write(long h, byte[] buf, int len);
    [MethodImpl(InternalCall)] public static extern int  Read(long h, byte[] buf, int len);
    [MethodImpl(InternalCall)] public static extern int  Close(long h);
}
public static class MsgQ {
    [MethodImpl(InternalCall)] public static extern long New(string name, int maxMsg, int msgSz);
    [MethodImpl(InternalCall)] public static extern int  Send(long h, byte[] msg);
    [MethodImpl(InternalCall)] public static extern int  Recv(long h, byte[] outBuf);
}
public static class Sock {
    public const int TCP=1, UDP=2, TLS=3;
    [MethodImpl(InternalCall)] public static extern long New(int kind);
    [MethodImpl(InternalCall)] public static extern int  Bind(long h, int ipBe, int port);
    [MethodImpl(InternalCall)] public static extern int  Listen(long h, int backlog);
    [MethodImpl(InternalCall)] public static extern long Accept(long h);
    [MethodImpl(InternalCall)] public static extern int  Connect(long h, int ipBe, int port);
    [MethodImpl(InternalCall)] public static extern int  Send(long h, byte[] b, int n);
    [MethodImpl(InternalCall)] public static extern int  Recv(long h, byte[] b, int n);
    [MethodImpl(InternalCall)] public static extern int  Close(long h);
    [MethodImpl(InternalCall)] public static extern int  IPToBe(string dotted);   // "10.0.0.1" → 大端 int
}
public static class WinMsg {
    [MethodImpl(InternalCall)] public static extern int  Post(uint32_ok /*int*/ dstApp, int msg, int wp, int lp);
    [MethodImpl(InternalCall)] public static extern int  Peek();          // 有消息返回 1
    [MethodImpl(InternalCall)] public static extern int  PeekMsg();
    [MethodImpl(InternalCall)] public static extern int  PeekW();
    [MethodImpl(InternalCall)] public static extern int  PeekL();
}
```
【宿主侧】mforms.cpp 现有 `Host.AiPoll` 风格：`mforms_paint_overlay` 增加 `WinMsg.Peek` 派发到托管壳的 `OnWinMsg`（新增可覆写方法）。
自测：`long p=Pipe.New(256,0,null); Pipe.Write(p, b, 4); Pipe.Read(p, b2, 4); Host.Log("L5 pipe ok="+(b2[0]==b[0])); long s=Sock.New(Sock.TCP); Sock.Bind(s, Sock.IPToBe("127.0.0.1"), 8088); Sock.Listen(s,2); Host.Log("L5 sock ok="+(Host.LastError()==Err.OK));`

---

# L9 动态应用注册与 .nexpkg 包管理

【内核侧】
```c
// .nexpkg = 头部魔数 NEXP + 清单段 + mex blob + 资源段（SFS 单文件）
int   k_pkg_install(const char* nexpkg_path, char* app_name_out, int cap);
      // 校验：魔数/清单 bound 段必须 ⊆ 安装者 boundary（boundary_subset 对 manifest 生成 boundary）；
      // 通过后写 /data/apps/<name>.mex + 追加 apps.man 行
int   k_pkg_uninstall(const char* app_name);
int   k_pkg_verify(const char* nexpkg_path);       // 只校验不安装
int   k_app_register(const char* name, const char* mex, int kind, int icon_color);
      // 运行时注册（不经包）；写 g_apms_apps[]，Flush 到 apps.man
```
托管侧：
```csharp
public static class Pkg {
    [MethodImpl(InternalCall)] public static extern int  Install(string path);
    [MethodImpl(InternalCall)] public static extern string InstalledName();
    [MethodImpl(InternalCall)] public static extern int  Uninstall(string appName);
    [MethodImpl(InternalCall)] public static extern int  Verify(string path);
    [MethodImpl(InternalCall)] public static extern int  Register(string name, string mex, int kind, int color);
}
```
manifest `bound` 段示例：`bound:vol=1,prefix=/data/myapp:rw,dev=2,port=1000/2000,cidr=10.0.0.0/8`
自测：`Host.Log("L9 ok=" + (Pkg.Register("Probe","probe_mex",20,3)==1));`

---

# L6 设备模型与电源

【内核侧】
```c
// 设备模型：dev_t 记账 + 类/id 白名单（L1 boundary）
h_t   k_dev_open(uint8_t dev_class, uint8_t dev_id);
      // 检查 bnd_dev_allowed(dev_class, dev_id)；DEV_BLOCK=1 DEV_CHAR=2 DEV_NET=3 DEV_GPU=4 DEV_AUDIO=5 DEV_INPUT=6
int   k_dev_read(h_t h, void* b, int n); int k_dev_write(h_t h, const void* b, int n);
int   k_dev_ioctl(h_t h, int cmd, int arg);
int   k_dev_close(h_t h);
// 电源
int   k_power_acpi_query(void);        // 位：0=AC 在线 1=电池存在
int   k_power_battery_pct(void);
int   k_power_set_profile(int p);      // 0=perf 1=balanced 2=saver
int   k_power_sleep(void);             // ring-0：仅特权 shell/manifest 触发，GUARD 特殊位 TOK_PRIV_R0
```
托管侧：
```csharp
public static class Dev {
    public const int Block=1, Char=2, Net=3, Gpu=4, Audio=5, Input=6;
    [MethodImpl(InternalCall)] public static extern long Open(int devClass, int devId);
    [MethodImpl(InternalCall)] public static extern int  Read(long h, byte[] b, int n);
    [MethodImpl(InternalCall)] public static extern int  Write(long h, byte[] b, int n);
    [MethodImpl(InternalCall)] public static extern int  IoCtl(long h, int cmd, int arg);
    [MethodImpl(InternalCall)] public static extern int  Close(long h);
}
public static class Power {
    [MethodImpl(InternalCall)] public static extern int  AcOnline();
    [MethodImpl(InternalCall)] public static extern int  BatteryPct();
    [MethodImpl(InternalCall)] public static extern int  SetProfile(int p);
}
```
设备边界执行点：`k_dev_open` 唯一入口，`ICALL_GUARD(CAP_DEV, cls_dev)` + boundary 检查双闸。
自测：`Host.Log("L6 ok batt=" + Power.BatteryPct() + " ac=" + Power.AcOnline());`

---

# L7 高精度时钟与 NTP

【内核侧】
```c
uint64_t k_clock_mono_ns(void);        // 单调高精度（TSC→ns 换算）
int   k_clock_wall(uint32_t* epoch_out);      // 拆标量：k_clock_wall_ms
int   k_clock_set_wall(uint32_t epoch, int ms);   // ring-0（TOK_PRIV_R0）
h_t   k_ntp_sync(const char* server);  // UDP:123，走 JOB_ 栈；结果写 wall
int   k_ntp_status(h_t h);             // 0=运行 1=成功 2=失败（差值经 k_clock_ntp_drift_ms）
```
托管侧：
```csharp
public static class Clock {
    [MethodImpl(InternalCall)] public static extern long MonoNs();
    [MethodImpl(InternalCall)] public static extern int  WallEpoch();
    [MethodImpl(InternalCall)] public static extern int  WallMs();
    [MethodImpl(InternalCall)] public static extern long NtpSync(string server);
    [MethodImpl(InternalCall)] public static extern int  NtpStatus(long h);
    [MethodImpl(InternalCall)] public static extern int  NtpDriftMs();
}
```
自测：`long t0=Clock.MonoNs(); long t1=Clock.MonoNs(); Host.Log("L7 ok="+(t1>t0));`

---

# L8 保留模式窗口 / 2D 加速 / 完整输入

【内核侧】（在 mforms/gui_legacy 之上补齐能力层；32 位镜像守门：HasCap(Cap.Gfx)&&Is64Bit）
```c
// 保留模式窗口（内核侧窗口对象记账，句柄化）
h_t   k_win_create(int x,int y,int w,int h,uint32_t style);   // style 位：CAPTION=1 RESIZE=2 SYSMENU=4
int   k_win_set_title(h_t h, const char* t);
int   k_win_rect(h_t h, int* x_out);                          // 拆标量 x/y/w/h 四个 icall
int   k_win_show(h_t h, int show);  int k_win_destroy(h_t h);
int   k_win_invalidate(h_t h);                                 // 触发重绘（不分配）
// 2D 加速（blitter：VBox/VBE 下用内存块搬移 + 矩形填充快速路径）
int   k_gfx_blit(int dx,int dy,int sx,int sy,int w,int h,uint32_t* src,uint32_t* dst);
int   k_gfx_fill(int x,int y,int w,int h,uint32_t color,uint32_t* dst);
int   k_gfx_caps(void);                                        // 位：0=fill 1=blit 2=alpha
// 完整输入：事件队列（鼠标滚轮/移动/按下释放、键盘按下释放+修饰键）
int   k_input_next(uint32_t* type_out);                        // 拆标量 type/k/x/y/key/mod
int   k_input_key(); int k_input_mouse_x(); ...
```
托管侧：
```csharp
public static class Win {
    public const int Caption=1, Resize=2, SysMenu=4;
    [MethodImpl(InternalCall)] public static extern long  Create(int x,int y,int w,int h,int style);
    [MethodImpl(InternalCall)] public static extern int   SetTitle(long h, string t);
    [MethodImpl(InternalCall)] public static extern int   X(long h);
    [MethodImpl(InternalCall)] public static extern int   Y(long h);
    [MethodImpl(InternalCall)] public static extern int   W(long h);
    [MethodImpl(InternalCall)] public static extern int   H(long h);
    [MethodImpl(InternalCall)] public static extern int   Show(long h, int show);
    [MethodImpl(InternalCall)] public static extern int   Invalidate(long h);
    [MethodImpl(InternalCall)] public static extern int   Destroy(long h);
}
public static class Gfx {
    [MethodImpl(InternalCall)] public static extern int Caps();
    [MethodImpl(InternalCall)] public static extern int Fill(int x,int y,int w,int h,int color);
    [MethodImpl(InternalCall)] public static extern int Blit(int dx,int dy,int sx,int sy,int w,int h);
}
public static class Input {
    [MethodImpl(InternalCall)] public static extern int  Next();       // 有事件 1
    [MethodImpl(InternalCall)] public static extern int  Type();
    [MethodImpl(InternalCall)] public static extern int  Key();        // 键码或按钮码
    [MethodImpl(InternalCall)] public static extern int  MouseX();
    [MethodImpl(InternalCall)] public static extern int  MouseY();
    [MethodImpl(InternalCall)] public static extern int  Mods();       // SHIFT=1 CTRL=2 ALT=4
}
```
约束重申：托管 Paint() 内禁止分配持久对象——窗口创建/销毁全部发生在事件循环侧。
自测：`long w=Win.Create(10,10,200,100,Win.Caption); Win.SetTitle(w,"probe"); Win.Destroy(w); Host.Log("L8 ok="+(Host.LastError()==Err.OK));`

---

# L10 统一日志 / 性能计数 / 崩溃转储

【内核侧】
```c
int   k_log_write(uint8_t level, uint32_t tag, const char* msg);
      // level: 0=trace 1=info 2=warn 3=error；落环形日志（APMS tee 环扩容至 64KiB）
int   k_log_read(int index, char* out, int cap);       // 按序读取；index<0 读最新
int   k_log_set_level(uint8_t level);
h_t   k_perf_new(const char* name);                    // 计数器
int   k_perf_inc(h_t h, int d);
int   k_perf_get(h_t h);
int   k_perf_time_start(h_t h); int k_perf_time_end(h_t h);   // 累计耗时 μs
int   k_crash_dump(const char* note);                  // 写 /data/crash/<ts>.dmp：寄存器+栈顶+CLR 调用栈（k_clr_stacktrace）
```
托管侧：
```csharp
public static class Log {
    public const int Trace=0, Info=1, Warn=2, Error=3;
    [MethodImpl(InternalCall)] public static extern int  Write(int level, string tag, string msg);
    [MethodImpl(InternalCall)] public static extern string ReadAt(int i);   // StrSub
    [MethodImpl(InternalCall)] public static extern int  SetLevel(int level);
    public static int I(string tag, string msg) { return Write(1, tag, msg); }
}
public static class Perf {
    [MethodImpl(InternalCall)] public static extern long New(string name);
    [MethodImpl(InternalCall)] public static extern int  Inc(long h, int d);
    [MethodImpl(InternalCall)] public static extern int  Get(long h);
    [MethodImpl(InternalCall)] public static extern int  TimeStart(long h);
    [MethodImpl(InternalCall)] public static extern int  TimeEnd(long h);
}
public static class Crash {
    [MethodImpl(InternalCall)] public static extern int  Dump(string note);
}
```
自测：`Log.I("probe","hello"); Host.Log("L10 ok="+(Log.ReadAt(0)!=null));`

---

# L11（编号承接：DNS/代理/证书）

【内核侧】
```c
int   k_dns_resolve(const char* host, uint32_t* ip_be_out);    // 缓存 + UDP:53；走 JOB 栈异步版 k_dns_resolve_async
int   k_proxy_set(const char* host, int port, int kind);       // kind: 0=off 1=http 2=socks5；ring-0 特权口
int   k_proxy_get(char* host_out, int cap);                    // 拆标量：k_proxy_port / k_proxy_kind
int   k_cert_install(const char* pem_path);                    // 装入信任库 /data/certs
int   k_cert_verify(const char* pem_path);                     // 0=ok 1=未知CA 2=过期 3=签名错
int   k_tls_handshake(h_t sock, const char* sni);              // Sock TLS 句柄的实际握手
```
托管侧：
```csharp
public static class Dns {
    [MethodImpl(InternalCall)] public static extern int   Resolve(string host);        // 返回大端 IP int
    [MethodImpl(InternalCall)] public static extern long  ResolveAsync(string host);
    [MethodImpl(InternalCall)] public static extern int   Status(long job);
}
public static class Proxy {
    [MethodImpl(InternalCall)] public static extern int  Set(string host, int port, int kind);
    [MethodImpl(InternalCall)] public static extern int  Kind();
    [MethodImpl(InternalCall)] public static extern int  Port();
}
public static class Cert {
    [MethodImpl(InternalCall)] public static extern int  Install(string pemPath);
    [MethodImpl(InternalCall)] public static extern int  Verify(string pemPath);
    [MethodImpl(InternalCall)] public static extern int  Handshake(long sock, string sni);
}
```
自测：`int ip=Dns.Resolve("localhost"); Host.Log("L11 ok="+(ip!=0 || Host.LastError()==Err.NotFound));`

---

# L13 MiniCLR 运行时

### L13.1 .cctor 支持 / 编译期 lint（二选一编译开关）
【内核侧】clr.cpp：
```c
// CLS_CCTOR=1 时：clr_load_slot 完成后扫描每个类型的 <cctor> 方法并 exec_method；
// 异常语义（无 try-catch）：任一 cctor fault → 整个 slot 加载失败，Err.ClrFault。
// CLS_CCTOR=0 时：编译期 lint（mex_pack.py --lint-cctor）：发现任何
//   “static 字段带初始化器且非 const” → 拒绝打包，报文件:行。
```
### L13.2 类库补齐（全部走 MiniCLR 支持的语法子集）
```csharp
public final class StrExt {                     // string 能力（编译器内建 + StrSub icall 落地）
    [MethodImpl(InternalCall)] public static extern string Concat(string a, string b);  // 单次分配
    [MethodImpl(InternalCall)] public static extern bool  StartsWith(string s, string p);
    [MethodImpl(InternalCall)] public static extern bool  EndsWith(string s, string p);
    [MethodImpl(InternalCall)] public static extern int   IndexOf(string s, char c);
    [MethodImpl(InternalCall)] public static extern string Substring(string s, int start, int len);
    [MethodImpl(InternalCall)] public static extern int   CompareOrdinal(string a, string b);
}
public final class StringBuilder {
    // 内部 char 缓冲 → 用 int[] 存储（MiniCLR 无 char[] 保证，采用 int[] 每元素一码点/UTF-16 码元）
    private int[] buf; private int len;
    public StringBuilder Init(int cap) { buf = new int[cap]; len = 0; return this; }  // 无 ctor 依赖
    public int Append(string s) { for (int i=0;i<s.Length();i++) Push(s.CharAt(i)); return len; }
    public int AppendInt(int v) { ... }
    public string ToString2() { return Host.StrFromInts(buf, len); }   // icall：int[] → string 单次分配
    [MethodImpl(InternalCall)] static extern string StrFromInts(int[] codes, int len);
}
public final class List {                        // 无泛型：object 版 + Int 版两份
    private object[] items; private int n;
    public List Init(int cap) { items = new object[cap]; n = 0; return this; }
    public int  Add(object o) { if (n==items.Length()) Grow(); items[n]=o; return n++; }
    public object Get(int i) { return items[i]; }
    public int  Count() { return n; }
    public void RemoveAt(int i) { for (int j=i;j<n-1;j++) items[j]=items[j+1]; n--; }
    private void Grow() { object[] t = new object[items.Length()*2]; Copy(t); items = t; }
}
public final class IntList {
    private int[] items; private int n;
    public IntList Init(int cap) { items = new int[cap]; n = 0; return this; }
    public int Add(int v) { if (n==items.Length()) { int[] t=new int[items.Length()*2]; ArrCopy(t); items=t; } items[n]=v; return n++; }
    public int Get(int i) { return items[i]; } public int Count() { return n; }
    public void Set(int i, int v) { items[i]=v; }
}
public final class Dict {                        // 开放寻址 int→object；string 键经 hash icall
    private int[] keys; private object[] vals; private int n;
    [MethodImpl(InternalCall)] public static extern int StrHash(string s);   // 内核侧 FNV-1a
    public Dict Init(int cap) { keys=new int[cap]; vals=new object[cap]; n=0; return this; }
    public void Put(string k, object v) { int i=FindSlot(StrHash(k),k,1); if(i>=0) vals[i]=v; else { Put0(StrHash(k),v); } }
    public object Get(string k) { int i=FindSlot(StrHash(k),k,0); return i>=0 ? vals[i] : null; }
    private int FindSlot(int h, string k, int forPut) { ... }                // 纯托管
}
```
Math/Convert（内核 icall 承载，MiniCLR 侧无浮点 → 全整点运算 + 定点）：
```csharp
public final class MathX {                       // 定点 Q16.16
    [MethodImpl(InternalCall)] public static extern int  Abs(int v);
    [MethodImpl(InternalCall)] public static extern int  Min(int a, int b);
    [MethodImpl(InternalCall)] public static extern int  Max(int a, int b);
    [MethodImpl(InternalCall)] public static extern int  SqrtQ16(int q16);
    [MethodImpl(InternalCall)] public static extern int  DivQ16(int a, int b);
    [MethodImpl(InternalCall)] public static extern int  MulQ16(int a, int b);
}
public final class Convert {
    [MethodImpl(InternalCall)] public static extern int    ToInt(string s);
    [MethodImpl(InternalCall)] public static extern string FromInt(int v);   // StrSub
    [MethodImpl(InternalCall)] public static extern string FromBool(int b);
    [MethodImpl(InternalCall)] public static extern string FromHex(int v);
}
```
try-catch：`CLS_EH=1` 编译开关在 CLR 解释器加异常表解析 + 虚拟 unwind；默认关（现存代码未用 EH）。开关关闭时编译器对 try 块报错（lint 拒绝），杜绝静默吞错。
自测：`StringBuilder sb = new StringBuilder().Init(32); sb.Append("ab"); sb.AppendInt(3); Host.Log("L13 ok="+(sb.ToString2()=="ab3")+" startswith="+(StrExt.StartsWith("abc","ab")));`

---

# L14 测试与工具链

### L14.1 Test 框架（托管侧，纯 MiniCLR）
```csharp
public final class Test {
    private static IntList fails; private static IntList counts; private static string[] names; private static int nNames;
    public static void Init() { fails = new IntList().Init(64); counts = new IntList().Init(64); names = new string[64]; nNames = 0; }   // 无 .cctor：显式 Init
    public static void Begin(string name) { names[nNames] = name; counts.Add(0); fails.Add(0); }
    public static void Check(int cond) { if (cond==0) fails.Set(fails.Count()-1, fails.Get(fails.Count()-1)+1); }
    public static void End() { nNames++; }
    public static int RunAll() {  // 遍历注册用例并输出汇总
        int total=0, bad=0;
        for (int i=0;i<nNames;i++) { total++; if (fails.Get(i)>0) { bad++; Log.Error("test", names[i]+" FAIL "+fails.Get(i)); } }
        Log.I("test", "tests="+total+" fail="+bad);
        return bad;
    }
}
```
用法：`Test.Init(); Test.Begin("L3"); Test.Check(File.Open("/nonexist",1)==0 && Host.LastError()==Err.NotFound); Test.End(); Test.RunAll();`
### L14.2 DebugBreak / StackTrace
```c
// 内核侧
void k_dbg_break(void);                 // 触发 int3 语义：停机打印 + APMS 事件，可继续
int  k_clr_stacktrace(char* out, int cap);   // 序列化当前 CLR 调用栈（g_dbg_mi 链）
```
```csharp
public static class Dbg {
    [MethodImpl(InternalCall)] public static extern int  Break();
    [MethodImpl(InternalCall)] public static extern string StackTrace();   // StrSub
    [MethodImpl(InternalCall)] public static extern int  Assert(int cond, string note); // cond==0 → Dump+Break
}
```
### L14.3 ABI 签名快照与 make abi-check
```python
# tools/abi_extract.py
# 扫描 csharp/NexOS.Forms/*.cs 中所有 [MethodImpl(InternalCall)] 方法 →
# 规范签名行 "Ns.Class.Method(类型列表)->返回"；内核侧扫描 icall 注册表 →
# "icall <名字> <argc>"。合并排序写入 abi_snapshot.txt。
```
```make
# Makefile 追加
abi-snapshot:
	python tools/abi_extract.py > tools/abi_snapshot.txt
abi-check: $(OBJS)
	python tools/abi_extract.py > build/abi_now.txt
	diff build/abi_now.txt tools/abi_snapshot.txt || (echo "ABI CHANGED: bump NEX_ABI_VERSION and refresh snapshot"; exit 1)
```
语义：CI 里 `make abi-check` 在签名漂移时**拒绝构建**（硬指标第 17 条）。改签名 = 必须 `NEX_ABI_VERSION++` + `make abi-snapshot` 显式刷新。
自测：`Dbg.Assert(Cap.Err==0, "caps stable"); Host.Log("L14 ok");`

---

# 17 条验收标准自检表（硬指标）

| # | 指标 | 验证方式 | 判定 |
|---|------|---------|------|
| 1 | 错误码可查 | 每个 API 失败路径均 `set_last_error`；`Host.LastError()` 读回 | L0 自测输出 `err ok=true` |
| 2 | 句柄防悬空 | `HeapFree` 后旧句柄 resolve → Err.BadHandle/Stale | L4 自测 |
| 3 | 边界拒绝生效 | 越权 fs/dev/net icall 返回 Err.PermDenied | `Perm.FsCanPath("/home/user10/x")` false |
| 4 | 前缀段边界 | `bnd_prefix_hit("/home/user10","/home/user1")==0` | L1 自测 hit/deny 双输出 |
| 5 | 换代失效 | `k_boundary_set` 后旧句柄 → Err.Stale | 手测：改边界后旧 File 句柄读失败 |
| 6 | 边界只降不升 | `boundary_subset` 拒绝任何超集 | CreateProcess 单测：子声明父外卷 → Err.PermDenied |
| 7 | syscall 拦截 | `syscall_mask` 位关 → GUARD 拒 | 构造 tok 无 cls_fs → File.Open 失败 PermDenied |
| 8 | 设备边界 | dev_id 白名单外 → 拒 | Dev.Open(Dev.Audio, 9) → PermDenied |
| 9 | 线程/互斥 | MutexNew→Lock→Unlock→Lock 复测 | L2 自测输出 true |
| 10 | 异步 Job 进度与取消 | Job.Start→Progress 递增→Cancel→Status==Cancel | Ai/Download 迁移后原用例通过 |
| 11 | VFS 挂载与 .. 解析 | `/a/b/../../c` → `/c`；越根 → Err.NoEntry | k_vfs_resolve 单测 |
| 12 | O_CREAT\|O_EXCL 返回 NameExists | 第二次 Open → Err.NameExists | L3 自测 |
| 13 | 本地 socket 互通 | 127.0.0.1 Bind/Listen/Connect/Send/Recv 回环 | L5 自测扩展 |
| 14 | 配置持久化 | Set→Flush→重 Open→Get 相等 | L12 自测 |
| 15 | .cctor 生效或 lint 拒绝 | CLS_CCTOR=1 时 cctor 执行；=0 时 mex_pack lint 拒绝非 const 静态初始化 | 两种构建各验一次 |
| 16 | StartsWith/StringBuilder 可用 | L13 自测字符串比较 + 拼接 | 输出 `L13 ok=true true` |
| 17 | make abi-check 拦截签名变更 | 手改一个 icall 参数后 make 失败 | diff 非空 → 构建拒绝 |

## 反面清单逐条落实位置（备忘）
- SetBound*/SetToken/SetCaps：托管 Perm 类全只读（L1.1），无任何写 icall。
- -1 泛指错误：全部 API 走 LastError；句柄 0=无效。
- 裸 int 句柄：所有句柄为打包 h_t / 托管 Handle。
- Paint() 内分配持久对象：L8/L13 均注明事件循环侧分配。
- 无段边界前缀匹配：唯一实现 `bnd_prefix_hit`（L1.2）。
- 省略 ICALL_GUARD：L2–L12 每个 icall 首行 GUARD。
- 子边界超父：CreateProcess 前置 `boundary_subset`。
- 改边界后旧句柄有效：`handle_gen_bump_all_owned`。
- .cctor 静态 string 赋值依赖：Test/StringBuilder 等全部显式 Init()；lint/CLS_CCTOR 双路径。
- 32 位镜像调 SetTextGamma/AudioPlay 无检查：所有 Gfx/Audio 入口前置 `HasCap&&Is64Bit`（托管侧与内核 GUARD 双保险）。
