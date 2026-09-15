#include "archive.h"

#include <ranges>
#include <unordered_set>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include "fnd/StrUtil.h"

#include "util/files.h"

#include "log.h"
#include "util.h"
#include "zip.h"

namespace HomeCompa::FliLib
{

Archives GetArchives(const QStringList& wildCards)
{
	std::multimap<int, Archive> sorted;
	std::unordered_set<QString> uniqueFiles;
	const QRegularExpression    rx("^.*?([0-9]+).*?$");

	for (const auto& wildCard : wildCards)
		std::ranges::move(
			Util::ResolveWildcard(wildCard) | std::views::as_rvalue | std::views::transform([&](QString&& item) {
				const auto match = rx.match(QFileInfo(item).fileName());
				return std::make_pair(match.hasMatch() ? match.captured(1).toInt() : 0, Archive { .filePath = std::move(item), .sourceLib = {} });
			}),
			std::inserter(sorted, sorted.end())
		);

	auto result = std::move(sorted) | std::views::values | std::views::reverse | std::ranges::to<Archives>();
	if (result.empty())
		throw std::invalid_argument("no archives found");

	return result;
}

size_t Total(const Archives& archives)
{
	PLOGD << "Total file count calculation";
	const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), 0ULL, [](const auto init, const auto& archive) {
		const Zip zip(archive.filePath);
		return init + static_cast<size_t>(zip.GetFileNameList().size());
	});
	PLOGI << "Total file count: " << totalFileCount;

	return totalFileCount;
}

} // namespace HomeCompa::FliLib
