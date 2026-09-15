#include <ranges>

#include <QCoreApplication>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "database/interface/IDatabase.h"

#include "database/factory/Factory.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"
#include "util/bookhash/flihash.h"

#include "log.h"

#include "config/version.h"

using namespace HomeCompa::Util;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flicmp";

void go(const int argc, char* argv[])
{
	const auto db = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag=READONLY", argv[1]));
	if (!db)
		throw std::invalid_argument(std::format("cannot open database{}", argv[1]));

	const auto items = std::views::iota(2, argc) | std::views::filter([](const int n) {
						   return (n & 1) == 0;
					   })
	                 | std::views::transform([&](const int n) {
						   assert(n < argc - 1);
						   auto bookHashItem = GetHash(*db, argv[n], argv[n + 1]);
						   bookHashItem.body.clear();
						   return bookHashItem;
					   })
	                 | std::ranges::to<std::vector<BookHashItem>>();

	for (const auto& item : items | std::views::drop(1))
		PLOGW << Compare(items.front(), item).join('\n');
}

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv);

	Log::LoggingInitializer                    logging(QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID));
	plog::ConsoleAppender<LogConsoleFormatter> consoleAppender;
	Log::LogAppender                           logConsoleAppender(&consoleAppender);

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
