#include "hashxml.h"

#include <ranges>

#include <QFile>

#include "UniqueFile.h"
#include "book.h"

using namespace HomeCompa::FliLib;

namespace
{

class XmlHashGetter final : HashParser::IObserver
{
public:
	XmlHashGetter(BookHashItem& bookHashItem, const QString& path, QString file)
		: m_bookHashItem { bookHashItem }
		, m_file { std::move(file) }
	{
		QFile stream(path);
		if (!stream.open(QIODevice::ReadOnly))
			throw std::invalid_argument(std::format("Cannot read from {}", path));

		HashParser::Parse(stream, *this);
	}

private: // HashParser::IObserver
	void OnParseStarted(const QString& /*sourceLib*/) override
	{
	}
	bool OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			HashParser::HashImageItem cover,
		HashParser::HashImageItems    images,
		Section::Ptr,
		TextHistogram textHistogram
	) override
	{
		if (file != m_file)
			return true;

		m_bookHashItem.folder = std::move(folder);
		m_bookHashItem.file   = std::move(file);

		m_bookHashItem.parseResult.hashText   = std::move(id);
		m_bookHashItem.parseResult.hashValues = std::move(textHistogram);

		const auto toImageHashItem = [](HashParser::HashImageItem&& image) {
			return ImageHashItem { .file = std::move(image.id), .hash = std::move(image.hash), .pHash = image.pHash.toULongLong(nullptr, 16) };
		};

		m_bookHashItem.cover = toImageHashItem(std::move(cover));
		std::ranges::transform(std::move(images) | std::views::as_rvalue, std::back_inserter(m_bookHashItem.images), toImageHashItem);

		return false;
	}

private:
	BookHashItem& m_bookHashItem;
	const QString m_file;
};

} // namespace

namespace HomeCompa::FliLib
{

BookHashItem ParseXmlHash(const QString& path, const QString& file)
{
	BookHashItem        bookHashItem;
	const XmlHashGetter xmlHashGetter(bookHashItem, path, file);
	if (bookHashItem.folder.isEmpty())
		throw std::invalid_argument(std::format("cannot find {} in {}", file, path));
	return bookHashItem;
}

}
