/* vfs.c's guest paths on a POSIX host: a mounted install whose files differ from the game's names
 * only in case (a private-server install's "ROM/119/50.dat" for the game's "ROM\119\50.DAT") is
 * found as Windows would find it; a path that is not there in any case keeps the game's name.
 *
 *   python3 tools/build_posix.py vfstest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plat.h"
#include "vfs.h"

static int g_failed;

static void touch(const char* path)
{
    FILE* f = fopen(path, "w");
    if (f)
        fclose(f);
}

static void expect(const char* guest, const char* root, const char* want)
{
    char host[1400], full[1400];
    snprintf(full, sizeof full, "%s%s", root, want);
    if (!vfs_host_path(guest, host, sizeof host) || strcmp(host, full))
    {
        printf("FAIL %s: %s, not %s\n", guest, host, full);
        g_failed = 1;
    }
}

int main(void)
{
    char root[] = "/tmp/vfs_test.XXXXXX", path[512];
    if (!mkdtemp(root))
        return 1;
    const char* dirs[] = { "/ROM", "/ROM/119", "/Rom2", "/Rom2/7" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i)
        snprintf(path, sizeof path, "%s%s", root, dirs[i]), mkdir(path, 0700);
    const char* files[] = { "/ROM/119/50.dat", "/ROM/119/51.DAT", "/Rom2/7/12.dat" };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i)
        snprintf(path, sizeof path, "%s%s", root, files[i]), touch(path);

    vfs_mount("C:\\Game", root);
    expect("C:\\Game\\ROM\\119\\51.DAT", root, "/ROM/119/51.DAT");  /* as the game names it */
    expect("C:\\Game\\ROM\\119\\50.DAT", root, "/ROM/119/50.dat");  /* the file in another case */
    expect("C:\\Game\\ROM2\\7\\12.DAT", root, "/Rom2/7/12.dat");    /* a folder in another case too */
    expect("C:\\Game\\rom\\119\\NEW.DAT", root, "/ROM/119/NEW.DAT"); /* not there: the game's name */
    expect("C:\\Game\\ROM\\9\\1.DAT", root, "/ROM/9/1.DAT");        /* no such folder in any case */

    for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i)
        snprintf(path, sizeof path, "%s%s", root, files[i]), unlink(path);
    for (size_t i = sizeof dirs / sizeof dirs[0]; i-- > 0;)
        snprintf(path, sizeof path, "%s%s", root, dirs[i]), rmdir(path);
    rmdir(root);
    puts(g_failed ? "vfs_test: FAILED" : "vfs_test: ok");
    return g_failed;
}
