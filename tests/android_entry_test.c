/* Stubs for host/android_main.c in tests/android_entry_test.py: the host prints its arguments and
 * the setting XI_ENTRY_TEST_ENV names, and each entry point returns its own exit code. No game client
 * is linked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int SDL_main(int argc, char** argv);

void gfx_worker_shutdown(void)
{
    puts("test:shutdown");
}

int xi_host_main(int argc, char** argv)
{
    assert(argv[argc] == NULL);
    for (int i = 1; i < argc; ++i)
    {
        assert(strncmp(argv[i], "--android-", 10));
        printf("test:arg=%s\n", argv[i]);
    }
    assert(!strcmp(getenv("FFXI_NATIVE_GEOMETRY"), getenv("FFXI_ANDROID_NATIVE_GEOMETRY")));
    const char* key = getenv("XI_ENTRY_TEST_ENV");
    const char* value = key ? getenv(key) : NULL;
    printf("test:env=%s\n", value ? value : "<unset>");
    return 42;
}

int xi_gfx_test_main(int argc, char** argv)
{
    assert(argc == 2 && argv[argc] == NULL);
    assert(!strcmp(argv[0], "gfx_test") && !strcmp(argv[1], "--window"));
    return 43;
}

int xi_format_test_main(void)
{
    return 44;
}

int xi_state_test_main(void)
{
    return 45;
}

int xi_async_test_main(void)
{
    return 46;
}

int xi_area_test_main(void)
{
    return 47;
}

int xi_pass_plan_test_main(void)
{
    return 48;
}

int xi_visibility_storage_test_main(void)
{
    return 49;
}

int main(int argc, char** argv)
{
    return SDL_main(argc, argv);
}
