#include <QCryptographicHash>

#include <ranges>
#include <set>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "lib/book.h"
#include "lib/dump/Factory.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "log.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flihasher";

constexpr auto OUTPUT                       = "output";
constexpr auto FOLDER                       = "folder";
constexpr auto LIBRARY                      = "library";
constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";

struct ParseResult
{
	QString     id;
	QString     title;
	QString     hashText;
	QStringList hashSections;
};

class Fb2Parser final : public Util::SaxParser
{
	static constexpr auto BODY     = "FictionBook/body";
	static constexpr auto TITLE    = "FictionBook/description/title-info/book-title";
	static constexpr auto SECTION  = "section";
	static constexpr auto SUBTITLE = "title";

	struct Section
	{
		Section* parent { nullptr };
		int      depth { 0 };
		size_t   size { 0 };

		std::unordered_map<QString, size_t>   hist;
		QString                               hash;
		std::vector<std::unique_ptr<Section>> children;

		void CalculateHash()
		{
			hash = GetHashImpl();
			size = hist.size();
			hist.clear();
		}

	private:
		QString GetHashImpl() const
		{
			QCryptographicHash md5 { QCryptographicHash::Md5 };

			std::set<std::pair<size_t, QString>, std::greater<>> counter;
			std::ranges::transform(hist, std::inserter(counter, counter.begin()), [](const auto& item) {
				return std::make_pair(item.second, item.first);
			});

			for (int n = 1; const auto& word : counter | std::views::values)
			{
				md5.addData(word.toUtf8());
				if (++n > 10)
					break;
			}

			return QString::fromUtf8(md5.result().toHex());
		}
	};

public:
	explicit Fb2Parser(QIODevice& input)
		: SaxParser(input, 512)
	{
		Parse();
		//		assert(m_tags.empty());
	}

	ParseResult GetResult()
	{
		QStringList sections;
		const auto  enumerate = [&](const Section& parent, const auto& r) -> void {
            sections << QString("%1%2\t%3").arg(QString(parent.depth, '\t')).arg(parent.hash).arg(parent.size);

            for (const auto& child : parent.children)
                r(*child, r);
		};

		m_section.CalculateHash();
		enumerate(m_section, enumerate);

		return { .id = QString::fromUtf8(m_md5.result().toHex()), .title = std::move(m_title), .hashText = std::move(m_section.hash), .hashSections = std::move(sections) };
	}

private: // Util::SaxParser
	bool OnStartElement(const QString& name, const QString& /*path*/, const Util::XmlAttributes& /*attributes*/) override
	{
		if (name == SECTION)
			m_currentSection = m_currentSection->children.emplace_back(std::make_unique<Section>(m_currentSection, m_currentSection->depth + 1)).get();

		return true;
	}

	bool OnEndElement(const QString& name, const QString& /*path*/) override
	{
		if (name == SECTION)
		{
			m_currentSection->CalculateHash();
			m_currentSection = m_currentSection->parent;
			assert(m_currentSection);
		}

		return true;
	}

	bool OnCharacters(const QString& path, const QString& value) override
	{
		UpdateHash(value.toLower());

		auto valueCopy = value;

		FliLib::PrepareTitle(valueCopy);

		if (path == TITLE)
			return (m_title = FliLib::SimplifyTitle(valueCopy)), true;

		if (path.startsWith(BODY, Qt::CaseInsensitive) && !path.contains(SUBTITLE))
		{
			for (auto&& word : valueCopy.split(' ', Qt::SkipEmptyParts))
			{
				word.removeIf([](const QChar ch) {
					return ch.category() != QChar::Letter_Lowercase;
				});
				if (word.length() > 5)
				{
					for (auto* section = m_currentSection; section; section = section->parent)
						++section->hist[word];
				}
			}
		}

		return true;
	}

	bool OnFatalError(const size_t line, const size_t column, const QString& text) override
	{
		return OnError(line, column, text);
	}

private:
	void UpdateHash(QString value)
	{
		value.removeIf([](const QChar ch) {
			return ch.category() != QChar::Letter_Lowercase;
		});
		m_md5.addData(value.toUtf8());
	}

private:
	QString            m_title;
	Section            m_section;
	Section*           m_currentSection { &m_section };
	QCryptographicHash m_md5 { QCryptographicHash::Md5 };
};

QString GetHash(const Zip& zip, const QString& file, QCryptographicHash& md5)
{
	md5.reset();
	md5.addData(&zip.Read(file)->GetStream());
	return QString::fromUtf8(md5.result().toHex());
}

QStringList GetImageHashes(const Zip& zip, const std::set<QString>& imageFiles, const QString& file, QCryptographicHash& md5)
{
	QStringList result;
	std::ranges::transform(
		std::ranges::equal_range(
			imageFiles,
			file,
			{},
			[n = file.length()](const QString& item) {
				return QStringView { item.begin(), std::next(item.begin(), n) };
			}
		),
		std::back_inserter(result),
		[&](const auto& item) {
			return GetHash(zip, item, md5);
		}
	);
	std::ranges::sort(result);
	return result;
}

void ProcessFile(
	const QString&           folder,
	const QString&           file,
	const Zip&               zip,
	const Zip*               coverZip,
	const std::set<QString>& coverFiles,
	const Zip*               imageZip,
	const std::set<QString>& imageFiles,
	Util::XmlWriter&         writer
)
{
	QCryptographicHash md5 { QCryptographicHash::Md5 };

	const auto bookGuard = writer.Guard("book");
	const auto zipFile   = zip.Read(file);
	Fb2Parser  parser(zipFile->GetStream());
	const auto parseResult = parser.GetResult();
	bookGuard->WriteAttribute("hash", parseResult.id).WriteAttribute("id", parseResult.hashText).WriteAttribute(Inpx::FOLDER, folder).WriteAttribute(Inpx::FILE, file).WriteAttribute("title", parseResult.title);

	const auto baseName = QFileInfo(file).completeBaseName();
	if (coverZip && coverFiles.contains(baseName))
		if (const auto coverHash = GetHash(*coverZip, baseName, md5); !coverHash.isEmpty())
			bookGuard->Guard(Global::COVER)->WriteCharacters(coverHash);

	if (imageZip)
		for (const auto& imageHash : GetImageHashes(*imageZip, imageFiles, baseName + "/", md5))
			bookGuard->Guard(Global::IMAGE)->WriteCharacters(imageHash);

	FliLib::SerializeHashSections(parseResult.hashSections, writer);
}

void ProcessArchive(const QDir& dstDir, const QString& filePath, const QString& sourceLib, Util::Progress& progress)
{
	PLOGI << "process " << filePath;
	assert(dstDir.exists());
	Zip       zip(filePath);
	QFileInfo fileInfo(filePath);

	const auto getZip = [&](const char* type) -> std::unique_ptr<Zip> {
		const auto path = fileInfo.dir().absoluteFilePath(QString("%1/%2.zip").arg(type, fileInfo.completeBaseName()));
		return QFile::exists(path) ? std::make_unique<Zip>(path) : std::unique_ptr<Zip> {};
	};

	const auto coversZip = getZip(Global::COVERS);
	const auto imagesZip = getZip(Global::IMAGES);

	QFile output(dstDir.filePath(fileInfo.completeBaseName() + ".xml"));
	if (!output.open(QIODevice::WriteOnly))
		throw std::ios_base::failure(std::format("Cannot create {}", dstDir.filePath(fileInfo.completeBaseName() + ".xml")));

	Util::XmlWriter writer(output);
	const auto      booksGuard = writer.Guard("books");
	booksGuard->WriteAttribute("source", sourceLib);

	for (const auto& file : zip.GetFileNameList())
	{
		ProcessFile(
			fileInfo.fileName(),
			file,
			zip,
			coversZip.get(),
			(coversZip ? coversZip->GetFileNameList() : QStringList {}) | std::ranges::to<std::set<QString>>(),
			imagesZip.get(),
			(imagesZip ? imagesZip->GetFileNameList() : QStringList {}) | std::ranges::to<std::set<QString>>(),
			writer
		);
		progress.Increment(1, file.toStdString());
	}
}

QStringList GetArchives(const QStringList& wildCards)
{
	QStringList result;

	for (const auto& wildCard : wildCards)
		std::ranges::move(Util::ResolveWildcard(wildCard), std::back_inserter(result));

	return result;
}

int run(const QCommandLineParser& parser)
{
	try
	{
		QDir dstDir = parser.value(OUTPUT);
		if (!dstDir.exists())
			dstDir.mkpath(".");

		const auto availableLibraries = FliLib::Dump::GetAvailableLibraries();
		auto       sourceLib          = parser.value(LIBRARY);
		if (sourceLib.isEmpty())
			sourceLib = availableLibraries.front();

		if (!availableLibraries.contains(sourceLib, Qt::CaseInsensitive))
			throw std::invalid_argument(std::format("{} must be {}", LIBRARY, availableLibraries.join(" | ")));

		const auto archives = GetArchives(parser.positionalArguments());

		PLOGD << "Total file count calculation";
		const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), size_t { 0 }, [](const auto init, const auto& item) {
			const Zip zip(item);
			return init + zip.GetFileNameList().size();
		});
		PLOGI << "Total file count: " << totalFileCount;

		Util::Progress progress(totalFileCount, "parsing");

		for (const auto& archive : archives)
			ProcessArchive(dstDir, archive, sourceLib, progress);

		return 0;
	}
	catch (const std::exception& ex)
	{
		PLOGE << QString("%1 failed: %2").arg(APP_ID).arg(ex.what());
	}
	catch (...)
	{
		PLOGE << QString("%1 failed").arg(APP_ID);
	}
	return 1;
}

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);
	Util::XMLPlatformInitializer xmlPlatformInitializer;

	const auto availableLibraries = FliLib::Dump::GetAvailableLibraries();

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 creates hash files for library").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives wildcards");
	parser.addOptions({
		{ { "o", OUTPUT }, "Output database path (required)", FOLDER },
		{ LIBRARY, "Library", QString("(%1) [%2]").arg(availableLibraries.join(" | "), availableLibraries.front()) },
	});
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	Log::LoggingInitializer                          logging((parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	if (!parser.isSet(OUTPUT) || parser.positionalArguments().isEmpty())
		parser.showHelp(1);

	return run(parser);
}
