#include "util.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include "fnd/IsOneOf.h"

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

	PLOGV << "select books";
	size_t n = 0;
	dump.CreateInpData([&](const DB::IQuery& query) {
		QString libId = query.Get<const char*>(7);

		QString fileName = QDir::fromNativeSeparators(query.Get<const char*>(5));
		auto    type     = query.Get<QString>(9).toLower();

		if (fileName.isEmpty())
		{
			fileName = libId;
			if (type != "fb2" && IsOneOf(type, "fd2", "fb", "???", "fb 2"))
				type = "fb2";
		}
		else
		{
			const QFileInfo fileInfo(fileName);
			type = fileInfo.suffix().toLower();
			if (const auto dir = fileInfo.dir(); dir.dirName() == '.')
				fileName = fileInfo.completeBaseName();
			else
				fileName = dir.filePath(fileInfo.completeBaseName());
		}

		auto index = fileName + "." + type;

		auto it = inpData.find(index);
		if (it == inpData.end())
		{
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
							 .sourceLib = dump.GetName(),
							 .hash      = query.Get<const char*>(16),
						 })
					 )
			         .first;
			it->second->title.replace(QChar { 0x2028 }, ' ');
		}

		it->second->series.emplace_back(query.Get<const char*>(3), Util::Fb2InpxParser::GetSeqNumber(query.Get<const char*>(4)), query.Get<int>(17), query.Get<double>(18));

		++n;
		PLOGV_IF(n % 50000 == 0) << n << " records selected";
	});
	PLOGV << n << " total records selected";

	PLOGV << "select books annotation";
	std::unordered_map<long long, QString> annotations;
	n = 0;
	dump.CreateAdditional({}, {}, IDump::AdditionalType::Annotation, [&](const DB::IQuery& query) {
		annotations.try_emplace(query.Get<long long>(0), query.Get<const char*>(1));
		++n;
		PLOGV_IF(n % 50000 == 0) << n << " records selected";
	});
	PLOGV << n << " total records selected";

	PLOGV << "update books data";
	n = 0;
	for (auto& [_, book] : inpData)
	{
		std::ranges::sort(book->series, {}, [](const Series& item) {
			return std::tuple(item.type, -item.level);
		});
		bool ok = false;
		if (const auto bookId = book->libId.toLongLong(&ok); ok)
		{
			if (const auto it = annotations.find(bookId); it != annotations.end())
			{
				if (!book->annotation.isEmpty())
					book->annotation.append('\n');
				book->annotation.append(it->second);
			}
		}
		++n;
		PLOGV_IF(n % 50000 == 0) << n << " records updated";
	}
	PLOGV << n << " total records updated";

	return inpData;
}

void SerializeHashSections(const QStringList& sections, Util::XmlWriter& writer)
{
	qsizetype depth = -1;
	for (const auto& str : sections)
	{
		const auto split = str.split('\t');
		auto       it    = split.begin();
		assert(it != split.end());

		bool ok       = false;
		auto newDepth = (it++)->toInt(&ok);
		assert(ok);

		const auto write = [&] {
			writer.WriteStartElement(u"section");
			if (it != split.end())
			{
				writer.WriteAttribute(u"id", *it++);
				if (it != split.end())
				{
					writer.WriteAttribute(u"count", *it++);
					if (it != split.end())
					{
						writer.WriteAttribute(u"size", *it++);
						if (it != split.end())
							writer.WriteAttribute(u"simHash", *it++);
					}
				}
			}
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
