#pragma once

#include <filesystem>
#include <unordered_map>

#include <QHash>
#include <QString>

namespace HomeCompa::FliParser
{

struct Book;

struct Settings
{
	std::filesystem::path sqlFolder;
	std::filesystem::path archivesFolder;
	std::filesystem::path outputFolder;
	std::filesystem::path collectionInfoTemplateFile;
	std::filesystem::path hashFolder;

	std::unordered_map<QString, Book*>   hashToBook;
	std::unordered_map<QString, QString> fileToHash;
	std::unordered_map<QString, QString> libIdToHash;

	[[nodiscard]] Book* FromFile(const QString& file) const;
	[[nodiscard]] Book* FromLibId(const QString& libId) const;

private:
	[[nodiscard]] Book* From(const std::unordered_map<QString, QString>& map, const QString& id) const;
};

} // namespace HomeCompa::FliParser
