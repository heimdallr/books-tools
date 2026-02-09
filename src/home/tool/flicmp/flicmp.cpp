#include <ranges>

#include <QGuiApplication>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "lib/flihash.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"

#include "log.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flicmp";

void go(const int argc, char* argv[])
{
	const auto items = std::views::iota(1, argc) | std::views::transform([&](const int n) {
						   auto item = QString(argv[n]).split(';', Qt::SkipEmptyParts);
						   assert(item.size() == 2);
						   auto bookHashItem = GetHash(item.front(), item.back());
						   bookHashItem.body.clear();
						   return bookHashItem;
					   })
	                 | std::ranges::to<std::vector<BookHashItem>>();
}

} // namespace

int main(int argc, char* argv[])
{
	const QGuiApplication app(argc, argv);

	Log::LoggingInitializer                          logging(QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);

	try
	{
		PLOGI << "start";
		go(argc, argv);
		return 0;
	}
	catch (const std::exception& ex)
	{
		PLOGE << ex.what();
	}
	catch (...)
	{
		PLOGE << "Unknown error";
	}

	return 1;
}
