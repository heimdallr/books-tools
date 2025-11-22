#include "Factory.h"

#include <ranges>

#include <QDir>

#include "database/interface/ICommand.h"
#include "database/interface/IDatabase.h"
#include "database/interface/ITransaction.h"

#include "database/factory/Factory.h"

#include "IDatabase.h"
#include "log.h"
#include "settings.h"
#include "util.h"

namespace HomeCompa::FliParser::Database
{

#define LIBRARY_ITEMS_X_MACRO \
	LIBRARY_ITEM(Flibusta)

#define LIBRARY_ITEM(NAME) std::unique_ptr<IDatabase> Create##NAME##Database();
LIBRARY_ITEMS_X_MACRO
#undef LIBRARY_ITEM

using Creator = std::unique_ptr<IDatabase> (*)();
constexpr std::pair<const char*, Creator> LIBRARIES[] {
#define LIBRARY_ITEM(NAME) { #NAME, &Create##NAME##Database },
	LIBRARY_ITEMS_X_MACRO
#undef LIBRARY_ITEM
};

namespace
{

std::unique_ptr<IDatabase> CreateImpl(const Settings& settings)
{
	const auto dbPath = settings.outputFolder / (settings.archivesFolder.filename().wstring() + L".db");

	if (!settings.library.isNull())
		if (const auto it = std::ranges::find_if(
				LIBRARIES,
				[&](const auto& item) {
					return settings.library.compare(item.first, Qt::CaseInsensitive) == 0;
				}
			);
		    it != std::end(LIBRARIES))
			return it->second();

	if (const auto it = std::ranges::find_if(
			LIBRARIES,
			[&](const auto& item) {
				return QDir::fromNativeSeparators(QString::fromStdWString(settings.sqlFolder)).contains(QString("/%1/").arg(item.first), Qt::CaseInsensitive);
			}
		);
	    it != std::end(LIBRARIES))
		return it->second();

	return LIBRARIES[0].second();
}

}

std::unique_ptr<IDatabase> Create(const Settings& settings)
{
	auto database = CreateImpl(settings);

	const auto dbPath   = settings.outputFolder / (settings.archivesFolder.filename().wstring() + L".db");
	const auto dbExists = exists(dbPath);
	auto&      db       = database->SetDatabase(Create(DB::Factory::Impl::Sqlite, std::format("path={};flag={}", dbPath.string(), dbExists ? "READONLY" : "CREATE")));
	if (dbExists)
		return database;

	{
		const auto tr = db.CreateTransaction();
		database->CreateTables([&](const std::string_view command) {
			tr->CreateCommand(command)->Execute();
		});
		tr->Commit();
	}

	std::ranges::for_each(
		std::filesystem::directory_iterator { settings.sqlFolder } | std::views::filter([](const auto& entry) {
			return !entry.is_directory();
		}) | std::views::transform([](const auto& entry) {
			return entry.path();
		}) | std::views::filter([](const auto& path) {
			return path.extension() == ".sql";
		}),
		[&](auto path) {
			FillTables(db, path.make_preferred());
		}
	);
	{
		const auto tr = db.CreateTransaction();
		database->CreateIndices([&](const std::string_view index) {
			PLOGI << index;
			tr->CreateCommand(index)->Execute();
		});
		tr->Commit();
	}

	return database;
}

} // namespace HomeCompa::FliParser::Database
