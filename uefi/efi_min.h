/* =====================================================================
 *  efi_min.h  -  Minimal UEFI headers (gnu-efi-free)
 * ---------------------------------------------------------------------
 *  Sufficient to build the NexOS UEFI loader (bootuefi.c) WITHOUT the
 *  gnu-efi package.  Compile the loader objects with:
 *
 *    x86_64-elf-gcc -mabi=ms -fshort-wchar -ffreestanding \
 *        -fno-stack-protector -fno-pic -mno-red-zone \
 *        -fvisibility=hidden -fPIC -fno-plt -Iuefi/gf_inc -Iuefi ...
 *
 *  The structs below follow the UEFI 2.x specification field order
 *  EXACTLY, because the firmware fills them in place; a single wrong
 *  field offset would make every BootServices call jump to garbage.
 * ===================================================================== */
#ifndef EFI_MIN_H
#define EFI_MIN_H

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef UINT64              UINTN;
typedef signed char         INT8;
typedef short               INT16;
typedef int                 INT32;
typedef long long           INT64;
typedef INT64               INTN;
typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;
typedef void               *EFI_HANDLE;
typedef void               *EFI_EVENT;
typedef UINTN               EFI_STATUS;
typedef UINT16              CHAR16;
typedef UINT8               BOOLEAN;

#define VOID      void
#define CONST     const
#define IN
#define OUT
#define OPTIONAL
#define VOLATILE  volatile
#define NULL       ((void *)0)

/* We compile the whole translation unit with -mabi=ms, so the Microsoft
 * x64 calling convention is the default for every function (including
 * firmware callbacks).  EFIAPI is therefore a no-op here. */
#ifndef EFIAPI
#define EFIAPI
#endif

/* ---- Status codes ---- */
#define EFI_SUCCESS                0ULL
#define EFI_ERROR(a)               (((INTN)(a)) < 0)
#define EFI_LOAD_ERROR             0x8000000000000001ULL
#define EFI_INVALID_PARAMETER      0x8000000000000002ULL
#define EFI_UNSUPPORTED            0x8000000000000003ULL
#define EFI_BAD_BUFFER_SIZE        0x8000000000000004ULL
#define EFI_BUFFER_TOO_SMALL       0x8000000000000005ULL
#define EFI_NOT_READY              0x8000000000000006ULL
#define EFI_DEVICE_ERROR           0x8000000000000007ULL
#define EFI_WRITE_PROTECTED        0x8000000000000008ULL
#define EFI_OUT_OF_RESOURCES       0x8000000000000009ULL
#define EFI_VOLUME_CORRUPTED       0x800000000000000aULL
#define EFI_NOT_FOUND              0x800000000000000eULL
#define EFI_ACCESS_DENIED          0x800000000000000fULL
#define EFI_TIMEOUT                0x8000000000000012ULL
#define EFI_ABORTED                0x8000000000000021ULL

/* ---- EFI_GUID ---- */
typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Pixel formats ---- */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3,
    PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                     Version;
    UINT32                     HorizontalResolution;
    UINT32                     VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT  PixelFormat;
    EFI_PIXEL_BITMASK          PixelInformation;
    UINT32                     PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* ---- Graphics Output Protocol ---- */
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber,
    UINTN *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber
);
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    VOID *BltBuffer,
    UINTN BltOperation,
    UINTN SourceX, UINTN SourceY,
    UINTN DestinationX, UINTN DestinationY,
    UINTN Width, UINTN Height,
    UINTN Delta
);

typedef struct {
    UINT32 Mode;
    UINT32 MaxMode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT        Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      *Mode;
};

extern EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

/* ---- Simple Text Output (for Print) ---- */
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(VOID *This, CHAR16 *String);
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID          *Reset;
    EFI_TEXT_STRING OutputString;
    VOID          *TestString;
    VOID          *QueryMode;
    VOID          *SetMode;
    VOID          *SetAttribute;
    VOID          *ClearScreen;
    VOID          *SetCursorPosition;
    VOID          *EnableCursor;
    VOID          *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- Memory types / allocate type ---- */
typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiMaxMemoryType = 15
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- Boot Services function-pointer typedefs (only the used ones) ---- */
typedef EFI_STATUS (EFIAPI *EFI_RAISE_TPL)(UINTN NewTpl);
typedef VOID       (EFIAPI *EFI_RESTORE_TPL)(UINTN OldTpl);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
    UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);
typedef VOID       (EFIAPI *EFI_COPY_MEM)(VOID *Destination, VOID *Source, UINTN Length);
typedef VOID       (EFIAPI *EFI_SET_MEM)(VOID *Buffer, UINTN Size, UINT8 Value);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol, VOID *Registration, VOID **Interface);

/* ---- EFI_BOOT_SERVICES (field order == UEFI spec, all 8-byte fields) ---- */
typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    EFI_RAISE_TPL        RaiseTPL;
    EFI_RESTORE_TPL      RestoreTPL;

    EFI_ALLOCATE_PAGES   AllocatePages;
    EFI_FREE_PAGES       FreePages;
    EFI_GET_MEMORY_MAP   GetMemoryMap;
    EFI_ALLOCATE_POOL    AllocatePool;
    EFI_FREE_POOL        FreePool;

    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;

    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    VOID *HandleProtocol;
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;

    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;

    VOID *GetNextMonotonicCount;
    EFI_STALL            Stall;
    VOID *SetWatchdogTimer;

    VOID *ConnectController;
    VOID *DisconnectController;

    VOID *OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;

    VOID *ProtocolsPerHandle;
    VOID *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL   LocateProtocol;
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;

    VOID *CalculateCrc32;

    EFI_COPY_MEM         CopyMem;
    EFI_SET_MEM          SetMem;
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

/* ---- EFI_SYSTEM_TABLE ---- */
typedef struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    VOID                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    VOID                            *RuntimeServices;
    EFI_BOOT_SERVICES              *BootServices;
    UINTN                           NumberOfTableEntries;
    VOID                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Global pointers (set by InitializeLib, used by efi_min.c) ---- */
extern EFI_SYSTEM_TABLE *ST;
extern EFI_BOOT_SERVICES *BS;

/* NOTE: bootuefi.c accesses BootServices members DIRECTLY via its local
 * 'bs' pointer (bs->CopyMem, bs->AllocatePages, ...).  We therefore do
 * NOT define CopyMem/AllocatePool/... style convenience macros here;
 * doing so would macro-expand bs->CopyMem into bs->BS->CopyMem and break
 * the build.  The struct fields already carry the correct function types. */

/* ---- Runtime-provided helpers (implemented in efi_min.c) ---- */
void InitializeLib(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable);
EFI_STATUS Print(IN CONST CHAR16 *fmt, ...);

#endif /* EFI_MIN_H */
