#include <QCommandLineParser>
#include <QCoreApplication>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "lib/dump/Factory.h"
#include "lib/dump/IDump.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"

#include "log.h"

#include "config/version.h"

using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flidumper";

constexpr auto SQL     = "sql";
constexpr auto OUTPUT  = "output";
constexpr auto FOLDER  = "folder";
constexpr auto PATH    = "path";
constexpr auto LIBRARY = "library";

struct Settings
{
	std::filesystem::path sqlDir;
	std::filesystem::path dbPath;
	QString               library;
};

void run(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv); //-V821
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);

	Settings settings {};

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 parses internet book libraries dump").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addOptions({
		{    { "s", SQL }, "Folder with sql files (required)",                             FOLDER },
		{ { "o", OUTPUT },  "Output database path (required)",                               PATH },
		{         LIBRARY,						  "Library", "(Flibusta | LibRusEc) [Flibusta]" },
	});
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	settings.sqlDir  = parser.value(SQL).toStdWString();
	settings.dbPath  = parser.value(OUTPUT).toStdWString();
	settings.library = parser.value(LIBRARY);

	if (settings.sqlDir.empty() || settings.dbPath.empty())
		parser.showHelp(1);

	Log::LoggingInitializer                          logging((parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	const auto dump = FliLib::Dump::Create(settings.sqlDir, settings.dbPath, settings.library);
	dump->CreateAdditional(settings.sqlDir, settings.dbPath.parent_path());
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
