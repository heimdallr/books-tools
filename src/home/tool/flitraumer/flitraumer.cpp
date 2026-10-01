#include <condition_variable>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/StrUtil.h"

#include "database/interface/IDatabase.h"

#include "database/factory/Factory.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"
#include "util/executor/ThreadPool.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/XmlWriter.h"

#include "log.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa::Util;
using namespace HomeCompa;

namespace {

constexpr auto APP_ID = "flitraumer";

constexpr auto PATH     = "path";
constexpr auto DATABASE = "database";
constexpr auto FOLDER   = "folder";

constexpr auto CHUNK_SIZE = 10000;

using Books = std::map<int, std::vector<std::pair<int, std::pair<QString, QString>>>>;

struct Options
{
	QDir                           outputDir;
	std::unique_ptr<DB::IDatabase> database;
};

Books GetBooks(DB::IDatabase& db)
{
	Books      books;
	const auto query = db.CreateQuery("select id, path || fname || '.zip', added from book order by id");
	for (query->Execute(); !query->Eof(); query->Next())
	{
		const auto id = query->Get<int>(0);
		books[id / CHUNK_SIZE * CHUNK_SIZE].emplace_back(id, std::make_pair(query->Get<QString>(1), query->Get<QString>(2)));
	}

	return books;
}

void Process(const QDir& outputDir, const Books::key_type startId, const Books::mapped_type& books)
{
	QString  ext;
	auto     zipFiles = Zip::CreateZipFileController();
	Progress progress(books.size(), "extracting");
	for (const auto& [id, book] : books)
	{
		if (!QFile::exists(book.first))
			continue;

		Zip        zip(book.first);
		const auto files = zip.ReadAll();
		assert(files.size() == 1);

		const auto fileExt = QFileInfo(files.begin()->first).suffix();
		if (ext.isEmpty())
			ext = fileExt;

		QDate date(
			QStringView(book.second.begin(), std::next(book.second.begin(), 4)).toInt(),
			QStringView(std::next(book.second.begin(), 4), std::next(book.second.begin(), 6)).toInt(),
			QStringView(std::next(book.second.begin(), 6), std::next(book.second.begin(), 8)).toInt()
		);
		const auto fileName = QString("%1.%2").arg(id).arg(fileExt);
		zipFiles->AddFile(fileName, files.begin()->second, QDateTime(date, QTime {}));
		progress.Increment(1, fileName.toStdString());
	}
	if (ext.isEmpty())
		return;

	const auto outputPath = outputDir.filePath(QString("t.%1-%2-%3.zip").arg(ext).arg(startId, 6, 10, QChar { '0' }).arg(startId + CHUNK_SIZE - 1, 6, 10, QChar { '0' }));

	PLOGI << "archive " << outputPath;
	Zip zip(outputPath, Zip::Format::Zip);
	zip.Write(*zipFiles);
}

int run(const Options& options)
{
	try
	{
		if (!options.outputDir.exists() && !options.outputDir.mkpath("."))
			throw std::ios_base::failure(std::format("Cannot create folder {}", options.outputDir.path()));

		for (const auto& [startId, books] : GetBooks(*options.database))
			Process(options.outputDir, startId, books);

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

	Options options;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 creates hash files for library").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addOptions(
		{
			{                    { "o", FOLDER },        "Output folder (required)", FOLDER },
			{ { QString(DATABASE[0]), DATABASE }, "Output database path (required)",   PATH },
	}
	);
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	const Log::LoggingInitializer logging(parser.value(logOption));
	const auto       consoleAppender(parser.value(logOption) != Log::LoggingInitializer::CONSOLE ? std::make_unique<plog::ConsoleAppender<Util::LogConsoleFormatter>>() : std::unique_ptr<plog::IAppender> {});
	Log::LogAppender logConsoleAppender(consoleAppender.get());

	PLOGI << QString("%1 started").arg(APP_ID);

	options.outputDir = QDir { parser.value(FOLDER) };
	if (!parser.isSet(DATABASE) || !parser.isSet(FOLDER))
		parser.showHelp(1);

	if (parser.isSet(DATABASE))
	{
		const auto dbPath = parser.value(DATABASE);
		options.database  = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag={}", dbPath, QFile::exists(dbPath) ? "READWRITE" : "CREATE"));
	}

	return run(options);
}
