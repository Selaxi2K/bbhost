#pragma once

#include "guest_abi.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct ElfImage;

void hle_register_all();
void hle_patch_guest(ElfImage* image);
void hle_set_guest_stack(void* base, std::size_t size);
// The boot thread's guest stack, [*lo, *hi); false before it is set.
bool hle_guest_boot_stack(std::uint64_t* lo, std::uint64_t* hi);
void* hle_thread_enter_guest();
void hle_thread_leave_guest(void* tcb);
std::uint64_t hle_make_stub(const std::string& name, const std::string& nid, std::uint64_t got_va);
void* hle_wrap_fn(void* fn);

GUEST_ABI int hle_unknown();
