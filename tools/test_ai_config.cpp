#include <cstdio>
#include <cstring>
#include "ai_config.h"

static int fails = 0;
static void ck(const char* n, int got, int want){
    int ok = (got == want);
    printf("%-44s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok){ printf("   got %d want %d\n", got, want); fails++; }
}
static void cks(const char* n, const char* got, const char* want){
    int ok = strcmp(got, want) == 0;
    printf("%-44s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok){ printf("   got [%s]\n   want[%s]\n", got, want); fails++; }
}

int main(){
    ai_config_t c;
    char buf[512];

    // defaults
    ck("parse(NULL) rc", ai_config_parse(&c, 0), 0);
    ck("default provider=LOCAL", c.provider == AI_PROVIDER_LOCAL, 1);
    ck("default ready", ai_config_ready(&c), 1);
    ck("default clean", c.dirty, 0);
    ai_config_describe(&c, buf, sizeof(buf));
    ck("default key masked (none)", strstr(buf, "(none)") != 0, 1);

    // deepseek with key
    const char* txt =
        "# comment\n"
        "provider=deepseek\n"
        "key=sk-1234567890abcdef\n"
        "model=deepseek-chat\n"
        "max_tokens=1024\n"
        "temperature=3\n";
    ai_config_parse(&c, txt);
    ck("provider=deepseek", c.provider == AI_PROVIDER_DEEPSEEK, 1);
    ck("model parsed", strcmp(c.model, "deepseek-chat") == 0, 1);
    ck("max_tokens parsed", c.max_tokens == 1024, 1);
    ck("temperature x10", c.temperature_x10 == 3, 1);
    ck("ready with key", ai_config_ready(&c), 1);
    ai_config_describe(&c, buf, sizeof(buf));
    ck("key masked, not leaked", strstr(buf, "1234567890") == 0, 1);
    ck("mask keeps prefix", strstr(buf, "sk-1") != 0, 1);

    // dirty on bad provider
    ai_config_parse(&c, "provider=bogus\n");
    ck("bad provider -> dirty", c.dirty, 1);
    ck("bad provider keeps default", c.provider == AI_PROVIDER_LOCAL, 1);

    // serialize / reparse roundtrip
    ai_config_parse(&c, txt);
    int n = ai_config_serialize(&c, buf, sizeof(buf));
    ck("serialize rc>0", n > 0, 1);
    ai_config_t c2;
    ai_config_parse(&c2, buf);
    ck("roundtrip provider", c2.provider == c.provider, 1);
    ck("roundtrip model", strcmp(c2.model, c.model) == 0, 1);
    ck("roundtrip key", strcmp(c2.key, c.key) == 0, 1);
    ck("roundtrip max_tokens", c2.max_tokens == c.max_tokens, 1);

    // set() validation
    ck("set provider ok", ai_config_set(&c, "provider", "opencode"), 0);
    ck("set provider bad", ai_config_set(&c, "provider", "nope"), -2);
    ck("set max_tokens too small", ai_config_set(&c, "max_tokens", "1"), -2);
    ck("set temperature too big", ai_config_set(&c, "temperature", "99"), -2);
    ck("set unknown field", ai_config_set(&c, "nonsense", "x"), -1);

    // deepseek without key is not ready
    ai_config_parse(&c, "provider=deepseek\n");
    ck("deepseek w/o key not ready", ai_config_ready(&c), 0);

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
