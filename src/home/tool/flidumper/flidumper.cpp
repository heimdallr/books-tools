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
	QString               logPath { QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID) };
};

void run(const Settings& settings)
{
	const auto dump = FliLib::Dump::Create(settings.sqlDir, settings.dbPath, settings.library);
	dump->CreateAdditional(settings.sqlDir, settings.dbPath.parent_path());
}

Settings parseCommandLine(const QCoreApplication& app)
{
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
	const auto logOption = Log::LoggingInitializer::AddLogFileOption(parser, settings.logPath);
	parser.process(app);

	settings.sqlDir  = parser.value(SQL).toStdWString();
	settings.dbPath  = parser.value(OUTPUT).toStdWString();
	settings.library = parser.value(LIBRARY);

	if (settings.sqlDir.empty() || settings.dbPath.empty())
		parser.showHelp(1);

	if (parser.isSet(logOption))
		settings.logPath = parser.value(logOption);

	return settings;
}

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);

	const auto                                       settings = parseCommandLine(app);
	Log::LoggingInitializer                          logging(settings.logPath.toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	try
	{
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
