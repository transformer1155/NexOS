// ---------------------------------------------------------------------------
//  step2a2_graphics.mjs - Strangler-Fig Step 2a, phase 2: move struct Graphics
//  out of gui_legacy.cpp into src/gui/gfx_graphics.inc.
//
//  WHY A .inc INSTEAD OF A REAL HEADER:
//    struct Graphics lives inside gui_legacy.cpp's anonymous namespace and its
//    1,522 lines of inline bodies reference ~60 file-scope entities (palette
//    constants, font tables, helper functions, VbeInfo, ...).  Pulling it into
//    a normal header would force every one of those to become externally
//    linked -- 30+ linkage changes, each a chance to change codegen or hit
//    "reference is ambiguous" against the anonymous namespace.
//
//    Including it back at its ORIGINAL position inside the same anonymous
//    namespace keeps every declaration order and every linkage exactly as it
//    is today.  This is a pure textual move: the .text section must come out
//    BYTE-IDENTICAL (verified with tools/refactor/check_text_equiv.sh).
//
//    Step 2b (moving method bodies into a .cpp) is where linkage work belongs,
//    because that step already accepts a codegen change.
//
//  SAFETY
//    * anchors only, exactly-one-match, brace matched
//    * --apply refuses a dirty tree
//    * build first, then compare; a failed build leaves the old object behind
//      and would make any comparison meaningless
//    * afterwards run 'make check-k64'
// ---------------------------------------------------------------------------
import fs from "node:fs";
import cp from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const APPLY = process.argv.includes("--apply");
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const LEGACY = ROOT + "/src/gui/gui_legacy.cpp";
const INC = ROOT + "/src/gui/gfx_graphics.inc";
const MK = ROOT + "/Makefile";
const die = (m) => { console.error("ABORT: " + m); process.exit(1); };

const dirty = cp.execSync("cd " + ROOT + " && git status --porcelain -- src/gui/ gui.cpp Makefile",
                          { encoding: "utf8" }).trim();
if (dirty) { if (APPLY) die("tree dirty:\n" + dirty); console.log("[warn] dirty tree: dry run only"); }
else console.log("[ok] preflight: clean");

const L = fs.readFileSync(LEGACY, "utf8").split("\n");
function findOne(re, what) {
  const hits = []; L.forEach((l, i) => { if (re.test(l)) hits.push(i); });
  if (hits.length !== 1) die("anchor '" + what + "' matched " + hits.length + " (need 1)");
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
const g0 = findOne(/^struct\s+Graphics\s*\{/, "struct Graphics");
const g1 = braceEnd(g0);
const anonOpen = (() => { const h=[]; L.forEach((l,i)=>{ if(/^namespace\s*\{/.test(l)) h.push(i); }); return h[0]; })();
const nsEnd = braceEnd(anonOpen);
if (!(anonOpen < g0 && g1 < nsEnd)) die("struct Graphics is not inside the first anonymous namespace");
console.log("[ok] struct Graphics L" + (g0+1) + "..L" + (g1+1) + " (" + (g1-g0+1) + " lines), anon ns L" + (anonOpen+1) + "..L" + (nsEnd+1));

const body = L.slice(g0, g1 + 1);
const inc = [
  "// =====================================================================",
  "//  gfx_graphics.inc  -  struct Graphics (framebuffer surface + pixel ops)",
  "// ---------------------------------------------------------------------",
  "//  Extracted from gui_legacy.cpp (Strangler Fig, step 2a phase 2).",
  "//",
  "//  This file is #included INSIDE gui_legacy.cpp's anonymous namespace, at the",
  "//  exact position the struct used to occupy.  That is deliberate: the inline",
  "//  bodies reference file-scope state (palette constants, font tables, helper",
  "//  functions) that only exists at that point in that translation unit.",
  "//",
  "//  Consequence: this is a pure textual move -- linkage, declaration order and",
  "//  therefore generated code are unchanged.  .text is verified byte-identical.",
  "//  Moving the bodies out to their own .cpp is step 2b, and that step is judged",
  "//  on performance numbers instead, because it genuinely changes codegen.",
  "//",
  "//  Do NOT include this file from anywhere else, and do NOT add a #pragma once:",
  "//  it is meant to be pasted into one specific namespace.",
  "// =====================================================================",
  "",
  ...body,
  "",
];
const out = [];
for (let i = 0; i < L.length; i++) {
  if (i === g0) {
    out.push("// ---- framebuffer surface + pixel operations (see gfx_graphics.inc) ----");
    out.push('#include "gfx_graphics.inc"');
    i = g1;
    continue;
  }
  out.push(L[i]);
}
console.log("[plan] " + INC + " = " + inc.length + " lines");
console.log("[plan] gui_legacy.cpp: " + L.length + " -> " + out.length + " lines (-" + (L.length - out.length) + ")");

let mk = fs.readFileSync(MK, "utf8");
function patchMakefile(text) {
  const ls = text.split("\n"); let n = 0;
  for (let i = 0; i < ls.length; i++) {
    if (/^\$\(BUILD\)\/(gui|gui64|gui_diag)\.o:/.test(ls[i]) && !ls[i].includes("gfx_graphics.inc")) {
      const bar = ls[i].indexOf("|");
      const add = " src/gui/gfx_graphics.inc";
      ls[i] = bar >= 0 ? ls[i].slice(0, bar) + add + " " + ls[i].slice(bar) : ls[i] + add;
      n++;
    }
  }
  return { text: ls.join("\n"), n };
}
const mkPlan = patchMakefile(mk);
console.log("[plan] Makefile prerequisite lines: " + mkPlan.n + "/3");
if (mkPlan.n !== 3) die("expected 3 Makefile rules");
if (!APPLY) { console.log("[mode] DRY RUN - nothing written"); process.exit(0); }

fs.writeFileSync(INC, inc.join("\n"));
fs.writeFileSync(LEGACY, out.join("\n"));
fs.writeFileSync(MK, mkPlan.text);
console.log("[write] " + INC + "\n[write] " + LEGACY + "\n[write] Makefile");
