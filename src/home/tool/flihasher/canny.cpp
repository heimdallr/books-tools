//
//  canny.cpp
//  Canny Edge Detector
//
//  Created by Hasan Akgün on 21/03/14.
//  Modifed by Michael Wang on 10/03/17.
//  Software is released under GNU-GPL 2.0
//

#include "canny.h"

#include <cmath>

#include <iostream>
#include <vector>

using namespace cimg_library;
using namespace std;

namespace
{

template <std::integral U, std::floating_point V>
U round(const V v)
{
	return static_cast<U>(std::min(std::llround(v), static_cast<long long>(std::numeric_limits<U>::max())));
}

vector<vector<double>> createFilter(const int row, const int column, const double sigmaIn)
{
	vector<vector<double>> filter;

	for (int i = 0; i < row; i++)
	{
		vector<double> col;
		for (int j = 0; j < column; j++)
		{
			col.push_back(-1);
		}
		filter.push_back(col);
	}

	const auto constant = 2.0 * sigmaIn * sigmaIn;

	// Sum is for normalization
	auto sum = 0.0;

	for (int x = -row / 2; x <= row / 2; x++)
	{
		for (int y = -column / 2; y <= column / 2; y++)
		{
			const auto coordSum                  = (x * x + y * y);
			filter[x + row / 2][y + column / 2]  = (exp(-(coordSum) / constant)) / (M_PI * constant);
			sum                                 += filter[x + row / 2][y + column / 2];
		}
	}

	// Normalize the Filter
	for (int i = 0; i < row; i++)
		for (int j = 0; j < column; j++)
			filter[i][j] /= sum;

	return filter;
}

CImg<unsigned char> useFilter(CImg<unsigned char> img_in, const vector<vector<double>>& filterIn)
{
	const auto          size = static_cast<unsigned int>(filterIn.size()) / 2;
	CImg<unsigned char> gFiltered(img_in._width - 2 * size, img_in._height - 2 * size);
	for (auto i = size; i < img_in._width - size; ++i)
	{
		for (auto j = size; j < img_in._height - size; ++j)
		{
			double sum = 0;

			for (auto x = 0U, sz = static_cast<unsigned int>(filterIn.size()); x < sz; ++x)
				for (auto y = 0U; y < sz; ++y)
					sum += filterIn[x][y] * img_in(i + y - size, j + x - size);

			gFiltered(i - size, j - size) = round<unsigned char>(sum);
		}
	}

	return gFiltered;
}

CImg<unsigned char> threshold(const CImg<unsigned char>& src, int low, int high)
{
	if (low > 255)
		low = 255;
	if (high > 255)
		high = 255;

	CImg<unsigned char> result(src._width, src._height);

	for (auto i = 0U; i < src._width; ++i)
	{
		for (auto j = 0U; j < src._height; ++j)
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
						if (x <= 0 || y <= 0 || x > result._width || y > result._height) //Out of bounds
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
							if (x < 0 || y < 0 || x > result._width || y > result._height) //Out of bounds
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

Canny::Canny(CImg<unsigned char> img_)
	: img(std::move(img_))
{
}

CImg<unsigned char> Canny::process(const int gfs, const double g_sig, const int thres_lo, const int thres_hi)
{
	_gfs      = gfs;
	_g_sig    = g_sig;
	_thres_lo = thres_lo;
	_thres_hi = thres_hi;

	vector<vector<double>> filter = createFilter(gfs, gfs, g_sig);

	auto gFiltered = useFilter(img, filter); //Gaussian Filter
	gFiltered.save("t:/gaussian.ppm");

	const auto [sFiltered, angles] = sobel(gFiltered); //Sobel Filter
	sFiltered.save("t:/sobel.ppm");

	auto nonMaxSupped = nonMaxSupp(sFiltered, angles);
	nonMaxSupped.save("t:/nonMaxSupped.ppm");

	auto thres = threshold(nonMaxSupped, thres_lo, thres_hi);
	thres.save("t:/thres.ppm");

	return thres;
}

std::pair<CImg<unsigned char>, CImg<float>> Canny::sobel(const CImg<unsigned char>& gFiltered)
{
	//Sobel X Filter
	double x1[] = { -1.0, 0, 1.0 };
	double x2[] = { -2.0, 0, 2.0 };
	double x3[] = { -1.0, 0, 1.0 };

	vector<vector<double>> xFilter(3);
	xFilter[0].assign(x1, x1 + 3);
	xFilter[1].assign(x2, x2 + 3);
	xFilter[2].assign(x3, x3 + 3);

	//Sobel Y Filter
	double y1[] = { 1.0, 2.0, 1.0 };
	double y2[] = { 0, 0, 0 };
	double y3[] = { -1.0, -2.0, -1.0 };

	vector<vector<double>> yFilter(3);
	yFilter[0].assign(y1, y1 + 3);
	yFilter[1].assign(y2, y2 + 3);
	yFilter[2].assign(y3, y3 + 3);

	//Limit Size
	const auto size = static_cast<unsigned int>(xFilter.size()) / 2;

	CImg<unsigned char> sFiltered(gFiltered._width - 2 * size, gFiltered._height - 2 * size);

	CImg<float> angles = CImg<unsigned char>(gFiltered._width - 2 * size, gFiltered._height - 2 * size); //AngleMap

	for (auto i = size; i < gFiltered._height - size; i++)
	{
		for (auto j = size; j < gFiltered._width - size; j++)
		{
			auto sumx = 0.0, sumy = 0.0;

			for (int x = 0; x < xFilter.size(); x++)
				for (int y = 0; y < xFilter.size(); y++)
				{
					sumx += xFilter[x][y] * gFiltered(j + y - size, i + x - size); //Sobel_X Filter Value
					sumy += yFilter[x][y] * gFiltered(j + y - size, i + x - size); //Sobel_Y Filter Value
				}
			const auto sumxsq = sumx * sumx, sumysq = sumy * sumy;

			double sq2 = std::min(sqrt(sumxsq + sumysq), 255.0);

			sFiltered(j - size, i - size) = round<unsigned char>(sq2);

			if (std::abs(sumx) <= std::numeric_limits<double>::epsilon())
				angles(j - size, i - size) = 90;
			else
				angles(j - size, i - size) = round<unsigned char>(std::atan(sumy / sumx));
		}
	}

	return std::make_pair(std::move(sFiltered), std::move(angles));
}
