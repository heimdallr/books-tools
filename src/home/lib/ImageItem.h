#pragma once

#include <QDateTime>

#include "fnd/algorithm.h"

#include "export/lib.h"

namespace HomeCompa::FliLib
{

struct LIB_EXPORT ImageItem
{
	QString    fileName;
	QByteArray body;
	QDateTime  dateTime;
	QString    hash;

	bool operator<(const ImageItem& rhs) const;
};

using ImageItems = std::vector<ImageItem>;

} // namespace HomeCompa::FliLib
