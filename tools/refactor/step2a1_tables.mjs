// ---------------------------------------------------------------------------
//  step2a1_tables.mjs - Strangler-Fig Step 2a, phase 1: move the three pure
//  data tables out of gui_legacy.cpp into src/gui/gfx_engine.{h,cpp}.
//
//  WHAT MOVES (294 lines, no code paths):
//      G_TO_LIN[256]      sRGB -> linear LUT   (gamma 2.4)
//      G_TO_SRGB[256]     linear -> sRGB LUT
//      font8x16[256][16]  legacy 8x16 ASCII bitmap font
//
//  WHY THIS PHASE EXISTS FIRST: it exercises the whole mechanism (move + extern
//  declarations in a new header + Makefile prerequisites + .text cmp) on data
//  only, where a mistake cannot change behaviour.  Step 2a phase 2 then moves
//  struct Graphics and needs ~30 linkage changes -- the risky part.
//
//  SAFETY NOTES
//    * NO LINE NUMBERS.  Regions are found by anchor regex + brace matching and
//      every anchor must match EXACTLY ONE line, otherwise the script aborts.
//    * No --apply  -> pure read-only dry run (works on a dirty tree).
//    * --apply     -> refuses if src/gui/ or gui.cpp is dirty.
//    * --apply --verify additionally takes .text baselines, rebuilds BOTH
//      kernels and compares.  If the build FAILS it aborts BEFORE the cmp:
//      a failed build leaves the OLD object in place, so comparing then would
//      "pass" against a stale file.  (That exact trap bit us once.)
//    * AFTER ANY MOVE, run 'make check-k64' and read it.  That gate is not
//      only about symbols bound to 0: during this very phase it caught three
//      real linkage mistakes -- an object missing from the link line, a rule
//      line that had become a multi-target rule (so OBJECT A was built from
//      SOURCE B), and prerequisites appended after '|' (order-only, so edits
//      stopped triggering a rebuild).  Treat its output as part of the move.
// ---------------------------------------------------------------------------
import fs from "node:fs";
import cp from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const APPLY  = process.argv.includes("--apply");
const VERIFY = process.argv.includes("--verify");
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const LEGACY = ROOT + "/src/gui/gui_legacy.cpp";
const HDR    = ROOT + "/src/gui/gfx_engine.h";
const CPP    = ROOT + "/src/gui/gfx_engine.cpp";
const MK     = ROOT + "/Makefile";
const die = (m) => { console.error("ABORT: " + m); process.exit(1); };
const run = (cmd, logfile) => {
  const r = cp.spawnSync("bash", ["-lc", cmd], { cwd: ROOT, encoding: "utf8" });
  if (logfile) fs.writeFileSync(logfile, (r.stdout || "") + (r.stderr || ""));
  return r;
};

const L = fs.readFileSync(LEGACY, "utf8").split("\n");

// ---- preflight -----------------------------------------------------------
const dirty = cp.execSync("cd " + ROOT + " && git status --porcelain -- src/gui/ gui.cpp",
                          { encoding: "utf8" }).trim();
if (dirty) {
  if (APPLY) die("src/gui/ or gui.cpp is dirty -- coordinate first:\n" + dirty);
  console.log("[warn] dirty tree: dry run only (anchors)");
} else console.log("[ok] preflight: clean");

// ---- anchors -------------------------------------------------------------
function findOne(re, what) {
  const hits = []; L.forEach((l, i) => { if (re.test(l)) hits.push(i); });
  if (hits.length !== 1) die("anchor '" + what + "' matched " + hits.length + " lines (need 1)");
  return hits[0];
}
function braceEnd(start) {
  let depth = 0, seen = false;
  for (let i = start; i < L.length; i++) {
    const s = L[i];
    for (let k = 0; k < s.length; k++) {
      const c = s[k];
      if (c === "/" && s[k+1] === "/") break;
      if (c === '"') { k++; while (k < s.length && s[k] !== '"') { if (s[k] === "\\") k++; k++; } continue; }
      if (c === "{") { depth++; seen = true; }
      else if (c === "}") { depth--; if (seen && depth === 0) return i; }
    }
  }
  die("unbalanced braces from L" + (start + 1));
}
const T = {
  lin:  { a: findOne(/^static\s+const\s+uint8_t\s+G_TO_LIN\[/,  "G_TO_LIN"),  name: "G_TO_LIN",  decl: "extern const uint8_t G_TO_LIN[256];"  },
  srgb: { a: findOne(/^static\s+const\s+uint8_t\s+G_TO_SRGB\[/, "G_TO_SRGB"), name: "G_TO_SRGB", decl: "extern const uint8_t G_TO_SRGB[256];" },
  font: { a: findOne(/^const\s+uint8_t\s+font8x16\[/,           "font8x16"),   name: "font8x16",  decl: "extern const uint8_t font8x16[256][16];" },
};
for (const k of Object.keys(T)) T[k].b = braceEnd(T[k].a);

console.log("[ok] anchors:");
for (const k of Object.keys(T)) {
  const t = T[k];
  console.log("   " + t.name.padEnd(12) + "L" + (t.a+1) + "..L" + (t.b+1) + "  (" + (t.b - t.a + 1) + " lines)");
}
const total = Object.values(T).reduce((s, t) => s + (t.b - t.a + 1), 0);
console.log("[plan] total lines moved: " + total);

// ---- build the new files -------------------------------------------------
const slice = (a, b) => L.slice(a, b + 1);
const hdr = [
  "// =====================================================================",
  "//  gfx_engine.h  -  rendering data tables shared by the GUI modules",
  "// ---------------------------------------------------------------------",
  "//  Extracted from gui_legacy.cpp (Strangler Fig, step 2a phase 1).",
  "//  These tables used to have INTERNAL linkage (static / const), so they are",
  "//  re-declared here and defined with EXTERNAL linkage in gfx_engine.cpp.",
  "//  Phase 1 moves data only -- no code path changes, .text is cmp-verified.",
  "// =====================================================================",
  "#ifndef NEXOS_GUI_GFX_ENGINE_H",
  "#define NEXOS_GUI_GFX_ENGINE_H",
  "",
  "#include <stdint.h>",
  "",
  "// sRGB <-> linear (gamma 2.4) lookup tables, used by blend_pixel_lin().",
  T.lin.decl,
  T.srgb.decl,
  "",
  "// Legacy 8x16 ASCII bitmap font (baked); draw_char()/fb console fallback.",
  T.font.decl,
  "",
  "#endif // NEXOS_GUI_GFX_ENGINE_H",
];
const cpp = [
  "// =====================================================================",
  "//  gfx_engine.cpp  -  definitions for the rendering data tables",
  "// ---------------------------------------------------------------------",
  "//  Definitions live here (not in the header) so the tables are emitted",
  "//  exactly once.  extern const gives them external linkage so every TU can",
  "//  reference them through gfx_engine.h.",
  "// =====================================================================",
  '#include "gfx_engine.h"',
  "",
];
for (const k of Object.keys(T)) {
  const blk = slice(T[k].a, T[k].b).slice();
  blk[0] = blk[0].replace(/^static\s+/, "").replace(/^const\s+/, "extern const ");
  cpp.push(...blk, "");
}

// ---- rewrite gui_legacy.cpp ---------------------------------------------
const remove = new Array(L.length).fill(false);
for (const k of Object.keys(T)) for (let i = T[k].a; i <= T[k].b; i++) remove[i] = true;
const out = [];
let includeInserted = false;
for (let i = 0; i < L.length; i++) {
  if (remove[i]) continue;
  out.push(L[i]);
  if (!includeInserted && /^#include\s+"font_vec\.h"/.test(L[i])) {
    out.push("");
    out.push("// ---- rendering data tables (extracted from this file, step 2a-1) ----");
    out.push('#include "gfx_engine.h"');
    includeInserted = true;
  }
}
if (!includeInserted) die('insertion point #include "font_vec.h" not found');
console.log("[plan] gfx_engine.h = " + hdr.length + " lines, gfx_engine.cpp = " + cpp.length + " lines");
console.log("[plan] gui_legacy.cpp: " + L.length + " -> " + out.length + " lines");

// ---- Makefile transforms (prerequisites, link lines, rules, KERNEL64_OBJS) ----
// Three distinct traps this function exists to avoid, all of which bit us once:
//   * appending deps to the END of a rule line puts them AFTER '|' -> order-only
//     prerequisites, so editing the header stops triggering a rebuild
//   * a naive string replace on "$(BUILD)/<something>.o" can hit a RULE
//     definition line and turn it into a multi-target rule (which silently
//     builds OBJECT A from SOURCE B -- gfx_engine64.o ended up containing lg_*)
//   * KERNEL64_OBJS spans continuation lines, so the object must be appended to
//     the LAST fragment, and never to a rule line that happens to name it
function patchMakefile(text) {
  const L = text.split("\n");
  const ch = { prereq: 0, link: 0, rules: 0, k64: 0 };

  // (a) prerequisites for the three gui objects, inserted BEFORE the '|'
  for (let i = 0; i < L.length; i++) {
    const m = /^(\$\(BUILD\)\/(gui|gui64|gui_diag)\.o:)(.*)$/.exec(L[i]);
    if (!m || L[i].includes("gfx_engine")) continue;
    const add = " src/gui/gfx_engine.h src/gui/gfx_engine.cpp";
    const rest = m[3];
    const bar = rest.indexOf("|");
    L[i] = m[1] + (bar >= 0 ? rest.slice(0, bar) + add + " " + rest.slice(bar) : rest + add);
    ch.prereq++;
  }

  // (b) 32-bit link lines (prereq + recipe) need the new object next to gui*.o
  for (let i = 0; i < L.length; i++) {
    const isLink = L[i].startsWith("\t$(LD) $(LDFLAGS)") ||
                   /^\$\(BUILD\)\/kernel(_textboot|_diag|_logo)?\.elf:/.test(L[i]);
    if (!isLink || L[i].includes("$(BUILD)/gfx_engine.o")) continue;
    if (L[i].includes("$(BUILD)/gui.o ")) {
      L[i] = L[i].replace("$(BUILD)/gui.o ", "$(BUILD)/gui.o $(BUILD)/gfx_engine.o "); ch.link++;
    } else if (L[i].includes("$(BUILD)/gui_diag.o ")) {
      L[i] = L[i].replace("$(BUILD)/gui_diag.o ", "$(BUILD)/gui_diag.o $(BUILD)/gfx_engine.o "); ch.link++;
    }
  }

  // (c) the two compile rules, right after the gui.o recipe
  const out2 = [];
  for (let i = 0; i < L.length; i++) {
    out2.push(L[i]);
    if (/^\$\(BUILD\)\/gui\.o:/.test(L[i]) && !text.includes("$(BUILD)/gfx_engine.o:")) {
      // skip past the recipe line(s), then insert
      let j = i + 1;
      while (j < L.length && L[j].startsWith("\t")) { out2.push(L[j]); j++; }
      out2.push("");
      out2.push("# Data tables extracted from gui_legacy.cpp (sRGB LUTs + baked 8x16 font).");
      out2.push("# Both kernels need them, hence a 32-bit and a 64-bit object.");
      out2.push("$(BUILD)/gfx_engine.o: src/gui/gfx_engine.cpp src/gui/gfx_engine.h | $(BUILD)");
      out2.push("\t$(CC) $(CXXFLAGS) -c src/gui/gfx_engine.cpp -o $@");
      out2.push("");
      out2.push("$(BUILD)/gfx_engine64.o: src/gui/gfx_engine.cpp src/gui/gfx_engine.h | $(BUILD)");
      out2.push("\t$(CC64) $(CXX64FLAGS) -c src/gui/gfx_engine.cpp -o $@");
      ch.rules += 2;
      i = j - 1;
    }
  }

  // (d) KERNEL64_OBJS: append to the LAST continuation fragment
  let k = out2.findIndex(l => /^KERNEL64_OBJS\s*:=/.test(l));
  if (k >= 0) {
    let last = k;
    while (out2[last].endsWith("\\") && last + 1 < out2.length) last++;
    if (!out2[last].includes("gfx_engine64.o")) {
      out2[last] += " $(BUILD)/gfx_engine64.o"; ch.k64++;
    }
  }
  return { text: out2.join("\n"), n: ch, ok: ch.prereq === 3 && ch.k64 === 1 && ch.rules === 2 && ch.link >= 4 };
}
const mkPlan = patchMakefile(fs.readFileSync(MK, "utf8"));
console.log("[plan] Makefile: prereq=" + mkPlan.n.prereq + "/3 rules=" + mkPlan.n.rules + "/2 link=" + mkPlan.n.link + " k64=" + mkPlan.n.k64 + (mkPlan.ok ? "  OK" : "  INCOMPLETE"));

if (!APPLY) { console.log("[mode] DRY RUN - nothing written"); process.exit(0); }

// ---- baseline .text (taken BEFORE writing, requires a green build) ------
let baseline = null;
if (VERIFY) {
  console.log("[verify] building BASELINE objects first ...");
  const b = run("make -j4 build/gui.o build/gui64.o", "/tmp/s2a1_base.log");
  if (b.status !== 0) die("baseline build failed -- fix the tree first (see /tmp/s2a1_base.log)");
  baseline = {};
  for (const o of ["gui", "gui64"]) {
    run("objcopy -O binary --only-section=.text build/" + o + ".o /tmp/s2a1_" + o + "_before.bin");
    baseline[o] = fs.readFileSync("/tmp/s2a1_" + o + "_before.bin");
    console.log("[verify] baseline " + o + ".o .text = " + baseline[o].length + " bytes");
  }
}

// ---- write --------------------------------------------------------------
fs.writeFileSync(HDR, hdr.join("\n") + "\n");
fs.writeFileSync(CPP, cpp.join("\n") + "\n");
fs.writeFileSync(LEGACY, out.join("\n"));
console.log("[write] " + HDR + "\n[write] " + CPP + "\n[write] " + LEGACY);

// ---- Makefile: apply all transforms -----------------------------------
{
  const r = patchMakefile(fs.readFileSync(MK, "utf8"));
  if (!r.ok) die("Makefile transform incomplete: prereq=" + r.n.prereq + " rules=" + r.n.rules + " link=" + r.n.link + " k64=" + r.n.k64);
  fs.writeFileSync(MK, r.text);
  console.log("[write] Makefile (prereq=" + r.n.prereq + ", rules=" + r.n.rules + ", link=" + r.n.link + ", k64=" + r.n.k64 + ")");
}

// ---- verify -------------------------------------------------------------
if (!VERIFY) process.exit(0);
console.log("[verify] rebuilding 32-bit ...");
if (run("make -j4", "/tmp/s2a1_b32.log").status !== 0) {
  console.error("*** BUILD FAILED (32-bit).  .text comparison NOT performed: a failed build");
  console.error("*** leaves the OLD object in place and the cmp would falsely 'pass'.");
  process.exit(2);
}
console.log("[verify] rebuilding 64-bit ...");
if (run("make -j4 textboot", "/tmp/s2a1_b64.log").status !== 0) {
  console.error("*** BUILD FAILED (64-bit).  .text comparison NOT performed.");
  process.exit(2);
}
console.log("[verify] build OK -- now comparing .text");
let ok = true;
for (const o of ["gui", "gui64"]) {
  run("objcopy -O binary --only-section=.text build/" + o + ".o /tmp/s2a1_" + o + "_after.bin");
  const after = fs.readFileSync("/tmp/s2a1_" + o + "_after.bin");
  const same = after.equals(baseline[o]);
  console.log("   " + o + ".o .text " + (same ? "IDENTICAL" : "DIFFERS") +
              "  before=" + baseline[o].length + " after=" + after.length);
  if (!same) ok = false;
}
process.exit(ok ? 0 : 3);
