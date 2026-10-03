// The Vulkan Memory Allocator's implementation (third_party/vma), for gfx_vulkan.c: Vulkan's functions
// through volk (loaded at run time), handed to it by gfx_init.
#include "volk.h"
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "vk_mem_alloc.h"
