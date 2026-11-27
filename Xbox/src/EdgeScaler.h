#pragma once
// "Max HD": rounds off the blocky edges of the Super Nintendo picture before the TV scales it up.
//
// The picture is run through Scale2x (the EPX / AdvMAME2x edge rule) twice, giving 4 times the width and height
// with diagonal edges and curves filled in instead of stair steps; the GPU then smooths the last step to the TV's
// size. Written here from the published rule, so it carries no other project's license.

#include <cstdint>
#include <vector>

namespace edge {

/// Makes `out` (w*2 by h*2) from `in` (w by h), both 0xAARRGGBB.
void Scale2x(const uint32_t* in, uint32_t w, uint32_t h, uint32_t* out);

/// Four times bigger (Scale2x twice). `scratch` and `out` are resized as needed.
void Scale4x(const std::vector<uint32_t>& in, uint32_t w, uint32_t h, std::vector<uint32_t>& scratch, std::vector<uint32_t>& out);

} // namespace edge
