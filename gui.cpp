// =====================================================================
//  gui.cpp  -  GUI translation-unit ENTRY POINT  (Strangler Fig, step 1)
// ---------------------------------------------------------------------
//  The legacy implementation now lives in:
//      src/gui/gui_legacy.cpp
//
//  This file exists so that the build keeps working untouched while that
//  body is split out module by module:
//      step 2  src/gui/gfx_engine.cpp    Graphics primitives + pixel ops
//      step 3  src/gui/gfx_font.cpp      font load / glyph / UTF-8
//      step 4  src/gui/gfx_ime.cpp       pinyin IME state machine
//      step 5  src/gui/browser.cpp       HTML/CSS layout engine
//      step 6  src/gui/gui_core.cpp      window manager + render_all +
//                                        the extern "C" bridge
//
//  RULES FOR THIS FILE:
//    * do NOT add code here -- it only stitches the modules together
//    * the extern "C" bridge signatures must never change (the C# layer
//      links against them)
//    * new modules get their own translation unit under src/gui/
// =====================================================================

#include "src/gui/gui_legacy.cpp"
