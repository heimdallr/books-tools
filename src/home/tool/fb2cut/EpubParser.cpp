#include "util/EpubParser.h"

#include <ranges>

#include <QBuffer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include "IParser.h"
#include "zip.h"

using namespace HomeCompa;
using namespace fb2cut;

namespace
{

QByteArray GetImageIndex(const IParser::ImageMapper& idToNum)
{
	QJsonArray array;
	for (const auto& [id, num] : idToNum)
		array.append(
			QJsonObject {
				{  "id",  id },
				{ "num", num },
        }
		);
	return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

bool CheckImpl(QByteArray& inputFileBody)
{
	try
	{
		QBuffer buffer(&inputFileBody);
		buffer.open(QIODevice::ReadOnly);
		Zip zip(buffer);
		return std::ranges::any_of(zip.GetFileNameList(), [](const QString& fileName) {
			return fileName.endsWith("META-INF/container.xml", Qt::CaseInsensitive);
		});
	}
	catch (...)
	{
	}
	return false;
}

class EpubParser final : public IParser
{
public:
	EpubParser(QString inputFilePath, QByteArray inputFileBody, QByteArray fbdBody, const IEncodingDetector& encodingDetector, const Decoder& decoder, const Util::XmlValidator& validator)
		: m_checked { CheckImpl(inputFileBody) }
		, m_inputFilePath { std::move(inputFilePath) }
		, m_inputFileBody { std::move(inputFileBody) }
		, m_fbdFileBody { std::move(fbdBody) }
		, m_encodingDetector { encodingDetector }
		, m_decoder { decoder }
		, m_validator(validator)
	{
	}

private: // IParser
	OutputFile Parse(OnBinaryFound binaryCallback, const ImageMapper& idToNum) override
	{
		QBuffer buffer(&m_inputFileBody);
		buffer.open(QIODevice::ReadOnly);

		auto parseResult = Util::EpubParser::Parse(buffer, Util::EpubParser::Mode::All);
		if (parseResult.coverExists)
			binaryCallback(std::move(parseResult.images.front().id), true, parseResult.images.front().body);
		for (auto&& [id, body] : parseResult.images | std::views::drop(parseResult.coverExists ? 1 : 0))
			binaryCallback(std::move(id), false, body);

		auto zipFiles = Zip::CreateZipFileController();

		const auto name = QFileInfo(m_inputFilePath).completeBaseName();

		if (!idToNum.empty())
			zipFiles->AddFile("FLibraryImageIndex.json", GetImageIndex(idToNum));
		if (!m_fbdFileBody.isEmpty())
			zipFiles->AddFile(name + ".fbd", m_fbdFileBody);

		for (const auto& [id, body] : parseResult.texts)
			zipFiles->AddFile(name + "/" + id, body);

		QByteArray body;
		{
			QBuffer stream(&body);
			stream.open(QIODevice::WriteOnly);
			Zip zip(stream, Zip::Format::SevenZip);
			zip.SetProperty(ZipDetails::PropertyId::SolidArchive, true);
			zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));
			zip.Write(*zipFiles);
		}

		return { .name = m_inputFilePath, .body = std::move(body) };
	}

	bool Check() const override
	{
		return m_checked;
	}

	const QString& GetInputFileName() const noexcept override
	{
		return m_inputFilePath;
	}

	const QByteArray& GetInputFileBody() const noexcept override
	{
		return m_inputFileBody;
	}

private:
	const bool                m_checked;
	const QString             m_inputFilePath;
	QByteArray                m_inputFileBody;
	QByteArray                m_fbdFileBody;
	const IEncodingDetector&  m_encodingDetector;
	const Decoder&            m_decoder;
	const Util::XmlValidator& m_validator;
};

} // namespace

namespace HomeCompa::fb2cut
{

std::unique_ptr<IParser>
create_epub_parser(QString inputFilePath, QByteArray inputFileBody, QByteArray fbdBody, const IEncodingDetector& encodingDetector, const Decoder& decoder, const Util::XmlValidator& validator)
{
	return std::make_unique<EpubParser>(std::move(inputFilePath), std::move(inputFileBody), std::move(fbdBody), encodingDetector, decoder, validator);
}

}
