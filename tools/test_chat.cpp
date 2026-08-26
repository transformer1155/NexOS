// tools/test_chat.cpp - host harness for the ChatML chat template in
// gguf_infer.cpp.  Verifies the prompt-wrapping logic (gap #4 fix) without
// needing a real model: the formatting helpers don't touch the weight blob.
//
// Build (MSYS2 / native g++):
//   g++ -O2 -I. -DGGUF_HOST_TEST -c gguf_infer.cpp        -o build/h_infer.o
//   g++ -O2 -I.                  -c gguf.cpp              -o build/h_gguf.o
//   g++ -O2 -I. -DGGUF_HOST_TEST -c tools/test_chat.cpp   -o build/h_chat.o
//   g++ build/h_infer.o build/h_gguf.o build/h_chat.o -o build/test_chat
//   ./build/test_chat

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "gguf.h"
#include "gguf_infer.h"

// ---- hooks the freestanding engine expects ---------------------------
extern "C" void gguf_host_log(const char* s){ fputs(s, stdout); }
extern "C" void* big_alloc(uint32_t){ return 0; }
extern "C" void  big_free(void*, uint32_t){}

static int contains(const char* hay, const char* needle){
    return strstr(hay, needle) != 0;
}

int main(){
    const char* sys = "You are NexOS, a helpful AI assistant running on a tiny OS.";
    const char* roles[1] = { "user" };
    const char* conts[1] = { "What is NexOS?" };

    char buf[8192];
    int n = qwen_format_chat(sys, roles, conts, 1, buf, sizeof(buf));

    printf("=== ChatML prompt (%d chars) ===\n%s\n=== end ===\n", n, buf);

    bool ok = (n > 0)
            && contains(buf, "<|im_start|>system\n")
            && contains(buf, sys)
            && contains(buf, "<|im_start|>user\n")
            && contains(buf, "What is NexOS?")
            && contains(buf, "<|im_end|>\n")
            && contains(buf, "<|im_start|>assistant\n");

    // Also verify a multi-turn build (system + 2 turns).
    const char* r2[2] = { "user", "assistant" };
    const char* c2[2] = { "Hi", "Hello! How can I help?" };
    char buf2[8192];
    int n2 = qwen_format_chat(sys, r2, c2, 2, buf2, sizeof(buf2));
    printf("\n=== multi-turn (%d chars) ===\n%s\n=== end ===\n", n2, buf2);
    bool ok2 = (n2 > 0)
             && contains(buf2, "<|im_start|>user\nHi<|im_end|>\n")
             && contains(buf2, "<|im_start|>assistant\nHello! How can I help?<|im_end|>\n")
             && contains(buf2, "<|im_start|>assistant\n");   // generation prompt at the end

    bool all_ok = ok && ok2;
    printf("\n%s  single-turn=%s multi-turn=%s\n",
           all_ok ? "PASS" : "FAIL",
           ok ? "ok" : "BAD", ok2 ? "ok" : "BAD");
    return all_ok ? 0 : 1;
}
