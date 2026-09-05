# NexOS 项目协作规则

> 本文件是**强制约束**。任何对本项目的修改（无论由人还是 AI 助手完成）都必须遵守。
> 仓库：`https://github.com/transformer1155/NexOS.git`（remote `origin`，分支 `master`）

## 一、版本控制（最高优先级）

1. **任何更改都必须提交 git。** 不允许把改动长时间留在未提交的工作区。
2. **每个逻辑改动一次提交。** 不要把无关改动混进同一个 commit。
3. **推送到 GitHub。** 本地提交后需 `git push origin master`；若推送失败，必须明确告知用户，不能当作已完成。
4. **禁止** 使用 `--force` 推送到 `master`；禁止 `git checkout --` 丢弃他人/未备份的工作。
5. 提交信息建议格式：
   ```
   <类型>(<范围>): <一句话说明>
   ```
   类型：`feat` / `fix` / `refactor` / `docs` / `build` / `test` / `chore`。
   例：`feat(distnet): 内核侧分片推理编排器`

## 二、改动前的安全要求（血泪教训）

> 2026-09-05 事故：一个按行号替换的脚本因 **0 基/1 基索引混淆**，误删
> `distnet.cpp` 约 500 行；由于 agent 子系统**从未提交 git**，源码无法恢复，
> 最终只能重写。以下规则由此而来。

1. **改大文件（>300 行）禁止盲写。** 若必须用脚本按行号修改：
   - 先用**唯一字符串锚点**定位，并在写入前**校验锚点出现次数 == 1**；
   - 打印将要删除/插入的**首行与末行以及行数**预览；
   - 确认后再写入。
2. **删除/覆盖文件前先备份**：`cp <file> /tmp/<file>.bak` 或先 `git add` 建立索引。
3. **重要功能必须先提交再继续开发。** 新写一个子系统后立刻 `git add` + `commit`，
   不要等到"做完再说"。
4. 不确定某个改动是否安全时，**先问**，不要凭猜测执行。

## 三、构建与验证

1. 构建在 **WSL（Debian）** 中进行：
   ```bash
   wsl -d Debian -e bash -lc "cd /mnt/d/MyOS/bootloader && make BUILD=/home/<user>/nb"
   ```
   产物目录指向 WSL 本地盘，可规避 WSL2 DrvFs 偶发"大文件写不进去/产物消失"。
   完成后拷回：`cp ~/nb/os_v2.img build/os_v2.img`。
2. **改内核前先跑容量自检**：
   ```bash
   bash tools/check_k64_fit.sh
   ```
3. 镜像布局涉及多处硬编码，**必须同步修改**，否则会静默错位：
   - `kernel.cpp`：`KERNEL64_SECTORS`、`SFS_ALT_LBA`、`SFS_LINUX_LBA`
   - `Makefile`：`SFS_LBA`、`LINUX_SFS_LBA`
   - 约束：`2048 + KERNEL64_SECTORS <= SFS_LBA`，且 32 位镜像
     `kernel.bin <= 588800` 字节（EBDA 上限 `0x9FC00`）。

## 三之二、仓库体积与第三方依赖

1. **生成物禁止入库**（`.gitignore` 已配置，勿回退）：
   - 镜像/光盘/文件系统：`*.img` `*.iso` `*.vhd` `*.vhdx` `*.qcow2` `*.sfs`
   - 编译产物：`build/` `*.o` `*.a` `*.elf` `*.bin` `*.map`
   - npm 依赖：`node_modules/`
2. **第三方依赖用 `git submodule` 引用**，不要把源码/压缩包塞进本仓库：
   ```bash
   git submodule add <外部仓库URL> vendor/<名称>
   git submodule update --init --recursive
   ```
   当前已从历史清除的第三方大包：`vendor/mono.tar.gz`（如需 mono，请改为 submodule）。
3. **若历史里混入大文件，GitHub 会直接拒绝推送**（`GH001` / 超过 100MB）。
   处理办法（`filter-repo` 会改写提交哈希，操作前先打备份标签）：
   ```bash
   git tag backup-before-purge
   git filter-repo --invert-paths --path build/ --path vendor/mono.tar.gz --force
   git remote add origin https://github.com/transformer1155/NexOS.git   # filter-repo 会移除 remote
   git push origin master:master
   ```
   2026-09-05 实绩：pack **514 MB → 61.8 MB**，推送随即成功。

## 四、架构约束（易踩坑）

1. **内核只实现 NE2000 ISA 网卡驱动**，不支持 virtio-net。
   启动必须 `-device ne2k_isa,...`，否则 `netinfo` 的 MAC 全 `FF`，网络功能全废。
2. **64 位文本 shell 不接收串口输入**，因此只能在 64 位编译的功能无法交互式调用。
3. **SLIRP（`-netdev user`）下宿主无法主动发 UDP 给 guest**，只有 guest 发起、
   宿主回包才通。所以分布式调度/推理的**编排器必须跑在 guest 侧**。
   多 VM 广播发现需要 `tools/nexos_l2hub.py` 做 L2 帧洪泛。
4. **Windows UDP 陷阱**：给已挂掉的对端发过 UDP 后，下一次 `recvfrom()` 会抛
   `WSAECONNRESET`。收包循环必须捕获后 `continue`，否则节点一挂，进程就"失聪"。

## 五、文档

1. 新增/修改功能后，同步更新 `README.md`（根目录）与相关子目录 README。
2. 已知限制要写进文档，不能只写在对话里。

## 六、提交前自检清单

- [ ] `bash tools/check_k64_fit.sh` 通过
- [ ] 32 位与 64 位均编译通过（无 error）
- [ ] 若改动镜像布局，四处 LBA/扇区已同步
- [ ] README 已更新
- [ ] `git status` 中无遗漏文件
- [ ] 已 `git commit`，并已 `git push origin master`
