#include <QCryptographicHash>

#include <ranges>
#include <set>

#include <QBuffer>
#include <QFileInfo>

#include "fnd/ScopedCall.h"

#include "database/interface/IDatabase.h"
#include "database/interface/IQuery.h"

#include "util/executor/ThreadPool.h"
#include "util/language.h"

#include "Constant.h"
#include "IDump.h"
#include "log.h"
#include "util.h"
#include "zip.h"

namespace HomeCompa::FliLib::Dump
{

namespace
{

constexpr auto g_libaannotations = R"(CREATE TABLE libaannotations (
  AvtorId INTEGER,
  nid INTEGER,
  Title VARCHAR(255),
  Body TEXT
))";

constexpr auto g_libapics = R"(CREATE TABLE libapics (
  AvtorId INTEGER,
  nid INTEGER,
  File VARCHAR(255)
))";

constexpr auto g_libbannotations = R"(CREATE TABLE libbannotations (
  BookId INTEGER,
  nid INTEGER,
  Title VARCHAR(255),
  Body TEXT
))";

constexpr auto g_libbpics = R"(CREATE TABLE libbpics (
  BookId INTEGER,
  nid INTEGER,
  File VARCHAR(255)
))";

constexpr auto g_libavtor = R"(CREATE TABLE libavtor (
  BookId INTEGER,
  AvtorId INTEGER,
  Pos INTEGER
))";

constexpr auto g_libavtorname = R"(CREATE TABLE libavtorname (
  AvtorId INTEGER,
  FirstName VARCHAR(99),
  MiddleName VARCHAR(99),
  LastName VARCHAR(99),
  NickName VARCHAR(33),
  uid INTEGER,
  Email VARCHAR(255),
  Homepage VARCHAR(255),
  Gender CHAR,
  MasterId INTEGER
))";

constexpr auto g_libbook = R"(CREATE TABLE libbook (
  BookId INTEGER,
  FileSize INTEGER,
  Time DATETIME,
  Title VARCHAR(254),
  Title1 VARCHAR(254),
  Lang VARCHAR(3),
  LangEx INTEGER,
  SrcLang VARCHAR(3),
  FileType VARCHAR(4),
  Encoding VARCHAR(32),
  Year INTEGER,
  Deleted VARCHAR(1),
  Ver VARCHAR(8),
  FileAuthor VARCHAR(64),
  N INTEGER,
  keywords VARCHAR(255),
  md5 VARCHAR(32),
  Modified DATETIME,
  pmd5 VARCHAR(32),
  InfoCode INTEGER,
  Pages INTEGER,
  Chars INTEGER
))";

constexpr auto g_libfilename = R"(CREATE TABLE libfilename (
  BookId INTEGER,
  FileName VARCHAR(255)
))";

constexpr auto g_libgenre = R"(CREATE TABLE libgenre (
  Id INTEGER,
  BookId INTEGER,
  GenreId INTEGER
))";

constexpr auto g_libgenrelist = R"(CREATE TABLE libgenrelist (
  GenreId INTEGER,
  GenreCode VARCHAR(45),
  GenreDesc VARCHAR(99),
  GenreMeta VARCHAR(45)
))";

constexpr auto g_libjoinedbooks = R"(CREATE TABLE libjoinedbooks (
  Id INTEGER,
  Time DATETIME,
  BadId INTEGER,
  GoodId INTEGER,
  realId INTEGER
))";

constexpr auto g_librate = R"(CREATE TABLE librate (
  ID INTEGER,
  BookId INTEGER,
  UserId INTEGER,
  Rate CHAR
))";

constexpr auto g_librecs = R"(CREATE TABLE librecs (
  id INTEGER,
  uid INTEGER,
  bid INTEGER,
  timestamp DATETIME
))";

constexpr auto g_libseq = R"(CREATE TABLE libseq (
  BookId INTEGER,
  SeqId INTEGER,
  SeqNumb INTEGER,
  Level INTEGER,
  Type INTEGER
))";

constexpr auto g_libseqname = R"(CREATE TABLE libseqname (
  SeqId INTEGER,
  SeqName VARCHAR(254)
))";

constexpr auto g_libtranslator = R"(CREATE TABLE libtranslator (
  BookId INTEGER,
  TranslatorId INTEGER,
  Pos INTEGER
))";

constexpr auto g_libreviews = R"(CREATE TABLE libreviews (
  Name VARCHAR(255),
  Time DATETIME,
  BookId INTEGER,
  Text TEXT
))";

constexpr const char* g_indices[] {
	"CREATE INDEX ix_libavtor_BookID_Pos ON libavtor (BookId, Pos)",
	"CREATE INDEX ix_libavtor_AvtorID ON libavtor (AvtorId)",
	"CREATE INDEX ix_libavtorname_primary_key ON libavtorname (AvtorId)",
	"CREATE INDEX ix_libbook_primary_key ON libbook (BookId)",
	"CREATE INDEX ix_libfilename_primary_key ON libfilename (BookId)",
	"CREATE INDEX ix_libgenre_BookID ON libgenre (BookId)",
	"CREATE INDEX ix_libgenre_GenreID ON libgenre (GenreId)",
	"CREATE INDEX ix_libgenrelist_primary_key ON libgenrelist (GenreId)",
	"CREATE INDEX ix_librate_BookID ON librate (BookId)",
	"CREATE INDEX ix_libseq_BookID ON libseq (BookId)",
	"CREATE INDEX ix_libseq_SeqID ON libseq (SeqId)",
	"CREATE INDEX ix_libseqname_primary_key ON libseqname (SeqId)",
	"CREATE INDEX ix_libreviews_Time ON libreviews (Time)",
	"CREATE INDEX ix_libaannotations_nid ON libaannotations (nid)",
	"CREATE INDEX ix_libapics_AvtorId ON libapics (AvtorId)",
	"delete from libseq where not exists(select 42 from libseqname where libseqname.SeqId = libseq.SeqId)",
};

constexpr const char* g_commands[] { g_libaannotations, g_libapics,       g_libbannotations, g_libbpics, g_libavtor, g_libavtorname, g_libbook,       g_libfilename, g_libgenre,
	                                 g_libgenrelist,    g_libjoinedbooks, g_librate,         g_librecs,  g_libseq,   g_libseqname,   g_libtranslator, g_libreviews };

std::vector<std::tuple<int, QByteArray, QByteArray>> CreateAuthorAnnotationsData(DB::IDatabase& db, const std::filesystem::path& sqlPath)
{
	auto threadPool = std::make_unique<Util::ThreadPool>();

	int currentId     = -1;
	using PictureList = std::set<QString>;
	std::map<QString, std::pair<QString, PictureList>> data;

	std::mutex                                           archivesGuard;
	std::vector<std::tuple<int, QByteArray, QByteArray>> archives;

	std::mutex                  picsGuard, zipGuard;
	std::unique_ptr<Zip>        pics;
	std::unordered_set<QString> picsFiles;
	if (const auto picsArchiveName = sqlPath / "lib.a.attached.zip"; exists(picsArchiveName))
	{
		pics       = std::make_unique<Zip>(QString::fromStdWString(picsArchiveName));
		auto files = pics->GetFileNameList();
		std::unordered_set(std::make_move_iterator(std::begin(files)), std::make_move_iterator(std::end(files))).swap(picsFiles);
	}

	const auto write = [&](const int id) {
		ScopedCall idGuard([&] {
			currentId = id;
			data.clear();
		});
		if (currentId < 0)
			return;

		auto dataCopy = std::move(data);
		data          = {};

		threadPool->enqueue([&archivesGuard, &archives, &picsGuard, &zipGuard, &pics, &picsFiles, currentId, data = std::move(dataCopy)]() mutable {
			size_t pictureCount = 0;

			const ScopedCall logGuard(
				[currentId, authorsCount = data.size()] {
					PLOGI << "Authors pack " << currentId << " started, authors: " << authorsCount;
				},
				[currentId, &pictureCount] {
					PLOGI << "Authors pack " << currentId << " finished, pictures: " << pictureCount;
				}
			);

			QByteArray annotation;

			{
				auto zipFiles = Zip::CreateZipFileController();
				std::ranges::for_each(data, [&](auto& value) {
					value.second.first.prepend(' ');
					value.second.first.append(' ');
					zipFiles->AddFile(value.first, ReplaceTags(value.second.first).simplified().toUtf8());
				});

				QBuffer          buffer(&annotation);
				const ScopedCall bufferGuard(
					[&] {
						buffer.open(QIODevice::WriteOnly);
					},
					[&] {
						buffer.close();
					}
				);
				Zip zip(buffer, Zip::Format::SevenZip);
				zip.SetProperty(ZipDetails::PropertyId::SolidArchive, false);
				zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
				zip.Write(std::move(zipFiles));
			}

			QByteArray pictures;

			if (pics)
			{
				auto zipFiles = Zip::CreateZipFileController();
				for (const auto& [dstFolder, values] : data)
				{
					std::unordered_set<QString> uniqueFiles;
					for (const auto& file : values.second)
					{
						if (!picsFiles.contains(file))
							continue;

						const auto fileSplit = file.split('/', Qt::SkipEmptyParts);
						if (fileSplit.size() != 3)
							continue;

						if (uniqueFiles.insert(fileSplit.back()).second)
						{
							std::lock_guard lock(picsGuard);
							auto            picBody = pics->Read(file)->GetStream().readAll();
							if (picBody.isEmpty())
								PLOGW << fileSplit.join("/") << " is empty";
							else
								zipFiles->AddFile(QString("%1/%2").arg(dstFolder, fileSplit.back()), std::move(picBody), pics->GetFileTime(file));
						}
					}
				}

				pictureCount = zipFiles->GetCount();

				QBuffer          buffer(&pictures);
				const ScopedCall bufferGuard(
					[&] {
						buffer.open(QIODevice::WriteOnly);
					},
					[&] {
						buffer.close();
					}
				);

				std::lock_guard zipLock(zipGuard);
				Zip             zip(buffer, Zip::Format::Zip);
				zip.Write(std::move(zipFiles));
			}

			std::lock_guard lock(archivesGuard);
			archives.emplace_back(currentId, std::move(annotation), std::move(pictures));
		});
	};

	const auto query = db.CreateQuery(R"(
select 
    n.nid / 10000, a.LastName || ' ' || a.FirstName || ' ' || a.MiddleName, n.Body, p.File
from libaannotations n 
join libavtorname a on a.AvtorId = n.AvtorId 
left join libapics p on p.AvtorId = n.AvtorId
order by n.nid
)");
	for (query->Execute(); !query->Eof(); query->Next())
	{
		if (const auto id = query->Get<int>(0); id != currentId)
			write(id);

		QCryptographicHash hash(QCryptographicHash::Algorithm::Md5);
		hash.addData(QString(query->Get<const char*>(1)).split(' ', Qt::SkipEmptyParts).join(' ').toLower().simplified().toUtf8());
		auto& files = data.try_emplace(hash.result().toHex(), std::make_pair(QString(query->Get<const char*>(2)), PictureList {})).first->second.second;
		if (const auto* file = query->Get<const char*>(3))
			files.insert(file);
	}

	write(currentId);
	threadPool.reset();

	return archives;
}

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
			"libavtorname",
			"AvtorId",
			{ "FirstName", "MiddleName", "LastName" }
		};
		return table;
	}

	const DictionaryTableDescription& GetSeriesTable() const noexcept override
	{
		static const DictionaryTableDescription table { "libseqname", "SeqId", { "SeqName" } };
		return table;
	}

	void CreateInpData(const std::function<void(const DB::IQuery&)>& functor) const override
	{
		const auto query = m_db->CreateQuery(R"(
with Books(  BookId,   Title,   FileSize,   LibID,    Deleted,                                FileType,   Time,   Lang,   Keywords, Year,              LibRateSum , LibRateCount) as (
    select b.BookId, b.Title, b.FileSize, b.BookId, b.Deleted, coalesce(nullif(b.FileType, ''), 'fb2'), b.Time, b.Lang, b.keywords, nullif(b.Year, 0), sum(r.Rate), count(r.Rate)
        from libbook b
        left join librate r on r.BookID = b.BookId
        group by b.BookId
)
select
    (select group_concat(
            case when m.rowid is null 
                then n.LastName ||','|| n.FirstName ||','|| n.MiddleName
                else m.LastName ||','|| m.FirstName ||','|| m.MiddleName
            end, ':')||':'
		from libavtor l
		join libavtorname n on n.AvtorId = l.AvtorId
		left join libavtorname m on m.AvtorID = n.MasterId
		where l.BookId = b.BookID 
			and (n.NickName != 'иллюстратор' or not exists (
				select 42 
				from libavtor ll
				join libavtorname nn on nn.AvtorId = ll.AvtorId and nn.NickName != 'иллюстратор'
				where ll.BookId = l.BookId )
			)
		order by l.Pos
    ) Author,
    (select group_concat(g.GenreCode, ':')||':'
        from libgenrelist g 
        join libgenre l on l.GenreId = g.GenreId and l.BookID = b.BookID 
        order by g.GenreID
    ) Genre,
    b.Title, s.SeqName, case when s.SeqId is null then null else ls.SeqNumb end, f.FileName, b.FileSize, b.LibID, b.Deleted, b.FileType, b.Time, b.Lang, b.LibRateSum, b.LibRateCount, b.keywords, b.Year, ls.Type, ls.Level
from Books b
left join libseq ls on ls.BookID = b.BookID
left join libseqname s on s.SeqID = ls.SeqID
left join libfilename f on f.BookId=b.BookID
)");

		PLOGV << "records selection started";

		for (query->Execute(); !query->Eof(); query->Next())
			functor(*query);
	}

	std::vector<std::pair<int, int>> GetReviewMonths() const override
	{
		std::vector<std::pair<int, int>> result;
		const auto                       query = m_db->CreateQuery("select distinct strftime('%Y', r.Time), strftime('%m', r.Time) from libreviews r");
		for (query->Execute(); !query->Eof(); query->Next())
			result.emplace_back(query->Get<int>(0), query->Get<int>(1));
		return result;
	}

	void Review(const int year, const int month, const std::function<void(const QString&, QString, QString, QString)>& functor) const override
	{
		const auto query = m_db->CreateQuery(std::format("select r.BookId, r.Name, r.Time, r.Text from libreviews r where r.Time BETWEEN '{:04}-{:02}' and '{:04}-{:02}'", year, month, year, month + 1));
		for (query->Execute(); !query->Eof(); query->Next())
			functor(query->Get<const char*>(0), query->Get<const char*>(1), query->Get<const char*>(2), query->Get<const char*>(3));
	}

	void CreateAdditional(const std::filesystem::path& sqlDir, const std::filesystem::path& dstDir) const override
	{
		CreateAuthorAnnotations(sqlDir, dstDir);
	}

private:
	void CreateAuthorAnnotations(const std::filesystem::path& sqlDir, const std::filesystem::path& dstDir) const
	{
		PLOGI << "write author annotations";

		const auto authorsFolder = dstDir / Inpx::AUTHORS_FOLDER;
		create_directory(authorsFolder);

		const auto authorImagesFolder = authorsFolder / Global::PICTURES;
		create_directory(authorImagesFolder);

		const auto write = [](const std::filesystem::path& path, const int id, const QString& ext, const QByteArray& data) {
			if (data.isEmpty())
				return;

			const auto archiveName = QString::fromStdWString(path / std::to_string(id)) + ext;
			if (const auto archivePath = std::filesystem::path(archiveName.toStdWString()); exists(archivePath))
				remove(archivePath);

			Write(archiveName, data);
		};

		for (const auto& [id, annotation, images] : CreateAuthorAnnotationsData(*m_db, sqlDir))
		{
			write(authorsFolder, id, ".7z", annotation);
			write(authorImagesFolder, id, ".zip", images);
		}
	}

private:
	std::unique_ptr<DB::IDatabase> m_db;
	const QString                  m_name { "flibusta" };
};

} // namespace

std::unique_ptr<IDump> CreateFlibustaDatabase()
{
	return std::make_unique<Dump>();
}

} // namespace HomeCompa::FliLib::Dump
