// win_fs_menu.cpp
// Build (MSVC):  cl /std:c++17 /EHsc win_fs_menu.cpp
// Run:           win_fs_menu.exe

#define UNICODE
#define _UNICODE
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>
#include <iomanip>

static std::wstring GetLastErrorMessage(DWORD err = GetLastError()) {
    if (err == 0) return L"";
    LPWSTR buf = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buf, 0, nullptr
    );
    std::wstring msg = (size && buf) ? buf : L"(no message)";
    if (buf) LocalFree(buf);
    return msg;
}

static void PrintDrives() {
    DWORD mask = GetLogicalDrives();
    if (mask == 0) {
        std::wcerr << L"GetLogicalDrives failed: " << GetLastErrorMessage() << L"\n";
        return;
    }

    std::wcout << L"Logical drives (GetLogicalDrives bitmask): 0x" << std::hex << mask << std::dec << L"\n";
    std::wcout << L"Drives:\n";
    for (int i = 0; i < 26; ++i) {
        if (mask & (1u << i)) {
            wchar_t root[] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
            std::wcout << L"  " << root;
            UINT type = GetDriveTypeW(root);
            switch (type) {
                case DRIVE_REMOVABLE: std::wcout << L" (REMOVABLE)"; break;
                case DRIVE_FIXED:     std::wcout << L" (FIXED)"; break;
                case DRIVE_REMOTE:    std::wcout << L" (REMOTE)"; break;
                case DRIVE_CDROM:     std::wcout << L" (CDROM)"; break;
                case DRIVE_RAMDISK:   std::wcout << L" (RAMDISK)"; break;
                case DRIVE_NO_ROOT_DIR: std::wcout << L" (NO_ROOT_DIR)"; break;
                default:              std::wcout << L" (UNKNOWN)"; break;
            }
            std::wcout << L"\n";
        }
    }

    // Альтернативно: GetLogicalDriveStringsW
    DWORD need = GetLogicalDriveStringsW(0, nullptr);
    if (need == 0) return;
    std::vector<wchar_t> buf(need + 1);
    DWORD got = GetLogicalDriveStringsW(need, buf.data());
    if (got == 0) return;

    std::wcout << L"\nDrive strings (GetLogicalDriveStringsW):\n";
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wcout << L"  " << p << L"\n";
    }
}

static void PrintDriveInfo(const std::wstring& root) {
    wchar_t volName[MAX_PATH]{};
    wchar_t fsName[MAX_PATH]{};
    DWORD serial = 0, maxCompLen = 0, fsFlags = 0;

    if (!GetVolumeInformationW(
            root.c_str(),
            volName, MAX_PATH,
            &serial,
            &maxCompLen,
            &fsFlags,
            fsName, MAX_PATH)) {
        std::wcerr << L"GetVolumeInformationW failed: " << GetLastErrorMessage() << L"\n";
        return;
    }

    ULARGE_INTEGER freeBytesAvail{}, totalBytes{}, totalFree{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytesAvail, &totalBytes, &totalFree)) {
        std::wcerr << L"GetDiskFreeSpaceExW failed: " << GetLastErrorMessage() << L"\n";
        return;
    }

    std::wcout << L"Volume: " << volName << L"\n";
    std::wcout << L"File system: " << fsName << L"\n";
    std::wcout << L"Serial: 0x" << std::hex << serial << std::dec << L"\n";
    std::wcout << L"Max component length: " << maxCompLen << L"\n";
    std::wcout << L"Total bytes: " << totalBytes.QuadPart << L"\n";
    std::wcout << L"Total free bytes: " << totalFree.QuadPart << L"\n";
    std::wcout << L"Free bytes available: " << freeBytesAvail.QuadPart << L"\n";
}

static void CreateOrRemoveDir(bool createOp) {
    std::wcout << (createOp ? L"Enter directory path to CREATE: " : L"Enter directory path to REMOVE: ");
    std::wstring path;
    std::getline(std::wcin, path);

    BOOL ok = createOp ? CreateDirectoryW(path.c_str(), nullptr)
                       : RemoveDirectoryW(path.c_str());
    if (!ok) {
        std::wcerr << (createOp ? L"CreateDirectoryW failed: " : L"RemoveDirectoryW failed: ")
                   << GetLastErrorMessage() << L"\n";
    } else {
        std::wcout << L"OK\n";
    }
}

static void CreateFileInDir() {
    std::wcout << L"Enter full file path to CREATE (e.g. C:\\temp\\new.txt): ";
    std::wstring path;
    std::getline(std::wcin, path);

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
        std::wcerr << L"CreateFileW failed: " << GetLastErrorMessage() << L"\n";
        return;
    }

    const char* text = "Hello from Win32 CreateFile!\r\n";
    DWORD written = 0;
    BOOL ok = WriteFile(h, text, (DWORD)strlen(text), &written, nullptr);
    if (!ok) {
        std::wcerr << L"WriteFile failed: " << GetLastErrorMessage() << L"\n";
    } else {
        std::wcout << L"Wrote " << written << L" bytes\n";
    }
    CloseHandle(h);
}

static void CopyOrMoveFile(bool copyOp) {
    std::wcout << (copyOp ? L"Enter source file path to COPY: " : L"Enter source file path to MOVE: ");
    std::wstring src;
    std::getline(std::wcin, src);

    std::wcout << (copyOp ? L"Enter destination file path: " : L"Enter destination file path: ");
    std::wstring dst;
    std::getline(std::wcin, dst);

    BOOL ok = FALSE;
    if (copyOp) {
        std::wcout << L"Fail if exists? (0/1): ";
        std::wstring s; std::getline(std::wcin, s);
        BOOL failIfExists = (!s.empty() && s[0] == L'1');
        ok = CopyFileW(src.c_str(), dst.c_str(), failIfExists);
        if (!ok) std::wcerr << L"CopyFileW failed: " << GetLastErrorMessage() << L"\n";
    } else {
        std::wcout << L"Use MoveFileEx (replace existing)? (0/1): ";
        std::wstring s; std::getline(std::wcin, s);
        bool replace = (!s.empty() && s[0] == L'1');
        if (replace) {
            ok = MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (!ok) std::wcerr << L"MoveFileExW failed: " << GetLastErrorMessage() << L"\n";
        } else {
            ok = MoveFileW(src.c_str(), dst.c_str());
            if (!ok) std::wcerr << L"MoveFileW failed: " << GetLastErrorMessage() << L"\n";
        }
    }

    if (ok) std::wcout << L"OK\n";
}

static void FileAttrsAndTimes() {
    std::wcout << L"Enter file path: ";
    std::wstring path;
    std::getline(std::wcin, path);

    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"GetFileAttributesW failed: " << GetLastErrorMessage() << L"\n";
        return;
    }

    std::wcout << L"Attributes bitmask: 0x" << std::hex << attrs << std::dec << L"\n";
    std::wcout << L"  READONLY: " << ((attrs & FILE_ATTRIBUTE_READONLY) ? L"yes" : L"no") << L"\n";
    std::wcout << L"  HIDDEN:   " << ((attrs & FILE_ATTRIBUTE_HIDDEN) ? L"yes" : L"no") << L"\n";
    std::wcout << L"  SYSTEM:   " << ((attrs & FILE_ATTRIBUTE_SYSTEM) ? L"yes" : L"no") << L"\n";
    std::wcout << L"  ARCHIVE:  " << ((attrs & FILE_ATTRIBUTE_ARCHIVE) ? L"yes" : L"no") << L"\n";

    std::wcout << L"Toggle READONLY? (0/1): ";
    std::wstring s; std::getline(std::wcin, s);
    if (!s.empty() && s[0] == L'1') {
        DWORD newAttrs = attrs ^ FILE_ATTRIBUTE_READONLY;
        if (!SetFileAttributesW(path.c_str(), newAttrs)) {
            std::wcerr << L"SetFileAttributesW failed: " << GetLastErrorMessage() << L"\n";
        } else {
            std::wcout << L"READONLY toggled.\n";
        }
    }

    // Times + info by handle
    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CreateFileW (open) failed: " << GetLastErrorMessage() << L"\n";
        return;
    }

    FILETIME c{}, a{}, w{};
    if (!GetFileTime(h, &c, &a, &w)) {
        std::wcerr << L"GetFileTime failed: " << GetLastErrorMessage() << L"\n";
        CloseHandle(h);
        return;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info)) {
        ULONGLONG size = (ULONGLONG(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        std::wcout << L"File size (GetFileInformationByHandle): " << size << L" bytes\n";
    }

    std::wcout << L"Set 'LastWriteTime = current time'? (0/1): ";
    std::getline(std::wcin, s);
    if (!s.empty() && s[0] == L'1') {
        SYSTEMTIME st{}; GetSystemTime(&st);
        FILETIME now{};
        SystemTimeToFileTime(&st, &now);
        if (!SetFileTime(h, nullptr, nullptr, &now)) {
            std::wcerr << L"SetFileTime failed: " << GetLastErrorMessage() << L"\n";
        } else {
            std::wcout << L"LastWriteTime updated.\n";
        }
    }

    CloseHandle(h);
}

static int Menu() {
    std::wcout << L"\n=== Win32 FS Menu ===\n"
               << L"1) List drives (GetLogicalDrives / GetLogicalDriveStrings)\n"
               << L"2) Drive info + free space (GetVolumeInformation / GetDiskFreeSpaceEx)\n"
               << L"3) Create directory (CreateDirectory)\n"
               << L"4) Remove directory (RemoveDirectory)\n"
               << L"5) Create file in directory (CreateFile)\n"
               << L"6) Copy file (CopyFile)\n"
               << L"7) Move file (MoveFile / MoveFileEx)\n"
               << L"8) File attributes & times (Get/SetFileAttributes, Get/SetFileTime, GetFileInformationByHandle)\n"
               << L"0) Exit\n"
               << L"Select: ";
    std::wstring s;
    std::getline(std::wcin, s);
    if (s.empty()) return -1;
    return std::stoi(s);
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    std::locale::global(std::locale(""));

    for (;;) {
        int choice = Menu();
        switch (choice) {
            case 1: PrintDrives(); break;
            case 2: {
                std::wcout << L"Enter drive root (e.g. C:\\): ";
                std::wstring root; std::getline(std::wcin, root);
                if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
                PrintDriveInfo(root);
                break;
            }
            case 3: CreateOrRemoveDir(true); break;
            case 4: CreateOrRemoveDir(false); break;
            case 5: CreateFileInDir(); break;
            case 6: CopyOrMoveFile(true); break;
            case 7: CopyOrMoveFile(false); break;
            case 8: FileAttrsAndTimes(); break;
            case 0: return 0;
            default: std::wcout << L"Unknown option\n"; break;
        }
    }
}
