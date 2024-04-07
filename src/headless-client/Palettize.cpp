// SPDX-License-Identifier: GPL-3.0-or-later

#include "Palettize.h"
#include <algorithm>
#include <execution>
#include <vector>

struct Lab {
	float L;
	float a;
	float b;
};

struct RGB {
	float r;
	float g;
	float b;
};

float sRgbGamaToLinear(float c)
{
	return .04045f < c ? powf((c + .055f) / 1.055f, 2.4f) : c / 12.92f;
}

RGB sRgbGamaToLinear(RGB p)
{
	return {
		sRgbGamaToLinear(p.r), sRgbGamaToLinear(p.g), sRgbGamaToLinear(p.b)};
}

Lab srgbToOklab(RGB c)
{
	c = sRgbGamaToLinear(c);

	float l = 0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b;
	float m = 0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b;
	float s = 0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b;

	float l_ = std::cbrtf(l);
	float m_ = std::cbrtf(m);
	float s_ = std::cbrtf(s);

	return {
		0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
		1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
		0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
	};
}

RGB DP_UPixel8ToRGB(DP_UPixel8 c)
{
	return {c.bytes.r / 255.f, c.bytes.g / 255.f, c.bytes.b / 255.f};
}

DP_UPixel8 makeDP_UPixel8(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	return DP_UPixel8{.bytes = {.b = b, .g = g, .r = r, .a = a}};
}

// This reduces the contribution of luminance because generally hue matters a
// lot more for pixel art. This is enough to get a greyscale gradient to only
// use greyscale colors in the 2023 r/place palette.
float squaredAdjustedDist(Lab a, Lab b)
{
	return std::powf((a.L - b.L) / 1.25f, 2) + std::powf(a.a - b.a, 2) +
		   std::powf(a.b - b.b, 2);
}

void palettize(std::span<DP_UPixel8> pixels, std::span<DP_UPixel8> palette)
{
	std::vector<Lab> paletteOklab;
	for(const auto &paletteEntry : palette) {
		// Oklab handles (0, 0, 0) poorly, considering it much farther away from
		// nearby greyscale colors. Here we pretend that it's (1, 1, 1) instead,
		// which largely fixes this issue.
		if(paletteEntry == makeDP_UPixel8(0, 0, 0, 255))
			paletteOklab.push_back(
				srgbToOklab(DP_UPixel8ToRGB(makeDP_UPixel8(1, 1, 1, 255))));
		else
			paletteOklab.push_back(srgbToOklab(DP_UPixel8ToRGB(paletteEntry)));
	}

	// TODO: Not all standard libraries have a parallel implementation of the
	//       parallel algorithms yet. This should use something else if the
	//       perf starts to matter.
	std::transform(
		std::execution::par_unseq, pixels.begin(), pixels.end(), pixels.begin(),
		[&](DP_UPixel8 pixel) noexcept {
			if(pixel.bytes.a <= 127)
				return DP_UPixel8{.color = 0};
			for(auto paletteEntry : palette)
				if(pixel == paletteEntry)
					return pixel;
			Lab okl = srgbToOklab(DP_UPixel8ToRGB(pixel));
			float best = std::numeric_limits<float>::max();
			size_t bestIndex = 0;
			for(size_t i = 0; i < paletteOklab.size(); ++i) {
				float dist = squaredAdjustedDist(okl, paletteOklab[i]);
				if(dist < best) {
					best = dist;
					bestIndex = i;
				}
			}
			return palette[bestIndex];
		});
}
