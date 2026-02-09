#pragma once

#include <vector>

#include <QStringList>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

#include "export/lib.h"

namespace HomeCompa::FliLib
{

using TextHistogram = std::vector<std::pair<size_t, QString>>;

struct HashParseResult
{
	QString       id;
	QString       title;
	QString       hashText;
	QStringList   hashSections;
	TextHistogram hashValues;
};

struct ImageHashItem
{
	QString    file;
	QByteArray body;
	QString    hash;
	uint64_t   pHash { 0 };
};

using ImageHashItems = std::vector<ImageHashItem>;

struct BookHashItem
{
	QString         folder;
	QString         file;
	QByteArray      body;
	ImageHashItem   cover;
	ImageHashItems  images;
	HashParseResult parseResult;
};

class LIB_EXPORT BookHashItemProvider
{
	NON_COPY_MOVABLE(BookHashItemProvider)

public:
	explicit BookHashItemProvider(const QString& path);
	~BookHashItemProvider();

public:
	[[nodiscard]] QStringList  GetFiles() const;
	[[nodiscard]] BookHashItem Get(const QString& file) const;

private:
	struct Impl;
	PropagateConstPtr<Impl> m_impl;
};

LIB_EXPORT BookHashItem GetHash(const QString& path, const QString& file);

} // namespace HomeCompa::FliLib
