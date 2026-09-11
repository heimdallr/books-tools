#pragma once

#include <vector>

#include <QString>

#include "export/lib.h"

namespace HomeCompa::FliLib
{

struct Archive
{
	QString filePath;
	QString sourceLib;
};

using Archives = std::vector<Archive>;

LIB_EXPORT Archives GetArchives(const QStringList& wildCards);
LIB_EXPORT size_t   Total(const Archives& archives);

}
