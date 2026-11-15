/*
 * POSIX implementation of the abstract I/O layer lilv's vendored sources use.
 *
 * The Android build gets these from 3rd_party/lilv/src/abstract_io.c, which is
 * the Android flavour of the same file (AAssetManager on top of stdio) and is
 * not usable here: it is compiled below its own `#define fopen abstract_fopen`
 * macros. The host build therefore provides the same symbols straight on top of
 * stdio/dirent, which is all lilv requires outside Android.
 *
 * Signatures are the ones declared in 3rd_party/lilv/src/abstract_io.h, copied
 * instead of included so the redirection macros of that header stay out of this
 * translation unit.
 */

#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

void abstract_set_io_context(void* ioContext)
{
    (void)ioContext;
}

void* abstract_fopen(const char* path, const char* mode)
{
    return fopen(path, mode);
}

int abstract_fread(void* ptr, size_t size, size_t count, void* stream)
{
    return (int)fread(ptr, size, count, (FILE*)stream);
}

int abstract_fwrite(const void* ptr, size_t size, size_t count, void* stream)
{
    return (int)fwrite(ptr, size, count, (FILE*)stream);
}

int abstract_error_vfprintf(const char* format, va_list arg)
{
    return vfprintf(stderr, format, arg);
}

int abstract_error_fprintf(const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    const int written = vfprintf(stderr, format, ap);
    va_end(ap);
    return written;
}

int abstract_ferror(void* stream)
{
    return ferror((FILE*)stream);
}

int abstract_fclose(void* stream)
{
    return fclose((FILE*)stream);
}

int abstract_getc(void* stream)
{
    return getc((FILE*)stream);
}

int abstract_ftell(void* stream)
{
    return (int)ftell((FILE*)stream);
}

int abstract_fseek(void* stream, long offset, int origin)
{
    return fseek((FILE*)stream, offset, origin);
}

void abstract_dir_for_each(const char* path,
                           void*       data,
                           void (*f)(const char* path, const char* name, void* data))
{
    DIR* dir = opendir(path);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        f(path, entry->d_name, data);
    }

    closedir(dir);
}
