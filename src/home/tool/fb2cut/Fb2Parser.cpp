#include <QCryptographicHash>

#include <set>
#include <stack>
#include <unordered_set>

#include <QBuffer>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QString>

#include "fnd/FindPair.h"
#include "fnd/IsOneOf.h"

#include "icu/icu.h"
#include "lib/book.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"
#include "util/xml/XmlWriter.h"

#include "IParser.h"
#include "log.h"

#include "config/version.h"

using namespace HomeCompa;
using namespace fb2cut;

namespace
{

constexpr auto ID     = "id";
constexpr auto L_HREF = "l:href";

constexpr auto FICTION_BOOK    = "FictionBook";
constexpr auto BINARY          = "FictionBook/binary";
constexpr auto BODY_BINARY     = "FictionBook/body/binary";
constexpr auto COVERPAGE_IMAGE = "FictionBook/description/title-info/coverpage/image";
constexpr auto DESCRIPTION     = "FictionBook/description";
constexpr auto DOCUMENT_INFO   = "FictionBook/description/document-info";
constexpr auto PROGRAM_USED    = "FictionBook/description/document-info/program-used";

constexpr auto CUSTOM_INFO = "custom-info";
constexpr auto BR          = "br";

constexpr const char* FB2_TAGS[] {
	"p",
	"fictionbook",
	"description",
	"body",
	"binary",
	"title-info",
	"src-title-info",
	"document-info",
	"publish-info",
	"custom-info",
	"genre",
	"author",
	"first-name",
	"last-name",
	"middle-name",
	"nickname",
	"nick-name",
	"nick",
	"email",
	"home-page",
	"book-title",
	"annotation",
	"poem",
	"cite",
	"subtitle",
	"empty-line",
	"keywords",
	"date",
	"coverpage",
	"image",
	"lang",
	"src-lang",
	"translator",
	"sequence",
	"program-used",
	"src-url",
	"src-ocr",
	"id",
	"version",
	"history",
	"book-author",
	"book-name",
	"publisher",
	"city",
	"year",
	"isbn",
	"title",
	"epigraph",
	"section",
	"v",
	"a",
	"text-author",
	"strong",
	"emphasis",
	"sub",
	"sup",
	"strikethrough",
	"code",
	"stanza",
	"i",
	"b",
	"u",
	"table",
	"tr",
	"td",
	"th",
	"img",
	"pre",
	"style",
	"center",
	"h1",
	"h2",
	"h3",
	"h4",
	"h5",
	"br",
	"stylesheet",
	"username",
	"em",
	"ol",
	"li",
	"col",
};

const std::unordered_set<QString> FB2_TAGS_CACHE { std::begin(FB2_TAGS), std::end(FB2_TAGS) };

QByteArray FixInputFile(const QByteArray& inputFileBody)
{
	auto str = QString::fromUtf8(inputFileBody);

	str.replace(
		QRegularExpression(R"(<([0-9a-zA-Z]+([0-9a-zA-Z]*[-\._+])*[0-9a-zA-Z]+@[0-9a-zA-Z]+([-\.][0-9a-zA-Z]+)*([0-9a-zA-Z]*[\.])[a-zA-Z]{2,6})>)", QRegularExpression::CaseInsensitiveOption),
		R"("\1")"
	);
	str.replace(QRegularExpression(R"(<section id=n(\d)>)", QRegularExpression::CaseInsensitiveOption), R"(<section id="n\1">)");

	{
		const QString         customInfo = "custom-info";
		const QString         br         = "br";
		constexpr const char* specials[] { "amp;", "apos;", "gt;", "lt;", "quot;" };
		const auto            index = str.indexOf(R"(?>)");
		QString               buf   = str.first(index + 3);
		buf.reserve(str.length());
		bool lineBreak    = false;
		bool isCustomInfo = false;
		for (qsizetype i = buf.length(), sz = str.length(); i < sz; ++i)
		{
			const auto ch = str[i];

			if (IsOneOf(ch, '\x0d', '\x0a'))
			{
				if (lineBreak)
					continue;

				buf.append("\x0d\x0a");
				lineBreak = true;
				continue;
			}

			lineBreak = false;

			if (ch == QChar { '&' } && std::ranges::none_of(specials, [&](const char* special) {
					return QStringView(str.constData() + i + 1, static_cast<qsizetype>(strlen(special))) == special;
				}))
			{
				buf.append("&amp;");
				continue;
			}

			if (ch == QChar { '<' } && str[i + 1] != '/')
			{
				const auto it = std::ranges::find_if(FB2_TAGS, [&](const QString& tag) {
					const auto  tagLength = tag.length();
					QStringView s(str.constData() + i + 1, tagLength);
					const auto  nextCh = str[i + 1 + tagLength];
					return s.compare(tag, Qt::CaseInsensitive) == 0 && IsOneOf(nextCh, ' ', '>', '/', '\x0d', '\x0a');
				});
				if (!isCustomInfo && it == std::end(FB2_TAGS))
				{
					buf.append("&lt;");
					continue;
				}

				if (br == *it)
				{
					i += 2;
					continue;
				}

				if (customInfo == *it)
					isCustomInfo = true;
			}

			if (ch == QChar { '>' } && !IsOneOf(str[i - 1], '/', '"'))
			{
				const auto it = std::ranges::find_if(FB2_TAGS, [&](const QString& tag) {
					const auto  tagLength = tag.length();
					QStringView s(str.constData() + i - tagLength, tagLength);
					const auto  prevCh = str[i - tagLength - 1];
					return IsOneOf(prevCh, '<', '/') && s.compare(tag, Qt::CaseInsensitive) == 0;
				});
				if (!isCustomInfo && it == std::end(FB2_TAGS))
				{
					buf.append("&gt;");
					continue;
				}

				if (br == *it)
					continue;

				if (customInfo == *it)
					isCustomInfo = false;
			}

			if (const auto value = ch.unicode(); value >= char16_t { 32 })
				buf.append(ch);
			else if (value == '\x0d')
				buf.append("\x0d\x0a");
		}

		str = std::move(buf);
	}

	str.replace(QRegularExpression(R"(</[^a-z]+?>)", QRegularExpression::CaseInsensitiveOption), "");
	str.replace("<p ", "<p> ");
	str.replace(" /p>", "</p> ");

	return str.toUtf8();
}

QByteArray Decode(const Decoder& decoder, QByteArray inputFileBody)
{
	if (inputFileBody.size() < 100)
		return {};

	auto str = [&]() -> QString {
		const auto encoding = [&inputFileBody]() -> QString {
			static constexpr std::pair<const char*, const char*> UTF16[] {
				{ "\xff\xfe", "UTF16LE" },
				{ "\xfe\xff", "UTF16BE" },
			};
			const auto it = std::ranges::find_if(UTF16, [&](const auto& item) {
				return inputFileBody.startsWith(item.first);
			});

			if (it != std::end(UTF16))
				return it->second;

			QBuffer buf(&inputFileBody);
			buf.open(QIODevice::ReadOnly);
			return Fb2EncodingParser::GetEncoding(buf).toUpper();
		}();

		if (encoding.isEmpty())
		{
			PLOGW << "encoding not found";
			return inputFileBody;
		}

		if (IsOneOf(encoding, "UTF-8", "UTF8"))
			return inputFileBody;

		auto result = decoder.Decode(encoding, inputFileBody.data());

		const auto index = result.indexOf("?>") + 2;
		return index < 2 ? result : R"(<?xml version="1.0" encoding="utf-8"?>)" + result.mid(index);
	}();

	if (str.isEmpty())
		return {};

	const auto addEnd = [&](const qsizetype index) {
		str.resize(index);
		str.append("</FictionBook>");
	};
	if (const auto endIndex = str.indexOf("</FictionBook>"); endIndex > 0)
	{
		str.resize(endIndex + 14);
	}
	else if (const auto binaryIndex = str.lastIndexOf("<binary"); binaryIndex > 0)
	{
		addEnd(binaryIndex);
	}
	else if (const auto bodyIndex = str.lastIndexOf("</body>"); bodyIndex > 0)
	{
		addEnd(bodyIndex + 7);
	}
	return str.toUtf8();
}

QByteArray ValidateFileBody(const QString& inputFilePath, const QByteArray& inputFileBody, const Decoder& decoder, const Util::XmlValidator& validator)
{
	if (!inputFilePath.endsWith(".fb2", Qt::CaseInsensitive))
		return inputFileBody;

	auto fixedInputFileBody = Decode(decoder, inputFileBody);
	if (auto errorText = Validate(validator, fixedInputFileBody); !errorText.isEmpty())
	{
		PLOGW << errorText << " trying to fix";
		fixedInputFileBody = FixInputFile(fixedInputFileBody);
		if (errorText = Validate(validator, fixedInputFileBody); !errorText.isEmpty())
			throw std::invalid_argument(errorText.toStdString());
	}

	return fixedInputFileBody;
}

bool CheckImpl(const QString& fileName, QByteArray& inputFileBody)
{
	if (fileName.endsWith(".fbd", Qt::CaseInsensitive) || fileName.endsWith(".fbd.fb2", Qt::CaseInsensitive))
		return false;

	struct Checker : private Util::SaxParser
	{
		bool checked { false };

		explicit Checker(QIODevice& input)
			: SaxParser(input)
		{
			Parse();
		}

	private: // SaxParser
		bool OnStartElement(const QString&, const QString& path, const Util::XmlAttributes&) override
		{
			checked = path == FICTION_BOOK;
			return false;
		}
	};

	QBuffer buffer(&inputFileBody);
	buffer.open(QIODevice::ReadOnly);

	return Checker(buffer).checked;
}

class Fb2ImageParser final : public Util::SaxParser
{
public:
	struct ParseResult
	{
		bool        result   = false;
		const char* encoding = "utf-8";

		operator bool() const noexcept
		{
			return result;
		}
	};

public:
	static ParseResult Parse(QIODevice& input, IParser::OnBinaryFound binaryCallback, const IEncodingDetector& encodingDetector)
	{
		const Fb2ImageParser parser(input, std::move(binaryCallback));
		return { .result = true, .encoding = encodingDetector.Detect(parser.GetText()) };
	}

public:
	Fb2ImageParser(QIODevice& input, IParser::OnBinaryFound binaryCallback)
		: SaxParser(input, 512)
		, m_binaryCallback { std::move(binaryCallback) }
	{
		SaxParser::Parse();
	}

	const QString& GetText() const noexcept
	{
		return m_text;
	}

private: // Util::SaxParser
	bool OnStartElement(const QString&, const QString& path, const Util::XmlAttributes& attributes) override
	{
		if (m_isBinary)
			throw std::runtime_error("bad binary");

		if (IsOneOf(path, BINARY, BODY_BINARY))
		{
			m_isBinary = true;
			m_picId    = attributes.GetAttribute(ID).trimmed();
			if (const auto it = std::ranges::find_if(
					m_picId,
					[](const auto ch) {
						return ch != '#';
					}
				);
			    it != m_picId.end())
				m_picId = m_picId.last(std::distance(it, m_picId.end())).trimmed();
			return true;
		}

		if (path == COVERPAGE_IMAGE)
		{
			for (size_t i = 0, sz = attributes.GetCount(); i < sz; ++i)
			{
				auto attributeName  = attributes.GetName(i);
				auto attributeValue = attributes.GetValue(i);
				if (attributeName.endsWith(":href"))
				{
					if (const auto it = std::ranges::find_if(
							attributeValue,
							[](const auto ch) {
								return ch != '#';
							}
						);
					    it != attributeValue.end())
						m_coverPage = attributeValue.last(std::distance(it, attributeValue.end())).trimmed();
					break;
				}
			}
			return true;
		}

		return true;
	}

	bool OnEndElement(const QString&, const QString& path) override
	{
		if (IsOneOf(path, BINARY, BODY_BINARY))
			m_isBinary = false;

		return true;
	}

	bool OnCharacters([[maybe_unused]] const QString& path, const QString& value) override
	{
		m_text.append(' ').append(value);

		if (m_picId.isEmpty())
			return true;

		if (!m_isBinary || !IsOneOf(path, BINARY, BODY_BINARY))
			throw std::runtime_error("bad binary");

		const auto isCover = m_picId == m_coverPage;
		m_binaryCallback(std::move(m_picId), isCover, QByteArray::fromBase64(value.toUtf8()));
		m_picId = {};

		return true;
	}

private:
	IParser::OnBinaryFound m_binaryCallback;

	bool    m_isBinary { false };
	QString m_coverPage;
	QString m_picId;
	QString m_text;
};

const std::pair<QString, QString> REPLACE_CHAR[] {
	{  "&lt;",  "<" },
    {  "&gt;",  ">" },
    {  "amp;",  "&" },
    { "apos;",  "'" },
    { "quot;", "\"" },
};

class Fb2TextParser final : public Util::SaxParser
{
public:
	static void Parse(QString fileName, QIODevice& input, QIODevice& output, const std::unordered_map<QString, int>& replaceId, const char* encoding)
	{
		[[maybe_unused]] const Fb2TextParser parser(std::move(fileName), input, output, replaceId, encoding);
	}

public:
	Fb2TextParser(QString fileName, QIODevice& input, QIODevice& output, const std::unordered_map<QString, int>& replaceId, const char* encoding)
		: SaxParser(input, 512)
		, m_fileName { std::move(fileName) }
		, m_replaceId { replaceId }
		, m_writer(output, Util::XmlWriter::Options { .type = Util::XmlWriter::Type::Xml, .indented = false, .encoding = encoding })
	{
		SaxParser::Parse();
		//		assert(m_tags.empty());
		if (!m_tags.empty())
			m_writer.WriteStartElement(QString::number(output.pos()));
	}

private: // Util::SaxParser
	bool OnProcessingInstruction(const QString& target, const QString& data) override
	{
		m_writer.WriteProcessingInstruction(target, data);
		return true;
	}

	bool OnStartElement(const QString& name, const QString& path, const Util::XmlAttributes& attributes) override
	{
		if (name == BR)
			return true;

		if (name == CUSTOM_INFO)
			m_isCustomInfo = true;

		if (!m_isCustomInfo && !FB2_TAGS_CACHE.contains(name.toLower()))
		{
			PLOGW << "Unexpected tag: " << name;
			m_writer.WriteCharacters(QString("<%1").arg(name));
			return true;
		}

		m_tags.push(name);

		if (path == FICTION_BOOK)
		{
			m_writer.WriteStartElement(name);
			m_writer.WriteAttribute("xmlns", "http://www.gribuser.ru/xml/fictionbook/2.0");
			m_writer.WriteAttribute("xmlns:l", "http://www.w3.org/1999/xlink");
			return true;
		}

		if (IsOneOf(path, BINARY, BODY_BINARY))
			return true;

		m_writer.WriteStartElement(name);
		for (size_t i = 0, sz = attributes.GetCount(); i < sz; ++i)
		{
			auto attributeName  = attributes.GetName(i);
			auto attributeValue = attributes.GetValue(i);
			ReplaceAttribute(attributeName, attributeValue);
			m_writer.WriteAttribute(attributeName, attributeValue);
		}

		return true;
	}

	bool OnEndElement(const QString& name, const QString& path) override
	{
		if (name == BR)
			return true;

		if (name == CUSTOM_INFO)
			m_isCustomInfo = false;

		if (!m_isCustomInfo && !FB2_TAGS_CACHE.contains(name.toLower()))
			return m_writer.WriteCharacters(">"), true;

		if (m_tags.top() != name)
			return false;

		m_tags.pop();

		if (path == DOCUMENT_INFO && !m_hasProgramUsed)
		{
			m_writer.WriteStartElement("program-used").WriteCharacters(QString("fb2cut %2").arg(PRODUCT_VERSION)).WriteEndElement();
			m_hasProgramUsed = true;
		}

		if (path == DESCRIPTION && !m_hasProgramUsed)
		{
			m_writer.WriteStartElement("document-info").WriteStartElement("program-used").WriteCharacters(QString("fb2cut %2").arg(PRODUCT_VERSION)).WriteEndElement().WriteEndElement();
			m_hasProgramUsed = true;
		}

		if (IsOneOf(path, BINARY, BODY_BINARY))
			return true;

		m_writer.WriteEndElement();

		return true;
	}

	bool OnCharacters(const QString& path, const QString& value) override
	{
		if (IsOneOf(path, BINARY, BODY_BINARY))
			return true;

		if (path == PROGRAM_USED)
		{
			m_writer.WriteCharacters(QString("%1, fb2cut %2").arg(value, PRODUCT_VERSION));
			m_hasProgramUsed = true;
			return true;
		}

		auto valueCopy = value;

		for (const auto& [before, after] : REPLACE_CHAR)
			valueCopy.replace(before, after, Qt::CaseInsensitive);

		m_writer.WriteCharacters(valueCopy);

		return true;
	}

	bool OnWarning(const size_t line, const size_t column, const QString& text) override
	{
		PLOGW << m_fileName << " " << line << ":" << column << " " << text;
		return true;
	}

	bool OnError(const size_t line, const size_t column, const QString& text) override
	{
		PLOGE << m_fileName << " " << line << ":" << column << " " << text;
		return false;
	}

	bool OnFatalError(const size_t line, const size_t column, const QString& text) override
	{
		return OnError(line, column, text);
	}

private:
	void ReplaceAttribute(QString& name, QString& value) const
	{
		if (name.startsWith("xlink:"))
			name = "l:" + name.last(name.length() - 6);

		if (!name.endsWith(":href"))
			return;

		name = L_HREF;

		if (!value.startsWith('#'))
			return;

		if (const auto it = std::ranges::find_if(
				value,
				[](const auto ch) {
					return ch != '#';
				}
			);
		    it != value.end() && it != value.begin())
			value = value.last(std::distance(it, value.end()));

		const auto it = m_replaceId.find(value);
		if (it == m_replaceId.end())
			return (void)value.prepend('#');

		value = it->second == -1 ? QString { "#cover" } : QString("#%1").arg(it->second);
	}

private:
	const QString                           m_fileName;
	const std::unordered_map<QString, int>& m_replaceId;
	Util::XmlWriter                         m_writer;
	bool                                    m_hasProgramUsed { false };
	std::stack<QString>                     m_tags;
	bool                                    m_isCustomInfo { false };
};

class Fb2Parser final : public IParser
{
public:
	Fb2Parser(QString inputFilePath, QByteArray inputFileBody, const IEncodingDetector& encodingDetector, const Decoder& decoder, const Util::XmlValidator& validator)
		: m_checked { CheckImpl(inputFilePath, inputFileBody) }
		, m_inputFilePath { std::move(inputFilePath) }
		, m_inputFileBody { std::move(inputFileBody) }
		, m_encodingDetector { encodingDetector }
		, m_decoder { decoder }
		, m_validator(validator)
	{
	}

private: // IParser
	OutputFile Parse(OnBinaryFound binaryCallback, const ImageMapper& idToNum) override
	{
		auto    fixedInputFileBody = ValidateFileBody(m_inputFilePath, m_inputFileBody, m_decoder, m_validator);
		QBuffer input(&fixedInputFileBody);
		input.open(QIODevice::ReadOnly);

		const auto parseResult = Fb2ImageParser::Parse(input, std::move(binaryCallback), m_encodingDetector);
		if (!parseResult)
			return {};

		input.seek(0);

		QByteArray bodyOutput;
		QBuffer    output(&bodyOutput);
		output.open(QIODevice::WriteOnly);
		Fb2TextParser::Parse(m_inputFilePath, input, output, idToNum, parseResult.encoding);

//			const QFileInfo fileInfo(m_inputFilePath);
#ifndef NDEBUG
//			WriteErrorFile(fileInfo.completeBaseName() + "_fix", fixedInputFileBody, QString("Validation %1 failed: %2").arg(outputFilePath, ""), true, fileInfo.suffix());
#endif
		//			if (const auto errorText = Validate(m_validator, bodyOutput); !errorText.isEmpty())
		//			{
		//				WriteErrorFile(fileInfo.completeBaseName() + "_out", bodyOutput, QString("Validation %1 failed: %2").arg(outputFilePath, ""), true, fileInfo.suffix());
		//				return WriteErrorFile(fileInfo.completeBaseName(), m_inputFileBody, QString("Validation %1 failed: %2").arg(outputFilePath, errorText), true, fileInfo.suffix()), true;
		//			}

		return { .name = m_inputFilePath, .body = std::move(bodyOutput) };
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
	const IEncodingDetector&  m_encodingDetector;
	const Decoder&            m_decoder;
	const Util::XmlValidator& m_validator;
};

} // namespace

namespace HomeCompa::fb2cut
{

std::unique_ptr<IParser>
create_fb2_parser(QString inputFilePath, QByteArray inputFileBody, QByteArray /*fbdBody*/, const IEncodingDetector& encodingDetector, const Decoder& decoder, const Util::XmlValidator& validator)
{
	return std::make_unique<Fb2Parser>(std::move(inputFilePath), std::move(inputFileBody), encodingDetector, decoder, validator);
}

}
