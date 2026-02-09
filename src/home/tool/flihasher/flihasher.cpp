#include <QCryptographicHash>

#include <condition_variable>
#include <queue>
#include <ranges>
#include <set>

#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "lib/dump/Factory.h"
#include "lib/hashfb2.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/LogConsoleFormatter.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "log.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
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

class Worker
{
	NON_COPY_MOVABLE(Worker)

public:
	class IObserver // NOLINT(cppcoreguidelines-special-member-functions)
	{
	public:
		virtual ~IObserver()                                       = default;
		virtual void OnFinished(std::vector<BookHashItem> results) = 0;
	};

public:
	Worker(IObserver& observer, std::condition_variable_any& queueCondition, std::mutex& queueGuard, std::queue<BookHashItem>& queue, std::atomic<unsigned int>& queueSize, Util::Progress& progress)
		: m_observer { observer }
		, m_queueCondition { queueCondition }
		, m_queueGuard { queueGuard }
		, m_queue { queue }
		, m_queueSize { queueSize }
		, m_progress { progress }
	{
	}

	~Worker()
	{
		m_thread = {};
		m_observer.OnFinished(std::move(m_results));
	}

private:
	void Work(std::stop_token stopToken)
	{
		QCryptographicHash md5 { QCryptographicHash::Md5 };

		PLOGV << "Worker started";
		while (!stopToken.stop_requested() || m_queueSize)
		{
			auto bookTaskItemOpt = Dequeue(stopToken);
			if (!bookTaskItemOpt)
				continue;

			auto& bookTaskItem = *bookTaskItemOpt;
			ParseFb2Hash(bookTaskItem, md5);
			const auto& result = m_results.emplace_back(std::move(bookTaskItem));

			m_progress.Increment(1, result.file.toStdString());
		}
		PLOGV << "Worker finished";
	}

	std::optional<BookHashItem> Dequeue(const std::stop_token& stopToken) const
	{
		std::unique_lock queueLock(m_queueGuard);
		m_queueCondition.wait(queueLock, stopToken, [&] {
			return !m_queue.empty() || stopToken.stop_requested();
		});

		if (m_queue.empty())
			return std::nullopt;

		auto bookTaskItem = std::move(m_queue.front());
		m_queue.pop();
		--m_queueSize;
		m_queueCondition.notify_all();

		return bookTaskItem;
	}

private:
	IObserver&                   m_observer;
	std::condition_variable_any& m_queueCondition;
	std::mutex&                  m_queueGuard;
	std::queue<BookHashItem>&    m_queue;
	std::atomic<unsigned int>&   m_queueSize;
	std::jthread                 m_thread { std::bind_front(&Worker::Work, this) };
	Util::Progress&              m_progress;
	std::vector<BookHashItem>    m_results;
};

class TaskProcessor final : public Worker::IObserver
{
public:
	TaskProcessor(std::condition_variable_any& queueCondition, std::mutex& queueGuard, const unsigned int poolSize, Util::Progress& progress)
		: m_queueCondition { queueCondition }
		, m_queueGuard { queueGuard }
		, m_workers { std::views::iota(0u, poolSize) | std::views::transform([&](const auto) {
						  return std::make_unique<Worker>(*this, queueCondition, queueGuard, m_queue, m_queueSize, progress);
					  })
		              | std::ranges::to<std::vector<std::unique_ptr<Worker>>>() }
	{
	}

public:
	void Enqueue(BookHashItem bookTaskItem)
	{
		{
			std::unique_lock lock(m_queueGuard);
			m_queue.emplace(std::move(bookTaskItem));
		}
		if (++m_queueSize > 1)
			PLOGV << "Queue size: " << m_queueSize;

		m_queueCondition.notify_all();
	}

	unsigned int GetQueueSize() const
	{
		return m_queueSize;
	}

	std::vector<BookHashItem>& Wait()
	{
		m_workers.clear();
		PLOGV << "sorting results";
		std::ranges::sort(m_results, {}, [](const auto& item) {
			return item.file;
		});
		return m_results;
	}

private: // Worker::IObserver
	void OnFinished(std::vector<BookHashItem> results) override
	{
		std::ranges::move(std::move(results), std::back_inserter(m_results));
	}

private:
	std::condition_variable_any&         m_queueCondition;
	std::mutex&                          m_queueGuard;
	std::queue<BookHashItem>             m_queue;
	std::atomic<unsigned int>            m_queueSize { 0 };
	std::vector<std::unique_ptr<Worker>> m_workers;
	std::vector<BookHashItem>            m_results;
};

void ProcessArchive(const Options& options, const QString& filePath, Util::Progress& progress)
{
	PLOGI << "process " << filePath;
	assert(options.dstDir.exists());
	Zip       zip(filePath);
	QFileInfo fileInfo(filePath);

	const auto getZip = [&](const char* type) -> std::unique_ptr<Zip> {
		const auto path = fileInfo.dir().absoluteFilePath(QString("%1/%2.zip").arg(type, fileInfo.completeBaseName()));
		return QFile::exists(path) ? std::make_unique<Zip>(path) : std::unique_ptr<Zip> {};
	};

	const auto coversZip = getZip(Global::COVERS);
	const auto imagesZip = getZip(Global::IMAGES);

	const auto covers = (coversZip ? coversZip->GetFileNameList() : QStringList {}) | std::ranges::to<std::set<QString>>();
	const auto images = (imagesZip ? imagesZip->GetFileNameList() : QStringList {}) | std::ranges::to<std::set<QString>>();

	QFile output(options.dstDir.filePath(fileInfo.completeBaseName() + ".xml"));
	if (!output.open(QIODevice::WriteOnly))
		throw std::ios_base::failure(std::format("Cannot create {}", options.dstDir.filePath(fileInfo.completeBaseName() + ".xml")));

	const auto fileList = zip.GetFileNameList();

	std::condition_variable_any queueCondition;
	std::mutex                  queueGuard;
	TaskProcessor               processor(queueCondition, queueGuard, std::min(options.maxThreadCount, static_cast<unsigned int>(fileList.size())), progress);

	for (const auto& file : fileList)
	{
		BookHashItem bookTaskItem { .folder = fileInfo.fileName(), .file = file, .body = zip.Read(file)->GetStream().readAll() };

		const auto baseName = QFileInfo(file).completeBaseName();
		if (coversZip && covers.contains(baseName))
			bookTaskItem.cover = { QString {}, coversZip->Read(baseName)->GetStream().readAll() };

		if (imagesZip)
			std::ranges::transform(
				std::ranges::equal_range(
					images,
					baseName + "/",
					{},
					[n = baseName.length() + 1](const QString& item) {
						return QStringView { item.begin(), std::next(item.begin(), n) };
					}
				),
				std::back_inserter(bookTaskItem.images),
				[&](const QString& item) {
					return ImageHashItem { item.split("/").back(), imagesZip->Read(item)->GetStream().readAll() };
				}
			);

		{
			std::unique_lock lockStart(queueGuard);
			queueCondition.wait(lockStart, [&]() {
				return processor.GetQueueSize() < options.maxThreadCount * 2;
			});
		}

		processor.Enqueue(std::move(bookTaskItem));
	}

	Util::XmlWriter writer(output);
	const auto      booksGuard = writer.Guard("books");
	booksGuard->WriteAttribute("source", options.sourceLib);

	PLOGV << "writing results";
	for (const auto& file : processor.Wait())
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

		const auto histogram = bookGuard->Guard("histogram");
		for (const auto& [count, word] : file.parseResult.hashValues)
		{
			auto histogramItem = histogram->Guard("item");
			histogramItem->WriteAttribute("count", QString::number(count)).WriteAttribute("word", word);
		}
	}
}

QStringList GetArchives(const QStringList& wildCards)
{
	QStringList result;

	for (const auto& wildCard : wildCards)
		std::ranges::move(Util::ResolveWildcard(wildCard), std::back_inserter(result));

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

		Util::Progress progress(totalFileCount, "parsing");

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
	Util::XMLPlatformInitializer xmlPlatformInitializer;

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

	Log::LoggingInitializer                          logging((parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
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
