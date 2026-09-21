#include "util.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include "fnd/IsOneOf.h"
#include "fnd/ScopedCall.h"
#include "fnd/try.h"

#include "database/interface/ICommand.h"
#include "database/interface/IQuery.h"
#include "database/interface/ITransaction.h"

#include "dump/IDump.h"
#include "util/EpubParser.h"
#include "util/Fb2InpxParser.h"
#include "util/bookhash/hashbook.h"
#include "util/language.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "UniqueFile.h"
#include "book.h"
#include "log.h"
#include "zip.h"

namespace HomeCompa::FliLib {

namespace {

void SetOriginalNames(Book& book, const QString& originBaseName, const QString& originSuffix)
{
	if (!originBaseName.isEmpty())
		book.libId = book.file = originBaseName;
	if (!originSuffix.isEmpty())
		book.ext = originSuffix;
}

std::optional<Book> ParseStub(
	QString& /*parserName*/,
	const QString& /*folder*/,
	const Zip& /*zip*/,
	const QString& /*fileName*/,
	const QDateTime& /*zipDateTime*/,
	const bool /*isDeleted*/,
	const QString& /*originBaseName*/,
	const QString& /*originSuffix*/
)
{
	return std::nullopt;
}

std::optional<Book> ParseFb2(
	QString&         parserName,
	const QString&   folder,
	const Zip&       zip,
	const QString&   fileName,
	const QDateTime& zipDateTime,
	const bool       isDeleted,
	const QString&   originBaseName = {},
	const QString&   originSuffix   = {}
)
{
	parserName            = "fb2";
	auto parseResult      = Util::Fb2InpxParser::Parse(folder, zip, fileName, zipDateTime, isDeleted);
	auto parsedBook       = Book::FromString(parseResult.line);
	parsedBook.annotation = std::move(parseResult.annotation);
	SetOriginalNames(parsedBook, originBaseName, originSuffix);
	return parsedBook;
}

std::optional<Book> ParseEpub(
	QString&         parserName,
	const QString&   folder,
	const Zip&       zip,
	const QString&   fileName,
	const QDateTime& zipDateTime,
	const bool       isDeleted,
	const QString&   originBaseName = {},
	const QString&   originSuffix   = {}
)
{
	parserName                 = "epub";
	const auto authorsToString = [](std::vector<QStringList> authors) {
		QStringList values;
		values.reserve(static_cast<int>(authors.size()));
		std::ranges::transform(authors, std::back_inserter(values), [](const auto& author) {
			return author.join(Util::Fb2InpxParser::NAMES_SEPARATOR);
		});
		return values.join(Inpx::LIST_SEPARATOR) + Inpx::LIST_SEPARATOR;
	};

	const auto genresToString = [](const QStringList& genres) {
		return genres.empty() ? QString {} : genres.join(Inpx::LIST_SEPARATOR) + Inpx::LIST_SEPARATOR;
	};

	const QFileInfo fileInfo(fileName);
	try
	{
		static constexpr const char* textExt[] { ".htm", ".html", ".xhtml", ".xml" };
		QCryptographicHash           md5 { QCryptographicHash::Md5 };
		auto                         parseResult = Util::EpubParser::Parse(zip, fileName, Util::CommonParser::Mode::Images | Util::CommonParser::Mode::Texts);
		size_t                       size        = 0;
		for (auto&& [id, body] : parseResult.texts | std::views::filter([](const auto& item) {
									 return std::ranges::any_of(textExt, [&](const char* ext) {
										 return item.id.endsWith(ext, Qt::CaseInsensitive);
									 });
								 }))
		{
			auto hist  = Util::CollectHistogram(std::move(body), md5);
			size      += Util::CalculateHash(hist).size;
		}
		Book book {
			.author  = authorsToString(std::move(parseResult.authors)),
			.genre   = genresToString(parseResult.genres),
			.title   = std::move(parseResult.title),
			.series  = { {} },
			.file    = fileInfo.completeBaseName(),
			.size    = QString::number(size),
			.libId   = fileInfo.completeBaseName(),
			.deleted = isDeleted,
			.ext     = fileInfo.suffix(),
			.date    = zipDateTime.toString("yyyy-MM-dd"),
			.lang    = GetLanguage(parseResult.language).toString(),
		};
		SetOriginalNames(book, originBaseName, originSuffix);
		return book;
	}
	catch (const std::exception& ex)
	{
		PLOGE << QString("%1/%2: %3").arg(folder, fileName, ex.what());
	}
	catch (...)
	{
		PLOGE << QString("%1/%2: unknown error").arg(folder, fileName);
	}
	return std::nullopt;
}

std::optional<Book> ParseFbd(
	QString&         parserName,
	const QString&   folder,
	const Zip&       zip,
	const QString&   fileName,
	const QDateTime& zipDateTime,
	const bool       isDeleted,
	const QString& /*originBaseName*/ = {},
	const QString& /*originSuffix*/   = {}
)
{
	const ScopedCall parserNameGuard([&] {
		parserName = "fbd";
	});
	const QFileInfo  fileInfo(fileName);
	if (const auto fbdFileName = fileName + ".fbd"; zip.GetFileIndex(fbdFileName) != Zip::INVALID_INDEX)
		return ParseFb2(parserName, folder, zip, fbdFileName, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix());
	if (const auto fbdFileName = fileInfo.completeBaseName() + ".fbd"; zip.GetFileIndex(fbdFileName) != Zip::INVALID_INDEX)
		return ParseFb2(parserName, folder, zip, fbdFileName, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix());
	return std::nullopt;
}

std::optional<Book> ParseZip(
	QString&         parserName,
	const QString&   folder,
	const Zip&       zip,
	const QString&   fileName,
	const QDateTime& zipDateTime,
	bool             isDeleted,
	const QString& /*originBaseName*/ = {},
	const QString& /*originSuffix*/   = {}
);

using FileParser = std::optional<Book> (*)(
	QString& /*parserName*/,
	const QString& /*folder*/,
	const Zip&,
	const QString& /*fileName*/,
	const QDateTime& /*zipDateTime*/,
	bool /*isDeleted*/,
	const QString& /*originBaseName*/,
	const QString& /*originSuffix*/
);
constexpr std::pair<const char*, std::pair<FileParser, bool /*parser exists*/>> FILE_PARSERS[] {
	{  ".fb2",   { &ParseFb2, true } },
    { ".epub",  { &ParseEpub, true } },
    {  ".fbd", { &ParseStub, false } },
    {  ".zip",  { &ParseZip, false } },
    {   ".7z",  { &ParseZip, false } },
    {  ".rar",  { &ParseZip, false } },
};

std::optional<Book> ParseZip(
	QString&         parserName,
	const QString&   folder,
	const Zip&       zip,
	const QString&   fileName,
	const QDateTime& zipDateTime,
	const bool       isDeleted,
	const QString& /*originBaseName*/,
	const QString& /*originSuffix*/
)
{
	const QFileInfo fileInfo(fileName);
	const auto      stream = zip.Read(fileName);
	const auto      subZip = TRY(QString("open %1").arg(fileName), [&] {
		return std::make_unique<Zip>(stream->GetStream());
	});
	if (!subZip)
		return {};
	const auto subZipFiles = subZip->GetFileNameList();
	for (const auto& [ext, parserPair] : FILE_PARSERS | std::views::filter([](const auto& item) {
											 return item.second.second;
										 }))
	{
		for (const auto& subZipFile : subZipFiles | std::views::filter([](const auto& item) {
										  return !item.startsWith("__MACOSX");
									  }))
		{
			if (subZipFile.endsWith(ext, Qt::CaseInsensitive))
			{
				if (auto book = parserPair.first(parserName, folder, *subZip, subZipFile, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix()))
					return book;
			}
		}
	}

	const auto it = std::ranges::find_if(subZipFiles, [](const QString& item) {
		return item.endsWith(".fbd", Qt::CaseInsensitive);
	});
	if (it == subZipFiles.end())
		return std::nullopt;

	const ScopedCall parserNameGuard([&] {
		parserName = "fbd";
	});
	return ParseFb2(parserName, folder, *subZip, *it, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix());
}

} // namespace

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

InpData CreateInpData(const IDump& dump, std::unordered_map<QString, QString>& series)
{
	InpData inpData;

	PLOGV << "select books";
	size_t n = 0;
	dump.CreateInpData([&](const DB::IQuery& query) {
		QString libId = query.Get<const char*>(7);

		QString fileName = QDir::fromNativeSeparators(query.Get<const char*>(5));
		auto    type     = query.Get<QString>(9).toLower();

		const QFileInfo fileInfo(fileName);

		if (fileName.isEmpty())
		{
			fileName = libId;
			if (type != "fb2" && IsOneOf(type, "fd2", "fb", "???", "fb 2"))
				type = "fb2";
		}
		else
		{
			type = fileInfo.suffix().toLower();
			if (const auto dir = fileInfo.dir(); dir.dirName() == '.')
				fileName = fileInfo.completeBaseName();
			else
				fileName = dir.filePath(fileInfo.completeBaseName());
		}

		auto it = inpData.find(libId);
		if (it == inpData.end())
		{
			const auto* deleted = query.Get<const char*>(8);
			auto        book    = std::make_shared<Book>(Book {
				.author    = query.Get<const char*>(0),
				.genre     = query.Get<const char*>(1),
				.title     = query.Get<const char*>(2),
				.file      = fileName,
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
			});
			book->title.replace(QChar { 0x2028 }, ' ');
			it = inpData.emplace(book->libId, book);

			while (true)
			{
				auto index = fileName.toLower().toLower().normalized(QString::NormalizationForm_D);
				index.removeIf([](const QChar ch) {
					return !IsOneOf(ch.category(), QChar::Category::Letter_Lowercase, QChar::Category::Number_DecimalDigit);
				});

				inpData.emplace(std::move(index), book);
				const auto dotIndex = fileName.lastIndexOf('.');
				if (dotIndex < 0)
					break;

				fileName.resize(dotIndex);
			}
		}

		QString seriesTitleSrc = query.Get<const char*>(3);
		auto    seriesTitleKey = seriesTitleSrc;
		std::ranges::transform(seriesTitleKey, seriesTitleKey.begin(), [](const QChar& ch) {
			return IsOneOf(
					   ch.category(),
					   QChar::Category::Letter_Lowercase,
					   QChar::Category::Letter_Uppercase,
					   QChar::Category::Letter_Titlecase,
					   QChar::Category::Number_DecimalDigit,
					   QChar::Category::Number_Letter
				   )
			         ? ch.toLower()
			         : QChar { ' ' };
		});
		seriesTitleKey   = seriesTitleKey.split(' ', Qt::SkipEmptyParts).join(' ');
		auto seriesTitle = series.try_emplace(std::move(seriesTitleKey), std::move(seriesTitleSrc)).first->second;

		it->second->series.emplace_back(std::move(seriesTitle), Util::Fb2InpxParser::GetSeqNumber(query.Get<const char*>(4)), query.Get<int>(17), query.Get<double>(18));

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

Book* ParseBook(const QString& fileName, InpDataProvider& inpDataProvider, const QString& folder, const Zip& zip, const QDateTime& zipDateTime, const bool isDeleted)
{
	PLOGI << "parsing " << folder + "/" + fileName;
	const auto parser = [&] {
		const auto it = std::ranges::find_if(FILE_PARSERS, [&](const auto& item) {
			return fileName.endsWith(item.first, Qt::CaseInsensitive);
		});
		return it != std::end(FILE_PARSERS) ? it->second.first : &ParseFbd;
	}();

	QString parserName;
	if (auto parsedBook = parser(parserName, folder, zip, fileName, zipDateTime, isDeleted, {}, {}))
	{
		PLOGI << parserName << " parser finished";
		parsedBook->folder = folder;
		return inpDataProvider.AddBook(std::make_unique<Book>(std::move(*parsedBook)));
	}

	PLOGW << "unknown book";
	auto book    = std::make_unique<Book>(Book::CreateUnknown(fileName, zip.GetFileSize(fileName), zip.GetFileTime(fileName).date()));
	book->folder = folder;
	return inpDataProvider.AddBook(std::move(book));
}

void WriteParsedBookToDatabase(DB::ITransaction& tr, const Book& book)
{
	const auto command = tr.CreateCommand(R"(insert or replace into FileCustom (FileId, Author, Title, Genre, Updated, Lang, Series, Keywords, PublishYear)
select f.FileId, ?, ?, ?, ?, ?, ?, ?, ?
from File f
join Folder d on d.FolderId = f.FolderId and d.Name = ?
where f.Name = ?
)");
	const auto bind    = [&](const size_t index, const QString& value) {
		value.isEmpty() ? command->Bind(index) : command->Bind(index, value);
	};
	const auto series = (book.series | std::views::filter([](const Series& item) {
							 return !item.title.isEmpty();
						 })
	                     | std::views::transform([](const Series& item) {
							   return QString("%1#%2").arg(item.title, item.serNo);
						   })
	                     | std::ranges::to<QStringList>())
	                        .join('|');

	bind(0, book.author);
	bind(1, book.title);
	bind(2, book.genre);
	bind(3, book.date);
	bind(4, book.lang);
	bind(5, series);
	bind(6, book.keywords);
	bind(7, book.year);
	command->Bind(8, book.folder);
	command->Bind(9, book.GetFileName());
	command->Execute();
}

} // namespace HomeCompa::FliLib
