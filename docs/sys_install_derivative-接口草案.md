# sys_install_derivative() 接口草案

> 状态：草案 · 作者：ai-engineer
> 核心原则：**身份来自内核自己的记录，不来自调用者自报**。

## 1. 为什么不能在 syscall 里验签

syscall 是内核入口，它能看到的是寄存器里的指针和数值。所谓「发起进程的签名」在内核里
并不存在：进程不会携带一个可验证的签名，只有「哪个 .mex 被 CLR 加载了」这个事实是
内核亲历的。让 syscall 去验签，等于让内核相信自己没见过的证据。

因此门禁必须建立在**内核已见证的身份**上。这正是 MCP 总线的做法。

## 2. 复用的既有机制（已实现，nexos_ai.cpp）

- MiniCLR 加载 .mex 时，把该文件的 basename 写入 `g_current_app_id`。
- 同时调用 `app_registry_note_resident(app_id)`，把该 id 加入**信任集**。
- `mcp_dispatch_request()` 拒绝任何不在信任集内的调用者。

关键点：managed code **无法**提供自己被检查的身份；它只能传目标、工具名、参数。

## 3. 接口定义

    /* 系统级特权：安装/替换内核衍生包。
     * 返回 0 成功；负值为错误码（见下）。
     * 调用者身份由内核根据当前 .mex 决定，不接受任何参数自报身份。 */
    int sys_install_derivative(struct derivative_install_req* req);

    struct derivative_install_req {
        uint32_t magic;          /* 'DVRQ' */
        uint32_t version;        /* 1 */
        const char* package_path; /* MKFS/SFS 内的路径，只读挂载 */
        uint32_t path_len;
        uint8_t  expected_hash[32]; /* 可选：调用者声明期望的包 SHA-256 */
        uint32_t flags;          /* bit0 = 允许降级, bit1 = 仅校验不安装(dry-run) */
        uint32_t reserved;
    };

## 4. 门禁判定顺序（全部通过才执行）

    1. GATE_SYSCALL_AVAILABLE
       编译期由 MARKET_ROOT_KEY 是否内置决定；未内置 -> 直接 -ENOSYS。
       纯开源构建不暴露这个 syscall 入口（连符号都不导出）。

    2. GATE_CALLER_RESIDENT
       若调用者不在信任集（app_is_resident(g_current_app_id) 为假）-> -EPERM。

    3. GATE_CALLER_IS_MARKET
       信任集里的 id 必须等于内置的市场 .mex basename（如 NexOSMarket）-> 否则 -EPERM。
       这一条把「任何系统 .mex」收窄成「只有市场进程」。

    4. GATE_PACKAGE_CHAIN
       对 package 做双签校验（复用双签草案第 4 节的流程）；
       只接受 flags 含 A+B 且 B 由内置根验证通过的包 -> 否则 -EKEYREJECTED。

    5. GATE_HASH_MATCH
       若调用者提供了 expected_hash，必须与包实际 hash 一致 -> 否则 -EBADMSG。

    6. GATE_SPACE
       校验目标分区/目录剩余空间 -> 不足返回 -ENOSPC（不允许半写）。

## 5. 错误码

| 码 | 含义 |
| --- | --- |
| -ENOSYS | 本构建未启用市场特权 |
| -EPERM | 调用者不是受信市场进程 |
| -EKEYREJECTED | 包未通过双签 |
| -EBADMSG | hash 不匹配或包结构损坏 |
| -ENOSPC | 空间不足 |
| -EBUSY | 已有安装在进行（单实例锁） |
| -EIO | 写入失败（此时必须回滚到旧镜像） |

## 6. 执行语义（原子性）

    1. 取安装锁（单实例，失败 -EBUSY）
    2. 把当前内核复制到备份位置（逻辑 A/B 的第一半）
    3. 把新包写入**非活动**槽位
    4. 校验写入结果的 hash
    5. 标记 next_boot = 新槽位
    6. 注册软件看门狗：若下次启动后 15–20 秒内未收到 shell.mex ready，回滚
    7. 释放锁并返回 0

任何一步失败：删除半成品、恢复 next_boot 标记、返回错误码。**绝不留下半写的镜像。**

## 7. 与 MCP 的关系

短期的市场进程可以用 MCP 工具实现同一功能（`market_install_derivative`），
由 `mcp_dispatch_request` 做身份校验，再调用本 syscall 的同一份内核实现。
这样：

- 身份校验只有一处实现（MCP 信任集）
- syscall 只是「内核侧执行体」，本身也再校验一次（纵深防御）

## 8. 待确认

1. 市场 .mex 的固定 basename 用什么（NexOSMarket？），需要和你确认后写死在内核。
2. 纯开源构建是否保留 syscall 符号但恒返回 -ENOSYS（便于统一测试）—— 我倾向保留符号、返回 -ENOSYS。
3. 备份位置用 SFS 保留目录还是 data 盘（受 435 KiB 余量约束）—— 需要容量评估。
