// Linux: the game is built on a recent distribution but must start on older ones (glibc 2.35, Ubuntu 22.04).
// C library functions given new symbol versions since then (used by the game, SDL3 and the C++ runtime, both
// linked in) are redirected here by the linker (-Wl,--wrap=<name>, list in CMakeLists.txt): they call the oldest
// version, which every glibc has, or are implemented here when they did not exist yet.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <sys/random.h>

#define OLD_VERSION(name) __asm__(".symver " #name "_glibc_2_2_5, " #name "@GLIBC_2.2.5")

extern "C"
{
    double fmod_glibc_2_2_5(double, double);
    float fmodf_glibc_2_2_5(float, float);
    float sqrtf_glibc_2_2_5(float);
    float acosf_glibc_2_2_5(float);
    float asinf_glibc_2_2_5(float);
    float atan2f_glibc_2_2_5(float, float);
    float log10f_glibc_2_2_5(float);
    double sinh_glibc_2_2_5(double);
    double cosh_glibc_2_2_5(double);
    long strtol_glibc_2_2_5(const char*, char**, int);
    long long strtoll_glibc_2_2_5(const char*, char**, int);
    unsigned long strtoul_glibc_2_2_5(const char*, char**, int);
    unsigned long long strtoull_glibc_2_2_5(const char*, char**, int);
    long wcstol_glibc_2_2_5(const wchar_t*, wchar_t**, int);
    int vsscanf_glibc_2_2_5(const char*, const char*, va_list);
    int vfscanf_glibc_2_2_5(FILE*, const char*, va_list);
}

OLD_VERSION(fmod);
OLD_VERSION(fmodf);
OLD_VERSION(sqrtf);
OLD_VERSION(acosf);
OLD_VERSION(asinf);
OLD_VERSION(atan2f);
OLD_VERSION(log10f);
OLD_VERSION(sinh);
OLD_VERSION(cosh);
OLD_VERSION(strtol);
OLD_VERSION(strtoll);
OLD_VERSION(strtoul);
OLD_VERSION(strtoull);
OLD_VERSION(wcstol);
OLD_VERSION(vsscanf);
OLD_VERSION(vfscanf);

template <typename Char>
static size_t Length(const Char* s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

// strlcpy/strlcat (BSD, glibc 2.38): copy into a buffer of `size`, always terminated, return the wanted length
template <typename Char>
static size_t CopyTerminated(Char* dst, const Char* src, size_t size)
{
    size_t length = Length(src);
    if (size != 0)
    {
        size_t count = length < size - 1 ? length : size - 1;
        memcpy(dst, src, count * sizeof(Char));
        dst[count] = 0;
    }
    return length;
}

template <typename Char>
static size_t AppendTerminated(Char* dst, const Char* src, size_t size)
{
    size_t used = 0;
    while (used < size && dst[used])
        used++;
    if (used == size)
        return size + Length(src);
    return used + CopyTerminated(dst + used, src, size - used);
}

extern "C"
{
    double __wrap_fmod(double x, double y) { return fmod_glibc_2_2_5(x, y); }
    float __wrap_fmodf(float x, float y) { return fmodf_glibc_2_2_5(x, y); }
    float __wrap_sqrtf(float x) { return sqrtf_glibc_2_2_5(x); }
    float __wrap_acosf(float x) { return acosf_glibc_2_2_5(x); }
    float __wrap_asinf(float x) { return asinf_glibc_2_2_5(x); }
    float __wrap_atan2f(float y, float x) { return atan2f_glibc_2_2_5(y, x); }
    float __wrap_log10f(float x) { return log10f_glibc_2_2_5(x); }
    double __wrap_sinh(double x) { return sinh_glibc_2_2_5(x); }
    double __wrap_cosh(double x) { return cosh_glibc_2_2_5(x); }

    // C23 versions (glibc 2.38) only add the "0b" binary prefix, which nothing here parses
    long __wrap___isoc23_strtol(const char* s, char** end, int base) { return strtol_glibc_2_2_5(s, end, base); }
    long long __wrap___isoc23_strtoll(const char* s, char** end, int base) { return strtoll_glibc_2_2_5(s, end, base); }
    unsigned long __wrap___isoc23_strtoul(const char* s, char** end, int base) { return strtoul_glibc_2_2_5(s, end, base); }
    unsigned long long __wrap___isoc23_strtoull(const char* s, char** end, int base) { return strtoull_glibc_2_2_5(s, end, base); }
    long __wrap___isoc23_wcstol(const wchar_t* s, wchar_t** end, int base) { return wcstol_glibc_2_2_5(s, end, base); }
    int __wrap___isoc23_vsscanf(const char* s, const char* format, va_list args) { return vsscanf_glibc_2_2_5(s, format, args); }
    int __wrap___isoc23_sscanf(const char* s, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        int result = vsscanf_glibc_2_2_5(s, format, args);
        va_end(args);
        return result;
    }
    int __wrap___isoc23_fscanf(FILE* file, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        int result = vfscanf_glibc_2_2_5(file, format, args);
        va_end(args);
        return result;
    }

    size_t __wrap_strlcpy(char* dst, const char* src, size_t size) { return CopyTerminated(dst, src, size); }
    size_t __wrap_strlcat(char* dst, const char* src, size_t size) { return AppendTerminated(dst, src, size); }
    size_t __wrap_wcslcpy(wchar_t* dst, const wchar_t* src, size_t size) { return CopyTerminated(dst, src, size); }
    size_t __wrap_wcslcat(wchar_t* dst, const wchar_t* src, size_t size) { return AppendTerminated(dst, src, size); }

    // arc4random (glibc 2.36): getrandom exists since glibc 2.25
    unsigned int __wrap_arc4random()
    {
        unsigned int value;
        while (getrandom(&value, sizeof(value), 0) != sizeof(value))
        {
        }
        return value;
    }
}
