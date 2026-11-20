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

static const char* DefaultProcess = "Bodycam-Win64-Shipping.exe";
static const wchar_t* DefaultUrl = L"https://github.com/brandonmathew/bodycam/releases/download/latest/bodycam.dll";

static std::string g_ProcessName = DefaultProcess;

static HWND g_Window = nullptr;
static HWND g_StatusText = nullptr;
static HWND g_InjectButton = nullptr;
static HWND g_LogEdit = nullptr;
static HWND g_Progress = nullptr;
static bool g_Busy = false;
static DWORD g_GamePid = 0;

static constexpr UINT WM_LOG_LINE = WM_APP + 1;
static constexpr UINT WM_INJECT_DONE = WM_APP + 2;
static constexpr UINT WM_DOWNLOAD_PROGRESS = WM_APP + 3;
static constexpr int IDC_INJECT = 1001;
static constexpr int IDC_STATUS = 1002;
static constexpr int IDC_LOG = 1003;
static constexpr int IDC_PROGRESS = 1004;

// ---------------------------------------------------------------------------
// Log - safe to call from any thread; the line is appended on the UI thread.
// ---------------------------------------------------------------------------

static void Log(const wchar_t* Format, ...)
{
    wchar_t Buffer[1024];
    va_list Args;
    va_start(Args, Format);
    vswprintf(Buffer, _countof(Buffer), Format, Args);
    va_end(Args);

    auto* Line = new std::wstring(Buffer);
    if (g_Window && IsWindow(g_Window))
        PostMessageW(g_Window, WM_LOG_LINE, 0, reinterpret_cast<LPARAM>(Line));
    else
        delete Line;
}

static void AppendLogLine(const wchar_t* Line)
{
    const int Length = GetWindowTextLengthW(g_LogEdit);
    SendMessageW(g_LogEdit, EM_SETSEL, Length, Length);
    SendMessageW(g_LogEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(Line));
    SendMessageW(g_LogEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"\r\n"));
    SendMessageW(g_LogEdit, EM_SCROLLCARET, 0, 0);
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
    Log(L"Downloading the latest bodycam.dll ...");
    std::vector<BYTE> DllBytes;
    if (!DownloadToMemory(DefaultUrl, DllBytes) || DllBytes.empty())
    {
        Log(L"Download failed.");
        PostMessageW(g_Window, WM_INJECT_DONE, 0, 0);
        return 0;
    }
    Log(L"Downloaded %zu KB into memory.", DllBytes.size() / 1024);

    const DWORD Pid = FindProcessId(g_ProcessName.c_str());
    if (Pid == 0)
    {
        Log(L"Game is not running anymore.");
        PostMessageW(g_Window, WM_INJECT_DONE, 0, 0);
        return 0;
    }

    HANDLE Process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, Pid);
    if (!Process)
    {
        Log(L"OpenProcess failed (%lu). Try running the loader as administrator.", GetLastError());
        PostMessageW(g_Window, WM_INJECT_DONE, 0, 0);
        return 0;
    }

    const bool bOk = ManualMap(Process, Pid, DllBytes);
    CloseHandle(Process);

    Log(bOk ? L"Injected. Nothing was written to disk." : L"Injection failed.");
    PostMessageW(g_Window, WM_INJECT_DONE, bOk ? 1 : 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void RefreshStatus()
{
    if (g_Busy)
        return;

    g_GamePid = FindProcessId(g_ProcessName.c_str());

    wchar_t Text[512];
    if (g_GamePid != 0)
        swprintf(Text, _countof(Text), L"Game: %hs - running (pid %lu)", g_ProcessName.c_str(), g_GamePid);
    else
        swprintf(Text, _countof(Text), L"Game: %hs - not running", g_ProcessName.c_str());
    SetWindowTextW(g_StatusText, Text);

    EnableWindow(g_InjectButton, g_GamePid != 0);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    switch (Message)
    {
    case WM_CREATE:
    {
        HFONT Font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto SetFontOf = [Font](HWND Control) {
            SendMessageW(Control, WM_SETFONT, reinterpret_cast<WPARAM>(Font), TRUE);
        };

        HWND Title = CreateWindowW(L"STATIC", L"Bodycam Loader", WS_CHILD | WS_VISIBLE,
            16, 14, 300, 24, Window, nullptr, nullptr, nullptr);
        SetFontOf(Title);

        g_StatusText = CreateWindowW(L"STATIC", L"Game: -", WS_CHILD | WS_VISIBLE,
            16, 46, 420, 22, Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), nullptr, nullptr);
        SetFontOf(g_StatusText);

        g_InjectButton = CreateWindowW(L"BUTTON", L"Inject", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
            448, 40, 110, 34, Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INJECT)), nullptr, nullptr);
        SetFontOf(g_InjectButton);

        g_Progress = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
            16, 84, 542, 18, Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS)), nullptr, nullptr);
        SendMessageW(g_Progress, PBM_SETRANGE32, 0, 100);
        SendMessageW(g_Progress, PBM_SETPOS, 0, 0);

        g_LogEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            16, 112, 542, 302, Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), nullptr, nullptr);
        SetFontOf(g_LogEdit);

        SetTimer(Window, 1, 1000, nullptr);
        RefreshStatus();
        return 0;
    }

    case WM_TIMER:
        RefreshStatus();
        return 0;

    case WM_COMMAND:
        if (LOWORD(WParam) == IDC_INJECT && HIWORD(WParam) == BN_CLICKED && !g_Busy)
        {
            g_Busy = true;
            EnableWindow(g_InjectButton, FALSE);
            SetWindowTextW(g_StatusText, L"Working ...");
            SendMessageW(g_Progress, PBM_SETPOS, 0, 0);
            HANDLE Thread = CreateThread(nullptr, 0, InjectThread, nullptr, 0, nullptr);
            if (Thread)
                CloseHandle(Thread);
            else
                g_Busy = false;
        }
        return 0;

    case WM_LOG_LINE:
    {
        auto* Line = reinterpret_cast<std::wstring*>(LParam);
        AppendLogLine(Line->c_str());
        delete Line;
        return 0;
    }

    case WM_DOWNLOAD_PROGRESS:
        if (WParam == PROGRESS_MARQUEE)
        {
            SetWindowLongPtrW(g_Progress, GWL_STYLE,
                GetWindowLongPtrW(g_Progress, GWL_STYLE) | PBS_MARQUEE);
            SendMessageW(g_Progress, PBM_SETMARQUEE, TRUE, 30);
        }
        else
        {
            if (GetWindowLongPtrW(g_Progress, GWL_STYLE) & PBS_MARQUEE)
            {
                SendMessageW(g_Progress, PBM_SETMARQUEE, FALSE, 0);
                SetWindowLongPtrW(g_Progress, GWL_STYLE,
                    GetWindowLongPtrW(g_Progress, GWL_STYLE) & ~PBS_MARQUEE);
            }
            if (WParam == PROGRESS_DONE)
                SendMessageW(g_Progress, PBM_SETPOS, 100, 0);
            else
                SendMessageW(g_Progress, PBM_SETPOS, static_cast<int>(WParam), 0);
        }
        return 0;

    case WM_INJECT_DONE:
        g_Busy = false;
        if (WParam == 0)
            SendMessageW(g_Progress, PBM_SETPOS, 0, 0);
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
    // Optional: loader.exe <process.exe> overrides the target process name.
    int Argc = 0;
    wchar_t** Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
    if (Argv && Argc > 1)
    {
        g_ProcessName.clear();
        for (const wchar_t* p = Argv[1]; *p; p++)
            g_ProcessName.push_back(static_cast<char>(*p));
    }
    if (Argv)
        LocalFree(Argv);

    INITCOMMONCONTROLSEX Icc = {};
    Icc.dwSize = sizeof(Icc);
    Icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&Icc);

    WNDCLASSW Class = {};
    Class.lpfnWndProc = WindowProc;
    Class.hInstance = Instance;
    Class.lpszClassName = L"BodycamLoaderWindow";
    Class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    Class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&Class);

    g_Window = CreateWindowExW(0, Class.lpszClassName, L"Bodycam Loader",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 590, 470, nullptr, nullptr, Instance, nullptr);
    if (!g_Window)
        return 1;

    ShowWindow(g_Window, ShowCmd);
    UpdateWindow(g_Window);

    Log(L"Ready. The loader does nothing until you press Inject.");
    Log(L"It will download the latest bodycam.dll into memory and inject it - no file is saved to disk.");

    MSG Msg = {};
    while (GetMessageW(&Msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    return 0;
}
