// Include the public header first so standard headers cannot hide missing type dependencies.
#include <liblockd_dll/liblockd_dll.h>

#include <stddef.h>
#include <stdint.h>

/* The public header must stand alone in C and link the same exports as C++ callers. */
// Called from dll_test.cpp to verify linkage across the C/C++ boundary.
// NOLINTNEXTLINE(misc-use-internal-linkage)
bool dll_c_header_test(void)
{
    char version[512];
    void* (*const volatile make_lock)(void*) = create_lock;
    bool (*const volatile acquire)(void*) = lock;
    int32_t (*const volatile try_acquire)(void*) = try_lock;
    bool (*const volatile release)(void*) = unlock;
    destroy_mutex(NULL);
    destroy_lock(NULL);

    // ReSharper disable once CppRedundantBooleanExpressionArgument
    return make_lock && acquire && try_acquire && release &&
           liblockd_version(version, sizeof version) && version[0] != '\0';
}

// Exercise the fixed-width ABI from a C translation unit, including on 32-bit x86.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void* dll_c_create_mutex(
        const char* const server, const uint16_t port, const int64_t timeout, const char* const name
)
{
    void* (*const create)(const char*, uint16_t, int64_t, const char*) = create_mutex;
    return create(server, port, timeout, name);
}
