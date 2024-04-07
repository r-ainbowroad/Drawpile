// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_CLIENT_PALETTIZE_H
#define HEADLESS_CLIENT_PALETTIZE_H

#include <cmath>
#include <span>
extern "C" {
#include <dpengine/pixels.h>
}

void palettize(std::span<DP_UPixel8> pixels, std::span<DP_UPixel8> palette);

inline bool operator==(DP_UPixel8 a, DP_UPixel8 b)
{
	return a.color == b.color;
}

#endif // HEADLESS_CLIENT_PALETTIZE_H
