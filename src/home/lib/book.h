#pragma once

#include <QHash>

#include <memory>
#include <unordered_map>
#include <vector>

#include <QString>

#include "export/lib.h"

class QIODevice;

namespace HomeCompa::FliLib
{

struct Series
{
	QString title;
	QString serNo;
	int     type { 0 };
	double  level { 0 };
};

struct Book
{
	QString             author;
	QString             genre;
	QString             title;
	std::vector<Series> series;
	QString             file;
	QString             size;
	QString             libId;
	bool                deleted { false };
	QString             ext;
	QString             date;
	QString             lang;
	double              rate { 0.0 };
	int                 rateCount { 1 };
	QString             keywords;
	QString             year;
	QString             sourceLib;
	size_t              insNo { 0 };

	QString id;
	QString folder;

	LIB_EXPORT static Book FromString(const QString& str);
	LIB_EXPORT QString     GetFileName() const;
	LIB_EXPORT QString     GetUid() const;
};

LIB_EXPORT QByteArray& operator<<(QByteArray& bytes, const Book& book);

} // namespace HomeCompa::FliLib
