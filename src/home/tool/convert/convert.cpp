#include <ranges>

#include <QBuffer>
#include <QDir>
#include <QStringList>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/ScopedCall.h"
#include "fnd/StrUtil.h"

#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"
#include "util/executor/ThreadPool.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"
#include "util/xml/XmlWriter.h"

#include "log.h"

using namespace HomeCompa::Util;
using namespace HomeCompa;

namespace
{

class HashReader final : SaxParser
{
public:
	using Sections = std::vector<std::pair<QString, size_t>>;

public:
	HashReader(QIODevice& stream, Sections& sections)
		: SaxParser(stream)
		, m_sections { sections }
	{
		Parse();
	}

private:
	bool OnStartElement(const QString& name, const QString& /*path*/, const XmlAttributes& attributes) override
	{
		if (name == "section")
			m_sections.emplace_back(attributes.GetAttribute("id"), attributes.GetAttribute("size").toULongLong());

		return true;
	}

private:
	Sections& m_sections;
};

class HashWriter final : SaxParser
{
public:
	using Sections = std::unordered_map<QString, size_t>;

public:
	HashWriter(QIODevice& input, QIODevice& output, const Sections& sections, QString fileName)
		: SaxParser(input)
		, m_writer(output)
		, m_sections { sections }
		, m_fileName { std::move(fileName) }
	{
		PLOGD << m_fileName;
		Parse();
	}

private: // SaxParser
	bool OnStartElement(const QString& name, const QString& /*path*/, const XmlAttributes& attributes) override
	{
		m_writer.WriteStartElement(name, attributes);

		if (name == "section")
		{
			const auto id = attributes.GetAttribute("id");
			const auto it = m_sections.find(id);
			if (it == m_sections.end())
				throw std::runtime_error(std::format("section {}, {} not found", m_fileName, id));

			m_writer.WriteAttribute("size", QString::number(it->second));
		}

		return true;
	}

	bool OnEndElement(const QString& /*name*/, const QString& /*path*/) override
	{
		m_writer.WriteEndElement();
		return true;
	}

	bool OnCharacters(const QString& /*path*/, const QString& value) override
	{
		m_writer.WriteCharacters(value);
		return true;
	}

private:
	XmlWriter       m_writer;
	const Sections& m_sections;
	const QString   m_fileName;
};

QByteArray Read(const QString& filePath)
{
	QFile file(filePath);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

HashWriter::Sections GetBooks(const int argc, char* argv[])
{
	QStringList inputFiles;

	for (auto i = 3; i < argc; ++i)
		for (QDir dir(argv[i]); const auto& file : dir.entryList({ "*.xml" }, QDir::Filter::Files))
			inputFiles << dir.filePath(file);

	ThreadPool<HashReader::Sections> threadPool({ .maxQueueSize = static_cast<size_t>(std::thread::hardware_concurrency()) * 2, .contextGetter = [](auto) {
													 return HashReader::Sections {};
												 } });

	Progress progress(static_cast<size_t>(inputFiles.size()), "parsing");
	for (auto&& filePath : inputFiles)
	{
		threadPool.enqueue([&, filePath = std::move(filePath)](auto& sections) {
			if (auto bytes = Read(filePath); !bytes.isEmpty())
			{
				QBuffer buffer(&bytes);
				buffer.open(QIODevice::ReadOnly);
				[[maybe_unused]] HashReader parser(buffer, sections);
			}
			progress.Increment(1, QFileInfo(filePath).fileName().toStdString());
		});
	}

	HashWriter::Sections sections;

	for (auto&& items : threadPool.wait())
		std::ranges::move(items, std::inserter(sections, sections.end()));

	return sections;
}

} // namespace

int main(const int argc, char* argv[])
{
	XMLPlatformInitializer                     xmlPlatformInitializer;
	Log::LoggingInitializer                    logging("t:/temp/convert.log");
	plog::ConsoleAppender<LogConsoleFormatter> consoleAppender;
	Log::LogAppender                           logConsoleAppender(&consoleAppender);
	try
	{
		const auto books = GetBooks(argc, argv);

		QDir     dirSrc(argv[1]), dirDst(argv[2]);
		auto     inputFiles = dirSrc.entryList({ "*.xml" }, QDir::Filter::Files);
		Progress progress(static_cast<size_t>(inputFiles.size()), "writing");

		ThreadPool threadPool({ .maxQueueSize = static_cast<size_t>(std::thread::hardware_concurrency()) * 2 });
		for (auto&& filePath : inputFiles)
		{
			threadPool.enqueue([&, filePath = std::move(filePath)](auto&) mutable {
				const ScopedCall progressGuard([&, str = QFileInfo(filePath).fileName().toStdString()] {
					progress.Increment(1, str);
				});
				if (auto bytes = Read(dirSrc.filePath(filePath)); !bytes.isEmpty())
					try
					{
						QBuffer input(&bytes);
						input.open(QIODevice::ReadOnly);
						QFile output(dirDst.filePath(filePath));
						if (output.open(QIODevice::WriteOnly)) [[likely]]
						{
							[[maybe_unused]] HashWriter writer(input, output, books, std::move(filePath));
						}
					}
					catch (const std::exception& ex)
					{
						PLOGE << ex.what();
					}
			});
		}

		threadPool.wait();

		return 0;
	}
	catch (const std::exception& ex)
	{
		PLOGE << ex.what();
	}
	return 1;
}
