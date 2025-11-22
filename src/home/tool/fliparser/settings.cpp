#include "settings.h"

using namespace HomeCompa::FliParser;

Book* Settings::FromFile(const QString& file) const
{
	return From(fileToHash, file);
}

Book* Settings::FromLibId(const QString& libId) const
{
	return From(libIdToHash, libId);
}

Book* Settings::From(const std::unordered_map<QString, QString>& map, const QString& id) const
{
	const auto hashIt = map.find(id);
	if (hashIt == map.end())
		return nullptr;

	const auto bookIt = hashToBook.find(hashIt->second);
	if (bookIt == hashToBook.end())
		return nullptr;

	return bookIt->second;
}
