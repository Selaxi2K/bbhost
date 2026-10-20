#pragma once

#include <cstdint>

#include "guest_abi.h"

// The effects' node update, sub_2c4df80 (decomp/sfx/node_update.cpp): the
// game's sequence as ours, for the smooth steps to split.
void decomp_sfx_node_update_add();

namespace sfx_node {

// What one node update is given. The game gives both kinds one step, with no
// offsets, every update; the smooth effect steps' frame domain gives the
// counted parts (a plain node's definition switch, pick and definition time,
// a staged node's state machine and render items, the node's time) the whole
// step's on a frame with one and nothing on a frame between, and the
// container calls the frame's own step with the node's time put that far past
// its whole step for each call.
struct Steps {
    float counted = 0.0f;  // the counted parts' step (the whole step's, times the instance's scale)
    float frame = 0.0f;    // the container calls' step (the frame's, times the scale)
    float from = 0.0f;     // the node's time offset for the container calls: the frame's start past its whole step, scaled
    float into = 0.0f;     // a plain node's definition-time offset for its second call: the frame's end past it, scaled
    bool tick = true;      // a frame with a whole step: the counted parts run, a dead node is freed
};

// The node update with those steps; false when the node is to be freed.
GUEST_ABI __attribute__((no_stack_protector)) std::uint8_t update(std::uint64_t node, const Steps& steps, std::uint64_t definition,
                                                                  std::uint64_t context);

}  // namespace sfx_node

// sub_2c4df80 itself, the game's signature: one step for both kinds, no offsets.
GUEST_ABI __attribute__((no_stack_protector)) std::uint8_t sfx_node_update(std::uint64_t node, float* step, std::uint64_t definition,
                                                                           std::uint64_t context);
