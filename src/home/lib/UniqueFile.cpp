#include "UniqueFile.h"

#include <ranges>
#include <unordered_set>

#include <QDir>
#include <QFile>

#include "dump/Factory.h"
#include "dump/IDump.h"
#include "util/StrUtil.h"
#include "util/files.h"
#include "util/xml/XmlWriter.h"

#include "book.h"
#include "log.h"
#include "util.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

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
		const auto getAllImages = [](const UniqueFile& uniqueFile) {
			std::vector<std::reference_wrapper<const ImageItem>> images;
			images.reserve(uniqueFile.images.size() + !uniqueFile.cover.hash.isEmpty());
			if (!uniqueFile.cover.hash.isEmpty())
				images.emplace_back(uniqueFile.cover);
			std::ranges::copy(uniqueFile.images, std::back_inserter(images));
			return images;
		};

		const auto lhsImages = getAllImages(lhs), rhsImages = getAllImages(rhs);

		using ImageHashes = std::unordered_multimap<uint64_t, QString>;
		using ImageHash   = std::pair<uint64_t, QString>;

		ImageHashes lpHashes, rpHashes;

		auto lIt = lhsImages.cbegin(), rIt = rhsImages.cbegin();
		while (lIt != lhsImages.cend() && rIt != rhsImages.cend())
		{
			const auto& lRef = lIt->get();
			const auto& rRef = rIt->get();
			if (lRef.hash < rRef.hash)
			{
				lpHashes.emplace(lRef.pHash, lRef.fileName);
				++lIt;
				continue;
			}

			if (lRef.hash > rRef.hash)
			{
				rpHashes.emplace(rRef.pHash, rRef.fileName);
				++rIt;
				continue;
			}

			++lIt;
			++rIt;
		}

		const auto transform = [](const auto& item) {
			return std::make_pair(item.get().pHash, item.get().fileName);
		};
		std::transform(lIt, lhsImages.cend(), std::inserter(lpHashes, lpHashes.end()), transform);
		std::transform(rIt, rhsImages.cend(), std::inserter(rpHashes, rpHashes.end()), transform);

		auto lIds = lpHashes | std::views::values | std::ranges::to<std::unordered_set<QString>>();
		auto rIds = rpHashes | std::views::values | std::ranges::to<std::unordered_set<QString>>();

		if (!(lpHashes.empty() || rpHashes.empty()))
		{
			std::multimap<std::pair<int, int>, std::pair<ImageHash, ImageHash>> distances;
			for (const auto& l : lpHashes)
				for (const auto& r : rpHashes)
					distances.emplace(std::make_pair(std::popcount(l.first ^ r.first), std::abs(l.second.toInt() - r.second.toInt())), std::make_pair(l, r));
			distances.erase(distances.upper_bound(std::make_pair(m_threshold, 0)), distances.end());

			for (const auto& [l, r] : distances | std::views::values)
			{
				if (!lIds.contains(l.second) || !rIds.contains(r.second))
					continue;

				lIds.erase(l.second);
				rIds.erase(r.second);
			}
		}

		auto result = lIds.empty() ? ImagesCompareResult::Equal : ImagesCompareResult::Outer;
		if (!rIds.empty())
			result = result == ImagesCompareResult::Outer ? ImagesCompareResult::Varied : ImagesCompareResult::Inner;
		if (result == ImagesCompareResult::Varied)
			return result;

		if (result == ImagesCompareResult::Equal && lhs.cover.hash != rhs.cover.hash)
			result = !lhs.cover.hash.isEmpty() ? ImagesCompareResult::Outer : !rhs.cover.hash.isEmpty() ? ImagesCompareResult::Inner : result;

		if (!(lhsImages.empty() || rhsImages.empty()))
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

		Util::HashParser::Parse(file, *this);
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

bool UniqueFileStorage::OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString NAME,
	HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
		Util::HashParser::HashImageItem cover,
	Util::HashParser::HashImageItems    images,
	Util::HashParser::Section::Ptr,
	Util::TextHistogram
)
{
	if (!originFolder.isEmpty())
		return true;

	decltype(UniqueFile::images) imageItems;
	std::ranges::transform(std::move(images) | std::views::as_rvalue, std::inserter(imageItems, imageItems.end()), [](auto&& item) {
		return ImageItem { .fileName = std::move(item.id), .hash = std::move(item.hash), .pHash = item.pHash.toULongLong(nullptr, 16) };
	});

	const UniqueFile::Uid uid { folder, file };

	if (const auto* book = m_inpDataProvider->SetFile(uid, id))
		title.append(" ").append(book->title);
	Util::SimplifyTitle(Util::PrepareTitle(title));
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

	return true;
}
