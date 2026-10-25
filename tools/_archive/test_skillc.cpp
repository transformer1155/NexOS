/* test_skillc.cpp - host harness: compile a .skill with tools/skillc.py, then
 * load+run the resulting .bc through the SAME skill_vm.h the kernel uses.
 * Verifies Phase 4 features (structs, string literals, function calls) without
 * booting QEMU.
 *
 * Build:  g++ -std=c++11 -I plugins tools/test_skillc.cpp -o build/test_skillc
 * Run:    ./build/test_skillc build/demo.bc
 */
#include "skill_vm.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_skillc <file.bc>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* bc = (unsigned char*)malloc((size_t)n);
    if (fread(bc, 1, (size_t)n, f) != (size_t)n) { printf("short read\n"); return 2; }
    fclose(f);

    static SkillVM vm;
    int lr = skill_load(&vm, bc, (int)n);
    if (lr != 0) { printf("skill_load failed: %d\n", lr); return 1; }
    printf("loaded: %d funcs, code_len=%d\n", vm.nf, vm.code_len);

    int out = 0;
    /* prefer main() (matches the kernel reload convention), else func 0 */
    int mf = skill_find(&vm, "main");
    int rr = skill_run(&vm, mf >= 0 ? mf : 0, &out);
    printf("skill_run(%s) -> r=%d out=%d\n", mf >= 0 ? "main" : "0", rr, out);

    /* dump the first few globals + the string pool head for inspection */
    printf("globals: %d %d %d %d\n", vm.globals[0], vm.globals[1], vm.globals[2], vm.globals[3]);
    /* string pool is stored one char per int slot in mem[] */
    printf("str@4096: '");
    for (int i = NBC_STR_BASE; i < NBC_STR_BASE + 16 && vm.mem[i]; i++) putchar((char)vm.mem[i]);
    printf("'\n");

    /* if the program defines calc(a,b,op), call it with 7 and 5 */
    int cf = skill_find(&vm, "calc");
    if (cf >= 0) {
        vm.stk[0] = 7; vm.stk[1] = 5; vm.stk[2] = 0; vm.sp = 3;
        int c0 = 0; skill_run(&vm, cf, &c0);
        vm.stk[0] = 7; vm.stk[1] = 5; vm.stk[2] = 2; vm.sp = 3;
        int c2 = 0; skill_run(&vm, cf, &c2);
        printf("calc(7,5,add)=%d calc(7,5,mul)=%d\n", c0, c2);
    }

    /* report the named plain-text form for the test driver */
    printf("RESULT out=%d g0=%d g1=%d g2=%d g3=%d\n",
           out, vm.globals[0], vm.globals[1], vm.globals[2], vm.globals[3]);
    return 0;
}
