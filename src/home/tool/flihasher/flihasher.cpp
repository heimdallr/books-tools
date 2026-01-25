#include <CImg.h>
#include <QCryptographicHash>

#include <condition_variable>
#include <queue>
#include <ranges>
#include <set>

#include <QBuffer>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QPixmap>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "lib/book.h"
#include "lib/dump/Factory.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/ImageUtil.h"
#include "util/LogConsoleFormatter.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "canny.h"
#include "log.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa;

namespace
{

using namespace cimg_library;

constexpr auto APP_ID = "flihasher";

constexpr auto OUTPUT                       = "output";
constexpr auto FOLDER                       = "folder";
constexpr auto LIBRARY                      = "library";
constexpr auto THREADS                      = "threads";
constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";

CImg<float> GetDctMatrix(const int N)
{
	const auto  n = static_cast<float>(N);
	CImg<float> matrix(N, N, 1, 1, 1 / std::sqrt(n));
	const auto  c1 = std::sqrt(2.0f / n);
	for (int x = 0; x < N; ++x)
		for (int y = 1; y < N; ++y)
			matrix(x, y) = c1 * static_cast<float>(std::cos((cimg::PI / 2.0 / N) * y * (2.0 * x + 1)));
	return matrix;
}

const CImg<float> DCT   = GetDctMatrix(32);
const CImg<float> DCT_T = DCT.get_transpose();
const CImg<float> MEAN_FILTER(7, 7, 1, 1, 1);

struct Options
{
	QDir         dstDir;
	QString      sourceLib;
	QStringList  args;
	unsigned int maxThreadCount { std::thread::hardware_concurrency() };
};

struct ParseResult
{
	QString     id;
	QString     title;
	QString     hashText;
	QStringList hashSections;
};

struct ImageTaskItem
{
	QString    file;
	QByteArray body;
	QString    hash;
	uint64_t   pHash { 0 };
};

struct BookTaskItem
{
	QString                    folder;
	QString                    file;
	QByteArray                 body;
	ImageTaskItem              cover;
	std::vector<ImageTaskItem> images;
	ParseResult                parseResult;
};

uint64_t GetPHash(const QByteArray& body)
{
	auto pixmap = Util::Decode(body);
	if (pixmap.isNull())
		return 0;

	auto       image    = pixmap.toImage();
	const auto hasAlpha = image.pixelFormat().alphaUsage() == QPixelFormat::UsesAlpha;
	image.convertTo(hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_Grayscale8);

	auto          data = new uint8_t[static_cast<size_t>(image.width()) * image.height()];
	CImg<uint8_t> img(data, image.width(), image.height(), 1, 1, true);
	img._is_shared = false;

	if (hasAlpha)
	{
		auto* dst = img.data();
		for (auto h = 0, szh = image.height(), szw = image.width(); h < szh; ++h)
		{
			const auto* src = image.scanLine(h);
			for (auto w = 0; w < szw; ++w, ++dst, src += 4)
				*dst = static_cast<uint8_t>(std::lround((0.299 * src[0] + 0.587 * src[1] + 0.114 * src[2]) * src[3] / 255.0 + (255.0 - src[3])));
		}
	}
	else
	{
		auto* dst = img.data();
		for (auto h = 0, szh = image.height(), szw = image.width(); h < szh; ++h, dst += szw)
			memcpy(dst, image.scanLine(h), szw);
	}

	Canny      canny;
	const auto cropRect = canny.Process(img);
	if (cropRect.width() > img.width() / 2 && cropRect.height() > img.height() / 2)
		img.crop(cropRect.left, cropRect.top, cropRect.right - 1, cropRect.bottom - 1);

	const auto resized = img.get_convolve(MEAN_FILTER).resize(32, 32);
	const auto dct     = (DCT * resized * DCT_T).crop(1, 1, 8, 8);

	return std::accumulate(dct._data, dct._data + 64, uint64_t { 0 }, [median = dct.median()](const uint64_t init, const float value) {
		auto result = init << 1;
		if (value > median)
			result |= 1;

		return result;
	});
}

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

			for (const auto& word : counter | std::views::values | std::views::take(10))
				md5.addData(word.toUtf8());

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
				if (word.length() > 7)
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

class Worker
{
	NON_COPY_MOVABLE(Worker)

public:
	class IObserver // NOLINT(cppcoreguidelines-special-member-functions)
	{
	public:
		virtual ~IObserver()                                       = default;
		virtual void OnFinished(std::vector<BookTaskItem> results) = 0;
	};

public:
	Worker(IObserver& observer, std::condition_variable_any& queueCondition, std::mutex& queueGuard, std::queue<BookTaskItem>& queue, std::atomic<unsigned int>& queueSize, Util::Progress& progress)
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
		const auto         setHash = [&](ImageTaskItem& item) {
            md5.reset();
            md5.addData(item.body);
            item.hash  = QString::fromUtf8(md5.result().toHex());
            item.pHash = GetPHash(item.body);
            item.body.clear();
		};

		PLOGV << "Worker started";
		while (!stopToken.stop_requested() || m_queueSize)
		{
			auto bookTaskItemOpt = Dequeue(stopToken);
			if (!bookTaskItemOpt)
				continue;

			auto& bookTaskItem = *bookTaskItemOpt;

			QBuffer buffer(&bookTaskItem.body);
			buffer.open(QIODevice::ReadOnly);
			Fb2Parser parser(buffer);
			bookTaskItem.parseResult = parser.GetResult();

			if (!bookTaskItem.cover.body.isEmpty())
				setHash(bookTaskItem.cover);
			std::ranges::for_each(bookTaskItem.images, setHash);
			std::ranges::sort(bookTaskItem.images, std::greater {}, [](const auto& item) {
				return -item.file.toInt();
			});

			const auto& result = m_results.emplace_back(std::move(bookTaskItem));

			m_progress.Increment(1, result.file.toStdString());
		}
		PLOGV << "Worker finished";
	}

	std::optional<BookTaskItem> Dequeue(const std::stop_token& stopToken) const
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
	std::queue<BookTaskItem>&    m_queue;
	std::atomic<unsigned int>&   m_queueSize;
	std::jthread                 m_thread { std::bind_front(&Worker::Work, this) };
	Util::Progress&              m_progress;
	std::vector<BookTaskItem>    m_results;
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
	void Enqueue(BookTaskItem bookTaskItem)
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

	std::vector<BookTaskItem>& Wait()
	{
		m_workers.clear();
		PLOGV << "sorting results";
		std::ranges::sort(m_results, {}, [](const auto& item) {
			return item.file;
		});
		return m_results;
	}

private: // Worker::IObserver
	void OnFinished(std::vector<BookTaskItem> results) override
	{
		std::ranges::move(std::move(results), std::back_inserter(m_results));
	}

private:
	std::condition_variable_any&         m_queueCondition;
	std::mutex&                          m_queueGuard;
	std::queue<BookTaskItem>             m_queue;
	std::atomic<unsigned int>            m_queueSize { 0 };
	std::vector<std::unique_ptr<Worker>> m_workers;
	std::vector<BookTaskItem>            m_results;
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
		BookTaskItem bookTaskItem { .folder = fileInfo.fileName(), .file = file, .body = zip.Read(file)->GetStream().readAll() };

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
					return ImageTaskItem { item.split("/").back(), imagesZip->Read(item)->GetStream().readAll() };
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

		const auto writeImage = [&](const QString& nodeName, const ImageTaskItem& item) {
			const auto guard = bookGuard->Guard(nodeName);
			if (!item.file.isEmpty())
				guard->WriteAttribute("id", item.file);
			if (item.pHash)
				guard->WriteAttribute("pHash", QString::number(item.pHash));
			guard->WriteCharacters(item.hash);
		};

		if (!file.cover.hash.isEmpty())
			writeImage(Global::COVER, file.cover);
		for (const auto& item : file.images)
			writeImage(Global::IMAGE, item);

		FliLib::SerializeHashSections(file.parseResult.hashSections, writer);
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

		const auto availableLibraries = FliLib::Dump::GetAvailableLibraries();
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

	const auto availableLibraries = FliLib::Dump::GetAvailableLibraries();

	Options options;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 creates hash files for library").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives wildcards");
	parser.addOptions({
		{ { QString(OUTPUT[0]), OUTPUT }, "Output database path (required)", FOLDER },
		{ LIBRARY, "Source library", QString("(%1) [%2]").arg(availableLibraries.join(" | "), availableLibraries.front()) },
		{ { QString(THREADS[0]), THREADS }, "Maximum number of CPU threads", QString("Thread count [%1]").arg(options.maxThreadCount) },
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

	options.dstDir = parser.value(OUTPUT);

	options.sourceLib = parser.value(LIBRARY);
	if (options.sourceLib.isEmpty())
		options.sourceLib = availableLibraries.front();

	if (parser.isSet(THREADS))
		options.maxThreadCount = parser.value(THREADS).toUInt();

	options.args = parser.positionalArguments();

	return run(options);
}
