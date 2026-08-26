// Host-side debug harness for gguf_loader.c (compiled with MinGW g++, not the
// freestanding kernel toolchain).  Verifies the parser against the real
// gguf_fixture.h without a QEMU round-trip.
#include <cstdio>
#include <cstdint>
#include "gguf_loader.h"
#include "file_adapter.h"
#include "gguf_fixture.h"

int main(){
    printf("fixture size = %llu\n", (unsigned long long)k_gguf_fixture_size);
    gguf_io* mio = gguf_io_mem_create(k_gguf_fixture, k_gguf_fixture_size);
    if(!mio){ printf("mem io FAIL\n"); return 1; }
    gguf_ctx ctx;
    int rc = gguf_load(mio, &ctx);
    printf("rc=%d version=%u tensor_count=%llu kv_count=%llu\n",
           rc, ctx.version, (unsigned long long)ctx.tensor_count, (unsigned long long)ctx.kv_count);
    printf("arch=[%s] quant=[%s] block=%u embed=%u head=%u kv=%u vocab=%u bos=%u eos=%u\n",
           ctx.arch, ctx.quant, ctx.block_count, ctx.embed_length,
           ctx.head_count, ctx.head_count_kv, ctx.vocab_size, ctx.bos_id, ctx.eos_id);
    printf("alignment=%llu tensor_data_offset=%llu n_tensors=%d\n",
           (unsigned long long)ctx.alignment, (unsigned long long)ctx.tensor_data_offset, ctx.n_tensors);
    for(int i=0;i<ctx.n_tensors;i++)
        printf("  tensor[%d] name=[%s] nd=%u type=%u off=%llu\n",
               i, ctx.tensors[i].name, ctx.tensors[i].n_dims,
               ctx.tensors[i].type, (unsigned long long)ctx.tensors[i].offset);
    return 0;
}
