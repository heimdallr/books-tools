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
#include "util/LogConsoleFormatter.h"
#include "util/StrUtil.h"
#include "util/progress.h"

#include "log.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace {

constexpr auto APP_ID = "fliduper";

constexpr auto ARCHIVES          = "archives";
constexpr auto PATH              = "path";
constexpr auto DUMP              = "dump";
constexpr auto HAMMING_THRESHOLD = "hamming";
constexpr auto DATABASE          = "database";

using BookItem    = std::pair<QString, QString>;
using Replacement = std::unordered_map<BookItem, BookItem, Util::PairHash<QString, QString>>;

struct Settings
{
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
			return book ? std::make_tuple(
							  true,
							  !book->deleted,
							  isFb2,
							  item.sectionCount,
							  book->size.toULongLong(),
							  FindSecond(weights, book->sourceLib.toStdString().data(), 0, PszComparerCaseInsensitive {}),
							  book->date,
							  book->libId
						  )
			            : std::make_tuple(false, false, isFb2, 0ULL, 0ULL, 0, QString { "0000-00-00" }, item.uid.file);
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

auto GetTemporaryTable(DB::IDatabase& db)
{
	return db.CreateTemporaryTable({ "Folder VARCHAR (64)", "File VARCHAR (256)", "FolderOrigin VARCHAR (64)", "FileOrigin VARCHAR (256)" });
}

template <typename PairContainer, typename ItemToPair>
void FillTemporaryTable(DB::ITransaction& tr, const std::string_view tmpTable, const PairContainer& container, const ItemToPair& itemToPair)
{
	const auto command = tr.CreateCommand(std::format("insert into {}(Folder, File, FolderOrigin, FileOrigin) values(?, ?, ?, ?)", tmpTable));
	for (const auto& [duplicate, origin] : container | std::views::transform([&](const auto& item) {
											   const auto& [first, second] = item;
											   return std::make_pair(itemToPair(first), itemToPair(second));
										   }))
	{
		command->Bind(0, duplicate.first);
		command->Bind(1, duplicate.second);
		command->Bind(2, origin.first);
		command->Bind(3, origin.second);
		command->Execute();
	}
}

void UpdateOriginId(DB::ITransaction& tr, const std::string_view tmpTable, const std::string_view updatedField)
{
	tr.CreateCommand(
		  std::format(
			  R"(
update File set {} = Id.FileIdOrigin from (
select f.FileId as FileId, f1.FileId as FileIdOrigin
from {} t
join Folder d on d.Name = t.Folder
join File f on f.FolderId = d.FolderId and f.Name = t.File
join Folder d1 on d1.Name = t.FolderOrigin
join File f1 on f1.FolderId = d1.FolderId and f1.Name = t.FileOrigin
) as Id 
where File.FileId = Id.FileId
)",
			  updatedField,
			  tmpTable
		  )
	)
		->Execute();
}

void WriteDuplicate(DB::IDatabase& db, const Replacement& replacement)
{
	if (replacement.empty())
		return;

	PLOGI << "mark found duplicates in database";

	const auto tmpTable = GetTemporaryTable(db);
	const auto tr       = db.CreateTransaction();

	FillTemporaryTable(*tr, tmpTable->GetName(), replacement, [](const auto& item) {
		return std::make_pair(item.first, item.second);
	});
	UpdateOriginId(*tr, tmpTable->GetName(), "OriginId");

	tr->Commit();
}

void WriteOldDuplicate(DB::IDatabase& db, const UniqueFileStorage::OldDuplicates& oldDuplicates)
{
	if (oldDuplicates.empty())
		return;

	PLOGI << "mark found old duplicates in database";

	const auto tmpTable = GetTemporaryTable(db);
	const auto tr       = db.CreateTransaction();

	FillTemporaryTable(*tr, tmpTable->GetName(), oldDuplicates, [](const UniqueFile& item) {
		return std::make_pair(item.uid.folder, item.uid.file);
	});
	UpdateOriginId(*tr, tmpTable->GetName(), "NextOriginId");

	tr->Commit();
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

void GetReplacement(DB::IDatabase& db, const QStringList& archives, UniqueFileStorage& uniqueFileStorage, InpDataProvider& inpDataProvider)
{
	const auto     totalCount = [&] {
		const auto tmpTable = db.CreateTemporaryTable({ "Folder VARCHAR (64)" });
		const auto tr       = db.CreateTransaction();
		{
			const auto command = tr->CreateCommand(std::format("insert into {}(Folder) values(?)", tmpTable->GetName()));
			for (const auto& archive : archives)
			{
				command->Bind(0, archive);
				command->Execute();
			}
		}

		const auto query = tr->CreateQuery(std::format("select count(42) from {} t join Folder d on d.Name = t.Folder join File f on f.FolderId = d.FolderId", tmpTable->GetName()));
		query->Execute();
		assert(!query->Eof());
		return query->Get<size_t>(0);
	}();

	Util::Progress progress(totalCount, "parsing");

	for (const auto& archive : archives)
		GetReplacement(db, archive, uniqueFileStorage, inpDataProvider, progress);
}

Settings ProcessCommandLine(const QCoreApplication& app)
{
	Settings settings;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 recodes images").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVES, "Archives to process");
	parser.addOptions(
		{
			{                                  DUMP,    "Dump database wildcards",                           "Semicolon separated wildcard list" },
			{ { QString { DATABASE[0] }, DATABASE },  "Books statistics database",                                                          PATH },
			{                     HAMMING_THRESHOLD, "Hamming distance threshold", QString("number [0, 64] [%1]").arg(settings.hammingThreshold) },
	}
	);

	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	if (!parser.isSet(DATABASE))
		parser.showHelp();

	settings.logFileName   = parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath;
	settings.arguments     = parser.positionalArguments();
	settings.database      = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag=READWRITE", parser.value(DATABASE)));
	settings.dumpWildCards = parser.value(DUMP);
	if (parser.isSet(HAMMING_THRESHOLD))
		settings.hammingThreshold = parser.value(HAMMING_THRESHOLD).toInt();

	return settings;
}

void run(const Settings& settings)
{
	auto              inpDataProvider = std::make_shared<InpDataProvider>(settings.dumpWildCards);
	UniqueFileStorage uniqueFileStorage(*settings.database, settings.arguments | std::ranges::to<std::unordered_set>(), settings.hammingThreshold, inpDataProvider);

	const auto conflictResolver = std::make_shared<UniqueFileConflictResolver>(*inpDataProvider);
	uniqueFileStorage.SetConflictResolver(conflictResolver);

	Replacement replacement;
	uniqueFileStorage.SetDuplicateObserver(std::make_unique<DuplicateObserver>(replacement));
	GetReplacement(*settings.database, settings.arguments, uniqueFileStorage, *inpDataProvider);

	const auto oldDuplicates = uniqueFileStorage.GetOldDuplicates();
	if (oldDuplicates.empty() && replacement.empty())
		PLOGI << "duplicates not found";
	else
		PLOGI << "duplicates found, old: " << oldDuplicates.size() << ", new: " << replacement.size();

	WriteDuplicate(*settings.database, replacement);
	WriteOldDuplicate(*settings.database, oldDuplicates);
}

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv); //-V821
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);

	auto settings = ProcessCommandLine(app);

	Log::LoggingInitializer                          logging(settings.logFileName);
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	try
	{
		if (settings.arguments.isEmpty())
		{
			const auto query = settings.database->CreateQuery("select Name from Folder");
			for (query->Execute(); !query->Eof(); query->Next())
				settings.arguments << query->Get<QString>(0);
		}

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
