#include "util.h"

#include <QFileInfo>
#include <QRegularExpression>

#include "database/interface/IQuery.h"

#include "dump/IDump.h"
#include "util/Fb2InpxParser.h"
#include "util/language.h"
#include "util/xml/XmlWriter.h"

#include "book.h"
#include "log.h"

namespace HomeCompa::FliLib
{

void Write(const QString& fileName, const QByteArray& data)
{
	QFile output(fileName);
	if (!output.open(QIODevice::WriteOnly))
	{
		PLOGE << "Cannot write to " << fileName;
		return;
	}

	const auto written = output.write(data);
	if (written == data.size())
		PLOGV << QFileInfo(fileName).fileName() << ": " << written << " bytes written";
	else
		PLOGE << QFileInfo(fileName).fileName() << ": " << written << " bytes written of a " << data.size();
}

QString& ReplaceTags(QString& str)
{
	static constexpr std::pair<const char*, const char*> tags[] {
		{    "br",    "br" },
        {    "hr",    "hr" },
        { "quote",     "q" },
        { "table", "table" },
        {    "tr",    "tr" },
        {    "th",    "th" },
        {    "td",    "td" },
	};

	str.replace("<p>&nbsp;</p>", "");

	auto strings = str.split('\n', Qt::SkipEmptyParts);
	erase_if(strings, [](const QString& item) {
		return item.simplified().isEmpty();
	});
	str = strings.join("<br/>");

	str.replace(QRegularExpression(R"(\[(\w)\])"), R"(<\1>)").replace(QRegularExpression(R"(\[(/\w)\])"), R"(<\1>)");
	for (const auto& [from, to] : tags)
		str.replace(QString("[%1]").arg(from), QString("<%1>").arg(to), Qt::CaseInsensitive).replace(QString("[/%1]").arg(from), QString("</%1>").arg(to), Qt::CaseInsensitive);

	str.replace(QRegularExpression(R"(\[img\](.*?)\[/img\])"), R"(<img src="\1"/>)");
	str.replace(QRegularExpression(R"(\[(URL|url)=(.*?)\](.*?)\[/(URL|url)\])"), R"(<a href="\2"/>\3</a>)");
	str.replace(QRegularExpression(R"(\[color=(.*?)\])"), R"(<font color="\1">)").replace("[/color]", "</font>");

	str.replace(QRegularExpression(R"(([^"])(https{0,1}:\/\/\S+?)([\s<]))"), R"(\1<a href="\2">\2</a>\3)");

	str.replace(QRegularExpression(R"(\[collapse collapsed title=(.*?)\])"), R"(<details><summary>\1</summary>)");
	str.replace(QRegularExpression(R"(\[/collapse])"), R"(</details>)");

	return str;
}

InpData CreateInpData(const IDump& dump)
{
	InpData inpData;

	size_t n = 0;
	dump.CreateInpData([&](const DB::IQuery& query) {
		QString libId = query.Get<const char*>(7);

		QString type = query.Get<const char*>(9);
		if (type != "fb2")
			for (const auto* typoType : { "fd2", "fb", "???", "fb 2", "fbd" })
				if (type == typoType)
				{
					type = "fb2";
					break;
				}

		QString fileName = query.Get<const char*>(5);

		auto index = fileName.isEmpty() ? libId + "." + type : fileName;

		auto it = inpData.find(index);
		if (it == inpData.end())
		{
			if (fileName.isEmpty())
			{
				fileName = libId;
			}
			else
			{
				QFileInfo fileInfo(fileName);
				fileName = fileInfo.completeBaseName();
				if (const auto ext = fileInfo.suffix().toLower(); ext == "fb2")
					type = "fb2";
			}

			const auto* deleted = query.Get<const char*>(8);

			it = inpData
			         .try_emplace(
						 std::move(index),
						 std::make_unique<Book>(Book {
							 .author    = query.Get<const char*>(0),
							 .genre     = query.Get<const char*>(1),
							 .title     = query.Get<const char*>(2),
							 .file      = std::move(fileName),
							 .size      = query.Get<const char*>(6),
							 .libId     = std::move(libId),
							 .deleted   = deleted && *deleted != '0',
							 .ext       = std::move(type),
							 .date      = QString::fromUtf8(query.Get<const char*>(10), 10),
							 .lang      = GetLanguage(QString(query.Get<QString>(11)).toLower()).toString(),
							 .rate      = query.Get<double>(12),
							 .rateCount = query.Get<int>(13),
							 .keywords  = query.Get<const char*>(14),
							 .year      = query.Get<const char*>(15),
						 })
					 )
			         .first;
		}

		it->second->series.emplace_back(query.Get<const char*>(3), Util::Fb2InpxParser::GetSeqNumber(query.Get<const char*>(4)), query.Get<int>(16), query.Get<double>(17));

		++n;
		PLOGV_IF(n % 50000 == 0) << n << " records selected";
	});

	PLOGV << n << " total records selected";

	for (auto& [_, book] : inpData)
		std::ranges::sort(book->series, {}, [](const Series& item) {
			return std::tuple(item.type, -item.level);
		});

	return inpData;
}

void SerializeHashSections(const QStringList& sections, Util::XmlWriter& writer)
{
	qsizetype depth = -1;
	for (const auto& str : sections)
	{
		const auto split    = str.split('\t');
		auto       newDepth = split.size() - 2;
		const auto last     = split.rbegin();

		const auto write = [&] {
			writer.WriteStartElement("section").WriteAttribute("id", *std::next(last)).WriteAttribute("count", *last);
		};

		if (depth == newDepth)
		{
			writer.WriteEndElement();
			write();
			continue;
		}

		if (depth < newDepth)
		{
			write();
			depth = newDepth;
			continue;
		}

		writer.WriteEndElement();
		for (; newDepth < depth; --depth)
			writer.WriteEndElement();

		write();
	}

	for (; depth >= 0; --depth)
		writer.WriteEndElement();
}

} // namespace HomeCompa::FliLib
