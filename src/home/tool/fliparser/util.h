#pragma once

#include <filesystem>

#include <QByteArray>

namespace HomeCompa::DB
{
class IDatabase;
}

namespace HomeCompa::FliParser
{

void FillTables(DB::IDatabase& db, const std::filesystem::path& path);
void Write(const QString& fileName, const QByteArray& data);
QString& ReplaceTags(QString& str);

}
