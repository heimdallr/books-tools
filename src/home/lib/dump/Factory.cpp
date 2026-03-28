#include "Factory.h"

#include <fstream>
#include <ranges>
#include <regex>

#include <QDir>
#include <QRegularExpression>

#include "fnd/StrUtil.h"

#include "database/interface/ICommand.h"
#include "database/interface/IDatabase.h"
#include "database/interface/IQuery.h"
#include "database/interface/ITransaction.h"

#include "database/factory/Factory.h"

#include "IDump.h"
#include "log.h"

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

const IDump::DictionaryTableDescription AUTHOR {
	.table = "Author",
	.id    = "AuthorId",
	.names = { "FirstName", "MiddleName", "LastName" },
};

const IDump::DictionaryTableDescription SERIES {
	.table = "Series",
	.id    = "SeriesId",
	.names = { "Title" },
};

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

std::unique_ptr<IDump> CreateExists(const std::filesystem::path& sqlDir, const std::filesystem::path& dbPath)
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

void CreateTablesImpl(const IDump& dump, DB::IDatabase& db)
{
	const auto tr = db.CreateTransaction();
	dump.CreateTables([&](const std::string_view command) {
		tr->CreateCommand(command)->Execute();
	});
	tr->CreateCommand("CREATE TABLE Settings(Id VARCHAR(32) NOT NULL PRIMARY KEY, Value BLOB)")->Execute();
	tr->CreateCommand(std::format("INSERT INTO Settings(Id, Value) VALUES('SourceLib', '{}')", dump.GetName()))->Execute();
	tr->Commit();
}

void FillTablesImpl(const std::filesystem::path& sqlDir, const IDump& dump, DB::IDatabase& db)
{
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
		dump.CreateIndices([&](const std::string_view index) {
			PLOGI << index;
			tr->CreateCommand(index)->Execute();
		});
		tr->Commit();
	}
}

using ReplaceDstValues = std::vector<std::pair<long long, std::vector<QString>>>;
using ReplaceSrcValues = std::unordered_map<long long, std::vector<QString>>;

ReplaceSrcValues GetReplaceSrcValues(DB::IDatabase& db, const IDump::DictionaryTableDescription& tableDescription, const QString& dumpName)
{
	ReplaceSrcValues result;

	std::string names;
	for (const auto* name : tableDescription.names)
		names.append(", t.").append(name);

	const auto query = db.CreateQuery(std::format("select t.{}{} from {} t join Library l on l.Id = t.LibraryId and l.Name = '{}'", tableDescription.id, names, tableDescription.table, dumpName));
	for (query->Execute(); !query->Eof(); query->Next())
	{
		auto& values = result.try_emplace(query->Get<long long>(0)).first->second;
		std::ranges::transform(std::views::iota(1, static_cast<int>(query->ColumnCount())), std::back_inserter(values), [&](const auto n) {
			return query->Get<const char*>(n);
		});
	}

	return result;
}

ReplaceDstValues GetReplacedValues(DB::IDatabase& db, const IDump::DictionaryTableDescription& tableDescription)
{
	ReplaceDstValues values;

	std::string names;
	for (const auto* name : tableDescription.names)
		names.append(", ").append(name);

	const auto query = db.CreateQuery(std::format("select {}{} from {}", tableDescription.id, names, tableDescription.table));
	for (query->Execute(); !query->Eof(); query->Next())
	{
		auto& seriesItem = values.emplace_back(query->Get<long long>(0), std::vector<QString> {});
		std::ranges::transform(std::views::zip(tableDescription.names, std::views::iota(1)), std::back_inserter(seriesItem.second), [&](const auto& item) {
			return query->Get<const char*>(std::get<1>(item));
		});
	}

	return values;
}

bool ReplaceImpl(const long long id, std::vector<QString>& value, const ReplaceSrcValues& replacement)
{
	const auto it = replacement.find(id);
	if (it == replacement.end())
		return false;

	value = it->second;
	return true;
}

void ReplaceImpl(
	DB::IDatabase&                           db,
	const IDump::DictionaryTableDescription& tableSrc,
	const IDump::DictionaryTableDescription& tableDst,
	DB::IDatabase&                           dbReplacement,
	const QString&                           dumpName,
	const auto&                              additional
)
{
	const auto values      = GetReplacedValues(db, tableDst);
	const auto replacement = GetReplaceSrcValues(dbReplacement, tableSrc, dumpName);

	const auto  tr = db.CreateTransaction();
	std::string names;
	for (const auto* name : tableDst.names)
		names.append(name).append(" = ?,");
	names.pop_back();
	const auto command = tr->CreateCommand(std::format("update {} set {} where {} = ?", tableDst.table, names, tableDst.id));

	for (const auto& [id, oldValues] : values)
	{
		auto newValues = oldValues | std::views::transform([](const auto& item) {
							 return item.simplified();
						 })
		               | std::ranges::to<std::vector<QString>>();

		ReplaceImpl(id, newValues, replacement) || additional(newValues);

		if (newValues == oldValues)
			continue;

		for (const auto index : std::views::iota(0, static_cast<int>(tableDst.names.size())))
			command->Bind(index, newValues[index].toStdString());

		command->Bind(tableDst.names.size(), id);
		command->Execute();
	}

	tr->Commit();
}

void Append(DB::IDatabase& db, const IDump::LinkTableDescription& tableDescription, DB::IDatabase& dbSource, const QString& tableSource, const QString& dumpName)
{
	const auto query = dbSource.CreateQuery(std::format("select t.Id, t.BookId, t.Additional from {} t join Library l on l.Id = t.LibraryId and l.Name = '{}'", tableSource, dumpName));
	const auto tr    = db.CreateTransaction();
	const auto command =
		tr->CreateCommand(std::format("insert or ignore into {}({}, {}, {}) values(?, ?, ?)", tableDescription.table, tableDescription.objId, tableDescription.bookId, tableDescription.additional));

	for (query->Execute(); !query->Eof(); query->Next())
	{
		command->Bind(0, query->Get<long long>(0));
		command->Bind(1, query->Get<long long>(1));
		command->Bind(2, query->Get<const char*>(2));
		command->Execute();
	}

	tr->Commit();
}

void ReplaceImpl(const std::filesystem::path& replacementPath, const IDump& dump, DB::IDatabase& db)
{
	if (replacementPath.empty())
		return;

	auto dbReplacement = Create(DB::Factory::Impl::Sqlite, std::format("path={};flag={}", replacementPath.string(), "READONLY"));

	const QRegularExpression expressions[] { QRegularExpression { R"(^(.+?)\s*[\(\[]\s*(.+?)\s*[\)\]]\s*(.*?)$)" } };
	const auto               processBrackets = [&](std::vector<QString>& values) {
		for (const auto& expr : expressions)
			if (const auto match = expr.match(values.front()); match.hasMatch())
				values.front() = QString("%1 [%2]%3").arg(match.captured(1), match.captured(2), match.captured(3)).simplified();

		return false;
	};

	const auto removeColon = [](std::vector<QString>& author) {
		for (QString& item : author)
		{
			erase_if(item, [](const QChar ch) {
				return ch == ':';
			});
			item.replace(',', ';');
		}
		return false;
	};

	ReplaceImpl(db, SERIES, dump.GetSeriesTable(), *dbReplacement, dump.GetName(), processBrackets);
	ReplaceImpl(db, AUTHOR, dump.GetAuthorTable(), *dbReplacement, dump.GetName(), removeColon);
	Append(db, dump.GetAuthorLinkTable(), *dbReplacement, "AuthorList", dump.GetName());
}

} // namespace

std::unique_ptr<IDump> Create(const std::filesystem::path& sqlDir, const std::filesystem::path& dbPath, const QString& sourceLib, const std::filesystem::path& replacementPath)
{
	if (exists(dbPath))
		return CreateExists(sqlDir, dbPath);

	auto  dump = CreateImpl(sqlDir, sourceLib);
	auto& db   = dump->SetDatabase(Create(DB::Factory::Impl::Sqlite, std::format("path={};flag={}", dbPath.string(), "CREATE")));

	CreateTablesImpl(*dump, db);
	FillTablesImpl(sqlDir, *dump, db);
	ReplaceImpl(replacementPath, *dump, db);

	return dump;
}

QStringList GetAvailableLibraries()
{
	return LIBRARIES | std::views::keys | std::ranges::to<QStringList>();
}

} // namespace HomeCompa::FliLib::Dump
