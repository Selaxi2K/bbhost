// Which draws start a command buffer of their own on lavapipe.
//
// On lavapipe (Mesa's llvmpipe Vulkan driver), a draw that writes no channel
// of a bound colour target - the write mask empty because the pixel shader
// does not export it - while it blends into the targets it does write puts its
// undefined output into the masked target as well: the G-buffer's puddle
// decals export six of the seven targets bound, blend into them, and leave NaN
// in the seventh. It happens only after other draws in the same command
// buffer. A new rendering instance in that command buffer does not change it,
// nor does binding the pipeline, the descriptor sets and every dynamic state
// again; submitting what was recorded before the draw does, with or without
// waiting for it. NVIDIA's and RADV's drivers draw such a draw correctly and
// never split.
//
// BBHOST_LVP_MASKED_SPLIT=0 records such draws like any other.
#pragma once

#include <cstdint>

namespace gpu {

// `bound[t]`: a colour target is bound at slot t; `mask[t]`: the draw's write
// mask for it (CB_TARGET_MASK cut to the pixel shader's exports); `blend[t]`:
// blending is on for it (and its format blends).
inline bool lvp_split_wanted(const bool bound[8], const std::uint32_t mask[8], const bool blend[8]) {
    bool masked = false, blending = false;
    for (int t = 0; t < 8; ++t) {
        if (!bound[t]) continue;
        if (!(mask[t] & 0xfu)) {
            masked = true;
        } else if (blend[t]) {
            blending = true;
        }
    }
    return masked && blending;
}

}  // namespace gpu
