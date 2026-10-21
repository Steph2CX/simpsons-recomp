// Contenus XAM (sauvegardes) : un disque dur emule (peripherique 1), dont les paquets sont des
// dossiers de userdata/content/<type de contenu>/<nom de fichier>/. Chaque dossier garde une copie
// de son XCONTENT_DATA (fichier .xcontent) pour l'enumeration.
//
// XCONTENT_DATA (0x134 octets) : DeviceID, ContentType, DisplayName (128 wchar), FileName (42 char)
// XDEVICE_DATA (0x50 octets) : DeviceID, DeviceType, taille, espace libre (u64), nom (27 wchar)
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

void FilesMount(const std::string& root, const fs::path& folder);
void FilesUnmount(const std::string& root);
fs::path FilesUserRoot();
fs::path FilesMountedFolder(const std::string& root);
void SignalEventHandle(uint32_t handle);
void PostNotification(uint32_t id, uint32_t param);

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
    constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
    constexpr uint32_t ERROR_NO_MORE_FILES = 18;
    constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
    constexpr uint32_t ERROR_ALREADY_EXISTS = 183;
    constexpr uint32_t ERROR_IO_PENDING = 997;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;

    constexpr uint32_t DEVICE_ID = 1;
    constexpr uint32_t CONTENT_DATA_SIZE = 0x134;
    constexpr uint32_t DEVICE_DATA_SIZE = 0x50;
    constexpr uint64_t DEVICE_BYTES = 20ull << 30;

    using ContentData = std::vector<uint8_t>; // raw XCONTENT_DATA (guest byte order)

    std::mutex g_mutex;
    std::atomic<uint32_t> g_nextHandle{ 0xFC000004 };
    struct Enumerator
    {
        std::vector<std::vector<uint8_t>> items;
        size_t itemSize = 0;
        size_t position = 0;
    };
    std::unordered_map<uint32_t, Enumerator> g_enumerators;

    // Same layout as xam.cpp: result, length, extended error, event
    void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
    {
        StoreBE32(overlapped + 0x00, result);
        StoreBE32(overlapped + 0x04, length);
        StoreBE32(overlapped + 0x18, result);
        uint32_t event = LoadBE32(overlapped + 0x0C);
        if (event != 0)
            SignalEventHandle(event);
    }

    // Returns the value for r3: synchronous result, or ERROR_IO_PENDING after completing the overlapped
    uint64_t Finish(uint32_t overlapped, uint32_t result, uint32_t length = 0)
    {
        if (overlapped == 0)
            return result;
        CompleteOverlapped(overlapped, result, length);
        return ERROR_IO_PENDING;
    }

    void StoreWide(uint32_t address, const char* text, uint32_t maxChars)
    {
        for (uint32_t i = 0; i < maxChars; i++)
        {
            StoreBE16(address + i * 2, uint16_t(uint8_t(text[i])));
            if (text[i] == 0)
                break;
        }
    }

    void FillDeviceData(uint32_t address)
    {
        memset(GuestToHost(address), 0, DEVICE_DATA_SIZE);
        StoreBE32(address + 0, DEVICE_ID);
        StoreBE32(address + 4, 1); // XCONTENTDEVICETYPE_HDD
        StoreBE32(address + 8, uint32_t(DEVICE_BYTES >> 32));
        StoreBE32(address + 12, uint32_t(DEVICE_BYTES));
        StoreBE32(address + 16, uint32_t(DEVICE_BYTES >> 32));
        StoreBE32(address + 20, uint32_t(DEVICE_BYTES));
        StoreWide(address + 24, "Disque dur", 27);
    }

    std::string FileName(uint32_t contentData)
    {
        const char* name = static_cast<const char*>(GuestToHost(contentData + 0x108));
        return std::string(name, strnlen(name, 42));
    }

    fs::path ContentFolder(uint32_t contentType, const std::string& fileName)
    {
        char type[16];
        snprintf(type, sizeof(type), "%08X", contentType);
        return FilesUserRoot() / "content" / type / fileName;
    }

    std::string ReadRootName(uint32_t address)
    {
        std::string root = static_cast<const char*>(GuestToHost(address));
        if (!root.empty() && root.back() == ':')
            root.pop_back();
        return root;
    }
}

KERNEL_FUNC(XamContentGetDeviceState)
{
    // (DWORD DeviceId, XOVERLAPPED* Overlapped)
    LOG_CALL(XamContentGetDeviceState);
    ctx.r3.u64 = Finish(ctx.r4.u32, ctx.r3.u32 == DEVICE_ID ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED);
}

KERNEL_FUNC(XamContentGetDeviceData)
{
    // (DWORD DeviceId, XDEVICE_DATA* Data)
    LOG_CALL(XamContentGetDeviceData);
    if (ctx.r3.u32 != DEVICE_ID)
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    if (ctx.r4.u32 != 0)
        FillDeviceData(ctx.r4.u32);
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamContentGetDeviceName)
{
    // (DWORD DeviceId, WCHAR* Name, DWORD NameLength)
    LOG_CALL(XamContentGetDeviceName);
    if (ctx.r3.u32 != DEVICE_ID)
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    if (ctx.r4.u32 != 0 && ctx.r5.u32 != 0)
        StoreWide(ctx.r4.u32, "Disque dur", ctx.r5.u32);
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamShowDeviceSelectorUI)
{
    // (DWORD User, DWORD ContentType, DWORD ContentFlags, QWORD TotalRequested, DWORD* DeviceId, XOVERLAPPED*)
    // Only one device: it is selected without showing anything
    LOG_CALL(XamShowDeviceSelectorUI);
    if (ctx.r7.u32 != 0)
        StoreBE32(ctx.r7.u32, DEVICE_ID);
    PostNotification(0x00000009, 1); // XN_SYS_UI shown
    PostNotification(0x00000009, 0); // hidden
    ctx.r3.u64 = Finish(ctx.r8.u32, ERROR_SUCCESS);
}

KERNEL_FUNC(XamContentCreateDeviceEnumerator)
{
    // (DWORD ContentType, DWORD Flags, DWORD ItemsPerEnum, DWORD* BufferSize, HANDLE* Enumerator)
    LOG_CALL(XamContentCreateDeviceEnumerator);
    uint32_t handle = g_nextHandle.fetch_add(4);
    Enumerator enumerator;
    enumerator.itemSize = DEVICE_DATA_SIZE;
    std::vector<uint8_t> device(DEVICE_DATA_SIZE);
    enumerator.items.push_back(device); // filled at enumeration time (FillDeviceData)
    {
        std::lock_guard lock(g_mutex);
        g_enumerators[handle] = std::move(enumerator);
    }
    if (ctx.r6.u32 != 0)
        StoreBE32(ctx.r6.u32, std::max<uint32_t>(ctx.r5.u32, 1) * DEVICE_DATA_SIZE);
    if (ctx.r7.u32 != 0)
        StoreBE32(ctx.r7.u32, handle);
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamContentCreateEnumerator)
{
    // (DWORD User, DWORD DeviceId, DWORD ContentType, DWORD Flags, DWORD ItemsPerEnum,
    //  DWORD* BufferSize, HANDLE* Enumerator)
    LOG_CALL(XamContentCreateEnumerator);
    uint32_t device = ctx.r4.u32, contentType = ctx.r5.u32;
    Enumerator enumerator;
    enumerator.itemSize = CONTENT_DATA_SIZE;
    if (device == 0 || device == DEVICE_ID)
    {
        char type[16];
        snprintf(type, sizeof(type), "%08X", contentType);
        std::error_code error;
        for (auto& entry : fs::directory_iterator(FilesUserRoot() / "content" / type, error))
        {
            std::ifstream file(entry.path() / ".xcontent", std::ios::binary);
            std::vector<uint8_t> data(CONTENT_DATA_SIZE);
            if (file.read(reinterpret_cast<char*>(data.data()), CONTENT_DATA_SIZE))
                enumerator.items.push_back(std::move(data));
        }
    }
    fprintf(stderr, "[contenu] enumeration type 0x%08X : %zu paquet(s)\n", contentType, enumerator.items.size());
    uint32_t handle = g_nextHandle.fetch_add(4);
    {
        std::lock_guard lock(g_mutex);
        g_enumerators[handle] = std::move(enumerator);
    }
    if (ctx.r8.u32 != 0)
        StoreBE32(ctx.r8.u32, std::max<uint32_t>(ctx.r7.u32, 1) * CONTENT_DATA_SIZE);
    if (ctx.r9.u32 != 0)
        StoreBE32(ctx.r9.u32, handle);
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamEnumerate)
{
    // (HANDLE Enumerator, DWORD Flags, VOID* Buffer, DWORD BufferSize, DWORD* ItemsReturned, XOVERLAPPED*)
    LOG_CALL(XamEnumerate);
    uint32_t buffer = ctx.r5.u32, bufferSize = ctx.r6.u32, itemsReturned = ctx.r7.u32, overlapped = ctx.r8.u32;
    uint32_t count = 0;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_enumerators.find(ctx.r3.u32);
        if (it != g_enumerators.end())
        {
            Enumerator& e = it->second;
            while (e.position < e.items.size() && (count + 1) * e.itemSize <= bufferSize)
            {
                uint32_t item = buffer + uint32_t(count * e.itemSize);
                if (e.itemSize == DEVICE_DATA_SIZE)
                    FillDeviceData(item);
                else
                    memcpy(GuestToHost(item), e.items[e.position].data(), e.itemSize);
                e.position++;
                count++;
            }
        }
    }
    if (itemsReturned != 0)
        StoreBE32(itemsReturned, count);
    ctx.r3.u64 = Finish(overlapped, count ? ERROR_SUCCESS : ERROR_NO_MORE_FILES, count);
}

KERNEL_FUNC(XamContentCreateEx)
{
    // (DWORD User, char* RootName, XCONTENT_DATA*, DWORD Flags, DWORD* Disposition, DWORD* LicenseMask,
    //  DWORD CacheSize, QWORD ContentSize, XOVERLAPPED* [stack])
    LOG_CALL(XamContentCreateEx);
    std::string root = ReadRootName(ctx.r4.u32);
    uint32_t contentData = ctx.r5.u32;
    uint32_t mode = ctx.r6.u32 & 0xF; // 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing
    uint32_t overlapped = LoadBE32(ctx.r1.u32 + 0x54);

    fs::path folder = ContentFolder(LoadBE32(contentData + 4), FileName(contentData));
    bool exists = fs::exists(folder / ".xcontent");
    uint32_t result = ERROR_SUCCESS;
    uint32_t disposition = 0;
    if ((mode == 3 || mode == 5) && !exists)
        result = ERROR_PATH_NOT_FOUND;
    else if (mode == 1 && exists)
        result = ERROR_ALREADY_EXISTS;
    else
    {
        std::error_code error;
        if (mode == 2 || mode == 5)
            fs::remove_all(folder, error);
        fs::create_directories(folder, error);
        if (!exists || mode == 2 || mode == 5)
        {
            std::ofstream file(folder / ".xcontent", std::ios::binary);
            file.write(static_cast<const char*>(GuestToHost(contentData)), CONTENT_DATA_SIZE);
            disposition = 1; // created
        }
        else
            disposition = 2; // opened
        FilesMount(root, folder);
    }
    fprintf(stderr, "[contenu] %s: -> %s (mode %u) : %s\n", root.c_str(), folder.c_str(), mode,
        result == ERROR_SUCCESS ? (disposition == 1 ? "cree" : "ouvert") : "erreur");
    if (ctx.r7.u32 != 0)
        StoreBE32(ctx.r7.u32, disposition);
    if (ctx.r8.u32 != 0)
        StoreBE32(ctx.r8.u32, 0);
    ctx.r3.u64 = Finish(overlapped, result);
}

KERNEL_FUNC(XamContentClose)
{
    // (char* RootName, XOVERLAPPED*)
    LOG_CALL(XamContentClose);
    FilesUnmount(ReadRootName(ctx.r3.u32));
    ctx.r3.u64 = Finish(ctx.r4.u32, ERROR_SUCCESS);
}

KERNEL_FUNC(XamContentFlush)
{
    // (char* RootName, XOVERLAPPED*): files are written through immediately
    LOG_CALL(XamContentFlush);
    ctx.r3.u64 = Finish(ctx.r4.u32, ERROR_SUCCESS);
}

KERNEL_FUNC(XamContentDelete)
{
    // (DWORD User, XCONTENT_DATA*, XOVERLAPPED*)
    LOG_CALL(XamContentDelete);
    uint32_t contentData = ctx.r4.u32;
    std::error_code error;
    fs::path folder = ContentFolder(LoadBE32(contentData + 4), FileName(contentData));
    bool removed = fs::remove_all(folder, error) > 0;
    ctx.r3.u64 = Finish(ctx.r5.u32, removed ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
}

KERNEL_FUNC(XamContentGetCreator)
{
    // (DWORD User, XCONTENT_DATA*, BOOL* IsCreator, XUID* Creator, XOVERLAPPED*)
    LOG_CALL(XamContentGetCreator);
    if (ctx.r5.u32 != 0)
        StoreBE32(ctx.r5.u32, 1);
    if (ctx.r6.u32 != 0)
    {
        StoreBE32(ctx.r6.u32, 0xE0000000);
        StoreBE32(ctx.r6.u32 + 4, 0x53494D50);
    }
    ctx.r3.u64 = Finish(ctx.r7.u32, ERROR_SUCCESS);
}

KERNEL_FUNC(XamContentGetThumbnail)
{
    // (DWORD User, XCONTENT_DATA*, BYTE* Buffer, DWORD* BufferSize, XOVERLAPPED*)
    LOG_CALL(XamContentGetThumbnail);
    uint32_t contentData = ctx.r4.u32, buffer = ctx.r5.u32, sizeAddress = ctx.r6.u32;
    fs::path path = ContentFolder(LoadBE32(contentData + 4), FileName(contentData)) / ".thumbnail";
    std::ifstream file(path, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    uint32_t result = ERROR_SUCCESS;
    if (data.empty())
        result = ERROR_FILE_NOT_FOUND;
    else if (sizeAddress != 0)
    {
        if (buffer == 0 || LoadBE32(sizeAddress) < data.size())
            result = ERROR_INSUFFICIENT_BUFFER;
        else
            memcpy(GuestToHost(buffer), data.data(), data.size());
        StoreBE32(sizeAddress, uint32_t(data.size()));
    }
    ctx.r3.u64 = Finish(ctx.r7.u32, result);
}

KERNEL_FUNC(XamContentSetThumbnail)
{
    // (char* RootName, BYTE* Buffer, DWORD Size, XOVERLAPPED*): kept next to the package files
    LOG_CALL(XamContentSetThumbnail);
    fs::path folder = FilesMountedFolder(ReadRootName(ctx.r3.u32));
    if (!folder.empty() && ctx.r4.u32 != 0)
    {
        std::ofstream file(folder / ".thumbnail", std::ios::binary);
        file.write(static_cast<const char*>(GuestToHost(ctx.r4.u32)), ctx.r5.u32);
    }
    ctx.r3.u64 = Finish(ctx.r6.u32, ERROR_SUCCESS);
}

KERNEL_FUNC(XamContentGetLicenseMask)
{
    // (DWORD* LicenseMask, XOVERLAPPED*)
    LOG_CALL(XamContentGetLicenseMask);
    if (ctx.r3.u32 != 0)
        StoreBE32(ctx.r3.u32, 0);
    ctx.r3.u64 = Finish(ctx.r4.u32, ERROR_SUCCESS);
}

// Asynchronous messages to system "apps" (0xFA music player, 0xFB game info/achievements...):
// accepted and completed at once
KERNEL_FUNC(XMsgStartIORequest)
{
    // (DWORD App, DWORD Message, XOVERLAPPED*, VOID* Buffer, DWORD BufferSize)
    LOG_CALL(XMsgStartIORequest);
    ctx.r3.u64 = Finish(ctx.r5.u32, ERROR_SUCCESS);
}

KERNEL_FUNC(XMsgStartIORequestEx)
{
    // (DWORD App, DWORD Message, XOVERLAPPED*, VOID* Buffer, DWORD BufferSize, VOID* Extra)
    LOG_CALL(XMsgStartIORequestEx);
    ctx.r3.u64 = Finish(ctx.r5.u32, ERROR_SUCCESS);
}
