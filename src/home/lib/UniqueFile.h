#pragma once

#include <mutex>
#include <set>

#include "fnd/NonCopyMovable.h"
#include "fnd/algorithm.h"

#include "util/bookhash/hashparser.h"

#include "ImageItem.h"
#include "book.h"
#include "util.h"

#include "export/lib.h"

namespace HomeCompa::FliLib
{

struct LIB_EXPORT UniqueFile
{
	struct Uid
	{
		QString folder;
		QString file;
	};

	Uid                  uid;
	QString              hash;
	std::set<QString>    title;
	QString              hashText;
	QStringList          hashSections;
	ImageItem            cover;
	std::set<ImageItem>  images;
	size_t               size;
	uint64_t             simHash;
	std::vector<QString> hist;

	QString GetTitle() const;
	void    ClearImages();
};

class LIB_EXPORT InpDataProvider
{
	NON_COPY_MOVABLE(InpDataProvider)

private:
	struct CacheItem
	{
		QString                sourceLib;
		std::unique_ptr<IDump> dump;
		InpData                inpData;
	};

public:
	explicit InpDataProvider(const QString& dumpWildCards = {});
	~InpDataProvider();

public:
	Book* GetBook(const UniqueFile::Uid& uid) const;
	Book* GetBook(const QString& sourceLib, const QString& libId) const;
	Book* GetBook(const QString& hash) const;
	void  SetSourceLib(const QString& sourceLib);
	void  AddLibToBook(Book* book);
	Book* SetFile(const UniqueFile::Uid& uid, QString id, size_t size);
	bool  Enumerate(std::function<bool(const QString&, const IDump&)> functor) const;
	Book* AddBook(Book* book);
	Book* AddBook(std::unique_ptr<Book> book);

	const std::vector<Book*>& Books() const noexcept;

private:
	InpData  m_stub;
	InpData* m_currentInpData { &m_stub };

	std::vector<CacheItem> m_cache;
	InpData                m_data;
	std::vector<Book*>     m_books;

	std::unordered_map<QString, Book*> m_libIdToBook;
	std::unordered_map<QString, Book*> m_sourceLibIdToBook;
	std::unordered_map<QString, Book*> m_hashToBook;

	const std::unordered_set<QString> m_commonLibFolders { "fb2-000024-030559", "fb2-030560-060423", "fb2-060424-074391", "fb2-074392-091839", "fb2-091841-104214", "fb2-104215-113436",
		                                                   "fb2-113437-119690", "fb2-119691-132107", "fb2-132108-141328", "fb2-141329-147517", "fb2-147519-153549", "fb2-153556-158325",
		                                                   "fb2-158328-161830", "fb2-161831-166042", "fb2-166043-168102", "fb2-168103-172702" };
};

class LIB_EXPORT UniqueFileStorage
{
	struct Dup
	{
		size_t file;
		size_t origin;
	};

public:
	class IDuplicateObserver // NOLINT(cppcoreguidelines-special-member-functions)
	{
	public:
		virtual ~IDuplicateObserver() = default;

		virtual void OnDuplicateFound(const UniqueFile::Uid& file, const UniqueFile::Uid& duplicate) = 0;
	};

	class IUniqueFileConflictResolver // NOLINT(cppcoreguidelines-special-member-functions)
	{
	public:
		virtual ~IUniqueFileConflictResolver() = default;

		virtual bool Resolve(const UniqueFile& file, const UniqueFile& duplicate) const = 0;
	};

	class ImageComparer // NOLINT(cppcoreguidelines-special-member-functions)
	{
	public:
		enum class ImagesCompareResult
		{
			Equal,
			Inner,
			Outer,
			Varied,
		};

	public:
		virtual ~ImageComparer() = default;

		[[nodiscard]] virtual ImagesCompareResult Compare(const UniqueFile& lhs, const UniqueFile& rhs) const = 0;
	};

public:
	explicit UniqueFileStorage(QString dstDir, int hammingThreshold = 10, std::shared_ptr<InpDataProvider> inpDataProvider = std::make_shared<InpDataProvider>());

public:
	std::pair<ImageItem, std::set<ImageItem>> GetImages(UniqueFile& file);
	void                                      SetImages(const QString& hash, const QString& fileName, ImageItem cover, std::set<ImageItem> images);
	UniqueFile*                               Add(QString hash, UniqueFile file);
	std::pair<ImageItems, ImageItems>         GetNewImages();
	void                                      SetDuplicateObserver(std::unique_ptr<IDuplicateObserver> duplicateObserver);
	void                                      SetConflictResolver(std::shared_ptr<IUniqueFileConflictResolver> conflictResolver);

private:
	bool                       CheckForOld(size_t indexDuplicate, size_t indexFile, bool histCheck);
	bool                       CheckForOld(const QString& hash, size_t indexDuplicate, bool histCheck);
	std::optional<UniqueFile*> CheckForNew(const QString& hash, size_t indexDuplicate, bool histCheck);

private:
	const QString                                m_hashDir;
	const std::unique_ptr<const ImageComparer>   m_imageComparer;
	std::mutex                                   m_guard;
	std::shared_ptr<InpDataProvider>             m_inpDataProvider;
	std::unique_ptr<IDuplicateObserver>          m_duplicateObserver;
	std::shared_ptr<IUniqueFileConflictResolver> m_conflictResolver;

	std::multimap<size_t, uint64_t> m_sizeToSimHash;
	std::vector<UniqueFile>         m_files;

	std::unordered_map<QString, std::vector<size_t>>                                 m_old;
	std::unordered_map<QString, std::vector<std::pair<size_t, std::vector<size_t>>>> m_new;

	using SimHashToHash = std::unordered_multimap<uint64_t, QString>;
	SimHashToHash m_oldSimHash;
	SimHashToHash m_newSimHash;

	std::vector<Dup> m_dup;

	const QString m_si;
};

} // namespace HomeCompa::FliLib
