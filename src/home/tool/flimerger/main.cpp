#include <ranges>
#include <set>
#include <unordered_set>

#include <QCommandLineParser>
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "lib/UniqueFile.h"
#include "lib/book.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/BookUtil.h"
#include "util/LogConsoleFormatter.h"
#include "util/files.h"
#include "util/xml/Initializer.h"

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

struct Settings
{
	QDir        outputDir;
	QStringList arguments;
	QString     collectionInfoTemplateFile;
	QString     logFileName;
};

void ProcessArchive(const QDir& outputDir, const Archive& archive, UniqueFileStorage& uniqueFileStorage, Replacement& replacement)
{
	QFile file(archive.hashPath);
	if (!file.open(QIODevice::ReadOnly))
		throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));

	Util::Remove::Books toRemove;

	const QFileInfo fileInfo(archive.filePath);

	HashParser::Parse(
		file,
		[&, idBook = 0LL](
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString NAME,
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
				QString cover,
			QStringList images
		) mutable {
			decltype(UniqueFile::images) imageItems;
			std::ranges::transform(std::move(images) | std::views::as_rvalue, std::inserter(imageItems, imageItems.end()), [](QString&& hash) {
				return ImageItem { .hash = std::move(hash) };
			});
			auto split    = title.split(' ', Qt::SkipEmptyParts);
			auto hashText = id;

			auto uniqueFile = uniqueFileStorage.Add(
				std::move(id),
				UniqueFile {
					.title    = { std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()) },
					.folder   = std::move(folder),
					.file     = file,
					.hashText = std::move(hashText),
					.cover    = { .hash = std::move(cover) },
					.images   = std::move(imageItems)
            }
			);

			if (uniqueFile)
				return;

			const auto& book         = toRemove.emplace_back(++idBook, fileInfo.fileName(), std::move(file));
			auto&       replacedWith = uniqueFile.error();
			replacement.try_emplace(std::make_pair(book.folder, book.file), std::make_pair(std::move(replacedWith.first), std::move(replacedWith.second)));
		}
	);

	uniqueFileStorage.Save(fileInfo.completeBaseName(), false);

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

Replacement ProcessArchives(const QDir& outputDir, const Archives& archives, UniqueFileStorage& uniqueFileStorage)
{
	PLOGD << "Total file count calculation";
	const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), size_t { 0 }, [](const size_t init, const Archive& archive) {
		const Zip zip(archive.filePath);
		return init + zip.GetFileNameList().size();
	});
	PLOGI << "Total file count: " << totalFileCount;

	Replacement replacement;

	for (const auto& archive : archives)
		ProcessArchive(outputDir, archive, uniqueFileStorage, replacement);

	return replacement;
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

QDateTime ProcessInpx(IZipFileController& zipFiles, const QString& inpxFilePath, const Replacement& replacement)
{
	auto maxDateTime = QDateTime::fromSecsSinceEpoch(0);
	Zip  zip(inpxFilePath);
	for (const auto& inpFileName : zip.GetFileNameList() | std::views::filter([](const QString& item) {
									   return item.endsWith(".inp");
								   }))
	{
		QByteArray bytes;
		const auto zipFile = zip.Read(inpFileName);
		auto&      stream  = zipFile->GetStream();
		size_t     counter = 0, total = 0;
		for (auto byteArray = stream.readLine(); !byteArray.isEmpty(); byteArray = stream.readLine())
		{
			++total;
			const auto book = Book::FromString(QString::fromUtf8(byteArray));
			if (replacement.contains(std::make_pair(QFileInfo(inpFileName).completeBaseName() + ".7z", book.file + '.' + book.ext)))
				++counter;
			else
				bytes.append(byteArray);
		}

		if (bytes.isEmpty())
		{
			PLOGI << inpFileName << " skipped";
			continue;
		}

		PLOGI << inpFileName << " rows removed: " << counter << " of " << total;

		auto inpFileDateTime = zip.GetFileTime(inpFileName);
		zipFiles.AddFile(inpFileName, std::move(bytes), inpFileDateTime);
		if (maxDateTime < inpFileDateTime)
			maxDateTime = std::move(inpFileDateTime);
	}

	return maxDateTime;
}

void ProcessInpx(const Settings& settings, const Archives& archives, const Replacement& replacement)
{
	auto zipFiles    = Zip::CreateZipFileController();
	auto maxDateTime = QDateTime::fromSecsSinceEpoch(0);

	std::unordered_set<QString> uniqueFolders;
	for (const auto& folder : archives | std::views::transform([](const Archive& item) {
								  return QFileInfo(item.filePath).dir();
							  }) | std::views::filter([&](const QDir& item) {
								  auto       path   = item.absolutePath().toLower();
								  const auto result = !uniqueFolders.contains(path);
								  if (result)
									  uniqueFolders.emplace(std::move(path));
								  return result;
							  }))
		for (const auto& inpx : folder.entryList({ "*.inpx" }, QDir::Files))
			if (auto inpxFileDateTime = ProcessInpx(*zipFiles, folder.absoluteFilePath(inpx), replacement); maxDateTime < inpxFileDateTime)
				maxDateTime = std::move(inpxFileDateTime);

	const auto outputZipFilePath = settings.outputDir.absoluteFilePath(settings.outputDir.dirName() + ".inpx");
	QFile::remove(outputZipFilePath);
	Zip zip(outputZipFilePath, Zip::Format::Zip);
	zip.SetProperty(Zip::PropertyId::CompressionLevel, QVariant::fromValue(Zip::CompressionLevel::Ultra));

	zipFiles->AddFile(Inpx::STRUCTURE_INFO, Inpx::INP_FIELDS_DESCRIPTION, QDateTime::currentDateTime());
	zipFiles->AddFile(QString::fromStdWString(Inpx::VERSION_INFO), maxDateTime.toString("yyyyMMdd").toUtf8(), QDateTime::currentDateTime());
	const auto collectionInfo = [&]() -> QString {
		if (QFile file(settings.collectionInfoTemplateFile); file.open(QIODevice::ReadOnly))
			return QString::fromUtf8(file.readAll()).arg(maxDateTime.toString("yyyy-MM-dd"), maxDateTime.toString("yyyyMMdd"));

		return {};
	}();
	if (!collectionInfo.isEmpty())
		zipFiles->AddFile(QString::fromStdWString(Inpx::COLLECTION_INFO), collectionInfo.toUtf8(), QDateTime::currentDateTime());

	PLOGI << "archive inpx files: " << zipFiles->GetCount();
	zip.Write(std::move(zipFiles));
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

	const auto archives = GetArchives(settings);

	UniqueFileStorage uniqueFileStorage(settings.outputDir.absolutePath());

	const auto replacement = ProcessArchives(settings.outputDir, archives, uniqueFileStorage);
	ProcessInpx(settings, archives, replacement);
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
