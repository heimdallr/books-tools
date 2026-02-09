#include <ranges>
#include <unordered_set>

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

using ImageHash   = std::pair<uint64_t, QString>;
using ImageHashes = std::unordered_multimap<uint64_t, QString>;

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

void Compare(std::multimap<int, QString, std::greater<>>& fileItems, const ImageHashes& lhs, const ImageHashes& rhs)
{
	std::multimap<int, std::pair<ImageHash, ImageHash>> distances;
	for (const auto& l : lhs)
		for (const auto& r : rhs)
			distances.emplace(std::popcount(l.first ^ r.first), std::make_pair(l, r));

	const auto toIdList = [](const ImageHashes& hashes) {
		return hashes | std::views::values | std::ranges::to<std::unordered_set<QString>>();
	};
	auto lIds = toIdList(lhs), rIds = toIdList(rhs);

	for (const auto& [l, r] : distances | std::views::values)
	{
		if (!lIds.contains(l.second) || !rIds.contains(r.second))
			continue;

		lIds.erase(l.second);
		rIds.erase(r.second);

		fileItems.emplace(
			-l.second.toInt(),
			QString("images are different: %1: %4 vs %2: %5, Hamming distance: %3")
				.arg(l.second, r.second)
				.arg(std::popcount(l.first ^ r.first))
				.arg(l.first, 16, 16, QChar { '0' })
				.arg(r.first, 16, 16, QChar { '0' })
		);
	}

	const auto notFound = [](const bool reverse, const QString& id) {
		return std::make_pair(-id.toInt(), QString("pair not found for %1 %2").arg(reverse ? "right" : "left", id));
	};

	std::ranges::transform(lIds, std::inserter(fileItems, fileItems.end()), std::bind_front(notFound, false));
	std::ranges::transform(rIds, std::inserter(fileItems, fileItems.end()), std::bind_front(notFound, true));
}

void Compare(QStringList& result, const ImageHashItems& lhs, const ImageHashItems& rhs)
{
	std::multimap<int, QString, std::greater<>> fileItems;
	ImageHashes lpHashes, rpHashes;

	auto lIt = lhs.cbegin(), rIt = rhs.cbegin();
	while (lIt != lhs.cend() && rIt != rhs.cend())
	{
		if (lIt->hash < rIt->hash)
		{
			lpHashes.emplace(lIt->pHash, lIt->file);
			++lIt;
			continue;
		}

		if (lIt->hash > rIt->hash)
		{
			rpHashes.emplace(rIt->pHash, rIt->file);
			++rIt;
			continue;
		}
		fileItems.emplace(-lIt->file.toInt(), QString("%1 and %2 are equal: %3").arg(lIt->file, rIt->file).arg(lIt->hash));
		++lIt;
		++rIt;
	}

	for (; lIt != lhs.cend(); ++lIt)
		lpHashes.emplace(lIt->pHash, lIt->file);
	for (; rIt != rhs.cend(); ++rIt)
		rpHashes.emplace(rIt->pHash, rIt->file);

	if (lpHashes.empty() && rpHashes.empty())
		return (void)(result << "images are equal");

	Compare(fileItems, lpHashes, rpHashes);
	std::ranges::move(std::move(fileItems) | std::views::values, std::back_inserter(result));
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
