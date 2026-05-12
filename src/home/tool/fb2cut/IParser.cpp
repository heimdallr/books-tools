#include "IParser.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextCodec>

#include "util/xml/SaxParser.h"
#include "util/xml/Validator.h"

#include "log.h"

namespace HomeCompa::fb2cut
{

std::unique_ptr<IParser>
CreateFb2Parser(QString /*inputFilePath*/, QByteArray /*inputFileBody*/, const IEncodingDetector& /*encodingDetector*/, const Decoder& /*decoder*/, const Util::XmlValidator& /*validator*/);

}

using namespace HomeCompa::fb2cut;
using namespace HomeCompa;

namespace
{

class EncodingDetector final : virtual public IEncodingDetector
{
	static constexpr auto                            UTF8 = "utf-8";
	static constexpr std::pair<const char*, uint8_t> ENCODINGS[] {
#define ITEM(NUM) { "windows-125" #NUM, 1 << (NUM) },
		ITEM(0) ITEM(1) ITEM(2) ITEM(3) ITEM(4) ITEM(5) ITEM(6)
#undef ITEM
	};

private: // IEncodingDetector
	const char* Detect(const QString& text) const override
	{
		qsizetype counters[std::size(ENCODINGS)] {};
		for (const auto ch : text)
		{
			const auto u = ch.unicode();
			for (const auto i : std::views::iota(uint8_t { 0 }, static_cast<uint8_t>(std::size(ENCODINGS))))
				if (m_unicodeTable[u] & (uint8_t { 1 } << i))
					++counters[i];
		}

		const auto it = std::ranges::max_element(counters);
		return *it * 6 < 5 * text.size() ? UTF8 : ENCODINGS[std::distance(std::begin(counters), it)].first;
	}

private:
	static std::vector<uint8_t> CreateEncodingTable()
	{
		std::vector<uint8_t> result(0x10000, 0);
		const auto           data = std::views::iota(0x0, 0x100) | std::ranges::to<std::vector<char>>();
		for (const auto& [encoding, bit] : ENCODINGS)
		{
			const auto* codec = QTextCodec::codecForName(encoding);
			for (const auto ch : codec->toUnicode(data.data(), static_cast<int>(data.size())))
				result[ch.unicode()] |= bit;
		}
		return result;
	}

private:
	const std::vector<uint8_t> m_unicodeTable { CreateEncodingTable() };
};

class Fb2EncodingParserImpl final : public Util::SaxParser
{
public:
	explicit Fb2EncodingParserImpl(QIODevice& input)
		: SaxParser(input)
	{
		Parse();
	}

	QString GetEncoding() const
	{
		return m_encoding;
	}

private: // SaxParser
	bool OnXMLDecl(const QString& /*versionStr*/, const QString& encodingStr, const QString& /*standaloneStr*/, const QString& /*actualEncodingStr*/) override
	{
		m_encoding = encodingStr;
		return false;
	}

	bool OnFatalError(size_t /*line*/, size_t /*column*/, const QString& text) override
	{
		QRegularExpression rx("unable to create converter for '(.+?)' encoding");
		if (const auto match = rx.match(text); match.hasMatch())
			m_encoding = match.captured(1);

		return false;
	}

private:
	QString m_encoding;
};

} // namespace

std::unique_ptr<IParser> IParser::Create(QString inputFilePath, QByteArray inputFileBody, const IEncodingDetector& encodingDetector, const Decoder& decoder, const Util::XmlValidator& validator)
{
	return CreateFb2Parser(std::move(inputFilePath), std::move(inputFileBody), encodingDetector, decoder, validator);
}

IEncodingDetector::Ptr IEncodingDetector::Create()
{
	return std::make_unique<EncodingDetector>();
}

QString Fb2EncodingParser::GetEncoding(QIODevice& input)
{
	return Fb2EncodingParserImpl(input).GetEncoding();
}

struct Decoder::Impl
{
	std::mutex                                     decodersGuard;
	std::unordered_map<QString, const QTextCodec*> decoders;

	const QTextCodec* GetDecoder(const QString& id)
	{
		std::lock_guard lock(decodersGuard);

		const auto it = decoders.find(id);
		if (it != decoders.end())
			return it->second;

		const auto codec = QTextCodec::codecForName(id.toUtf8());
		PLOGI << id << " codec created";
		return decoders.try_emplace(id, codec).first->second;
	}
};

Decoder::Decoder()
	: m_impl { std::make_unique<Impl>() }
{
}

Decoder::~Decoder()
{
	for (const auto& decoder : m_impl->decoders | std::views::keys)
		PLOGV << decoder;
}

QString Decoder::Decode(const QString& id, const QByteArray& src) const
{
	return m_impl->GetDecoder(id)->toUnicode(src);
}

namespace HomeCompa::fb2cut
{

QString Validate(const Util::XmlValidator& validator, QByteArray& body)
{
	QBuffer buffer(&body);
	buffer.open(QIODevice::ReadOnly);
	return validator.Validate(buffer);
}

void WriteErrorFile(const QDir& dir, std::mutex& guard, const QString& name, const QString& ext, const QByteArray& body)
{
	std::scoped_lock lock(guard);

	const auto filePath = dir.filePath(QString("error/%1.%2").arg(name, !ext.isEmpty() ? ext : "bad"));
	const QDir imgDir   = QFileInfo(filePath).dir();
	if (!imgDir.exists() && !imgDir.mkpath("."))
	{
		PLOGE << QString("Cannot create folder %1").arg(imgDir.path());
		return;
	}

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly))
	{
		PLOGE << QString("Cannot write to %1").arg(filePath);
		return;
	}

	if (file.write(body) != body.size())
	{
		PLOGE << QString("%1 written with errors").arg(filePath);
	}
}

} // namespace HomeCompa::fb2cut
