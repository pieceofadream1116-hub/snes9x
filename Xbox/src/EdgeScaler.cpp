#include "EdgeScaler.h"

namespace edge {

void Scale2x(const uint32_t* in, uint32_t w, uint32_t h, uint32_t* out)
{
	const uint32_t outW = w * 2;
	for(uint32_t y = 0; y < h; y++) {
		const uint32_t* row = in + (size_t)y * w;
		const uint32_t* up = y > 0 ? row - w : row;
		const uint32_t* down = y + 1 < h ? row + w : row;
		uint32_t* out0 = out + (size_t)(y * 2) * outW;
		uint32_t* out1 = out0 + outW;
		for(uint32_t x = 0; x < w; x++) {
			const uint32_t left = row[x > 0 ? x - 1 : x];
			const uint32_t right = row[x + 1 < w ? x + 1 : x];
			const uint32_t centre = row[x];
			const uint32_t b = up[x];
			const uint32_t d = left;
			const uint32_t f = right;
			const uint32_t hh = down[x];
			uint32_t e0 = centre, e1 = centre, e2 = centre, e3 = centre;
			// Only where two neighbours meet at a corner (and the other two differ) is the corner filled in
			if(b != hh && d != f) {
				if(d == b) e0 = d;
				if(b == f) e1 = f;
				if(d == hh) e2 = d;
				if(hh == f) e3 = f;
			}
			out0[x * 2] = e0;
			out0[x * 2 + 1] = e1;
			out1[x * 2] = e2;
			out1[x * 2 + 1] = e3;
		}
	}
}

void Scale4x(const std::vector<uint32_t>& in, uint32_t w, uint32_t h, std::vector<uint32_t>& scratch, std::vector<uint32_t>& out)
{
	scratch.resize((size_t)w * h * 4);
	out.resize((size_t)w * h * 16);
	Scale2x(in.data(), w, h, scratch.data());
	Scale2x(scratch.data(), w * 2, h * 2, out.data());
}

} // namespace edge
