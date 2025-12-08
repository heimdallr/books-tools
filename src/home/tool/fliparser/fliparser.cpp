#include <QCryptographicHash>

#include <filesystem>
#include <ranges>
#include <regex>
#include <set>

#include <QBuffer>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/ScopedCall.h"

#include "database/interface/IQuery.h"

#include "lib/UniqueFile.h"
#include "lib/archive.h"
#include "lib/book.h"
#include "lib/dump/Factory.h"
#include "lib/dump/IDump.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/Fb2InpxParser.h"
#include "util/LogConsoleFormatter.h"
#include "util/executor/ThreadPool.h"
#include "util/language.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"

#include "Constant.h"
#include "log.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";
constexpr auto COLLECTION_INFO_TEMPLATE     = "collection-info-template";
constexpr auto DUMP                         = "dump";
constexpr auto OUTPUT                       = "output";
constexpr auto FOLDER                       = "folder";
constexpr auto PATH                         = "path";

constexpr auto APP_ID = "fliparser";

using FileToFolder = std::unordered_map<QString, QStringList>;
using BookItem     = std::pair<QString, QString>;
using Replacement  = std::unordered_map<BookItem, BookItem, Util::PairHash<QString, QString>>;

struct Settings
{
	std::filesystem::path outputFolder;
	std::filesystem::path collectionInfoTemplateFile;

	std::unordered_map<QString, Book*>   hashToBook;
	std::unordered_map<QString, QString> fileToHash;
	std::unordered_map<QString, QString> libIdToHash;

	FileToFolder fileToFolder;

	[[nodiscard]] Book* FromFile(const QString& file) const
	{
		return From(fileToHash, file);
	}

	[[nodiscard]] Book* FromLibId(const QString& libId) const
	{
		return From(libIdToHash, libId);
	}

private:
	[[nodiscard]] Book* From(const std::unordered_map<QString, QString>& map, const QString& id) const
	{
		const auto hashIt = map.find(id);
		if (hashIt == map.end())
			return nullptr;

		const auto bookIt = hashToBook.find(hashIt->second);
		if (bookIt == hashToBook.end())
			return nullptr;

		return bookIt->second;
	}
};

struct BookStorage
{
	std::vector<Book*> books;

	Book* Add(Book* book)
	{
		return books.emplace_back(book);
	}

	Book* Add(std::unique_ptr<Book> book)
	{
		return books.emplace_back(m_storage.emplace_back(std::move(book)).get());
	}

private:
	std::vector<std::unique_ptr<Book>> m_storage;
};

struct FileInfo
{
	QByteArray hash;
	qsizetype  size;
};

FileInfo GetFileHash(const Zip& zip, const QString& fileName)
{
	const auto fileData = zip.Read(fileName)->GetStream().readAll();

	QCryptographicHash hash(QCryptographicHash::Algorithm::Md5);
	hash.addData(fileData);
	return { hash.result().toHex(), fileData.size() };
}

Book* GetBookCustom(const QString& fileName, BookStorage& storage, const Zip& zip, const QJsonObject& unIndexed)
{
	const auto [key, size] = GetFileHash(zip, fileName);

	const auto it = unIndexed.constFind(key);
	if (it == unIndexed.constEnd())
		return nullptr;

	QFileInfo fileInfo(fileName);

	const auto value = it.value().toObject();

	std::vector<Series> series;
	if (const auto seriesObj = value["series"]; seriesObj.isArray())
		std::ranges::transform(seriesObj.toArray(), std::back_inserter(series), [](const auto& item) {
			const auto obj = item.toObject();
			return Series { obj["title"].toString(), obj["number"].toString() };
		});
	if (series.empty())
		series.emplace_back();

	return storage.Add(std::make_unique<Book>(Book {
		.author   = value["author"].toString(),
		.genre    = value["genre"].toString(),
		.title    = value["title"].toString(),
		.series   = std::move(series),
		.file     = fileInfo.baseName(),
		.size     = QString::number(size),
		.libId    = fileInfo.baseName(),
		.deleted  = true,
		.ext      = fileInfo.suffix(),
		.date     = value["date"].toString(),
		.lang     = value["lang"].toString(),
		.keywords = value["keywords"].toString(),
		.year     = value["year"].toString(),
	}));
}

Book* ParseBook(const QString& fileName, BookStorage& storage, const QString& folder, const Zip& zip, const QDateTime& zipDateTime)
{
	if (!fileName.endsWith(".fb2", Qt::CaseInsensitive))
		return nullptr;

	const auto hash = GetFileHash(zip, fileName).hash;
	PLOGV << "parse " << fileName << ", hash: " << hash;

	auto parsedBook = Book::FromString(Util::Fb2InpxParser::Parse(folder, zip, fileName, zipDateTime, true));
	if (parsedBook.title.isEmpty())
		return nullptr;

	return storage.Add(std::make_unique<Book>(std::move(parsedBook)));
}

Book* GetBook(Settings& settings, const QString& fileName, const InpData& inpData)
{
	const auto bookIt = inpData.find(fileName);
	if (auto* book = settings.FromFile(fileName))
	{
		if (bookIt != inpData.end())
		{
			if (book != bookIt->second.get())
			{
				const auto& src = *bookIt->second;
				book->file      = src.file;
				book->ext       = src.ext;
				book->date      = src.date;
			}
		}
		else
		{
			const QFileInfo fileInfo(fileName);
			book->file = fileInfo.completeBaseName();
			book->ext  = fileInfo.suffix();
		}

		return book;
	}

	if (bookIt == inpData.end())
		return nullptr;

	auto* book = bookIt->second.get();

	settings.fileToHash[fileName]     = fileName;
	settings.libIdToHash[book->libId] = fileName;
	return settings.hashToBook.try_emplace(fileName, book).first->second;
}

BookStorage CreateInpx(const Settings& settings, const Archives& archives, const InpDataProvider& inpDataProvider)
{
	const auto unIndexed = []() -> QJsonObject {
		QFile                       file(":/data/unindexed.json");
		[[maybe_unused]] const auto ok = file.open(QIODevice::ReadOnly);
		assert(ok);

		QJsonParseError jsonParserError;
		const auto      doc = QJsonDocument::fromJson(file.readAll(), &jsonParserError);
		assert(jsonParserError.error == QJsonParseError::NoError && doc.isObject());

		return doc.object();
	}();

	const auto inpxFileName = settings.outputFolder / L"inpx.inpx";
	if (exists(inpxFileName))
		remove(inpxFileName);

	auto      zipFileController = Zip::CreateZipFileController();
	QDateTime maxTime;

	BookStorage bookStorage;

	for (const auto& [zipFileInfo, sourceLib] : archives | std::views::transform([](const auto& item) {
													return std::make_pair(QFileInfo(item.filePath), item.sourceLib);
												}))
	{
		PLOGV << zipFileInfo.fileName();
		QByteArray file;
		Zip        zip(zipFileInfo.filePath());
		for (const auto& bookFile : zip.GetFileNameList())
		{
			auto* book = inpDataProvider.GetBook({ zipFileInfo.fileName(), bookFile });
			if (book)
			{
				bookStorage.Add(book);
			}
			else
			{
				book = GetBookCustom(bookFile, bookStorage, zip, unIndexed);
				if (!book)
				{
					book = ParseBook(bookFile, bookStorage, zipFileInfo.fileName(), zip, zipFileInfo.birthTime());
					if (!book)
					{
						PLOGW << zipFileInfo.filePath() << "/" << bookFile << " not found";
						continue;
					}
				}
			}

			book->sourceLib = sourceLib;
			book->folder    = zipFileInfo.fileName();

			file << *book;

			maxTime = std::max(maxTime, zip.GetFileTime(bookFile));
		}

		if (!file.isEmpty())
			zipFileController->AddFile(zipFileInfo.completeBaseName() + ".inp", std::move(file), QDateTime::currentDateTime());
	}

	const auto collectionInfo = [&]() -> QString {
		if (!QFile::exists(settings.collectionInfoTemplateFile))
			return {};

		if (QFile file(settings.collectionInfoTemplateFile); file.open(QIODevice::ReadOnly))
			return QString::fromUtf8(file.readAll()).arg(maxTime.toString("yyyy-MM-dd"), maxTime.toString("yyyyMMdd"));

		return {};
	}();

	zipFileController->AddFile(Inpx::STRUCTURE_INFO, Inpx::INP_FIELDS_DESCRIPTION, QDateTime::currentDateTime());
	zipFileController->AddFile(QString::fromStdWString(Inpx::VERSION_INFO), maxTime.toString("yyyyMMdd").toUtf8(), QDateTime::currentDateTime());
	if (!collectionInfo.isEmpty())
		zipFileController->AddFile(QString::fromStdWString(Inpx::COLLECTION_INFO), collectionInfo.toUtf8(), QDateTime::currentDateTime());

	{
		Zip inpx(QString::fromStdWString(inpxFileName), ZipDetails::Format::Zip);
		inpx.Write(std::move(zipFileController));
	}

	return bookStorage;
}

QByteArray CreateReviewAdditional(const BookStorage& bookStorage)
{
	QJsonArray jsonArray;
	for (const auto& book : bookStorage.books | std::views::filter([&](const Book* item) {
								return item->rate > std::numeric_limits<double>::epsilon();
							}))
	{
		jsonArray.append(QJsonObject {
			{ Inpx::FOLDER,                 book->folder },
			{   Inpx::FILE, book->file + '.' + book->ext },
			{    Inpx::SUM,                   book->rate },
			{  Inpx::COUNT,              book->rateCount },
		});
	}

	if (jsonArray.isEmpty())
		return {};

	return QJsonDocument(jsonArray).toJson();
}

std::vector<std::tuple<QString, QByteArray>> CreateReviewData(const std::filesystem::path& outputFolder, const InpDataProvider& inpDataProvider, const Replacement& replacement, const BookStorage& bookStorage)
{
	auto threadPool = std::make_unique<Util::ThreadPool>();

	const auto reviewsFolder = outputFolder / Inpx::REVIEWS_FOLDER;
	QDir(reviewsFolder).mkpath(".");

	std::mutex                                   archivesGuard;
	std::vector<std::tuple<QString, QByteArray>> archives;

	threadPool->enqueue([&]() {
		auto             archiveName = QString::fromStdWString(reviewsFolder / Inpx::REVIEWS_ADDITIONAL_ARCHIVE_NAME);
		const ScopedCall logGuard(
			[&] {
				PLOGI << archiveName << " started";
			},
			[=] {
				PLOGI << archiveName << " finished";
			}
		);

		auto additional = CreateReviewAdditional(bookStorage);
		if (additional.isEmpty())
			return;

		QByteArray zipBytes;
		{
			QBuffer          buffer(&zipBytes);
			const ScopedCall bufferGuard(
				[&] {
					buffer.open(QIODevice::WriteOnly);
				},
				[&] {
					buffer.close();
				}
			);
			Zip  zip(buffer, Zip::Format::Zip);
			auto zipFiles = Zip::CreateZipFileController();
			zipFiles->AddFile(Inpx::REVIEWS_ADDITIONAL_BOOKS_FILE_NAME, std::move(additional));
			zip.Write(std::move(zipFiles));
		}

		std::lock_guard lock(archivesGuard);
		archives.emplace_back(std::move(archiveName), std::move(zipBytes));
	});

	using Data = std::vector<std::tuple<QString, QString, QString, QString, QString>>;

	const auto write = [&](const int year, const int month, Data data) {
		auto archiveName = QString::fromStdWString(reviewsFolder / std::format("{:04}{:02}", year, month)) + ".7z";

		threadPool->enqueue([&archivesGuard, &archives, archiveName = std::move(archiveName), data = std::move(data)]() mutable {
			size_t           counter = 0;
			const ScopedCall logGuard(
				[&] {
					PLOGI << archiveName << " started, books: " << data.size();
				},
				[archiveName, &counter] {
					PLOGI << archiveName << " finished, records: " << counter;
				}
			);

			std::map<std::pair<QString, QString>, std::map<std::pair<QString, QString>, QString>> sorted;
			for (auto&& [folder, file, name, time, text] : data)
				sorted[std::make_pair(std::move(folder), std::move(file))].try_emplace(std::make_pair(std::move(time), std::move(name)), std::move(text));

			auto zipFiles = Zip::CreateZipFileController();
			std::ranges::for_each(sorted, [&](auto& value) {
				QJsonArray array;
				for (auto& [id, text] : value.second)
				{
					text.prepend(' ');
					text.append(' ');
					array.append(QJsonObject {
						{ Inpx::NAME,         id.second.simplified() },
						{ Inpx::TIME,                       id.first },
						{ Inpx::TEXT, ReplaceTags(text).simplified() },
					});
					++counter;
				}
				zipFiles->AddFile(QString("%1#%2").arg(value.first.first, value.first.second), QJsonDocument(array).toJson());
			});

			QByteArray zipBytes;
			{
				QBuffer          buffer(&zipBytes);
				const ScopedCall bufferGuard(
					[&] {
						buffer.open(QIODevice::WriteOnly);
					},
					[&] {
						buffer.close();
					}
				);
				Zip zip(buffer, Zip::Format::SevenZip);
				zip.SetProperty(ZipDetails::PropertyId::SolidArchive, false);
				zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
				zip.Write(std::move(zipFiles));
			}

			std::lock_guard lock(archivesGuard);
			archives.emplace_back(std::move(archiveName), std::move(zipBytes));
		});
	};

	const auto libIdToBook = bookStorage.books | std::views::transform([](const Book* book) {
								 return std::make_pair(std::make_pair(book->libId, book->sourceLib.toLower()), book);
							 })
	                       | std::ranges::to<std::unordered_map<std::pair<QString, QString>, const Book*, Util::PairHash<QString, QString>>>();

	std::set<std::pair<int, int>> months;
	inpDataProvider.Enumerate([&](const QString&, const IDump& dump) {
		std::ranges::move(dump.GetReviewMonths(), std::inserter(months, months.end()));
		return false;
	});

	for (const auto& [year, month] : months)
	{
		inpDataProvider.Enumerate([&](const QString& sourceLib, const IDump& dump) {
			Data data;

			dump.Review(year, month, [&](const QString& libId, QString name, QString time, QString text) {
				const auto it = libIdToBook.find(std::make_pair(libId, sourceLib));
				if (it == libIdToBook.end())
					return;

				auto* book = it->second;
				if (const auto rIt = replacement.find({ book->folder, book->file }); rIt != replacement.end())
					book = inpDataProvider.GetBook({ rIt->second.first, rIt->second.second });
				if (!book)
					return;

				data.emplace_back(book->folder, book->GetFileName(), std::move(name), std::move(time), std::move(text));
			});

			if (!data.empty())
				write(year, month, std::move(data));

			return false;
		});
	}

	threadPool.reset();

	return archives;
}

void CreateBookList(const std::filesystem::path& outputFolder, const BookStorage& bookStorage)
{
	PLOGI << "write contents";

	const auto getSortedString = [](const QString& src) {
		return src.isEmpty() ? QString(QChar { 0xffff }) : src.toLower().simplified();
	};
	const auto getSortedNum = [](const QString& src) {
		if (src.isEmpty())
			return std::numeric_limits<int>::max();

		bool       ok    = false;
		const auto value = src.toInt(&ok);
		return ok ? value : std::numeric_limits<int>::max();
	};

	std::unordered_map<QString, std::vector<std::pair<const Book*, std::tuple<QString, QString, int, QString>>>> langs;
	for (const auto* book : bookStorage.books)
	{
		assert(book);
		langs[book->lang].emplace_back(
			book,
			std::make_tuple(getSortedString(book->author), getSortedString(book->series.front().title), getSortedNum(book->series.front().serNo), getSortedString(book->title))
		);
	}

	auto zipFiles = Zip::CreateZipFileController();
	std::ranges::for_each(langs, [&](auto& value) {
		QByteArray data;
		std::ranges::sort(value.second, {}, [](const auto& item) {
			return item.second;
		});

		for (const Book* book : value.second | std::views::keys)
			data.append(QString("%1\t%2\t%3\t%4\t%5\x0d\x0a")
			                .arg(
								book->author,
								book->title,
								book->series.empty() || book->series.front().title.isEmpty()
									? QString()
									: QString("[%1%2]").arg(book->series.front().title, book->series.front().serNo.isEmpty() ? QString {} : QString(" #%1").arg(book->series.front().serNo)),
								book->folder,
								book->GetFileName()
							)
			                .toUtf8());

		zipFiles->AddFile(value.first + ".txt", std::move(data));
	});

	PLOGI << "archive contents";
	const auto contentsFile = outputFolder / Inpx::CONTENTS;
	remove(contentsFile);

	Zip zip(QString::fromStdWString(contentsFile), Zip::Format::SevenZip);
	zip.SetProperty(ZipDetails::PropertyId::SolidArchive, false);
	zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
	zip.Write(std::move(zipFiles));
}

void ProcessCompilations(Settings& settings)
{
	PLOGI << "collect compilation info";

	std::unordered_multimap<QString, Book*> sectionToBook;
	std::ranges::transform(
		settings.hashToBook | std::views::values | std::views::filter([&](const Book* book) {
			return book->section && settings.fileToFolder.contains(book->file + '.' + book->ext);
		}),
		std::inserter(sectionToBook, sectionToBook.end()),
		[](Book* book) {
			return std::make_pair(book->id, book);
		}
	);

	if (sectionToBook.empty())
	{
		PLOGI << "compilations not found";
		return;
	}

	const auto enumerate =
		[&](const QString& folder, const QString& file, const Section& parent, QJsonArray& found, std::unordered_set<QString>& idNotFound, std::unordered_set<QString>& idFound, const auto& r) -> void {
		for (const auto& [id, child] : parent.children)
		{
			if (child->count < 100)
				continue;

			auto [it, end] = sectionToBook.equal_range(id);
			if (it == end)
			{
				if (child->children.empty())
					idNotFound.insert(id);
				else
					r(folder, file, *child, found, idNotFound, idFound, r);
				continue;
			}

			for (; it != end; ++it)
			{
				const auto folderIt = settings.fileToFolder.find(it->second->file + '.' + it->second->ext);
				if (folderIt == settings.fileToFolder.end() || (folderIt->second.contains(folder) && it->second->file == file))
					continue;

				found.append(QJsonObject {
					{   Inpx::PART,       std::ssize(idFound) },
					{ Inpx::FOLDER,  folderIt->second.front() },
					{   Inpx::FILE, it->second->GetFileName() },
				});

				idFound.emplace(id);
				child->children.clear();
			}

			r(folder, file, *child, found, idNotFound, idFound, r);
		}
	};

	QJsonArray compilations;

	for (const auto* book : sectionToBook | std::views::values)
	{
		const auto folderIt = settings.fileToFolder.find(book->file + '.' + book->ext);
		assert(folderIt != settings.fileToFolder.end());
		QJsonArray                  found;
		std::unordered_set<QString> idNotFound;
		std::unordered_set<QString> idFound;
		enumerate(folderIt->second.front(), book->file, *book->section, found, idNotFound, idFound, enumerate);
		if (idFound.size() > 1)
		{
			QJsonObject compilation {
				{      Inpx::FOLDER, folderIt->second.front() },
				{        Inpx::FILE,      book->GetFileName() },
				{ Inpx::COMPILATION,         std::move(found) },
				{     Inpx::COVERED,       idNotFound.empty() },
			};
			/*
			if (!idNotFound.empty())
			{
				QJsonArray notFound;
				std::ranges::copy(idNotFound, std::back_inserter(notFound));
				compilation.insert("not_found", std::move(notFound));
			}
*/
			compilations.append(std::move(compilation));
		}
	}

	PLOGI << "archive compilation info";
	const auto contentsFile = settings.outputFolder / Inpx::COMPILATIONS;
	remove(contentsFile);

	auto zipFiles = Zip::CreateZipFileController();
	zipFiles->AddFile(Inpx::COMPILATIONS_JSON, QJsonDocument(compilations).toJson());

	Zip zip(QString::fromStdWString(contentsFile), Zip::Format::SevenZip);
	zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
	zip.Write(std::move(zipFiles));
}

void CreateReview(const std::filesystem::path& outputFolder, const InpDataProvider& inpDataProvider, const Replacement& replacement, const BookStorage& bookStorage)
{
	PLOGI << "write reviews";

	for (const auto& [fileName, data] : CreateReviewData(outputFolder, inpDataProvider, replacement, bookStorage))
		Write(fileName, data);
}

class FileHashParser : HashParser::IObserver
{
public:
	FileHashParser(Archive& archive, InpDataProvider& inpDataProvider, Replacement& replacement, Util::Progress& progress)
		: m_sourceLib { archive.sourceLib }
		, m_inpDataProvider { inpDataProvider }
		, m_replacement { replacement }
		, m_progress { progress }
	{
		QFile file(archive.hashPath);
		if (!file.open(QIODevice::ReadOnly))
			throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));
		HashParser::Parse(file, *this);
	}

private:
	void OnParseStarted(const QString& sourceLib) override
	{
		m_sourceLib = sourceLib;
		m_inpDataProvider.SetSourceLib(sourceLib);
	}

	void OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			QString /*cover*/,
		QStringList /*images*/
	) override
	{
		UniqueFile::Uid uid { folder, file };
		m_inpDataProvider.SetFile(uid);

		if (!originFolder.isEmpty())
			return (void)m_replacement.try_emplace(std::make_pair(std::move(uid.file), std::move(uid.file)), std::make_pair(std::move(originFolder), std::move(originFile)));

		m_progress.Increment(1, file.toStdString());
	}

private:
	QString&         m_sourceLib;
	InpDataProvider& m_inpDataProvider;
	Replacement&     m_replacement;
	Util::Progress&  m_progress;
};

Replacement ReadHash(const size_t totalFileCount, InpDataProvider& inpDataProvider, Archives& archives)
{
	Replacement    replacement;
	Util::Progress progress(totalFileCount, "parsing");

	for (auto& archive : archives)
		FileHashParser(archive, inpDataProvider, replacement, progress);

	return replacement;
}

void MergeRate(const InpDataProvider& inpDataProvider, const Replacement& replacement)
{
	for (const auto& [fileUid, originUid] : replacement)
	{
		if (const auto* file = inpDataProvider.GetBook({ fileUid.first, fileUid.second }))
		{
			if (auto* origin = inpDataProvider.GetBook({ originUid.first, originUid.second }))
			{
				origin->rate      += file->rate;
				origin->rateCount += file->rateCount;
			}
		}
	}
}

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv);

	Util::XMLPlatformInitializer xmlPlatformInitializer;

	Settings settings {};

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 creates inpx for flibusta collections").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives with hashes (required)");
	parser.addOptions({
		{				   { "o", OUTPUT }, "Output folder (required)",                              FOLDER },
		{							  DUMP,  "Dump database wildcards", "Semicolon separated wildcard list" },
		{ { "i", COLLECTION_INFO_TEMPLATE }, "Collection info template",                                PATH },
	});
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	Log::LoggingInitializer                          logging((parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	if (parser.positionalArguments().isEmpty() || !parser.isSet(OUTPUT))
		parser.showHelp(1);

	settings.outputFolder               = parser.value(OUTPUT).toStdWString();
	settings.collectionInfoTemplateFile = parser.value(COLLECTION_INFO_TEMPLATE).toStdWString();

	auto       archives        = GetArchives(parser.positionalArguments());
	const auto totalFileCount  = Total(archives);
	const auto inpDataProvider = std::make_shared<InpDataProvider>(parser.value(DUMP));
	const auto replacement     = ReadHash(totalFileCount, *inpDataProvider, archives);
	MergeRate(*inpDataProvider, replacement);

	const auto bookStorage = CreateInpx(settings, archives, *inpDataProvider);

	CreateBookList(settings.outputFolder, bookStorage);
	CreateReview(settings.outputFolder, *inpDataProvider, replacement, bookStorage);
	//	ProcessCompilations(settings);
	//	dump->CreateAdditional(settings.sqlFolder, settings.outputFolder);

	return 0;
}
