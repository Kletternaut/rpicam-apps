/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace preview_resample
{
// Separable, coverage-weighted reduction of an 8-bit plane. Interpolation is
// used only for enlargement. Coordinates describe pixel centres, and every
// fixed-point weight row sums to exactly 65536 so flat fields stay unchanged.
class PlaneScaler
{
public:
	const std::vector<uint8_t> &resize(const uint8_t *source, int width, int height,
									 int stride, int outWidth, int outHeight)
	{
		if (width != width_ || height != height_ || outWidth != outWidth_ || outHeight != outHeight_)
		{
			width_ = width;
			height_ = height;
			outWidth_ = outWidth;
			outHeight_ = outHeight;
			xWeights_ = makeAxis(width, outWidth);
			yWeights_ = makeAxis(height, outHeight);
			horizontal_.resize(size_t(outWidth) * height);
			result_.resize(size_t(outWidth) * outHeight);
		}
		for (int y = 0; y < height; ++y)
		{
			const uint8_t *row = source + size_t(y) * stride;
			uint8_t *dest = horizontal_.data() + size_t(y) * outWidth;
			if (width == outWidth)
				std::memcpy(dest, row, width);
			else
				for (int x = 0; x < outWidth; ++x)
				{
					uint32_t value = 32768;
					for (const Tap &tap : xWeights_[x])
						value += row[tap.index] * tap.weight;
					dest[x] = uint8_t(value >> 16);
				}
		}
		for (int y = 0; y < outHeight; ++y)
			for (int x = 0; x < outWidth; ++x)
			{
				uint32_t value = 32768;
				for (const Tap &tap : yWeights_[y])
					value += horizontal_[size_t(tap.index) * outWidth + x] * tap.weight;
				result_[size_t(y) * outWidth + x] = uint8_t(value >> 16);
			}
		return result_;
	}

private:
	struct Tap { int index; uint32_t weight; };
	using Axis = std::vector<std::vector<Tap>>;
	static Axis makeAxis(int source, int dest)
	{
		Axis axis(dest);
		const double scale = double(source) / dest;
		for (int d = 0; d < dest; ++d)
		{
			if (source > dest)
			{
				const double begin = d * scale, end = (d + 1) * scale;
				uint32_t previous = 0;
				for (int s = int(std::floor(begin)); s < std::min(source, int(std::ceil(end))); ++s)
				{
					uint32_t cumulative = uint32_t(std::lround((std::min(end, double(s + 1)) - begin) / scale * 65536));
					axis[d].push_back({ s, cumulative - previous });
					previous = cumulative;
				}
			}
			else
			{
				double position = std::clamp((d + 0.5) * scale - 0.5, 0.0, double(source - 1));
				int s = int(position);
				uint32_t right = uint32_t(std::lround((position - s) * 65536));
				axis[d].push_back({ s, 65536 - right });
				if (right)
					axis[d].push_back({ std::min(source - 1, s + 1), right });
			}
		}
		return axis;
	}
	int width_ = 0, height_ = 0, outWidth_ = 0, outHeight_ = 0;
	Axis xWeights_, yWeights_;
	std::vector<uint8_t> horizontal_, result_;
};
} // namespace preview_resample
