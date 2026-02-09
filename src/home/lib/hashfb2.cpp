#include "hashfb2.h"

#include <CImg.h>
#include <QCryptographicHash>

#include <ranges>
#include <set>

#include <QBuffer>
#include <QPixmap>

#include "fnd/ScopedCall.h"

#include "util/ImageUtil.h"
#include "util/xml/SaxParser.h"

#include "book.h"
#include "canny.h"
#include "log.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;
using namespace cimg_library;

namespace
{

CImg<float> GetDctMatrix(const int N)
{
	const auto  n = static_cast<float>(N);
	CImg<float> matrix(N, N, 1, 1, 1 / std::sqrt(n));
	const auto  c1 = std::sqrt(2.0f / n);
	for (int x = 0; x < N; ++x)
		for (int y = 1; y < N; ++y)
			matrix(x, y) = c1 * static_cast<float>(std::cos((cimg::PI / 2.0 / N) * y * (2.0 * x + 1)));
	return matrix;
}

const CImg<float> DCT   = GetDctMatrix(32);
const CImg<float> DCT_T = DCT.get_transpose();
const CImg<float> MEAN_FILTER(7, 7, 1, 1, 1);

uint64_t GetPHash(const ImageHashItem& item)
{
	auto pixmap = Util::Decode(item.body);
	if (pixmap.isNull())
		return 0;

	auto       image    = pixmap.toImage();
	const auto hasAlpha = image.pixelFormat().alphaUsage() == QPixelFormat::UsesAlpha;
	image.convertTo(hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_Grayscale8);

	auto          data = new uint8_t[static_cast<size_t>(image.width()) * image.height()];
	CImg<uint8_t> img(data, image.width(), image.height(), 1, 1, true);
	img._is_shared = false;

	if (hasAlpha)
	{
		auto* dst = img.data();
		for (auto h = 0, szh = image.height(), szw = image.width(); h < szh; ++h)
		{
			const auto* src = image.scanLine(h);
			for (auto w = 0; w < szw; ++w, ++dst, src += 4)
				*dst = static_cast<uint8_t>(std::lround((0.299 * src[0] + 0.587 * src[1] + 0.114 * src[2]) * src[3] / 255.0 + (255.0 - src[3])));
		}
	}
	else
	{
		auto* dst = img.data();
		for (auto h = 0, szh = image.height(), szw = image.width(); h < szh; ++h, dst += szw)
			memcpy(dst, image.scanLine(h), szw);
	}

	const Canny canny;
	const auto  cropRect = canny.Process(img);
	static_assert(sizeof(cropRect) == sizeof(uint64_t));

	if (cropRect.width() > img.width() / 2 && cropRect.height() > img.height() / 2)
		img.crop(cropRect.left, cropRect.top, cropRect.right - 1, cropRect.bottom - 1);

	const auto resized = img.get_convolve(MEAN_FILTER).resize(32, 32);
	const auto dct     = (DCT * resized * DCT_T).crop(1, 1, 8, 8);

#ifndef NDEBUG
	QString          str;
	const ScopedCall strGuard([&] {
		PLOGV << item.file << ": " << str;
	});
#endif

	return std::accumulate(dct._data, dct._data + 64, uint64_t { 0 }, [&, median = dct.median()](const uint64_t init, const float value) {
		auto result = init << 1;
		if (value > median)
			result |= 1;

#ifndef NDEBUG
		str.append(value > median ? "1" : "0");
#endif

		return result;
	});
}

void SetHash(ImageHashItem& item, QCryptographicHash& cryptographicHash)
{
	cryptographicHash.reset();
	cryptographicHash.addData(item.body);
	item.hash  = QString::fromUtf8(cryptographicHash.result().toHex());
	item.pHash = GetPHash(item);
	item.body.clear();
}

class Fb2Parser final : public Util::SaxParser
{
	static constexpr auto BODY    = "FictionBook/body";
	static constexpr auto TITLE   = "FictionBook/description/title-info/book-title";
	static constexpr auto SECTION = "section";

	struct HistComparer
	{
		using ItemType = std::pair<size_t, QString>;

		bool operator()(const ItemType& lhs, const ItemType& rhs) const
		{
			return ToComparable(lhs) > ToComparable(rhs);
		}

	private:
		static ItemType ToComparable(const ItemType& item)
		{
			auto result   = item;
			result.first |= (1llu << (32 + std::min(static_cast<int>(item.second.length()), 8)));
			return result;
		}
	};

	struct Section
	{
		using HashValues = std::vector<std::pair<size_t, QString>>;

		Section* parent { nullptr };
		int      depth { 0 };
		size_t   size { 0 };

		std::unordered_map<QString, size_t>   hist;
		QString                               hash;
		std::vector<std::unique_ptr<Section>> children;

		HashValues CalculateHash()
		{
			auto               hashValues = GetHashValues();
			QCryptographicHash md5 { QCryptographicHash::Md5 };
			for (const auto& word : hashValues | std::views::values)
				md5.addData(word.toUtf8());

			hash = QString::fromUtf8(md5.result().toHex());
			size = hist.size();
			hist.clear();

			return hashValues;
		}

	private:
		std::vector<std::pair<size_t, QString>> GetHashValues() const
		{
			std::set<std::pair<size_t, QString>, HistComparer> counter(HistComparer {});
			std::ranges::transform(hist, std::inserter(counter, counter.begin()), [](const auto& item) {
				return std::make_pair(item.second, item.first);
			});

			return counter | std::views::take(10) | std::ranges::to<std::vector<std::pair<size_t, QString>>>();
		}
	};

public:
	explicit Fb2Parser(QIODevice& input)
		: SaxParser(input, 512)
	{
		Parse();
		//		assert(m_tags.empty());
	}

	HashParseResult GetResult()
	{
		QStringList sections;
		const auto  enumerate = [&](const Section& parent, const auto& r) -> void {
            sections << QString("%1%2\t%3").arg(QString(parent.depth, '\t')).arg(parent.hash).arg(parent.size);

            for (const auto& child : parent.children)
                r(*child, r);
		};

		auto hashValues = m_section.CalculateHash();
		enumerate(m_section, enumerate);

		return { .id           = QString::fromUtf8(m_md5.result().toHex()),
			     .title        = std::move(m_title),
			     .hashText     = std::move(m_section.hash),
			     .hashSections = std::move(sections),
			     .hashValues   = std::move(hashValues) };
	}

private: // Util::SaxParser
	bool OnStartElement(const QString& name, const QString& /*path*/, const Util::XmlAttributes& /*attributes*/) override
	{
		if (name == SECTION)
			m_currentSection = m_currentSection->children.emplace_back(std::make_unique<Section>(m_currentSection, m_currentSection->depth + 1)).get();

		return true;
	}

	bool OnEndElement(const QString& name, const QString& /*path*/) override
	{
		if (name == SECTION)
		{
			m_currentSection->CalculateHash();
			m_currentSection = m_currentSection->parent;
			assert(m_currentSection);
		}

		return true;
	}

	bool OnCharacters(const QString& path, const QString& value) override
	{
		UpdateHash(value.toLower());

		auto valueCopy = value;

		PrepareTitle(valueCopy);

		if (path == TITLE)
			return (m_title = SimplifyTitle(valueCopy)), true;

		if (path.startsWith(BODY, Qt::CaseInsensitive))
		{
			for (auto&& word : valueCopy.split(' ', Qt::SkipEmptyParts))
			{
				word.removeIf([](const QChar ch) {
					const auto category = ch.category();
					return category < QChar::Letter_Lowercase || category > QChar::Letter_Other;
				});
				if (word.isEmpty())
					continue;

				for (auto* section = m_currentSection; section; section = section->parent)
					++section->hist[word];
			}
		}

		return true;
	}

	bool OnFatalError(const size_t line, const size_t column, const QString& text) override
	{
		return OnError(line, column, text);
	}

private:
	void UpdateHash(QString value)
	{
		value.removeIf([](const QChar ch) {
			return ch.category() != QChar::Letter_Lowercase;
		});
		m_md5.addData(value.toUtf8());
	}

private:
	QString            m_title;
	Section            m_section;
	Section*           m_currentSection { &m_section };
	QCryptographicHash m_md5 { QCryptographicHash::Md5 };
};

} // namespace

namespace HomeCompa::FliLib
{

void ParseFb2Hash(BookHashItem& bookHashItem, QCryptographicHash& cryptographicHash)
{
	QBuffer buffer(&bookHashItem.body);
	buffer.open(QIODevice::ReadOnly);
	Fb2Parser parser(buffer);
	bookHashItem.parseResult = parser.GetResult();

	if (!bookHashItem.cover.body.isEmpty())
		SetHash(bookHashItem.cover, cryptographicHash);
	std::ranges::for_each(bookHashItem.images, [&](auto& item) {
		SetHash(item, cryptographicHash);
	});
	std::ranges::sort(bookHashItem.images, std::greater {}, [](const auto& item) {
		return -item.file.toInt();
	});
}

}
