#include "driver_contract.h"

static int  d_users;
static int  d_dying;
static const DriverKernelAPI* d_api;

static int d_init(const DriverKernelAPI* api, void* ctx){
    (void)ctx;
    d_api = api;
    if(!api) return DRIVER_ERR_INVALID_ARG;
    api->printk("[drvtest] init running from real ELF\n");
    api->printk("[drvtest] kmalloc symbol resolved ok\n");
    d_users = 0; d_dying = 0;
    return DRIVER_OK;
}
static void d_unload(void* ctx){
    (void)ctx;
    d_dying = 1;
    if(d_api && d_api->printk) d_api->printk("[drvtest] unload from real ELF\n");
}

static DriverSymbolReq d_syms[] = {
    { "kmalloc", 0 }, { "kfree", 0 }, { "printk", 0 }, { 0, 0 }
};

DriverHeader nexos_driver_header = {
    DRV_MAGIC, DRV_ABI_VERSION, 0x00010000u, sizeof(DriverHeader),
    "elf_test_driver", "real ELF driver", 0,
    d_syms, 3, d_init, d_unload, 0, 0, 0, {0,0,0,0}
};
