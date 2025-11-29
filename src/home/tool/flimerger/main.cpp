#include <ranges>
#include <set>
#include <unordered_set>

#include <QCommandLineParser>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/ScopedCall.h"

#include "lib/UniqueFile.h"
#include "lib/book.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/BookUtil.h"
#include "util/LogConsoleFormatter.h"
#include "util/files.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "log.h"

#include "config/version.h"

using namespace HomeCompa;

template <>
struct std::formatter<QString> : std::formatter<std::string>
{
	auto format(const QString& obj, std::format_context& ctx) const
	{
		return std::formatter<std::string>::format(obj.toStdString(), ctx);
	}
};

namespace
{

constexpr auto APP_ID = "flimerger";

constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";
constexpr auto COLLECTION_INFO_TEMPLATE     = "collection-info-template";
constexpr auto FOLDER                       = "folder";
constexpr auto PATH                         = "path";

struct Archive
{
	QString filePath;
	QString hashPath;
};

using Archives    = std::vector<Archive>;
using BookItem    = std::pair<QString, QString>;
using Replacement = std::unordered_map<BookItem, BookItem, Util::PairHash<QString, QString>>;
using UniquePaths = std::unordered_set<QString>;
using InputDir    = std::pair<QDir, UniquePaths>;
using InputDirs   = std::vector<InputDir>;

using CompilationParts = std::vector<std::pair<BookItem, int>>;
using Compilation      = std::pair<CompilationParts, bool>;
using Compilations     = std::unordered_map<BookItem, Compilation, Util::PairHash<QString, QString>>;

struct Settings
{
	QDir        outputDir;
	QStringList arguments;
	QString     collectionInfoTemplateFile;
	QString     logFileName;
};

class HashCopier final : public Util::SaxParser
{
public:
	HashCopier(QIODevice& input, QIODevice& output, QString folder, const Replacement& replacement)
		: SaxParser(input, 512)
		, m_replacement { replacement }
		, m_writer { output }
		, m_folder { std::move(folder) }
	{
		Parse();
	}

private:
	bool OnStartElement(const QString& name, const QString& path, const Util::XmlAttributes& attributes) override
	{
		if (path == "books/book")
			m_file = attributes.GetAttribute("file");

		m_writer.WriteStartElement(name, attributes);
		return true;
	}

	bool OnEndElement(const QString& /*name*/, const QString& path) override
	{
		if (path == "books/book")
		{
			if (const auto it = m_replacement.find(std::make_pair(m_folder, m_file)); it != m_replacement.end())
			{
				auto duplicates = m_writer.Guard("duplicates");
				duplicates->WriteAttribute(Inpx::FOLDER, QFileInfo(it->second.first).completeBaseName());
				duplicates->WriteAttribute(Inpx::FILE, it->second.second);
			}
			m_file.clear();
		}

		m_writer.WriteEndElement();
		return true;
	}

	bool OnCharacters(const QString& /*path*/, const QString& value) override
	{
		m_writer.WriteCharacters(value);
		return true;
	}

private:
	const Replacement& m_replacement;
	Util::XmlWriter    m_writer;
	const QString      m_folder;
	QString            m_file;
};

class DuplicateObserver final : public UniqueFileStorage::IDuplicateObserver
{
public:
	explicit DuplicateObserver(Replacement& replacement)
		: m_replacement { replacement }
	{
	}

private: // UniqueFileStorage::IDuplicateObserver
	void OnDuplicateFound(const UniqueFile::Uid& file, const UniqueFile::Uid& duplicate) override
	{
		m_replacement.try_emplace(std::make_pair(duplicate.folder, duplicate.file), std::make_pair(file.folder, file.file));
	}

private:
	Replacement& m_replacement;
};

void ProcessArchive(const QDir& outputDir, const Archive& archive, const Replacement& replacement)
{
	const QFileInfo fileInfo(archive.filePath);

	const auto dstFilePath = outputDir.filePath(fileInfo.fileName());
	QFile::remove(dstFilePath);
	if (!QFile::copy(fileInfo.filePath(), dstFilePath))
		throw std::invalid_argument(std::format("Cannot copy {} to {}", fileInfo.filePath(), dstFilePath));

	for (const char* imageFolder : { Global::COVERS, Global::IMAGES })
	{
		auto imageDir = fileInfo.dir();
		if (!imageDir.cd(imageFolder))
			continue;

		const auto imageArchiveFileSrc = imageDir.absoluteFilePath(fileInfo.completeBaseName()) + ".zip";
		if (!QFile::exists(imageArchiveFileSrc))
			continue;

		QDir dstDir(outputDir.filePath(imageFolder));
		if (!dstDir.exists())
			dstDir.mkpath(".");

		const auto imageArchiveFileDst = dstDir.filePath(fileInfo.completeBaseName() + ".zip");
		QFile::remove(imageArchiveFileDst);
		if (!QFile::copy(imageArchiveFileSrc, imageArchiveFileDst))
			throw std::invalid_argument(std::format("Cannot copy {} to {}", imageArchiveFileSrc, imageArchiveFileDst));
	}

	auto toRemove = Zip(dstFilePath).GetFileNameList() | std::views::filter([&](const QString& fileName) {
						const auto key    = std::make_pair(fileInfo.fileName(), fileName);
						const auto result = replacement.contains(key);
						return result;
					})
	              | std::views::transform([&, n = 0](const QString& fileName) mutable {
						return Util::Remove::Book { ++n, fileInfo.fileName(), fileName };
					})
	              | std::ranges::to<Util::Remove::Books>();

	if (toRemove.empty())
		return;

	auto allFiles = CollectBookFiles(toRemove, [] {
		return nullptr;
	});
	auto images   = Util::Remove::CollectImageFiles(allFiles, outputDir.absolutePath(), [] {
        return nullptr;
    });
	std::ranges::move(std::move(images), std::inserter(allFiles, allFiles.end()));
	Util::Remove::RemoveFiles(allFiles, outputDir.absolutePath());
}

void MergeArchives(const QDir& outputDir, const Archives& archives, const Replacement& replacement)
{
	for (const auto& archive : archives)
		ProcessArchive(outputDir, archive, replacement);
}

void ProcessHash(const QDir& outputDir, const Archive& archive, const Replacement& replacement)
{
	PLOGI << "parsing " << archive.hashPath;
	outputDir.mkpath("hash");
	QFileInfo fileInfo(archive.hashPath);

	QFile input(archive.hashPath);
	if (!input.open(QIODevice::ReadOnly))
		throw std::ios_base::failure(std::format("Cannot read from {}", archive.hashPath));

	const auto outputFilePath = outputDir.filePath(QString("hash/%1").arg(fileInfo.fileName()));

	QFile output(outputFilePath);
	if (!output.open(QIODevice::WriteOnly))
		throw std::ios_base::failure(std::format("Cannot write to", outputFilePath));

	HashCopier parser(input, output, QFileInfo(archive.filePath).fileName(), replacement);
}

void MergeHash(const QDir& outputDir, const Archives& archives, const Replacement& replacement)
{
	for (const auto& archive : archives)
		ProcessHash(outputDir, archive, replacement);
}

void GetReplacement(const Archive& archive, UniqueFileStorage& uniqueFileStorage)
{
	QFile file(archive.hashPath);
	if (!file.open(QIODevice::ReadOnly))
		throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));

	const QFileInfo fileInfo(archive.filePath);

	std::unordered_map<QString, QString> titles;
	for (const auto& inpx : fileInfo.dir().entryList({ "*.inpx" }, QDir::Files))
	{
		Zip        zip(fileInfo.dir().absoluteFilePath(inpx));
		const auto zipFile = zip.Read(fileInfo.completeBaseName() + ".inp");
		auto&      stream  = zipFile->GetStream();
		for (auto byteArray = stream.readLine(); !byteArray.isEmpty(); byteArray = stream.readLine())
		{
			auto book = Book::FromString(QString::fromUtf8(byteArray));
			titles.try_emplace(book.GetFileName(), SimplifyTitle(PrepareTitle(book.title)));
		}
	}

	const auto bookFiles = Zip(archive.filePath).GetFileNameList() | std::ranges::to<std::unordered_set<QString>>();

	HashParser::Parse(
		file,
		[&](
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
				QString cover,
			QStringList images
		) mutable {
			decltype(UniqueFile::images) imageItems;
			std::ranges::transform(std::move(images) | std::views::as_rvalue, std::inserter(imageItems, imageItems.end()), [](QString&& hash) {
				return ImageItem { .hash = std::move(hash) };
			});

			if (!bookFiles.contains(file))
				return;

			const auto it = titles.find(file);
			if (it != titles.end() && !it->second.isEmpty())
				title = std::move(it->second);

			auto split    = title.split(' ', Qt::SkipEmptyParts);
			auto hashText = id;

			uniqueFileStorage.Add(
				std::move(id),
				UniqueFile {
					.uid      = { .folder = fileInfo.fileName(), .file = file },
					.title    = { std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()) },
					.hashText = std::move(hashText),
					.cover    = { .hash = std::move(cover) },
					.images   = std::move(imageItems)
            }
			);
		}
	);
}

void GetReplacement(const Archives& archives, UniqueFileStorage& uniqueFileStorage)
{
	for (const auto& archive : archives)
		GetReplacement(archive, uniqueFileStorage);
}

Archives GetArchives(const Settings& settings)
{
	std::multimap<int, Archive> sorted;
	std::unordered_set<QString> uniqueFiles;
	const QRegularExpression    rx("^.*?fb2.*?([0-9]+).*?$");

	for (const auto& argument : settings.arguments)
	{
		auto splitted = argument.split(';');
		if (splitted.size() != 2)
			throw std::invalid_argument(std::format("{} must be archives_wildcard;hash_folder", argument));

		const auto wildCard = std::move(splitted.front());
		const QDir hashFolder(splitted.back());
		if (!hashFolder.exists())
			throw std::invalid_argument(std::format("hash folder {} not found", splitted.back()));

		std::ranges::transform(
			Util::ResolveWildcard(wildCard) | std::views::transform([&](const QString& item) {
				return QFileInfo(item);
			}) | std::views::filter([&](const QFileInfo& item) {
				auto       fileName = item.fileName().toLower();
				const auto result   = !uniqueFiles.contains(fileName);
				if (result)
					uniqueFiles.emplace(fileName);
				return result;
			}) | std::views::transform([&](const QFileInfo& item) {
				auto hashPath = hashFolder.filePath(item.completeBaseName()) + ".xml";
				return Archive { item.absoluteFilePath(), std::move(hashPath) };
			}) | std::views::filter([](const Archive& item) {
				return QFile::exists(item.hashPath);
			}),
			std::inserter(sorted, sorted.end()),
			[&](Archive archive) {
				const auto match = rx.match(QFileInfo(archive.filePath).fileName());
				return std::make_pair(match.hasMatch() ? match.captured(1).toInt() : 0, std::move(archive));
			}
		);
	}

	return std::move(sorted) | std::views::values | std::views::reverse | std::ranges::to<Archives>();
}

using ContentsItem = std::tuple<QString /*authors*/, QString /*title*/, QString /*series*/, QString /*seqNum*/>;
using Contents     = std::unordered_map<QString /*lang*/, std::unordered_map<BookItem, ContentsItem, Util::PairHash<QString, QString>>>;

void WriteContents(const QDir& outputDir, Contents& contents)
{
	PLOGI << "merge contents";

	const auto outputFilePath = outputDir.absoluteFilePath(QString::fromStdWString(Inpx::CONTENTS));
	QFile::remove(outputFilePath);

	auto zipFiles = Zip::CreateZipFileController();
	for (auto&& [lang, books] : contents)
	{
		const ScopedCall logGuard([&] {
			PLOGI << lang << ": " << books.size();
		});

		std::vector<std::tuple<QString /*authors*/, QString /*title*/, QString /*series*/, QString /*folder*/, QString /*file*/>> sorted;
		for (auto&& [uid, book] : books)
			sorted.emplace_back(
				std::move(std::get<0>(book)),
				std::move(std::get<1>(book)),
				std::get<2>(book).isEmpty() ? QString {} : QString("[%1%2]").arg(std::get<2>(book), std::get<3>(book).isEmpty() ? QString {} : QString(" #%1").arg(std::get<3>(book))),
				uid.first,
				uid.second
			);
		std::ranges::sort(sorted);

		QByteArray data;
		for (const auto& item : sorted)
			data.append(QString("%1\t%2\t%3\t%4\t%5\x0d\x0a").arg(std::get<0>(item), std::get<1>(item), std::get<2>(item), std::get<3>(item), std::get<4>(item)).toUtf8());

		zipFiles->AddFile(QString("%1.txt").arg(lang), std::move(data));
	}

	PLOGI << "archive contents";
	Zip zip(outputFilePath, Zip::Format::SevenZip);
	zip.SetProperty(ZipDetails::PropertyId::SolidArchive, false);
	zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
	zip.Write(std::move(zipFiles));
}

QDateTime ProcessInpx(IZipFileController& zipFiles, const QString& inpxFilePath, const UniquePaths& inputPaths, const Replacement& replacement, Contents& contents)
{
	const auto inputInpFiles = inputPaths | std::views::transform([](const QString& item) {
								   return QFileInfo(item).completeBaseName().toLower() + ".inp";
							   })
	                         | std::ranges::to<UniquePaths>();
	auto maxDateTime = QDateTime::fromSecsSinceEpoch(0);
	Zip  zip(inpxFilePath);
	for (const auto& inpFileName : zip.GetFileNameList() | std::views::filter([&](const QString& item) {
									   return item.endsWith(".inp") && inputInpFiles.contains(item.toLower());
								   }))
	{
		QByteArray bytes;
		const auto zipFile = zip.Read(inpFileName);
		auto&      stream  = zipFile->GetStream();
		size_t     counter = 0, total = 0;
		for (auto byteArray = stream.readLine(); !byteArray.isEmpty(); byteArray = stream.readLine())
		{
			++total;
			auto book   = Book::FromString(QString::fromUtf8(byteArray));
			book.folder = QFileInfo(inpFileName).completeBaseName() + ".7z";
			if (replacement.contains(std::make_pair(book.folder, book.GetFileName())))
				continue;

			++counter;
			bytes.append(byteArray);
			contents[book.lang].try_emplace(
				std::make_pair(book.folder, book.GetFileName()),
				std::make_tuple(book.author, book.title, book.series.empty() ? QString {} : book.series.front().title, book.series.empty() ? QString {} : book.series.front().serNo)
			);
		}

		if (bytes.isEmpty())
		{
			PLOGI << inpFileName << " skipped";
			continue;
		}

		PLOGI << inpFileName << " rows removed: " << total - counter << " of " << total;

		auto inpFileDateTime = zip.GetFileTime(inpFileName);
		zipFiles.AddFile(inpFileName, std::move(bytes), inpFileDateTime);
		if (maxDateTime < inpFileDateTime)
			maxDateTime = std::move(inpFileDateTime);
	}

	return maxDateTime;
}

void MergeInpx(const Settings& settings, const InputDirs& inputDirs, const Replacement& replacement)
{
	auto     zipFiles    = Zip::CreateZipFileController();
	auto     maxDateTime = QDateTime::fromSecsSinceEpoch(0);
	Contents contents;

	for (const auto& [inputDir, inputPaths] : inputDirs)
		for (const auto& inpx : inputDir.entryList({ "*.inpx" }, QDir::Files))
			if (auto inpxFileDateTime = ProcessInpx(*zipFiles, inputDir.absoluteFilePath(inpx), inputPaths, replacement, contents); maxDateTime < inpxFileDateTime)
				maxDateTime = std::move(inpxFileDateTime);

	WriteContents(settings.outputDir, contents);

	const auto outputZipFilePath = settings.outputDir.absoluteFilePath(settings.outputDir.dirName() + ".inpx");
	QFile::remove(outputZipFilePath);
	Zip zip(outputZipFilePath, Zip::Format::Zip);
	zip.SetProperty(Zip::PropertyId::CompressionLevel, QVariant::fromValue(Zip::CompressionLevel::Ultra));

	zipFiles->AddFile(Inpx::STRUCTURE_INFO, Inpx::INP_FIELDS_DESCRIPTION, QDateTime::currentDateTime());
	zipFiles->AddFile(QString::fromStdWString(Inpx::VERSION_INFO), maxDateTime.toString("yyyyMMdd").toUtf8(), QDateTime::currentDateTime());
	const auto collectionInfo = [&]() -> QString {
		if (!QFile::exists(settings.collectionInfoTemplateFile))
			return {};

		if (QFile file(settings.collectionInfoTemplateFile); file.open(QIODevice::ReadOnly))
			return QString::fromUtf8(file.readAll()).arg(maxDateTime.toString("yyyy-MM-dd"), maxDateTime.toString("yyyyMMdd"));

		return {};
	}();
	if (!collectionInfo.isEmpty())
		zipFiles->AddFile(QString::fromStdWString(Inpx::COLLECTION_INFO), collectionInfo.toUtf8(), QDateTime::currentDateTime());

	PLOGI << "archive inpx files: " << zipFiles->GetCount();
	zip.Write(std::move(zipFiles));
}

void MergeReviews(const QDir& outputDir, const InputDirs& inputDirs, const Replacement& replacement)
{
	std::unordered_map<QString, std::vector<std::pair<QString, std::reference_wrapper<const UniquePaths>>>> reviews;
	for (const auto& [inputDir, inputPaths] : inputDirs)
		for (const auto reviewFolder = QDir(inputDir.absoluteFilePath(QString::fromStdWString(Inpx::REVIEWS_FOLDER))); const auto& reviewFile : reviewFolder.entryList({ "??????.7z" }, QDir::Files))
			reviews[reviewFile].emplace_back(reviewFolder.absoluteFilePath(reviewFile), inputPaths);

	for (auto&& [index, review] : std::views::zip(std::views::iota(1), reviews))
	{
		const auto& [fileName, reviewFiles] = review;
		const ScopedCall logGuard([&] {
			PLOGI << QString("process %1: %2 (%3) %4%").arg(fileName).arg(index).arg(reviews.size()).arg(static_cast<qsizetype>(index) * 100 / reviews.size());
		});

		std::unordered_map<QString, std::pair<std::unordered_set<std::tuple<QString, QString, QString>, Util::TupleHash<QString, QString, QString>>, QDateTime>> data;

		const auto outputReviewFilePath = outputDir.absoluteFilePath(QString("%1/%2").arg(QString::fromStdWString(Inpx::REVIEWS_FOLDER), fileName));
		QFile::remove(outputReviewFilePath);

		for (const auto& [reviewFile, inputPaths] : reviewFiles)
		{
			const Zip zip(reviewFile);
			for (const auto& reviewBookFile : zip.GetFileNameList())
			{
				QJsonParseError error;
				const auto      doc = QJsonDocument::fromJson(zip.Read(reviewBookFile)->GetStream().readAll(), &error);
				if (error.error != QJsonParseError::NoError || !doc.isArray())
					throw std::ios_base::failure(std::format("bad json: {}/{}", reviewFile, reviewBookFile));

				const auto splitted = reviewBookFile.split('#');
				assert(splitted.size() == 2);
				if (!inputPaths.get().contains(splitted.front()))
					continue;

				const auto it                = replacement.find(std::make_pair(splitted.front(), splitted.back()));
				const auto dstReviewBookFile = it == replacement.end() ? reviewBookFile : QString("%1#%2").arg(it->second.first, it->second.second);
				auto&      dstArray          = data[dstReviewBookFile];
				for (const auto reviewValue : doc.array())
				{
					const auto reviewObj = reviewValue.toObject();
					dstArray.first.emplace(reviewObj[Inpx::NAME].toString(), reviewObj[Inpx::TIME].toString(), reviewObj[Inpx::TEXT].toString());
				}
				if (auto dateTime = zip.GetFileTime(reviewBookFile); !dstArray.second.isValid() || dateTime > dstArray.second)
					dstArray.second = std::move(dateTime);
			}
		}

		auto zipFiles = Zip::CreateZipFileController();
		for (auto&& [reviewBookFile, dstArray] : data)
		{
			QJsonArray fileData;
			for (const auto& [name, time, text] : dstArray.first)
				fileData.append(QJsonObject {
					{ Inpx::NAME, name },
					{ Inpx::TIME, time },
					{ Inpx::TEXT, text },
				});
			zipFiles->AddFile(reviewBookFile, QJsonDocument(fileData).toJson(), std::move(dstArray.second));
		}

		Zip zip(outputReviewFilePath, Zip::Format::SevenZip);
		zip.SetProperty(ZipDetails::PropertyId::SolidArchive, false);
		zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
		zip.Write(std::move(zipFiles));
	}
}

void MergeReviewsAdditional(const QDir& outputDir, const InputDirs& inputDirs, const Replacement& replacement)
{
	PLOGI << "merge reviews additional";
	const auto additionalFileName = QString::fromStdWString(Inpx::REVIEWS_FOLDER) + "/" + QString::fromStdWString(Inpx::REVIEWS_ADDITIONAL_ARCHIVE_NAME);
	std::vector<std::pair<QString, std::reference_wrapper<const UniquePaths>>> reviewsAdditional;
	for (const auto& [inputDir, inputPaths] : inputDirs)
		if (auto reviewsAdditionalFile = inputDir.absoluteFilePath(additionalFileName); QFile::exists(reviewsAdditionalFile))
			reviewsAdditional.emplace_back(std::move(reviewsAdditionalFile), inputPaths);

	const auto outputReviewsAdditionalFilePath = outputDir.absoluteFilePath(additionalFileName);
	QFile::remove(outputReviewsAdditionalFilePath);

	std::unordered_map<BookItem, std::pair<double, int>, Util::PairHash<QString, QString>> additional;

	auto maxDateTime = QDateTime::fromSecsSinceEpoch(0);

	for (const auto& [reviewsAdditionalFile, inputPaths] : reviewsAdditional)
	{
		const Zip       zip(reviewsAdditionalFile);
		QJsonParseError error;
		const auto      doc = QJsonDocument::fromJson(zip.Read(Inpx::REVIEWS_ADDITIONAL_BOOKS_FILE_NAME)->GetStream().readAll(), &error);
		if (error.error != QJsonParseError::NoError || !doc.isArray())
			throw std::ios_base::failure(std::format("bad json: {}/{}", reviewsAdditionalFile, Inpx::REVIEWS_ADDITIONAL_BOOKS_FILE_NAME));

		for (const auto reviewAdditionalValue : doc.array())
		{
			const auto obj = reviewAdditionalValue.toObject();
			BookItem   book(obj[Inpx::FOLDER].toString(), obj[Inpx::FILE].toString());
			if (const auto it = replacement.find(book); it != replacement.end())
				book = it->second;

			auto& value   = additional[book];
			value.first  += obj[Inpx::SUM].toDouble(0.0);
			value.second += obj[Inpx::COUNT].toInt(0);
		}

		maxDateTime = std::max(maxDateTime, zip.GetFileTime(Inpx::REVIEWS_ADDITIONAL_BOOKS_FILE_NAME));
	}

	QJsonArray additionalArray;
	for (const auto& [book, value] : additional)
		additionalArray.append(QJsonObject {
			{ Inpx::FOLDER,   book.first },
			{   Inpx::FILE,  book.second },
			{    Inpx::SUM,  value.first },
			{  Inpx::COUNT, value.second },
		});

	PLOGI << "write reviews additional";
	Zip  zip(outputReviewsAdditionalFilePath, Zip::Format::Zip);
	auto zipFiles = Zip::CreateZipFileController();
	zipFiles->AddFile(Inpx::REVIEWS_ADDITIONAL_BOOKS_FILE_NAME, QJsonDocument(additionalArray).toJson(), std::move(maxDateTime));
	zip.Write(std::move(zipFiles));
}

void ParseCompilations(const QString& filePath, const UniquePaths& inputPaths, Compilations& compilations)
{
	PLOGI << "parsing " << filePath;
	QJsonParseError error;
	const auto      doc = QJsonDocument::fromJson(Zip(filePath).Read(Inpx::COMPILATIONS_JSON)->GetStream().readAll(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isArray())
		throw std::ios_base::failure(std::format("bad json: {}/{}", filePath, Inpx::COMPILATIONS_JSON));

	const auto getBookItem = [](const auto& obj) {
		return BookItem { obj[Inpx::FOLDER].toString(), obj[Inpx::FILE].toString() };
	};

	std::ranges::transform(
		doc.array() | std::views::transform([](const auto& value) {
			return value.toObject();
		}),
		std::inserter(compilations, compilations.end()),
		[&](const auto& obj) {
			return std::make_pair(
				getBookItem(obj),
				Compilation { obj[Inpx::COMPILATION].toArray() | std::views::transform([](const auto& item) {
								  return item.toObject();
							  }) | std::views::transform([&](const auto& item) {
								  return std::make_pair(getBookItem(item), item[Inpx::PART].toInt());
							  }) | std::views::filter([&](const auto& item) {
								  return inputPaths.contains(item.first.first);
							  }) | std::ranges::to<CompilationParts>(),
		                      obj[Inpx::COVERED].toBool() }
			);
		}
	);
}

void MergeCompilations(const QDir& outputDir, const InputDirs& inputDirs, const Replacement& replacement)
{
	PLOGI << "merge compilations info";
	const auto outputCompilationsFilePath = outputDir.absoluteFilePath(QString::fromStdWString(Inpx::COMPILATIONS));
	QFile::remove(outputCompilationsFilePath);

	Compilations compilations;
	for (const auto& [inputDir, inputPaths] : inputDirs)
		if (auto reviewsAdditionalFile = inputDir.absoluteFilePath(QString::fromStdWString(Inpx::COMPILATIONS)); QFile::exists(reviewsAdditionalFile))
			ParseCompilations(reviewsAdditionalFile, inputPaths, compilations);

	erase_if(compilations, [&](const auto item) {
		return replacement.contains(item.first);
	});

	QJsonArray jsonCompilationsArray;

	for (auto& [uid, compilation] : compilations)
	{
		CompilationParts compilationParts;
		for (auto&& part : compilation.first)
			if (const auto it = replacement.find(part.first); it != replacement.end())
				compilationParts.emplace_back(it->second, part.second);
			else
				compilationParts.emplace_back(std::move(part));
		compilation.first = std::move(compilationParts);

		QJsonArray compilationArray;
		for (const auto& [partUid, index] : compilation.first)
			compilationArray.append(QJsonObject {
				{   Inpx::PART,          index },
				{ Inpx::FOLDER,  partUid.first },
				{   Inpx::FILE, partUid.second },
			});

		jsonCompilationsArray.append(QJsonObject {
			{      Inpx::FOLDER,                   uid.first },
			{        Inpx::FILE,                  uid.second },
			{ Inpx::COMPILATION, std::move(compilationArray) },
			{     Inpx::COVERED,          compilation.second },
		});
	}

	PLOGI << "write compilations info";
	auto zipFiles = Zip::CreateZipFileController();
	zipFiles->AddFile(Inpx::COMPILATIONS_JSON, QJsonDocument(jsonCompilationsArray).toJson());
	Zip zip(outputCompilationsFilePath, Zip::Format::SevenZip);
	zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
	zip.Write(std::move(zipFiles));
}

InputDirs GetInputFolders(const Archives& archives)
{
	UniquePaths                           uniquePaths;
	std::unordered_map<QString, InputDir> unique;
	std::vector<QDir>                     dirs;
	for (auto&& [dir, path] : archives | std::views::transform([](const Archive& item) {
								  return std::make_pair(QFileInfo(item.filePath).dir(), item.filePath);
							  }))
	{
		auto& [uniqueDir, paths] = unique[dir.absolutePath().toLower()];
		if (!paths.empty())
			continue;

		uniqueDir = std::move(dir);
		std::ranges::move(
			uniqueDir.entryList(QDir::Files) | std::views::filter([&](const auto& item) {
				return !uniquePaths.contains(item);
			}),
			std::inserter(paths, paths.end())
		);
		std::ranges::copy(paths, std::inserter(uniquePaths, uniquePaths.end()));
	}

	InputDirs result;
	std::ranges::move(unique | std::views::values, std::back_inserter(result));

	return result;
}

Settings ProcessCommandLine(const QCoreApplication& app)
{
	Settings settings;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 recodes images").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives with hashes (required)");
	parser.addOptions({
		{				   { "o", FOLDER }, "Output folder (required)", FOLDER },
		{ { "i", COLLECTION_INFO_TEMPLATE }, "Collection info template",   PATH },
	});

	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	settings.logFileName                = parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath;
	settings.collectionInfoTemplateFile = parser.value(COLLECTION_INFO_TEMPLATE);

	if (const auto& positionalArguments = parser.positionalArguments(); !positionalArguments.isEmpty())
	{
		settings.arguments = parser.positionalArguments();
	}
	else
	{
		parser.showHelp();
	}

	if (parser.isSet(FOLDER))
	{
		settings.outputDir = QDir { parser.value(FOLDER) };
	}
	else
	{
		parser.showHelp();
	}

	return settings;
}

void run(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv); //-V821
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);
	Util::XMLPlatformInitializer xmlPlatformInitializer;

	const auto settings = ProcessCommandLine(app);

	Log::LoggingInitializer                          logging(settings.logFileName.toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	if (!settings.outputDir.exists() && !settings.outputDir.mkpath("."))
		throw std::ios_base::failure(std::format("Cannot create folder {}", settings.outputDir.path()));

	const auto archives  = GetArchives(settings);
	const auto inputDirs = GetInputFolders(archives);

	PLOGD << "Total file count calculation";
	const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), size_t { 0 }, [](const size_t init, const Archive& archive) {
		const Zip zip(archive.filePath);
		return init + zip.GetFileNameList().size();
	});
	PLOGI << "Total file count: " << totalFileCount;

	UniqueFileStorage uniqueFileStorage(settings.outputDir.absolutePath());

	Replacement replacement;
	uniqueFileStorage.SetDuplicateObserver(std::make_unique<DuplicateObserver>(replacement));
	GetReplacement(archives, uniqueFileStorage);

	settings.outputDir.mkpath(QString::fromStdWString(Inpx::REVIEWS_FOLDER));
	MergeCompilations(settings.outputDir, inputDirs, replacement);
	MergeReviews(settings.outputDir, inputDirs, replacement);
	MergeReviewsAdditional(settings.outputDir, inputDirs, replacement);

	MergeInpx(settings, inputDirs, replacement);
	MergeArchives(settings.outputDir, archives, replacement);
	MergeHash(settings.outputDir, archives, replacement);
}

} // namespace

int main(const int argc, char* argv[])
{
	try
	{
		run(argc, argv);
		return 0;
	}
	catch (const std::exception& ex)
	{
		PLOGE << QString("%1 failed: %2").arg(APP_ID).arg(ex.what());
	}
	catch (...)
	{
		PLOGE << QString("%1 failed").arg(APP_ID);
	}
	return 1;
}
