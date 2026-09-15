#include <condition_variable>
#include <queue>
#include <thread>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/StrUtil.h"

#include "database/interface/IDatabase.h"
#include "database/interface/ITransaction.h"

#include "database/factory/Factory.h"
#include "lib/dump/Factory.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"
#include "util/bookhash/hashbook.h"
#include "util/executor/ThreadPool.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "log.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa::Util;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flihasher";

constexpr auto FOLDER                       = "folder";
constexpr auto PATH                         = "path";
constexpr auto LIBRARY                      = "library";
constexpr auto THREADS                      = "threads";
constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";
constexpr auto DATABASE                     = "database";

struct Options
{
	QString                        sourceLib;
	QStringList                    args;
	unsigned int                   maxThreadCount { std::thread::hardware_concurrency() };
	std::unique_ptr<DB::IDatabase> database;
};

void SerializeHashSections(const long long fileId, const QStringList& sections, DB::ITransaction& tr)
{
	const auto command = tr.CreateCommand("insert into Section(FileId, ParentSectionId, Hash, WordCount, SymbolCount, SimHash) values(?, ?, ?, ?, ?, ?)");

	std::vector<long long> stack { -1 };
	qsizetype              depth = -1;
	for (const auto& str : sections)
	{
		const auto split = str.split('\t');
		assert(split.size() == 5);

		bool ok       = false;
		auto newDepth = split[0].toInt(&ok);
		assert(ok);

		for (int i = newDepth; i <= depth; ++i)
			stack.pop_back();

		depth = newDepth;

		command->Bind(0, fileId);
		if (stack.back() == -1)
			command->Bind(1);
		else
			command->Bind(1, stack.back());
		command->Bind(2, split[1]);
		command->Bind(3, split[2]);
		command->Bind(4, split[3]);
		command->Bind(5, split[4]);
		command->Execute();
		const auto query = tr.CreateQuery("select last_insert_rowid()");
		stack.push_back(query->Execute() ? query->Get<long long>(0) : (assert(false), -1LL));
	}
}

void ProcessArchive(const Options& options, const QString& filePath, Progress& progress)
{
	PLOGI << "process " << filePath;

	BookHashItemProvider bookHashItemProvider(filePath);
	QFileInfo            fileInfo(filePath);

	const auto fileList = bookHashItemProvider.GetFiles();

	std::vector<BookHashItem> bookHashItems;
	bookHashItems.reserve(static_cast<size_t>(fileList.size()));

	ThreadPool<QCryptographicHash> threadPool({ .threadCount = options.maxThreadCount, .maxQueueSize = static_cast<size_t>(options.maxThreadCount) * 2, .contextGetter = [](size_t) {
												   return QCryptographicHash { QCryptographicHash::Md5 };
											   } });
	for (const auto& file : fileList)
	{
		auto& bookTaskItem = bookHashItems.emplace_back(bookHashItemProvider.Get(file));
		threadPool.enqueue([&](QCryptographicHash& md5, const auto&) {
			PLOGV << "start parsing: " << bookTaskItem.file;
			ParseBookHash(bookTaskItem, md5);
			progress.Increment(1, bookTaskItem.file.toStdString());
		});
	}

	PLOGI << "wait for threads finished";
	threadPool.wait();

	if (bookHashItems.empty())
	{
		PLOGW << "no data received";
		return;
	}

	if (!options.database)
		return;

	PLOGD << "write database";

	const auto tr = options.database->CreateTransaction();

	const auto insertQuery = [&](const std::string_view queryText, const std::vector<QString>& parameters, const bool needInsertedId = false) {
		const auto command = tr->CreateCommand(queryText);
		for (auto&& [parameter, index] : std::views::zip(parameters, std::views::iota(0)))
			command->Bind(index, parameter);
		command->Execute();

		if (needInsertedId)
		{
			const auto query = tr->CreateQuery("select last_insert_rowid()");
			return query->Execute() ? query->Get<long long>(0) : -1LL;
		}

		return -1LL;
	};

	const auto insertImage = [&](const long long fileId, const QString& name, const ImageHashItem& item, const bool linked) {
		insertQuery(
			"insert into Image(FileId, Name, EncodedSize, DecodedSize, Width, Height, PHash, Md5, Linked, HasAlpha) values(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
			{
				QString::number(fileId),
				name,
				QString::number(item.encodedSize),
				QString::number(item.decodedSize),
				QString::number(item.size.width()),
				QString::number(item.size.height()),
				QString("%1").arg(item.pHash, 16, 16, QChar { '0' }),
				item.hash,
				linked ? "1" : "0",
				item.hasAlpha ? "1" : "0",
			}
		);
	};

	const auto folderId = insertQuery("insert into Folder(SourceLibraryId, Name) select SourceLibraryId, ? from SourceLibrary where SourceLibrary.Name = ?", { fileInfo.fileName(), options.sourceLib }, true);

	for (const auto& file : bookHashItems)
	{
		assert(file.folder == fileInfo.fileName());

		const auto fileId = insertQuery(
			"insert into File(FolderId, Name, Md5, Hash, WordCount, SymbolCount, SimHash, Title, Annotation) values(?, ?, ?, ?, ?, ?, ?, ?, ?)",
			{ QString::number(folderId),
		      file.file,
		      file.parseResult.id,
		      file.parseResult.hashText,
		      QString::number(file.parseResult.count),
		      QString::number(file.parseResult.size),
		      QString("%1").arg(file.parseResult.simHash, 16, 16, QChar { '0' }),
		      file.parseResult.title,
		      file.parseResult.annotation },
			true
		);

		SerializeHashSections(fileId, file.parseResult.hashSections, *tr);

		if (!file.cover.hash.isEmpty())
			insertImage(fileId, Global::COVER, file.cover, true);
		for (const auto& item : file.images)
			insertImage(fileId, item.file, item, file.parseResult.linkedImages.contains(item.file));

		for (const auto& [count, word] : file.parseResult.hashValues)
			insertQuery("insert into Histogram(FileId, Word, WordCount) values(?, ?, ?)", { QString::number(fileId), word, QString::number(count) });
	}

	tr->Commit();
}

QStringList GetArchives(const QStringList& wildCards)
{
	QStringList result;

	for (const auto& wildCard : wildCards)
		std::ranges::move(ResolveWildcard(wildCard), std::back_inserter(result));

	return result;
}

int run(const Options& options)
{
	try
	{
		const auto availableLibraries = Dump::GetAvailableLibraries();
		if (!availableLibraries.contains(options.sourceLib, Qt::CaseInsensitive))
			throw std::invalid_argument(std::format("{} must be {}", LIBRARY, availableLibraries.join(" | ")));

		const auto archives = GetArchives(options.args);

		PLOGD << "Total file count calculation";
		const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), size_t { 0 }, [](const auto init, const auto& item) {
			const Zip zip(item);
			return init + zip.GetFileNameList().size();
		});
		PLOGI << "Total file count: " << totalFileCount;

		Progress progress(totalFileCount, "parsing");

		for (const auto& archive : archives)
			ProcessArchive(options, archive, progress);

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

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);
	XMLPlatformInitializer xmlPlatformInitializer;

	const auto availableLibraries = Dump::GetAvailableLibraries();

	Options options;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 creates hash files for library").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives wildcards");
	parser.addOptions(
		{
			{ { QString(DATABASE[0]), DATABASE }, "Output database path (required)", PATH },
			{ LIBRARY, "Source library", QString("(%1) [%2]").arg(availableLibraries.join(" | "), availableLibraries.front()) },
			{ { QString(THREADS[0]), THREADS }, "Maximum number of CPU threads", QString("Thread count [%1]").arg(options.maxThreadCount) },
    }
	);
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	Log::LoggingInitializer                    logging(parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath);
	plog::ConsoleAppender<LogConsoleFormatter> consoleAppender;
	Log::LogAppender                           logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	if (!parser.isSet(DATABASE) || parser.positionalArguments().isEmpty())
		parser.showHelp(1);

	options.sourceLib = parser.value(LIBRARY).toLower();
	if (options.sourceLib.isEmpty())
		options.sourceLib = availableLibraries.front().toLower();

	if (parser.isSet(THREADS))
		options.maxThreadCount = parser.value(THREADS).toUInt();

	if (parser.isSet(DATABASE))
	{
		const auto dbPath = parser.value(DATABASE);
		options.database  = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag={}", dbPath, QFile::exists(dbPath) ? "READWRITE" : "CREATE"));
	}

	options.args = parser.positionalArguments();

	return run(options);
}
