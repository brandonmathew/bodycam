// Bodycam loader
//
// A small window. It does nothing on its own - when you press Inject it
// downloads the latest bodycam.dll from the GitHub release straight into
// memory (nothing is ever written to disk) and manual-maps it into the
// game: sections copied, relocations applied, imports resolved against
// the modules loaded in the game, then the TLS callbacks and DllMain are
// called in the target process.
//
// Usage: loader.exe [process.exe]
// Default process: Bodycam-Win64-Shipping.exe

#include <Windows.h>
#include <intrin.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <commctrl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#include <dwmapi.h>

static const char* DefaultProcess = "Bodycam-Win64-Shipping.exe";
static const wchar_t* DefaultUrl = L"https://github.com/brandonmathew/bodycam/releases/download/latest/bodycam.dll";

static std::string g_ProcessName = DefaultProcess;

static HWND g_Window = nullptr;
static HWND g_InjectButton = nullptr;
static HWND g_Progress = nullptr;
static bool g_Busy = false;
static bool g_Debug = false;
static DWORD g_GamePid = 0;

static constexpr UINT WM_INJECT_DONE = WM_APP + 2;
static constexpr UINT WM_DOWNLOAD_PROGRESS = WM_APP + 3;
static constexpr UINT WM_RESULT_TEXT = WM_APP + 4;
static constexpr int IDC_INJECT = 1001;
static constexpr int IDC_PROGRESS = 1004;

// ---------------------------------------------------------------------------
// Log - quiet by default. With --debug the loader opens a console and the
// log goes there. The window itself never shows a log.
// ---------------------------------------------------------------------------

static void Log(const wchar_t* Format, ...)
{
    if (!g_Debug)
        return;

    wchar_t Buffer[1024];
    va_list Args;
    va_start(Args, Format);
    vswprintf(Buffer, _countof(Buffer), Format, Args);
    va_end(Args);

    char Narrow[1024];
    const int Len = static_cast<int>(wcslen(Buffer));
    for (int i = 0; i < Len && i < 1023; i++)
        Narrow[i] = Buffer[i] < 128 ? static_cast<char>(Buffer[i]) : '?';
    Narrow[Len < 1023 ? Len : 1023] = '\0';

    fputs(Narrow, stdout);
    fputs("\n", stdout);
    fflush(stdout);
}

// One-line status/result text under the progress bar in the window.
static void SetResult(const wchar_t* Text, int Kind)
{
    auto* Line = new std::wstring(Text);
    if (g_Window && IsWindow(g_Window))
        PostMessageW(g_Window, WM_RESULT_TEXT, static_cast<WPARAM>(Kind), reinterpret_cast<LPARAM>(Line));
    else
        delete Line;
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

// WPARAM for WM_DOWNLOAD_PROGRESS: 0-100 = percent, PROGRESS_MARQUEE =
// total size unknown, PROGRESS_DONE = transfer finished.
static constexpr WPARAM PROGRESS_MARQUEE = static_cast<WPARAM>(-1);
static constexpr WPARAM PROGRESS_DONE = static_cast<WPARAM>(-2);

static void ReportProgress(size_t Downloaded, size_t Total)
{
    if (!g_Window || !IsWindow(g_Window))
        return;
    if (Total > 0)
        PostMessageW(g_Window, WM_DOWNLOAD_PROGRESS, (Downloaded * 100) / Total, 0);
    else
        PostMessageW(g_Window, WM_DOWNLOAD_PROGRESS, PROGRESS_MARQUEE, 0);
}

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
                DWORD Total = 0;
                DWORD TotalSize = sizeof(Total);
                WinHttpQueryHeaders(Request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &Total, &TotalSize, WINHTTP_NO_HEADER_INDEX);
                ReportProgress(0, Total);

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
                    ReportProgress(Out.size(), Total);
                }
                if (bOk)
                    PostMessageW(g_Window, WM_DOWNLOAD_PROGRESS, PROGRESS_DONE, 0);
            }
            else
            {
                Log(L"Download failed: HTTP %lu", Status);
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

static std::wstring ToWide(const std::string& Text)
{
    std::wstring Wide;
    for (char c : Text)
        Wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return Wide;
}

static DWORD FindProcessId(const char* ProcessName)
{
    const std::wstring WideName = ToWide(ProcessName);

    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W Entry = {};
    Entry.dwSize = sizeof(Entry);

    DWORD Pid = 0;
    if (Process32FirstW(Snapshot, &Entry))
    {
        do
        {
            if (_wcsicmp(Entry.szExeFile, WideName.c_str()) == 0)
            {
                Pid = Entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(Snapshot, &Entry));
    }

    CloseHandle(Snapshot);
    return Pid;
}

// Base address of a module inside the target process, 0 if not loaded there.
static ULONGLONG GetRemoteModuleBase(DWORD Pid, const char* ModuleName)
{
    const std::wstring WideName = ToWide(ModuleName);

    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, Pid);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W Entry = {};
    Entry.dwSize = sizeof(Entry);

    ULONGLONG Base = 0;
    if (Module32FirstW(Snapshot, &Entry))
    {
        do
        {
            if (_wcsicmp(Entry.szModule, WideName.c_str()) == 0)
            {
                Base = reinterpret_cast<ULONGLONG>(Entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(Snapshot, &Entry));
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
// API-set resolution
//
// Names like api-ms-win-crt-heap-l1-1-0.dll are not real files. The Windows
// loader translates them through the API-set schema (a map stored in every
// process's PEB) to a host DLL - every api-ms-win-crt-* contract is hosted
// by ucrtbase.dll. Xenos/BlackBone's user-mode manual mapper does the same
// translation (NameResolve) before resolving an import; without it a
// manual map tries to literally load the contract name and fails.
// ---------------------------------------------------------------------------

struct ApiSetValueEntry
{
    ULONG Flags;
    ULONG NameOffset;
    ULONG NameLength;
    ULONG ValueOffset;
    ULONG ValueLength;
};

struct ApiSetValueArray
{
    ULONG Flags;
    ULONG NameOffset;
    ULONG Unk;
    ULONG NameLength;
    ULONG DataOffset;
    ULONG Count;
};

struct ApiSetNamespaceEntry
{
    ULONG Limit;
    ULONG Size;
};

struct ApiSetNamespaceArray
{
    ULONG Version;
    ULONG Size;
    ULONG Flags;
    ULONG Count;
    ULONG Start;
    ULONG End;
    ULONG Unk[2];
};

static bool IsApiSetName(const char* Name)
{
    return _strnicmp(Name, "api-ms-", 7) == 0 || _strnicmp(Name, "ext-ms-", 7) == 0;
}

static std::wstring SchemaString(const BYTE* MapBase, ULONG Offset, ULONG LengthBytes)
{
    return std::wstring(reinterpret_cast<const wchar_t*>(MapBase + Offset), LengthBytes / sizeof(wchar_t));
}

static std::wstring ToWideLower(const char* Text)
{
    std::wstring Wide;
    for (const char* p = Text; *p; p++)
        Wide.push_back(towlower(static_cast<unsigned char>(*p)));
    return Wide;
}

// Host DLL for an API-set contract, or "" if the schema has no answer.
static std::string ResolveApiSetHost(const char* ApiSetName, const char* ImporterName)
{
    // The schema lives in this process's PEB and is the same system-wide.
    const BYTE* Peb = reinterpret_cast<const BYTE*>(__readgsqword(0x60));
    const auto* Map = *reinterpret_cast<const ApiSetNamespaceArray* const*>(Peb + 0x68);
    if (!Map || Map->Version != 6)
        return "";

    const BYTE* Base = reinterpret_cast<const BYTE*>(Map);

    std::wstring Query = ToWideLower(ApiSetName);
    if (Query.size() > 4 && Query.compare(Query.size() - 4, 4, L".dll") == 0)
        Query.resize(Query.size() - 4);
    const std::wstring Importer = ToWideLower(ImporterName);

    for (ULONG i = 0; i < Map->Count; i++)
    {
        const auto* Entry = reinterpret_cast<const ApiSetNamespaceEntry*>(Base + Map->End + i * sizeof(ApiSetNamespaceEntry));
        const auto* Values = reinterpret_cast<const ApiSetValueArray*>(Base + Map->Start + Entry->Size * sizeof(ApiSetValueArray));

        std::wstring Name = SchemaString(Base, Values->NameOffset, Values->NameLength);
        for (auto& c : Name)
            c = towlower(c);
        if (Name != Query)
            continue;

        // Pick the host meant for this importer; the entry with no name is
        // the default that applies to everyone else.
        std::string DefaultHost;
        for (ULONG j = 0; j < Values->Count; j++)
        {
            const auto* Host = reinterpret_cast<const ApiSetValueEntry*>(Base + Values->DataOffset + j * sizeof(ApiSetValueEntry));
            if (Host->ValueLength == 0)
                continue;

            const std::wstring HostName = SchemaString(Base, Host->ValueOffset, Host->ValueLength);
            std::string Narrow;
            for (wchar_t c : HostName)
                Narrow.push_back(static_cast<char>(c));

            std::wstring ForImporter = SchemaString(Base, Host->NameOffset, Host->NameLength);
            for (auto& c : ForImporter)
                c = towlower(c);

            if (!ForImporter.empty() && ForImporter == Importer)
                return Narrow;
            if (ForImporter.empty() && DefaultHost.empty())
                DefaultHost = Narrow;
        }
        return DefaultHost;
    }
    return "";
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
        Log(L"Downloaded file is not a valid PE.");
        return false;
    }

    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(FileBytes.data() + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        Log(L"Downloaded file is not a 64-bit PE.");
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
        Log(L"VirtualAllocEx failed (%lu).", GetLastError());
        return false;
    }
    const ULONGLONG RemoteBaseAddr = reinterpret_cast<ULONGLONG>(RemoteBase);
    Log(L"Image space in target: 0x%llX (%lu KB)", RemoteBaseAddr, Opt.SizeOfImage / 1024);

    // Relocations, applied to the local copy for the remote base.
    const LONGLONG Delta = static_cast<LONGLONG>(RemoteBaseAddr) - static_cast<LONGLONG>(Opt.ImageBase);
    if (Delta != 0)
    {
        const IMAGE_DATA_DIRECTORY& RelocDir = Opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (RelocDir.Size == 0)
        {
            Log(L"DLL has no relocations and could not get its preferred base.");
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

            // API-set contracts resolve to their host DLL before anything
            // tries to find or load them by name.
            std::string HostName;
            if (IsApiSetName(DllName))
            {
                HostName = ResolveApiSetHost(DllName, "bodycam.dll");
                if (HostName.empty() && _strnicmp(DllName, "api-ms-win-crt-", 15) == 0)
                    HostName = "ucrtbase.dll";
                if (HostName.empty())
                {
                    Log(L"Could not resolve API-set %hs.", DllName);
                    return false;
                }
                Log(L"API-set %hs -> %hs", DllName, HostName.c_str());
                DllName = HostName.c_str();
            }

            ULONGLONG RemoteModule = GetRemoteModuleBase(Pid, DllName);
            if (RemoteModule == 0)
            {
                Log(L"Loading dependency into target: %hs", DllName);
                if (!LoadRemoteLibrary(Process, Pid, DllName))
                {
                    Log(L"Could not load %hs into the target.", DllName);
                    return false;
                }
                RemoteModule = GetRemoteModuleBase(Pid, DllName);
            }

            HMODULE LocalModule = GetModuleHandleA(DllName);
            if (!LocalModule)
                LocalModule = LoadLibraryA(DllName);
            if (!LocalModule)
            {
                Log(L"Could not load %hs in the loader.", DllName);
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
                    Log(L"Could not resolve an import from %hs.", DllName);
                    return false;
                }

                Thunk->u1.Function = RemoteModule + (reinterpret_cast<ULONGLONG>(LocalProc) - LocalBase);
            }
        }
    }

    // Write the finished image into the target.
    if (!WriteProcessMemory(Process, RemoteBase, Image.data(), Opt.SizeOfImage, nullptr))
    {
        Log(L"WriteProcessMemory failed (%lu).", GetLastError());
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
        Log(L"VirtualAllocEx for the stub failed (%lu).", GetLastError());
        return false;
    }
    WriteProcessMemory(Process, RemoteStub, Stub.data(), Stub.size(), nullptr);

    HANDLE Thread = CreateRemoteThread(Process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(RemoteStub), nullptr, 0, nullptr);
    if (!Thread)
    {
        Log(L"CreateRemoteThread failed (%lu).", GetLastError());
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
        Log(L"The entry point call did not finish.");
        return false;
    }

    Log(L"DllMain returned %lu.", ExitCode);
    return true;
}

// ---------------------------------------------------------------------------
// Injection - runs on a worker thread so the window stays responsive.
// ---------------------------------------------------------------------------

static DWORD WINAPI InjectThread(void*)
{
    SetResult(L"Downloading the latest build ...", 0);
    Log(L"Downloading the latest bodycam.dll ...");
    std::vector<BYTE> DllBytes;
    if (!DownloadToMemory(DefaultUrl, DllBytes) || DllBytes.empty())
    {
        Log(L"Download failed.");
        SetResult(L"Download failed. Run with --debug for details.", 2);
        PostMessageW(g_Window, WM_INJECT_DONE, 0, 0);
        return 0;
    }
    Log(L"Downloaded %zu KB into memory.", DllBytes.size() / 1024);

    const DWORD Pid = FindProcessId(g_ProcessName.c_str());
    if (Pid == 0)
    {
        Log(L"Game is not running anymore.");
        SetResult(L"The game closed before injection could start.", 2);
        PostMessageW(g_Window, WM_INJECT_DONE, 0, 0);
        return 0;
    }

    HANDLE Process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, Pid);
    if (!Process)
    {
        Log(L"OpenProcess failed (%lu). Try running the loader as administrator.", GetLastError());
        SetResult(L"Could not open the game process. Try running the loader as administrator.", 2);
        PostMessageW(g_Window, WM_INJECT_DONE, 0, 0);
        return 0;
    }

    SetResult(L"Injecting ...", 0);
    const bool bOk = ManualMap(Process, Pid, DllBytes);
    CloseHandle(Process);

    Log(bOk ? L"Injected. Nothing was written to disk." : L"Injection failed.");
    SetResult(bOk ? L"Injected. Closing in 5 seconds."
                  : L"Injection failed. Run with --debug for details.", bOk ? 1 : 2);
    PostMessageW(g_Window, WM_INJECT_DONE, bOk ? 1 : 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Window - dark theme matching the in-game menu: charcoal panel, header
// band, 3px blue accent edge, accent colour for the action button and the
// progress fill.
// ---------------------------------------------------------------------------

static const COLORREF ColPanel = RGB(19, 21, 24);
static const COLORREF ColHeader = RGB(24, 27, 32);
static const COLORREF ColBorder = RGB(45, 50, 58);
static const COLORREF ColAccent = RGB(41, 140, 217);
static const COLORREF ColAccentHover = RGB(56, 152, 224);
static const COLORREF ColAccentPressed = RGB(33, 116, 182);
static const COLORREF ColText = RGB(232, 234, 237);
static const COLORREF ColMuted = RGB(148, 156, 165);
static const COLORREF ColTrack = RGB(13, 15, 17);
static const COLORREF ColGreen = RGB(74, 190, 110);
static const COLORREF ColFail = RGB(224, 108, 108);
static const COLORREF ColDisabled = RGB(38, 44, 51);

static HFONT g_FontTitle = nullptr;
static HFONT g_FontBody = nullptr;
static HFONT g_FontSmall = nullptr;
static HFONT g_FontButton = nullptr;

static bool g_StatusRunning = false;
static std::wstring g_ResultText;
static int g_ResultKind = 0; // 0 neutral, 1 success, 2 failure
static int g_ProgressPercent = 0;
static bool g_ProgressMarquee = false;
static int g_MarqueePhase = 0;
static bool g_ButtonHover = false;

// --- progress bar (custom painted) ---

static LRESULT CALLBACK ProgressProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    switch (Message)
    {
    case WM_TIMER:
        g_MarqueePhase += 4;
        InvalidateRect(Window, nullptr, FALSE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT Ps;
        HDC Dc = BeginPaint(Window, &Ps);
        RECT Rc;
        GetClientRect(Window, &Rc);

        HBRUSH TrackBrush = CreateSolidBrush(ColTrack);
        HPEN BorderPen = CreatePen(PS_SOLID, 1, ColBorder);
        SelectObject(Dc, TrackBrush);
        SelectObject(Dc, BorderPen);
        RoundRect(Dc, Rc.left, Rc.top, Rc.right, Rc.bottom, 10, 10);
        DeleteObject(TrackBrush);
        DeleteObject(BorderPen);

        const int InnerW = (Rc.right - Rc.left) - 2;
        HRGN Clip = CreateRoundRectRgn(Rc.left + 1, Rc.top + 1, Rc.right - 1, Rc.bottom - 1, 8, 8);
        SelectClipRgn(Dc, Clip);
        HBRUSH Fill = CreateSolidBrush(ColAccent);
        if (g_ProgressMarquee)
        {
            const int BlockW = 70;
            const int Span = InnerW + BlockW;
            const int X = Rc.left + 1 + (Span > 0 ? g_MarqueePhase % Span : 0) - BlockW;
            RECT Block = { X, Rc.top + 1, X + BlockW, Rc.bottom - 1 };
            FillRect(Dc, &Block, Fill);
        }
        else if (g_ProgressPercent > 0)
        {
            RECT FillRc = { Rc.left + 1, Rc.top + 1,
                Rc.left + 1 + InnerW * g_ProgressPercent / 100, Rc.bottom - 1 };
            FillRect(Dc, &FillRc, Fill);
        }
        DeleteObject(Fill);
        SelectClipRgn(Dc, nullptr);
        DeleteObject(Clip);

        EndPaint(Window, &Ps);
        return 0;
    }
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

// --- inject button hover tracking ---

static LRESULT CALLBACK ButtonSubclass(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam,
    UINT_PTR, DWORD_PTR)
{
    if (Message == WM_MOUSEMOVE && !g_ButtonHover)
    {
        g_ButtonHover = true;
        TRACKMOUSEEVENT Tme = {};
        Tme.cbSize = sizeof(Tme);
        Tme.dwFlags = TME_LEAVE;
        Tme.hwndTrack = Window;
        TrackMouseEvent(&Tme);
        InvalidateRect(Window, nullptr, FALSE);
    }
    else if (Message == WM_MOUSELEAVE)
    {
        g_ButtonHover = false;
        InvalidateRect(Window, nullptr, FALSE);
    }
    return DefSubclassProc(Window, Message, WParam, LParam);
}

// --- main window ---

static void RefreshStatus()
{
    const DWORD Pid = FindProcessId(g_ProcessName.c_str());
    const bool Running = Pid != 0;
    if (Running == g_StatusRunning && Pid == g_GamePid)
        return;

    g_GamePid = Pid;
    g_StatusRunning = Running;
    if (!g_Busy)
        EnableWindow(g_InjectButton, Running);
    InvalidateRect(g_Window, nullptr, FALSE);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    switch (Message)
    {
    case WM_CREATE:
        g_Progress = CreateWindowW(L"BodycamProgress", L"", WS_CHILD | WS_VISIBLE,
            20, 118, 460, 10, Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS)), nullptr, nullptr);

        g_InjectButton = CreateWindowW(L"BUTTON", L"Inject", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
            372, 182, 108, 30, Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INJECT)), nullptr, nullptr);
        SetWindowSubclass(g_InjectButton, ButtonSubclass, 1, 0);

        SetTimer(Window, 1, 1000, nullptr);
        RefreshStatus();
        return 0;

    case WM_TIMER:
        if (WParam == 3)
        {
            KillTimer(Window, 3);
            DestroyWindow(Window);
            return 0;
        }
        RefreshStatus();
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT Ps;
        HDC Dc = BeginPaint(Window, &Ps);
        RECT Client;
        GetClientRect(Window, &Client);

        HBRUSH Panel = CreateSolidBrush(ColPanel);
        FillRect(Dc, &Client, Panel);
        DeleteObject(Panel);

        // Header band + divider
        RECT Header = { 0, 0, Client.right, 52 };
        HBRUSH HeaderBrush = CreateSolidBrush(ColHeader);
        FillRect(Dc, &Header, HeaderBrush);
        DeleteObject(HeaderBrush);
        HPEN BorderPen = CreatePen(PS_SOLID, 1, ColBorder);
        SelectObject(Dc, BorderPen);
        MoveToEx(Dc, 0, 52, nullptr);
        LineTo(Dc, Client.right, 52);
        MoveToEx(Dc, 0, 168, nullptr);
        LineTo(Dc, Client.right, 168);
        DeleteObject(BorderPen);

        // Accent edge, full height on the left (same as the game menu)
        RECT Edge = { 0, 0, 3, Client.bottom };
        HBRUSH Accent = CreateSolidBrush(ColAccent);
        FillRect(Dc, &Edge, Accent);
        DeleteObject(Accent);

        SetBkMode(Dc, TRANSPARENT);

        // Title
        SelectObject(Dc, g_FontTitle);
        SetTextColor(Dc, ColText);
        TextOutW(Dc, 20, 15, L"Bodycam Loader", 14);

        // Game status
        SelectObject(Dc, g_FontSmall);
        SetTextColor(Dc, ColMuted);
        TextOutW(Dc, 20, 66, L"GAME", 4);

        HBRUSH Dot = CreateSolidBrush(g_StatusRunning ? ColGreen : RGB(90, 98, 106));
        SelectObject(Dc, Dot);
        SelectObject(Dc, GetStockObject(NULL_PEN));
        Ellipse(Dc, 20, 90, 30, 100);
        DeleteObject(Dot);

        SelectObject(Dc, g_FontBody);
        SetTextColor(Dc, ColText);
        wchar_t Status[256];
        if (g_StatusRunning)
            swprintf(Status, _countof(Status), L"%hs - running (pid %lu)", g_ProcessName.c_str(), g_GamePid);
        else
            swprintf(Status, _countof(Status), L"%hs - not running", g_ProcessName.c_str());
        TextOutW(Dc, 38, 86, Status, static_cast<int>(wcslen(Status)));

        // Result line
        SelectObject(Dc, g_FontBody);
        SetTextColor(Dc, g_ResultKind == 1 ? ColGreen : (g_ResultKind == 2 ? ColFail : ColMuted));
        TextOutW(Dc, 20, 140, g_ResultText.c_str(), static_cast<int>(g_ResultText.size()));

        EndPaint(Window, &Ps);
        return 0;
    }

    case WM_DRAWITEM:
    {
        const auto* Dis = reinterpret_cast<const DRAWITEMSTRUCT*>(LParam);
        if (Dis->CtlID != IDC_INJECT)
            break;

        // The control erases its square corners with a light brush before
        // we draw - paint the whole item rect in the panel colour first so
        // only the pill shows.
        HBRUSH Bg = CreateSolidBrush(ColPanel);
        FillRect(Dis->hDC, &Dis->rcItem, Bg);
        DeleteObject(Bg);

        const bool Disabled = (Dis->itemState & ODS_DISABLED) != 0;
        const bool Pressed = (Dis->itemState & ODS_SELECTED) != 0;
        const COLORREF FillColor = Disabled ? ColDisabled
            : (Pressed ? ColAccentPressed : (g_ButtonHover ? ColAccentHover : ColAccent));

        HBRUSH Brush = CreateSolidBrush(FillColor);
        HPEN Pen = CreatePen(PS_SOLID, 1, FillColor);
        SelectObject(Dis->hDC, Brush);
        SelectObject(Dis->hDC, Pen);
        RoundRect(Dis->hDC, Dis->rcItem.left, Dis->rcItem.top, Dis->rcItem.right, Dis->rcItem.bottom, 30, 30);
        DeleteObject(Brush);
        DeleteObject(Pen);

        SetBkMode(Dis->hDC, TRANSPARENT);
        SetTextColor(Dis->hDC, Disabled ? ColMuted : RGB(255, 255, 255));
        SelectObject(Dis->hDC, g_FontButton);
        RECT Rc = Dis->rcItem;
        DrawTextW(Dis->hDC, L"Inject", -1, &Rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(WParam) == IDC_INJECT && HIWORD(WParam) == BN_CLICKED && !g_Busy)
        {
            g_Busy = true;
            EnableWindow(g_InjectButton, FALSE);
            g_ProgressPercent = 0;
            g_ProgressMarquee = false;
            InvalidateRect(g_Progress, nullptr, FALSE);
            HANDLE Thread = CreateThread(nullptr, 0, InjectThread, nullptr, 0, nullptr);
            if (Thread)
                CloseHandle(Thread);
            else
                g_Busy = false;
        }
        return 0;

    case WM_RESULT_TEXT:
    {
        auto* Line = reinterpret_cast<std::wstring*>(LParam);
        g_ResultText = *Line;
        g_ResultKind = static_cast<int>(WParam);
        delete Line;
        InvalidateRect(Window, nullptr, FALSE);
        return 0;
    }

    case WM_DOWNLOAD_PROGRESS:
        if (WParam == PROGRESS_MARQUEE)
        {
            if (!g_ProgressMarquee)
            {
                g_ProgressMarquee = true;
                SetTimer(g_Progress, 2, 30, nullptr);
            }
        }
        else
        {
            if (g_ProgressMarquee)
            {
                g_ProgressMarquee = false;
                KillTimer(g_Progress, 2);
            }
            g_ProgressPercent = WParam == PROGRESS_DONE ? 100 : static_cast<int>(WParam);
        }
        InvalidateRect(g_Progress, nullptr, FALSE);
        return 0;

    case WM_INJECT_DONE:
        g_Busy = false;
        if (WParam == 1)
            SetTimer(Window, 3, 5000, nullptr);
        if (WParam == 0)
        {
            g_ProgressPercent = 0;
            g_ProgressMarquee = false;
            KillTimer(g_Progress, 2);
            InvalidateRect(g_Progress, nullptr, FALSE);
        }
        RefreshStatus();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(Window, Message, WParam, LParam);
}

int WINAPI wWinMain(HINSTANCE Instance, HINSTANCE, wchar_t*, int ShowCmd)
{
    // Arguments: loader.exe [process.exe] [--debug]
    int Argc = 0;
    wchar_t** Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
    if (Argv)
    {
        for (int i = 1; i < Argc; i++)
        {
            if (wcscmp(Argv[i], L"--debug") == 0)
            {
                g_Debug = true;
            }
            else if (g_ProcessName == DefaultProcess)
            {
                g_ProcessName.clear();
                for (const wchar_t* p = Argv[i]; *p; p++)
                    g_ProcessName.push_back(static_cast<char>(*p));
            }
        }
        LocalFree(Argv);
    }

    if (g_Debug)
    {
        AllocConsole();
        FILE* ConsoleOut = nullptr;
        freopen_s(&ConsoleOut, "CONOUT$", "w", stdout);
        SetConsoleTitleW(L"Bodycam Loader - debug");
        printf("Debug log on. Target process: %s\n", g_ProcessName.c_str());
    }

    g_FontTitle = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_FontBody = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_FontSmall = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_FontButton = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSW ProgressClass = {};
    ProgressClass.lpfnWndProc = ProgressProc;
    ProgressClass.hInstance = Instance;
    ProgressClass.lpszClassName = L"BodycamProgress";
    RegisterClassW(&ProgressClass);

    WNDCLASSW Class = {};
    Class.lpfnWndProc = WindowProc;
    Class.hInstance = Instance;
    Class.lpszClassName = L"BodycamLoaderWindow";
    Class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&Class);

    RECT Desired = { 0, 0, 500, 226 };
    AdjustWindowRect(&Desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_Window = CreateWindowExW(0, Class.lpszClassName, L"Bodycam Loader",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        Desired.right - Desired.left, Desired.bottom - Desired.top,
        nullptr, nullptr, Instance, nullptr);
    if (!g_Window)
        return 1;

    // Dark title bar to match the window.
    const BOOL Dark = TRUE;
    DwmSetWindowAttribute(g_Window, 20, &Dark, sizeof(Dark));

    ShowWindow(g_Window, ShowCmd);
    UpdateWindow(g_Window);

    MSG Msg = {};
    while (GetMessageW(&Msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    return 0;
}
