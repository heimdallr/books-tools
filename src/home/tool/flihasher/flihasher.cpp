#include <QCryptographicHash>

#include <condition_variable>
#include <queue>
#include <thread>

#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/StrUtil.h"

#include "lib/dump/Factory.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"
#include "util/bookhash/hashbook.h"
#include "util/executor/ThreadPool.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "log.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa::Util;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flihasher";

constexpr auto OUTPUT                       = "output";
constexpr auto FOLDER                       = "folder";
constexpr auto LIBRARY                      = "library";
constexpr auto THREADS                      = "threads";
constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";

struct Options
{
	QDir         dstDir;
	QString      sourceLib;
	QStringList  args;
	unsigned int maxThreadCount { std::thread::hardware_concurrency() };
};

void ProcessArchive(const Options& options, const QString& filePath, Progress& progress)
{
	PLOGI << "process " << filePath;
	assert(options.dstDir.exists());
	BookHashItemProvider bookHashItemProvider(filePath);
	QFileInfo            fileInfo(filePath);

	QFile output(options.dstDir.filePath(fileInfo.completeBaseName() + ".xml"));
	if (!output.open(QIODevice::WriteOnly))
		throw std::ios_base::failure(std::format("Cannot create {}", options.dstDir.filePath(fileInfo.completeBaseName() + ".xml")));

	const auto fileList = bookHashItemProvider.GetFiles();

	std::vector<BookHashItem> bookHashItems;
	bookHashItems.reserve(static_cast<size_t>(fileList.size()));

	ThreadPool<QCryptographicHash> threadPool({ .threadCount = options.maxThreadCount, .maxQueueSize = static_cast<size_t>(options.maxThreadCount) * 2, .contextGetter = [](size_t) {
												   return QCryptographicHash { QCryptographicHash::Md5 };
											   } });
	for (const auto& file : fileList)
	{
		auto& bookTaskItem = bookHashItems.emplace_back(bookHashItemProvider.Get(file));
		threadPool.enqueue([&](QCryptographicHash& md5) {
			ParseBookHash(bookTaskItem, md5);
			progress.Increment(1, bookTaskItem.file.toStdString());
		});
	}

	threadPool.wait();

	XmlWriter  writer(output);
	const auto booksGuard = writer.Guard("books");
	booksGuard->WriteAttribute("source", options.sourceLib);

	PLOGV << "writing results";
	for (const auto& file : bookHashItems)
	{
		const auto bookGuard = writer.Guard("book");
		bookGuard->WriteAttribute("hash", file.parseResult.id)
			.WriteAttribute("id", file.parseResult.hashText)
			.WriteAttribute(Inpx::FOLDER, file.folder)
			.WriteAttribute(Inpx::FILE, file.file)
			.WriteAttribute("title", file.parseResult.title);

		const auto writeImage = [&](const QString& nodeName, const ImageHashItem& item) {
			const auto guard = bookGuard->Guard(nodeName);
			if (!item.file.isEmpty())
				guard->WriteAttribute("id", item.file);
			if (item.pHash)
				guard->WriteAttribute("pHash", QString::number(item.pHash, 16));
			guard->WriteCharacters(item.hash);
		};

		if (!file.cover.hash.isEmpty())
			writeImage(Global::COVER, file.cover);
		for (const auto& item : file.images)
			writeImage(Global::IMAGE, item);

		SerializeHashSections(file.parseResult.hashSections, writer);

		if (!file.parseResult.hashValues.empty())
		{
			const auto histogram = bookGuard->Guard("histogram");
			for (const auto& [count, word] : file.parseResult.hashValues)
			{
				auto histogramItem = histogram->Guard("item");
				histogramItem->WriteAttribute("count", QString::number(count)).WriteAttribute("word", word);
			}
		}

		if (!file.parseResult.annotation.isEmpty())
		{
			const auto guard = bookGuard->Guard("annotation");
			for (const auto& str : file.parseResult.annotation)
				guard->WriteStartElement("p").WriteCharacters(str).WriteEndElement();
		}
	}
}

QStringList GetArchives(const QStringList& wildCards)
{
	QStringList result;

	for (const auto& wildCard : wildCards)
		std::ranges::move(ResolveWildcard(wildCard), std::back_inserter(result));

	return result;
}

int run(const Options& options)
{
	try
	{
		if (!options.dstDir.exists())
			options.dstDir.mkpath(".");

		const auto availableLibraries = Dump::GetAvailableLibraries();
		if (!availableLibraries.contains(options.sourceLib, Qt::CaseInsensitive))
			throw std::invalid_argument(std::format("{} must be {}", LIBRARY, availableLibraries.join(" | ")));

		const auto archives = GetArchives(options.args);

		PLOGD << "Total file count calculation";
		const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), size_t { 0 }, [](const auto init, const auto& item) {
			const Zip zip(item);
			return init + zip.GetFileNameList().size();
		});
		PLOGI << "Total file count: " << totalFileCount;

		Progress progress(totalFileCount, "parsing");

		for (const auto& archive : archives)
			ProcessArchive(options, archive, progress);

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
	const QGuiApplication app(argc, argv);
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);
	XMLPlatformInitializer xmlPlatformInitializer;

	const auto availableLibraries = Dump::GetAvailableLibraries();

	Options options;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 creates hash files for library").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives wildcards");
	parser.addOptions(
		{
			{ { QString(OUTPUT[0]), OUTPUT }, "Output database path (required)", FOLDER },
			{ LIBRARY, "Source library", QString("(%1) [%2]").arg(availableLibraries.join(" | "), availableLibraries.front()) },
			{ { QString(THREADS[0]), THREADS }, "Maximum number of CPU threads", QString("Thread count [%1]").arg(options.maxThreadCount) },
    }
	);
	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	Log::LoggingInitializer                    logging(parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath);
	plog::ConsoleAppender<LogConsoleFormatter> consoleAppender;
	Log::LogAppender                           logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	if (!parser.isSet(OUTPUT) || parser.positionalArguments().isEmpty())
		parser.showHelp(1);

	options.dstDir = parser.value(OUTPUT);

	options.sourceLib = parser.value(LIBRARY);
	if (options.sourceLib.isEmpty())
		options.sourceLib = availableLibraries.front();

	if (parser.isSet(THREADS))
		options.maxThreadCount = parser.value(THREADS).toUInt();

	options.args = parser.positionalArguments();

	return run(options);
}
