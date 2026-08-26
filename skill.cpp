// =====================================================================
//  skill.cpp  -  P4 minimal AI skill system (implementation)
// ---------------------------------------------------------------------
//  Registry of skills + a heuristic intent matcher.  The agent command
//  (kernel.cpp cmd_agent) calls agent_skill_dispatch() before falling back
//  to the Planner->Actor->Critic pipeline.
//
//  Freestanding build: no libc, no C++ standard library.  All string work
//  is done with the tiny local helpers below.
// =====================================================================

#include "skill.h"

// ---- minimal string helpers (freestanding, case-insensitive search) ----
static int sk_strlen(const char* s){
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int sk_toupper(int c){
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

// case-insensitive substring: returns index of first match, or -1.
static int sk_istr(const char* hay, const char* needle){
    if (!hay || !needle) return -1;
    int hl = sk_strlen(hay), nl = sk_strlen(needle);
    if (nl == 0) return 0;
    if (nl > hl) return -1;
    for (int i = 0; i + nl <= hl; i++){
        int j = 0;
        for (; j < nl; j++){
            if (sk_toupper((unsigned char)hay[i+j]) != sk_toupper((unsigned char)needle[j])) break;
        }
        if (j == nl) return i;
    }
    return -1;
}

// tiny %s/%d formatter (no varargs). s1 may be NULL when unused.
static void build_msg(char* buf, int sz, const char* fmt, const char* s1, int n){
    int bi = 0;
    for (int i = 0; fmt[i] && bi < sz - 1; i++){
        if (fmt[i] == '%' && fmt[i+1] == 's' && s1){
            for (int j = 0; s1[j] && bi < sz - 1; j++) buf[bi++] = s1[j];
            i++; // consume the 's'
        } else if (fmt[i] == '%' && fmt[i+1] == 'd'){
            char tmp[12]; int ti = 0; int v = n;
            if (v < 0){ if (bi < sz-1) buf[bi++] = '-'; v = -v; }
            if (v == 0) tmp[ti++] = '0';
            else { while (v > 0 && ti < 11){ tmp[ti++] = (char)('0' + (v % 10)); v /= 10; } }
            for (int j = ti - 1; j >= 0 && bi < sz - 1; j--) buf[bi++] = tmp[j];
            i++; // consume the 'd'
        } else {
            buf[bi++] = fmt[i];
        }
    }
    buf[bi] = 0;
}

// try a list of candidate phrases; return index of first hit + its length.
static int find_any(const char* s, const char* const* cands, int n, int* matchlen){
    for (int i = 0; i < n; i++){
        int pos = sk_istr(s, cands[i]);
        if (pos >= 0){ if (matchlen) *matchlen = sk_strlen(cands[i]); return pos; }
    }
    return -1;
}

// ---- skill: create_file ----
static const char* const CREATE_VERBS[]  = { "创建文件", "新建文件", "建立文件",
                                             "create file", "make file", "touch file" };
static const char* const CONTENT_MARKS[] = { "内容:", "内容=", "content:", "content=", "text:" };

static char* skill_create_file(char* goal){
    static char result[256];
    int vlen = 0;
    int vpos = find_any(goal, CREATE_VERBS, 6, &vlen);
    if (vpos < 0){
        build_msg(result, sizeof(result), "技能错误: 未识别『创建文件』意图", (const char*)0, 0);
        return result;
    }
    int p = vpos + vlen;
    while (goal[p] == ' ' || goal[p] == '\t') p++;

    // locate optional content marker
    int mlen = 0;
    int cpos = find_any(goal, CONTENT_MARKS, 5, &mlen);

    // filename runs from p up to the next whitespace or the content marker
    int end = p;
    while (goal[end] && goal[end] != ' ' && goal[end] != '\t' && (cpos < 0 || end < cpos))
        end++;
    int fnlen = end - p;
    if (fnlen <= 0){
        build_msg(result, sizeof(result), "技能错误: 未解析到文件名", (const char*)0, 0);
        return result;
    }
    char name[64];
    int k = 0;
    for (; k < fnlen && k < 63; k++) name[k] = goal[p + k];
    name[k] = 0;

    const unsigned char* data = (const unsigned char*)"";
    int len = 0;
    if (cpos >= 0){
        int cp = cpos + mlen;
        while (goal[cp] == ' ' || goal[cp] == '\t') cp++;
        data = (const unsigned char*)(goal + cp);
        len = sk_strlen(goal + cp);
    }

    int r = kern_fs_create(name, data, len);
    if (r >= 0)      build_msg(result, sizeof(result), "已创建文件 %s (%d 字节)", name, r);
    else if (r == -2) build_msg(result, sizeof(result),
                                "创建失败: 数据盘未挂载(请先执行 mkfs 格式化)", (const char*)0, 0);
    else             build_msg(result, sizeof(result), "创建失败 (code %d)", (const char*)0, r);
    return result;
}

// ---- skill registry ----
// Identity / self-introduction: the user wants the AI to answer "who are
// you?" with a proper self-introduction (like Ollama models reply with their
// maker + name), instead of random Markov text.
static char* skill_whoami(char* goal){
    (void)goal;
    static char result[256];
    build_msg(result, sizeof(result),
              "我是 NexOS AI 助手，运行在 NexOS 操作系统的 AI 桌面上。"
              "我可以帮你回答问题、创建文件、执行系统命令。"
              "有什么可以帮你的吗？", (const char*)0, 0);
    return result;
}

Skill g_skills[] = {
    {
        "create_file",
        "创建/新建文件: 创建文件 <名> [内容:文本]",
        skill_create_file,
        0,
        "创建文件;新建文件;建立文件;create file;make file;touch file"
    },
    {
        "whoami",
        "自我介绍/身份查询",
        skill_whoami,
        0,
        "你是谁;你叫什么;你的名字;你是谁啊;介绍一下你自己;介绍一下自己;介绍下自己;"
        "自我介绍一下;你的身份;who are you;what is your name;your name;what are you;"
        "your identity;who r u"
    },
    // Extend here (P4 more skills): 读取/写入/删除/搜索, 启动进程, HTTP 请求...
};
int g_skill_count = (int)(sizeof(g_skills) / sizeof(g_skills[0]));

// ---- dispatch + list ----
int agent_skill_dispatch(const char* goal, char* out, int outsz){
    for (int i = 0; i < g_skill_count; i++){
        const char* kw = g_skills[i].keywords;
        int len = sk_strlen(kw);
        int start = 0;
        for (int s = 0; s <= len; s++){
            if (kw[s] == ';' || kw[s] == 0){
                if (s > start){
                    char tmp[32];
                    int t = 0;
                    for (int x = start; x < s && t < 31; x++) tmp[t++] = kw[x];
                    tmp[t] = 0;
                    if (sk_istr(goal, tmp) >= 0){
                        char* r = g_skills[i].execute((char*)goal);
                        int n = 0;
                        while (r[n] && n < outsz - 1){ out[n] = r[n]; n++; }
                        out[n] = 0;
                        return 1;
                    }
                }
                start = s + 1;
            }
        }
    }
    return 0;
}

void agent_skill_list(char* out, int outsz){
    int n = 0;
    for (int i = 0; i < g_skill_count; i++){
        int room = outsz - n - 1;
        if (room <= 0) break;
        const char* pre = "  ";
        int w = 0;
        for (int j = 0; pre[j] && w < room; j++) out[n + w++] = pre[j];
        for (int j = 0; g_skills[i].name[j] && w < room; j++) out[n + w++] = g_skills[i].name[j];
        if (w < room) out[n + w++] = ' ';
        if (w < room) out[n + w++] = '-';
        if (w < room) out[n + w++] = ' ';
        for (int j = 0; g_skills[i].description[j] && w < room; j++) out[n + w++] = g_skills[i].description[j];
        if (w < room) out[n + w++] = '\n';
        n += w;
    }
    out[n] = 0;
}
