#include <QCryptographicHash>

#include <filesystem>
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

#include "db/Factory.h"
#include "db/IDatabase.h"
#include "lib/book.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/Fb2InpxParser.h"
#include "util/LogConsoleFormatter.h"
#include "util/executor/ThreadPool.h"
#include "util/language.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"

#include "Constant.h"
#include "log.h"
#include "settings.h"
#include "util.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa::FliParser;
using namespace HomeCompa;

namespace
{

constexpr auto SQL                      = "sql";
constexpr auto COLLECTION_INFO_TEMPLATE = "collection-info-template";
constexpr auto ARCHIVES                 = "archives";
constexpr auto OUTPUT                   = "output";
constexpr auto FOLDER                   = "folder";
constexpr auto PATH                     = "path";
constexpr auto HASH                     = "hash";
constexpr auto LIBRARY                  = "library";

using InpData = std::unordered_map<QString, std::unique_ptr<Book>, Util::CaseInsensitiveHash<QString>>;

constexpr auto APP_ID = "fliparser";

class HashParser final : public Util::SaxParser
{
	static constexpr auto BOOK       = "books/book";
	static constexpr auto DUPLICATES = "books/book/duplicates";
	static constexpr auto SECTION    = "section";

	using Callback = std::function<void(QString file, QString duplicate, QString id, Section::Ptr)>;

public:
	HashParser(QIODevice& input, Callback callback)
		: SaxParser(input, 512)
		, m_callback { std::move(callback) }
	{
		Parse();
	}

private: // Util::SaxParser
	bool OnStartElement(const QString& name, const QString& path, const Util::XmlAttributes& attributes) override
	{
		if (path == BOOK)
		{
			m_id   = attributes.GetAttribute("id");
			m_file = attributes.GetAttribute("file");
		}
		else if (path == DUPLICATES)
		{
			m_duplicates = attributes.GetAttribute("file");
		}
		else if (name == SECTION)
		{
			m_currentSection =
				m_currentSection->children.try_emplace(attributes.GetAttribute("id"), std::make_unique<Section>(m_currentSection, attributes.GetAttribute("count").toULongLong())).first->second.get();
		}

		return true;
	}

	bool OnEndElement(const QString& name, const QString& path) override
	{
		if (path == BOOK)
		{
			const auto enumerate = [](Section& parent, const auto& r) -> void {
				const auto ids = parent.children | std::views::filter([](const auto& item) {
									 return item.second->count < 100;
								 })
				               | std::views::keys | std::ranges::to<std::vector<QString>>();
				for (const auto& id : ids)
					parent.children.erase(id);
				for (const auto& child : parent.children | std::views::values)
					r(*child, r);
			};
			enumerate(*m_section, enumerate);

			m_callback(std::move(m_file), std::move(m_duplicates), std::move(m_id), std::move(m_section));
			m_id             = {};
			m_file           = {};
			m_duplicates     = {};
			m_section        = std::make_unique<Section>();
			m_currentSection = m_section.get();
		}
		else if (name == SECTION)
		{
			assert(m_currentSection->parent);
			m_currentSection = m_currentSection->parent;
		}

		return true;
	}

private:
	Callback     m_callback;
	QString      m_id, m_file, m_duplicates;
	Section::Ptr m_section { std::make_unique<Section>() };
	Section*     m_currentSection { m_section.get() };
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

Book* GetBookCustom(Settings& settings, const QString& fileName, InpData& inpData, const Zip& zip, const QJsonObject& unIndexed)
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

	auto& book = inpData
	                 .try_emplace(
						 fileName,
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
					 )
	                 .first->second;

	settings.fileToHash[fileName]     = fileName;
	settings.libIdToHash[book->libId] = fileName;
	return settings.hashToBook.try_emplace(fileName, book.get()).first->second;
}

Book* ParseBook(Settings& settings, const QString& fileName, InpData& inpData, const QString& folder, const Zip& zip, const QDateTime& zipDateTime)
{
	if (!fileName.endsWith(".fb2", Qt::CaseInsensitive))
		return nullptr;

	const auto hash = GetFileHash(zip, fileName).hash;
	PLOGV << "parse " << fileName << ", hash: " << hash;

	auto parsedBook = Book::FromString(Util::Fb2InpxParser::Parse(folder, zip, fileName, zipDateTime, true));
	if (parsedBook.title.isEmpty())
		return nullptr;

	auto* book = inpData.try_emplace(fileName, std::make_unique<Book>(std::move(parsedBook))).first->second.get();

	settings.fileToHash[fileName]     = fileName;
	settings.libIdToHash[book->libId] = fileName;
	return settings.hashToBook.try_emplace(fileName, book).first->second;
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

void CreateInpx(Settings& settings, InpData& inpData, const QString& sourceLib)
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

	const auto inpxFileName = settings.outputFolder / (settings.archivesFolder.filename().wstring() + L".inpx");
	if (exists(inpxFileName))
		remove(inpxFileName);

	auto      zipFileController = Zip::CreateZipFileController();
	QDateTime maxTime;

	std::ranges::for_each(
		std::filesystem::directory_iterator { settings.archivesFolder } | std::views::filter([](const auto& entry) {
			return !entry.is_directory() && Zip::IsArchive(QString::fromStdWString(entry.path()));
		}),
		[&](const auto& entry) {
			const auto& path = entry.path();
			PLOGV << path.string();

			QByteArray      file;
			const QFileInfo zipFileInfo(QString::fromStdWString(path));
			Zip             zip(zipFileInfo.filePath());
			for (const auto& bookFile : zip.GetFileNameList())
			{
				auto* book = GetBook(settings, bookFile, inpData);
				if (!book)
				{
					book = GetBookCustom(settings, bookFile, inpData, zip, unIndexed);
					if (!book)
					{
						book = ParseBook(settings, bookFile, inpData, QString::fromStdWString(path.filename()), zip, zipFileInfo.birthTime());
						if (!book)
						{
							PLOGW << zipFileInfo.filePath() << "/" << bookFile << " not found";
							continue;
						}
					}
				}

				if (const auto it = inpData.find(bookFile); it != inpData.end())
					book->libId = it->second->libId;

				book->sourceLib = sourceLib;

				QFileInfo fileInfo(bookFile);
				book->file   = fileInfo.completeBaseName();
				book->ext    = fileInfo.suffix();
				book->folder = QString::fromStdWString(path.filename());

				file << *book;

				maxTime = std::max(maxTime, zip.GetFileTime(bookFile));
				settings.fileToFolder[bookFile].emplace_back(zipFileInfo.fileName());
			}

			if (!file.isEmpty())
				zipFileController->AddFile(QString::fromStdWString(path.filename().replace_extension("inp")), std::move(file), QDateTime::currentDateTime());
		}
	);

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

QByteArray CreateReviewAdditional(const Settings& settings)
{
	QJsonArray jsonArray;
	for (const auto& book : settings.hashToBook | std::views::values | std::views::filter([&](const Book* item) {
								return item->rate > std::numeric_limits<double>::epsilon() && settings.fileToFolder.contains(item->file + "." + item->ext);
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

std::vector<std::tuple<QString, QByteArray>> CreateReviewData(const IDatabase& db, const Settings& settings)
{
	auto threadPool = std::make_unique<Util::ThreadPool>();

	const auto reviewsFolder = settings.outputFolder / Inpx::REVIEWS_FOLDER;
	QDir(reviewsFolder).mkpath(".");
	int currentMonth { -1 };

	std::map<QString, std::vector<std::tuple<QString, QString, QString>>> data;

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

		auto additional = CreateReviewAdditional(settings);
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

	const auto write = [&](const int month) {
		ScopedCall monthGuard([&] {
			currentMonth = month;
			data.clear();
		});
		if (currentMonth < 0)
			return;

		auto archiveName = QString::fromStdWString(reviewsFolder / std::to_string(currentMonth)) + ".7z";

		auto dataCopy = std::move(data);
		data          = {};

		threadPool->enqueue([&archivesGuard, &archives, archiveName = std::move(archiveName), data = std::move(dataCopy)]() mutable {
			size_t           counter = 0;
			const ScopedCall logGuard(
				[&] {
					PLOGI << archiveName << " started, books: " << data.size();
				},
				[archiveName, &counter] {
					PLOGI << archiveName << " finished, records: " << counter;
				}
			);

			auto zipFiles = Zip::CreateZipFileController();
			std::ranges::for_each(data, [&](auto& value) {
				QJsonArray array;
				for (auto& [name, time, text] : value.second)
				{
					text.prepend(' ');
					text.append(' ');
					array.append(QJsonObject {
						{ Inpx::NAME,              name.simplified() },
						{ Inpx::TIME,                           time },
						{ Inpx::TEXT, ReplaceTags(text).simplified() },
					});
					++counter;
				}
				zipFiles->AddFile(value.first, QJsonDocument(array).toJson());
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

	db.Review([&](const QString& bookId, QString name, QString time, QString text) {
		const auto* book = settings.FromLibId(bookId);
		if (!book || !settings.fileToFolder.contains(book->file + "." + book->ext))
			return;

		if (const auto month = QStringView(time.begin(), std::next(time.begin(), 4)).toInt() * 100 + QStringView(std::next(time.begin(), 5), std::next(time.begin(), 7)).toInt(); month != currentMonth)
			write(month);

		data[book->GetUid()].emplace_back(std::move(name), std::move(time), std::move(text));
	});

	write(currentMonth);

	threadPool.reset();

	return archives;
}

void CreateBookList(const Settings& settings)
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
	for (const auto& book : settings.hashToBook | std::views::values)
	{
		assert(book);
		if (settings.fileToFolder.contains(book->file + '.' + book->ext))
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
		{
			const auto fileName = book->file + "." + book->ext;
			const auto it       = settings.fileToFolder.find(fileName);
			assert(it != settings.fileToFolder.end());

			data.append(book->author.toUtf8()).append("\t").append(book->title.toUtf8()).append("\t");
			if (!book->series.empty())
			{
				if (const auto& series = book->series.front(); !series.title.isEmpty())
				{
					data.append("[").append(series.title.toUtf8());
					if (!series.serNo.isEmpty())
						data.append(" #").append(series.serNo.toUtf8());
					data.append("]");
				}
			}
			data.append("\t").append(it->second.join(',').toUtf8()).append("\t").append(fileName.toUtf8()).append("\x0d\x0a");
		}

		zipFiles->AddFile(value.first + ".txt", std::move(data));
	});

	PLOGI << "archive contents";
	const auto contentsFile = settings.outputFolder / Inpx::CONTENTS;
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
					{       "part",                      std::ssize(idFound) },
					{ Inpx::FOLDER,                 folderIt->second.front() },
					{   Inpx::FILE, it->second->file + '.' + it->second->ext },
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
				{  Inpx::FOLDER,     folderIt->second.front() },
				{    Inpx::FILE, book->file + '.' + book->ext },
				{ "compilation",             std::move(found) },
				{     "covered",           idNotFound.empty() },
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

void CreateReview(const Settings& settings, const IDatabase& db)
{
	PLOGI << "write reviews";

	for (const auto& [fileName, data] : CreateReviewData(db, settings))
		Write(fileName, data);
}

void ReadHash(Settings& settings, InpData& inpData)
{
	for (const auto& [file, book] : inpData)
	{
		settings.hashToBook.try_emplace(file, book.get());
		settings.fileToHash.try_emplace(file, file);
		settings.libIdToHash.try_emplace(book->libId, file);
	}

	if (settings.hashFolder.empty())
		return;

	PLOGI << "reading hash files";

	size_t n = 0;

	std::unordered_map<QString, std::set<QString>>                files;
	std::unordered_map<QString, std::pair<QString, Section::Ptr>> sections;

	for (const auto& entry : std::filesystem::directory_iterator(settings.hashFolder) | std::views::filter([](const std::filesystem::path& item) {
								 return !is_directory(item) && item.extension() == ".xml";
							 }))
	{
		const auto entryPath = QString::fromStdWString(entry.path());

		QFile stream(entryPath);
		if (!stream.open(QIODevice::ReadOnly))
		{
			PLOGW << "cannot read from " << entryPath;
			continue;
		}

		PLOGI << "reading " << entryPath;

		const HashParser parser(stream, [&](QString file, QString duplicate, QString id, Section::Ptr section) {
			sections.try_emplace(file, std::make_pair(std::move(id), std::move(section)));

			auto it = [&] {
				if (duplicate.isEmpty())
					return files.try_emplace(std::move(file), std::set<QString> {}).first;

				auto result = files.try_emplace(std::move(duplicate), std::set<QString> {}).first;
				result->second.emplace(std::move(file));
				return result;
			}();
			it->second.emplace(it->first);

			++n;
			PLOGV_IF(n % 50000 == 0) << n << " entries read";
		});
	}

	for (size_t id = 0; const auto& siblings : files | std::views::values)
	{
		assert(!siblings.empty());
		auto& [hash, book] = *settings.hashToBook.try_emplace(QString::number(++id), static_cast<Book*>(nullptr)).first;

		for (const auto& file : siblings)
		{
			const auto it = inpData.find(file);
			if (it == inpData.end())
				continue;

			if (const auto sectionIt = sections.find(file); sectionIt != sections.end())
			{
				it->second->id      = std::move(sectionIt->second.first);
				it->second->section = std::move(sectionIt->second.second);
			}

			settings.hashToBook.erase(file);
			settings.fileToHash[file]               = hash;
			settings.libIdToHash[it->second->libId] = hash;

			if (!book)
			{
				book = it->second.get();
				continue;
			}

			const auto rate      = book->rate + it->second->rate;
			const auto rateCount = book->rateCount + it->second->rateCount;

			if ((book->deleted && !it->second->deleted) || (book->deleted == it->second->deleted && book->file.toInt() < it->second->file.toInt()))
			{
				book->id.clear();
				book->section.reset();
				book = it->second.get();
			}

			book->rate      = rate;
			book->rateCount = rateCount;
		}

		if (!book)
			settings.hashToBook.erase(hash);
	}
	assert(std::ranges::all_of(settings.hashToBook | std::views::values, [](const auto* item) {
		return !!item;
	}));
}

InpData CreateInpData(const IDatabase& db)
{
	InpData inpData;

	size_t n = 0;
	db.CreateInpData([&](const DB::IQuery& query) {
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
							 .lang      = QString::fromStdWString(GetLanguage(QString(query.Get<const char*>(11)).toLower().toStdWString())),
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
	parser.addOptions({
		{					  { "s", SQL },     "Folder with sql files (required)",                          FOLDER },
		{				 { "a", ARCHIVES }, "Folder with book archives (required)",                          FOLDER },
		{				   { "o", OUTPUT },             "Output folder (required)",                          FOLDER },
		{ { "i", COLLECTION_INFO_TEMPLATE },             "Collection info template",                            PATH },
		{							  HASH,						  "Hash folder",                          FOLDER },
		{						   LIBRARY,							  "Library", "(Flibusta | RusEc) [Flibusta]" },
	});
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	Log::LoggingInitializer                          logging((parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	settings.sqlFolder                  = parser.value(SQL).toStdWString();
	settings.archivesFolder             = parser.value(ARCHIVES).toStdWString();
	settings.outputFolder               = parser.value(OUTPUT).toStdWString();
	settings.collectionInfoTemplateFile = parser.value(COLLECTION_INFO_TEMPLATE).toStdWString();
	settings.hashFolder                 = parser.value(HASH).toStdWString();
	settings.library                    = parser.value(LIBRARY);

	if (settings.sqlFolder.empty() || settings.archivesFolder.empty() || settings.outputFolder.empty())
		parser.showHelp(1);

	const auto db      = Database::Create(settings);
	auto       inpData = CreateInpData(*db);
	ReadHash(settings, inpData);

	CreateInpx(settings, inpData, db->GetName());
	ProcessCompilations(settings);
	CreateBookList(settings);

	CreateReview(settings, *db);
	db->CreateAdditional(settings);

	return 0;
}
