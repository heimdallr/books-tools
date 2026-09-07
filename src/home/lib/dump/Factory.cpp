#include "Factory.h"

#include <fstream>
#include <ranges>
#include <regex>

#include <QDir>
#include <QRegularExpression>

#include "fnd/IsOneOf.h"
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

const IDump::DictionaryTableDescription BOOK {
	.table = "Book",
	.id    = "BookId",
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

std::string GetLine(std::ifstream& inp)
{
	std::string result;
	std::string line;
	while (std::getline(inp, line))
	{
		if (!line.starts_with("INSERT INTO"))
			continue;

		if (line.ends_with(';'))
			return line;

		result = std::move(line);
		break;
	}

	if (result.empty())
		return {};

	while (std::getline(inp, line))
	{
		if (line.starts_with("--"))
			continue;

		result.append(line);
		if (line.ends_with(';'))
			return result;
	}

	return {};
}

void FillTables(DB::IDatabase& db, const std::filesystem::path& path)
{
	std::ifstream inp(path);
	inp.seekg(0, std::ios_base::end);
	const auto size = inp.tellg();
	inp.seekg(0, std::ios_base::beg);

	const auto tr = db.CreateTransaction();

	const std::regex escape(R"(\\(.))"), escapeBack("\x04(.)\x05");

	int64_t currentPercents = 0;
	while (true)
	{
		auto line = GetLine(inp);
		if (line.empty())
			break;

		assert(line.starts_with("INSERT INTO"));

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
		{
			LOGI << path.stem().string() << " " << (currentPercents = percents) << "%";
		}
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
				return QDir::fromNativeSeparators(Platform::PathToString(sqlDir)).contains(QString("/%1/").arg(item.first), Qt::CaseInsensitive);
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

void UpdateImpl(DB::IDatabase& db, const IDump::DictionaryTableDescription& table, const auto& additional)
{
	if (!table.table)
		return;

	std::vector<std::tuple<long long, bool, std::vector<QString>>> values;

	{
		std::string names = table.id;
		for (const auto* name : table.names)
			names.append(", ").append(name);

		PLOGV << "select " << table.table;
		const auto query = db.CreateQuery(std::format("select {} from {}", names, table.table));
		for (query->Execute(); !query->Eof(); query->Next())
		{
			std::vector<QString> value;
			for (size_t i = 1, sz = table.names.size(); i <= sz; ++i)
				value.emplace_back(query->Get<const char*>(i));
			values.emplace_back(query->Get<long long>(0), false, std::move(value));

			PLOGV_IF(values.size() % 50000 == 0) << values.size() << " records selected";
		}
	}
	{
		PLOGV << "update " << table.table;
		for (auto&& [valueItem, n] : std::views::zip(values, std::views::iota(1)))
		{
			auto& [_, changes, value] = valueItem;
			changes                   = additional(value);
			PLOGV_IF(n % 50000 == 0) << n << " records updated";
		}
	}

	const auto tr = db.CreateTransaction();

	const auto allEmpty = [](const auto& items) {
		return std::ranges::all_of(items, [](const auto& item) {
			return item.isEmpty();
		});
	};

	std::vector<long long> toRemove;
	{
		std::string names;
		for (const auto* name : table.names)
			names.append(name).append(" = ?,");
		names.pop_back();

		PLOGV << "write " << table.table;
		const auto sz      = table.names.size();
		const auto command = tr->CreateCommand(std::format("update {} set {} where {} = ?", table.table, names, table.id));
		for (auto&& [valueItem, n] : std::views::zip(
				 values | std::views::filter([&](const auto& item) {
					 return std::get<1>(item);
				 }),
				 std::views::iota(1)
			 ))
		{
			auto& [id, _, value] = valueItem;
			if (allEmpty(value))
			{
				toRemove.emplace_back(id);
				continue;
			}

			for (size_t i = 0; i < sz; ++i)
				command->Bind(i, value[i]);
			command->Bind(sz, id);
			command->Execute();
			PLOGV_IF(n % 50000 == 0) << n << " records written";
		}
	}
	if (!toRemove.empty())
	{
		PLOGV << "delete from " << table.table << "  " << toRemove.size() << " records";
		const auto command = tr->CreateCommand(std::format("delete from {} where {} = ?", table.table, table.id));
		for (auto&& [id, n] : std::views::zip(toRemove, std::views::iota(1)))
		{
			command->Bind(0, id);
			command->Execute();
			PLOGV_IF(n % 50000 == 0) << n << " records deleted";
		}
	}

	tr->Commit();
}

void Append(DB::IDatabase& db, const IDump::LinkTableDescription& tableDescription, DB::IDatabase& dbSource, const QString& tableSource, const QStringList& fieldsSource, const QString& dumpName)
{
	const auto query   = dbSource.CreateQuery(std::format("select {} from {} t join Library l on l.Id = t.LibraryId and l.Name = '{}'", fieldsSource.join(','), tableSource, dumpName));
	const auto tr      = db.CreateTransaction();
	const auto command = tr->CreateCommand(
		std::format(
			"insert or ignore into {}({}) values({})",
			tableDescription.table,
			tableDescription.fields.join(','),
			(tableDescription.fields | std::views::transform([](const auto&) {
				 return QString { "?" };
			 })
	         | std::ranges::to<QStringList>())
				.join(',')
		)
	);

	for (query->Execute(); !query->Eof(); query->Next())
	{
		for (qsizetype i = 0, sz = fieldsSource.size(); i < sz; ++i)
			if (query->IsNull(i))
				command->Bind(i);
			else
				command->Bind(i, query->Get<const char*>(i));
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
	ReplaceImpl(db, BOOK, dump.GetBookTable(), *dbReplacement, dump.GetName(), [](auto&) {
		return false;
	});
	Append(db, dump.GetAuthorLinkTable(), *dbReplacement, "AuthorList", { "t.BookId", "t.Id", "t.Additional" }, dump.GetName());
	Append(db, dump.GetSeriesLinkTable(), *dbReplacement, "SeriesList", { "t.BookId", "t.Id", "t.SeqNumber", "t.Additional" }, dump.GetName());

	const QRegularExpression noClass { QRegularExpression { R"(<p\s*class="{0,1}book"{0,1}>\s*(.*?)\s*<\/p>)" } };
	const QRegularExpression bbCodeToHtml { QRegularExpression { R"(\[(.*?)\]\s*(.*?)\s*\[\/(.*?)\])" } };

	const auto updateAnnotation = [&](std::vector<QString>& strings) {
		assert(!strings.empty());
		auto annotation = strings.front();
		if (annotation.isEmpty())
			return true;

		annotation = annotation.trimmed();

		annotation.removeIf([](const QChar ch) {
			return IsOneOf(ch.category(), QChar::Category::Other_Control, QChar::Other_PrivateUse);
		});

		annotation.replace(noClass, R"(<p>\1</p>)");
		annotation.replace(bbCodeToHtml, R"(<\1>\2</\3>)");

		if (annotation == "<p></p>")
			annotation.clear();

		if (!annotation.isEmpty() && !annotation.startsWith("<p>"))
			annotation.prepend("<p>").append("</p>");

		if (strings.front() == annotation)
			return false;

		strings.front() = annotation;
		return true;
	};
	UpdateImpl(db, dump.GetAnnotationTable(), updateAnnotation);

	PLOGV << "vacuum";
	db.CreateQuery("vacuum")->Execute();
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
