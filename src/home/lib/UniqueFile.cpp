#include "UniqueFile.h"

#include <ranges>
#include <unordered_set>

#include <QDir>
#include <QFile>

#include "dump/Factory.h"
#include "dump/IDump.h"
#include "util/files.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "book.h"
#include "log.h"
#include "util.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

class HashParserImpl final : public Util::SaxParser
{
	static constexpr auto BOOKS   = "books";
	static constexpr auto BOOK    = "books/book";
	static constexpr auto COVER   = "books/book/cover";
	static constexpr auto IMAGE   = "books/book/image";
	static constexpr auto ORIGIN  = "books/book/origin";
	static constexpr auto SECTION = "section";

public:
	HashParserImpl(QIODevice& input, HashParser::IObserver& observer)
		: SaxParser(input, 512)
		, m_observer { observer }
	{
		Parse();
	}

private: // Util::SaxParser
	bool OnStartElement(const QString& name, const QString& path, const Util::XmlAttributes& attributes) override
	{
		if (path == BOOKS)
		{
			m_observer.OnParseStarted(attributes.GetAttribute("source"));
		}
		else if (path == BOOK)
		{
#define HASH_PARSER_CALLBACK_ITEM(NAME) m_##NAME = attributes.GetAttribute(#NAME);
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			m_section        = std::make_unique<Section>();
			m_currentSection = m_section.get();
		}
		else if (path == ORIGIN)
		{
			m_originFolder = attributes.GetAttribute(Inpx::FOLDER);
			m_originFile   = attributes.GetAttribute(Inpx::FILE);
		}
		else if (name == SECTION)
		{
			auto& section    = m_currentSection->children.try_emplace(attributes.GetAttribute("id"), std::make_unique<Section>()).first->second;
			section->count   = attributes.GetAttribute("count").toULongLong();
			section->parent  = m_currentSection;
			m_currentSection = section.get();
		}
		else if (path == COVER)
		{
			m_cover.pHash = attributes.GetAttribute("pHash");
		}
		else if (path == IMAGE)
		{
			m_images.emplace_back(attributes.GetAttribute("id"), QString(), attributes.GetAttribute("pHash"));
		}

		return true;
	}

	bool OnEndElement(const QString& name, const QString& path) override
	{
		if (path == BOOK)
		{
			assert(!m_id.isEmpty());
			m_observer.OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) std::move(m_##NAME),
				HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
					std::move(m_cover),
				std::move(m_images),
				std::move(m_section)
			);

#define HASH_PARSER_CALLBACK_ITEM(NAME) m_##NAME = {};
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM

			m_cover          = {};
			m_images         = {};
			m_section        = {};
			m_currentSection = nullptr;
		}
		else if (name == SECTION)
		{
			m_currentSection = m_currentSection->parent;
		}

		return true;
	}

	bool OnCharacters(const QString& path, const QString& value) override
	{
		if (path == COVER)
			m_cover.hash = value;
		else if (path == IMAGE)
			m_images.back().hash = value;
		return true;
	}

private:
	HashParser::IObserver& m_observer;
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString m_##NAME;
	HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM

	HashParser::HashImageItem              m_cover;
	std::vector<HashParser::HashImageItem> m_images;
	Section::Ptr                           m_section;
	Section*                               m_currentSection { nullptr };
};

class ISerializer // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
	virtual ~ISerializer()                                                   = default;
	virtual void Serialize(const UniqueFile& file, const UniqueFile& origin) = 0;
};

class SerializerStub final : public ISerializer
{
public:
	static std::unique_ptr<ISerializer> Create()
	{
		return std::make_unique<SerializerStub>();
	}

private: // ISerializer
	void Serialize(const UniqueFile&, const UniqueFile&) override
	{
	}
};

class Serializer final : public ISerializer
{
public:
	static std::unique_ptr<ISerializer> Create(const QString& fileName)
	{
		if (const auto dir = QFileInfo(fileName).dir(); !dir.exists())
			(void)dir.mkpath(".");

		if (auto file = std::make_unique<QFile>(fileName); file->open(QIODevice::WriteOnly))
			return std::make_unique<Serializer>(std::move(file));

		PLOGE << "Cannot write to " << fileName;
		return SerializerStub::Create();
	}

	explicit Serializer(std::unique_ptr<QIODevice> ioDevice)
		: m_ioDevice { std::move(ioDevice) }
	{
	}

private: // ISerializer
	void Serialize(const UniqueFile& file, const UniqueFile& origin) override
	{
		const auto book = m_booksGuard->Guard("book");
		book->WriteAttribute("id", file.hashText).WriteAttribute("folder", file.uid.folder).WriteAttribute("file", file.uid.file).WriteAttribute("title", file.GetTitle());
		if (!file.cover.fileName.isEmpty())
			book->Guard("cover")->WriteCharacters(file.cover.hash);
		for (const auto& image : file.images)
			book->Guard("image")->WriteCharacters(image.hash);
		if (!origin.uid.file.isEmpty())
			book->Guard("duplicates")->WriteAttribute("folder", origin.uid.folder).WriteAttribute("file", origin.uid.file);

		SerializeHashSections(file.hashSections, m_writer);
	}

private:
	std::unique_ptr<QIODevice>    m_ioDevice;
	Util::XmlWriter               m_writer { *m_ioDevice };
	Util::XmlWriter::XmlNodeGuard m_booksGuard { m_writer.Guard("books") };
};

class DuplicateObserverStub final : public UniqueFileStorage::IDuplicateObserver
{
	void OnDuplicateFound(const UniqueFile::Uid&, const UniqueFile::Uid&) override
	{
	}
};

class UniqueFileConflictResolver final : public UniqueFileStorage::IUniqueFileConflictResolver
{
	bool Resolve(const UniqueFile& file, const UniqueFile& duplicate) const override
	{
		return file.order > duplicate.order;
	}
};

using ImagesCompareResult = UniqueFileStorage::ImageComparer::ImagesCompareResult;

class ImageComparerSub final : public UniqueFileStorage::ImageComparer
{
private: // UniqueFileStorage::ImageComparer
	[[nodiscard]] ImagesCompareResult Compare(const UniqueFile& lhs, const UniqueFile& rhs) const override
	{
		if (!Util::Intersect(lhs.title, rhs.title))
			return ImagesCompareResult::Varied;

		const auto lhsImageCount = lhs.images.size() + !lhs.cover.hash.isEmpty();
		const auto rhsImageCount = rhs.images.size() + !rhs.cover.hash.isEmpty();
		return lhsImageCount < rhsImageCount ? ImagesCompareResult::Inner : lhsImageCount > rhsImageCount ? ImagesCompareResult::Outer : ImagesCompareResult::Equal;
	}
};

class ImageComparerHamming final : public UniqueFileStorage::ImageComparer
{
public:
	explicit ImageComparerHamming(const int threshold)
		: m_threshold { threshold }
	{
	}

private: // UniqueFileStorage::ImageComparer
	[[nodiscard]] ImagesCompareResult Compare(const UniqueFile& lhs, const UniqueFile& rhs) const override
	{
		auto result = ImagesCompareResult::Equal;
		if (lhs.cover.hash != rhs.cover.hash)
		{
			if (lhs.cover.hash.isEmpty())
				result = ImagesCompareResult::Inner;
			else if (rhs.cover.hash.isEmpty())
				result = ImagesCompareResult::Outer;
			else if (lhs.cover.pHash == 0 || rhs.cover.pHash == 0 || std::popcount(lhs.cover.pHash ^ rhs.cover.pHash) > m_threshold)
				return ImagesCompareResult::Varied;
		}

		std::vector<uint64_t>             lpHashes;
		std::unordered_multiset<uint64_t> rpHashes;
		auto                              lIt = lhs.images.cbegin(), rIt = rhs.images.cbegin();
		while (lIt != lhs.images.cend() && rIt != rhs.images.cend())
		{
			if (lIt->hash < rIt->hash)
			{
				lpHashes.emplace_back(lIt->pHash);
				++lIt;
				continue;
			}

			if (lIt->hash > rIt->hash)
			{
				rpHashes.emplace(rIt->pHash);
				++rIt;
				continue;
			}
			++lIt;
			++rIt;
		}

		if (!(lpHashes.empty() || rpHashes.empty()))
		{
			for (; lIt != lhs.images.cend(); ++lIt)
				lpHashes.emplace_back(lIt->pHash);
			for (; rIt != rhs.images.cend(); ++rIt)
				rpHashes.emplace(rIt->pHash);
			erase_if(lpHashes, [&](const uint64_t lItem) {
				if (lItem == 0)
					return false;
				if (const auto it = std::ranges::find_if(
						rpHashes,
						[&](const uint64_t rItem) {
							return std::popcount(lItem ^ rItem) <= m_threshold;
						}
					);
				    it != rpHashes.end())
				{
					rpHashes.erase(it);
					return true;
				}
				return false;
			});
		}

		if (!lpHashes.empty())
			result = result == ImagesCompareResult::Inner ? ImagesCompareResult::Varied : ImagesCompareResult::Outer;

		if (result == ImagesCompareResult::Varied)
			return result;

		if (!rpHashes.empty())
			result = result == ImagesCompareResult::Outer ? ImagesCompareResult::Varied : ImagesCompareResult::Inner;

		if (result == ImagesCompareResult::Varied)
			return result;

		if (!((lhs.images.empty() || rhs.images.empty()) && (lhs.cover.hash.isEmpty() || rhs.cover.hash.isEmpty())))
			return result;

		if (Util::Intersect(lhs.title, rhs.title))
			return result;

		PLOGW << QString("same hash, different titles: %1/%2 %3 vs %4/%5 %6").arg(lhs.uid.folder, lhs.uid.file, lhs.GetTitle(), rhs.uid.folder, rhs.uid.file, rhs.GetTitle());
		return ImagesCompareResult::Varied;
	}

private:
	const int m_threshold;
};

std::unique_ptr<UniqueFileStorage::ImageComparer> GetImageCompared(const int hammingThreshold)
{
	return hammingThreshold >= 64 ? std::unique_ptr<UniqueFileStorage::ImageComparer> { std::make_unique<ImageComparerSub>() } : std::make_unique<ImageComparerHamming>(hammingThreshold);
}

QString createSi()
{
	QString result;
	result.append(QChar { 0x0441 });
	result.append(QChar { 0x0438 });
	return result;
}

} // namespace

QString UniqueFile::GetTitle() const
{
	const QStringList list { title.cbegin(), title.cend() };
	return list.join(' ');
}

void UniqueFile::ClearImages()
{
	cover.body.clear();
	decltype(images) tmp;
	std::ranges::transform(images, std::inserter(tmp, tmp.end()), [](const auto& image) {
		return ImageItem { .hash = image.hash };
	});
	images = std::move(tmp);
}

InpDataProvider::InpDataProvider(const QString& dumpWildCards)
{
	for (const auto& wildCard : dumpWildCards.split(';', Qt::SkipEmptyParts))
		for (const auto& dumpPath : Util::ResolveWildcard(wildCard))
		{
			auto        dump = Dump::Create({}, dumpPath.toStdWString());
			const auto& ref  = *dump;
			m_cache.emplace_back(ref.GetName(), std::move(dump));
		}
}

InpDataProvider::~InpDataProvider() = default;

Book* InpDataProvider::GetBook(const UniqueFile::Uid& uid) const
{
	const auto it = m_data.find(QString("%1#%2").arg(uid.folder, uid.file));
	return it != m_data.end() ? it->second.get() : nullptr;
}

void InpDataProvider::SetSourceLib(const QString& sourceLib)
{
	if (const auto it = std::ranges::find_if(
			m_cache,
			[&](const auto& item) {
				return item.sourceLib.compare(sourceLib, Qt::CaseInsensitive) == 0;
			}
		);
	    it != m_cache.end())
	{
		if (it->inpData.empty())
			it->inpData = CreateInpData(*it->dump);
		m_currentInpData = &it->inpData;
		return;
	}

	m_currentInpData = &m_stub;
}

bool InpDataProvider::Enumerate(std::function<bool(const QString&, const IDump&)> functor) const
{
	return std::ranges::any_of(m_cache, [functor = std::move(functor)](const CacheItem& item) {
		return functor(item.sourceLib.toLower(), *item.dump);
	});
}

Book* InpDataProvider::AddBook(Book* book)
{
	return m_books.emplace_back(book);
}

Book* InpDataProvider::AddBook(std::unique_ptr<Book> book)
{
	auto  key    = book->GetUid();
	auto& result = m_data.try_emplace(std::move(key), std::move(book)).first->second;
	return m_books.emplace_back(result.get());
}

const std::vector<Book*>& InpDataProvider::Books() const noexcept
{
	return m_books;
}

Book* InpDataProvider::SetFile(const UniqueFile::Uid& uid, QString id)
{
	assert(m_currentInpData);
	if (const auto it = m_currentInpData->find(uid.file); it != m_currentInpData->end())
	{
		assert(it->second);
		auto& book = m_data.try_emplace(QString("%1#%2").arg(uid.folder, uid.file), it->second).first->second;
		book->id   = std::move(id);
		return book.get();
	}

	return nullptr;
}

UniqueFileStorage::UniqueFileStorage(QString dstDir, const int hammingThreshold, std::shared_ptr<InpDataProvider> inpDataProvider)
	: m_hashDir { std::move(dstDir) }
	, m_imageComparer { GetImageCompared(hammingThreshold) }
	, m_inpDataProvider { std::move(inpDataProvider) }
	, m_duplicateObserver { std::make_unique<DuplicateObserverStub>() }
	, m_conflictResolver { std::make_unique<UniqueFileConflictResolver>() }
	, m_si { createSi() }
{
	if (m_hashDir.isEmpty())
		return;

	const QDir srcDir(m_hashDir);
	for (const auto& xml : srcDir.entryList({ "*.xml" }, QDir::Filter::Files))
	{
		PLOGV << "parsing " << xml;
		QFile file(srcDir.filePath(xml));
		if (!file.open(QIODevice::ReadOnly))
			continue;

		HashParser::Parse(file, *this);
	}

	PLOGI << "ready books found: " << m_old.size();
}

std::pair<ImageItem, std::set<ImageItem>> UniqueFileStorage::GetImages(UniqueFile& file)
{
	std::lock_guard lock(m_guard);
	return std::make_pair(file.cover, file.images);
}

void UniqueFileStorage::SetImages(const QString& hash, const QString& fileName, ImageItem cover, std::set<ImageItem> images)
{
	std::lock_guard lock(m_guard);
	for (auto [it, end] = m_new.equal_range(hash); it != end; ++it)
	{
		if (it->second.first.uid.file == fileName)
		{
			it->second.first.cover  = std::move(cover);
			it->second.first.images = std::move(images);
			return;
		}
	}
}

UniqueFile* UniqueFileStorage::Add(QString hash, UniqueFile file)
{
	file.title.erase(m_si);

	std::lock_guard lock(m_guard);

	if (m_hashDir.isEmpty())
		return &m_new.emplace(std::move(hash), std::make_pair(std::move(file), std::vector<UniqueFile> {}))->second.first;

	const auto log = [&](const UniqueFile& old) {
		PLOGV << QString("duplicates detected: %1/%2 vs %3/%4, %5").arg(file.uid.folder, file.uid.file, old.uid.folder, old.uid.file, file.GetTitle());
	};

	for (auto [it, end] = m_old.equal_range(hash); it != end; ++it)
	{
		const auto imagesCompareResult = m_imageComparer->Compare(it->second, file);
		if (imagesCompareResult == ImagesCompareResult::Varied)
			continue;

		if (imagesCompareResult == ImagesCompareResult::Inner || (imagesCompareResult == ImagesCompareResult::Equal && file.hash != it->second.hash && m_conflictResolver->Resolve(file, it->second)))
		{
			PLOGW << QString("old duplicate detected by %1/%2: %3/%4, %5").arg(file.uid.folder, file.uid.file, it->second.uid.folder, it->second.uid.file, file.GetTitle());
			continue;
		}

		log(it->second);
		m_duplicateObserver->OnDuplicateFound(it->second.uid, file.uid);
		m_dup.emplace_back(std::move(file), it->second).file.ClearImages();
		return nullptr;
	}

	for (auto [it, end] = m_new.equal_range(hash); it != end; ++it)
	{
		const auto imagesCompareResult = m_imageComparer->Compare(it->second.first, file);
		if (imagesCompareResult == ImagesCompareResult::Varied)
			continue;

		log(it->second.first);

		if (imagesCompareResult == ImagesCompareResult::Outer
		    || (imagesCompareResult == ImagesCompareResult::Equal
		        && (m_conflictResolver->Resolve(it->second.first, file) || (!m_conflictResolver->Resolve(file, it->second.first) && it->second.first.order >= file.order))))
		{
			m_duplicateObserver->OnDuplicateFound(it->second.first.uid, file.uid);
			it->second.second.emplace_back(std::move(file)).ClearImages();
			return nullptr;
		}

		m_duplicateObserver->OnDuplicateFound(file.uid, it->second.first.uid);
		it->second.second.emplace_back(std::move(it->second.first)).ClearImages();
		it->second.first = std::move(file);
		return &it->second.first;
	}

	return &m_new.emplace(std::move(hash), std::make_pair(std::move(file), std::vector<UniqueFile> {}))->second.first;
}

std::pair<ImageItems, ImageItems> UniqueFileStorage::GetNewImages()
{
	ImageItems covers, images;

	for (auto&& [hash, item] : m_new)
	{
		if (!item.first.cover.fileName.isEmpty())
			covers.emplace_back(item.first.cover);
		std::ranges::copy(item.first.images, std::back_inserter(images));
	}

	return std::make_pair(std::move(covers), std::move(images));
}

void UniqueFileStorage::Save(const QString& folder, const bool moveDuplicates)
{
	if (m_new.empty() && m_dup.empty())
		return;

	if (m_hashDir.isEmpty())
		return m_new.clear();

	const QDir dstDir(m_hashDir);
	const auto serializer = Serializer::Create(dstDir.filePath(QString("%1.xml").arg(folder)));

	const auto save = [&](UniqueFile& item, const UniqueFile& origin = {}) {
		serializer->Serialize(item, origin);
		item.hashText.clear();
		item.hashSections.clear();
	};

	const QDir srcDir     = dstDir.filePath(folder);
	const QDir duplicates = srcDir.filePath("duplicates");
	const auto rename     = [&](const QString& fileName) {
        if (!moveDuplicates)
            return;

        if (!duplicates.exists())
            duplicates.mkpath(".");

        [[maybe_unused]] const auto ok = QFile::rename(srcDir.filePath(fileName), duplicates.filePath(fileName));
        assert(ok);
	};

	for (auto&& [hash, newItems] : m_new)
	{
		const auto it = m_old.emplace(hash, std::move(newItems.first));
		it->second.ClearImages();

		save(it->second);

		if (newItems.second.empty())
			continue;

		std::ranges::transform(newItems.second, std::back_inserter(m_dup), [&](auto&& item) {
			rename(item.uid.file);
			return Dup { std::forward<UniqueFile>(item), it->second };
		});
	}

	std::ranges::for_each(m_dup, [&](Dup& dup) {
		rename(dup.file.uid.file);
		save(dup.file, dup.origin);
	});

	m_new.clear();
	m_dup.clear();
}

void UniqueFileStorage::SetDuplicateObserver(std::unique_ptr<IDuplicateObserver> duplicateObserver)
{
	m_duplicateObserver = std::move(duplicateObserver);
}

void UniqueFileStorage::SetConflictResolver(std::shared_ptr<IUniqueFileConflictResolver> conflictResolver)
{
	m_conflictResolver = std::move(conflictResolver);
}

void UniqueFileStorage::OnParseStarted(const QString& sourceLib)
{
	m_inpDataProvider->SetSourceLib(sourceLib);
}

void UniqueFileStorage::OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString NAME,
	HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
		HashParser::HashImageItem          cover,
	std::vector<HashParser::HashImageItem> images,
	Section::Ptr
)
{
	if (!originFolder.isEmpty())
		return;

	decltype(UniqueFile::images) imageItems;
	std::ranges::transform(std::move(images) | std::views::as_rvalue, std::inserter(imageItems, imageItems.end()), [](auto&& item) {
		return ImageItem { .fileName = std::move(item.id), .hash = std::move(item.hash), .pHash = item.pHash.toULongLong(nullptr, 16) };
	});

	const UniqueFile::Uid uid { folder, file };

	if (const auto* book = m_inpDataProvider->SetFile(uid, id))
		title.append(" ").append(book->title);
	SimplifyTitle(PrepareTitle(title));
	auto split = title.split(' ', Qt::SkipEmptyParts);

	UniqueFile uniqueFile {
		.uid      = { .folder = std::move(folder), .file = std::move(file) },
		.hash     = std::move(hash),
		.title    = { std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()) },
		.hashText = id,
		.cover    = { .hash = std::move(cover.hash), .pHash = cover.pHash.toULongLong(nullptr, 16) },
		.images   = std::move(imageItems),
	};
	uniqueFile.order = QFileInfo(uniqueFile.uid.file).baseName().toInt();
	m_old.emplace(std::move(id), std::move(uniqueFile));
}

void HashParser::Parse(QIODevice& input, IObserver& observer)
{
	[[maybe_unused]] const HashParserImpl parser(input, observer);
}
