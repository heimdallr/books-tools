#include <ranges>

#include <QGuiApplication>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "lib/flihash.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"

#include "log.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flicmp";

using ImageHashes = std::unordered_multimap<uint64_t, std::reference_wrapper<const ImageHashItem>>;

void Compare(QStringList& result, const HashParseResult& lhs, const HashParseResult& rhs)
{
	if (lhs.hashText == rhs.hashText)
		return (void)(result << "texts are equal");

	result << QString("texts are different: %1 vs %2").arg(lhs.hashText, rhs.hashText);
	std::ranges::transform(std::views::zip(lhs.hashValues, rhs.hashValues), std::back_inserter(result), [](const auto& item) {
		const auto& lhsItem = std::get<0>(item);
		const auto& rhsItem = std::get<1>(item);
		return QString("%1 %2 \t %3 %4").arg(lhsItem.first).arg(lhsItem.second).arg(rhsItem.first).arg(rhsItem.second);
	});
}

void Compare(QStringList& result, const ImageHashItem& lhs, const ImageHashItem& rhs)
{
	if (lhs.hash == rhs.hash)
		return (void)(result << "covers are equal");

	if (lhs.hash.isEmpty())
		return (void)(result << QString("%1: no cover").arg(lhs.file));

	if (rhs.hash.isEmpty())
		return (void)(result << QString("%1: no cover").arg(rhs.file));

	const auto hammingDistance = std::popcount(lhs.pHash ^ rhs.pHash);
	result << QString("covers are different: %1 vs %2, Hamming distance: %3").arg(lhs.pHash, 16, 16, QChar { '0' }).arg(rhs.pHash, 16, 16, QChar { '0' }).arg(hammingDistance);
}

void Compare(QStringList& result, ImageHashes& lhs, ImageHashes& rhs, const bool reverse = false)
{
	std::multimap<int, QString, std::greater<>> fileItems;
	for (auto lIt = lhs.begin(); lIt != lhs.end();)
	{
		auto rIt = std::ranges::min_element(rhs, {}, [pHash = lIt->first](const auto& item) {
			return std::popcount(pHash ^ item.first);
		});

		if (rIt == rhs.end())
		{
			fileItems.emplace(-lIt->second.get().file.toInt(), QString("pair not found for %1 %2").arg(reverse ? "right" : "left", lIt->second.get().file));
			++lIt;
			continue;
		}

		std::reference_wrapper<const ImageHashItem> l = lIt->second, r = rIt->second;
		if (reverse)
			std::swap(l, r);

		const auto& lImage = l.get();
		const auto& rImage = r.get();

		fileItems.emplace(
			-lImage.file.toInt(),
			QString("images are different: %1: %4 vs %2: %5, Hamming distance: %3")
				.arg(lImage.file, rImage.file)
				.arg(std::popcount(lImage.pHash ^ rImage.pHash))
				.arg(lImage.pHash, 16, 16, QChar { '0' })
				.arg(rImage.pHash, 16, 16, QChar { '0' })
		);
		lIt = lhs.erase(lIt);
		rhs.erase(rIt);
	}
	std::ranges::move(std::move(fileItems) | std::views::values, std::back_inserter(result));
}

void Compare(QStringList& result, const ImageHashItems& lhs, const ImageHashItems& rhs)
{
	ImageHashes lpHashes, rpHashes;

	auto lIt = lhs.cbegin(), rIt = rhs.cbegin();
	while (lIt != lhs.cend() && rIt != rhs.cend())
	{
		if (lIt->hash < rIt->hash)
		{
			lpHashes.emplace(lIt->pHash, *lIt);
			++lIt;
			continue;
		}

		if (lIt->hash > rIt->hash)
		{
			rpHashes.emplace(rIt->pHash, *rIt);
			++rIt;
			continue;
		}
		++lIt;
		++rIt;
	}

	for (; lIt != lhs.cend(); ++lIt)
		lpHashes.emplace(lIt->pHash, *lIt);
	for (; rIt != rhs.cend(); ++rIt)
		rpHashes.emplace(rIt->pHash, *rIt);

	if (lpHashes.empty() && rpHashes.empty())
		return (void)(result << "images are equal");

	Compare(result, lpHashes, rpHashes);
	Compare(result, rpHashes, lpHashes, true);
}

void Compare(const BookHashItem& lhs, const BookHashItem& rhs)
{
	QStringList result { QString("%1 vs %2:").arg(lhs.file, rhs.file) };

	Compare(result, lhs.parseResult, rhs.parseResult);
	Compare(result, lhs.cover, rhs.cover);
	Compare(result, lhs.images, rhs.images);

	PLOGW << result.join("\n");
}

void go(const int argc, char* argv[])
{
	const auto items = std::views::iota(1, argc) | std::views::transform([&](const int n) {
						   auto item = QString(argv[n]).split(';', Qt::SkipEmptyParts);
						   assert(item.size() == 2);
						   auto bookHashItem = GetHash(item.front(), item.back());
						   bookHashItem.body.clear();
						   return bookHashItem;
					   })
	                 | std::ranges::to<std::vector<BookHashItem>>();

	for (const auto& item : items | std::views::drop(1))
		Compare(items.front(), item);
}

} // namespace

int main(int argc, char* argv[])
{
	const QGuiApplication app(argc, argv);

	Log::LoggingInitializer                          logging(QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);

	try
	{
		PLOGI << "start";
		go(argc, argv);
		return 0;
	}
	catch (const std::exception& ex)
	{
		PLOGE << ex.what();
	}
	catch (...)
	{
		PLOGE << "Unknown error";
	}

	return 1;
}
