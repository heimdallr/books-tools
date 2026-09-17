#pragma once

#include "fnd/algorithm.h"

#include "export/lib.h"

namespace HomeCompa {
namespace DB {

class ITransaction;

}

class Zip;

}

namespace HomeCompa::Util {

class XmlWriter;

}

class QByteArray;

namespace HomeCompa::FliLib {

class InpDataProvider;

class IDump;
struct Book;

using InpData = std::unordered_map<QString, std::shared_ptr<Book>, Util::CaseInsensitiveHash<QString>>;

struct FileInfo
{
	QByteArray hash;
	qsizetype  size;
};

LIB_EXPORT void     Write(const QString& fileName, const QByteArray& data);
LIB_EXPORT QString& ReplaceTags(QString& str);
LIB_EXPORT InpData  CreateInpData(const IDump& db, std::unordered_map<QString, QString>& series);
LIB_EXPORT Book*    ParseBook(const QString& fileName, InpDataProvider& inpDataProvider, const QString& folder, const Zip& zip, const QDateTime& zipDateTime, bool isDeleted = false);
LIB_EXPORT void     WriteParsedBookToDatabase(DB::ITransaction& tr, const Book& book);

}
