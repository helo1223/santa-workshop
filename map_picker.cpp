#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <vector>
#include "map_picker.h"

#pragma comment(lib, "comdlg32.lib")

std::string PickMapFile(void* owner, const std::string& currentPath, std::string& error) {
    error.clear();
    const auto directory = std::filesystem::absolute(currentPath).parent_path().wstring();
    std::vector<wchar_t> filename(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(owner);
    dialog.lpstrFilter = L"Map files (*.dat)\0*.dat\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrInitialDir = directory.c_str();
    dialog.lpstrTitle = L"Open map";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        const DWORD code = CommDlgExtendedError();
        if (code) error = "Could not open map picker (error " + std::to_string(code) + ")";
        return {};
    }
    // The format libraries currently use narrow Windows file paths. Reject
    // unrepresentable names instead of silently opening a substituted path.
    const UINT codePage = GetACP();
    BOOL substituted = FALSE;
    BOOL* usedDefault = codePage == CP_UTF8 ? nullptr : &substituted;
    const DWORD flags = codePage == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS;
    const int length = WideCharToMultiByte(codePage, flags, filename.data(), -1,
        nullptr, 0, nullptr, usedDefault);
    std::string path(length > 0 ? length : 0, '\0');
    if (!length || !WideCharToMultiByte(codePage, flags, filename.data(), -1,
            path.data(), length, nullptr, usedDefault) || substituted) {
        error = "This map path is not supported by the file loader; use a compatible folder name";
        return {};
    }
    path.pop_back();
    return path;
}
