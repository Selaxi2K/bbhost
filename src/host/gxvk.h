// GX on Vulkan, stage 1: the engine's GX buffers as Vulkan buffers.
//
// GX, the engine's Direct3D 11-shaped graphics layer, keeps its buffers in
// guest memory, and a draw reaches a buffer's bytes through the address its
// records carry: the copy versions, the buffer shadow's mirrors and
// snapshots, the constant-buffer snapshots, the import (render.cpp
// plan_vertex_input, resolve_stage_buffers). Stage 1 makes a GX buffer a
// Vulkan buffer when GX creates it, and the draw carries that buffer's handle
// instead of an address to resolve.
//
// Its first part is the census (BBHOST_GXVK_CENSUS=1, or BBHOST_GXVK_BUFFERS=
// census), which changes nothing the game or the renderer does and measures
// what the stage rests on:
// - GX's buffers by usage, bind flags, CPU access and initial data, the Maps
//   and updates each kind takes, and how they die;
// - every vertex record, index buffer and constant buffer a draw binds, by
//   what holds the bytes: an immutable GX buffer, a dynamic one's own memory,
//   a renamed copy in GX's upload heap, a GPU-written one, or no GX object;
// - GX's upload heap (the one region the device gives its block pool at
//   creation, 0x256b0f0 -> 0x2aa9b50): its size, its peak use, and whether
//   every range in it is handed out again only after the GPU finished with
//   the work that bound it (the release proof, below);
// - the spare bytes of GX's objects, which their creators never write: a
//   canary written there at creation must be found intact wherever the
//   object is used and when it dies.
//
// The release proof has two halves. (i) When the heap hands a range out, no
// submission that bound any of it may still be running (the command
// processor stamps each 4 KiB page with the submission it binds the page in;
// the hand-out compares those stamps with the completed submissions). (ii)
// When the command processor binds a range, the range must not have been
// handed out again since the GX call that recorded the binding (each page's
// hand-out sequence against the draw's stamp at its call). Both counts must
// be 0 for a renamed copy to be bound in place without a snapshot.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gxvk {

// BBHOST_GXVK_BUFFERS: 0 (the default) today's path; 1 GX buffers bound by
// their handles; 2 both, compared; "census" the census alone (as
// BBHOST_GXVK_CENSUS=1, which can join the others). Read once.
enum class Buffers : int { kOff = 0, kOn = 1, kCompare = 2 };
Buffers buffers_mode();
bool census();

// A GX buffer holder's spare dwords: +0x14, the reference count's upper half
// (every locked access GX makes to the count is to the dword at +0x10), and
// +0x44, which the buffer creator (0x2565c10) copies with the resource struct
// from a stack slot it never initialises. A texture holder has the first
// only (its resource struct is written whole); a shader-resource view has
// +0x48..+0x4f, an unordered-access view +0x44..+0x4f (their record builders,
// 0x257a5d0 and 0x257a8b0, write the T# and the description and nothing
// after them).
constexpr std::uint64_t kHolderSpareA = 0x14, kHolderSpareB = 0x44;
constexpr std::uint64_t kSrvSpare = 0x48, kUavSpare = 0x44;

// --- The census's hooks. Each is a no-op unless census() is on. ---

// The registry's (engine/gx_resources.cpp), on the thread that called GX.
void census_buffer_made(std::uint64_t holder, bool initial_data);
void census_buffer_mapped(std::uint64_t holder, std::uint32_t type, std::uint64_t mapped, std::uint32_t bytes, bool ok, bool own);
void census_buffer_updated(std::uint64_t holder);
void census_buffer_gone(std::uint64_t holder);
void census_texture_made(std::uint64_t holder);
void census_texture_gone(std::uint64_t holder);
// kind: 3 shader-resource view, 5 unordered-access view.
void census_view_made(std::uint64_t view, int kind);
// The block pool's region (0x2aa9b50's base and size).
void census_heap(std::uint64_t pool, std::uint64_t base, std::uint64_t bytes);
// A hand-out by the block pool (0x2aa9d50 / 0x2aa9fa0 returned `node`; the
// range is [node+8, +bytes)).
void census_handout(std::uint64_t pool, std::uint64_t node, std::uint64_t bytes);
// The ring allocator's hand-outs (0x2adb1b0).
void census_ring(std::uint64_t va, std::uint64_t bytes);

// The GX threads' (hle/gx_trace.cpp), at a draw's call.
// What a draw's call is stamped with for half (ii): the hand-outs so far.
std::uint64_t handout_seq();
// A constant buffer the call's records name: its holder and the address the
// records carry (its own memory, or the context's renamed copy).
void census_constant(std::uint64_t holder, std::uint64_t va, std::uint32_t bytes);
// A view a draw or dispatch binds (kind 3 shader-resource, 5 unordered-access):
// its spare bytes; a shader-resource view's texture holder's; an
// unordered-access view's buffer, as a binding of kUseUav.
void census_view_bound(std::uint64_t view, int kind);

// The command processor's (host/render.cpp), as it binds.
enum Use : int { kUseVertex, kUseIndex, kUseConstant, kUseUav, kUses };
// A vertex record or index buffer: the holder the GX call found it in (0:
// none - Scaleform, YEBIS, a table the game binds itself), the bytes bound,
// the draw's call stamp (0: none), the submission being recorded.
void census_binding(Use use, std::uint64_t holder, std::uint64_t va, std::uint64_t bytes, std::uint64_t call_seq,
                    std::uint64_t serial);
// A constant buffer as the command processor binds it (the GX side has
// classed it): only the heap's two checks, and the bindings of draws without
// GX records.
void census_constant_bound(std::uint64_t va, std::uint64_t bytes, std::uint64_t call_seq, std::uint64_t serial, bool gx_records);

// The exit report (gx_resources_report calls it).
void census_report();

}  // namespace gxvk
