// ntfs.cpp - Minimal read + delete NTFS driver for NexOS
//
// Implements Phase 0-4 (mount, MFT parse, attribute walk, runlist decode,
// file read, directory listing) and Phase 7 (logical delete). Built freestanding;
// I/O is delegated to the read/write-sector callbacks set via set_io().
//
// All disk structures are little-endian; x86 access is LE so packed-member
// reads are correct. No libc, no STL, no dynamic allocation.

#include "ntfs.h"

namespace NTFS {

// ---- freestanding-safe primitives (no libc dependency) ----
static void ntfs_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void ntfs_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    for (uint32_t i = 0; i < n; i++) dd[i] = v;
}
static int ntfs_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
// UTF-16LE -> ASCII (best effort). out must hold >= name_length+1 bytes.
static void utf16_to_ascii(const uint8_t* u16, int name_length, char* out) {
    int i = 0;
    for (; i < name_length && i < 255; i++) {
        uint16_t c = (uint16_t)(u16[i*2] | (u16[i*2+1] << 8));
        out[i] = (c < 0x80) ? (char)c : '?';
    }
    out[i] = 0;
}

// ---------------------------------------------------------------- mount
bool Ntfs::mount(uint32_t part_lba) {
    part_start = part_lba;
    read_sector(part_start, sector_buf);          // boot sector is at LBA 0 of partition
    BootSector* bs = (BootSector*)sector_buf;
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) return false;
    if (!(bs->oem[0]=='N' && bs->oem[1]=='T' && bs->oem[2]=='F' && bs->oem[3]=='S'))
        return false;   // OEM id is "NTFS    " (space padded)

    bytes_per_sector    = bs->bytes_per_sector ? bs->bytes_per_sector : 512;
    sectors_per_cluster = bs->sectors_per_cluster ? bs->sectors_per_cluster : 1;
    bytes_per_cluster   = bytes_per_sector * sectors_per_cluster;
    mft_lcn             = (uint32_t)bs->mft_lcn;

    if (bs->clusters_per_record > 0)
        mft_record_size = (uint32_t)bs->clusters_per_record * bytes_per_cluster;
    else
        mft_record_size = (uint32_t)1 << (-(int)bs->clusters_per_record);  // 2^(-n)
    if (mft_record_size < 1024) mft_record_size = 1024;
    mft_record_sectors = (mft_record_size + bytes_per_sector - 1) / bytes_per_sector;
    sector_size = bytes_per_sector;
    mounted = true;
    return true;
}

// ---------------------------------------------------------------- fixup
void Ntfs::apply_fixup(uint8_t* rec, uint32_t rec_size) {
    MftRecord* r = (MftRecord*)rec;
    if (r->usa_offset == 0 || r->usa_count < 2) return;
    uint16_t* usa = (uint16_t*)(rec + r->usa_offset);
    uint16_t usn = usa[0];
    (void)usn; // expected value; we trust the array
    uint32_t nsec = rec_size / bytes_per_sector;
    for (uint32_t i = 0; i < nsec; i++) {
        uint16_t* last = (uint16_t*)(rec + (i + 1) * bytes_per_sector - 2);
        *last = usa[1 + i];
    }
}

// ---------------------------------------------------------------- read_mft_record
bool Ntfs::read_mft_record(uint64_t n, uint8_t* out) {
    if (!mounted) return false;
    uint32_t base = cluster_to_lba(mft_lcn) + (uint32_t)(n * mft_record_sectors);
    for (uint32_t i = 0; i < mft_record_sectors; i++) {
        uint8_t tmp[512];
        read_sector(base + i, tmp);
        ntfs_memcpy(out + i * bytes_per_sector, tmp, bytes_per_sector);
    }
    apply_fixup(out, mft_record_size);
    return (((MftRecord*)out)->signature[0] == 'F' &&
            ((MftRecord*)out)->signature[1] == 'I' &&
            ((MftRecord*)out)->signature[2] == 'L' &&
            ((MftRecord*)out)->signature[3] == 'E');
}

// ---------------------------------------------------------------- find_attr
const uint8_t* Ntfs::find_attr(const uint8_t* rec, uint32_t type,
                               const char* name, uint32_t* out_len) {
    const MftRecord* r = (const MftRecord*)rec;
    const uint8_t* p = rec + r->attr_offset;
    const uint8_t* end = rec + r->bytes_in_use;
    while (p + 4 <= end) {
        const AttrHeader* h = (const AttrHeader*)p;
        if (h->type == 0xFFFFFFFF) break;          // end marker
        if (h->length == 0) break;
        if (h->type == type) {
            if (name == nullptr || h->name_length == 0) {
                if (out_len) *out_len = h->length;
                return p;
            }
        }
        p += h->length;
    }
    return nullptr;
}

// ---------------------------------------------------------------- for_each_run
bool Ntfs::for_each_run(const uint8_t* attr,
                        void (*cb)(uint64_t lcn, uint64_t len, void* u), void* u) {
    const AttrNonResident* nr = (const AttrNonResident*)attr;
    if (!nr->h.non_resident) return false;
    const uint8_t* p = attr + nr->runlist_offset;
    int64_t current = 0;
    while (*p) {
        uint8_t header = *p++;
        int len_bytes = header & 0x0F;
        int off_bytes = (header >> 4) & 0x0F;
        if (len_bytes == 0) break;
        uint64_t len = 0;
        for (int i = 0; i < len_bytes; i++) len |= (uint64_t)(*p++) << (8 * i);
        int64_t off = 0;
        for (int i = 0; i < off_bytes; i++) {
            uint8_t b = *p++;
            off |= (int64_t)b << (8 * i);
        }
        if (off_bytes && (off & ((int64_t)1 << (8 * off_bytes - 1))))
            off -= (int64_t)1 << (8 * off_bytes);     // sign-extend
        current += off;
        if (current > 0 && len > 0) cb((uint64_t)current, len, u);
    }
    return true;
}

// ---------------------------------------------------------------- read_clusters
bool Ntfs::read_clusters(uint32_t start_cluster, uint32_t n, uint8_t* out) {
    for (uint32_t c = 0; c < n; c++) {
        uint32_t lba = cluster_to_lba(start_cluster + c);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            read_sector(lba + s, out);
            out += bytes_per_sector;
        }
    }
    return true;
}

// ---------------------------------------------------------------- read_attr_data
int Ntfs::read_attr_data(const uint8_t* attr, uint8_t* out, uint32_t maxlen) {
    const AttrHeader* h = (const AttrHeader*)attr;
    if (!h->non_resident) {
        const AttrResident* r = (const AttrResident*)attr;
        uint32_t n = r->value_length;
        if (n > maxlen) n = maxlen;
        ntfs_memcpy(out, attr + r->value_offset, n);
        return (int)n;
    }
    const AttrNonResident* nr = (const AttrNonResident*)attr;
    uint64_t remaining = nr->data_size;
    uint32_t got = 0;
    const uint8_t* p = attr + nr->runlist_offset;
    int64_t current = 0;
    while (*p) {
        uint8_t header = *p++;
        int len_bytes = header & 0x0F;
        int off_bytes = (header >> 4) & 0x0F;
        if (len_bytes == 0) break;
        uint64_t len = 0;
        for (int i = 0; i < len_bytes; i++) len |= (uint64_t)(*p++) << (8 * i);
        int64_t off = 0;
        for (int i = 0; i < off_bytes; i++) { uint8_t b = *p++; off |= (int64_t)b << (8 * i); }
        if (off_bytes && (off & ((int64_t)1 << (8 * off_bytes - 1))))
            off -= (int64_t)1 << (8 * off_bytes);
        current += off;
        if (current > 0 && len > 0 && got < maxlen && remaining > 0) {
            uint32_t clusters = (uint32_t)len;
            uint32_t avail = clusters * bytes_per_cluster;
            if (avail > remaining) avail = (uint32_t)remaining;
            if (got + avail > maxlen) avail = maxlen - got;
            if (avail > 0) {
                read_clusters((uint32_t)current, clusters, out + got);
                got += avail;
                remaining -= avail;
            }
        }
    }
    return (int)got;
}

// ---------------------------------------------------------------- read_file
int Ntfs::read_file(uint64_t ref, uint8_t* out, uint32_t maxlen) {
    uint8_t rec[1024];
    if (!read_mft_record(ref, rec)) return 0;
    uint32_t len = 0;
    const uint8_t* attr = find_attr(rec, ATTR_DATA, nullptr, &len);
    if (!attr) return 0;
    return read_attr_data(attr, out, maxlen);
}

// ---------------------------------------------------------------- list_dir
bool Ntfs::list_dir(uint64_t dir_ref,
                    void (*cb)(uint64_t ref, bool is_dir, const char* name, void* u),
                    void* u) {
    uint8_t rec[1024];
    if (!read_mft_record(dir_ref, rec)) return false;
    const MftRecord* r = (const MftRecord*)rec;
    if (!(r->flags & 0x02)) return false;     // not a directory

    uint32_t len = 0;
    const uint8_t* iroot = find_attr(rec, ATTR_INDEX_ROOT, nullptr, &len);
    if (!iroot) return false;
    const AttrResident* rr = (const AttrResident*)iroot;
    const uint8_t* val = iroot + rr->value_offset;
    // IndexHeader starts at val + 0x10
    const uint8_t* ih = val + 0x10;
    uint32_t first_off = *(const uint32_t*)(ih + 0);
    uint32_t idx_flags = *(const uint32_t*)(ih + 0xC);
    const uint8_t* entry = val + first_off;

    uint8_t child[1024];
    while (true) {
        uint32_t elen = *(const uint16_t*)(entry + 8);
        uint32_t eflags = *(const uint32_t*)(entry + 0xC);
        if (elen == 0) break;
        uint64_t refn = *(const uint64_t*)(entry) & 0x0000FFFFFFFFFFFFULL;
        // FILE_NAME attribute begins at entry + 0x10
        const uint8_t* fn = entry + 0x10;
        const AttrResident* fr = (const AttrResident*)fn;
        const uint8_t* fv = fn + fr->value_offset;
        const FileNameValue* fv2 = (const FileNameValue*)fv;
        uint8_t ns = fv2->nspace;
        bool is_dir = false;
        if (ns != 2) {   // skip pure DOS (8.3) duplicates
            if (read_mft_record(refn, child)) {
                const MftRecord* cr = (const MftRecord*)child;
                is_dir = (cr->flags & 0x02) != 0;
            }
            char nm[256];
            utf16_to_ascii(fv + 0x3E, fv2->name_length, nm);
            if (nm[0] && ntfs_strcmp(nm, ".") != 0 && ntfs_strcmp(nm, "..") != 0)
                cb(refn, is_dir, nm, u);
        }
        if (eflags & 0x01) break;     // last entry
        entry += elen;
    }
    (void)idx_flags;
    return true;
}

// ---------------------------------------------------------------- $Bitmap bit ops
// Set/clear cluster bit in the volume $Bitmap (MFT record 6).
bool Ntfs::delete_file(uint64_t ref) {
    if (read_only || !write_sector) return false;
    uint8_t rec[1024];
    if (!read_mft_record(ref, rec)) return false;
    MftRecord* r = (MftRecord*)rec;
    if (!(r->flags & 0x01)) return true;   // already not in use

    // Free clusters of every non-resident attribute in this record.
    const uint8_t* p = rec + r->attr_offset;
    const uint8_t* end = rec + r->bytes_in_use;
    while (p + 4 <= end) {
        const AttrHeader* h = (const AttrHeader*)p;
        if (h->type == 0xFFFFFFFF || h->length == 0) break;
        if (h->non_resident) {
            struct Frec { Ntfs* self; } fr2{this};
            auto real_cb = [](uint64_t lcn, uint64_t len, void* u) {
                auto* self = ((Frec*)u)->self;
                for (uint64_t c = 0; c < len; c++) {
                    uint64_t bit = lcn + c;
                    uint64_t byte_off = bit / 8;
                    uint64_t byte_idx = byte_off / self->bytes_per_cluster;
                    uint8_t bmp[4096];
                    uint8_t brec[1024];
                    if (!self->read_mft_record(MFT_BITMAP, brec)) return;
                    const uint8_t* ba = self->find_attr(brec, ATTR_DATA, nullptr);
                    if (!ba) return;
                    const uint8_t* rp = ba + ((const AttrNonResident*)ba)->runlist_offset;
                    int64_t cur = 0; bool done = false;
                    while (*rp && !done) {
                        uint8_t hdr = *rp++;
                        int lb = hdr & 0xF, ob = (hdr >> 4) & 0xF;
                        if (lb == 0) break;
                        uint64_t ln = 0;
                        for (int i = 0; i < lb; i++) ln |= (uint64_t)(*rp++) << (8*i);
                        int64_t of = 0;
                        for (int i = 0; i < ob; i++) { uint8_t b = *rp++; of |= (int64_t)b << (8*i); }
                        if (ob && (of & ((int64_t)1 << (8*ob-1)))) of -= (int64_t)1 << (8*ob);
                        cur += of;
                        if ((uint64_t)byte_idx < ln) {
                            self->read_clusters((uint32_t)cur + (uint32_t)byte_idx, 1, bmp);
                            bmp[byte_off % self->bytes_per_cluster] &= ~(1u << (bit % 8));
                            uint32_t wlba = self->cluster_to_lba((uint32_t)cur + (uint32_t)byte_idx);
                            for (uint32_t s = 0; s < self->sectors_per_cluster; s++)
                                self->write_sector(wlba + s, bmp + s * self->bytes_per_sector);
                            done = true;
                        }
                        byte_idx -= ln;
                    }
                }
            };
            for_each_run(p, real_cb, &fr2);
        }
        p += h->length;
    }

    // Clear in-use flag and write the record back.
    r->flags &= ~0x01;
    uint32_t base = cluster_to_lba(mft_lcn) + (uint32_t)(ref * mft_record_sectors);
    for (uint32_t i = 0; i < mft_record_sectors; i++)
        write_sector(base + i, rec + i * bytes_per_sector);
    return true;
}

} // namespace NTFS
