#include "book.h"

#include <ranges>

#include <QStringList>

#include "fnd/IsOneOf.h"

#include "util/language.h"

using namespace HomeCompa;

Book Book::FromString(const QString& str)
{
	if (str.isEmpty())
		return {};

	//"AUTHOR;GENRE;TITLE;SERIES;SERNO;FILE;SIZE;LIBID;DEL;EXT;DATE;LANG;LIBRATE;KEYWORDS;YEAR;"
	auto l = str.split('\04');
	assert(l.size() >= 15);
	return Book {
		.author    = std::move(l[0]),
		.genre     = std::move(l[1]),
		.title     = std::move(l[2]),
		.series    = { { std::move(l[3]), std::move(l[4]) } },
		.file      = std::move(l[5]),
		.size      = std::move(l[6]),
		.libId     = std::move(l[7]),
		.deleted   = l[8] == "1",
		.ext       = std::move(l[9]),
		.date      = std::move(l[10]),
		.lang      = QString::fromStdWString(GetLanguage(l[11].toLower().toStdWString())),
		.rate      = l[12].toDouble(),
		.rateCount = 1,
		.keywords  = std::move(l[13]),
		.year      = std::move(l[14]),
	};
}

QString Book::GetFileName() const
{
	return QString("%1.%2").arg(file, ext);
}

QString Book::GetUid() const
{
	return QString("%1#%2").arg(folder, GetFileName());
}

namespace HomeCompa
{

QByteArray& operator<<(QByteArray& bytes, const Book& book)
{
	const auto rate      = std::llround(book.rate / book.rateCount);
	const auto rateBytes = rate > 0 && rate <= 5 ? QString::number(rate).toUtf8() : QByteArray {};

	for (const auto& [seriesTitle, serNo, type, level] : book.series)
	{
		QByteArray data;
		data.append(book.author.toUtf8())
			.append('\04')
			.append(book.genre.toUtf8())
			.append('\04')
			.append(book.title.toUtf8())
			.append('\04')
			.append(seriesTitle.toUtf8())
			.append('\04')
			.append(serNo.toUtf8())
			.append('\04')
			.append(book.file.toUtf8())
			.append('\04')
			.append(book.size.toUtf8())
			.append('\04')
			.append(book.libId.toUtf8())
			.append('\04')
			.append(book.deleted ? "1" : "0")
			.append('\04')
			.append(book.ext.toUtf8())
			.append('\04')
			.append(book.date.toUtf8())
			.append('\04')
			.append(book.lang.toUtf8())
			.append('\04')
			.append(rateBytes)
			.append('\04')
			.append(book.keywords.toUtf8())
			.append('\04')
			.append(book.year.toUtf8())
			.append('\04')
			.append(book.sourceLib.toUtf8())
			.append('\04');
		data.replace('\n', ' ');
		data.replace('\r', "");
		data.append("\x0d\x0a");

		bytes.append(data);
	}
	return bytes;
}

QString& SimplifyTitle(QString& value)
{
	value.removeIf([](const QChar ch) {
		return ch != ' ' && !IsOneOf(ch.category(), QChar::Number_DecimalDigit, QChar::Letter_Lowercase);
	});

	QStringList digits;
	auto        split = value.split(' ');
	for (auto& word : split)
	{
		QString digitsWord;
		word.removeIf([&](const QChar ch) {
			const auto c = ch.category();
			if (c == QChar::Number_DecimalDigit)
			{
				digitsWord.append(ch);
				return true;
			}

			return c != QChar::Letter_Lowercase;
		});
		if (!digitsWord.isEmpty())
			digits << std::move(digitsWord);
	}
	std::ranges::move(std::move(digits), std::back_inserter(split));

	return value;
}

QString& PrepareTitle(QString& value)
{
	value = value.toLower();
	value.replace(QChar { 0x0451 }, QChar { 0x0435 });
	value.replace(QChar { 0x0439 }, QChar { 0x0438 });
	value.replace(QChar { 0x044A }, QChar { 0x044C });
	return value;
}

} // namespace HomeCompa::FliParser
