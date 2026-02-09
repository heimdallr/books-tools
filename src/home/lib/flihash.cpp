#include "flihash.h"

#include <QCryptographicHash>

#include <ranges>
#include <set>

#include <QDir>
#include <QFileInfo>

#include "fnd/FindPair.h"

#include "Constant.h"
#include "hashfb2.h"
#include "hashxml.h"
#include "zip.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

BookHashItem GetHash_7z(const QString& path, const QString& file)
{
	QCryptographicHash md5 { QCryptographicHash::Md5 };
	auto               bookHashItem = BookHashItemProvider(path).Get(file);
	ParseFb2Hash(bookHashItem, md5);
	return bookHashItem;
}

BookHashItem GetHash_xml(const QString& path, const QString& file)
{
	return ParseXmlHash(path, file);
}

std::unique_ptr<Zip> GetZip(const QFileInfo& fileInfo, const char* type)
{
	const auto zipPath = fileInfo.dir().absoluteFilePath(QString("%1/%2.zip").arg(type, fileInfo.completeBaseName()));
	return QFile::exists(zipPath) ? std::make_unique<Zip>(zipPath) : std::unique_ptr<Zip> {};
}

} // namespace

struct BookHashItemProvider::Impl
{
	Zip             zip;
	const QFileInfo fileInfo;

	std::unique_ptr<Zip> coversZip { GetZip(fileInfo, Global::COVERS) };
	std::unique_ptr<Zip> imagesZip { GetZip(fileInfo, Global::IMAGES) };

	std::set<QString> covers { (coversZip ? coversZip->GetFileNameList() : QStringList {}) | std::ranges::to<std::set<QString>>() };
	std::set<QString> images { (imagesZip ? imagesZip->GetFileNameList() : QStringList {}) | std::ranges::to<std::set<QString>>() };

	explicit Impl(const QString& path)
		: zip(path)
		, fileInfo(path)
	{
	}
};

BookHashItemProvider::BookHashItemProvider(const QString& path)
	: m_impl(path)
{
}

BookHashItemProvider::~BookHashItemProvider() = default;

QStringList BookHashItemProvider::GetFiles() const
{
	return m_impl->zip.GetFileNameList();
}

BookHashItem BookHashItemProvider::Get(const QString& file) const
{
	BookHashItem bookHashItem { .folder = m_impl->fileInfo.fileName(), .file = file, .body = m_impl->zip.Read(file)->GetStream().readAll() };

	const auto baseName = QFileInfo(file).completeBaseName();
	if (m_impl->coversZip && m_impl->covers.contains(baseName))
		bookHashItem.cover = { QString {}, m_impl->coversZip->Read(baseName)->GetStream().readAll() };

	if (m_impl->imagesZip)
		std::ranges::transform(
			std::ranges::equal_range(
				m_impl->images,
				baseName + "/",
				{},
				[n = baseName.length() + 1](const QString& item) {
					return QStringView { item.begin(), std::next(item.begin(), n) };
				}
			),
			std::back_inserter(bookHashItem.images),
			[&](const QString& item) {
				return ImageHashItem { item.split("/").back(), m_impl->imagesZip->Read(item)->GetStream().readAll() };
			}
		);

	return bookHashItem;
}

namespace HomeCompa::FliLib
{

BookHashItem GetHash(const QString& path, const QString& file)
{
	static constexpr std::pair<const char*, BookHashItem (*)(const QString&, const QString&)> parsers[] {
#define ITEM(NAME) { #NAME, &GetHash_##NAME }
		ITEM(7z),
		ITEM(xml),
#undef ITEM
	};

	return FindSecond(parsers, QFileInfo(path).suffix().toLower().toStdString().data(), PszComparer {})(path, file);
}

}
