// Windows implementation of BuildPipelinePlatform — embeds an .ico file into a
// built PE executable via the Win32 resource-update API.

#include "OloEnginePCH.h"
#include "OloEngine/Build/BuildPipelinePlatform.h"

#ifdef OLO_PLATFORM_WINDOWS

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"

#include <cstring>
#include <fstream>
#include <vector>

// Win32 symbols come via the engine PCH (Platform/Windows/WindowsHWrapper.h).

namespace OloEngine::BuildPipelinePlatform
{
    bool EmbedCustomIcon(const std::filesystem::path& exePath,
                         const std::filesystem::path& iconPath,
                         std::string& outError)
    {
        if (!std::filesystem::exists(iconPath))
        {
            outError = "Icon file not found: " + iconPath.string();
            return false;
        }

        // Read the entire .ico file into memory
        std::ifstream icoFile(iconPath, std::ios::binary | std::ios::ate);
        if (!icoFile.is_open())
        {
            outError = "Failed to open icon file: " + iconPath.string();
            return false;
        }

        const auto fileSize = icoFile.tellg();
        if (fileSize < 6) // Minimum ICO header size
        {
            outError = "Icon file is too small or corrupt";
            return false;
        }
        icoFile.seekg(0);

        std::vector<u8> icoData(static_cast<sizet>(fileSize));
        icoFile.read(reinterpret_cast<char*>(icoData.data()), fileSize);
        icoFile.close();

        // Parse ICO header: 6 bytes header + 16 bytes per entry
        // ICONDIR: Reserved(2) + Type(2) + Count(2)
        if (icoData.size() < 6)
        {
            outError = "Invalid ICO file format";
            return false;
        }

        u16 imageCount{};
        std::memcpy(&imageCount, &icoData[4], sizeof(u16));
        if (imageCount == 0 || icoData.size() < static_cast<sizet>(6 + imageCount * 16))
        {
            outError = "Invalid ICO file: no images or truncated directory";
            return false;
        }

        // Open the executable for resource updates
        HANDLE hUpdate = ::BeginUpdateResourceW(exePath.wstring().c_str(), FALSE);
        if (!hUpdate)
        {
            outError = "BeginUpdateResource failed (error " + std::to_string(::GetLastError()) + ")";
            return false;
        }

        // Build the RT_GROUP_ICON directory that references individual RT_ICON entries.
        // GRPICONDIR: Reserved(2) + Type(2) + Count(2) + GRPICONDIRENTRY[Count]
        // Each GRPICONDIRENTRY is 14 bytes (same as ICONDIRENTRY but with nID instead of dwImageOffset)
        const sizet grpSize = 6 + imageCount * 14;
        std::vector<u8> grpData(grpSize);
        std::memcpy(grpData.data(), icoData.data(), 6); // Copy header

        bool anyFailed = false;
        for (u16 i = 0; i < imageCount; ++i)
        {
            const sizet entryOffset = 6 + static_cast<sizet>(i) * 16;
            const u8* entry = &icoData[entryOffset];

            // ICONDIRENTRY: Width(1) Height(1) ColorCount(1) Reserved(1)
            //               Planes(2) BitCount(2) BytesInRes(4) ImageOffset(4)
            u32 bytesInRes{};
            u32 imageOffset{};
            std::memcpy(&bytesInRes, &entry[8], sizeof(u32));
            std::memcpy(&imageOffset, &entry[12], sizeof(u32));

            if (static_cast<sizet>(imageOffset) + bytesInRes > icoData.size())
            {
                anyFailed = true;
                continue;
            }

            // Write individual RT_ICON resource (1-indexed ID)
            u16 iconId = static_cast<u16>(i + 1);
            if (!::UpdateResourceW(hUpdate, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(iconId),
                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                                   const_cast<u8*>(&icoData[imageOffset]), bytesInRes))
            {
                anyFailed = true;
            }

            // Build GRPICONDIRENTRY: copy first 12 bytes from ICONDIRENTRY, then nID(2)
            const sizet grpEntryOffset = 6 + static_cast<sizet>(i) * 14;
            std::memcpy(&grpData[grpEntryOffset], entry, 12);
            std::memcpy(&grpData[grpEntryOffset + 12], &iconId, sizeof(u16));
        }

        // Write RT_GROUP_ICON resource (ID 1 — matches the .rc resource ID)
        if (!::UpdateResourceW(hUpdate, MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1),
                               MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                               grpData.data(), static_cast<DWORD>(grpData.size())))
        {
            anyFailed = true;
        }

        if (!::EndUpdateResourceW(hUpdate, FALSE))
        {
            outError = "EndUpdateResource failed (error " + std::to_string(::GetLastError()) + ")";
            return false;
        }

        if (anyFailed)
        {
            OLO_CORE_WARN("[GameBuild] Some icon entries could not be embedded");
        }

        OLO_CORE_INFO("[GameBuild] Custom icon embedded: {} ({} image(s))", iconPath.filename().string(), imageCount);
        return true;
    }

} // namespace OloEngine::BuildPipelinePlatform

#endif // OLO_PLATFORM_WINDOWS
