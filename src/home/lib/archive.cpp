#include "archive.h"

#include <ranges>
#include <unordered_set>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include "util/StrUtil.h"
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
	const QRegularExpression    rx("^.*?fb2.*?([0-9]+).*?$");

	for (const auto& argument : wildCards)
	{
		auto splitted = argument.split(';');

		const auto wildCard   = std::move(splitted.front());
		const QDir hashFolder = [&]() -> QDir {
			if (splitted.size() < 2)
				return {};

			QDir result(splitted.back());
			if (!hashFolder.exists())
				throw std::invalid_argument(std::format("hash folder {} not found", splitted.back()));

			return result;
		}();

		const auto getHashPath = [&](const QString& name) {
			return splitted.size() < 2 ? QString {} : hashFolder.filePath(name + ".xml");
		};

		std::ranges::transform(
			Util::ResolveWildcard(wildCard) | std::views::transform([&](const QString& item) {
				return QFileInfo(item);
			}) | std::views::filter([&](const QFileInfo& item) {
				auto       fileName = item.fileName().toLower();
				const auto result   = !uniqueFiles.contains(fileName);
				if (result)
					uniqueFiles.emplace(fileName);
				return result;
			}) | std::views::transform([&](const QFileInfo& item) {
				auto hashPath = getHashPath(item.completeBaseName());
				if (!(hashPath.isEmpty() || QFile::exists(hashPath)))
					throw std::invalid_argument(std::format("{} not found", hashPath));
				return Archive { item.absoluteFilePath(), std::move(hashPath) };
			}),
			std::inserter(sorted, sorted.end()),
			[&](Archive archive) {
				const auto match = rx.match(QFileInfo(archive.filePath).fileName());
				return std::make_pair(match.hasMatch() ? match.captured(1).toInt() : 0, std::move(archive));
			}
		);
	}

	auto result = std::move(sorted) | std::views::values | std::views::reverse | std::ranges::to<Archives>();
	if (result.empty())
		throw std::invalid_argument("no archives found");

	return result;
}

size_t Total(const Archives& archives)
{
	PLOGD << "Total file count calculation";
	const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), size_t { 0 }, [](const size_t init, const Archive& archive) {
		const Zip zip(archive.filePath);
		return init + zip.GetFileNameList().size();
	});
	PLOGI << "Total file count: " << totalFileCount;

	return totalFileCount;
}

} // namespace HomeCompa::FliLib
