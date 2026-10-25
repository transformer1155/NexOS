/* Deterministic verification of the h_file_name space-truncation fix.
 *
 * The desktop right-click delete and the file-manager delete BOTH resolve the
 * target file name through mforms.cpp::h_file_name (Host.FileName), then call
 * Host.FileDelete -> gui_cb_remove -> mkfs.remove.  The bug: the OLD code
 * stopped the name at the first space, so "This PC.lnk" became "This" and
 * mkfs.remove("This") silently no-op'd.  The NEW code extends to end-of-line
 * (or to the SFS " (NNNB)" size annotation).  This test compiles BOTH variants
 * verbatim and asserts the NEW one returns the FULL space-bearing name while
 * the OLD one reproduces the original bug, on the exact listing strings the
 * kernel emits (MKFS/Desktop = bare "<name>\n"; SFS = "<name> (NNNB)\n").
 */
#include <stdio.h>
#include <string.h>

/* ---- NEW (fixed) behaviour, copied verbatim from mforms.cpp::h_file_name ---- */
static const char* extract_new(const char* line, char* buf, int bufsize) {
    const char* l = line;
    if (l[0] == '[' && l[1] == 'D' && l[2] == ']') l += 3;
    while (*l == ' ') l++;
    const char* end = l;
    while (end[0]) end++;
    for (const char* p = l; *p; p++) {
        if (*p == '(' && p > l && p[-1] == ' ' &&
            p[1] >= '0' && p[1] <= '9') {
            const char* q = p + 1;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == 'B' && q[1] == ')') { end = p; break; }
        }
    }
    while (end > l && (end[-1] == '\n' || end[-1] == ' ')) end--;
    int len = (int)(end - l);
    if (len >= bufsize) len = bufsize - 1;
    for (int i = 0; i < len; i++) buf[i] = l[i];
    buf[len] = 0;
    return buf;
}

/* ---- OLD (buggy) behaviour, as it was before the fix ---- */
static const char* extract_old(const char* line, char* buf, int bufsize) {
    const char* l = line;
    if (l[0] == '[' && l[1] == 'D' && l[2] == ']') l += 3;
    while (*l == ' ') l++;
    const char* end = l;
    while (end[0] && end[0] != ' ') end++;
    int len = (int)(end - l);
    if (len >= bufsize) len = bufsize - 1;
    for (int i = 0; i < len; i++) buf[i] = l[i];
    buf[len] = 0;
    return buf;
}

typedef struct { const char* raw; const char* expect; } TC;

int main(void) {
    TC cases[] = {
        /* desktop shortcuts (MKFS/Desktop = bare name) */
        {"This PC.lnk\n",      "This PC.lnk"},
        {"Task Mgr.lnk\n",     "Task Mgr.lnk"},
        {"AI Agent.lnk\n",     "AI Agent.lnk"},
        /* file-manager auto-named file (MKFS = bare name, has a space) */
        {"New File.txt\n",     "New File.txt"},
        /* SFS listing carries a " (NNNB)" size suffix */
        {"readme.txt (1234B)\n","readme.txt"},
        {"my doc (4096B)\n",   "my doc"},
    };
    int n = sizeof(cases) / sizeof(cases[0]);
    int fails = 0;
    printf("=== h_file_name verification (NEW = fixed, OLD = buggy) ===\n");
    for (int i = 0; i < n; i++) {
        char bnew[64], bold[64];
        const char* rnew = extract_new(cases[i].raw, bnew, sizeof(bnew));
        const char* rold = extract_old(cases[i].raw, bold, sizeof(bold));
        int ok = (strcmp(rnew, cases[i].expect) == 0);
        if (!ok) fails++;
        printf("[%s] raw=%-22s NEW=%-14s OLD=%-10s %s\n",
               ok ? "PASS" : "FAIL", cases[i].raw, rnew, rold,
               ok ? "" : "(expected NEW to match full name)");
    }
    printf("=== %d/%d passed ===\n", n - fails, n);
    return fails ? 1 : 0;
}
