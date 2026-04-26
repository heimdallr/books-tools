#pragma once

#include <QString>
#include <QStringList>

class QIODevice;

namespace HomeCompa::fb2cut
{

class IEncodingDetector // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
	using Ptr = std::unique_ptr<IEncodingDetector>;
	static Ptr Create();

public:
	virtual ~IEncodingDetector() = default;

	virtual const char* Detect(const QString& text) const = 0;
};

struct Fb2EncodingParser
{
	static QString GetEncoding(QIODevice& input);
};

struct Fb2ImageParser
{
	struct ParseResult
	{
		bool        result   = false;
		const char* encoding = "utf-8";

		operator bool() const noexcept
		{
			return result;
		}
	};

	using OnBinaryFound = std::function<void(QString&&, bool isCover, const QByteArray& data)>;
	static ParseResult Parse(QIODevice& input, OnBinaryFound binaryCallback, const IEncodingDetector& encodingDetector);
};

struct Fb2Parser
{
	static constexpr const char* FB2_TAGS[] {
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

	static void Parse(QString fileName, QIODevice& input, QIODevice& output, const std::unordered_map<QString, int>& replaceId, const char* encoding);
};

} // namespace HomeCompa::fb2cut
