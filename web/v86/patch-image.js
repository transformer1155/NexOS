// patch-image.js <image> <lba> <file> [<lba> <file> ...]
//
// Overwrites regions of an existing disk image at explicit LBA offsets.
// Written because a FileStream opened with OpenWrite starts at position 0,
// so poking a component into an image silently destroyed the boot sector.
const fs = require('fs');

const [, , imagePath, ...rest] = process.argv;
if (!imagePath || rest.length === 0 || rest.length % 2 !== 0) {
  console.error('usage: node patch-image.js <image> <lba> <file> [<lba> <file> ...]');
  process.exit(2);
}

const imageSize = fs.statSync(imagePath).size;
const fd = fs.openSync(imagePath, 'r+');

try {
  for (let i = 0; i < rest.length; i += 2) {
    const lba = Number(rest[i]);
    const file = rest[i + 1];
    const data = fs.readFileSync(file);
    const offset = lba * 512;

    if (!Number.isInteger(lba) || lba < 0) throw new Error('bad LBA: ' + rest[i]);
    if (offset + data.length > imageSize) {
      throw new Error(`${file} (${data.length} bytes) does not fit at LBA ${lba}`);
    }

    fs.writeSync(fd, data, 0, data.length, offset);

    // Read back and confirm.
    const got = Buffer.alloc(data.length);
    fs.readSync(fd, got, 0, data.length, offset);
    if (!got.equals(data)) throw new Error(`verify failed for ${file} at LBA ${lba}`);
    console.log(`patched LBA ${lba} <- ${file} (${data.length} bytes)  verified`);
  }

  // The boot signature must survive every patch.
  const sig = Buffer.alloc(2);
  fs.readSync(fd, sig, 0, 2, 510);
  if (sig[0] !== 0x55 || sig[1] !== 0xAA) throw new Error('boot signature 0x55AA is missing after patching');
  console.log('boot signature 0x55AA: present');
} finally {
  fs.closeSync(fd);
}
