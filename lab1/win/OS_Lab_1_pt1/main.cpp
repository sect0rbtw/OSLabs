//g++ -std=c++17 -O2 win_fs_menu_split.cpp -o win_fs_menu_split.exe -municode

#define UNICODE
#define _UNICODE
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

// ===================== CLR (Clear Screen) =====================
static void ClearConsoleWinAPI() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;

    DWORD cellCount = (DWORD)(csbi.dwSize.X * csbi.dwSize.Y);
    DWORD written = 0;
    COORD home = {0, 0};

    FillConsoleOutputCharacterW(hOut, L' ', cellCount, home, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, cellCount, home, &written);
    SetConsoleCursorPosition(hOut, home);
}

// Пауза после команды: чтобы вывод не “проскочил”
static void PauseEnter() {
    std::wcout << L"\nPress Enter to continue...";
    std::wstring tmp;
    std::getline(std::wcin, tmp);
}

static void PrintFsFlags(DWORD fsFlags) {
    auto yn = [&](DWORD bit) { return (fsFlags & bit) ? L"YES" : L"NO"; };

    std::wcout << L"\n--- File system capabilities (fsFlags) ---\n";
    std::wcout << L"Case sensitive search       : " << yn(FILE_CASE_SENSITIVE_SEARCH) << L"\n";
    std::wcout << L"Preserves case of names     : " << yn(FILE_CASE_PRESERVED_NAMES) << L"\n";
    std::wcout << L"Unicode on disk             : " << yn(FILE_UNICODE_ON_DISK) << L"\n";
    std::wcout << L"Persistent ACLs (permissions): " << yn(FILE_PERSISTENT_ACLS) << L"\n";
    std::wcout << L"File compression supported  : " << yn(FILE_FILE_COMPRESSION) << L"\n";
    std::wcout << L"Volume supports compression : " << yn(FILE_VOLUME_IS_COMPRESSED) << L"\n";
    std::wcout << L"Encryption supported (EFS)  : " << yn(FILE_SUPPORTS_ENCRYPTION) << L"\n";
    std::wcout << L"Reparse points supported    : " << yn(FILE_SUPPORTS_REPARSE_POINTS) << L"\n";
    std::wcout << L"Sparse files supported      : " << yn(FILE_SUPPORTS_SPARSE_FILES) << L"\n";
    std::wcout << L"Hard links supported        : " << yn(FILE_SUPPORTS_HARD_LINKS) << L"\n";
    std::wcout << L"Named streams supported     : " << yn(FILE_NAMED_STREAMS) << L"\n";
    std::wcout << L"Object IDs supported        : " << yn(FILE_SUPPORTS_OBJECT_IDS) << L"\n";
    std::wcout << L"USN Journal supported       : " << yn(FILE_SUPPORTS_USN_JOURNAL) << L"\n";
    std::wcout << L"Extended attributes supported: " << yn(FILE_SUPPORTS_EXTENDED_ATTRIBUTES) << L"\n";
    std::wcout << L"Transactions supported (TxF): " << yn(FILE_SUPPORTS_TRANSACTIONS) << L"\n";
    std::wcout << L"\n";
}

// ===============================================================

static std::wstring LastErrorMsg(DWORD err = GetLastError()) {
    if (err == 0) return L"";
    LPWSTR buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPWSTR)&buf, 0, nullptr);
    std::wstring msg = buf ? buf : L"(no message)";
    if (buf) LocalFree(buf);
    return msg;
}

static std::wstring ReadLine(const wchar_t* prompt) {
    std::wcout << prompt;
    std::wstring s;
    std::getline(std::wcin, s);
    return s;
}

// ------------------------ 1) GetLogicalDrives ------------------------

static void Cmd_GetLogicalDrives() {
    DWORD mask = GetLogicalDrives();
    if (mask == 0) {
        std::wcerr << L"GetLogicalDrives failed: " << LastErrorMsg() << L"\n";
        return;
    }

    std::wcout << L"Bitmask: 0x" << std::hex << mask << std::dec << L"\n";
    std::wcout << L"Drives:\n";
    for (int i = 0; i < 26; ++i) {
        if (mask & (1u << i)) {
            wchar_t root[] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
            std::wcout << L"  " << root << L"\n";
        }
    }
}

// --------------------- 2) GetLogicalDriveStrings ---------------------

static void Cmd_GetLogicalDriveStrings() {
    DWORD need = GetLogicalDriveStringsW(0, nullptr);
    if (need == 0) {
        std::wcerr << L"GetLogicalDriveStringsW(size) failed: " << LastErrorMsg() << L"\n";
        return;
    }

    std::vector<wchar_t> buf(need + 1);
    DWORD got = GetLogicalDriveStringsW(need, buf.data());
    if (got == 0) {
        std::wcerr << L"GetLogicalDriveStringsW(data) failed: " << LastErrorMsg() << L"\n";
        return;
    }

    std::wcout << L"Drive strings:\n";
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wcout << L"  " << p << L"\n";
    }
}

// ------------------------ 3) GetDriveType ----------------------------

static void Cmd_GetDriveType() {
    std::wstring root = ReadLine(L"Enter root (e.g. C:\\): ");
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');

    UINT t = GetDriveTypeW(root.c_str());
    std::wcout << L"GetDriveTypeW(" << root << L") = " << t << L" => ";
    switch (t) {
        case DRIVE_REMOVABLE:  std::wcout << L"REMOVABLE"; break;
        case DRIVE_FIXED:      std::wcout << L"FIXED"; break;
        case DRIVE_REMOTE:     std::wcout << L"REMOTE"; break;
        case DRIVE_CDROM:      std::wcout << L"CDROM"; break;
        case DRIVE_RAMDISK:    std::wcout << L"RAMDISK"; break;
        case DRIVE_NO_ROOT_DIR:std::wcout << L"NO_ROOT_DIR"; break;
        default:               std::wcout << L"UNKNOWN"; break;
    }
    std::wcout << L"\n";
}

// --------------------- 4) GetVolumeInformation -----------------------

static void Cmd_GetVolumeInformation() {
    std::wstring root = ReadLine(L"Enter root (e.g. C:\\): ");
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');

    wchar_t volName[MAX_PATH]{};
    wchar_t fsName[MAX_PATH]{};
    DWORD serial = 0, maxCompLen = 0, fsFlags = 0;

    BOOL ok = GetVolumeInformationW(
        root.c_str(),
        volName, MAX_PATH,
        &serial,
        &maxCompLen,
        &fsFlags,
        fsName, MAX_PATH
    );
    if (!ok) {
        std::wcerr << L"GetVolumeInformationW failed: " << LastErrorMsg() << L"\n";
        return;
    }

    std::wcout << L"Volume root : " << root << L"\n";
    std::wcout << L"Volume label: " << volName << L"\n";
    std::wcout << L"File system : " << fsName << L"\n";
    std::wcout << L"Serial      : 0x" << std::hex << serial << std::dec << L"\n";
    std::wcout << L"Max name len: " << maxCompLen << L"\n";
    std::wcout << L"FS flags    : 0x" << std::hex << fsFlags << std::dec << L"\n";

    // Расшифровка возможностей
    PrintFsFlags(fsFlags);
}

// ---------------------- 5) GetDiskFreeSpaceEx ------------------------

static void Cmd_GetDiskFreeSpaceEx() {
    std::wstring root = ReadLine(L"Enter root (e.g. C:\\): ");
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');

    ULARGE_INTEGER avail{}, total{}, freeAll{};
    BOOL ok = GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &freeAll);
    if (!ok) {
        std::wcerr << L"GetDiskFreeSpaceExW failed: " << LastErrorMsg() << L"\n";
        return;
    }

    std::wcout << L"Total bytes          : " << total.QuadPart << L"\n";
    std::wcout << L"Total free bytes     : " << freeAll.QuadPart << L"\n";
    std::wcout << L"Free bytes available : " << avail.QuadPart << L"\n";
}

// ----------------------- 6) CreateDirectory --------------------------

static void Cmd_CreateDirectory() {
    std::wstring path = ReadLine(L"Enter directory path to create: ");
    BOOL ok = CreateDirectoryW(path.c_str(), nullptr);
    if (!ok) std::wcerr << L"CreateDirectoryW failed: " << LastErrorMsg() << L"\n";
    else std::wcout << L"OK\n";
}

// ----------------------- 7) RemoveDirectory --------------------------

static void Cmd_RemoveDirectory() {
    std::wstring path = ReadLine(L"Enter directory path to remove (must be empty): ");
    BOOL ok = RemoveDirectoryW(path.c_str());
    if (!ok) std::wcerr << L"RemoveDirectoryW failed: " << LastErrorMsg() << L"\n";
    else std::wcout << L"OK\n";
}

// -------------------------- 8) CreateFile ----------------------------

static void Cmd_CreateFile() {
    std::wstring path = ReadLine(L"Enter full file path to create (e.g. C:\\temp\\a.txt): ");

    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CreateFileW failed: " << LastErrorMsg() << L"\n";
        return;
    }

    CloseHandle(h);
}

// --------------------------- 9) CopyFile -----------------------------

static void Cmd_CopyFile() {
    std::wstring src = ReadLine(L"Enter source file: ");
    std::wstring dst = ReadLine(L"Enter destination file: ");
    std::wstring s   = ReadLine(L"Fail if destination exists? (0/1): ");
    BOOL failIfExists = (!s.empty() && s[0] == L'1');

    BOOL ok = CopyFileW(src.c_str(), dst.c_str(), failIfExists);
    if (!ok) std::wcerr << L"CopyFileW failed: " << LastErrorMsg() << L"\n";
    else std::wcout << L"OK\n";
}

// --------------------------- 10) MoveFile ----------------------------

static void Cmd_MoveFile() {
    std::wstring src = ReadLine(L"Enter source file: ");
    std::wstring dst = ReadLine(L"Enter destination file: ");

    BOOL ok = MoveFileW(src.c_str(), dst.c_str());
    if (!ok) std::wcerr << L"MoveFileW failed: " << LastErrorMsg() << L"\n";
    else std::wcout << L"OK\n";
}

// --------------------------- 11) MoveFileEx ---------------------------

static void Cmd_MoveFileEx() {
    std::wstring src = ReadLine(L"Enter source file: ");
    std::wstring dst = ReadLine(L"Enter destination file: ");
    std::wstring s   = ReadLine(L"Replace existing? (0/1): ");
    DWORD flags = 0;
    if (!s.empty() && s[0] == L'1') flags |= MOVEFILE_REPLACE_EXISTING;

    BOOL ok = MoveFileExW(src.c_str(), dst.c_str(), flags);
    if (!ok) std::wcerr << L"MoveFileExW failed: " << LastErrorMsg() << L"\n";
    else std::wcout << L"OK\n";
}

// ------------------------ 12) GetFileAttributes ----------------------

static void Cmd_GetFileAttributes() {
    std::wstring path = ReadLine(L"Enter file path: ");
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"GetFileAttributesW failed: " << LastErrorMsg() << L"\n";
        return;
    }
    std::wcout << L"Attributes mask: 0x" << std::hex << attrs << std::dec << L"\n";
    std::wcout << L"  READONLY=" << ((attrs & FILE_ATTRIBUTE_READONLY) ? L"1" : L"0") << L"\n";
    std::wcout << L"  HIDDEN  =" << ((attrs & FILE_ATTRIBUTE_HIDDEN)   ? L"1" : L"0") << L"\n";
    std::wcout << L"  SYSTEM  =" << ((attrs & FILE_ATTRIBUTE_SYSTEM)   ? L"1" : L"0") << L"\n";
    std::wcout << L"  ARCHIVE =" << ((attrs & FILE_ATTRIBUTE_ARCHIVE)  ? L"1" : L"0") << L"\n";
}

// ------------------------ 13) SetFileAttributes ----------------------

static void Cmd_SetFileAttributes() {
    std::wstring path = ReadLine(L"Enter file path: ");
    std::wstring s    = ReadLine(L"Set READONLY? (0/1): ");
    bool setRO = (!s.empty() && s[0] == L'1');

    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"GetFileAttributesW failed: " << LastErrorMsg() << L"\n";
        return;
    }

    DWORD newAttrs = attrs;
    if (setRO) newAttrs |= FILE_ATTRIBUTE_READONLY;
    else       newAttrs &= ~FILE_ATTRIBUTE_READONLY;

    BOOL ok = SetFileAttributesW(path.c_str(), newAttrs);
    if (!ok) std::wcerr << L"SetFileAttributesW failed: " << LastErrorMsg() << L"\n";
    else std::wcout << L"OK\n";
}

// -------------------- 14) GetFileInformationByHandle -----------------

static void Cmd_GetFileInformationByHandle() {
    std::wstring path = ReadLine(L"Enter file path: ");

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CreateFileW(open) failed: " << LastErrorMsg() << L"\n";
        return;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    BOOL ok = GetFileInformationByHandle(h, &info);
    if (!ok) {
        std::wcerr << L"GetFileInformationByHandle failed: " << LastErrorMsg() << L"\n";
        CloseHandle(h);
        return;
    }

    ULONGLONG size = (ULONGLONG(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    std::wcout << L"File size: " << size << L" bytes\n";
    std::wcout << L"Number of links: " << info.nNumberOfLinks << L"\n";
    std::wcout << L"Attributes mask: 0x" << std::hex << info.dwFileAttributes << std::dec << L"\n";

    CloseHandle(h);
}

static void PrintFileTime(const wchar_t* label, const FILETIME& ft) {
    SYSTEMTIME utc{}, local{};
    // FILETIME -> UTC SYSTEMTIME
    if (!FileTimeToSystemTime(&ft, &utc)) {
        std::wcout << label << L": <convert error>\n";
        return;
    }

    // UTC SYSTEMTIME -> local SYSTEMTIME (важно для “как в проводнике”)
    FILETIME localFt{};
    if (!SystemTimeToFileTime(&utc, &localFt)) {
        std::wcout << label << L": <convert error>\n";
        return;
    }
    if (!FileTimeToLocalFileTime(&ft, &localFt)) {
        // fallback: просто печатаем UTC
        std::wcout << label << L" (UTC): "
                   << utc.wDay << L"." << utc.wMonth << L"." << utc.wYear << L" "
                   << utc.wHour << L":" << utc.wMinute << L":" << utc.wSecond << L"\n";
        return;
    }
    if (!FileTimeToSystemTime(&localFt, &local)) {
        std::wcout << label << L": <convert error>\n";
        return;
    }

    std::wcout << label << L" (local): "
               << local.wDay << L"." << local.wMonth << L"." << local.wYear << L" "
               << local.wHour << L":" << local.wMinute << L":" << local.wSecond << L"\n";
}

// -------------------------- 15) GetFileTime --------------------------

static void Cmd_GetFileTime() {
    std::wstring path = ReadLine(L"Enter file path: ");

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CreateFileW(open) failed: " << LastErrorMsg() << L"\n";
        return;
    }

    FILETIME c{}, a{}, w{};
    BOOL ok = GetFileTime(h, &c, &a, &w);
    if (!ok) {
        std::wcerr << L"GetFileTime failed: " << LastErrorMsg() << L"\n";
        CloseHandle(h);
        return;
    }

    PrintFileTime(L"Creation time", c);
    PrintFileTime(L"Last access  ", a);
    PrintFileTime(L"Last write   ", w);

    CloseHandle(h);
}

// -------------------------- 16) SetFileTime --------------------------

static void Cmd_SetFileTime() {
    std::wstring path = ReadLine(L"Enter file path: ");

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CreateFileW(open) failed: " << LastErrorMsg() << L"\n";
        return;
    }

    FILETIME c0{}, a0{}, w0{};
    if (!GetFileTime(h, &c0, &a0, &w0)) {
        std::wcerr << L"GetFileTime(before) failed: " << LastErrorMsg() << L"\n";
        CloseHandle(h);
        return;
    }

    std::wcout << L"Before:\n";
    PrintFileTime(L"Last write", w0);

    // Берём текущее системное время (UTC) и ставим его как LastWriteTime
    SYSTEMTIME st{};
    GetSystemTime(&st);

    FILETIME ftNow{};
    SystemTimeToFileTime(&st, &ftNow);

    BOOL ok = SetFileTime(h, nullptr, nullptr, &ftNow);
    if (!ok) {
        std::wcerr << L"SetFileTime failed: " << LastErrorMsg() << L"\n";
        CloseHandle(h);
        return;
    }

    FILETIME c1{}, a1{}, w1{};
    if (!GetFileTime(h, &c1, &a1, &w1)) {
        std::wcerr << L"GetFileTime(after) failed: " << LastErrorMsg() << L"\n";
        CloseHandle(h);
        return;
    }

    std::wcout << L"After:\n";
    PrintFileTime(L"Last write", w1);

    CloseHandle(h);
}

// ------------------------------ Menu --------------------------------

static int Menu() {
    std::wcout << L"\n=== Win32 FS Menu ===\n"
               << L" 1) GetLogicalDrives\n"
               << L" 2) GetLogicalDriveStringsW\n"
               << L" 3) GetDriveTypeW\n"
               << L" 4) GetVolumeInformationW\n"
               << L" 5) GetDiskFreeSpaceExW\n"
               << L" 6) CreateDirectoryW\n"
               << L" 7) RemoveDirectoryW\n"
               << L" 8) CreateFileW\n"
               << L" 9) CopyFileW\n"
               << L"10) MoveFileW\n"
               << L"11) MoveFileExW\n"
               << L"12) GetFileAttributesW\n"
               << L"13) SetFileAttributesW\n"
               << L"14) GetFileInformationByHandle\n"
               << L"15) GetFileTime\n"
               << L"16) SetFileTime\n"
               << L" 0) Exit\n"
               << L"Select: ";

    std::wstring s;
    std::getline(std::wcin, s);
    if (s.empty()) return -1;
    return std::stoi(s);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::locale::global(std::locale(""));

    for (;;) {
        int c = Menu();

        if (c == 0) return 0;

        // CLR: очищаем консоль перед выполнением новой команды
        ClearConsoleWinAPI();

        switch (c) {
            case 1:  Cmd_GetLogicalDrives(); break;
            case 2:  Cmd_GetLogicalDriveStrings(); break;
            case 3:  Cmd_GetDriveType(); break;
            case 4:  Cmd_GetVolumeInformation(); break;
            case 5:  Cmd_GetDiskFreeSpaceEx(); break;
            case 6:  Cmd_CreateDirectory(); break;
            case 7:  Cmd_RemoveDirectory(); break;
            case 8:  Cmd_CreateFile(); break;
            case 9:  Cmd_CopyFile(); break;
            case 10: Cmd_MoveFile(); break;
            case 11: Cmd_MoveFileEx(); break;
            case 12: Cmd_GetFileAttributes(); break;
            case 13: Cmd_SetFileAttributes(); break;
            case 14: Cmd_GetFileInformationByHandle(); break;
            case 15: Cmd_GetFileTime(); break;
            case 16: Cmd_SetFileTime(); break;
            default: std::wcout << L"Unknown option\n"; break;
        }

        PauseEnter();
    }
}
