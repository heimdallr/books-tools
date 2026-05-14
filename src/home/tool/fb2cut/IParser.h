#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include <QString>

#include "fnd/NonCopyMovable.h"

class QDir;

namespace HomeCompa::Util
{

class XmlValidator;

}

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

class Decoder
{
	NON_COPY_MOVABLE(Decoder)

public:
	Decoder();
	~Decoder();
	QString Decode(const QString& id, const QByteArray& src) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

class IParser // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
	struct OutputFile
	{
		QString    name;
		QByteArray body;
	};

	using OnBinaryFound = std::function<void(QString&&, bool isCover, const QByteArray& data)>;
	using ImageMapper   = std::unordered_map<QString, int>;

public:
	static std::unique_ptr<IParser> Create(QString inputFilePath, QByteArray inputFileBody, const IEncodingDetector& encodingDetector, const Decoder& decoder, const Util::XmlValidator& validator);

public:
	virtual ~IParser() = default;

	virtual OutputFile Parse(OnBinaryFound binaryCallback, const ImageMapper& idToNum) = 0;

	virtual bool Check() const = 0;

	virtual const QString&    GetInputFileName() const noexcept = 0;
	virtual const QByteArray& GetInputFileBody() const noexcept = 0;
};

QString Validate(const Util::XmlValidator& validator, QByteArray& body);
void    WriteErrorFile(const QDir& dir, std::mutex& guard, const QString& name, const QString& ext, const QByteArray& body);

} // namespace HomeCompa::fb2cut
