#pragma once

#include <QHash>

#include <memory>
#include <unordered_map>
#include <vector>

#include <QString>

namespace HomeCompa::FliParser
{

struct Series
{
	QString title;
	QString serNo;
	int     type { 0 };
	double  level { 0 };
};

struct Section
{
	Section* parent { nullptr };
	size_t   count { 0 };
	using Ptr = std::unique_ptr<Section>;
	std::unordered_map<QString, Ptr> children;
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
	bool                deleted;
	QString             ext;
	QString             date;
	QString             lang;
	double              rate;
	int                 rateCount;
	QString             keywords;
	QString             year;

	QString      id;
	Section::Ptr section;

	static Book fromString(const QString& str);
};

QByteArray& operator<<(QByteArray& bytes, const Book& book);

} // namespace HomeCompa::FliParser
