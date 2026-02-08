#include "Factory.h"

#include <fstream>
#include <ranges>
#include <regex>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "database/interface/ICommand.h"
#include "database/interface/IDatabase.h"
#include "database/interface/IQuery.h"
#include "database/interface/ITransaction.h"

#include "database/factory/Factory.h"

#include "IDump.h"
#include "log.h"
#include "util.h"

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

ReplaceSrcValues GetReplaceSrcValues(const QJsonObject& obj, const IDump::DictionaryTableDescription& tableDescription)
{
	ReplaceSrcValues result;
	const auto       valuesVar = obj[tableDescription.table];
	if (!valuesVar.isObject())
		return result;

	const auto valuesObj = valuesVar.toObject();

	for (auto it = valuesObj.constBegin(), end = valuesObj.constEnd(); it != end; ++it)
	{
		auto& values = result.try_emplace(it.key().toLongLong()).first->second;
		if (it.value().isString())
		{
			values.push_back(it.value().toString());
			continue;
		}

		assert(it.value().isObject());
		const auto valueObj = it.value().toObject();
		std::ranges::transform(tableDescription.names, std::back_inserter(values), [&](const auto& name) {
			return valueObj[name].toString();
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

void ReplaceImpl(DB::IDatabase& db, const IDump::DictionaryTableDescription& tableDescription, const QJsonObject& replacementObj, const auto& additional)
{
	const auto values      = GetReplacedValues(db, tableDescription);
	const auto replacement = GetReplaceSrcValues(replacementObj, tableDescription);

	const auto  tr = db.CreateTransaction();
	std::string names;
	for (const auto* name : tableDescription.names)
		names.append(name).append(" = ?,");
	names.pop_back();
	const auto command = tr->CreateCommand(std::format("update {} set {} where {} = ?", tableDescription.table, names, tableDescription.id));

	for (const auto& [id, oldValues] : values)
	{
		auto newValues = oldValues | std::views::transform([](const auto& item) {
							 return item.simplified();
						 })
		               | std::ranges::to<std::vector<QString>>();

		ReplaceImpl(id, newValues, replacement) || additional(newValues);

		if (newValues == oldValues)
			continue;

		for (const auto index : std::views::iota(0, static_cast<int>(tableDescription.names.size())))
			command->Bind(index, newValues[index].toStdString());

		command->Bind(tableDescription.names.size(), id);
		command->Execute();
	}

	tr->Commit();
}

void ReplaceImpl(const std::filesystem::path& replacementPath, const IDump& dump, DB::IDatabase& db)
{
	auto doc = [&]() -> QJsonDocument {
		if (replacementPath.empty())
			return {};

		QFile file(replacementPath);
		if (!file.open(QIODevice::ReadOnly))
			throw std::invalid_argument(std::format("cannot open {}", replacementPath));

		const auto result = QJsonDocument::fromJson(file.readAll());
		if (!result.isObject())
			throw std::invalid_argument(std::format("{} must be the object json", replacementPath));

		return result;
	}();

	const auto replacementObj = doc.object();

	const auto dash = QString(" %1 ").arg(QChar { 0x2013 });

	const QRegularExpression expressions[] { QRegularExpression { R"(^(.+?)\s*[\(\[]\s*(.+?)\s*[\)\]]\s*(.*?)$)" } };
	const auto               processBrackets = [&](std::vector<QString>& values) {
        for (const auto& expr : expressions)
            if (const auto match = expr.match(values.front()); match.hasMatch())
                values.front() = QString("%1 [%2]%3").arg(match.captured(1), match.captured(2), match.captured(3)).simplified();

        std::ranges::transform(values.front(), values.front().begin(), [](const QChar ch) {
            return ch >= QChar { 0x2010 } && ch <= QChar { 0x2015 } ? QChar { '-' } : ch;
        });

        values.front().replace(" - ", dash);

        return false;
	};

	const auto removeColon = [](std::vector<QString>& author) {
		for (QString& item : author)
			erase_if(item, [](const QChar ch) {
				return ch == ':';
			});
		return false;
	};

	ReplaceImpl(db, dump.GetSeriesTable(), replacementObj, processBrackets);
	ReplaceImpl(db, dump.GetAuthorTable(), replacementObj, removeColon);
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
