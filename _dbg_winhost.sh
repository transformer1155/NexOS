#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS
mkdir -p /tmp/mcptest
cat > /tmp/mcptest/dbg.cpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nexos_ai.h"
extern "C" void* kmalloc(unsigned int n){ return malloc(n); }
extern "C" int ai_reason(const char*, char* o, int n){ snprintf(o,n,"r"); return 0; }
extern "C" int ai_set_mode(int){ return 0; }
extern "C" int net_http_get(const char*, char* o, int n){ snprintf(o,n,"{}"); return 2; }
extern "C" int kern_fs_read(const char* nm, unsigned char* b, int n){
    FILE* f=fopen(nm[0]=='/'?nm+1:nm,"rb"); if(!f) return -1;
    int r=(int)fread(b,1,n,f); fclose(f); return r; }
extern "C" int browser_navigate(const char*){return 0;}
extern "C" int browser_status(void){return 3;}
extern "C" int browser_get_page(char* b,int n){snprintf(b,n,"x");return 1;}
extern "C" int nexos_mcp_exec(const char*,char* o,int n){snprintf(o,n,"x");return 0;}
extern "C" int nexos_mcp_fs_list(int,char* b,int n){snprintf(b,n,"x");return 1;}
extern "C" int nexos_mcp_fs_read(int,const char*,unsigned char* b,int n){snprintf((char*)b,n,"x");return 1;}
extern "C" int nexos_mcp_fs_write(int,const char*,const unsigned char*,int s){return s;}
extern "C" int nexos_mcp_fs_mkdir(int,const char*){return 0;}
extern "C" int nexos_mcp_fs_remove(int,const char*){return 0;}
extern "C" int nexos_mcp_fs_rename(int,const char*,const char*){return 0;}
#include "nexos_ai.cpp"
int main(int argc, char** argv){
    (void)argc;
    FILE* f=fopen(argv[1],"rb"); if(!f){printf("no file\n");return 1;}
    static char buf[65536]; int n=(int)fread(buf,1,sizeof(buf)-1,f); fclose(f); buf[n]=0;
    parse_apps_json(buf);
    // find winhost
    struct AppRegistry* w = app_registry_find("winhost");
    if(!w){ printf("winhost not found\n"); return 1; }
    printf("winhost: tools=%d\n", w->mcp_server_count);
    for(int i=0;i<w->mcp_server_count;i++){
        printf("  [%d] tool_name=%-22s desc_len=%d schema_first=%c schema_len=%d\n",
               i, w->mcp_server[i]->tool_name ? w->mcp_server[i]->tool_name : "(null)",
               (int)strlen(w->mcp_server[i]->description),
               w->mcp_server[i]->parameters_schema ? w->mcp_server[i]->parameters_schema[0] : 0,
               (int)strlen(w->mcp_server[i]->parameters_schema));
    }
    printf("toolpool_used=%d/%d\n", g_toolpool_used, (int)(sizeof(g_toolpool)/sizeof(g_toolpool[0])));
    printf("strpool_used=%d/%d\n", g_strpool_used, (int)sizeof(g_strpool));
    return 0;
}
EOF
g++ -std=c++17 -w -I. -o /tmp/mcptest/dbg /tmp/mcptest/dbg.cpp
/tmp/mcptest/dbg apps.json
