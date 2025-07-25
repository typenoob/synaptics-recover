#include <iomanip>
#include <iostream>
#include <vector>

#include <shlwapi.h>

#include <ShlObj.h>

#include <Windows.h>

#include <fcntl.h>
#include <io.h>

#include <synare.h>
#include <winutils.h>

#ifdef min
#    undef min
#endif
#ifdef max
#    undef max
#endif

static bool g_debug = false;
static bool g_reserve = false;
static int g_numerator = 20;

// Convert a path string to the most appropriate string in a single line in console
static std::wstring getShortPath(const std::wstring &longFilePath) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(hConsole, &csbi);

    int maxLen = std::min(104, (csbi.dwSize.X / 8) * 8);
    int len = 0;
    for (const auto &ch : longFilePath) {
        len += (ch > 0xFF) ? 2 : 1;
    }

    if (len <= maxLen)
        return longFilePath;

    // Cut string
    const int leftLen = maxLen * 3 / 8;
    const int rightLen = maxLen - leftLen;

    len = 0;
    int leftIndex = 0;
    for (auto it = longFilePath.begin(); it != longFilePath.end(); ++it) {
        const auto &ch = *it;
        len += (ch > 0xFF) ? 2 : 1;
        leftIndex++;
        if (len >= leftLen)
            break;
    }

    len = 0;
    int rightIndex = 0;
    for (auto it = longFilePath.rbegin(); it != longFilePath.rend(); ++it) {
        const auto &ch = *it;
        len += (ch > 0xFF) ? 2 : 1;
        rightIndex++;
        if (len >= rightLen)
            break;
    }

    return longFilePath.substr(0, leftIndex) + L"..." + longFilePath.substr(longFilePath.size() - rightIndex);
}

static bool forceDeleteExe(const std::wstring &filePath) {
    int attempts = 0;
    while (!WinUtils::removeFile(filePath.data()) && ++attempts < 10) {
        // This executable may have a living process
        // Try scan processes and terminate it
        auto code = ::GetLastError();
        uint32_t pid = 0;
        if (!WinUtils::walkThroughProcesses([&](const WinUtils::ProcessInfo &info, void *) -> bool {
                auto canonicalPath1 = WinUtils::getCanonicalPath(filePath);
                auto canonicalPath2 = WinUtils::getCanonicalPath(info.path);
                if (_wcsicmp(canonicalPath1.data(), canonicalPath2.data()) == 0) {
                    pid = info.pid;
                    return true;
                }
                return false;
            })) {
            return false;
        }

        if (pid == 0) {
            ::SetLastError(code);
            return false;
        }

        WinUtils::winConsoleColorScope(
            [&]() {
                wprintf(L"Killing process %-10ld %s\n", pid, filePath.data()); //
            },
            WinUtils::Yellow);

        // Kill
        if (!WinUtils::killProcess(pid)) {
            return false;
        }

        // Wait for process terminated
        ::Sleep(50);
    }
    return true;
}

static void waitForEnter() {
    WinUtils::winConsoleColorScope(
        []() {
            wprintf(L"\n--- Press the <ENTER> key to exit ---"); //
        },
        WinUtils::Green | WinUtils::Highlight);
    std::getchar();
}

static int doScan(const std::wstring &path) {
    WinUtils::winConsoleColorScope(
        [&]() {
            wprintf(L"[Scan Mode]\n");
            wprintf(L"Searching \"%s\" for infected files and recover them.\n",
                    WinUtils::getCanonicalPath(path).data());
        },
        WinUtils::Yellow | WinUtils::Highlight);
    ;
    if (!::IsUserAnAdmin()) {
        WinUtils::winConsoleColorScope(
            [&]() {
                wprintf(L"Warning: %s\n",
                        L"This program isn't running as Administrator, the operation may be incomplete."); //
            },
            WinUtils::Yellow);
    }
    wprintf(L"\n");

    bool needBreak = false;

    size_t my_cnt = 0;
    auto lastTime = ::GetTickCount64();
    bool ret = WinUtils::walkThroughDirectory(path, [&](const std::wstring &filePath) -> bool {
        auto curTime = ::GetTickCount64();
        auto printNormal = [&](const std::wstring &s) {
            WinUtils::winClearConsoleLine();
            std::wcout << getShortPath(s) << std::flush;
            needBreak = true;

            // Update last time
            lastTime = curTime;
        };
        auto printHighlight = [&](const std::wstring &s, int color = WinUtils::Red | WinUtils::Highlight) {
            WinUtils::winClearConsoleLine();
            WinUtils::winConsoleColorScope(
                [&]() {
                    std::wcout << s << std::flush << std::endl; //
                },
                color);
            needBreak = false;
        };

        if (g_debug) {
            if (my_cnt % g_numerator == 0) {
                my_cnt = 0;
                printNormal(filePath);
            }
            my_cnt++;
        } else {
            // If there's no responce for 500ms,
            // simply print something
            if (curTime - lastTime > 500) {
                printNormal(filePath);
            }
        }

        if (_wcsicmp(WinUtils::pathFindExtension(filePath).data(), L"xlsm") == 0) {
            // XLSM
            std::string data;
            std::wstring errorString;
            printNormal(filePath);
            if (Synare::parseXlsmFile(filePath, filePath.substr(0, filePath.size() - 4) + L"xlsx", &errorString) &
                Synare::Infected) {
                printHighlight(filePath);

                // TODO
                while (!g_reserve && !WinUtils::removeFile(filePath.data())) {
                    auto code = ::GetLastError();

                    // Possibly Microsoft Excel is still using the file, terminate it
                    if (code == ERROR_SHARING_VIOLATION) {
                        std::vector<WinUtils::ProcessInfo> processes;
                        if (!WinUtils::walkThroughProcesses([&](const WinUtils::ProcessInfo &info, void *) -> bool {
                                if (_wcsicmp(WinUtils::pathFindFileName(info.path).data(), L"EXCEL.exe") == 0) {
                                    processes.push_back(info);
                                }
                                return false;
                            })) {
                            wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
                            return false;
                        }

                        if (processes.empty()) {
                            ::SetLastError(code);
                        } else {
                            for (const auto &info : std::as_const(processes)) {
                                WinUtils::winConsoleColorScope(
                                    [&]() {
                                        wprintf(L"Killing process %-10ld %s\n", info.pid, info.path.data()); //
                                    },
                                    WinUtils::Yellow);
                                if (!WinUtils::killProcess(info.pid)) {
                                    wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
                                    return false;
                                }

                                // Wait for process terminated
                                ::Sleep(50);
                            }
                            continue;
                        }
                    }
                    wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
                    return false;
                }
            }
            (void) errorString; // used
        } else if (_wcsicmp(WinUtils::pathFindExtension(filePath).data(), L"exe") == 0) {
            // EXE
            printNormal(filePath);
            if (wcsncmp(L"._cache_", WinUtils::pathFindFileName(filePath).data(), 8) == 0) {
                auto fileDir = WinUtils::pathFindDirectory(filePath);
                auto originalFile = fileDir + L"\\" + WinUtils::pathFindFileName(filePath).substr(8);
                if (!WinUtils::pathIsFile(originalFile)) {
                    // Original file doesn't exist
                    // Check file attributes
                    auto attributes = GetFileAttributesW(filePath.data());
                    if ((attributes & FILE_ATTRIBUTE_HIDDEN) && (attributes & FILE_ATTRIBUTE_SYSTEM)) {
                        // Hidden and system
                        // It's most likely a cached executable
                        printHighlight(filePath, WinUtils::Green | WinUtils::Highlight);
                        if (CopyFileW(filePath.c_str(), originalFile.c_str(), false)) {
                            // Set normal file attributes
                            SetFileAttributesW(originalFile.c_str(), FILE_ATTRIBUTE_NORMAL);

                            // Remove
                            if (!forceDeleteExe(filePath)) {
                                wprintf(L"Warning: %s: %s\n", WinUtils::pathFindFileName(filePath).data(),
                                        WinUtils::winLastErrorMessage().data());
                            }
                        } else {
                            wprintf(L"Warning: %s: %s\n", WinUtils::pathFindFileName(filePath).data(),
                                    WinUtils::winLastErrorMessage().data());
                        }
                    }
                }
            } else {
                bool first = true;

                // Although it seems not possible for the virus to wrap an infected file twice,
                // we still need to make sure the extracted one is absolutely safe
                std::string data;
                while (Synare::parseWinExecutable(filePath, nullptr, &data) & Synare::Infected) {
                    if (first) {
                        printHighlight(filePath);
                    }

                    if (!forceDeleteExe(filePath)) {
                        wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
                        return false;
                    }

                    if (first) {
                        // Remove cached executable if exists
                        std::wstring cacheFileName = L"._cache_" + WinUtils::pathFindFileName(filePath);
                        std::wstring cacheFilePath = WinUtils::pathFindDirectory(filePath) + L"\\" + cacheFileName;
                        if (WinUtils::pathIsFile(cacheFilePath)) {
                            printHighlight(cacheFilePath, WinUtils::Green | WinUtils::Highlight);
                            if (!forceDeleteExe(cacheFilePath)) {
                                wprintf(L"Warning: %s: %s\n", cacheFileName.data(),
                                        WinUtils::winLastErrorMessage().data());
                            }
                        }
                        first = false;
                    }

                    // Rewrite
                    if (!data.empty() && !WinUtils::writeFile(filePath, data)) {
                        return false;
                    }
                }
            }
        } else if (_wcsicmp(WinUtils::pathFindFileName(filePath).data(), L"~$cache1") == 0) {
            // Exists if there's infected XLSM file in the directory
            printNormal(filePath);
            if (Synare::parseWinExecutable(filePath, nullptr, nullptr) & Synare::Infected) {
                printHighlight(filePath);

                if (!forceDeleteExe(filePath)) {
                    wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
                    return false;
                }
            }
        }
        return true;
    });

    if (!ret) {
        if (needBreak) {
            std::wcout << std::endl;
        }
        wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
        return false;
    }
    if (needBreak) {
        WinUtils::winClearConsoleLine();
    }
    wprintf(L"OK\n");
    return 0;
}

static int doRecover(const std::wstring &fileName, std::wstring outFileName) {
    if (_wcsicmp(WinUtils::pathFindExtension(fileName).data(), L"xlsm") == 0) {
        if (outFileName.empty()) {
            auto dir = WinUtils::pathFindDirectory(fileName);
            if (!dir.empty()) {
                dir += L"\\";
            }
            outFileName = dir + L"recover_" + WinUtils::pathFindBaseName(fileName) + L".xlsx";
        }
        // XLSM
        std::string data;
        std::wstring errorString;
        switch (Synare::parseXlsmFile(fileName, outFileName, &errorString)) {
            case Synare::XLSM_Failed: {
                wprintf(L"Error: %s: %s\n", fileName.data(), errorString.data());
                return -1;
            }
            case Synare::XLSM_NoVBAProject: {
                wprintf(L"%s: No VBA Project found in document.\n", fileName.data());
                return 1;
            }
            case Synare::XLSM_VirusNotFound: {
                wprintf(L"%s: Virus script not found in document.\n", fileName.data());
                return 1;
            }
            default:
                break;
        }

        WinUtils::winConsoleColorScope(
            [&]() {
                wprintf(L"%s: Successfully recover XLSX file.\n", fileName.data()); //
            },
            WinUtils::Green | WinUtils::Highlight);
    } else {
        if (outFileName.empty()) {
            auto dir = WinUtils::pathFindDirectory(fileName);
            if (!dir.empty()) {
                dir += L"\\";
            }
            outFileName = dir + L"recover_" + WinUtils::pathFindFileName(fileName);
        }

        auto formatMessageWithFileName = [](const std::wstring &fileName) {
            std::wstring errorMessage = WinUtils::winLastErrorMessage(false);
            auto index = errorMessage.find(L"%1");
            if (index == std::wstring::npos) {
                wprintf(L"Error: %s: %s\n", fileName.data(), errorMessage.data());
                return;
            }

            errorMessage = errorMessage.substr(0, index) + fileName + errorMessage.substr(index + 2);
            wprintf(L"Error: %s\n", errorMessage.data());
        };

        // EXE
        std::string version;
        std::string data;
        switch (Synare::parseWinExecutable(fileName, &version, &data)) {
            case Synare::EXEVSNX_NotFound: {
                wprintf(L"%s: File is not infected, the virus version not found.\n", fileName.data());
                return 1;
            }
            case Synare::EXERESX_NotFound: {
                WinUtils::winConsoleColorScope(
                    [&]() {
                        wprintf(L"%s: File is infected, but the binary resource not found.\n", fileName.data()); //
                    },
                    WinUtils::Red);
                return 0;
            }
            case Synare::EXE_Failed: {
                formatMessageWithFileName(fileName);
                return -1;
            }
            case Synare::EXE_Disguised: {
                WinUtils::winConsoleColorScope(
                    [&]() {
                        wprintf(L"%s: This tool is disguised as being infected.\n", fileName.data()); //
                    },
                    WinUtils::Yellow | WinUtils::Highlight);
                return 0;
            }
            default: {
                break;
            }
        }

        // Write output
        if (!WinUtils::writeFile(outFileName, data)) {
            formatMessageWithFileName(outFileName);
            return -1;
        }

        WinUtils::winConsoleColorScope(
            [&]() {
                wprintf(L"%s: Successfully recover executable file, virus version %d.\n", fileName.data(),
                        std::atoi(version.data())); //
            },
            WinUtils::Green | WinUtils::Highlight);
    }
    return 0;
}

static bool removeFileSystemVirus() {
    std::wstring dirs[] = {
        WinUtils::getPathEnv(L"ALLUSERSPROFILE") + L"\\Synaptics",
        WinUtils::getPathEnv(L"WINDIR") + L"\\System32\\Synaptics",
    };

    for (const auto &dir : std::as_const(dirs)) {
        if (::PathIsDirectoryW(dir.data())) {
            WinUtils::winConsoleColorScope(
                [&]() {
                    wprintf(L"Remove \"%s\"\n", dir.data()); //
                },
                WinUtils::Red | WinUtils::Highlight);

            // Remove all executables first
            if (!WinUtils::walkThroughDirectory(
                    dir,
                    [&](const std::wstring &filePath) -> bool {
                        if (_wcsicmp(WinUtils::pathFindExtension(filePath).data(), L"exe") == 0) {
                            if (!forceDeleteExe(filePath)) {
                                return false;
                            }
                        }
                        return true;
                    },
                    true))
                return false;

            if (!WinUtils::removeDirectoryRecursively(dir))
                return false;
        }
    }

    return true;
}

static bool removeRegistryVirus() {
    struct RegEntry {
        const wchar_t *field;
        const wchar_t *key;
    };

#define REG_PREFIX      L"Software\\Microsoft\\Windows\\CurrentVersion\\"
#define VIRUS_FULL_NAME L"Synaptics Pointing Device Driver"

    static const RegEntry regs[] = {
        {REG_PREFIX LR"(Run)",             VIRUS_FULL_NAME                },
        {REG_PREFIX LR"(RunNotification)", L"StartupTNoti" VIRUS_FULL_NAME},
    };

    HKEY hKey;
    for (const auto &reg : regs) {
        // Open entry
        LSTATUS result = RegOpenKeyExW(HKEY_CURRENT_USER, reg.field, 0, KEY_WRITE, &hKey);
        if (result == ERROR_SUCCESS) {
            // Remove key
            switch (RegDeleteValueW(hKey, reg.key)) {
                case ERROR_SUCCESS: {
                    WinUtils::winConsoleColorScope(
                        [&]() {
                            wprintf(L"Remove entry \"HKCU\\%s\\%s\"\n", reg.field, reg.key); //
                        },
                        WinUtils::Red | WinUtils::Highlight);
                    break;
                }
                case ERROR_FILE_NOT_FOUND: {
                    break;
                }
                default: {
                    RegCloseKey(hKey);
                    return false;
                    break;
                }
            }
            RegCloseKey(hKey);
        } else if (result != ERROR_FILE_NOT_FOUND) {
            return false;
        }
    }

    return true;
}

static int doKill() {
    WinUtils::winConsoleColorScope(
        [&]() {
            wprintf(L"[Kill Mode]\n");
            wprintf(L"Sanitizing the processes, virus directory and registry entries.\n");
        },
        WinUtils::Yellow | WinUtils::Highlight);

    // Show warning if not running as Administrator
    if (!IsUserAnAdmin()) {
        WinUtils::winConsoleColorScope(
            [&]() {
                wprintf(L"Warning: %s\n",
                        L"This program isn't running as Administrator, the virus process may be invisible."); //
            },
            WinUtils::Yellow);
    }

    wprintf(L"\n");
    wprintf(L"[Step 1] Terminate virus process\n");
    {
        // Walk through all processes, collect infected ones
        std::vector<WinUtils::ProcessInfo> processes;
        if (!WinUtils::walkThroughProcesses([&](const WinUtils::ProcessInfo &info, void *) -> bool {
                if (Synare::parseWinExecutable(info.path, nullptr, nullptr) & Synare::Infected) {
                    processes.push_back(info);
                }
                return false;
            })) {
            wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
            return -1;
        }

        if (!processes.empty()) {
            // Terminate all infected
            for (const auto &p : std::as_const(processes)) {
                WinUtils::winConsoleColorScope(
                    [&]() {
                        wprintf(L"Killing process %-10ld %s\n", p.pid, p.path.data()); //
                    },
                    WinUtils::Red | WinUtils::Highlight);

                // Kill
                if (!WinUtils::killProcess(p.pid)) {
                    wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
                    return -1;
                }

                // Wait for process terminated
                ::Sleep(50);
            }
        }
        wprintf(L"OK\n");
    }

    wprintf(L"\n");
    wprintf(L"[Step 2] Remove virus directories\n");
    {
        // Remove Synaptics directories
        if (!removeFileSystemVirus()) {
            wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
            return -1;
        }
        wprintf(L"OK\n");
    }

    wprintf(L"\n");
    wprintf(L"[Step 3] Clean Registry\n");
    {
        // The virus will add itself to startup list in registry
        // Remove them all
        if (!removeRegistryVirus()) {
            wprintf(L"Error: %s\n", WinUtils::winLastErrorMessage().data());
            return -1;
        }
        wprintf(L"OK\n");
    }
    return 0;
}

static void displayVersion() {
    wprintf(L"%s\n", TEXT(APP_VERSION));
    wprintf(L"%s\n", TEXT(SUB_VERSION));
}

static void displayHelpText() {
    wprintf(L"Command line tool to remove Synaptics Virus.\n");
    wprintf(L"\n");
    wprintf(L"Usage: %s [-k] [-h] [-v] [<dir>] [<input> [output]] [-d <N>] [--reserve]\n", WinUtils::appName().data());
    wprintf(L"\n");
    wprintf(L"Modes:\n");
    wprintf(L"    %-12s: Kill virus processes, remove virus directories and registry entries\n", L"Kill Mode");
    wprintf(L"    %-12s: Scan the given directory recursively, fix infected EXE or XLSM files\n", L"Scan Mode");
    wprintf(L"    %-12s: Read the given input file, output the original one if infected\n", L"Single Mode");
    wprintf(L"\n");
    wprintf(L"Options:\n");
    wprintf(L"    %-16s    Run in kill mode\n", L"-k");
    wprintf(L"    %-16s    Print after scanning every N files in scan mode\n", L"-d/--debug");
    wprintf(L"    %-16s    Reserve the xlsm files in scan mode\n", L"--reserve");
    wprintf(L"    %-16s    Show this message\n", L"-h/--help");
    wprintf(L"    %-16s    Show version\n", L"-v/--version");
    wprintf(L"\n");

    WinUtils::winConsoleColorScope(
        []() {
            wprintf(L"Copyright SineStriker, checkout https://github.com/SineStriker/synaptics-recover\n"); //
        },
        WinUtils::Yellow | WinUtils::Highlight);
}

int main(int argc, char *argv[]) {
    (void) argc; // unused
    (void) argv; // unused

    struct LocaleGuard {
        LocaleGuard() {
            mode = _setmode(_fileno(stdout), _O_U16TEXT);
        }
        ~LocaleGuard() {
            _setmode(_fileno(stdout), mode);
        }
        int mode;
    };
    LocaleGuard lg;

    std::vector<std::wstring> fileNames;
    bool version = false;
    bool kill = false;
    bool help = false;

    // Parse arguments
    {
        std::vector<std::wstring> arguments = WinUtils::commandLineArguments();
        for (int i = 1; i < arguments.size(); ++i) {
            const auto &arg = arguments[i];
            if (arg == L"--version" || arg == L"-v") {
                version = true;
                break;
            }
            if (arg == L"--help" || arg == L"-h") {
                help = true;
                break;
            }
            if (arg == L"-k") {
                kill = true;
                break;
            }
            if (arg == L"-d" || arg == L"--debug") {
                g_debug = true;
                if (i + 1 < arguments.size()) {
                    g_numerator = std::max(0, std::atoi(WinUtils::strWide2Multi(arguments[i + 1]).data()));
                    i++;
                }
                break;
            }
            if (arg == L"--reserve") {
                g_reserve = true;
                break;
            }
            fileNames.push_back(arg);
        }
    }

    if (version) {
        displayVersion();
        return 0;
    }

    if (argc == 1 || help) {
        displayHelpText();
        return 0;
    }

    if (kill) {
        auto ret = doKill();
        return ret;
    }

    if (fileNames.empty()) {
        displayHelpText();
        return 0;
    }

    struct WaitEnterGuard {
        ~WaitEnterGuard() {
            waitForEnter();
        }
    };
    WaitEnterGuard weg;

    const std::wstring &fileName = fileNames.front();

    DWORD attributes = ::GetFileAttributesW(fileName.data());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"Error: %s: %s\n", fileName.data(), L"Invalid path.");
        return -1;
    }

    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        // Directory
        auto path = WinUtils::fixDirectoryPath(fileName);
        path = ::PathIsRelativeW(path.data()) ? WinUtils::getAbsolutePath(WinUtils::currentDirectory(), path)
                                              : WinUtils::getAbsolutePath(path, L".");

        // If the path is the system root, always run kill mode
        // if (_wcsicmp(path.data(), L"C:") == 0 || _wcsicmp(path.data(), L"C:\\") == 0) {
        //     WinUtils::winConsoleColorScope(
        //         [&]() {
        //             wprintf(L"The path is the system root, automatically run kill mode first.\n"); //
        //         },
        //         WinUtils::White | WinUtils::Highlight);
        //     wprintf(L"\n");

        //     int ret = doKill();
        //     if (ret != 0) {
        //         return ret;
        //     }
        //     wprintf(L"\n");
        // }

        // Always do kill
        int ret = doKill();
        if (ret != 0) {
            return ret;
        }
        wprintf(L"\n");
        return doScan(path);
    }
    return doRecover(fileName, fileNames.size() > 1 ? fileNames.at(1) : std::wstring());
}
