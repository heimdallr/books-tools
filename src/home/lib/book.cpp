#include "book.h"

#include <ranges>

#include <QStringList>

#include "fnd/IsOneOf.h"

#include "util/language.h"

using namespace HomeCompa::FliLib;

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

namespace HomeCompa::FliLib
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
	static constexpr std::pair<char16_t, char16_t> replacementChar[] {
		{ 0x0451, 0x0435 },
		{ 0x0439, 0x0438 },
		{ 0x044A, 0x044C },
	};
	const std::pair<QString, QString> replacementString[] {
		{ QString("%1%2").arg(QChar { 0x044B }, QChar { 0x043E }), QString("%1%2").arg(QChar { 0x044C }, QChar { 0x044E }) },
	};

	value = value.toLower();

	std::ranges::transform(value, std::begin(value), [&](const QChar& ch) {
		const auto it = std::ranges::find(replacementChar, ch.unicode(), &std::pair<char16_t, char16_t>::first);
		if (it != std::cend(replacementChar))
			return QChar { it->second };

		const auto category = ch.category();
		if (IsOneOf(category, QChar::Separator_Space, QChar::Separator_Line, QChar::Separator_Paragraph, QChar::Other_Control)
		    || (category >= QChar::Punctuation_Connector && category <= QChar::Punctuation_Other))
			return QChar { 0x20 };

		return ch;
	});

	for (const auto& [from, to] : replacementString)
		value.replace(from, to);

	return value;
}

} // namespace HomeCompa::FliLib
