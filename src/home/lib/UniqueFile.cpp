#include "UniqueFile.h"

#include <ranges>

#include <QDir>
#include <QFile>

#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"
#include "util/xml/XmlWriter.h"

#include "log.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

class HashParserImpl final : public Util::SaxParser
{
	static constexpr auto BOOK       = "books/book";
	static constexpr auto COVER      = "books/book/cover";
	static constexpr auto IMAGE      = "books/book/image";
	static constexpr auto DUPLICATES = "books/book/duplicates";

public:
	HashParserImpl(QIODevice& input, HashParser::Callback callback)
		: SaxParser(input, 512)
		, m_callback { std::move(callback) }
	{
		Parse();
	}

private: // Util::SaxParser
	bool OnStartElement(const QString& /*name*/, const QString& path, const Util::XmlAttributes& attributes) override
	{
		if (path == BOOK)
		{
#define HASH_PARSER_CALLBACK_ITEM(NAME) m_##NAME = attributes.GetAttribute(#NAME);
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
		}
		else if (path == DUPLICATES)
		{
#define HASH_PARSER_CALLBACK_ITEM(NAME) m_##NAME.clear();
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			m_cover.clear();
			m_images.clear();
		}

		return true;
	}

	bool OnEndElement(const QString& /*name*/, const QString& path) override
	{
		if (path == BOOK)
		{
			if (!m_id.isEmpty())
				m_callback(
#define HASH_PARSER_CALLBACK_ITEM(NAME) std::move(m_##NAME),
					HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
						std::move(m_cover),
					std::move(m_images)
				);

#define HASH_PARSER_CALLBACK_ITEM(NAME) m_##NAME = {};
			HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM

			m_cover  = {};
			m_images = {};
		}

		return true;
	}

	bool OnCharacters(const QString& path, const QString& value) override
	{
		if (path == COVER)
			m_cover = value;
		else if (path == IMAGE)
			m_images << value;
		return true;
	}

private:
	HashParser::Callback m_callback;
#define HASH_PARSER_CALLBACK_ITEM(NAME) QString m_##NAME;
	HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM

	QString     m_cover;
	QStringList m_images;
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

		qsizetype depth = -1;
		for (const auto& str : file.hashSections)
		{
			const auto split    = str.split('\t');
			auto       newDepth = split.size() - 2;
			const auto last     = split.rbegin();

			const auto write = [&] {
				m_writer.WriteStartElement("section").WriteAttribute("id", *std::next(last)).WriteAttribute("count", *last);
			};

			if (depth == newDepth)
			{
				m_writer.WriteEndElement();
				write();
				continue;
			}

			if (depth < newDepth)
			{
				write();
				depth = newDepth;
				continue;
			}

			m_writer.WriteEndElement();
			for (; newDepth < depth; --depth)
				m_writer.WriteEndElement();

			write();
		}

		for (; depth >= 0; --depth)
			m_writer.WriteEndElement();
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

enum class ImagesCompareResult
{
	Equal,
	Inner,
	Outer,
	Varied,
};

[[nodiscard]] ImagesCompareResult CompareImages(const UniqueFile& lhs, const UniqueFile& rhs)
{
	auto result = ImagesCompareResult::Equal;
	if (lhs.cover.hash != rhs.cover.hash)
	{
		if (lhs.cover.hash.isEmpty())
			result = ImagesCompareResult::Inner;
		else if (rhs.cover.hash.isEmpty())
			result = ImagesCompareResult::Outer;
		else
			return ImagesCompareResult::Varied;
	}

	auto lIt = lhs.images.cbegin(), rIt = rhs.images.cbegin();
	while (lIt != lhs.images.cend() && rIt != rhs.images.cend())
	{
		if (lIt->hash < rIt->hash)
		{
			if (result == ImagesCompareResult::Inner)
				return ImagesCompareResult::Varied;

			result = ImagesCompareResult::Outer;
			++lIt;
			continue;
		}

		if (lIt->hash > rIt->hash)
		{
			if (result == ImagesCompareResult::Outer)
				return ImagesCompareResult::Varied;

			result = ImagesCompareResult::Inner;
			++rIt;
			continue;
		}
		++lIt;
		++rIt;
	}

	if (result == ImagesCompareResult::Varied)
		return result;

	if (lIt != lhs.images.cend())
		result = result == ImagesCompareResult::Inner ? ImagesCompareResult::Varied : ImagesCompareResult::Outer;

	if (result == ImagesCompareResult::Varied)
		return result;

	if (rIt != rhs.images.cend())
		result = result == ImagesCompareResult::Outer ? ImagesCompareResult::Varied : ImagesCompareResult::Inner;

	if (result == ImagesCompareResult::Varied)
		return result;

	if ((!lhs.images.empty() && !rhs.images.empty()) || (!lhs.cover.hash.isEmpty() && lhs.cover.hash == rhs.cover.hash))
		return result;

	if (std::ranges::includes(lhs.title, rhs.title) || std::ranges::includes(rhs.title, lhs.title))
		return result;

	PLOGW << QString("same hash, different titles: %1/%2 %3 vs %4/%5 %6").arg(lhs.uid.folder, lhs.uid.file, lhs.GetTitle(), rhs.uid.folder, rhs.uid.file, rhs.GetTitle());
	return ImagesCompareResult::Varied;
}

QString createSi()
{
	QString result;
	result.append(QChar { 0x0441 });
	result.append(QChar { 0x0438 });
	return result;
}

} // namespace

bool ImageItem::operator<(const ImageItem& rhs) const
{
	return hash < rhs.hash;
}

QString UniqueFile::GetTitle() const
{
	const QStringList list { title.cbegin(), title.cend() };
	return list.join(' ');
}

void UniqueFile::ClearImages()
{
	cover.body.clear();
	cover.image = {};
	decltype(images) tmp;
	std::ranges::transform(images, std::inserter(tmp, tmp.end()), [](const auto& image) {
		return ImageItem { .hash = image.hash };
	});
	images = std::move(tmp);
}

UniqueFileStorage::UniqueFileStorage(QString dstDir)
	: m_dstDir { std::move(dstDir) }
	, m_duplicateObserver { std::make_unique<DuplicateObserverStub>() }
	, m_si { createSi() }
{
	if (m_dstDir.isEmpty())
		return;

	const QDir srcDir(QDir(m_dstDir).filePath("hash"));
	for (const auto& xml : srcDir.entryList({ "*.xml" }, QDir::Filter::Files))
	{
		PLOGV << "parsing " << xml;
		QFile file(srcDir.filePath(xml));
		if (!file.open(QIODevice::ReadOnly))
			continue;

		HashParser::Parse(
			file,
			[this](
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
				HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
					QString cover,
				QStringList images
			) {
				decltype(UniqueFile::images) imageItems;
				std::ranges::transform(std::move(images) | std::views::as_rvalue, std::inserter(imageItems, imageItems.end()), [](QString&& hash) {
					return ImageItem { .hash = std::move(hash) };
				});
				auto       split = title.split(' ', Qt::SkipEmptyParts);
				UniqueFile uniqueFile {
					.uid      = { .folder = std::move(folder), .file = std::move(file) },
					.title    = { std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()) },
					.hashText = id,
					.cover    = { .hash = std::move(cover) },
					.images   = std::move(imageItems)
				};
				m_old.emplace(std::move(id), std::move(uniqueFile));
			}
		);
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

	if (m_dstDir.isEmpty())
		return &m_new.emplace(std::move(hash), std::make_pair(std::move(file), std::vector<UniqueFile> {}))->second.first;

	const auto log = [&](const UniqueFile& old) {
		PLOGV << QString("duplicates detected: %1/%2 vs %3/%4, %5").arg(file.uid.folder, file.uid.file, old.uid.folder, old.uid.file, file.GetTitle());
	};

	if (const auto it = m_skip.find(std::make_pair(file.uid.folder, file.uid.file)); it != m_skip.end())
	{
		PLOGV << QString("%1/%2 skipped by %3/%4, %5").arg(file.uid.folder, file.uid.file, it->second.first, it->second.second, file.GetTitle());
		m_dup
			.emplace_back(
				std::move(file),
				UniqueFile {
					.uid = { .folder = it->second.first, .file = it->second.second, }
        }
			)
			.file.ClearImages();
		return nullptr;
	}

	for (auto [it, end] = m_old.equal_range(hash); it != end; ++it)
	{
		const auto imagesCompareResult = CompareImages(it->second, file);
		if (imagesCompareResult == ImagesCompareResult::Varied)
			continue;

		if (imagesCompareResult == ImagesCompareResult::Inner)
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
		const auto imagesCompareResult = CompareImages(it->second.first, file);
		if (imagesCompareResult == ImagesCompareResult::Varied)
			continue;

		log(it->second.first);

		if (imagesCompareResult == ImagesCompareResult::Outer || (imagesCompareResult == ImagesCompareResult::Equal && it->second.first.order > file.order))
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

	if (m_dstDir.isEmpty())
		return m_new.clear();

	const QDir dstDir(m_dstDir);
	const auto serializer = Serializer::Create(dstDir.filePath(QString("hash/%1.xml").arg(folder)));

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

void HashParser::Parse(QIODevice& input, Callback callback)
{
	[[maybe_unused]] const HashParserImpl parser(input, std::move(callback));
}
