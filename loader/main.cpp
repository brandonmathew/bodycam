// Bodycam loader
//
// Downloads the latest bodycam.dll from the GitHub release straight into
// memory (nothing is ever written to disk) and manual-maps it into the
// game: sections copied, relocations applied, imports resolved against
// the modules loaded in the game, then the TLS callbacks and DllMain are
// called in the target process.
//
// Usage: loader.exe [process.exe] [download url]
// Defaults: Bodycam.exe and the rolling "latest" release of the repo.

#include <Windows.h>
#include <winhttp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

static const char* DefaultProcess = "Bodycam-Win64-Shipping.exe";
static const wchar_t* DefaultUrl = L"https://github.com/brandonmathew/bodycam/releases/download/latest/bodycam.dll";

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

static bool DownloadToMemory(const wchar_t* Url, std::vector<BYTE>& Out)
{
    bool bOk = false;

    HINTERNET Session = WinHttpOpen(L"bodycam-loader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!Session)
        return false;

    // GitHub release downloads redirect to storage - follow every redirect.
    DWORD RedirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(Session, WINHTTP_OPTION_REDIRECT_POLICY, &RedirectPolicy, sizeof(RedirectPolicy));

    URL_COMPONENTS Parts = {};
    Parts.dwStructSize = sizeof(Parts);
    wchar_t Host[256] = {};
    wchar_t Path[2048] = {};
    Parts.lpszHostName = Host;
    Parts.dwHostNameLength = _countof(Host);
    Parts.lpszUrlPath = Path;
    Parts.dwUrlPathLength = _countof(Path);
    if (!WinHttpCrackUrl(Url, 0, 0, &Parts))
        goto Done;

    {
        HINTERNET Connect = WinHttpConnect(Session, Host, Parts.nPort, 0);
        if (!Connect)
            goto Done;

        HINTERNET Request = WinHttpOpenRequest(Connect, L"GET", Path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, Parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!Request)
        {
            WinHttpCloseHandle(Connect);
            goto Done;
        }

        if (WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(Request, nullptr))
        {
            DWORD Status = 0;
            DWORD StatusSize = sizeof(Status);
            WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &Status, &StatusSize, WINHTTP_NO_HEADER_INDEX);

            if (Status == 200)
            {
                bOk = true;
                for (;;)
                {
                    DWORD Available = 0;
                    if (!WinHttpQueryDataAvailable(Request, &Available))
                    {
                        bOk = false;
                        break;
                    }
                    if (Available == 0)
                        break;

                    const size_t OldSize = Out.size();
                    Out.resize(OldSize + Available);
                    DWORD Read = 0;
                    if (!WinHttpReadData(Request, Out.data() + OldSize, Available, &Read))
                    {
                        bOk = false;
                        break;
                    }
                    Out.resize(OldSize + Read);
                }
            }
            else
            {
                printf("Download failed: HTTP %lu\n", Status);
            }
        }

        WinHttpCloseHandle(Request);
        WinHttpCloseHandle(Connect);
    }

Done:
    WinHttpCloseHandle(Session);
    return bOk;
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

static DWORD FindProcessId(const char* ProcessName)
{
    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 Entry = {};
    Entry.dwSize = sizeof(Entry);

    DWORD Pid = 0;
    if (Process32First(Snapshot, &Entry))
    {
        do
        {
            if (_stricmp(Entry.szExeFile, ProcessName) == 0)
            {
                Pid = Entry.th32ProcessID;
                break;
            }
        } while (Process32Next(Snapshot, &Entry));
    }

    CloseHandle(Snapshot);
    return Pid;
}

// Base address of a module inside the target process, 0 if not loaded there.
static ULONGLONG GetRemoteModuleBase(DWORD Pid, const char* ModuleName)
{
    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, Pid);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32 Entry = {};
    Entry.dwSize = sizeof(Entry);

    ULONGLONG Base = 0;
    if (Module32First(Snapshot, &Entry))
    {
        do
        {
            if (_stricmp(Entry.szModule, ModuleName) == 0)
            {
                Base = reinterpret_cast<ULONGLONG>(Entry.modBaseAddr);
                break;
            }
        } while (Module32Next(Snapshot, &Entry));
    }

    CloseHandle(Snapshot);
    return Base;
}

// Loads a DLL into the target process with a remote LoadLibraryA call.
static bool LoadRemoteLibrary(HANDLE Process, DWORD Pid, const char* DllName)
{
    const size_t NameSize = strlen(DllName) + 1;
    void* RemoteName = VirtualAllocEx(Process, nullptr, NameSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!RemoteName)
        return false;

    WriteProcessMemory(Process, RemoteName, DllName, NameSize, nullptr);

    static const auto LoadLibraryAAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));

    HANDLE Thread = CreateRemoteThread(Process, nullptr, 0, LoadLibraryAAddr, RemoteName, 0, nullptr);
    bool bOk = false;
    if (Thread)
    {
        WaitForSingleObject(Thread, 10000);
        bOk = GetRemoteModuleBase(Pid, DllName) != 0;
        CloseHandle(Thread);
    }

    VirtualFreeEx(Process, RemoteName, 0, MEM_RELEASE);
    return bOk;
}

// ---------------------------------------------------------------------------
// Manual map
// ---------------------------------------------------------------------------

// One remote call: function(rcx = image base, rdx = DLL_PROCESS_ATTACH, r8 = 0)
static void AppendRemoteCall(std::vector<BYTE>& Code, ULONGLONG ImageBase, ULONGLONG Function)
{
    const BYTE MovRcx[] = { 0x48, 0xB9 };
    const BYTE MovEdx1[] = { 0xBA, 0x01, 0x00, 0x00, 0x00 };
    const BYTE XorR8[] = { 0x4D, 0x31, 0xC0 };
    const BYTE MovRax[] = { 0x48, 0xB8 };
    const BYTE CallRax[] = { 0xFF, 0xD0 };

    Code.insert(Code.end(), MovRcx, MovRcx + sizeof(MovRcx));
    Code.insert(Code.end(), reinterpret_cast<const BYTE*>(&ImageBase), reinterpret_cast<const BYTE*>(&ImageBase) + 8);
    Code.insert(Code.end(), MovEdx1, MovEdx1 + sizeof(MovEdx1));
    Code.insert(Code.end(), XorR8, XorR8 + sizeof(XorR8));
    Code.insert(Code.end(), MovRax, MovRax + sizeof(MovRax));
    Code.insert(Code.end(), reinterpret_cast<const BYTE*>(&Function), reinterpret_cast<const BYTE*>(&Function) + 8);
    Code.insert(Code.end(), CallRax, CallRax + sizeof(CallRax));
}

static DWORD SectionProtection(DWORD Characteristics)
{
    const bool Exec = (Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool Read = (Characteristics & IMAGE_SCN_MEM_READ) != 0;
    const bool Write = (Characteristics & IMAGE_SCN_MEM_WRITE) != 0;

    if (Exec) return Write ? PAGE_EXECUTE_READWRITE : (Read ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (Write) return PAGE_READWRITE;
    if (Read) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

static bool ManualMap(HANDLE Process, DWORD Pid, const std::vector<BYTE>& FileBytes)
{
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(FileBytes.data());
    if (FileBytes.size() < sizeof(IMAGE_DOS_HEADER) || Dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        printf("Downloaded file is not a valid PE.\n");
        return false;
    }

    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(FileBytes.data() + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        printf("Downloaded file is not a 64-bit PE.\n");
        return false;
    }

    const IMAGE_OPTIONAL_HEADER64& Opt = Nt->OptionalHeader;
    const auto* Sections = IMAGE_FIRST_SECTION(Nt);

    // Build the image locally first: headers + sections in place.
    std::vector<BYTE> Image(Opt.SizeOfImage, 0);
    memcpy(Image.data(), FileBytes.data(), Opt.SizeOfHeaders);
    for (WORD i = 0; i < Nt->FileHeader.NumberOfSections; i++)
    {
        if (Sections[i].SizeOfRawData == 0)
            continue;
        memcpy(Image.data() + Sections[i].VirtualAddress,
            FileBytes.data() + Sections[i].PointerToRawData, Sections[i].SizeOfRawData);
    }

    // Space for the image in the target.
    BYTE* RemoteBase = static_cast<BYTE*>(VirtualAllocEx(Process, nullptr, Opt.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!RemoteBase)
    {
        printf("VirtualAllocEx failed (%lu).\n", GetLastError());
        return false;
    }
    const ULONGLONG RemoteBaseAddr = reinterpret_cast<ULONGLONG>(RemoteBase);
    printf("Image space in target: 0x%llX (%lu KB)\n", RemoteBaseAddr, Opt.SizeOfImage / 1024);

    // Relocations, applied to the local copy for the remote base.
    const LONGLONG Delta = static_cast<LONGLONG>(RemoteBaseAddr) - static_cast<LONGLONG>(Opt.ImageBase);
    if (Delta != 0)
    {
        const IMAGE_DATA_DIRECTORY& RelocDir = Opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (RelocDir.Size == 0)
        {
            printf("DLL has no relocations and could not get its preferred base.\n");
            return false;
        }

        size_t Offset = RelocDir.VirtualAddress;
        const size_t End = Offset + RelocDir.Size;
        while (Offset < End)
        {
            const auto* Block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(Image.data() + Offset);
            if (Block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
                break;

            const DWORD Count = (Block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            const auto* Entries = reinterpret_cast<const WORD*>(Image.data() + Offset + sizeof(IMAGE_BASE_RELOCATION));
            for (DWORD i = 0; i < Count; i++)
            {
                const WORD Type = Entries[i] >> 12;
                const WORD RelOffset = Entries[i] & 0xFFF;
                BYTE* Patch = Image.data() + Block->VirtualAddress + RelOffset;

                if (Type == IMAGE_REL_BASED_DIR64)
                    *reinterpret_cast<ULONGLONG*>(Patch) += static_cast<ULONGLONG>(Delta);
                else if (Type == IMAGE_REL_BASED_HIGHLOW)
                    *reinterpret_cast<DWORD*>(Patch) += static_cast<DWORD>(Delta);
            }
            Offset += Block->SizeOfBlock;
        }
    }

    // Imports: resolve every function against the modules in the target.
    // A dependency the game has not loaded is loaded into it first.
    const IMAGE_DATA_DIRECTORY& ImportDir = Opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (ImportDir.Size > 0)
    {
        auto* Descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(Image.data() + ImportDir.VirtualAddress);
        for (; Descriptor->Name != 0; Descriptor++)
        {
            const char* DllName = reinterpret_cast<const char*>(Image.data() + Descriptor->Name);

            ULONGLONG RemoteModule = GetRemoteModuleBase(Pid, DllName);
            if (RemoteModule == 0)
            {
                printf("Loading dependency into target: %s\n", DllName);
                if (!LoadRemoteLibrary(Process, Pid, DllName))
                {
                    printf("Could not load %s into the target.\n", DllName);
                    return false;
                }
                RemoteModule = GetRemoteModuleBase(Pid, DllName);
            }

            HMODULE LocalModule = GetModuleHandleA(DllName);
            if (!LocalModule)
                LocalModule = LoadLibraryA(DllName);
            if (!LocalModule)
            {
                printf("Could not load %s in the loader.\n", DllName);
                return false;
            }
            const ULONGLONG LocalBase = reinterpret_cast<ULONGLONG>(LocalModule);

            auto* Thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(Image.data() + Descriptor->FirstThunk);
            auto* OriginalThunk = Descriptor->OriginalFirstThunk != 0
                ? reinterpret_cast<IMAGE_THUNK_DATA64*>(Image.data() + Descriptor->OriginalFirstThunk)
                : Thunk;

            for (; OriginalThunk->u1.AddressOfData != 0; OriginalThunk++, Thunk++)
            {
                FARPROC LocalProc = nullptr;
                if (IMAGE_SNAP_BY_ORDINAL64(OriginalThunk->u1.Ordinal))
                {
                    LocalProc = GetProcAddress(LocalModule,
                        reinterpret_cast<const char*>(IMAGE_ORDINAL64(OriginalThunk->u1.Ordinal)));
                }
                else
                {
                    const auto* ByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                        Image.data() + OriginalThunk->u1.AddressOfData);
                    LocalProc = GetProcAddress(LocalModule, ByName->Name);
                }

                if (!LocalProc)
                {
                    printf("Could not resolve an import from %s.\n", DllName);
                    return false;
                }

                Thunk->u1.Function = RemoteModule + (reinterpret_cast<ULONGLONG>(LocalProc) - LocalBase);
            }
        }
    }

    // Write the finished image into the target.
    if (!WriteProcessMemory(Process, RemoteBase, Image.data(), Opt.SizeOfImage, nullptr))
    {
        printf("WriteProcessMemory failed (%lu).\n", GetLastError());
        return false;
    }

    // Per-section protections now that the image is in place.
    DWORD OldProtect = 0;
    VirtualProtectEx(Process, RemoteBase, Opt.SizeOfHeaders, PAGE_READONLY, &OldProtect);
    for (WORD i = 0; i < Nt->FileHeader.NumberOfSections; i++)
    {
        if (Sections[i].Misc.VirtualSize == 0)
            continue;
        VirtualProtectEx(Process, RemoteBase + Sections[i].VirtualAddress, Sections[i].Misc.VirtualSize,
            SectionProtection(Sections[i].Characteristics), &OldProtect);
    }

    // Calls to make in the target: TLS callbacks first, then the entry point.
    std::vector<ULONGLONG> Calls;
    const IMAGE_DATA_DIRECTORY& TlsDir = Opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (TlsDir.Size > 0)
    {
        const auto* Tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(Image.data() + TlsDir.VirtualAddress);
        if (Tls->AddressOfCallBacks != 0)
        {
            const size_t CallbacksRva = static_cast<size_t>(Tls->AddressOfCallBacks - Opt.ImageBase);
            const auto* CallbackArray = reinterpret_cast<const ULONGLONG*>(Image.data() + CallbacksRva);
            for (int i = 0; CallbackArray[i] != 0 && i < 32; i++)
                Calls.push_back(RemoteBaseAddr + (CallbackArray[i] - Opt.ImageBase));
        }
    }
    Calls.push_back(RemoteBaseAddr + Opt.AddressOfEntryPoint);

    // Small stub that makes those calls with (image base, DLL_PROCESS_ATTACH, 0).
    std::vector<BYTE> Stub;
    const BYTE Prologue[] = { 0x48, 0x83, 0xEC, 0x28 }; // sub rsp, 0x28
    const BYTE Epilogue[] = { 0x48, 0x83, 0xC4, 0x28, 0xC3 }; // add rsp, 0x28 / ret
    Stub.insert(Stub.end(), Prologue, Prologue + sizeof(Prologue));
    for (ULONGLONG Fn : Calls)
        AppendRemoteCall(Stub, RemoteBaseAddr, Fn);
    Stub.insert(Stub.end(), Epilogue, Epilogue + sizeof(Epilogue));

    void* RemoteStub = VirtualAllocEx(Process, nullptr, Stub.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!RemoteStub)
    {
        printf("VirtualAllocEx for the stub failed (%lu).\n", GetLastError());
        return false;
    }
    WriteProcessMemory(Process, RemoteStub, Stub.data(), Stub.size(), nullptr);

    HANDLE Thread = CreateRemoteThread(Process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(RemoteStub), nullptr, 0, nullptr);
    if (!Thread)
    {
        printf("CreateRemoteThread failed (%lu).\n", GetLastError());
        VirtualFreeEx(Process, RemoteStub, 0, MEM_RELEASE);
        return false;
    }

    const DWORD Wait = WaitForSingleObject(Thread, 30000);
    DWORD ExitCode = 0;
    GetExitCodeThread(Thread, &ExitCode);
    CloseHandle(Thread);
    VirtualFreeEx(Process, RemoteStub, 0, MEM_RELEASE);

    if (Wait != WAIT_OBJECT_0)
    {
        printf("The entry point call did not finish.\n");
        return false;
    }

    printf("DllMain returned %lu.\n", ExitCode);
    return true;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    const char* ProcessName = argc > 1 ? argv[1] : DefaultProcess;

    std::wstring Url = DefaultUrl;
    if (argc > 2)
    {
        Url.clear();
        for (const char* p = argv[2]; *p; p++)
            Url.push_back(*p);
    }

    printf("Downloading the latest bodycam.dll ...\n");
    std::vector<BYTE> DllBytes;
    if (!DownloadToMemory(Url.c_str(), DllBytes) || DllBytes.empty())
    {
        printf("Download failed.\n");
        return 1;
    }
    printf("Downloaded %zu KB into memory.\n", DllBytes.size() / 1024);

    printf("Looking for %s ...\n", ProcessName);
    DWORD Pid = 0;
    for (int i = 0; i < 60 && Pid == 0; i++)
    {
        Pid = FindProcessId(ProcessName);
        if (Pid == 0)
            Sleep(1000);
    }
    if (Pid == 0)
    {
        printf("Process not found. Start the game first, or pass its exe name: loader.exe <process.exe>\n");
        return 1;
    }
    printf("Found %s, pid %lu.\n", ProcessName, Pid);

    HANDLE Process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, Pid);
    if (!Process)
    {
        printf("OpenProcess failed (%lu). Try running the loader as administrator.\n", GetLastError());
        return 1;
    }

    const bool bOk = ManualMap(Process, Pid, DllBytes);
    CloseHandle(Process);

    printf(bOk ? "Injected. Nothing was written to disk.\n" : "Injection failed.\n");
    return bOk ? 0 : 1;
}
