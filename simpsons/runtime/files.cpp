// Systeme de fichiers : traduit les chemins Xbox 360 vers le disque du Mac.
//
// d:\, game:\ et \Device\Cdrom0\ pointent vers le dossier game/ (lecture seule).
// Toute ecriture, et tout autre peripherique (cache:, hdd...), va dans userdata/<peripherique>/.
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

namespace fs = std::filesystem;

#ifdef _WIN32
// Windows: positioned reads and writes (thread safe, like pread/pwrite), pc_window.cpp
ssize_t WindowsPositionedIo(int fd, void* buffer, uint32_t size, uint64_t offset, bool write);
static ssize_t pread(int fd, void* buffer, uint32_t size, uint64_t offset) { return WindowsPositionedIo(fd, buffer, size, offset, false); }
static ssize_t pwrite(int fd, void* buffer, uint32_t size, uint64_t offset) { return WindowsPositionedIo(fd, buffer, size, offset, true); }
static int fsync(int fd) { return _commit(fd); }
static int ftruncate(int fd, uint64_t size) { return _chsize_s(fd, int64_t(size)); }
static struct timespec ModifiedTime(const struct stat& st) { return { st.st_mtime, 0 }; }
#elif defined(__APPLE__)
#define O_BINARY 0
static struct timespec ModifiedTime(const struct stat& st) { return st.st_mtimespec; }
#else
#define O_BINARY 0
static struct timespec ModifiedTime(const struct stat& st) { return st.st_mtim; }
#endif

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;

constexpr uint32_t FILE_ATTRIBUTE_READONLY = 0x01;
constexpr uint32_t FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr uint32_t FILE_ATTRIBUTE_NORMAL = 0x80;

// CreateDisposition
constexpr uint32_t FILE_SUPERSEDE = 0;
constexpr uint32_t FILE_OPEN = 1;
constexpr uint32_t FILE_CREATE = 2;
constexpr uint32_t FILE_OPEN_IF = 3;
constexpr uint32_t FILE_OVERWRITE = 4;
constexpr uint32_t FILE_OVERWRITE_IF = 5;

// CreateOptions
constexpr uint32_t FILE_DIRECTORY_FILE = 0x01;

constexpr uint32_t GENERIC_WRITE = 0x40000000;
constexpr uint32_t FILE_WRITE_DATA = 0x02;
constexpr uint32_t FILE_APPEND_DATA = 0x04;

void SignalEventHandle(uint32_t handle);
bool CloseObjectHandle(uint32_t handle);

static fs::path g_gameRoot;
static fs::path g_userRoot;

struct FileHandle
{
    int fd = -1;
    bool directory = false;
    fs::path hostPath;
    std::string guestPath;
    uint64_t position = 0;
    std::vector<fs::directory_entry> entries; // directory listing for NtQueryDirectoryFile
    size_t nextEntry = 0;
    bool listed = false;
};

static std::mutex g_fileMutex;
static std::unordered_map<uint32_t, FileHandle*> g_files;
static uint32_t g_nextFileHandle = 0xF9000004;

void InitFileSystem(const fs::path& gameRoot)
{
    g_gameRoot = gameRoot;
    // SIMPSONS_DONNEES=<folder>: other user data folder (overlay, saves), e.g. for tests
    const char* userRoot = getenv("SIMPSONS_DONNEES");
    g_userRoot = userRoot ? fs::absolute(userRoot) : gameRoot.parent_path() / "userdata";
    fs::create_directories(g_userRoot);
    fprintf(stderr, "[fichiers] jeu : %s, donnees : %s\n", g_gameRoot.string().c_str(), g_userRoot.string().c_str());
}

// Files shipped with the game that replace or complete the disc (texts, images): <game>/../modifications
fs::path ModificationsRoot()
{
    return g_gameRoot.parent_path() / "modifications";
}

static FileHandle* GetFile(uint32_t handle)
{
    std::lock_guard lock(g_fileMutex);
    auto it = g_files.find(handle);
    return it != g_files.end() ? it->second : nullptr;
}

bool CloseFileHandle(uint32_t handle)
{
    std::lock_guard lock(g_fileMutex);
    auto it = g_files.find(handle);
    if (it == g_files.end())
        return false;
    if (it->second->fd >= 0)
        close(it->second->fd);
    delete it->second;
    g_files.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
// Path translation

static std::string ReadAnsiString(uint32_t ansiString)
{
    uint16_t length = LoadBE16(ansiString);
    uint32_t buffer = LoadBE32(ansiString + 4);
    return std::string(static_cast<char*>(GuestToHost(buffer)), length);
}

static std::string ObjectAttributesPath(uint32_t attributes)
{
    // OBJECT_ATTRIBUTES: RootDirectory, ObjectName (ANSI_STRING*), Attributes
    uint32_t root = LoadBE32(attributes);
    std::string name = ReadAnsiString(LoadBE32(attributes + 4));
    if (root != 0)
    {
        if (auto* directory = GetFile(root))
            name = directory->guestPath + "\\" + name;
    }
    return name;
}

static std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// Content packages mounted by XamContentCreateEx: root name ("savegame") -> host folder
static std::mutex g_mountMutex;
static std::unordered_map<std::string, fs::path> g_mounts;

void FilesMount(const std::string& root, const fs::path& folder)
{
    std::lock_guard lock(g_mountMutex);
    g_mounts[ToLower(root)] = folder;
}

void FilesUnmount(const std::string& root)
{
    std::lock_guard lock(g_mountMutex);
    g_mounts.erase(ToLower(root));
}

fs::path FilesMountedFolder(const std::string& root)
{
    std::lock_guard lock(g_mountMutex);
    auto it = g_mounts.find(ToLower(root));
    return it != g_mounts.end() ? it->second : fs::path();
}

fs::path FilesUserRoot()
{
    return g_userRoot;
}

// Linux file systems are case sensitive while the game mixes cases ("CAE47A5A.str" for "cae47a5a.str"):
// a missing path is resolved component by component ignoring case (results cached).
static std::mutex g_caseCacheMutex;
static std::unordered_map<std::string, fs::path> g_caseCache;

static fs::path ResolveCase(const fs::path& path)
{
    // Case-insensitive file systems (Windows, macOS); tvOS is case-sensitive like Linux
#if defined(_WIN32) || (defined(__APPLE__) && !defined(SIMPSONS_TVOS))
    return path;
#else
    std::error_code error;
    if (fs::exists(path, error))
        return path;
    std::lock_guard lock(g_caseCacheMutex);
    auto cached = g_caseCache.find(path.string());
    if (cached != g_caseCache.end())
        return cached->second;
    fs::path resolved = path.root_path();
    for (const fs::path& part : path.relative_path())
    {
        fs::path next = resolved / part;
        if (!fs::exists(next, error))
        {
            std::string wanted = ToLower(part.string());
            for (const fs::directory_entry& entry : fs::directory_iterator(resolved, error))
            {
                if (ToLower(entry.path().filename().string()) == wanted)
                {
                    next = entry.path();
                    break;
                }
            }
        }
        resolved = next;
    }
    g_caseCache[path.string()] = resolved;
    return resolved;
#endif
}

// The cache also remembers paths that did not exist yet: forget it once the game creates a file or directory
static void ForgetResolvedCases()
{
    std::lock_guard lock(g_caseCacheMutex);
    g_caseCache.clear();
}

struct TranslatedPath
{
    fs::path readPath; // where an existing file is looked up
    fs::path writePath; // where new or modified files go
};

static TranslatedPath TranslatePath(const std::string& guestPath)
{
    std::string path = guestPath;
    std::string device;
    std::string lower = ToLower(path);

    static const char* gamePrefixes[] = { "\\device\\cdrom0", "\\??\\d:", "\\??\\game:", "d:", "game:" };
    bool isGame = false;
    for (const char* prefix : gamePrefixes)
    {
        if (lower.rfind(prefix, 0) == 0)
        {
            path = path.substr(strlen(prefix));
            isGame = true;
            break;
        }
    }

    if (!isGame)
    {
        size_t colon = path.find(':');
        if (colon != std::string::npos)
        {
            device = ToLower(path.substr(path.rfind('\\', colon) == std::string::npos ? 0 : path.rfind('\\', colon) + 1, colon));
            path = path.substr(colon + 1);
        }
        else
        {
            device = "racine";
        }
    }

    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());

    TranslatedPath result;
    if (isGame)
    {
        // Lookup order: user overlay, modifications shipped with the game (next to its data), the disc
        result.writePath = ResolveCase(g_userRoot / "d" / path);
        fs::path modified = ResolveCase(g_gameRoot.parent_path() / "modifications" / path);
        result.readPath = fs::exists(result.writePath) ? result.writePath : fs::exists(modified) ? modified : ResolveCase(g_gameRoot / path);
    }
    else
    {
        fs::path base = g_userRoot / device;
        {
            std::lock_guard lock(g_mountMutex);
            auto it = g_mounts.find(device);
            if (it != g_mounts.end())
                base = it->second;
        }
        result.writePath = ResolveCase(base / path);
        result.readPath = result.writePath;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Attributes and times

static uint64_t ToFiletime(const struct timespec& time)
{
    return uint64_t(time.tv_sec) * 10000000ull + uint64_t(time.tv_nsec) / 100 + 116444736000000000ull;
}

static void StoreBE64(uint32_t address, uint64_t value)
{
    *static_cast<uint64_t*>(GuestToHost(address)) = __builtin_bswap64(value);
}

// FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
static void WriteNetworkOpenInformation(uint32_t info, const struct stat& st)
{
    uint64_t time = ToFiletime(ModifiedTime(st));
    bool directory = S_ISDIR(st.st_mode);
    StoreBE64(info + 0x00, time);
    StoreBE64(info + 0x08, time);
    StoreBE64(info + 0x10, time);
    StoreBE64(info + 0x18, time);
    StoreBE64(info + 0x20, directory ? 0 : (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull);
    StoreBE64(info + 0x28, directory ? 0 : uint64_t(st.st_size));
    StoreBE32(info + 0x30, directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    StoreBE32(info + 0x34, 0);
}

static void SetIoStatus(uint32_t ioStatusBlock, uint32_t status, uint32_t information)
{
    if (ioStatusBlock == 0)
        return;
    StoreBE32(ioStatusBlock, status);
    StoreBE32(ioStatusBlock + 4, information);
}

// ---------------------------------------------------------------------------
// Open / create

static uint32_t OpenFile(uint32_t handlePtr, uint32_t desiredAccess, uint32_t attributes, uint32_t ioStatusBlock,
    uint32_t disposition, uint32_t options)
{
    std::string guestPath = ObjectAttributesPath(attributes);
    TranslatedPath path = TranslatePath(guestPath);

    bool wantsWrite = (desiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    bool exists = fs::exists(path.readPath);

    // SIMPSONS_TRACE_FICHIERS: every file opened, with the time since start
    static const bool traceFiles = getenv("SIMPSONS_TRACE_FICHIERS") != nullptr;
    static const auto started = std::chrono::steady_clock::now();
    if (traceFiles)
        fprintf(stderr, "[fichiers] %.2f s : %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), guestPath.c_str());

    // The EA, Fox and Gracie logo movies of the start are reported missing: the game skips them and reaches the
    // "press START" screen in ~8 s instead of ~24 s. SIMPSONS_LOGOS=1 shows them.
    static const bool skipLogos = getenv("SIMPSONS_LOGOS") == nullptr;
    if (skipLogos && exists)
    {
        std::string name = ToLower(path.readPath.filename().string());
        if (name.starts_with("ealogo") || name.starts_with("foxlogo") || name.starts_with("gracielogo"))
            exists = false;
    }
    bool isDirectory = exists && fs::is_directory(path.readPath);

    if (!exists && disposition == FILE_OPEN)
    {
        fprintf(stderr, "[fichiers] introuvable : %s\n", guestPath.c_str());
        SetIoStatus(ioStatusBlock, STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (exists && disposition == FILE_CREATE)
    {
        SetIoStatus(ioStatusBlock, STATUS_OBJECT_NAME_COLLISION, 0);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    auto* file = new FileHandle();
    file->guestPath = guestPath;
    uint32_t information = 1; // FILE_OPENED

    if ((options & FILE_DIRECTORY_FILE) || isDirectory)
    {
        if (!exists)
        {
            fs::create_directories(path.writePath);
            ForgetResolvedCases();
            path.readPath = path.writePath;
            information = 2; // FILE_CREATED
        }
        file->directory = true;
        file->hostPath = path.readPath;
    }
    else
    {
        fs::path hostPath = path.readPath;
        bool create = !exists;
        bool truncate = disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF;

        if (wantsWrite || create)
        {
            // Copy-on-write: game files are never modified in place
            hostPath = path.writePath;
            fs::create_directories(hostPath.parent_path());
            if (exists && path.readPath != path.writePath && !truncate)
                fs::copy_file(path.readPath, path.writePath, fs::copy_options::overwrite_existing);
        }

        int flags = (wantsWrite || create ? O_RDWR : O_RDONLY) | O_BINARY;
        if (create)
            flags |= O_CREAT;
        if (truncate && exists)
            flags |= O_TRUNC;

        file->fd = open(hostPath.string().c_str(), flags, 0644);
        if (file->fd < 0)
        {
            fprintf(stderr, "[fichiers] echec d'ouverture %s (%s)\n", guestPath.c_str(), strerror(errno));
            delete file;
            SetIoStatus(ioStatusBlock, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
        file->hostPath = hostPath;
        if (create)
        {
            ForgetResolvedCases();
            information = 2;
        }
        else if (truncate)
            information = 3; // FILE_OVERWRITTEN
    }

    uint32_t handle;
    {
        std::lock_guard lock(g_fileMutex);
        handle = g_nextFileHandle;
        g_nextFileHandle += 4;
        g_files[handle] = file;
    }

    StoreBE32(handlePtr, handle);
    SetIoStatus(ioStatusBlock, STATUS_SUCCESS, information);
    fprintf(stderr, "[fichiers] ouvert %s%s\n", guestPath.c_str(), file->directory ? " (dossier)" : "");
    return STATUS_SUCCESS;
}

KERNEL_FUNC(NtCreateFile)
{
    // (HANDLE* Handle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES* Attributes, IO_STATUS_BLOCK* IoStatus,
    //  LARGE_INTEGER* AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions)
    LOG_CALL(NtCreateFile);
    uint32_t options = PPC_LOAD_U32(ctx.r1.u32 + 0x54);
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options);
}

KERNEL_FUNC(NtOpenFile)
{
    // (HANDLE* Handle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES* Attributes, IO_STATUS_BLOCK* IoStatus,
    //  ULONG ShareAccess, ULONG OpenOptions)
    LOG_CALL(NtOpenFile);
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, FILE_OPEN, ctx.r8.u32);
}

KERNEL_FUNC(NtQueryFullAttributesFile)
{
    // (OBJECT_ATTRIBUTES* Attributes, FILE_NETWORK_OPEN_INFORMATION* Information)
    LOG_CALL(NtQueryFullAttributesFile);
    std::string guestPath = ObjectAttributesPath(ctx.r3.u32);
    struct stat st;
    if (stat(TranslatePath(guestPath).readPath.string().c_str(), &st) != 0)
    {
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    WriteNetworkOpenInformation(ctx.r4.u32, st);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Read / write

static void CompleteIo(uint32_t eventHandle, uint32_t ioStatusBlock, uint32_t status, uint32_t information)
{
    SetIoStatus(ioStatusBlock, status, information);
    if (eventHandle != 0)
        SignalEventHandle(eventHandle);
}

KERNEL_FUNC(NtReadFile)
{
    // (HANDLE File, HANDLE Event, VOID* ApcRoutine, VOID* ApcContext, IO_STATUS_BLOCK* IoStatus,
    //  VOID* Buffer, ULONG Length, LARGE_INTEGER* ByteOffset)
    LOG_CALL(NtReadFile);
    auto* file = GetFile(ctx.r3.u32);
    if (file == nullptr || file->fd < 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r5.u32 != 0)
        fprintf(stderr, "[fichiers] NtReadFile avec APC non gere\n");

    uint64_t offset = ctx.r10.u32 != 0 ? __builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(ctx.r10.u32))) : file->position;
    ssize_t count = pread(file->fd, GuestToHost(ctx.r8.u32), ctx.r9.u32, offset);
    if (count < 0)
        count = 0;
    file->position = offset + uint64_t(count);

    uint32_t status = count == 0 && ctx.r9.u32 != 0 ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    CompleteIo(ctx.r4.u32, ctx.r7.u32, status, uint32_t(count));
    ctx.r3.u64 = status;
}

KERNEL_FUNC(NtWriteFile)
{
    // Same arguments as NtReadFile
    LOG_CALL(NtWriteFile);
    auto* file = GetFile(ctx.r3.u32);
    if (file == nullptr || file->fd < 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    uint64_t offset = ctx.r10.u32 != 0 ? __builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(ctx.r10.u32))) : file->position;
    ssize_t count = pwrite(file->fd, GuestToHost(ctx.r8.u32), ctx.r9.u32, offset);
    if (count < 0)
        count = 0;
    file->position = offset + uint64_t(count);
    CompleteIo(ctx.r4.u32, ctx.r7.u32, STATUS_SUCCESS, uint32_t(count));
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtFlushBuffersFile)
{
    LOG_CALL(NtFlushBuffersFile);
    if (auto* file = GetFile(ctx.r3.u32); file && file->fd >= 0)
        fsync(file->fd);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Information

enum FileInformationClass : uint32_t
{
    FileDirectoryInformation = 1,
    FileBasicInformation = 4,
    FileStandardInformation = 5,
    FileInternalInformation = 6,
    FilePositionInformation = 14,
    FileEndOfFileInformation = 20,
    FileNetworkOpenInformation = 34,
};

KERNEL_FUNC(NtQueryInformationFile)
{
    // (HANDLE File, IO_STATUS_BLOCK* IoStatus, VOID* Information, ULONG Length, FILE_INFORMATION_CLASS Class)
    LOG_CALL(NtQueryInformationFile);
    auto* file = GetFile(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }

    struct stat st{};
    if (file->fd >= 0)
        fstat(file->fd, &st);
    else
        stat(file->hostPath.string().c_str(), &st);

    uint32_t info = ctx.r5.u32;
    uint32_t written = 0;
    switch (ctx.r7.u32)
    {
    case FileNetworkOpenInformation:
        WriteNetworkOpenInformation(info, st);
        written = 0x38;
        break;
    case FileStandardInformation:
        StoreBE64(info + 0x00, (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull);
        StoreBE64(info + 0x08, uint64_t(st.st_size));
        StoreBE32(info + 0x10, 1);
        g_base[info + 0x14] = 0;
        g_base[info + 0x15] = file->directory;
        written = 0x18;
        break;
    case FilePositionInformation:
        StoreBE64(info, file->position);
        written = 8;
        break;
    case FileInternalInformation:
        StoreBE64(info, uint64_t(st.st_ino));
        written = 8;
        break;
    case FileBasicInformation:
    {
        uint64_t time = ToFiletime(ModifiedTime(st));
        for (uint32_t i = 0; i < 4; i++)
            StoreBE64(info + i * 8, time);
        StoreBE32(info + 0x20, file->directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        written = 0x28;
        break;
    }
    default:
        fprintf(stderr, "[fichiers] NtQueryInformationFile classe %u non geree\n", ctx.r7.u32);
        SetIoStatus(ctx.r4.u32, STATUS_INVALID_PARAMETER, 0);
        ctx.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }

    SetIoStatus(ctx.r4.u32, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtSetInformationFile)
{
    // (HANDLE File, IO_STATUS_BLOCK* IoStatus, VOID* Information, ULONG Length, FILE_INFORMATION_CLASS Class)
    LOG_CALL(NtSetInformationFile);
    auto* file = GetFile(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }

    uint32_t info = ctx.r5.u32;
    switch (ctx.r7.u32)
    {
    case FilePositionInformation:
        file->position = __builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(info)));
        break;
    case FileEndOfFileInformation:
        if (file->fd >= 0)
            ftruncate(file->fd, (__builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(info)))));
        break;
    default:
        fprintf(stderr, "[fichiers] NtSetInformationFile classe %u ignoree\n", ctx.r7.u32);
        break;
    }
    SetIoStatus(ctx.r4.u32, STATUS_SUCCESS, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtQueryVolumeInformationFile)
{
    // (HANDLE File, IO_STATUS_BLOCK* IoStatus, VOID* Information, ULONG Length, FS_INFORMATION_CLASS Class)
    LOG_CALL(NtQueryVolumeInformationFile);
    uint32_t info = ctx.r5.u32;
    memset(GuestToHost(info), 0, ctx.r6.u32);
    switch (ctx.r7.u32)
    {
    case 3: // FileFsSizeInformation: 8 GB free on a 16 GB volume, 16 KB clusters
        StoreBE64(info + 0x00, 0x100000);
        StoreBE64(info + 0x08, 0x80000);
        StoreBE32(info + 0x10, 32);
        StoreBE32(info + 0x14, 512);
        break;
    case 5: // FileFsAttributeInformation
    {
        StoreBE32(info + 0x00, 0x02); // case preserved names
        StoreBE32(info + 0x04, 255);
        const char name[] = "FATX";
        StoreBE32(info + 0x08, sizeof(name) - 1);
        memcpy(GuestToHost(info + 0x0C), name, sizeof(name) - 1);
        break;
    }
    default:
        break;
    }
    SetIoStatus(ctx.r4.u32, STATUS_SUCCESS, ctx.r6.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}

static bool MatchesMask(const std::string& name, const std::string& mask)
{
    // Wildcards '*' and '?', case insensitive
    if (mask.empty() || mask == "*" || mask == "*.*")
        return true;
    std::string n = ToLower(name), m = ToLower(mask);
    size_t ni = 0, mi = 0, star = std::string::npos, mark = 0;
    while (ni < n.size())
    {
        if (mi < m.size() && (m[mi] == '?' || m[mi] == n[ni]))
        {
            ni++;
            mi++;
        }
        else if (mi < m.size() && m[mi] == '*')
        {
            star = mi++;
            mark = ni;
        }
        else if (star != std::string::npos)
        {
            mi = star + 1;
            ni = ++mark;
        }
        else
        {
            return false;
        }
    }
    while (mi < m.size() && m[mi] == '*')
        mi++;
    return mi == m.size();
}

KERNEL_FUNC(NtQueryDirectoryFile)
{
    // (HANDLE File, HANDLE Event, VOID* ApcRoutine, VOID* ApcContext, IO_STATUS_BLOCK* IoStatus,
    //  VOID* FileInformation, ULONG Length, ANSI_STRING* FileMask, BOOLEAN RestartScan)
    LOG_CALL(NtQueryDirectoryFile);
    auto* file = GetFile(ctx.r3.u32);
    if (file == nullptr || !file->directory)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }

    bool restart = PPC_LOAD_U32(ctx.r1.u32 + 0x54) != 0;
    if (!file->listed || restart)
    {
        std::string mask = ctx.r10.u32 != 0 ? ReadAnsiString(ctx.r10.u32) : "*";
        file->entries.clear();
        std::error_code error;
        for (auto& entry : fs::directory_iterator(file->hostPath, error))
        {
            if (MatchesMask(entry.path().filename().string(), mask))
                file->entries.push_back(entry);
        }
        file->nextEntry = 0;
        file->listed = true;
    }

    if (file->nextEntry >= file->entries.size())
    {
        SetIoStatus(ctx.r7.u32, STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = STATUS_NO_MORE_FILES;
        return;
    }

    // One FILE_DIRECTORY_INFORMATION entry per call
    auto& entry = file->entries[file->nextEntry++];
    std::string name = entry.path().filename().string();
    uint32_t info = ctx.r8.u32;
    uint32_t size = 0x40 + uint32_t(name.size());
    if (size > ctx.r9.u32)
    {
        file->nextEntry--;
        ctx.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }

    struct stat st{};
    stat(entry.path().string().c_str(), &st);
    memset(GuestToHost(info), 0, size);
    StoreBE32(info + 0x00, 0); // NextEntryOffset
    StoreBE32(info + 0x04, uint32_t(file->nextEntry - 1));
    uint64_t time = ToFiletime(ModifiedTime(st));
    for (uint32_t i = 0; i < 4; i++)
        StoreBE64(info + 0x08 + i * 8, time);
    bool directory = S_ISDIR(st.st_mode);
    StoreBE64(info + 0x28, directory ? 0 : uint64_t(st.st_size));
    StoreBE64(info + 0x30, directory ? 0 : (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull);
    StoreBE32(info + 0x38, directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    StoreBE32(info + 0x3C, uint32_t(name.size()));
    memcpy(GuestToHost(info + 0x40), name.data(), name.size());

    CompleteIo(ctx.r4.u32, ctx.r7.u32, STATUS_SUCCESS, size);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtClose)
{
    LOG_CALL(NtClose);
    if (!CloseFileHandle(ctx.r3.u32))
        CloseObjectHandle(ctx.r3.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}
