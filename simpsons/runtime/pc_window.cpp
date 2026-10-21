// Windows and Linux: full screen without borders on the desktop (SDL), Windows file access helpers; counterpart
// of mac_window.mm
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include <cstdint>
#include <sys/types.h>

bool PlatformIsFullscreen(SDL_Window* window)
{
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void PlatformToggleFullscreen(SDL_Window* window)
{
    SDL_SetWindowFullscreen(window, !PlatformIsFullscreen(window));
}

int PlatformMenuCount()
{
    return 0;
}

#ifdef _WIN32
// Positioned read or write through the file's handle: does not move a shared file position, so several guest
// threads can read the same file at once (pread/pwrite on macOS)
ssize_t WindowsPositionedIo(int fd, void* buffer, uint32_t size, uint64_t offset, bool write)
{
    OVERLAPPED overlapped{};
    overlapped.Offset = DWORD(offset);
    overlapped.OffsetHigh = DWORD(offset >> 32);
    DWORD count = 0;
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    BOOL ok = write ? WriteFile(handle, buffer, size, &count, &overlapped) : ReadFile(handle, buffer, size, &count, &overlapped);
    if (!ok)
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return ssize_t(count);
}
#endif
