# AI/TLS 模块的 64 位接线（可直接套用）

> 状态：待应用。**没有写进 Makefile**，因为该文件是共享路径，
> 应用前请确认 `pgrep -a make` 为空（同一时刻只允许一个 make）。

## 1. 追加编译规则（放在 64 位区域，建议紧跟 tls_x50964.o 之后）

    # ---- AI 模块（64 位专属；32 位镜像余量约 350 B，一律不加） ----
    $(BUILD)/ai_config64.o: ai_config.cpp ai_config.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c ai_config.cpp -o $@
    $(BUILD)/ai_tooluse64.o: ai_tooluse.cpp ai_tooluse.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c ai_tooluse.cpp -o $@
    $(BUILD)/ai_context64.o: ai_context.cpp ai_context.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c ai_context.cpp -o $@
    $(BUILD)/ai_usage64.o: ai_usage.cpp ai_usage.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c ai_usage.cpp -o $@
    $(BUILD)/ai_kb64.o: ai_kb.cpp ai_kb.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c ai_kb.cpp -o $@
    $(BUILD)/ai_redact64.o: ai_redact.cpp ai_redact.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c ai_redact.cpp -o $@
    $(BUILD)/proc_isolation64.o: proc_isolation.cpp proc_isolation.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c proc_isolation.cpp -o $@

## 2. 加入 KERNEL64_OBJS

在 `tls_x50964.o` 之后插入：

    $(BUILD)/ai_config64.o $(BUILD)/ai_tooluse64.o $(BUILD)/ai_context64.o
    $(BUILD)/ai_usage64.o $(BUILD)/ai_kb64.o $(BUILD)/ai_redact64.o
    $(BUILD)/proc_isolation64.o

（写在一行，反斜杠续行）

## 3. 32 位：一行都不加

- 不给 32 位任何新规则
- 之前为 TLS 加的那 4 条 32 位规则是无引用的惰性规则，可留可删
- 32 位 AI 通路维持宿主 gateway，不动

## 4. 应用后的自检顺序

    1. pgrep -a make        # 必须为空
    2. make                 # 只看真退出码（vm-run make 已修好）
    3. nm build/kernel64.elf | grep -E 'ai_tooluse_run|ai_kb_add|sandbox_create'
       期望：三个符号都存在
    4. nm build/kernel.elf  | grep -cE 'ai_tooluse_run|sandbox_create'
       期望：0（32 位不得包含）
    5. vm-run test-log      # 启动健全性，不测性能

## 5. 已知风险

- proc_isolation.cpp 依赖 extern 声明的 pmm_alloc_page / pmm_free_page /
  paging_map_in / paging_read_cr3 / paging_write_cr3 / kmalloc / kfree。
  若 VMM 没有导出这些名字，**链接会失败**——这是接线的第一道坎，
  需要按 VMM 实际导出的符号改名（ai_engine 里有类似封装可参考）。
- ai_kb 与 knowledge_base.c 是两套实现。若两者同时进 64 位，
  会出现两个知识库互不知情。建议：ai_kb 主 64 位，knowledge_base 留 32 位。
## 6. 分两阶段执行（依据 VMM 符号实测结果）

我核对了 proc_isolation 需要的 VMM 符号，结论一半对一半：

    存在:      pmm_alloc_page()          kernel.cpp:3377  extern "C" uint32_t
    不存在:    vmm_map_page()            kernel.cpp:2359 / kernel64.cpp:1542  static
               read_cr3() / write_cr3()  kernel.cpp:2147,2150                  static inline

文件内 static 无法跨翻译单元链接，所以 proc_isolation 现在**编得过、链不过**。

### 阶段一（本次构建槽可做，零风险）

6 个只依赖既有符号的模块：

    ai_config / ai_tooluse / ai_context / ai_usage / ai_kb / ai_redact

它们的新增静态占用很小，主要成本在 ai_kb 的条目池（约 24 KB .bss）。
**注意 0.5 GiB 内存机器上的预算**：若 kernel.bin 余量告急，先只接前 4 个，
ai_kb / ai_redact 放阶段一之后。

### 阶段二（等两个条件，单独排期）

    proc_isolation.cpp
      条件 1: kernel.cpp 与 .attic64/kernel64.cpp 各导出 4 个薄包装
              （vmm_map_page / read_cr3 / write_cr3 / 释页）—— 需归属人点头
      条件 2: VA 布局策略 —— 现有 vmm_map_page 的参数是 uint32_t，
              不接受我原本假设的 0x8000000000 高位地址。
              我倾向改成由调用方传入 VA 范围，模块不假设地址布局。

两个条件未满足前，**不要把 proc_isolation64.o 加进 KERNEL64_OBJS**，
否则链接会因未定义符号失败。
