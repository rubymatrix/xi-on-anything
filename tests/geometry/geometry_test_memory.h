/* The tests' 4 GB guest window: reserved, with only the pages a test uses committed. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned char* geometry_test_reserve(void)
{
    return (unsigned char*)VirtualAlloc(NULL, (size_t)1 << 32, MEM_RESERVE, PAGE_NOACCESS);
}
static void geometry_test_commit(unsigned char* base, uint32_t address, size_t bytes)
{
    if (!VirtualAlloc(base + address, bytes, MEM_COMMIT, PAGE_READWRITE))
        abort();
}
static void geometry_test_release(unsigned char* base)
{
    VirtualFree(base, 0, MEM_RELEASE);
}
#else
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
static unsigned char* geometry_test_reserve(void)
{
    void* p = mmap(NULL, (size_t)1 << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : (unsigned char*)p;
}
static void geometry_test_commit(unsigned char* base, uint32_t address, size_t bytes)
{
    size_t size = (size_t)sysconf(_SC_PAGESIZE), lo = (size_t)address & ~(size - 1);
    size_t end = ((size_t)address + bytes + size - 1) & ~(size - 1);
    if (mprotect(base + lo, end - lo, PROT_READ | PROT_WRITE))
        abort();
}
static void geometry_test_release(unsigned char* base)
{
    munmap(base, (size_t)1 << 32);
}
#endif
