# =====================================================================
#  mono_port/Makefile - freestanding 32-bit Mono port for MiniOS
# ---------------------------------------------------------------------
#  Phase 0 of the Mono/CLR route-B port.
#
#    libmono_port.a   PAL (freestanding libc/libm/POSIX) + eglib core
#    libmono_utils.a  curated mono/utils layer (the interpreter's floor)
#
#  Build hygiene: -nostdinc is deliberate.  Without it gcc happily reaches
#  into /usr/include and the port "works" on the host while being
#  unbuildable for MiniOS.  Only the gcc-provided freestanding headers
#  (stdint/stddef/stdarg/stdbool) plus pal/include are visible, so every
#  missing piece shows up as a hard error instead of a hidden host
#  dependency.
# =====================================================================

MONO      := ../vendor/mono
MONOROOT  := ../vendor
EGLIB     := $(MONO)/eglib
PALINC    := pal/include

CC        := gcc
GCCINC    := $(shell $(CC) -m32 -print-file-name=include)

# Common flags for anything compiled against the PAL.
BASEFLAGS := -m32 -ffreestanding -nostdinc -isystem $(GCCINC) -O0 -g \
             -I$(PALINC) -I$(EGLIB) \
             -DG_OS_UNIX -DUSE_GCC_ATOMIC_OPS

CFLAGS    := $(BASEFLAGS) -Wall -Wno-unused -Wno-implicit-function-declaration

# Upstream trim switches.  Every one of these is a real Mono configure
# option, not a macro we invented:
#
#   DISABLE_SOCKETS - MiniOS has no network stack.  This is the right knob
#                     rather than "just don't compile w32socket.c", because it
#                     also removes the System.Net.Sockets block from
#                     icall-def.h, so the icall table has no dangling entries.
#   DISABLE_ATTACH  - the runtime-attach listener wants a Unix domain socket.
#                     attach.c ships an #else branch with no-op stubs.
#   DISABLE_CRASH_REPORTING
#                   - configure.ac:1196 does exactly this
#                     (CFLAGS += -DDISABLE_CRASH_REPORTING=1).  It empties
#                     utils/mono-state.c + utils/mono-merp.c *and* the call
#                     sites in metadata/icall.c, metadata/threads.c and
#                     mini/mini-exceptions.c, so both sides stay in sync.
#                     Crash dumps want mmap'd shared memory and a Mach/MERP
#                     reporter -- neither exists on MiniOS.
#
# These MUST be given to every Mono layer (utils AND metadata AND sgen AND
# interp), never to just some of them: several of them gate struct members
# (e.g. MonoThreadInfo grows a crash-reporting field), so a mismatch between
# two translation units is silent ABI corruption rather than a link error.
TRIMFLAGS := -DDISABLE_SOCKETS -DDISABLE_ATTACH -DDISABLE_CRASH_REPORTING=1

# mono/utils additionally needs the vendor root on the include path so that
# `#include <mono/utils/...>` resolves.
MONOFLAGS := $(BASEFLAGS) -I$(MONOROOT) -I$(MONO) -w $(TRIMFLAGS)

RTFLAGS := $(MONOFLAGS)

# The GC selection macro is per-directory, exactly as upstream has it:
#
#   configure.ac:4985           SGEN_DEFINES="-DHAVE_SGEN_GC"
#   metadata/Makefile.am:431    libmonoruntimesgen_la_CFLAGS = $(SGEN_DEFINES) ...
#   sgen/Makefile.am:67         libmonosgen_la_CFLAGS        = $(SGEN_DEFINES)
#
# It must NOT go into pal/include/config.h, because mini/mini.h:70 says
#
#   #if defined(HAVE_BOEHM_GC) || defined(HAVE_SGEN_GC)
#   #error "The code in mini/ should not depend on these defines."
#
# i.e. mini/ (and therefore mini/interp/) is deliberately GC-agnostic and
# refuses to compile if it can see the macro.  Without the macro, though,
# every sgen/*.c and metadata/sgen-mono.c compiles to a 0-symbol object
# file without a single warning.  Hence: give it to meta+sgen, never interp.
SGEN_DEFINES := -DHAVE_SGEN_GC

# ---------------------------------------------------------------------
# PAL
# ---------------------------------------------------------------------
PAL_SRC := \
	pal/libc_impl.c \
	pal/libc_posix.c \
	pal/libm_impl.c \
	pal/setjmp_i386.c

# Curated eglib core (data structures + strings + markup + unicode).
EGLIB_CORE := \
	$(EGLIB)/garray.c \
	$(EGLIB)/gbytearray.c \
	$(EGLIB)/gerror.c \
	$(EGLIB)/ghashtable.c \
	$(EGLIB)/glist.c \
	$(EGLIB)/gmarkup.c \
	$(EGLIB)/gmem.c \
	$(EGLIB)/gptrarray.c \
	$(EGLIB)/gqsort.c \
	$(EGLIB)/gqueue.c \
	$(EGLIB)/gslist.c \
	$(EGLIB)/gstr.c \
	$(EGLIB)/gstring.c \
	$(EGLIB)/gutf8.c \
	$(EGLIB)/gshell.c \
	$(EGLIB)/gpattern.c \
	$(EGLIB)/gpath.c \
	$(EGLIB)/goutput.c \
	$(EGLIB)/gunicode.c \
	$(EGLIB)/giconv.c \
	$(EGLIB)/gmisc-unix.c \
	$(EGLIB)/gfile.c \
	$(EGLIB)/gfile-posix.c \
	$(EGLIB)/gfile-unix.c \
	$(EGLIB)/gdir-unix.c \
	$(EGLIB)/gdate-unix.c \
	$(EGLIB)/gtimer-unix.c \
	$(EGLIB)/gmodule-unix.c \
	$(EGLIB)/gclock-nanosleep.c

# ---------------------------------------------------------------------
# mono/utils subset
# ---------------------------------------------------------------------
#  Was hand-picked ("whatever the interpreter seemed to pull in"), which
#  left ~120 unresolved symbols.  Now it is `libmonoutils_la_SOURCES` out of
#  utils/Makefile.am, evaluated for Unix/x86 by tools/gen_srclist.py:
#
#      python3 tools/gen_srclist.py utils        -> 87 files
#
#  The wrong-platform members of that list (mono-*-windows.c, -darwin.c,
#  -wasm.c, mach-support-x86.c ...) are kept on purpose: upstream compiles
#  them too, they just end in MONO_EMPTY_SOURCE_FILE() and contribute one
#  dummy symbol each.  Dropping them would make our list diverge from
#  upstream for no benefit.
#
#  Five subtractions, all sockets:
#    networking.c networking-posix.c networking-fallback.c
#    networking-missing.c networking-windows.c
#  MiniOS has no network stack (DISABLE_SOCKETS).  These five have no upstream
#  guard, so they have to be dropped by name.
#
#  mono-networkinterfaces.c is NOT one of them despite the name -- it reads
#  /proc/net/dev, needs no socket header, and metadata/mono-perfcounters.c
#  calls mono_networkinterface_list() unconditionally.  It stays in.
UTILS_NAMES := \
	dlmalloc os-event-unix mono-md5 mono-sha1 mono-logger mono-codeman \
	mono-counters mono-dl mono-dl-windows mono-dl-darwin mono-dl-posix \
	mono-dl-wasm mono-experiments mono-log-windows mono-log-common \
	mono-log-posix mono-log-android mono-log-darwin \
	mono-log-flight-recorder mono-merp mono-state mono-internal-hash \
	mono-io-portability monobitset mono-filemap mono-mmap \
	mono-mmap-windows mono-mmap-wasm mono-os-mutex mono-flight-recorder \
	mono-proclib mono-proclib-windows mono-publib mono-jemalloc \
	mono-time strenc mono-uri mono-poll mono-path mono-stdlib \
	mono-property-hash mono-value-hash mono-error mono-context \
	hazard-pointer lock-free-queue lock-free-alloc lock-free-array-queue \
	mono-linked-list-set mono-threads mono-threads-state-machine \
	mono-threads-posix mono-threads-posix-signals mono-threads-mach \
	mono-threads-mach-helper mono-threads-windows mono-threads-linux \
	mono-threads-freebsd mono-threads-netbsd mono-threads-openbsd \
	mono-threads-android mono-threads-haiku mono-threads-aix \
	mono-threads-wasm mono-threads-coop mono-utility-thread mono-tls \
	mono-utils-debug atomic mono-hwcap bsearch mono-signal-handler \
	mono-conc-hashtable json mono-rand mono-rand-windows memfuncs parse \
	checked-build mach-support-x86 mono-hwcap-x86 \
	mono-networkinterfaces mono-math-c

UTILS_SRC := $(patsubst %,$(MONO)/utils/%.c,$(UTILS_NAMES))

# ---------------------------------------------------------------------
# metadata -- the runtime proper
# ---------------------------------------------------------------------
#  This list is not "whatever compiles".  It is `libmonoruntimesgen_la_SOURCES`
#  out of metadata/Makefile.am, evaluated for Unix/x86/SGen by
#  tools/gen_srclist.py, so we build exactly what upstream builds.
#  Re-generate with:  python3 tools/gen_srclist.py metadata
#
#  One deliberate subtraction from that list:
#    w32socket-unix.c   - has no DISABLE_SOCKETS guard (upstream always
#                         compiles it) and pulls in the whole BSD socket
#                         header family.  Nothing outside w32socket*.c
#                         references mono_w32socket_*, verified by grep, so
#                         dropping it leaves no unresolved symbol.
#
#  w32socket.c is NOT subtracted.  An earlier revision dropped it believing
#  DISABLE_SOCKETS made it an empty TU -- it does not.  w32socket.c:2645 is an
#  `#else` arm that still defines mono_network_init/mono_network_cleanup as
#  empty functions, and appdomain.c:368/530 calls them unconditionally.
#  Dropping the file left those two symbols dangling.
#
#  mono-config-dirs.c is an addition.  Upstream keeps it in its own
#  convenience library (metadata/Makefile.am:125) purely so it can carry four
#  private -D flags, so it is absent from libmonoruntimesgen_la_SOURCES -- but
#  its mono_config_get_*_dir() are linked into the same libmono.  See the
#  dedicated rule below for the flags.
META_NAMES := \
	console-unix w32mutex-unix w32semaphore-unix w32event-unix \
	w32process-unix w32process-unix-osx w32process-unix-bsd \
	w32process-unix-haiku w32process-unix-default \
	w32file-unix w32file-unix-glob w32error-unix threadpool-worker-default \
	appdomain assembly-load-context domain assembly attach \
	class class-init class-accessors cominterop coree \
	debug-helpers debug-mono-symfile debug-mono-ppdb environment exception \
	w32file filewatcher icall image jit-info loader \
	loaded-images loaded-images-global loaded-images-netcore locales \
	lock-tracer marshal mempool metadata metadata-verify method-builder \
	mono-basic-block mono-config mono-debug mono-endian mono-hash \
	mono-conc-hash mono-mlist mono-perfcounters mono-route \
	object opcodes property-bag w32process profiler rand remoting runtime \
	mono-security security-core-clr security-manager string-icalls sysmath \
	threads threadpool threadpool-io verify dynamic-stream \
	file-mmap-posix file-mmap-windows metadata-cross-helpers \
	seq-points-data handle w32handle-namespace w32handle \
	reflection dynamic-image sre sre-encode sre-save custom-attrs \
	fdhandle callspec abi native-library icall-table w32socket \
	mono-config-dirs \
	method-builder-ilgen marshal-ilgen sgen-mono-ilgen \
	gc-stats gc monitor \
	sgen-bridge sgen-old-bridge sgen-new-bridge sgen-tarjan-bridge \
	sgen-toggleref sgen-stw sgen-mono

META_SRC := $(patsubst %,$(MONO)/metadata/%.c,$(META_NAMES))

# ---------------------------------------------------------------------
# sgen -- the collector itself (25 files, the whole directory)
# ---------------------------------------------------------------------
SGEN_NAMES := \
	sgen-alloc sgen-cardtable sgen-debug sgen-descriptor sgen-fin-weak-hash \
	sgen-gc sgen-gchandles sgen-gray sgen-hash-table sgen-internal \
	sgen-layout-stats sgen-los sgen-marksweep sgen-memory-governor \
	sgen-nursery-allocator sgen-pinning-stats sgen-pinning \
	sgen-pointer-queue sgen-array-list sgen-protocol sgen-qsort \
	sgen-simple-nursery sgen-split-nursery sgen-thread-pool sgen-workers

SGEN_SRC := $(patsubst %,$(MONO)/sgen/%.c,$(SGEN_NAMES))

# ---------------------------------------------------------------------
# mini/interp -- the reason this whole port exists
# ---------------------------------------------------------------------
#  mini/Makefile.am: interp_sources = interp.c mintops.c transform.c
#  (whitebox.c is a test harness, not part of the runtime.)
INTERP_NAMES := interp mintops transform
INTERP_SRC   := $(patsubst %,$(MONO)/mini/interp/%.c,$(INTERP_NAMES))

BUILD := build

PAL_OBJS    := $(patsubst pal/%.c,$(BUILD)/pal_%.o,$(PAL_SRC))
EGLIB_OBJS  := $(patsubst $(EGLIB)/%.c,$(BUILD)/eglib_%.o,$(EGLIB_CORE))
UTILS_OBJS  := $(patsubst %,$(BUILD)/utils_%.o,$(UTILS_NAMES))
META_OBJS   := $(patsubst %,$(BUILD)/meta_%.o,$(META_NAMES))
SGEN_OBJS   := $(patsubst %,$(BUILD)/sgen_%.o,$(SGEN_NAMES))
INTERP_OBJS := $(patsubst %,$(BUILD)/interp_%.o,$(INTERP_NAMES))

OBJS  := $(EGLIB_OBJS) $(PAL_OBJS)
LIB   := $(BUILD)/libmono_port.a
ULIB  := $(BUILD)/libmono_utils.a
MLIB  := $(BUILD)/libmono_meta.a
GLIB_ := $(BUILD)/libmono_sgen.a
ILIB  := $(BUILD)/libmono_interp.a

all: $(LIB)

# 全量：P0.4 的交付物
runtime: config-check $(LIB) $(ULIB) $(MLIB) $(GLIB_) $(ILIB)
	@echo "---------------------------------------------"
	@echo "PAL+eglib : $(words $(OBJS)) objs"
	@echo "utils     : $(words $(UTILS_OBJS)) objs"
	@echo "metadata  : $(words $(META_OBJS)) objs"
	@echo "sgen      : $(words $(SGEN_OBJS)) objs"
	@echo "interp    : $(words $(INTERP_OBJS)) objs"

$(LIB): $(OBJS)
	ar rcs $@ $(OBJS)
	@echo "BUILT $(LIB): $(words $(OBJS)) objects"

$(ULIB): $(UTILS_OBJS)
	ar rcs $@ $(UTILS_OBJS)
	@echo "BUILT $(ULIB): $(words $(UTILS_OBJS)) objects"

$(MLIB): $(META_OBJS)
	ar rcs $@ $(META_OBJS)
	@echo "BUILT $(MLIB): $(words $(META_OBJS)) objects"

$(GLIB_): $(SGEN_OBJS)
	ar rcs $@ $(SGEN_OBJS)
	@echo "BUILT $(GLIB_): $(words $(SGEN_OBJS)) objects"

$(ILIB): $(INTERP_OBJS)
	ar rcs $@ $(INTERP_OBJS)
	@echo "BUILT $(ILIB): $(words $(INTERP_OBJS)) objects"

utils:    $(ULIB)
metadata: $(MLIB)
sgen:     $(GLIB_)
interp:   $(ILIB)

$(BUILD)/eglib_%.o: $(EGLIB)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pal_%.o: pal/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/utils_%.o: $(MONO)/utils/%.c | $(BUILD)
	$(CC) $(MONOFLAGS) -c $< -o $@

$(BUILD)/meta_%.o: $(MONO)/metadata/%.c | $(BUILD)
	$(CC) $(RTFLAGS) $(SGEN_DEFINES) -c $< -o $@

# mono-config-dirs.c is the one metadata file with private -D flags
# (metadata/Makefile.am:128).  Without them every mono_config_get_*_dir()
# falls through to its `#else return NULL;` arm, and the assembly loader then
# searches a NULL path.  The values are MiniOS VFS paths, not host paths.
# This rule is more specific than the pattern above, so make prefers it.
CONFIG_DIRS_FLAGS := \
	-DMONO_BINDIR=\"/bin/\" \
	-DMONO_ASSEMBLIES=\"/lib/mono\" \
	-DMONO_CFG_DIR=\"/etc\" \
	-DMONO_RELOC_LIBDIR=\"../lib\"

$(BUILD)/meta_mono-config-dirs.o: $(MONO)/metadata/mono-config-dirs.c | $(BUILD)
	$(CC) $(RTFLAGS) $(SGEN_DEFINES) $(CONFIG_DIRS_FLAGS) -c $< -o $@

$(BUILD)/sgen_%.o: $(MONO)/sgen/%.c | $(BUILD)
	$(CC) $(RTFLAGS) $(SGEN_DEFINES) -c $< -o $@

# NOTE: no $(SGEN_DEFINES) here.  See the comment next to its definition.
$(BUILD)/interp_%.o: $(MONO)/mini/interp/%.c | $(BUILD)
	$(CC) $(RTFLAGS) -I$(MONO)/mini -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

# ---------------------------------------------------------------------
# Smoke test: link the library into a 32-bit static -nostdlib ELF and run
# it.  Its console/exit stubs use `int 0x80` with the Linux numbering,
# which happens to match the MiniOS ring-3 ABI (SYS_WRITE=4 fd1 -> serial,
# SYS_EXIT=1), so the very same binary is the Phase 0.5 ring-3 vehicle.
# ---------------------------------------------------------------------
SMOKE := $(BUILD)/eglib_smoke.elf

$(BUILD)/eglib_smoke.o: tests/eglib_smoke.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SMOKE): $(BUILD)/eglib_smoke.o $(LIB)
	ld -m elf_i386 -static -nostdlib -e _start -o $@ $(BUILD)/eglib_smoke.o $(LIB)

MSMOKE := $(BUILD)/libm_smoke.elf

$(BUILD)/libm_smoke.o: tests/libm_smoke.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(MSMOKE): $(BUILD)/libm_smoke.o $(LIB)
	ld -m elf_i386 -static -nostdlib -e _start -o $@ $(BUILD)/libm_smoke.o $(LIB)

JSMOKE := $(BUILD)/setjmp_smoke.elf

# -O2 是故意的：setjmp 的坑几乎全在优化器身上（返回两次的控制流 +
# 被缓存在寄存器里的局部变量），-O0 下测不出来。
$(BUILD)/setjmp_smoke.o: tests/setjmp_smoke.c | $(BUILD)
	$(CC) $(filter-out -O0,$(CFLAGS)) -O2 -c $< -o $@

$(JSMOKE): $(BUILD)/setjmp_smoke.o $(LIB)
	ld -m elf_i386 -static -nostdlib -e _start -o $@ $(BUILD)/setjmp_smoke.o $(LIB)

# Phase 0.4 gate: links against libmono_utils.a, so it proves the utils
# layer is not just compilable but actually usable.  $(LIB) comes after
# $(ULIB) because utils pulls eglib/PAL, not the other way round.
USMOKE := $(BUILD)/utils_smoke.elf

$(BUILD)/utils_smoke.o: tests/utils_smoke.c | $(BUILD)
	$(CC) $(MONOFLAGS) -c $< -o $@

$(USMOKE): $(BUILD)/utils_smoke.o $(ULIB) $(LIB)
	ld -m elf_i386 -static -nostdlib -e _start -o $@ $(BUILD)/utils_smoke.o $(ULIB) $(LIB)

smoke: $(SMOKE) $(MSMOKE) $(JSMOKE) $(USMOKE)

test: $(SMOKE) $(MSMOKE) $(JSMOKE) $(USMOKE)
	@./$(SMOKE)
	@./$(MSMOKE)
	@./$(JSMOKE)
	@./$(USMOKE)

# Compilability survey over a whole Mono subdirectory (utils/metadata/...).
probe-%:
	@bash tools/probe.sh $*

# config.h 的自相矛盾（后面的 #undef 干掉前面的 #define）不会有任何
# 编译警告，只会表现成"头补了却还是找不到"。所以每次 make runtime 前查。
config-check:
	@bash tools/check_config.sh

# 跨库未定义符号核查：ar 成功 != 能链接。
audit:
	@bash tools/link_audit.sh

# utils/ 不是自洽的底层（mono-error.c 要构造 MonoException，
# mono-threads-posix.c 要 mono_gc_pthread_create）。这个目标算出
# 在 mini/ 还没进来之前哪些 utils 对象能独立链接。
# 带符号参数时改为查这些符号的依赖闭包：
#     make leafcheck ARGS="mono_bitset_new mono_md5_get_digest"
leafcheck:
	@bash tools/leafcheck.sh $(ARGS)

clean:
	rm -rf $(BUILD)
	rm -f $(EGLIB)/*.o

.PHONY: all clean smoke test utils runtime metadata sgen interp \
        config-check audit leafcheck
