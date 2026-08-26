// =====================================================================
//  skill.h  -  P4 minimal AI skill system
// ---------------------------------------------------------------------
//  A Skill maps a natural-language intent to a system API call.  The
//  `agent` command tries the skill registry first; on a match it executes
//  the skill directly, otherwise it falls back to the Planner->Actor->
//  Critic pipeline (see ai_engine.cpp).
//
//  This keeps the doc's P4 Skill shape (name/description/execute/
//  requires_confirmation) and adds `keywords` (case-insensitive trigger
//  phrases, ';'-separated) used by the intent matcher.
// =====================================================================
#ifndef NEXOS_SKILL_H
#define NEXOS_SKILL_H

struct Skill {
    char  name[32];
    char  description[256];
    char* (*execute)(char* args);   // args = original goal; returns static result string
    int   requires_confirmation;    // 1 => would prompt Y/N (P8); MVP auto-runs
    const char* keywords;           // trigger phrases, ';'-separated, case-insensitive
};

extern Skill g_skills[];
extern int   g_skill_count;

// Try to satisfy `goal` with a registered skill.
//   returns 1 if a skill handled it (result written to out, NUL-terminated)
//   returns 0 if no skill matched (caller should fall back to the pipeline)
int  agent_skill_dispatch(const char* goal, char* out, int outsz);

// List registered skills into out, one "name - description" per line.
void agent_skill_list(char* out, int outsz);

// Kernel-side bridge: create/overwrite a file on the writable MKFS volume.
//   returns >=0 on success (bytes written), <0 on error (e.g. -2 not mounted)
int  kern_fs_create(const char* name, const unsigned char* data, int len);

#endif // NEXOS_SKILL_H
