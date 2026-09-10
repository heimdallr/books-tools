#pragma once

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

}
