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

#include "lib/UniqueFile.h"
#include "lib/archive.h"
#include "lib/book.h"
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

using BookItem    = std::pair<QString, QString>;
using Replacement = std::unordered_map<BookItem, BookItem, Util::PairHash<QString, QString>>;

struct Settings
{
	std::filesystem::path outputFolder;
	std::filesystem::path collectionInfoTemplateFile;
};

struct FileInfo
{
	QByteArray hash;
	qsizetype  size;
};

class FileHashParser final : HashParser::IObserver
{
public:
	FileHashParser(Archive& archive, InpDataProvider& inpDataProvider, Replacement& replacement)
		: m_sourceLib { archive.sourceLib }
		, m_inpDataProvider { inpDataProvider }
		, m_replacement { replacement }
	{
		QFile file(archive.hashPath);
		if (!file.open(QIODevice::ReadOnly))
			throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));
		HashParser::Parse(file, *this);
	}

private: // HashParser::IObserver
	void OnParseStarted(const QString& sourceLib) override
	{
		m_sourceLib = sourceLib;
		m_inpDataProvider.SetSourceLib(sourceLib);
	}

	bool OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			HashParser::HashImageItem /*cover*/,
		HashParser::HashImageItems /*images*/,
		Section::Ptr,
		TextHistogram
	) override
	{
		UniqueFile::Uid uid { folder, file };
		if (!originFolder.isEmpty())
			m_replacement.try_emplace(std::make_pair(uid.folder, uid.file), std::make_pair(std::move(originFolder), std::move(originFile)));

		m_inpDataProvider.SetFile(uid, std::move(id));

		return true;
	}

private:
	QString&         m_sourceLib;
	InpDataProvider& m_inpDataProvider;
	Replacement&     m_replacement;
};

class CompilationHandler final : HashParser::IObserver
{
public:
	CompilationHandler(const Archives& archives, const InpDataProvider& inpDataProvider)
		: m_inpDataProvider { inpDataProvider }
		, m_sectionToBook { m_inpDataProvider.Books() | std::views::transform([](Book* book) {
								return std::make_pair(book->id, book);
							})
		                    | std::ranges::to<std::unordered_multimap<QString, Book*>>() }
		, m_progress { archives.size(), "compilations" }
	{
		if (m_sectionToBook.empty())
			return;

		for (const auto& archive : archives)
		{
			QFile file(archive.hashPath);
			if (!file.open(QIODevice::ReadOnly))
				throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));
			HashParser::Parse(file, *this);
			m_progress.Increment(1, QFileInfo(archive.hashPath).fileName().toStdString());
		}
	}

	QByteArray GetResult() const
	{
		if (!m_compilations.isEmpty())
			return QJsonDocument(std::move(m_compilations)).toJson();

		PLOGI << "compilations not found";
		return {};
	}

private: // HashParser::IObserver
	void OnParseStarted(const QString&) override
	{
	}

	bool OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			HashParser::HashImageItem /*cover*/,
		HashParser::HashImageItems /*images*/,
		Section::Ptr section,
		TextHistogram
	) override
	{
		if (!originFolder.isEmpty())
			return true;

		const auto enumerate = [this](const Book* book, const Section& parent, QJsonArray& found, std::unordered_set<QString>& idNotFound, std::unordered_set<QString>& idFound, const auto& r) -> void {
			for (const auto& [childId, child] : parent.children)
			{
				if (child->count < 100)
					continue;

				if (childId == book->id)
					return r(book, *child, found, idNotFound, idFound, r);

				auto [it, end] = m_sectionToBook.equal_range(childId);
				if (it == end)
				{
					if (child->children.empty())
						idNotFound.insert(childId);
					else
						r(book, *child, found, idNotFound, idFound, r);
					continue;
				}

				for (; it != end; ++it)
				{
					found.append(
						QJsonObject {
							{   Inpx::PART,       std::ssize(idFound) },
							{ Inpx::FOLDER,        it->second->folder },
							{   Inpx::FILE, it->second->GetFileName() },
                    }
					);

					idFound.emplace(childId);
					child->children.clear();
				}

				r(book, *child, found, idNotFound, idFound, r);
			}
		};

		const auto* book = m_inpDataProvider.GetBook({ folder, file });
		if (!book)
			return true;

		QJsonArray                  found;
		std::unordered_set<QString> idNotFound;
		std::unordered_set<QString> idFound;

		enumerate(book, *section, found, idNotFound, idFound, enumerate);
		if (idFound.size() > 1)
		{
			QJsonObject compilation {
				{      Inpx::FOLDER,        book->folder },
				{        Inpx::FILE, book->GetFileName() },
				{ Inpx::COMPILATION,    std::move(found) },
				{     Inpx::COVERED,  idNotFound.empty() },
			};

			//			if (!idNotFound.empty())
			//			{
			//				QJsonArray notFound;
			//				std::ranges::copy(idNotFound, std::back_inserter(notFound));
			//				compilation.insert("not_found", std::move(notFound));
			//			}

			m_compilations.append(std::move(compilation));
		}

		return true;
	}

private:
	const InpDataProvider&                        m_inpDataProvider;
	const std::unordered_multimap<QString, Book*> m_sectionToBook;
	QJsonArray                                    m_compilations;
	Util::Progress                                m_progress;
};

FileInfo GetFileHash(const Zip& zip, const QString& fileName)
{
	const auto fileData = zip.Read(fileName)->GetStream().readAll();

	QCryptographicHash hash(QCryptographicHash::Algorithm::Md5);
	hash.addData(fileData);
	return { hash.result().toHex(), fileData.size() };
}

Book* GetBookCustom(const QString& fileName, InpDataProvider& inpDataProvider, const Zip& zip, const QJsonObject& unIndexed)
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

	return inpDataProvider.AddBook(
		std::make_unique<Book>(Book {
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
		})
	);
}

Book* ParseBook(const QString& fileName, InpDataProvider& inpDataProvider, const QString& folder, const Zip& zip, const QDateTime& zipDateTime)
{
	if (!fileName.endsWith(".fb2", Qt::CaseInsensitive))
		return nullptr;

	const auto hash = GetFileHash(zip, fileName).hash;
	PLOGV << "parse " << fileName << ", hash: " << hash;

	auto parsedBook = Book::FromString(Util::Fb2InpxParser::Parse(folder, zip, fileName, zipDateTime, true));
	if (parsedBook.title.isEmpty())
		return nullptr;

	return inpDataProvider.AddBook(std::make_unique<Book>(std::move(parsedBook)));
}

void CreateInpx(const Settings& settings, const Archives& archives, InpDataProvider& inpDataProvider)
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

	const auto seriesUniquePredicate = [](const auto& item) {
		return item.title;
	};
	const auto seriesOrdNumPredicate = [](const auto& item) {
		return item.level;
	};

	const auto inpxFileName = settings.outputFolder / (QFileInfo(archives.front().filePath).dir().dirName() + ".inpx").toStdString();
	if (exists(inpxFileName))
		remove(inpxFileName);

	auto      zipFileController = Zip::CreateZipFileController();
	QDateTime maxTime;

	for (const auto& [zipFileInfo, sourceLib] : archives | std::views::reverse | std::views::transform([](const auto& item) {
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
				inpDataProvider.AddBook(book);
			}
			else
			{
				book = GetBookCustom(bookFile, inpDataProvider, zip, unIndexed);
				if (!book)
				{
					book = ParseBook(bookFile, inpDataProvider, zipFileInfo.fileName(), zip, zipFileInfo.birthTime());
					if (!book)
					{
						PLOGW << zipFileInfo.filePath() << "/" << bookFile << " not found";
						continue;
					}
				}
			}

			book->sourceLib = sourceLib;
			book->folder    = zipFileInfo.fileName();

			auto& series = book->series;
			std::ranges::sort(series, std::greater {}, seriesUniquePredicate);
			if (const auto [begin, end] = std::ranges::unique(series, {}, seriesUniquePredicate); begin != end)
				series.erase(begin, end);
			if (series.size() > 1 && series.back().title.isEmpty())
				series.pop_back();
			std::ranges::sort(series, {}, seriesOrdNumPredicate);

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
}

QByteArray CreateReviewAdditional(const InpDataProvider& inpDataProvider)
{
	QJsonArray jsonArray;
	for (const auto& book : inpDataProvider.Books() | std::views::filter([&](const Book* item) {
								return item->rate > std::numeric_limits<double>::epsilon();
							}))
	{
		jsonArray.append(
			QJsonObject {
				{ Inpx::FOLDER,                 book->folder },
				{   Inpx::FILE, book->file + '.' + book->ext },
				{    Inpx::SUM,                   book->rate },
				{  Inpx::COUNT,              book->rateCount },
        }
		);
	}

	if (jsonArray.isEmpty())
		return {};

	return QJsonDocument(jsonArray).toJson();
}

std::vector<std::tuple<QString, QByteArray>> CreateReviewData(const std::filesystem::path& outputFolder, const InpDataProvider& inpDataProvider, const Replacement& replacement)
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

		auto additional = CreateReviewAdditional(inpDataProvider);
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
					array.append(
						QJsonObject {
							{ Inpx::NAME,         id.second.simplified() },
							{ Inpx::TIME,                       id.first },
							{ Inpx::TEXT, ReplaceTags(text).simplified() },
                    }
					);
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

	PLOGI << "Creating LibID to book index";
	const auto libIdToBook = inpDataProvider.Books() | std::views::transform([](const Book* book) {
								 return std::make_pair(std::make_pair(book->libId, book->sourceLib.toLower()), book);
							 })
	                       | std::ranges::to<std::unordered_map<std::pair<QString, QString>, const Book*, Util::PairHash<QString, QString>>>();

	PLOGI << "Get review months";
	std::set<std::pair<int, int>> months;
	inpDataProvider.Enumerate([&](const QString&, const IDump& dump) {
		std::ranges::move(dump.GetReviewMonths(), std::inserter(months, months.end()));
		return false;
	});

	Util::Progress progress(months.size(), "select reviews");
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
		progress.Increment(1, std::format("{:04}-{:02}", year, month));
	}

	threadPool.reset();

	return archives;
}

void CreateBookList(const std::filesystem::path& outputFolder, const InpDataProvider& inpDataProvider)
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
	for (const auto* book : inpDataProvider.Books())
	{
		assert(book);
		const auto& series = book->series.front();
		langs[book->lang].emplace_back(book, std::make_tuple(getSortedString(book->author), getSortedString(series.title), getSortedNum(series.serNo), getSortedString(book->title)));
	}

	auto zipFiles = Zip::CreateZipFileController();
	std::ranges::for_each(langs, [&](auto& value) {
		QByteArray data;
		std::ranges::sort(value.second, {}, [](const auto& item) {
			return item.second;
		});

		for (const Book* book : value.second | std::views::keys)
		{
			const auto& series = book->series.front();
			data.append(QString("%1\t%2\t%3\t%4\t%5\x0d\x0a")
			                .arg(
								book->author,
								book->title,
								book->series.empty() || series.title.isEmpty() ? QString() : QString("[%1%2]").arg(series.title, series.serNo.isEmpty() ? QString {} : QString(" #%1").arg(series.serNo)),
								book->folder,
								book->GetFileName()
							)
			                .toUtf8());
		}

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

void ProcessCompilations(const std::filesystem::path& outputFolder, const Archives& archives, const InpDataProvider& inpDataProvider)
{
	PLOGI << "collect compilation info";
	const CompilationHandler compilationHandler(archives, inpDataProvider);
	auto                     data = compilationHandler.GetResult();
	if (data.isEmpty())
		return;

	PLOGI << "archive compilation info";
	const auto contentsFile = outputFolder / Inpx::COMPILATIONS;
	remove(contentsFile);

	auto zipFiles = Zip::CreateZipFileController();
	zipFiles->AddFile(Inpx::COMPILATIONS_JSON, std::move(data));

	Zip zip(QString::fromStdWString(contentsFile), Zip::Format::SevenZip);
	zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
	zip.Write(std::move(zipFiles));
}

void CreateReview(const std::filesystem::path& outputFolder, const InpDataProvider& inpDataProvider, const Replacement& replacement)
{
	PLOGI << "write reviews";

	for (const auto& [fileName, data] : CreateReviewData(outputFolder, inpDataProvider, replacement))
		Write(fileName, data);
}

Replacement ReadHash(InpDataProvider& inpDataProvider, Archives& archives)
{
	Replacement    replacement;
	Util::Progress progress(archives.size(), "parsing");

	for (auto& archive : archives)
	{
		const FileHashParser parser(archive, inpDataProvider, replacement);
		progress.Increment(1, QFileInfo(archive.hashPath).fileName().toStdString());
	}

	return replacement;
}

void MergeBookData(const InpDataProvider& inpDataProvider, const Replacement& replacement)
{
	struct BookIndexItem
	{
		BookItem                    uid;
		std::vector<BookIndexItem*> children;
	};

	std::unordered_map<BookItem, BookIndexItem, Util::PairHash<QString, QString>> index;
	for (const auto& [fileUid, originUid] : replacement)
	{
		auto& origin = index.try_emplace(originUid, BookIndexItem { originUid }).first->second;
		auto& file   = index.try_emplace(fileUid, BookIndexItem { fileUid }).first->second;
		origin.children.emplace_back(&file);
	}

	const auto enumerate = [&](Book& origin, const BookIndexItem& parent, const auto& r) -> void {
		for (const auto* item : parent.children)
		{
			if (const auto* file = inpDataProvider.GetBook({ item->uid.first, item->uid.second }))
			{
				origin.rate      += file->rate;
				origin.rateCount += file->rateCount;

				std::ranges::copy(file->series, std::back_inserter(origin.series));

				origin.deleted = origin.deleted && file->deleted;
			}
			r(origin, *item, r);
		}
	};

	for (const auto& indexItem : index | std::views::filter([&](const auto& item) {
									 return !replacement.contains(item.first);
								 }) | std::views::values)
	{
		if (auto* origin = inpDataProvider.GetBook({ indexItem.uid.first, indexItem.uid.second }))
		{
			enumerate(*origin, indexItem, enumerate);
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
	parser.addOptions(
		{
			{				   { "o", OUTPUT }, "Output folder (required)",                              FOLDER },
			{							  DUMP,  "Dump database wildcards", "Semicolon separated wildcard list" },
			{ { "i", COLLECTION_INFO_TEMPLATE }, "Collection info template",                                PATH },
    }
	);
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

	auto archives = GetArchives(parser.positionalArguments());
	Total(archives);

	const auto inpDataProvider = std::make_shared<InpDataProvider>(parser.value(DUMP));
	const auto replacement     = ReadHash(*inpDataProvider, archives);

	MergeBookData(*inpDataProvider, replacement);
	CreateInpx(settings, archives, *inpDataProvider);
	CreateBookList(settings.outputFolder, *inpDataProvider);
	CreateReview(settings.outputFolder, *inpDataProvider, replacement);
	ProcessCompilations(settings.outputFolder, archives, *inpDataProvider);

	return 0;
}
