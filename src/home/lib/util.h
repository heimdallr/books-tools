#pragma once

#include <filesystem>
#include <format>
#include <string>

#include "fnd/algorithm.h"

#include "export/lib.h"

namespace HomeCompa::Util
{

class XmlWriter;

}

class QByteArray;

namespace HomeCompa::FliLib
{

class IDump;
struct Book;

using InpData = std::unordered_map<QString, std::shared_ptr<Book>, Util::CaseInsensitiveHash<QString>>;

LIB_EXPORT void     Write(const QString& fileName, const QByteArray& data);
LIB_EXPORT QString& ReplaceTags(QString& str);
LIB_EXPORT InpData  CreateInpData(const IDump& db);
LIB_EXPORT void     SerializeHashSections(const QStringList& sections, Util::XmlWriter& writer);

}

template <>
struct std::formatter<QString> : std::formatter<std::string>
{
	auto format(const QString& obj, std::format_context& ctx) const
	{
		return std::formatter<std::string>::format(obj.toStdString(), ctx);
	}
};

template <>
struct std::formatter<std::filesystem::path> : std::formatter<std::string>
{
	auto format(const std::filesystem::path& obj, std::format_context& ctx) const
	{
		return std::formatter<std::string>::format(obj.string(), ctx);
	}
};
