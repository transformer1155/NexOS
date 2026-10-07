// fetch-assets.js - fetch the V86 runtime files this page needs.
//
//   node fetch-assets.js
//
// The page needs four files that are not stored in the repository because of
// their size or because they are third-party build artefacts:
//
//   libv86.js      V86 emulator build          (from the npm `v86` package)
//   v86.wasm       V86 WebAssembly core        (from the npm `v86` package)
//   seabios.bin    SeaBIOS ROM                 (from the V86 repository)
//   vgabios.bin    VGA BIOS ROM                (from the V86 repository)
//
// nexos32.img is built locally instead: see `node build-image.js`.
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const OUT = __dirname;
const BIOS_URLS = [
  'https://raw.githubusercontent.com/copy/v86/master/bios/seabios.bin',
  'https://raw.githubusercontent.com/copy/v86/master/bios/vgabios.bin',
];

function fromNpm() {
  const pkg = path.join(OUT, 'node_modules', 'v86', 'build');
  const want = [
    ['libv86.js', path.join(pkg, 'libv86.js')],
    ['v86.wasm', path.join(pkg, 'v86.wasm')],
  ];
  if (!fs.existsSync(pkg)) {
    console.log('installing the v86 npm package …');
    execFileSync('npm', ['install', '--no-save', 'v86'], { cwd: OUT, stdio: 'inherit' });
  }
  for (const [name, src] of want) {
    const dst = path.join(OUT, name);
    fs.copyFileSync(src, dst);
    console.log(`  ${name} <- npm v86 package (${fs.statSync(dst).size} bytes)`);
  }
}

async function fromGithub() {
  for (const url of BIOS_URLS) {
    const name = path.basename(url);
    const dst = path.join(OUT, name);
    if (fs.existsSync(dst)) { console.log(`  ${name} already present`); continue; }
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${url} -> HTTP ${res.status}`);
    const buf = Buffer.from(await res.arrayBuffer());
    fs.writeFileSync(dst, buf);
    console.log(`  ${name} <- ${url} (${buf.length} bytes)`);
  }
}

(async () => {
  console.log('fetching V86 runtime assets into', OUT);
  fromNpm();
  await fromGithub();
  console.log('done.  now run:  node build-image.js');
})().catch((e) => { console.error('FETCH FAILED:', e.message); process.exit(1); });
