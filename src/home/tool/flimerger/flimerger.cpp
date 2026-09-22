#include <ranges>
#include <unordered_set>

#include <QCommandLineParser>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/FindPair.h"
#include "fnd/StrUtil.h"

#include "database/interface/IDatabase.h"
#include "database/interface/ITransaction.h"

#include "database/factory/Factory.h"
#include "impl/FileItem.h"
#include "lib/UniqueFile.h"
#include "lib/archive.h"
#include "lib/book.h"
#include "lib/dump/Factory.h"
#include "lib/dump/IDump.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/BookUtil.h"
#include "util/LogConsoleFormatter.h"
#include "util/progress.h"

#include "Constant.h"
#include "log.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace {

constexpr auto APP_ID = "flimerger";

constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";
constexpr auto FOLDER                       = "folder";
constexpr auto PATH                         = "path";
constexpr auto DATABASE                     = "database";
constexpr auto DUMP                         = "dump";

struct Settings
{
	QDir                           outputDir;
	QStringList                    arguments;
	QString                        logFileName;
	QString                        dumpWildCards;
	std::unique_ptr<DB::IDatabase> database;
};

size_t WriteParsedBooks(DB::IDatabase& db, const QString& folder, InpDataProvider& inpDataProvider, const Zip& zip)
{
	size_t count = 0;

	const auto fileList      = zip.GetFileNameList();
	const auto alreadyParsed = [&] {
		std::unordered_set<QString> result;
		const auto                  query = db.CreateQuery("SELECT f.Name FROM FileCustom c JOIN File f ON f.FileId = c.FileId JOIN Folder d ON d.FolderId = f.FolderId AND d.Name = ?");
		query->Bind(0, folder);
		for (query->Execute(); !query->Eof(); query->Next())
			result.emplace(query->Get<const char*>(0));
		return result;
	}();
	const auto tr = db.CreateTransaction();
	for (const auto& fileName : fileList | std::views::filter([&](const QString& item) {
									return !alreadyParsed.contains(item) && !inpDataProvider.SetFile({ .folder = folder, .file = item }, {}, 0);
								}))
	{
		auto book = ParseBook(fileName, inpDataProvider, folder, zip, zip.GetFileTime(fileName));
		WriteParsedBookToDatabase(*tr, *book);
		++count;
	}
	tr->Commit();

	return count;
}

struct ProcessResult
{
	size_t total { 0 };
	size_t removed { 0 };
	size_t parsed { 0 };

	ProcessResult& operator+=(const ProcessResult& rhs) noexcept
	{
		total   += rhs.total;
		removed += rhs.removed;
		parsed  += rhs.parsed;

		return *this;
	}
};

ProcessResult ProcessArchive(const QDir& outputDir, DB::IDatabase& db, const QString& archive, const std::unordered_set<QString>& replaced, InpDataProvider& inpDataProvider)
{
	const QFileInfo fileInfo(archive);
	const auto      dstFilePath = outputDir.filePath(fileInfo.fileName());
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

	ProcessResult result;

	auto fileList = [&] {
		const Zip zip(dstFilePath);
		result.parsed = WriteParsedBooks(db, fileInfo.fileName(), inpDataProvider, zip);
		return zip.GetFileNameList();
	}();

	result.total = fileList.size();

	auto toRemove = fileList | std::views::filter([&](const QString& fileName) {
						return replaced.contains(fileName);
					})
	              | std::views::transform([&, n = 0](const QString& fileName) mutable {
						return Util::Remove::Book { ++n, fileInfo.fileName(), fileName };
					})
	              | std::ranges::to<Util::Remove::Books>();

	if (toRemove.empty())
		return result;

	result.removed = toRemove.size();

	auto allFiles = CollectBookFiles(toRemove, [] {
		return nullptr;
	});
	auto images   = Util::Remove::CollectImageFiles(allFiles, outputDir.absolutePath(), [] {
		return nullptr;
	});
	std::ranges::move(std::move(images), std::inserter(allFiles, allFiles.end()));
	Util::Remove::RemoveFiles(allFiles, outputDir.absolutePath());

	return result;
}

std::unordered_set<QString> GetReplacement(DB::IDatabase& db, const QString& archive)
{
	std::unordered_set<QString> replaced;

	const QFileInfo fileInfo(archive);
	const auto      query = db.CreateQuery("select f.Name from File f join Folder d on d.FolderId = f.FolderId and d.Name = ? where +f.OriginId is not null");
	query->Bind(0, fileInfo.fileName());
	for (query->Execute(); !query->Eof(); query->Next())
		replaced.emplace(query->Get<const char*>(0));

	return replaced;
}

Settings ProcessCommandLine(const QCoreApplication& app)
{
	Settings settings;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 recodes images").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives (required)");
	parser.addOptions(
		{
			{                       { "o", FOLDER },  "Output folder (required)",                              FOLDER },
			{                                  DUMP,   "Dump database wildcards", "Semicolon separated wildcard list" },
			{ { QString { DATABASE[0] }, DATABASE }, "Books statistics database",                                PATH },
	}
	);

	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	if (parser.positionalArguments().isEmpty() || !parser.isSet(FOLDER) || !parser.isSet(DATABASE))
		parser.showHelp();

	settings.logFileName   = parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath;
	settings.arguments     = parser.positionalArguments();
	settings.outputDir     = QDir { parser.value(FOLDER) };
	settings.dumpWildCards = parser.value(DUMP);
	settings.database      = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag=READWRITE", parser.value(DATABASE)));

	return settings;
}

void run(const Settings& settings)
{
	const auto archives        = GetArchives(settings.arguments);
	const auto inpDataProvider = std::make_shared<InpDataProvider>(settings.dumpWildCards);

	ProcessResult count;

	Util::Progress progress(archives.size(), "merging");
	for (const auto& archive : archives)
	{
		const auto folder    = QFileInfo(archive.filePath).fileName();
		const auto sourceLib = [&] {
			const auto query = settings.database->CreateQuery("select l.Name from SourceLibrary l join Folder d on d.SourceLibraryId = l.SourceLibraryId and d.Name = ?");
			query->Bind(0, folder);
			query->Execute();
			assert(!query->Eof());
			return query->Get<QString>(0);
		}();
		inpDataProvider->SetSourceLib(sourceLib);
		const auto replaced  = GetReplacement(*settings.database, archive.filePath);
		const auto result    = ProcessArchive(settings.outputDir, *settings.database, archive.filePath, replaced, *inpDataProvider);
		count               += result;
		progress.Increment(1, QString("%1: %2").arg(folder).arg(result.total).toStdString());
	}

	PLOGI << "total books removed: " << count.removed << ", parsed: " << count.parsed;
}

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv); //-V821
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);

	const auto settings = ProcessCommandLine(app);

	Log::LoggingInitializer                          logging(settings.logFileName);
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	try
	{
		if (!settings.outputDir.exists() && !settings.outputDir.mkpath("."))
			throw std::ios_base::failure(std::format("Cannot create folder {}", settings.outputDir.path()));

		run(settings);
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
