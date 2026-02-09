#pragma once

#include <vector>

#include "CImg.h"

namespace HomeCompa::FliLib
{

class Canny
{
public:
	struct Rect
	{
		int16_t top { 0 }, left { 0 }, bottom { 0 }, right { 0 };

		int16_t width() const noexcept
		{
			return right - left;
		}

		int16_t height() const noexcept
		{
			return bottom - top;
		}
	};

public:
	Canny(int gaussianFilterSize = 3, double gaussianSigma = 1, int thresholdLow = 20, int thresholdHigh = 40);
	Rect Process(const cimg_library::CImg<unsigned char>& img) const;

private:
	const int m_thresholdLow, m_thresholdHigh;

	const std::vector<std::vector<double>> m_gaussianFilter;
};

} // namespace HomeCompa::FliLib
