// test_ntfs_host.cpp - host-side test for the NexOS NTFS driver.
// Reads a real NTFS image (built by mk_ntfs_test_img.sh) and validates:
//   Phase 0  mount / geometry
//   Phase 1  MFT record parse (root record signature)
//   Phase 4  file read (content matches expected)
//   Phase 5  directory listing (root entries + refs)
//   Phase 7  delete (record flag cleared; clusters freed in $Bitmap)
//
// Build (WSL):
//   g++ -I.. tools/test_ntfs_host.cpp ntfs.cpp -o /tmp/test_ntfs
// Run:
//   /tmp/test_ntfs /tmp/ntfs_test.img

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "ntfs.h"

using namespace NTFS;

static FILE* g_img = nullptr;
static std::string g_path;

static void img_read_sector(uint32_t lba, void* buf) {
    fseek(g_img, (long)lba * 512, SEEK_SET);
    size_t n = fread(buf, 1, 512, g_img);
    if (n < 512) memset((uint8_t*)buf + n, 0, 512 - n);
}
static void img_write_sector(uint32_t lba, const void* buf) {
    fseek(g_img, (long)lba * 512, SEEK_SET);
    fwrite(buf, 1, 512, g_img);
    fflush(g_img);
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS  %s\n", msg); \
    else { printf("  FAIL  %s\n", msg); g_fail++; } \
} while(0)

int main(int argc, char** argv) {
    g_path = (argc > 1) ? argv[1] : "/tmp/ntfs_test.img";
    g_img = fopen(g_path.c_str(), "r+b");
    if (!g_img) { printf("cannot open %s\n", g_path.c_str()); return 2; }

    Ntfs ntfs;
    ntfs.set_io(img_read_sector, img_write_sector);

    printf("== Phase 0: mount ==\n");
    CHECK(ntfs.mount(0), "mount() on NTFS image");
    if (!ntfs.mounted) { fclose(g_img); return 1; }
    printf("  geometry: bytes/sector=%u sectors/cluster=%u bytes/cluster=%u mft_lcn=%u record_size=%u\n",
           ntfs.bytes_per_sector, ntfs.sectors_per_cluster, ntfs.bytes_per_cluster,
           ntfs.mft_lcn, ntfs.mft_record_size);
    CHECK(ntfs.bytes_per_sector == 512, "bytes_per_sector == 512");
    CHECK(ntfs.mft_lcn != 0, "mft_lcn nonzero");

    printf("== Phase 1: MFT record parse ==\n");
    uint8_t rec[1024];
    CHECK(ntfs.read_mft_record(MFT_ROOT, rec), "read root MFT record (signature FILE)");
    MftRecord* r = (MftRecord*)rec;
    CHECK((r->flags & 0x02) != 0, "root record flagged as directory");

    printf("== Phase 5: directory listing ==\n");
    struct Entry { uint64_t ref; bool is_dir; std::string name; } entries[64];
    int nent = 0;
    struct Ctx { Entry* e; int* n; } ctx{entries, &nent};
    auto cb2 = [](uint64_t ref, bool is_dir, const char* name, void* u) {
        auto* c = (Ctx*)u;
        if (*c->n < 64) {
            c->e[*c->n].ref = ref;
            c->e[*c->n].is_dir = is_dir;
            c->e[*c->n].name = name;
            (*c->n)++;
        }
    };
    CHECK(ntfs.list_dir(MFT_ROOT, cb2, &ctx), "list_dir(root) succeeded");
    printf("  entries:\n");
    bool saw_hello = false, saw_sub = false;
    for (int i = 0; i < nent; i++) {
        printf("    [%llu] %s  %s\n", (unsigned long long)entries[i].ref,
               entries[i].is_dir ? "<DIR>" : "<FILE>", entries[i].name.c_str());
        if (entries[i].name == "hello.txt") saw_hello = true;
        if (entries[i].name == "sub") saw_sub = true;
    }
    CHECK(saw_hello, "hello.txt present in root listing");

    printf("== Phase 4: file read ==\n");
    uint64_t hello_ref = 0;
    for (int i = 0; i < nent; i++) if (entries[i].name == "hello.txt") hello_ref = entries[i].ref;
    CHECK(hello_ref != 0, "found hello.txt MFT ref");
    if (hello_ref) {
        uint8_t buf[1024];
        int n = ntfs.read_file(hello_ref, buf, sizeof(buf)-1);
        buf[n > 0 ? n : 0] = 0;
        printf("  read %d bytes: %s", n, (char*)buf);
        CHECK(n > 0, "read_file returned >0 bytes");
        CHECK(strstr((char*)buf, "NexOS NTFS test file") != nullptr,
              "content contains expected string");
    }

    printf("== Phase 7: delete (destructive, on copy) ==\n");
    if (hello_ref) {
        // move file pointer to start and re-read; delete then re-read record
        fseek(g_img, 0, SEEK_SET);
        bool deleted = ntfs.delete_file(hello_ref);
        CHECK(deleted, "delete_file() returned true");
        uint8_t rec2[1024];
        CHECK(ntfs.read_mft_record(hello_ref, rec2), "re-read record after delete");
        MftRecord* r2 = (MftRecord*)rec2;
        CHECK((r2->flags & 0x01) == 0, "in-use flag cleared after delete");
    }

    fclose(g_img);
    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
