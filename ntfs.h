// ntfs.h - Minimal read + delete NTFS driver for NexOS (freestanding C++)
//
// Scope (per plan): read-only mount, directory traversal, file read, file/dir
// delete. NO write/create, NO format, NO ACL, NO $LogFile replay, NO compression.
//
// Design notes:
//  - All multi-byte fields on disk are little-endian; x86 is LE so direct
//    packed-member access is correct at runtime.
//  - I/O is abstracted through read/write-sector callbacks so the same code
//    runs (a) inside the kernel (backed by ATA) and (b) in a host unit test
//    (backed by an image file).
//  - No dynamic allocation, no STL, no libc: safe under -ffreestanding.

#pragma once
#include <stdint.h>

namespace NTFS {

typedef void (*ReadSectorFn)(uint32_t lba, void* buf);   // buf must be >= 512 bytes
typedef void (*WriteSectorFn)(uint32_t lba, const void* buf);

// ---------------------------------------------------------------- Boot sector
// NTFS BPB. Field offsets follow the on-disk NTFS specification.
struct [[gnu::packed]] BootSector {
    uint8_t  jump[3];               // 0x00
    char     oem[8];                // 0x03  "NTFS    "
    uint16_t bytes_per_sector;      // 0x0B
    uint8_t  sectors_per_cluster;   // 0x0D
    uint16_t reserved_sectors;      // 0x0E
    uint8_t  fat_count;             // 0x0F  (0)
    uint16_t root_entries;          // 0x10  (0)
    uint16_t total_sectors_16;      // 0x12  (0)
    uint8_t  media;                 // 0x14
    uint16_t sectors_per_fat;       // 0x15  (0)
    uint16_t sectors_per_track;     // 0x17
    uint16_t heads;                 // 0x19
    uint32_t hidden_sectors;        // 0x1B
    uint32_t total_sectors_32;      // 0x1F  (0)
    uint32_t reserved1;             // 0x23  (0)
    uint64_t total_sectors_64;      // 0x28
    uint64_t mft_lcn;               // 0x30  MFT start cluster
    uint64_t mft_mirror_lcn;        // 0x38
    int8_t   clusters_per_record;   // 0x40  signed: >=0 -> clusters, <0 -> 2^(-n) bytes
    uint8_t  reserved2[3];          // 0x41
    int8_t   clusters_per_index;    // 0x44  signed
    uint8_t  reserved3[3];          // 0x45
    uint64_t volume_serial;         // 0x48
    uint32_t checksum;              // 0x50
    uint8_t  boot_code[426];        // 0x54
    uint16_t signature;             // 0x1FE 0x55AA
};

// ---------------------------------------------------------------- MFT record
struct [[gnu::packed]] MftRecord {
    char     signature[4];          // 0x00  "FILE" or "BAAD"
    uint16_t usa_offset;           // 0x04  update sequence array offset
    uint16_t usa_count;            // 0x06  +1 = number of usa entries
    uint64_t lsn;                  // 0x08
    uint16_t sequence;             // 0x10
    uint16_t hardlinks;            // 0x12
    uint16_t attr_offset;          // 0x14  first attribute offset
    uint16_t flags;                // 0x16  0x01 in-use, 0x02 directory
    uint32_t bytes_in_use;         // 0x18
    uint32_t bytes_allocated;      // 0x1C
    uint64_t base_ref;             // 0x20
    uint16_t next_attr_id;         // 0x28
    uint16_t align;                // 0x2A
    // usa array begins at usa_offset
};

// Attribute type codes
enum AttrType {
    ATTR_STANDARD_INFO = 0x10,
    ATTR_ATTR_LIST     = 0x20,
    ATTR_FILE_NAME     = 0x30,
    ATTR_OBJECT_ID     = 0x40,
    ATTR_SECURITY     = 0x50,
    ATTR_VOLUME_NAME  = 0x60,
    ATTR_VOLUME_INFO  = 0x70,
    ATTR_DATA         = 0x80,
    ATTR_INDEX_ROOT   = 0x90,
    ATTR_INDEX_ALLOC  = 0xA0,
    ATTR_BITMAP       = 0xB0,
    ATTR_REPARSE      = 0xC0,
    ATTR_EA_INFO      = 0xD0,
    ATTR_EA           = 0xE0,
    ATTR_LOG_UTIL     = 0x100,
};

// Common attribute header (resident & non-resident share this prefix)
struct [[gnu::packed]] AttrHeader {
    uint32_t type;          // 0x00
    uint32_t length;        // 0x04  total attr length (incl. header)
    uint8_t  non_resident;  // 0x08  0 = resident, 1 = non-resident
    uint8_t  name_length;   // 0x09
    uint16_t name_offset;   // 0x0A  relative to attr start
    uint16_t flags;         // 0x0C
    uint16_t instance;      // 0x0E
};

// Resident attribute tail (when non_resident == 0)
struct [[gnu::packed]] AttrResident {
    AttrHeader h;
    uint32_t value_length;  // 0x10
    uint16_t value_offset;  // 0x14  relative to attr start
    uint8_t  indexed;      // 0x16
    uint8_t  pad;          // 0x17
};

// Non-resident attribute tail (when non_resident == 1)
struct [[gnu::packed]] AttrNonResident {
    AttrHeader h;
    uint64_t low_vcn;           // 0x10
    uint64_t high_vcn;          // 0x18
    uint16_t runlist_offset;    // 0x20  relative to attr start
    uint16_t compression_unit;  // 0x22
    uint32_t reserved;          // 0x24
    uint64_t allocated_size;    // 0x28
    uint64_t data_size;         // 0x30
    uint64_t initialized_size;  // 0x38
};

// FILE_NAME attribute value (content of a $FILE_NAME / 0x30 attribute)
struct [[gnu::packed]] FileNameValue {
    uint64_t parent_ref;    // 0x00  low 48 bits = parent MFT record number
    uint64_t ctime;         // 0x08
    uint64_t mft_change;    // 0x10
    uint64_t mft_mod;       // 0x18
    uint64_t access;        // 0x20
    uint64_t alloc_size;    // 0x28
    uint64_t file_size;     // 0x30
    uint32_t dos_flags;     // 0x38
    uint8_t  name_length;   // 0x3C  in chars (UTF-16)
    uint8_t  nspace;        // 0x3D  0=POSIX 1=Win32 2=DOS 3=Win32+DOS
    // 0x3E name[] UTF-16
};

// Well-known MFT record numbers
enum {
    MFT_ROOT = 5,        // root directory
    MFT_BITMAP = 6,      // $Bitmap (volume cluster usage)
    MFT_MFT = 0,         // $MFT itself
};

// ---------------------------------------------------------------- Driver
struct Ntfs {
    ReadSectorFn  read_sector  = nullptr;
    WriteSectorFn write_sector = nullptr;
    uint32_t part_start    = 0;     // partition LBA offset (added to all LBAs)
    int      sector_size   = 512;
    bool     mounted       = false;
    bool     read_only     = false; // if true, delete_file() refuses to write

    // geometry (filled by mount)
    uint32_t bytes_per_sector    = 512;
    uint32_t sectors_per_cluster = 1;
    uint32_t bytes_per_cluster   = 512;
    uint32_t mft_lcn             = 0;
    uint32_t mft_record_size     = 1024;
    uint32_t mft_record_sectors  = 2;

    uint8_t  sector_buf[512];

    void set_io(ReadSectorFn r, WriteSectorFn w) { read_sector = r; write_sector = w; }

    // Phase 0: parse boot sector at part_start, derive geometry.
    bool mount(uint32_t part_lba);

    // Read MFT record #n into out (>= mft_record_size bytes). Applies USN fixups.
    bool read_mft_record(uint64_t n, uint8_t* out);

    // Find first attribute of given type in record; optional name match.
    // Returns pointer into rec, or nullptr. *out_len receives attr length.
    const uint8_t* find_attr(const uint8_t* rec, uint32_t type,
                             const char* name = nullptr, uint32_t* out_len = nullptr);

    // Read full attribute value into out (resident or non-resident).
    int  read_attr_data(const uint8_t* attr, uint8_t* out, uint32_t maxlen);

    // Read entire file (by MFT record number) into out; returns bytes read.
    int  read_file(uint64_t ref, uint8_t* out, uint32_t maxlen);

    // List a directory. cb called per entry.
    bool list_dir(uint64_t dir_ref,
                  void (*cb)(uint64_t ref, bool is_dir, const char* name, void* u),
                  void* u);

    // Delete a file/dir (mark not-in-use + free data clusters in $Bitmap).
    // Returns true if the logical delete succeeded.
    bool delete_file(uint64_t ref);

    // ---- helpers ----
    uint32_t cluster_to_lba(uint32_t cluster) const {
        return part_start + cluster * sectors_per_cluster; // NTFS: cluster 0 holds boot sector
    }
    bool read_clusters(uint32_t start_cluster, uint32_t n, uint8_t* out);
    void apply_fixup(uint8_t* rec, uint32_t rec_size);

    // runlist decode: for each extent call cb(lcn, length_clusters).
    bool for_each_run(const uint8_t* attr,
                      void (*cb)(uint64_t lcn, uint64_t len, void* u), void* u);
};

} // namespace NTFS
