#include "Factory.h"

#include <fstream>
#include <ranges>
#include <regex>

#include <QDir>
#include <QRegularExpression>

#include "database/interface/ICommand.h"
#include "database/interface/IDatabase.h"
#include "database/interface/IQuery.h"
#include "database/interface/ITransaction.h"

#include "database/factory/Factory.h"

#include "IDump.h"
#include "log.h"
#include "util.h"

//#include "util.h"

namespace HomeCompa::FliLib::Dump
{

#define LIBRARY_ITEMS_X_MACRO \
	LIBRARY_ITEM(Flibusta)    \
	LIBRARY_ITEM(LibRusEc)

#define LIBRARY_ITEM(NAME) std::unique_ptr<IDump> Create##NAME##Database();
LIBRARY_ITEMS_X_MACRO
#undef LIBRARY_ITEM

using Creator = std::unique_ptr<IDump> (*)();
constexpr std::pair<const char*, Creator> LIBRARIES[] {
#define LIBRARY_ITEM(NAME) { #NAME, &Create##NAME##Database },
	LIBRARY_ITEMS_X_MACRO
#undef LIBRARY_ITEM
};

namespace
{

void ReplaceStringInPlace(std::string& subject, const std::string& search, const std::string& replace)
{
	size_t pos = 0;
	while ((pos = subject.find(search, pos)) != std::string::npos)
	{
		subject.replace(pos, search.length(), replace);
		pos += replace.length();
	}
}

void FillTables(DB::IDatabase& db, const std::filesystem::path& path)
{
	//	LOGI << path.string();

	std::ifstream inp(path);
	inp.seekg(0, std::ios_base::end);
	const auto size = inp.tellg();
	inp.seekg(0, std::ios_base::beg);

	const auto  tr = db.CreateTransaction();
	std::string line;

	const std::regex escape(R"(\\(.))"), escapeBack("\x04(.)\x05");

	int64_t currentPercents = 0;
	while (std::getline(inp, line))
	{
		if (!line.starts_with("INSERT INTO"))
			continue;

		ReplaceStringInPlace(line, R"(\\\")", "\"");
		ReplaceStringInPlace(line, R"(\r\n)", "\n");
		ReplaceStringInPlace(line, R"(\\n)", "\n");
		ReplaceStringInPlace(line, R"(\n)", "\n");
		line = std::regex_replace(line, escape, "\x04$1\x05");
		ReplaceStringInPlace(line, "\x04'\x05", "''");
		line                           = std::regex_replace(line, escapeBack, "$1");
		[[maybe_unused]] const auto ok = tr->CreateCommand(line)->Execute();
		assert(ok);
		if (const auto percents = 100 * inp.tellg() / size; percents != currentPercents)
			LOGI << path.stem().string() << " " << (currentPercents = percents) << "%";
	}

	LOGI << path.stem().string() << " " << 100 << "%";

	tr->Commit();
}

std::unique_ptr<IDump> CreateImpl(const std::filesystem::path& sqlDir, const QString& sourceLib)
{
	if (!sourceLib.isNull())
		if (const auto it = std::ranges::find_if(
				LIBRARIES,
				[&](const auto& item) {
					return sourceLib.compare(item.first, Qt::CaseInsensitive) == 0;
				}
			);
		    it != std::end(LIBRARIES))
			return it->second();

	if (const auto it = std::ranges::find_if(
			LIBRARIES,
			[&](const auto& item) {
				return QDir::fromNativeSeparators(QString::fromStdWString(sqlDir)).contains(QString("/%1/").arg(item.first), Qt::CaseInsensitive);
			}
		);
	    it != std::end(LIBRARIES))
		return it->second();

	return LIBRARIES[0].second();
}

} // namespace

std::unique_ptr<IDump> Create(const std::filesystem::path& sqlDir, const std::filesystem::path& dbPath, const QString& sourceLib)
{
	if (exists(dbPath))
	{
		if (is_directory(dbPath))
			throw std::invalid_argument("database path must be a file");

		auto       db    = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag={}", dbPath.string(), "READONLY"));
		const auto query = db->CreateQuery("select Value from Settings where Id='SourceLib'");
		query->Execute();
		assert(!query->Eof());
		auto dump = CreateImpl(sqlDir, query->Get<const char*>(0));
		dump->SetDatabase(std::move(db));
		return dump;
	}

	auto  dump = CreateImpl(sqlDir, sourceLib);
	auto& db   = dump->SetDatabase(Create(DB::Factory::Impl::Sqlite, std::format("path={};flag={}", dbPath.string(), "CREATE")));

	{
		const auto tr = db.CreateTransaction();
		dump->CreateTables([&](const std::string_view command) {
			tr->CreateCommand(command)->Execute();
		});
		tr->CreateCommand("CREATE TABLE Settings(Id VARCHAR(32) NOT NULL PRIMARY KEY, Value BLOB)")->Execute();
		tr->CreateCommand(std::format("INSERT INTO Settings(Id, Value) VALUES('SourceLib', '{}')", dump->GetName()))->Execute();
		tr->Commit();
	}

	std::ranges::for_each(
		std::filesystem::directory_iterator { sqlDir } | std::views::filter([](const auto& entry) {
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
		dump->CreateIndices([&](const std::string_view index) {
			PLOGI << index;
			tr->CreateCommand(index)->Execute();
		});
		tr->Commit();
	}

	{
		std::vector<std::pair<long long, QString>> series;

		const auto& seriesTable = dump->GetSeriesTable();

		{
			const auto query = db.CreateQuery(std::format("select {}, {} from {}", seriesTable.id, seriesTable.name, seriesTable.table));
			for (query->Execute(); !query->Eof(); query->Next())
				series.emplace_back(query->Get<long long>(0), query->Get<const char*>(1));
		}

		const auto tr      = db.CreateTransaction();
		const auto command = tr->CreateCommand(std::format("update {} set {} = ? where {} = ?", seriesTable.table, seriesTable.name, seriesTable.id));

		const QRegularExpression exprs[] { QRegularExpression { R"(^(.+?)\s*[\(\[]\s*(.+?)\s*[\)\]]\s*(.*?)$)" } };

		for (const auto& [id, name] : series)
		{
			QString newName = name.simplified();
			for (const auto& expr : exprs)
				if (const auto match = expr.match(newName); match.hasMatch())
					newName = QString("%1 [%2]%3").arg(match.captured(1), match.captured(2), match.captured(3)).simplified();

			if (newName != name)
			{
				command->Bind(0, newName.toStdString());
				command->Bind(1, id);
				command->Execute();
			}
		}

		tr->Commit();
	}

	return dump;
}

QStringList GetAvailableLibraries()
{
	return LIBRARIES | std::views::keys | std::ranges::to<QStringList>();
}

} // namespace HomeCompa::FliLib::Dump
