#pragma once

#include <QHash>

#include <filesystem>
#include <unordered_map>

#include <QString>

namespace HomeCompa::FliParser
{

struct Book;
using FileToFolder = std::unordered_map<QString, QStringList>;

struct Settings
{
	std::filesystem::path sqlFolder;
	std::filesystem::path archivesFolder;
	std::filesystem::path outputFolder;
	std::filesystem::path collectionInfoTemplateFile;
	std::filesystem::path hashFolder;

	QString library;

	std::unordered_map<QString, Book*>   hashToBook;
	std::unordered_map<QString, QString> fileToHash;
	std::unordered_map<QString, QString> libIdToHash;

	FileToFolder fileToFolder;

	[[nodiscard]] Book* FromFile(const QString& file) const;
	[[nodiscard]] Book* FromLibId(const QString& libId) const;

private:
	[[nodiscard]] Book* From(const std::unordered_map<QString, QString>& map, const QString& id) const;
};

} // namespace HomeCompa::FliParser
