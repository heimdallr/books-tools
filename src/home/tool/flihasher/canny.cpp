#include "canny.h"

#include <cmath>

#include <array>
#include <cassert>
#include <iostream>
#include <vector>

using namespace cimg_library;

namespace
{

template <std::integral U, std::floating_point V>
U round(const V v)
{
	return static_cast<U>(std::min(std::llround(v), static_cast<long long>(std::numeric_limits<U>::max())));
}

std::vector<std::vector<double>> createGaussianFilter(const int row, const int column, const double sigmaIn)
{
	assert((row & 1) && (column & 1));

	std::vector result(3, std::vector(3, -1.0));

	const auto constant = 2.0 * sigmaIn * sigmaIn;

	// Sum is for normalization
	auto sum = 0.0;

	for (int x = -row / 2; x <= row / 2; x++)
		for (int y = -column / 2; y <= column / 2; y++)
			sum += (result[x + row / 2][y + column / 2] = exp(-(x * x + y * y) / constant) / (M_PI * constant));

	// Normalize the Filter
	for (int i = 0; i < row; ++i)
		for (int j = 0; j < column; ++j)
			result[i][j] /= sum;

	return result;
}

template <typename T>
CImg<T> useFilter(CImg<T> src, const std::vector<std::vector<double>>& filterIn)
{
	const auto size = static_cast<unsigned int>(filterIn.size()) / 2;
	CImg<T>    result(src._width - 2 * size, src._height - 2 * size);
	for (auto i = size; i < src._width - size; ++i)
	{
		for (auto j = size; j < src._height - size; ++j)
		{
			double sum = 0;

			for (auto x = 0U, sz = static_cast<unsigned int>(filterIn.size()); x < sz; ++x)
				for (auto y = 0U; y < sz; ++y)
					sum += filterIn[x][y] * src(i + y - size, j + x - size);

			result(i - size, j - size) = round<T>(sum);
		}
	}

	return result;
}

std::pair<CImg<unsigned char>, CImg<float>> Sobel(const CImg<unsigned char>& src)
{
	//Sobel X Filter
	static constexpr std::array<std::array<double, 3>, 3> xFilter = {
		{
         { -1.0, 0, 1.0 },
         { -2.0, 0, 2.0 },
         { -1.0, 0, 1.0 },
		 }
	};

	//Sobel Y Filter
	static constexpr std::array<std::array<double, 3>, 3> yFilter = {
		{
         { 1.0, 2.0, 1.0 },
         { 0, 0, 0 },
         { -1.0, -2.0, -1.0 },
		 }
	};

	//Limit Size
	static constexpr auto size = static_cast<unsigned int>(xFilter.size()) / 2;

	CImg<unsigned char> sFiltered(src._width - 2 * size, src._height - 2 * size);
	CImg<float>         angles(src._width - 2 * size, src._height - 2 * size);

	for (auto i = size; i < src._height - size; ++i)
	{
		for (auto j = size; j < src._width - size; ++j)
		{
			auto sumX = 0.0, sumY = 0.0;

			for (auto x = 0U; x < xFilter.size(); ++x)
				for (auto y = 0U; y < xFilter.size(); ++y)
				{
					sumX += xFilter[x][y] * src(j + y - size, i + x - size);
					sumY += yFilter[x][y] * src(j + y - size, i + x - size);
				}

			sFiltered(j - size, i - size) = round<unsigned char>(std::sqrt(sumX * sumX + sumY * sumY));
			angles(j - size, i - size)    = std::abs(sumX) <= std::numeric_limits<double>::epsilon() ? 90.0f : static_cast<float>(std::atan(sumY / sumX));
		}
	}

	return std::make_pair(std::move(sFiltered), std::move(angles));
}

CImg<unsigned char> threshold(const CImg<unsigned char>& src, int low, int high)
{
	if (low > 255)
		low = 255;
	if (high > 255)
		high = 255;

	CImg<unsigned char> result(src._width, src._height);

	for (auto i = 0; i < static_cast<int>(src._width); ++i)
	{
		for (auto j = 0; j < static_cast<int>(src._height); ++j)
		{
			result(i, j) = src(i, j);
			if (result(i, j) > high)
				result(i, j) = 255;
			else if (result(i, j) < low)
				result(i, j) = 0;
			else
			{
				bool anyHigh    = false;
				bool anyBetween = false;
				for (auto x = i - 1; x < i + 2; ++x)
				{
					for (auto y = j - 1; y < j + 2; ++y)
					{
						//Wang Note: a missing "x" in Hasan's code.
						if (x < 0 || y < 0 || x >= static_cast<int>(result._width) || y >= static_cast<int>(result._height)) //Out of bounds
							continue;

						if (result(x, y) > high)
						{
							result(i, j) = 255;
							anyHigh      = true;
							break;
						}

						if (result(x, y) <= high && result(x, y) >= low)
							anyBetween = true;
					}
					if (anyHigh)
						break;
				}
				if (!anyHigh && anyBetween)
					for (auto x = i - 2; x < i + 3; ++x)
					{
						for (auto y = j - 1; y < j + 3; ++y)
						{
							if (x < 0 || y < 0 || x >= static_cast<int>(result._width) || y >= static_cast<int>(result._height)) //Out of bounds
								continue;

							if (result(x, y) > high)
							{
								result(i, j) = 255;
								anyHigh      = true;
								break;
							}
						}
						if (anyHigh)
							break;
					}
				if (!anyHigh)
					result(i, j) = 0;
			}
		}
	}

	return result;
}

CImg<unsigned char> nonMaxSupp(const CImg<unsigned char>& sFiltered, const CImg<float>& angles)
{
	auto result = CImg<unsigned char>(sFiltered._width - 2, sFiltered._height - 2);
	for (auto i = 1U; i < sFiltered._width - 1; ++i)
	{
		for (auto j = 1U; j < sFiltered._height - 1; ++j)
		{
			float Tangent = angles(i, j) * 57.296f;
			// cout << Tangent << ' ';
			result(i - 1, j - 1) = sFiltered(i, j);
			//Horizontal Edge
			if (((-22.5 < Tangent) && (Tangent <= 22.5)) || ((157.5 < Tangent) && (Tangent <= -157.5)))
			{
				if ((sFiltered(i, j) < sFiltered(i + 1, j)) || (sFiltered(i, j) < sFiltered(i - 1, j)))
					result(i - 1, j - 1) = 0;
			}
			//Vertical Edge
			if (((-112.5 < Tangent) && (Tangent <= -67.5)) || ((67.5 < Tangent) && (Tangent <= 112.5)))
			{
				if ((sFiltered(i, j) < sFiltered(i, j + 1)) || (sFiltered(i, j) < sFiltered(i, j - 1)))
					result(i - 1, j - 1) = 0;
			}

			//-45 Degree Edge
			if (((-67.5 < Tangent) && (Tangent <= -22.5)) || ((112.5 < Tangent) && (Tangent <= 157.5)))
			{
				if ((sFiltered(i, j) < sFiltered(i + 1, j + 1)) || (sFiltered(i, j) < sFiltered(i - 1, j - 1)))
					result(i - 1, j - 1) = 0;
			}

			//45 Degree Edge
			if (((-157.5 < Tangent) && (Tangent <= -112.5)) || ((22.5 < Tangent) && (Tangent <= 67.5)))
			{
				if ((sFiltered(i, j) < sFiltered(i - 1, j + 1)) || (sFiltered(i, j) < sFiltered(i + 1, j - 1)))
					result(i - 1, j - 1) = 0;
			}
		}
		// cout << '\n';
	}

	return result;
}

} // namespace

Canny::Canny(const int gaussianFilterSize, const double gaussianSigma, const int thresholdLow, const int thresholdHigh)
	: m_thresholdLow { thresholdLow }
	, m_thresholdHigh { thresholdHigh }
	, m_gaussianFilter { createGaussianFilter(gaussianFilterSize, gaussianFilterSize, gaussianSigma) }
{
}

Canny::Rect Canny::Process(const CImg<unsigned char>& img) const
{
	if (std::max(img.width(), img.height()) < 50)
		return Rect {};

	const auto gFiltered = useFilter(img, m_gaussianFilter);
	const auto [sFiltered, angles] = Sobel(gFiltered);
	const auto nonMaxSupped = nonMaxSupp(sFiltered, angles);
	const auto thresholded = threshold(nonMaxSupped, m_thresholdLow, m_thresholdHigh);
	Rect rect { .top = 0, .left = 0, .bottom = static_cast<int>(thresholded._height), .right = static_cast<int>(thresholded._width )};
	for (const auto* data = thresholded.data(); rect.top < rect.bottom; ++rect.top, data += thresholded._width)
		if (memchr(data, 255, thresholded._width))
			break;

	for (const auto* data = thresholded.data() + static_cast<size_t>(thresholded._width) * (thresholded._height - 1); rect.top < rect.bottom; --rect.bottom, data -= thresholded._width)
		if (memchr(data, 255, thresholded._width))
			break;

	for (; rect.left < rect.right; ++rect.left)
		for (auto i = 0U; i < thresholded._height; ++i)
			if (thresholded(rect.left, i) == 255)
				goto leftFound;
leftFound:

	for (; rect.left < rect.right; --rect.right)
		for (auto i = 0U; i < thresholded._height; ++i)
			if (thresholded(rect.right - 1, i) == 255)
				goto rightFound;
rightFound:

	return rect;
}
