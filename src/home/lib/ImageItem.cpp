#include "ImageItem.h"

using namespace HomeCompa::FliLib;

bool ImageItem::operator<(const ImageItem& rhs) const
{
	return hash < rhs.hash;
}
