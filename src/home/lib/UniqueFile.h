#pragma once

#include <set>

#include "fnd/NonCopyMovable.h"
#include "fnd/algorithm.h"

#include "ImageItem.h"
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

	Uid                 uid;
	std::set<QString>   title;
	QString             hashText;
	QStringList         hashSections;
	ImageItem           cover;
	std::set<ImageItem> images;

	int order { 0 };

	QString GetTitle() const;
	void    ClearImages();
};

struct HashParser
{
#define HASH_PARSER_CALLBACK_ITEMS_X_MACRO \
	HASH_PARSER_CALLBACK_ITEM(id)          \
	HASH_PARSER_CALLBACK_ITEM(folder)      \
	HASH_PARSER_CALLBACK_ITEM(file)        \
	HASH_PARSER_CALLBACK_ITEM(title)

	class IObserver // NOLINT(cppcoreguidelines-special-member-functions)
	{
	public:
		virtual ~IObserver()                                  = default;
		virtual void OnParseStarted(const QString& sourceLib) = 0;
		virtual void OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString NAME,
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
				QString cover,
			QStringList images
		) = 0;
	};

	LIB_EXPORT static void Parse(QIODevice& input, IObserver& observer);
};

class LIB_EXPORT InpDataProvider
{
	NON_COPY_MOVABLE(InpDataProvider)
public:
	explicit InpDataProvider(const QString& dumpWildCards = {});
	~InpDataProvider();

public:
	const InpData& GetInpData() const noexcept;
	void           SetSourceLib(const QString& sourceLib);

private:
	const InpData  m_stub;
	const InpData* m_currentInpData { &m_stub };

	std::vector<std::pair<QString, std::unique_ptr<IDump>>> m_dumps;
	std::vector<std::pair<QString, InpData>>                m_cache;
};

class LIB_EXPORT UniqueFileStorage final : HashParser::IObserver
{
	struct Dup
	{
		UniqueFile file;
		UniqueFile origin;
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
		virtual void SetSourceLib(const QString& sourceLib)                             = 0;
	};

public:
	explicit UniqueFileStorage(QString dstDir, std::shared_ptr<InpDataProvider> inpDataProvider = std::make_shared<InpDataProvider>());

public:
	std::pair<ImageItem, std::set<ImageItem>> GetImages(UniqueFile& file);
	void                                      SetImages(const QString& hash, const QString& fileName, ImageItem cover, std::set<ImageItem> images);
	UniqueFile*                               Add(QString hash, UniqueFile file);
	std::pair<ImageItems, ImageItems>         GetNewImages();
	void                                      Save(const QString& folder, bool moveDuplicates);
	void                                      SetDuplicateObserver(std::unique_ptr<IDuplicateObserver> duplicateObserver);
	void                                      SetConflictResolver(std::shared_ptr<IUniqueFileConflictResolver> conflictResolver);

private:
	void OnParseStarted(const QString& sourceLib) override;
	void OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			QString cover,
		QStringList images
	) override;

private:
	const QString                                m_hashDir;
	std::mutex                                   m_guard;
	std::shared_ptr<InpDataProvider>             m_inpDataProvider;
	std::unique_ptr<IDuplicateObserver>          m_duplicateObserver;
	std::shared_ptr<IUniqueFileConflictResolver> m_conflictResolver;

	std::unordered_multimap<QString, UniqueFile> m_old;
	std::vector<Dup>                             m_dup;

	std::unordered_map<std::pair<QString, QString>, std::pair<QString, QString>, Util::PairHash<QString, QString>> m_skip;

	std::unordered_multimap<QString, std::pair<UniqueFile, std::vector<UniqueFile>>> m_new;

	const QString m_si;
};

} // namespace HomeCompa::FliLib
