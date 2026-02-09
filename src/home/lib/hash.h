#pragma once

#include <vector>

#include <QStringList>

namespace HomeCompa::FliLib
{

struct HashParseResult
{
	QString     id;
	QString     title;
	QString     hashText;
	QStringList hashSections;

	std::vector<std::pair<size_t, QString>> hashValues;
};

struct ImageHashItem
{
	QString    file;
	QByteArray body;
	QString    hash;
	uint64_t   pHash { 0 };
};

struct BookHashItem
{
	QString                    folder;
	QString                    file;
	QByteArray                 body;
	ImageHashItem              cover;
	std::vector<ImageHashItem> images;
	HashParseResult            parseResult;
};

} // namespace HomeCompa::FliLib
