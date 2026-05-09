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
#include "fnd/StrUtil.h"
#include "fnd/try.h"

#include "lib/UniqueFile.h"
#include "lib/archive.h"
#include "lib/book.h"
#include "lib/dump/Factory.h"
#include "lib/dump/IDump.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/EpubParser.h"
#include "util/Fb2InpxParser.h"
#include "util/LogConsoleFormatter.h"
#include "util/bookhash/hashparser.h"
#include "util/executor/ThreadPool.h"
#include "util/language.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlWriter.h"

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
constexpr auto MAX_SERIES                   = "max-series-per-book";
constexpr auto LIBRARY                      = "library";
constexpr auto INPX_ONLY                    = "inpx-only";
constexpr auto SKIP_CONTENTS                = "skip-contents";
constexpr auto SKIP_REVIEWS                 = "skip-reviews";
constexpr auto SKIP_COMPILATIONS            = "skip-compilations";
constexpr auto SKIP_ANNOTATIONS             = "skip-annotations";
constexpr auto DELETED                      = "deleted";

constexpr auto APP_ID = "fliparser";

const auto DASH = QString(" %1 ").arg(QChar { 0x2013 });

using BookItem      = std::pair<QString, QString>;
using Replacement   = std::unordered_map<BookItem, BookItem, Util::PairHash<QString, QString>>;
using SectionToBook = std::unordered_multimap<QString, Book*>;

struct Settings
{
	std::filesystem::path outputFolder;
	std::filesystem::path collectionInfoTemplateFile;
	QString               sourceLib;
	ptrdiff_t             maxSeriesPerBook { std::numeric_limits<ptrdiff_t>::max() };
	bool                  isDeleted { false };
};

struct FileInfo
{
	QByteArray hash;
	qsizetype  size;
};

class IAnnotationCollector // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
	virtual ~IAnnotationCollector()                                                             = default;
	virtual void StartFolder()                                                                  = 0;
	virtual void Add(const QString& folder, const QString& file, const QStringList& annotation) = 0;
};

class AnnotationCollectorStub final : virtual public IAnnotationCollector
{
public:
	static std::unique_ptr<IAnnotationCollector> Create()
	{
		return std::make_unique<AnnotationCollectorStub>();
	}

private: // IAnnotationCollector
	void StartFolder() override
	{
	}

	void Add(const QString&, const QString&, const QStringList&) override
	{
	}
};

class AnnotationCollector final : virtual public IAnnotationCollector
{
	NON_COPY_MOVABLE(AnnotationCollector)

	class Data
	{
		NON_COPY_MOVABLE(Data)

	public:
		Data(IZipFileController& zipFiles, QString folder)
			: m_zipFiles { zipFiles }
			, m_folder { std::move(folder) }
		{
			(*m_folderGuard)->WriteAttribute("name", m_folder);
		}

		~Data()
		{
			m_folderGuard.reset();
			m_writer.reset();
			m_stream.reset();
			if (m_found)
				m_zipFiles.AddFile(std::move(m_folder), m_data);
		}

		void Add(const QString& file, const QStringList& annotation)
		{
			if (annotation.isEmpty())
				return;

			m_found = true;

			auto item = (*m_folderGuard)->Guard("file");
			item->WriteAttribute("name", file);
			for (const auto& str : annotation)
				item->WriteStartElement("p").WriteCharacters(str).WriteEndElement();
		}

	private:
		static std::unique_ptr<QIODevice> CreateStream(QByteArray& data)
		{
			auto stream = std::make_unique<QBuffer>(&data);
			stream->open(QIODevice::WriteOnly);
			return stream;
		}

	private:
		IZipFileController& m_zipFiles;
		QString             m_folder;

		QByteArray                                     m_data;
		std::unique_ptr<QIODevice>                     m_stream { CreateStream(m_data) };
		std::unique_ptr<Util::XmlWriter>               m_writer { std::make_unique<Util::XmlWriter>(*m_stream) };
		std::unique_ptr<Util::XmlWriter::XmlNodeGuard> m_folderGuard { std::make_unique<Util::XmlWriter::XmlNodeGuard>(m_writer->Guard("folder")) };

		bool m_found { false };
	};

public:
	static std::unique_ptr<IAnnotationCollector> Create(const std::filesystem::path& outputFolder)
	{
		return std::make_unique<AnnotationCollector>(outputFolder);
	}

	explicit AnnotationCollector(const std::filesystem::path& outputFolder)
		: m_outputFolder { outputFolder }
	{
	}

	~AnnotationCollector() override
	{
		PLOGI << "archive annotations";
		m_data.reset();

		const auto zipFileName = Platform::PathToString(m_outputFolder / Inpx::ANNOTATIONS);
		QFile::remove(zipFileName);
		Zip zip(zipFileName, ZipDetails::Format::SevenZip);
		zip.SetProperty(ZipDetails::PropertyId::SolidArchive, false);
		zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
		zip.Write(*m_zipFiles);
	}

private: // IAnnotationCollector
	void StartFolder() override
	{
		m_data.reset();
	}

	void Add(const QString& folder, const QString& file, const QStringList& annotation) override
	{
		if (annotation.isEmpty())
			return;

		if (!m_data)
			m_data = std::make_unique<Data>(*m_zipFiles, folder);

		m_data->Add(file, annotation);
	}

private:
	const std::filesystem::path&        m_outputFolder;
	std::shared_ptr<IZipFileController> m_zipFiles { Zip::CreateZipFileController() };
	std::unique_ptr<Data>               m_data;
};

class FileHashParser final : Util::HashParser::IObserver
{
public:
	struct ParseStorage
	{
		std::reference_wrapper<Archive>                  archive;
		QByteArray                                       bytes;
		std::vector<std::pair<BookItem, BookItem>>       replacement;
		std::vector<std::pair<UniqueFile::Uid, QString>> data;
	};

public:
	explicit FileHashParser(ParseStorage& parseStorage)
		: m_parseStorage { parseStorage }
		, m_folderExt { QFileInfo(parseStorage.archive.get().filePath).suffix().toLower() }
	{
		QBuffer buffer(&parseStorage.bytes);
		buffer.open(QIODevice::ReadOnly);
		Util::HashParser::Parse(buffer, *this);
		parseStorage.bytes.clear();
	}

private: // HashParser::IObserver
	void OnParseStarted(const QString& sourceLib) override
	{
		m_parseStorage.archive.get().sourceLib = sourceLib;
	}

	bool OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			Util::HashParser::HashImageItem /*cover*/,
		Util::HashParser::HashImageItems /*images*/,
		Util::HashParser::Section::Ptr,
		Util::TextHistogram,
		QStringList
	) override
	{
		if (!m_folderExt.isEmpty())
			if (const auto pos = folder.lastIndexOf('.'); pos > 0)
				folder = folder.first(pos + 1) + m_folderExt;

		UniqueFile::Uid uid { folder, file };
		if (!originFolder.isEmpty())
			m_parseStorage.replacement.emplace_back(std::make_pair(uid.folder, uid.file), std::make_pair(std::move(originFolder), std::move(originFile)));

		m_parseStorage.data.emplace_back(std::move(uid), std::move(id));

		return true;
	}

private:
	ParseStorage& m_parseStorage;
	const QString m_folderExt;
};

class CompilationHandler final : Util::HashParser::IObserver
{
public:
	struct ParseStorage
	{
		struct DataItem
		{
			QString     folder;
			QString     file;
			QStringList annotation;
		};

		std::reference_wrapper<const Archive> archive;
		QByteArray                            bytes;
		std::vector<QJsonObject>              compilations;
		std::vector<DataItem>                 data;
	};

public:
	CompilationHandler(const InpDataProvider& inpDataProvider, const SectionToBook& sectionToBook, ParseStorage& parseStorage)
		: m_inpDataProvider { inpDataProvider }
		, m_sectionToBook { sectionToBook }
		, m_folderExt { QFileInfo(parseStorage.archive.get().filePath).suffix().toLower() }
		, m_parseStorage { parseStorage }
	{
		assert(!m_sectionToBook.empty());

		QBuffer buffer(&parseStorage.bytes);
		buffer.open(QIODevice::ReadOnly);
		Util::HashParser::Parse(buffer, *this);
		parseStorage.bytes.clear();
	}

private: // HashParser::IObserver
	void OnParseStarted(const QString&) override
	{
	}

	bool OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			Util::HashParser::HashImageItem /*cover*/,
		Util::HashParser::HashImageItems /*images*/,
		Util::HashParser::Section::Ptr section,
		Util::TextHistogram,
		QStringList annotation
	) override
	{
		if (!originFolder.isEmpty())
			return true;

		if (!m_folderExt.isEmpty())
			if (const auto pos = folder.lastIndexOf('.'); pos > 0)
				folder = folder.first(pos + 1) + m_folderExt;

		if (!annotation.isEmpty())
			m_parseStorage.data.emplace_back(folder, file, std::move(annotation));

		const auto enumerate =
			[this](const Book* book, const Util::HashParser::Section& parent, QJsonArray& found, std::unordered_set<QString>& idNotFound, std::unordered_set<QString>& idFound, const auto& r) -> void {
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
							{   Inpx::PART, static_cast<qlonglong>(idFound.size()) },
							{ Inpx::FOLDER,                     it->second->folder },
							{   Inpx::FILE,              it->second->GetFileName() },
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

			m_parseStorage.compilations.emplace_back(std::move(compilation));
		}

		return true;
	}

private:
	const InpDataProvider& m_inpDataProvider;
	const SectionToBook&   m_sectionToBook;
	const QString          m_folderExt;
	ParseStorage&          m_parseStorage;
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

void SetOriginalNames(Book& book, const QString& originBaseName, const QString& originSuffix)
{
	if (!originBaseName.isEmpty())
		book.libId = book.file = originBaseName;
	if (!originSuffix.isEmpty())
		book.ext = originSuffix;
}

Book ParseStub(
	const QString& /*folder*/,
	const Zip& /*zip*/,
	const QString& /*fileName*/,
	const QDateTime& /*zipDateTime*/,
	const bool /*isDeleted*/,
	const QString& /*originBaseName*/,
	const QString& /*originSuffix*/
)
{
	return {};
}

Book ParseFb2(const QString& folder, const Zip& zip, const QString& fileName, const QDateTime& zipDateTime, const bool isDeleted, const QString& originBaseName = {}, const QString& originSuffix = {})
{
	auto parsedBook = Book::FromString(Util::Fb2InpxParser::Parse(folder, zip, fileName, zipDateTime, isDeleted).line);
	SetOriginalNames(parsedBook, originBaseName, originSuffix);
	return parsedBook;
}

Book ParseEpub(const QString& folder, const Zip& zip, const QString& fileName, const QDateTime& zipDateTime, const bool isDeleted, const QString& originBaseName = {}, const QString& originSuffix = {})
{
	const auto authorsToString = [](std::vector<QStringList> authors) {
		QStringList values;
		values.reserve(static_cast<int>(authors.size()));
		std::ranges::transform(authors, std::back_inserter(values), [](const auto& author) {
			return author.join(Util::Fb2InpxParser::NAMES_SEPARATOR);
		});
		return values.join(Util::Fb2InpxParser::LIST_SEPARATOR) + Util::Fb2InpxParser::LIST_SEPARATOR;
	};

	const auto genresToString = [](const QStringList& genres) {
		return genres.empty() ? QString {} : genres.join(Util::Fb2InpxParser::LIST_SEPARATOR) + Util::Fb2InpxParser::LIST_SEPARATOR;
	};

	const QFileInfo fileInfo(fileName);
	try
	{
		auto parseResult = Util::EpubParser::Parse(zip, fileName);
		Book book {
			.author  = authorsToString(std::move(parseResult.authors)),
			.genre   = genresToString(parseResult.genres),
			.title   = std::move(parseResult.title),
			.series  = { {} },
			.file    = fileInfo.completeBaseName(),
			.size    = QString::number(zip.GetFileSize(fileName)),
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
	return {};
}

Book ParseFbd(const QString& folder, const Zip& zip, const QString& fileName, const QDateTime& zipDateTime, const bool isDeleted, const QString& /*originBaseName*/ = {}, const QString& /*originSuffix*/ = {})
{
	const QFileInfo fileInfo(fileName);
	if (const auto fbdFileName = fileName + ".fbd"; zip.GetFileIndex(fbdFileName) != Zip::INVALID_INDEX)
		return ParseFb2(folder, zip, fbdFileName, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix());
	if (const auto fbdFileName = fileInfo.completeBaseName() + ".fbd"; zip.GetFileIndex(fbdFileName) != Zip::INVALID_INDEX)
		return ParseFb2(folder, zip, fbdFileName, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix());
	return {};
}

Book ParseZip(const QString& folder, const Zip& zip, const QString& fileName, const QDateTime& zipDateTime, bool isDeleted, const QString& /*originBaseName*/ = {}, const QString& /*originSuffix*/ = {});

using FileParser =
	Book (*)(const QString& /*folder*/, const Zip&, const QString& /*fileName*/, const QDateTime& /*zipDateTime*/, bool /*isDeleted*/, const QString& /*originBaseName*/, const QString& /*originSuffix*/);
constexpr std::pair<const char*, std::pair<FileParser, bool /*parser exists*/>> FILE_PARSERS[] {
	{  ".fb2",   { &ParseFb2, true } },
    { ".epub",  { &ParseEpub, true } },
    {  ".fbd", { &ParseStub, false } },
    {  ".zip",  { &ParseZip, false } },
    {   ".7z",  { &ParseZip, false } },
    {  ".rar",  { &ParseZip, false } },
};

Book ParseZip(const QString& folder, const Zip& zip, const QString& fileName, const QDateTime& zipDateTime, const bool isDeleted, const QString& /*originBaseName*/, const QString& /*originSuffix*/)
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
				auto book = parserPair.first(folder, *subZip, subZipFile, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix());
				if (!book.title.isEmpty())
					return book;
			}
		}
	}

	const auto it = std::ranges::find_if(subZipFiles, [](const QString& item) {
		return item.endsWith(".fbd", Qt::CaseInsensitive);
	});
	if (it == subZipFiles.end())
		return {};

	return ParseFb2(folder, *subZip, *it, zipDateTime, isDeleted, fileInfo.completeBaseName(), fileInfo.suffix());
}

Book* ParseBook(const QString& fileName, InpDataProvider& inpDataProvider, const QString& folder, const Zip& zip, const QDateTime& zipDateTime, const bool isDeleted)
{
	const auto hash = GetFileHash(zip, fileName).hash;
	PLOGV << "parse " << fileName << ", hash: " << hash;

	const auto parser = [&] {
		const auto it = std::ranges::find_if(FILE_PARSERS, [&](const auto& item) {
			return fileName.endsWith(item.first, Qt::CaseInsensitive);
		});
		return it != std::end(FILE_PARSERS) ? it->second.first : &ParseFbd;
	}();

	if (auto parsedBook = parser(folder, zip, fileName, zipDateTime, isDeleted, {}, {}); !parsedBook.title.isEmpty())
		return inpDataProvider.AddBook(std::make_unique<Book>(std::move(parsedBook)));

	return nullptr;
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

	size_t totalCounter = 0;

	for (const auto& [zipFileInfo, sourceLib] : archives | std::views::reverse | std::views::transform([](const auto& item) {
													return std::make_pair(QFileInfo(item.filePath), item.sourceLib);
												}))
	{
		QByteArray file;
		Zip        zip(zipFileInfo.filePath());
		const auto bookFiles = zip.GetFileNameList();
		const auto folder    = zipFileInfo.fileName();

		PLOGV << folder << ", files count: " << bookFiles.size();
		size_t counter = 0;

		for (const auto& bookFile : bookFiles)
		{
			auto* book = inpDataProvider.GetBook({ folder, bookFile });
			if (book)
			{
				inpDataProvider.AddBook(book);
			}
			else
			{
				if (bookFile.endsWith(".zip") && ((book = inpDataProvider.GetBook({ folder, bookFile.first(bookFile.length() - 4) }))))
				{
					book->file = bookFile.first(bookFile.length() - 4);
					book->ext  = "zip";
					inpDataProvider.AddBook(book);
				}
				else
				{
					book = GetBookCustom(bookFile, inpDataProvider, zip, unIndexed);
					if (!book)
					{
						book = ParseBook(bookFile, inpDataProvider, folder, zip, zipFileInfo.birthTime(), settings.isDeleted);
						if (!book)
						{
							PLOGW << zipFileInfo.filePath() << "/" << bookFile << " not found";
							continue;
						}
					}
				}
			}

			const auto bookFileName = book->GetFileName();
			if (bookFileName.contains('\n') || bookFileName.contains('\r'))
			{
				PLOGW << bookFile << " contains bad symbols: " << bookFileName;
				continue;
			}

			book->sourceLib = sourceLib;
			book->folder    = folder;

			const auto dashIt = [](QString& title) {
				std::ranges::transform(title, title.begin(), [](const QChar ch) {
					return ch >= QChar { 0x2010 } && ch <= QChar { 0x2015 } ? QChar { '-' } : ch == QChar { 0x0451 } ? QChar { 0x0435 } : ch;
				});
				title.replace(" - ", DASH);
				title.replace(" -- ", DASH);
			};

			dashIt(book->author);

			auto& series = book->series; //-V826
			std::ranges::for_each(series, dashIt, &Series::title);

			std::ranges::sort(series, std::greater {}, seriesUniquePredicate);
			if (const auto [begin, end] = std::ranges::unique(series, {}, seriesUniquePredicate); begin != end)
				series.erase(begin, end);
			if (series.size() > 1 && series.back().title.isEmpty())
				series.pop_back();
			std::ranges::sort(series, {}, seriesOrdNumPredicate);

			if (settings.maxSeriesPerBook > 0)
			{
				book->series.erase(std::next(book->series.begin(), std::min(settings.maxSeriesPerBook, std::ssize(book->series))), book->series.end());
			}
			else
			{
				book->series.clear();
				book->series.emplace_back();
			}

			book->insNo = zip.GetFileIndex(bookFile) + 1;

			file << *book;
			++counter;

			maxTime = std::max(maxTime, zip.GetFileTime(bookFile));
		}

		if (static_cast<qsizetype>(counter) == bookFiles.size())
			PLOGV << folder << ", books added: " << counter;
		else
			PLOGW << folder << ", not all books added: " << counter << " out of " << bookFiles.size();

		if (!file.isEmpty())
			zipFileController->AddFile(zipFileInfo.completeBaseName() + ".inp", file, QDateTime::currentDateTime());

		totalCounter += counter;
	}

	PLOGV << "books added total: " << totalCounter;

	const auto collectionInfo = [&]() -> QString {
		if (!QFile::exists(settings.collectionInfoTemplateFile))
			return {};

		if (QFile file(settings.collectionInfoTemplateFile); file.open(QIODevice::ReadOnly))
			return QString::fromUtf8(file.readAll()).arg(maxTime.toString("yyyy-MM-dd"), maxTime.toString("yyyyMMdd"));

		return {};
	}();

	zipFileController->AddFile(Inpx::STRUCTURE_INFO, Inpx::INP_FIELDS_DESCRIPTION, QDateTime::currentDateTime());
	zipFileController->AddFile(Inpx::VERSION_INFO, maxTime.toString("yyyyMMdd").toUtf8(), QDateTime::currentDateTime());
	if (!collectionInfo.isEmpty())
		zipFileController->AddFile(Inpx::COLLECTION_INFO, collectionInfo.toUtf8(), QDateTime::currentDateTime());

	{
		Zip inpx(Platform::PathToString(inpxFileName), ZipDetails::Format::Zip);
		inpx.Write(*zipFileController);
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
	Util::ThreadPool threadPool;

	const auto reviewsFolder = outputFolder / Inpx::REVIEWS_FOLDER;
	QDir(reviewsFolder).mkpath(".");

	std::mutex                                   archivesGuard;
	std::vector<std::tuple<QString, QByteArray>> archives;

	threadPool.enqueue([&](auto) {
		auto             archiveName = Platform::PathToString(reviewsFolder / Inpx::REVIEWS_ADDITIONAL_ARCHIVE_NAME);
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
			zipFiles->AddFile(Inpx::REVIEWS_ADDITIONAL_BOOKS_FILE_NAME, additional);
			zip.Write(*zipFiles);
		}

		std::lock_guard lock(archivesGuard);
		archives.emplace_back(std::move(archiveName), std::move(zipBytes));
	});

	using Data = std::vector<std::tuple<QString, QString, QString, QString, QString>>;

	const auto write = [&](const int year, const int month, Data data) {
		auto archiveName = Platform::PathToString(reviewsFolder / std::format("{:04}{:02}", year, month)) + ".7z";

		threadPool.enqueue([&archivesGuard, &archives, archiveName = std::move(archiveName), data = std::move(data)](auto) mutable {
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
				zip.Write(*zipFiles);
			}

			std::lock_guard lock(archivesGuard);
			archives.emplace_back(std::move(archiveName), std::move(zipBytes));
		});
	};

	PLOGI << "get review months";
	std::set<std::pair<int, int>> months;
	inpDataProvider.Enumerate([&](const QString&, const IDump& dump) {
		std::ranges::move(dump.GetReviewMonths(), std::inserter(months, months.end()));
		return false;
	});

	const auto inpxedBooks = inpDataProvider.Books() | std::ranges::to<std::unordered_set<const Book*>>();

	Util::Progress progress(months.size(), "select reviews");
	for (const auto& [year, month] : months)
	{
		Data data;

		inpDataProvider.Enumerate([&](const QString& sourceLib, const IDump& dump) {
			dump.Review(year, month, [&](const QString& libId, QString name, QString time, QString text) {
				auto* book = inpDataProvider.GetBook(sourceLib, libId);
				while (book)
				{
					if (const auto rIt = replacement.find({ book->folder, book->GetFileName() }); rIt != replacement.end())
					{
						if (const auto& [replacementFolder, replacementFile] = rIt->second; !((book = inpDataProvider.GetBook({ replacementFolder, replacementFile }))))
						{
							auto replacementLibId = replacementFile;
							if (const auto pos = replacementLibId.lastIndexOf('.'); pos > 0)
								replacementLibId = replacementLibId.first(pos);
							book = inpDataProvider.GetBook(sourceLib, replacementLibId);
						}
						continue;
					}

					break;
				}

				if (book && inpxedBooks.contains(book))
					data.emplace_back(book->folder, book->GetFileName(), std::move(name), std::move(time), std::move(text));
			});

			return false;
		});

		if (!data.empty())
			write(year, month, std::move(data));

		progress.Increment(1, std::format("{:04}-{:02}", year, month));
	}

	threadPool.wait();

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

		zipFiles->AddFile(value.first + ".txt", data);
	});

	PLOGI << "archive contents";
	const auto contentsFile = outputFolder / Inpx::CONTENTS;
	remove(contentsFile);

	Zip zip(Platform::PathToString(contentsFile), Zip::Format::SevenZip);
	zip.SetProperty(ZipDetails::PropertyId::SolidArchive, false);
	zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
	zip.Write(*zipFiles);
}

void ProcessCompilations(const std::filesystem::path& outputFolder, const Archives& archives, const InpDataProvider& inpDataProvider, IAnnotationCollector& annotationCollector)
{
	PLOGI << "collect compilation info";

	const auto sectionToBook = inpDataProvider.Books() | std::views::transform([](Book* book) {
								   return std::make_pair(book->id, book);
							   })
	                         | std::ranges::to<std::unordered_multimap<QString, Book*>>();
	if (sectionToBook.empty())
		return;

	std::vector<CompilationHandler::ParseStorage> storage;
	storage.reserve(archives.size());

	{
		Util::Progress   progress(archives.size(), "parsing");
		Util::ThreadPool threadPool({ .maxQueueSize = std::thread::hardware_concurrency() });

		for (const auto& archive : archives | std::views::filter([](const auto& item) {
									   return !item.hashPath.isEmpty();
								   }) | std::views::reverse)
		{
			auto& storageItem = storage.emplace_back(archive);

			{
				QFile file(archive.hashPath);
				if (!file.open(QIODevice::ReadOnly))
					throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));
				storageItem.bytes = file.readAll();
			}

			threadPool.enqueue([&](auto) {
				[[maybe_unused]] const CompilationHandler compilationHandler(inpDataProvider, sectionToBook, storageItem);
				progress.Increment(1, QFileInfo(archive.hashPath).fileName().toStdString());
			});
		}

		threadPool.wait();
	}

	QJsonArray jsonArray;

	size_t totalData = 0, totalCompilations = 0;
	{
		Util::Progress progress(storage.size(), "store parsed data");
		for (auto& storageItem : storage)
		{
			annotationCollector.StartFolder();
			for (const auto& [folder, file, annotation] : storageItem.data)
				annotationCollector.Add(folder, file, annotation);

			for (auto&& obj : storageItem.compilations)
				jsonArray.append(std::move(obj));

			progress.Increment(1, QString("%1 (%2, %3)").arg(QFileInfo(storageItem.archive.get().hashPath).fileName()).arg(storageItem.data.size()).arg(storageItem.compilations.size()).toStdString());
			totalData         += storageItem.data.size();
			totalCompilations += storageItem.compilations.size();
		}
	}

	PLOGI << "total annotations: " << totalData << ", compilations: " << totalCompilations;

	const auto data = QJsonDocument(jsonArray).toJson();

	if (data.isEmpty())
	{
		PLOGI << "no compilation info";
		return;
	}

	PLOGI << "archive compilation info";
	const auto contentsFile = outputFolder / Inpx::COMPILATIONS;
	remove(contentsFile);

	auto zipFiles = Zip::CreateZipFileController();
	zipFiles->AddFile(Inpx::COMPILATIONS_JSON, data);

	Zip zip(Platform::PathToString(contentsFile), Zip::Format::SevenZip);
	zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
	zip.Write(*zipFiles);
}

void CreateReview(const std::filesystem::path& outputFolder, const InpDataProvider& inpDataProvider, const Replacement& replacement)
{
	PLOGI << "write reviews";

	for (const auto& [fileName, data] : CreateReviewData(outputFolder, inpDataProvider, replacement))
		Write(fileName, data);
}

Replacement ReadHash(InpDataProvider& inpDataProvider, Archives& archives)
{
	Replacement                               replacement;
	std::vector<FileHashParser::ParseStorage> storage;
	storage.reserve(archives.size());

	{
		Util::Progress   progress(archives.size(), "parsing");
		Util::ThreadPool threadPool({ .maxQueueSize = std::thread::hardware_concurrency() });

		for (auto& archive : archives | std::views::filter([](const auto& item) {
								 return !item.hashPath.isEmpty();
							 }))
		{
			auto& storageItem = storage.emplace_back(archive);

			{
				QFile file(archive.hashPath);
				if (!file.open(QIODevice::ReadOnly))
					throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));
				storageItem.bytes = file.readAll();
			}

			threadPool.enqueue([&](auto) {
				[[maybe_unused]] const FileHashParser parser(storageItem);
				progress.Increment(1, QFileInfo(archive.hashPath).fileName().toStdString());
			});
		}

		threadPool.wait();
	}
	{
		Util::Progress progress(storage.size(), "store parsed data");

		for (auto&& storageItem : storage)
		{
			std::ranges::move(storageItem.replacement, std::inserter(replacement, replacement.end()));
			inpDataProvider.SetSourceLib(storageItem.archive.get().sourceLib);
			for (auto&& [uid, id] : storageItem.data)
				inpDataProvider.SetFile(uid, std::move(id));
			progress.Increment(1, QString("%1 (%2)").arg(QFileInfo(storageItem.archive.get().hashPath).fileName()).arg(storageItem.data.size()).toStdString());
		}
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

	const auto availableLibraries = Dump::GetAvailableLibraries();

	Settings settings {};

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 creates inpx for flibusta collections").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives with hashes (required)");
	parser.addOptions(
		{
			{ { "o", OUTPUT }, "Output folder (required)", FOLDER },
			{ DUMP, "Dump database wildcards", "Semicolon separated wildcard list" },
			{ { "i", COLLECTION_INFO_TEMPLATE }, "Collection info template", PATH },
			{ LIBRARY, "Source library", QString("(%1) [%2]").arg(availableLibraries.join(" | "), availableLibraries.front()) },
			{ MAX_SERIES, "Maximum series per book", QString("[%1]").arg(settings.maxSeriesPerBook) },
			{ DELETED, "Mark books missing from the dump as deleted" },
			{ SKIP_CONTENTS, "Skip contents" },
			{ SKIP_REVIEWS, "Skip size readers reviews" },
			{ SKIP_COMPILATIONS, "Skip compilations info" },
			{ SKIP_ANNOTATIONS, "Skip annotations" },
			{ INPX_ONLY, "Skip all except inpx" },
    }
	);
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	Log::LoggingInitializer                          logging(parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath);
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	try
	{
		PLOGI << QString("%1 started").arg(APP_ID);

		if (parser.positionalArguments().isEmpty() || !parser.isSet(OUTPUT))
			parser.showHelp(1);

		settings.outputFolder               = parser.value(OUTPUT).toStdWString();
		settings.collectionInfoTemplateFile = parser.value(COLLECTION_INFO_TEMPLATE).toStdWString();

		settings.sourceLib = parser.value(LIBRARY);
		if (settings.sourceLib.isEmpty())
			settings.sourceLib = availableLibraries.front();
		if (!availableLibraries.contains(settings.sourceLib, Qt::CaseInsensitive))
			throw std::invalid_argument(std::format("{} must be {}", LIBRARY, availableLibraries.join(" | ")));

		if (parser.isSet(MAX_SERIES))
			settings.maxSeriesPerBook = parser.value(MAX_SERIES).toLongLong();

		settings.isDeleted = parser.isSet(DELETED);

		auto archives = GetArchives(parser.positionalArguments());
		Total(archives);

		const auto inpDataProvider = std::make_shared<InpDataProvider>(parser.value(DUMP));
		const auto replacement     = ReadHash(*inpDataProvider, archives);

		if (archives.front().hashPath.isNull())
		{
			inpDataProvider->SetSourceLib(settings.sourceLib);
			for (auto& archive : archives)
				archive.sourceLib = settings.sourceLib;
		}

		MergeBookData(*inpDataProvider, replacement);
		CreateInpx(settings, archives, *inpDataProvider);

		if (parser.isSet(INPX_ONLY))
			return 0;

		if (!parser.isSet(SKIP_CONTENTS))
			CreateBookList(settings.outputFolder, *inpDataProvider);

		if (!parser.isSet(SKIP_REVIEWS))
			CreateReview(settings.outputFolder, *inpDataProvider, replacement);

		if (!parser.isSet(SKIP_COMPILATIONS))
		{
			auto annotationCollector = parser.isSet(SKIP_ANNOTATIONS) ? AnnotationCollectorStub::Create() : AnnotationCollector::Create(settings.outputFolder);
			ProcessCompilations(settings.outputFolder, archives, *inpDataProvider, *annotationCollector);
		}

		return 0;
	}
	catch (const std::exception& ex)
	{
		PLOGE << ex.what();
	}
	catch (...)
	{
		PLOGE << "Unknown error";
	}

	return 1;
}
