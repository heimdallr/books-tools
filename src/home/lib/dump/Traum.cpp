#include "database/interface/IDatabase.h"
#include "database/interface/IQuery.h"

#include "IDump.h"
#include "log.h"

namespace HomeCompa::FliLib::Dump {

namespace {

class Dump final : public IDump
{
private: // IDatabase
	const QString& GetName() const noexcept override
	{
		return m_name;
	}

	DB::IDatabase& SetDatabase(std::unique_ptr<DB::IDatabase> db) noexcept override
	{
		m_db = std::move(db);
		return *m_db;
	}

	void CreateTables(const std::function<void(std::string_view)>& /*functor*/) const override
	{
	}

	void CreateIndices(const std::function<void(std::string_view)>& /*functor*/) const override
	{
	}

	const DictionaryTableDescription& GetAuthorTable() const noexcept override
	{
		static const DictionaryTableDescription table {
			"author_full",
			"id",
			{ "firstname", "middlename", "lastname" }
		};
		return table;
	}

	const DictionaryTableDescription& GetSeriesTable() const noexcept override
	{
		static const DictionaryTableDescription table { "series", "id", { "name" } };
		return table;
	}

	const DictionaryTableDescription& GetBookTable() const noexcept override
	{
		static const DictionaryTableDescription table { "book", "id", { "name" } };
		return table;
	}

	const DictionaryTableDescription& GetAnnotationTable() const noexcept override
	{
		static const DictionaryTableDescription table {};
		return table;
	}

	const LinkTableDescription& GetAuthorLinkTable() const noexcept override
	{
		static const LinkTableDescription table {};
		return table;
	}

	const LinkTableDescription& GetSeriesLinkTable() const noexcept override
	{
		static const LinkTableDescription table {};
		return table;
	}

	void CreateInpData(const std::function<void(const DB::IQuery&)>& functor) const override
	{
		const auto query = m_db->CreateQuery(R"(
select
coalesce( 
    (select group_concat(trim(n.lastname) ||','|| trim(coalesce(n.firstname, '')) ||','|| trim(coalesce(n.middlename, '')), ':')
            from author_full n
            join authorlink l on l.linked = n.id and l.author = b.author)
    , trim(a.lastname) ||','|| trim(coalesce(a.firstname, '')) ||','|| trim(coalesce(a.middlename, ''))
) || ':' Author
, (select group_concat(t.name, ':') from booktags t where t.id = b.id) || ':' Genre
, b.name Title
, replace(trim(iif(instr(s.name, '\') <= 0, s.name, replace(s.name, rtrim(s.name, replace(s.name, '\', '')), ''))), '_', '') SeriesTitle
, iif(s.name is null, null, nullif(b.number, 0)) SeqNum, null FileName, b.size FileSize, b.id LibId, 0 Del
, replace(b.fname, rtrim(b.fname, replace(b.fname, '.', '')), '') ext
, substr(b.added, 1, 4) || '-' || substr(b.added, 5, 2) || '-' || substr(b.added, 7, 2)
, lower(b.lang), null, null, null, nullif(b.year, 0), b.crc32, 0, 0
from book b
join series s on s.id = b.series
left join author_full a on a.id = b.author
)");

		PLOGV << GetName() << " records selection started";

		for (query->Execute(); !query->Eof(); query->Next())
			functor(*query);
	}

	//	void Review(const std::function<void(const QString&, QString, QString, QString)>& functor) const override
	//	{
	//		const auto query = m_db->CreateQuery("select p.bid, null, p.Time, p.Text from libpolka p where p.type = 'b'");
	//		for (query->Execute(); !query->Eof(); query->Next())
	//			functor(query->Get<const char*>(0), query->Get<const char*>(1), query->Get<const char*>(2), query->Get<const char*>(3));
	//	}

	std::vector<std::pair<int, int>> GetReviewMonths() const override
	{
		return {};
	}

	void Review(const int /*year*/, const int /*month*/, const std::function<void(const QString&, QString, QString, QString)>& /*functor*/) const override
	{
	}

	void CreateAdditional(
		const std::filesystem::path& /*dstDir*/,
		const std::filesystem::path& /*sqlDir*/,
		const AdditionalType additionalType,
		const std::function<void(const DB::IQuery&)>& functor
	) const override
	{
		if (!!(additionalType & AdditionalType::Annotation))
			CreateBookAnnotations(functor);
	}

private:
	void CreateBookAnnotations(const std::function<void(const DB::IQuery&)>& functor) const
	{
		const auto query = m_db->CreateQuery("select id, text from bookanno");
		for (query->Execute(); !query->Eof(); query->Next())
			functor(*query);
	}


private:
	std::unique_ptr<DB::IDatabase> m_db;
	const QString                  m_name { "traum" };
};

} // namespace

std::unique_ptr<IDump> CreateTraumDatabase()
{
	return std::make_unique<Dump>();
}

} // namespace HomeCompa::FliLib::Dump
