// build-image.js - assemble the bootable NexOS 32-bit disk image for V86.
//
// Layout (must match bootloader/README.md):
//   LBA 0        boot.bin       (stage 1, 512 bytes)
//   LBA 1..32    stage2.bin     (stage 2, fixed 16 KiB)
//   LBA 33..     kernel.bin     (32-bit flat kernel)
//
// Plus a partition table entry in the MBR: V86 derives the emulated drive
// geometry from it (see copy/v86 get_disk_geometry), and without one the BIOS
// reports an all-zero geometry through INT 13h AH=08h.
//
// The image is always created in one pass with explicit offsets, then every
// region is read back and verified, so a mis-built image fails loudly instead
// of booting into mystery zeros.
const fs = require('fs');
const path = require('path');

const repoBuild = path.resolve(__dirname, '..', '..', 'bootloader', 'build');
const outDir = __dirname;
const outFile = path.join(outDir, 'nexos32.img');

const IMAGE_SIZE = 4 * 1024 * 1024; // matches the Makefile's `truncate -s 4M`

const parts = [
  { name: 'boot.bin', lba: 0, file: path.join(repoBuild, 'boot.bin') },
  // stage2_v86.bin is built from bootloader/tools/stage2_v86.asm; it is the
  // V86-safe Stage 2 (single-sector reads, ESI preserved across INT 13h, video
  // mode left alone).  Fall back to the stock stage2.bin if it is absent.
  { name: 'stage2.bin', lba: 1, file: pickStage2() },
  { name: 'kernel.bin', lba: 33, file: path.join(repoBuild, 'kernel.bin') },
];

function pickStage2() {
  const v86 = path.join(repoBuild, 'stage2_v86.bin');
  return fs.existsSync(v86) ? v86 : path.join(repoBuild, 'stage2.bin');
}

// Add a single MBR partition table entry covering the whole image.  V86 reads
// this to derive the emulated disk geometry; with no partition table the BIOS
// reports an all-zero geometry via INT 13h AH=08h.
function addPartitionTable(image) {
  const totalSectors = image.length / 512;
  const p = 446;
  image[p + 0] = 0x80;              // bootable
  image[p + 1] = 0x00;              // start head
  image[p + 2] = 0x01;              // start sector 1
  image[p + 3] = 0x00;              // start cylinder low
  image[p + 4] = 0x83;              // type: Linux
  image[p + 5] = 0xFE;              // end head
  image[p + 6] = 0xFF;              // end sector/cylinder
  image[p + 7] = 0xFF;
  image.writeUInt32LE(1, p + 8);                    // start LBA
  image.writeUInt32LE(totalSectors - 1, p + 12);    // sector count
}

function build() {
  const image = Buffer.alloc(IMAGE_SIZE, 0);
  const written = [];

  for (const p of parts) {
    if (!fs.existsSync(p.file)) throw new Error('missing input: ' + p.file);
    const data = fs.readFileSync(p.file);
    const offset = p.lba * 512;
    if (offset + data.length > IMAGE_SIZE) {
      throw new Error(`${p.name} does not fit at LBA ${p.lba}`);
    }
    data.copy(image, offset);
    written.push({ ...p, size: data.length, offset, head: data.subarray(0, 4) });
  }

  fs.mkdirSync(outDir, { recursive: true });
  addPartitionTable(image);
  fs.writeFileSync(outFile, image);

  // Verify from disk, not from memory.
  const check = fs.readFileSync(outFile);
  const problems = [];

  if (check.length !== IMAGE_SIZE) problems.push(`size is ${check.length}, expected ${IMAGE_SIZE}`);
  if (!(check[510] === 0x55 && check[511] === 0xAA)) problems.push('missing 0x55AA boot signature at LBA 0');

  for (const w of written) {
    if (w.lba === 0) continue;   // handled separately: the MBR is patched below
    const got = check.subarray(w.offset, w.offset + w.size);
    const stored = fs.readFileSync(w.file);
    if (!got.equals(stored)) problems.push(`${w.name} mismatch at LBA ${w.lba}`);
    if (got.equals(Buffer.alloc(w.size, 0))) problems.push(`${w.name} region is all zeros at LBA ${w.lba}`);
  }

  // The partition table lives inside LBA 0, so compare only the bytes the
  // boot sector itself owns (the partition table starts at offset 446).
  const bootPart = written.find((w) => w.lba === 0);
  if (bootPart) {
    const storedBoot = fs.readFileSync(bootPart.file);
    const codeLen = Math.min(446, storedBoot.length);
    if (!check.subarray(0, codeLen).equals(storedBoot.subarray(0, codeLen))) {
      problems.push('boot.bin boot code changed at LBA 0 (bytes 0..445)');
    }
    if (check.readUInt32LE(446 + 12) === 0) problems.push('partition table entry is empty');
  }

  console.log(`image: ${outFile} (${check.length} bytes)`);
  for (const w of written) {
    console.log(
      `  LBA ${String(w.lba).padStart(3)} + ${String(w.size).padStart(7)} bytes  ${w.name.padEnd(11)}` +
      ` head=${[...w.head].map((b) => b.toString(16).padStart(2, '0')).join(' ')}`
    );
  }
  if (problems.length) {
    console.error('VERIFY FAILED:\n  - ' + problems.join('\n  - '));
    process.exit(1);
  }
  console.log('verify: OK (all regions present and correct)');
}

build();
