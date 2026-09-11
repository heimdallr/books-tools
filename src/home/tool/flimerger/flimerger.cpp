#include <ranges>
#include <set>
#include <unordered_set>

#include <QCommandLineParser>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/FindPair.h"
#include "fnd/StrUtil.h"

#include "database/interface/IDatabase.h"
#include "database/interface/ITemporaryTable.h"
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
#include "util/StrUtil.h"
#include "util/progress.h"

#include "Constant.h"
#include "log.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flimerger";

constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";
constexpr auto FOLDER                       = "folder";
constexpr auto PATH                         = "path";
constexpr auto DUMP                         = "dump";
constexpr auto HAMMING_THRESHOLD            = "hamming";
constexpr auto DATABASE                     = "database";

using BookItem    = std::pair<QString, QString>;
using Replacement = std::unordered_map<BookItem, BookItem, Util::PairHash<QString, QString>>;

struct Settings
{
	QDir                           outputDir;
	QStringList                    arguments;
	QString                        logFileName;
	QString                        dumpWildCards;
	int                            hammingThreshold { 10 };
	std::unique_ptr<DB::IDatabase> database;
};

class UniqueFileConflictResolver final : public UniqueFileStorage::IUniqueFileConflictResolver
{
public:
	explicit UniqueFileConflictResolver(InpDataProvider& inpDataProvider)
		: m_inpDataProvider { inpDataProvider }
	{
	}

private: // UniqueFileStorage::IUniqueFileConflictResolver
	[[nodiscard]] bool Resolve(const UniqueFile& file, const UniqueFile& duplicate) const override
	{
		static constexpr std::pair<const char*, int> weights[] {
			{ "flibusta", 1000 },
			{ "librusec",  100 }
		};
		const auto toComparable = [this](const UniqueFile& item) {
			const auto* book  = m_inpDataProvider.GetBook(item.uid);
			const auto  isFb2 = QFileInfo(item.uid.file).suffix().toLower() == "fb2";
			return book ? std::make_tuple(true, !book->deleted, isFb2, FindSecond(weights, book->sourceLib.toStdString().data(), 0, PszComparerCaseInsensitive {}), book->date, book->libId)
			            : std::make_tuple(false, false, isFb2, 0, QString { "0000-00-00" }, item.uid.file);
		};

		return toComparable(file) > toComparable(duplicate);
	}

private:
	InpDataProvider& m_inpDataProvider;
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

//void ProcessHash(DB::IDatabase& db, const Archive& archive, const Replacement& replacement)
//{
//	PLOGI << "parsing " << archive.hashPath;
//	hashDir.mkpath(".");
//	QFileInfo fileInfo(archive.hashPath);
//
//	QFile input(archive.hashPath);
//	if (!input.open(QIODevice::ReadOnly))
//		throw std::ios_base::failure(std::format("Cannot read from {}", archive.hashPath));
//
//	const auto outputFilePath = hashDir.filePath(fileInfo.fileName());
//
//	QFile output(outputFilePath);
//	if (!output.open(QIODevice::WriteOnly))
//		throw std::ios_base::failure(std::format("Cannot write to", outputFilePath));
//
//	[[maybe_unused]] const HashCopier parser(input, output, replacement);
//}

void UpdateDatabase(DB::IDatabase& db, const QString& path, const Replacement& replacement)
{
	const QFileInfo fileInfo(path);
	const auto      folder = fileInfo.fileName();

	const auto tmpTable = db.CreateTemporaryTable({ "Folder VARCHAR (64)", "File VARCHAR (256)", "FolderOrigin VARCHAR (64)", "FileOrigin VARCHAR (256)" });

	const auto tr = db.CreateTransaction();
	tr->CreateCommand(std::format("update File set OriginId = null from (select FolderId from Folder where Name = '{}') as Id where File.FolderId = Id.FolderId", folder))->Execute();
	{
		const auto command = tr->CreateCommand(std::format("insert into {}(Folder, File, FolderOrigin, FileOrigin) values(?, ?, ?, ?)", tmpTable->GetName()));
		for (const auto& [duplicate, origin] : replacement)
		{
			command->Bind(0, duplicate.first);
			command->Bind(1, duplicate.second);
			command->Bind(2, origin.first);
			command->Bind(3, origin.second);
			command->Execute();
		}
	}
	tr->CreateCommand(
		  std::format(
			  R"(
update File set OriginId = Id.FileIdOrigin from (
select f.FileId as FileId, f1.FileId as FileIdOrigin
from {} t
join Folder d on d.Name = t.Folder
join File f on f.FolderId = d.FolderId and f.Name = t.File
join Folder d1 on d1.Name = t.FolderOrigin
join File f1 on f1.FolderId = d1.FolderId and f1.Name = t.FileOrigin
) as Id 
where File.FileId = Id.FileId
)",
			  tmpTable->GetName()
		  )
	)
		->Execute();
	tr->Commit();
}

void MergeArchives(const QDir& outputDir, DB::IDatabase& db, const Archives& archives, const Replacement& replacement)
{
	for (const auto& archive : archives)
	{
		ProcessArchive(outputDir, archive, replacement);
		UpdateDatabase(db, archive.filePath, replacement);
	}
}

void GetReplacement(DB::IDatabase& db, const QString& path, UniqueFileStorage& uniqueFileStorage, InpDataProvider& inpDataProvider, Util::Progress& progress)
{
	const QFileInfo fileInfo(path);
	const auto      folder = fileInfo.fileName();

	const auto [folderId, sourceLib] = [&] {
		const auto query = db.CreateQuery("select f.FolderId, s.Name from Folder f join SourceLibrary s on s.SourceLibraryId = f.SourceLibraryId where f.name = ?");
		query->Bind(0, folder);
		query->Execute();
		if (query->Eof())
			throw std::invalid_argument(std::format("{} not found in database", folder));
		return std::make_pair(query->Get<long long>(0), query->Get<QString>(1));
	}();

	inpDataProvider.SetSourceLib(sourceLib);

	for (auto&& uniqueFile : SelectUniqueFiles(db, folderId, folder) | std::views::values)
	{
		if (const auto* book = inpDataProvider.SetFile(uniqueFile.uid, uniqueFile.hash, uniqueFile.size))
			std::ranges::move(Util::UniqTitle(book->title), std::inserter(uniqueFile.title, uniqueFile.title.end()));

		const auto file = uniqueFile.uid.file;
		auto       hash = uniqueFile.hash;
		uniqueFileStorage.Add(std::move(hash), std::move(uniqueFile));
		progress.Increment(1, file.toStdString());
	}
}

void GetReplacement(const size_t totalFileCount, DB::IDatabase& db, const Archives& archives, UniqueFileStorage& uniqueFileStorage, InpDataProvider& inpDataProvider)
{
	Util::Progress progress(totalFileCount, "parsing");

	for (const auto& archive : archives)
		GetReplacement(db, archive.filePath, uniqueFileStorage, inpDataProvider, progress);
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
			{					   { "o", FOLDER },   "Output folder (required)",                                                        FOLDER },
			{								  DUMP,    "Dump database wildcards",                           "Semicolon separated wildcard list" },
			{ { QString { DATABASE[0] }, DATABASE },  "Books statistics database",                                                          PATH },
			{					 HAMMING_THRESHOLD, "Hamming distance threshold", QString("number [0, 64] [%1]").arg(settings.hammingThreshold) },
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
	settings.database      = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag=READWRITE", parser.value(DATABASE)));
	settings.dumpWildCards = parser.value(DUMP);
	if (parser.isSet(HAMMING_THRESHOLD))
		settings.hammingThreshold = parser.value(HAMMING_THRESHOLD).toInt();

	return settings;
}

void run(const Settings& settings)
{
	const auto                  archives       = GetArchives(settings.arguments);
	[[maybe_unused]] const auto totalFileCount = Total(archives);

	auto inpDataProvider = std::make_shared<InpDataProvider>(settings.dumpWildCards);

	UniqueFileStorage uniqueFileStorage(
		*settings.database,
		archives | std::views::transform([](const auto& item) {
			return QFileInfo(item.filePath).fileName();
		}) | std::ranges::to<std::unordered_set>(),
		settings.hammingThreshold,
		inpDataProvider
	);

	const auto conflictResolver = std::make_shared<UniqueFileConflictResolver>(*inpDataProvider);
	uniqueFileStorage.SetConflictResolver(conflictResolver);

	Replacement replacement;
	uniqueFileStorage.SetDuplicateObserver(std::make_unique<DuplicateObserver>(replacement));
	GetReplacement(totalFileCount, *settings.database, archives, uniqueFileStorage, *inpDataProvider);

	PLOGI << "Duplicates found: " << replacement.size();

	MergeArchives(settings.outputDir, *settings.database, archives, replacement);
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
