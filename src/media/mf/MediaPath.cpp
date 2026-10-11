#include "MediaPath.h"
#include "../../platform/win32/HResult.h"
#include <mferror.h>
#include <filesystem>
#include <algorithm>
#include <cwctype>

namespace glasswall::media
{
    std::wstring ValidateLocalPath(std::wstring const& value)
    {
        std::filesystem::path path(value);
        if (value.find(L'\0') != std::wstring::npos || !path.is_absolute() || value.rfind(L"\\\\", 0) == 0)
            throw platform::Failure(E_INVALIDARG, L"Use an absolute local drive path, not a URL/UNC/device path");
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        if (extension != L".mp4") throw platform::Failure(MF_E_INVALIDMEDIATYPE, L"Only local MP4 is supported");
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            throw platform::Failure(HRESULT_FROM_WIN32(GetLastError()), L"Wallpaper file does not exist or is inaccessible");
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) throw platform::Failure(E_INVALIDARG, L"Wallpaper is a directory");
        if (GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE)
            throw platform::Failure(E_INVALIDARG, L"Mapped network drives are not local wallpaper files");
        auto canonical = std::filesystem::canonical(path);
        if (canonical.wstring().rfind(L"\\\\", 0) == 0 ||
            GetDriveTypeW(canonical.root_path().c_str()) == DRIVE_REMOTE)
            throw platform::Failure(E_INVALIDARG, L"Resolved wallpaper is not on a local drive");
        return canonical.wstring();
    }
}
