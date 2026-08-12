/* smallest possible PE32+ that touches our loader's API surface */
typedef unsigned long long u64; typedef unsigned int u32;
__declspec(dllimport) int  __stdcall MessageBoxA(void*,const char*,const char*,u32);
__declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
int PeMain(void){ OutputDebugStringA("spike alive\n"); return 42; }
