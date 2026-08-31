#include "UniqueFile.h"

#include <ranges>
#include <unordered_set>

#include <QBuffer>
#include <QDir>
#include <QFile>

#include "fnd/ScopedCall.h"

#include "dump/Factory.h"
#include "dump/IDump.h"
#include "util/StrUtil.h"
#include "util/executor/ThreadPool.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/XmlWriter.h"

#include "book.h"
#include "log.h"
#include "util.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

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
		return file.uid.file > duplicate.uid.file;
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
		using ImageHashes = std::unordered_multimap<uint64_t, QString>;
		using ImageHash   = std::pair<uint64_t, QString>;

		ImageHashes lpHashes, rpHashes;

		auto lIt = lhs.images.cbegin(), rIt = rhs.images.cbegin();
		while (lIt != lhs.images.cend() && rIt != rhs.images.cend())
		{
			const auto& lRef = *lIt;
			const auto& rRef = *rIt;
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
			return std::make_pair(item.pHash, item.fileName);
		};
		std::transform(lIt, lhs.images.cend(), std::inserter(lpHashes, lpHashes.end()), transform);
		std::transform(rIt, rhs.images.cend(), std::inserter(rpHashes, rpHashes.end()), transform);

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

		if (result == ImagesCompareResult::Equal && lhs.cover.hash.isEmpty() != rhs.cover.hash.isEmpty())
			result = rhs.cover.hash.isEmpty() ? ImagesCompareResult::Outer : (assert(lhs.cover.hash.isEmpty()), ImagesCompareResult::Inner);

		if (!(lhs.images.empty() || rhs.images.empty()) || lhs.hash == rhs.hash)
			return result;

		if (Util::Intersect(lhs.title, rhs.title))
			return result;

		PLOGW << QString("same hash, different titles: %1/%2 %3 vs %4/%5 %6").arg(lhs.uid.folder, lhs.uid.file, lhs.GetTitle(), rhs.uid.folder, rhs.uid.file, rhs.GetTitle());
		return ImagesCompareResult::Varied;
	}

private:
	const int m_threshold;
};

struct HashParserObserver final : Util::HashParser::IObserver
{
	struct Item
	{
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString NAME;
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
		Util::HashParser::HashImageItem  cover;
		Util::HashParser::HashImageItems images;
		size_t                           size { 0 };
		uint64_t                         simHash { 0 };
		Util::TextHistogram              hist;
	};

	using Items = std::vector<Item>;
	using Data  = std::vector<std::pair<QString, Items>>;
	Data data;

private:
	void OnParseStarted(const QString& sourceLib) override
	{
		data.emplace_back(std::make_pair(sourceLib, Items {}));
	}
	bool OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			Util::HashParser::HashImageItem cover,
		Util::HashParser::HashImageItems    images,
		Util::HashParser::Section::Ptr      section,
		size_t                              size,
		uint64_t                            simHash,
		Util::TextHistogram                 hist,
		QStringList
	) override
	{
		if (!originFolder.isEmpty())
			return true;

		const auto it = section->children.find(id);

		assert(!data.empty());
		data.back().second.emplace_back(
#define HASH_PARSER_CALLBACK_ITEM(NAME) std::move(NAME),
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
				std::move(cover),
			std::move(images),
			size,
			simHash,
			std::move(hist)
		);
		return true;
	}
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
	if (const auto it = m_data.find(QString("%1#%2").arg(uid.folder, uid.file)); it != m_data.end())
		return it->second.get();

	if (!std::ranges::empty(m_cache | std::views::filter([this](const auto& item) {
								return &item.inpData != m_currentInpData && !item.inpData.empty();
							})))
		return nullptr;

	if (const auto it = m_currentInpData->find(uid.file); it != m_currentInpData->end())
		return it->second.get();

	auto file = uid.file;
	for (auto baseFile = QFileInfo(file).completeBaseName(); baseFile != file; file = baseFile)
		if (const auto it = m_currentInpData->find(baseFile); it != m_currentInpData->end())
			return it->second.get();

	return nullptr;
}

Book* InpDataProvider::GetBook(const QString& sourceLib, const QString& libId) const
{
	if (const auto it = m_libIdToBook.find(libId); it != m_libIdToBook.end())
		return it->second;

	const auto it = m_sourceLibIdToBook.find(QString("%1_%2").arg(sourceLib.toLower(), libId));
	return it != m_sourceLibIdToBook.end() ? it->second : nullptr;
}

Book* InpDataProvider::GetBook(const QString& hash) const
{
	const auto it = m_hashToBook.find(hash);
	return it != m_hashToBook.end() ? it->second : nullptr;
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

		std::ranges::transform(*m_currentInpData | std::views::values, std::inserter(m_sourceLibIdToBook, m_sourceLibIdToBook.end()), [sourceLib = sourceLib.toLower()](const auto& item) {
			return std::make_pair(QString("%1_%2").arg(sourceLib, item->libId), item.get());
		});

		std::ranges::transform(*m_currentInpData | std::views::values, std::inserter(m_hashToBook, m_hashToBook.end()), [](const auto& item) {
			return std::make_pair(item->hash, item.get());
		});

		return;
	}

	m_currentInpData = &m_stub;
}

void InpDataProvider::AddLibToBook(Book* book)
{
	if (m_commonLibFolders.contains(QFileInfo(book->folder).completeBaseName()))
		m_libIdToBook.try_emplace(book->libId, book);
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

Book* InpDataProvider::SetFile(const UniqueFile::Uid& uid, QString id, const size_t size)
{
	const auto add = [&](std::shared_ptr<Book> bookSrc) {
		auto& book   = m_data.try_emplace(QString("%1#%2").arg(uid.folder, uid.file), std::move(bookSrc)).first->second;
		book->id     = std::move(id);
		book->folder = uid.folder;
		if (size != 0)
			book->size = QString::number(size);
		return book.get();
	};

	assert(m_currentInpData);
	if (const auto it = m_currentInpData->find(uid.file); it != m_currentInpData->end())
	{
		assert(it->second);
		return add(it->second);
	}

	const QFileInfo fileInfo(uid.file);
	if (const auto it = m_currentInpData->find(fileInfo.baseName() + "." + fileInfo.suffix()); it != m_currentInpData->end())
	{
		assert(it->second);
		return add(it->second);
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
	const auto xmlList = srcDir.entryList({ "*.xml" }, QDir::Filter::Files);

	Util::ThreadPool<HashParserObserver> threadPool({ .maxQueueSize = static_cast<size_t>(std::thread::hardware_concurrency()) * 2, .contextGetter = [](auto) {
														 return HashParserObserver {};
													 } });
	{
		Util::Progress progress(static_cast<size_t>(xmlList.size()), "parsing");
		for (const auto& xml : xmlList)
		{
			QFile file(srcDir.filePath(xml));
			if (!file.open(QIODevice::ReadOnly))
				continue;

			threadPool.enqueue([&, xml, bytes = file.readAll()](HashParserObserver& observer, const auto&) mutable {
				QBuffer buffer(&bytes);
				buffer.open(QIODevice::ReadOnly);
				Util::HashParser::Parse(buffer, observer);
				progress.Increment(1, QFileInfo(xml).fileName().toStdString());
			});
		}
	}

	auto observers = threadPool.wait();
	erase_if(observers, [](const auto& item) {
		return item.data.empty();
	});

	{
		Util::Progress progress(observers.size(), "collect ready books");

		for (auto&& observer : observers)
		{
			for (auto&& observerDataItem : observer.data)
			{
				m_inpDataProvider->SetSourceLib(observerDataItem.first);
				for (auto&& observerItem : observerDataItem.second)
				{
					auto imageItems = observerItem.images | std::views::as_rvalue | std::views::filter([](auto&& item) {
										  return item.linked;
									  })
					                | std::views::transform([](auto&& item) {
										  return ImageItem { .fileName = std::move(item.id), .hash = std::move(item.hash), .pHash = item.pHash.toULongLong(nullptr, 16) };
									  })
					                | std::ranges::to<decltype(UniqueFile::images)>();

					const UniqueFile::Uid uid { observerItem.folder, observerItem.file };

					if (const auto* book = m_inpDataProvider->SetFile(uid, observerItem.id, observerItem.size))
						observerItem.title.append(" ").append(book->title);
					Util::SimplifyTitle(Util::PrepareTitle(observerItem.title));
					auto split = observerItem.title.split(' ', Qt::SkipEmptyParts);

					UniqueFile uniqueFile {
						.uid      = { .folder = std::move(observerItem.folder), .file = std::move(observerItem.file) },
						.hash     = std::move(observerItem.hash),
						.title    = { std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()) },
						.hashText = observerItem.id,
						.cover    = { .hash = std::move(observerItem.cover.hash), .pHash = observerItem.cover.pHash.toULongLong(nullptr, 16) },
						.images   = std::move(imageItems),
						.size     = observerItem.size,
						.simHash  = observerItem.simHash,
						.hist     = observerItem.hist | std::views::as_rvalue | std::views::values | std::ranges::to<std::vector>(),
					};

					m_sizeToSimHash.emplace(uniqueFile.size, uniqueFile.simHash);
					m_oldSimHash.emplace(uniqueFile.simHash, observerItem.id);

					const auto index = m_files.size();
					m_files.emplace_back(std::move(uniqueFile));
					m_old[std::move(observerItem.id)].emplace_back(index);
				}
				observerDataItem.second.clear();
			}
			observer.data.clear();
			progress.Increment(1, std::to_string(m_files.size()));
		}
	}

	PLOGI << "ready books found: " << m_files.size();
}

std::pair<ImageItem, std::set<ImageItem>> UniqueFileStorage::GetImages(UniqueFile& file)
{
	std::lock_guard lock(m_guard);
	return std::make_pair(file.cover, file.images);
}

void UniqueFileStorage::SetImages(const QString& hash, const QString& fileName, ImageItem cover, std::set<ImageItem> images)
{
	std::lock_guard lock(m_guard);
	const auto      it = m_new.find(hash);
	if (it == m_new.end())
		return;

	for (const auto& index : it->second | std::views::keys)
	{
		auto& file = m_files[index];
		if (file.uid.file == fileName)
		{
			file.cover  = std::move(cover);
			file.images = std::move(images);
			return;
		}
	}
}

void LogIt(const UniqueFile& duplicate, const UniqueFile& file)
{
	PLOGV << QString("duplicates detected: %1/%2 vs %3/%4, %5").arg(duplicate.uid.folder, duplicate.uid.file, file.uid.folder, file.uid.file, duplicate.GetTitle());
}

bool HistCheck(const std::vector<QString>& lhs, const std::vector<QString>& rhs)
{
	const auto rhsOrder = std::views::zip(rhs, std::views::iota(0)) | std::views::transform([](const auto& item) {
							  const auto& [word, index] = item;
							  return std::make_pair(word, index);
						  })
	                    | std::ranges::to<std::unordered_map>();

	const auto lhsFiltered = std::views::zip(lhs, std::views::iota(0)) | std::views::filter([&](const auto& item) {
								 return rhsOrder.contains(std::get<0>(item));
							 })
	                       | std::views::values | std::ranges::to<std::vector>();

	const auto need = lhs.size() - 1;
	if (lhsFiltered.size() < need)
		return false;

	const auto check = [&](const size_t k) {
		int index = -1;
		for (size_t i = 0, sz = lhsFiltered.size(); i < sz; ++i)
		{
			if (k & (1llu << i))
			{
				const auto& str = lhs[lhsFiltered[i]];
				const auto  it  = rhsOrder.find(str);
				assert(it != rhsOrder.end());
				if (it->second < index)
					return false;

				index = it->second;
			}
		}
		return true;
	};

	for (size_t k = (1llu << need) - 1; k < (1llu << lhsFiltered.size()); ++k)
		if (std::popcount(k) >= static_cast<int>(need) && check(k))
			return true;

	return false;
}

bool UniqueFileStorage::CheckForOld(const size_t indexDuplicate, const size_t indexFile, const bool histCheck)
{
	auto&       duplicate = m_files[indexDuplicate];
	const auto& file      = m_files[indexFile];

	if (histCheck && !HistCheck(duplicate.hist, file.hist))
		return false;

	const auto imagesCompareResult = m_imageComparer->Compare(file, duplicate);
	if (imagesCompareResult == ImagesCompareResult::Varied)
		return false;

	if (imagesCompareResult == ImagesCompareResult::Inner || (imagesCompareResult == ImagesCompareResult::Equal && duplicate.hash != file.hash && m_conflictResolver->Resolve(duplicate, file)))
	{
		PLOGW << QString("old duplicate detected by %1/%2: %3/%4, %5").arg(duplicate.uid.folder, duplicate.uid.file, file.uid.folder, file.uid.file, duplicate.GetTitle());
		return false;
	}

	LogIt(duplicate, file);
	m_duplicateObserver->OnDuplicateFound(file.uid, duplicate.uid);
	m_dup.emplace_back(indexDuplicate, indexFile);
	duplicate.ClearImages();

	return true;
}

bool UniqueFileStorage::CheckForOld(const QString& hash, const size_t indexDuplicate, const bool histCheck)
{
	const auto it = m_old.find(hash);
	if (it == m_old.end())
		return false;

	if (std::ranges::none_of(it->second, [this, indexDuplicate, histCheck](const auto item) {
			return CheckForOld(indexDuplicate, item, histCheck);
		}))
		return false;

	const auto& duplicate = m_files[indexDuplicate];
	m_oldSimHash.emplace(duplicate.simHash, hash);
	return true;
}

std::optional<UniqueFile*> UniqueFileStorage::CheckForNew(const QString& hash, const size_t indexDuplicate, const bool histCheck)
{
	const auto it = m_new.find(hash);
	if (it == m_new.end())
		return {};

	auto& duplicate = m_files[indexDuplicate];
	for (auto& [indexCurrent, indicesOld] : it->second)
	{
		auto& file = m_files[indexCurrent];

		if (histCheck && !HistCheck(duplicate.hist, file.hist))
			continue;

		const auto imagesCompareResult = m_imageComparer->Compare(file, duplicate);
		if (imagesCompareResult == ImagesCompareResult::Varied)
			continue;

		m_newSimHash.emplace(duplicate.simHash, hash);

		if (imagesCompareResult == ImagesCompareResult::Outer || (imagesCompareResult == ImagesCompareResult::Equal && m_conflictResolver->Resolve(file, duplicate)))
		{
			LogIt(duplicate, file);
			m_duplicateObserver->OnDuplicateFound(file.uid, duplicate.uid);
			duplicate.ClearImages();
			duplicate.hist.clear();
			indicesOld.emplace_back(indexDuplicate);
			return nullptr;
		}

		LogIt(file, duplicate);
		m_duplicateObserver->OnDuplicateFound(duplicate.uid, file.uid);
		file.ClearImages();
		file.hist.clear();
		indicesOld.emplace_back(indexCurrent);
		indexCurrent = indexDuplicate;
		return &duplicate;
	}

	return {};
}

UniqueFile* UniqueFileStorage::Add(QString hash, UniqueFile fileSrc)
{
	fileSrc.title.erase(m_si);

	std::lock_guard lock(m_guard);

	const auto indexFile = m_files.size();
	auto&      file      = m_files.emplace_back(std::move(fileSrc));
	ScopedCall sizeToSimHashGuard([&] {
		m_sizeToSimHash.emplace(file.size, file.simHash);
	});

	const auto checkSimHash = [&](const SimHashToHash& simHashToHash, const auto& f) -> std::optional<UniqueFile*> {
		for (auto it = m_sizeToSimHash.upper_bound(95 * file.size / 100), end = m_sizeToSimHash.upper_bound(105 * file.size / 100); it != end; ++it)
		{
			if (const auto hamming = std::popcount(it->second ^ file.simHash); hamming <= 8)
			{
				for (auto [from, to] = simHashToHash.equal_range(it->second); from != to; ++from)
				{
					if (from->second != hash)
					{
						if (const auto result = f(from->second))
						{
							PLOGV << "text hamming distance: " << hamming;
							return result;
						}
					}
				}
			}
		}
		return {};
	};

	if (CheckForOld(hash, indexFile, false))
		return nullptr;

	if (const auto result = checkSimHash(m_oldSimHash, [&](const QString& h) {
			return CheckForOld(h, indexFile, true) ? std::optional<UniqueFile*> { nullptr } : std::nullopt;
		}))
		return *result;

	if (const auto result = CheckForNew(hash, indexFile, false))
		return *result;

	if (const auto result = checkSimHash(m_newSimHash, [&](const QString& h) -> std::optional<UniqueFile*> {
			if (auto checkResult = CheckForNew(h, indexFile, true))
				return checkResult;
			return std::nullopt;
		}))
		return *result;

	m_newSimHash.emplace(file.simHash, hash);
	m_new[std::move(hash)].emplace_back(indexFile, std::vector<size_t> {});
	return &file;
}

std::pair<ImageItems, ImageItems> UniqueFileStorage::GetNewImages()
{
	ImageItems covers, images;

	for (const auto& indices : m_new | std::views::values)
	{
		for (const auto& index : indices | std::views::keys)
		{
			const auto& file = m_files[index];
			if (!file.cover.fileName.isEmpty())
				covers.emplace_back(file.cover);
			std::ranges::copy(file.images, std::back_inserter(images));
		}
	}

	return std::make_pair(std::move(covers), std::move(images));
}

void UniqueFileStorage::SetDuplicateObserver(std::unique_ptr<IDuplicateObserver> duplicateObserver)
{
	m_duplicateObserver = std::move(duplicateObserver);
}

void UniqueFileStorage::SetConflictResolver(std::shared_ptr<IUniqueFileConflictResolver> conflictResolver)
{
	m_conflictResolver = std::move(conflictResolver);
}
