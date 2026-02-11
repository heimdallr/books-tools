#include "database/interface/IDatabase.h"
#include "database/interface/IQuery.h"

#include "IDump.h"
#include "log.h"

namespace HomeCompa::FliLib::Dump
{

namespace
{

constexpr auto g_libavtor = R"(CREATE TABLE libavtor (
  bid INTEGER,
  aid INTEGER,
  role CHAR
))";

constexpr auto g_libavtors = R"(CREATE TABLE libavtors (
  aid INTEGER,
  FirstName VARCHAR(99),
  MiddleName VARCHAR(99),
  LastName VARCHAR(99),
  NickName VARCHAR(33),
  NoDonate CHAR,
  uid INTEGER,
  Email VARCHAR(255),
  Homepage VARCHAR(255),
  Blocked CHAR,
  public CHAR,
  pna VARCHAR(254),
  pnb VARCHAR(254),
  pnc VARCHAR(254),
  pnd VARCHAR(254),
  pnf VARCHAR(254),
  png VARCHAR(254),
  lang VARCHAR(2),
  main INTEGER,
  gender CHAR,
  books INTEGER
))";

constexpr auto g_libbook = R"(CREATE TABLE libbook (
  bid INTEGER,
  FileSize INTEGER,
  Time DATETIME,
  Title VARCHAR(254),
  Title1 VARCHAR(254),
  Lang VARCHAR(2),
  SrcLang VARCHAR(2),
  FileType VARCHAR(4),
  Year INTEGER,
  Year1 INTEGER,
  Deleted CHAR,
  Ver VARCHAR(8),
  FileAuthor VARCHAR(64),
  keywords VARCHAR(255),
  Blocked CHAR,
  md5 VARCHAR(32),
  Broken CHAR,
  Modified DATETIME,
  authors INTEGER,
  ReplacedBy INTEGER,
  Pages INTEGER,
  metaphone VARCHAR(254)
))";

constexpr auto g_libgenre = R"(CREATE TABLE libgenre (
  bid INTEGER,
  gid INTEGER
))";

constexpr auto g_libgenres = R"(CREATE TABLE libgenres (
  gid INTEGER,
  code VARCHAR(45),
  gdesc VARCHAR(99),
  edesc VARCHAR(99),
  gidm INTEGER
))";

constexpr auto g_libmag = R"(CREATE TABLE libmag (
  bid INTEGER,
  mid INTEGER,
  y INTEGER,
  m INTEGER
))";

constexpr auto g_libmags = R"(CREATE TABLE libmags (
  mid INTEGER,
  class VARCHAR(9),
  title VARCHAR(254),
  firstyear INTEGER,
  lastyear INTEGER,
  peryear INTEGER,
  aid INTEGER
))";

constexpr auto g_libpolka = R"(CREATE TABLE libpolka (
  pid INTEGER,
  Time DATETIME,
  bid INTEGER,
  type CHAR,
  uid INTEGER,
  Text TEXT,
  Flag CHAR,
  hastext CHAR
))";

constexpr auto g_libquality = R"(CREATE TABLE libquality (
  bid INTEGER,
  uid INTEGER,
  q CHAR
))";

constexpr auto g_librate = R"(CREATE TABLE librate (
  bid INTEGER,
  uid INTEGER,
  Rate CHAR,
  Time DATETIME
))";

constexpr auto g_libseq = R"(CREATE TABLE libseq (
  bid INTEGER,
  sid INTEGER,
  sn DECIMAL(12,2),
  sort DECIMAL(28,0)
))";

constexpr auto g_libseqs = R"(CREATE TABLE libseqs (
  sid INTEGER,
  seqname VARCHAR(254),
  parent INTEGER,
  nn INTEGER,
  good INTEGER,
  lang VARCHAR(2),
  type CHAR,
  pid INTEGER
))";

constexpr const char* g_commands[] { g_libavtor, g_libavtors, g_libbook, g_libgenre, g_libgenres, g_libmag, g_libmags, g_libpolka, g_libquality, g_librate, g_libseq, g_libseqs };

constexpr const char* g_indices[] {
	"CREATE INDEX ix_libbook_primary_key ON libbook (bid)", "CREATE INDEX ix_libavtor_bid ON libavtor (bid)",           "CREATE INDEX ix_libavtors_primary_key ON libavtors (aid)",
	"CREATE INDEX ix_libgenre_bid ON libgenre (bid)",       "CREATE INDEX ix_libgenres_primary_key ON libgenres (gid)", "CREATE INDEX ix_libseq_bid ON libseq (bid)",
	"CREATE INDEX ix_libseqs_primary_key ON libseqs (sid)", "CREATE INDEX ix_libpolka_time ON libpolka (Time)",
};

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

	void CreateTables(const std::function<void(std::string_view)>& functor) const override
	{
		for (const char* command : g_commands)
			functor(command);
	}

	void CreateIndices(const std::function<void(std::string_view)>& functor) const override
	{
		for (const char* index : g_indices)
			functor(index);
	}

	const DictionaryTableDescription& GetAuthorTable() const noexcept override
	{
		static const DictionaryTableDescription table {
			"libavtors",
			"aid",
			{ "FirstName", "MiddleName", "LastName" }
		};
		return table;
	}

	const DictionaryTableDescription& GetSeriesTable() const noexcept override
	{
		static const DictionaryTableDescription table { "libseqs", "sid", { "seqname" } };
		return table;
	}

	void CreateInpData(const std::function<void(const DB::IQuery&)>& functor) const override
	{
		const auto query = m_db->CreateQuery(R"(
with Books(BookId,   Title,   FileSize, LibID,   Deleted,                                FileType,   Time,   Lang,   Keywords,              Year, LibRateSum , LibRateCount) as (
    select  b.bid, b.Title, b.FileSize, b.bid, b.Deleted, coalesce(nullif(b.FileType, ''), 'fb2'), b.Time, b.Lang, b.keywords, nullif(b.Year, 0), sum(r.Rate), count(r.Rate)
        from libbook b
        left join librate r on r.bid = b.bid
        group by b.bid
)
select
    (select group_concat(
            case when m.rowid is null 
                then n.LastName ||','|| n.FirstName ||','|| n.MiddleName
                else m.LastName ||','|| m.FirstName ||','|| m.MiddleName
            end, ':')||':'
		from libavtor l
		join libavtors n on n.aid = l.aid
		left join libavtors m on m.aid = n.main
		where l.bid = b.BookId and l.role='a'
		order by l.rowid
    ) Author,
    (select group_concat(g.code, ':')||':'
        from libgenres g 
        join libgenre l on l.gid = g.gid and l.bid = b.BookID 
        order by g.gid
    ) Genre,
    b.Title, coalesce(ss.seqname, s.seqname), case when ls.sid is null then null else ls.sn end, null, b.FileSize, b.LibID, b.Deleted, b.FileType, b.Time, b.Lang, b.LibRateSum, b.LibRateCount, b.keywords, b.Year, 0, -ls.sort
from Books b
left join libseq ls on ls.bid = b.BookID
left join libseqs s on s.sid = ls.sid
left join libseqs ss on ss.sid = s.good
)");

		PLOGV << "records selection started";

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
		std::vector<std::pair<int, int>> result;
		const auto                       query = m_db->CreateQuery("select distinct strftime('%Y', p.Time), strftime('%m', p.Time) from libpolka p");
		for (query->Execute(); !query->Eof(); query->Next())
			result.emplace_back(query->Get<int>(0), query->Get<int>(1));
		return result;
	}

	void Review(const int year, const int month, const std::function<void(const QString&, QString, QString, QString)>& functor) const override
	{
		const auto query =
			m_db->CreateQuery(std::format("select p.bid, null, p.Time, p.Text from libpolka p where p.type = 'b' and p.Time BETWEEN '{:04}-{:02}' and '{:04}-{:02}'", year, month, year, month + 1));
		for (query->Execute(); !query->Eof(); query->Next())
			functor(query->Get<const char*>(0), query->Get<const char*>(1), query->Get<const char*>(2), query->Get<const char*>(3));
	}

	void CreateAdditional(const std::filesystem::path& /*dstDir*/, const std::filesystem::path& /*sqlDir*/) const override
	{
	}

private:
	std::unique_ptr<DB::IDatabase> m_db;
	const QString                  m_name { "librusec" };
};

} // namespace

std::unique_ptr<IDump> CreateLibRusEcDatabase()
{
	return std::make_unique<Dump>();
}

} // namespace HomeCompa::FliLib::Dump
