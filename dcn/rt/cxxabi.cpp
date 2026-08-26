/* C++ ABI stubs so freestanding link succeeds without libstdc++. */
extern "C" {
  __attribute__((weak)) void __cxa_pure_virtual(void){ for(;;){} }
  __attribute__((weak)) int  __cxa_guard_acquire(void* g){ (void)g; return 1; }
  __attribute__((weak)) void __cxa_guard_release(void* g){ (void)g; }
  __attribute__((weak)) void __cxa_guard_abort(void* g){ (void)g; }
  __attribute__((weak)) void* __dso_handle = 0;
}
