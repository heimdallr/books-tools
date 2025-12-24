#include <queue>
#include <unordered_set>

#include <QBuffer>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <QTimer>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/FindPair.h"

#include "logging/LogAppender.h"
#include "logging/init.h"
#include "network/network/downloader.h"
#include "util/LogConsoleFormatter.h"

#include "log.h"

#include "config/version.h"

using namespace HomeCompa;

namespace
{
constexpr auto APP_ID        = "fliscaner";
constexpr auto OUTPUT_FOLDER = "output-folder";
constexpr auto CONFIG        = "config";
QString        DST_PATH;

class EventLooper
{
public:
	void Add()
	{
		++m_counter;
	}

	void Release()
	{
		if (--m_counter == 0)
			m_eventLoop.exit();
	}

	int Start()
	{
		return m_counter ? m_eventLoop.exec() : 0;
	}

private:
	size_t     m_counter { 0 };
	QEventLoop m_eventLoop;
};

QJsonObject ReadConfig(QFile& file)
{
	assert(file.exists());
	[[maybe_unused]] const auto ok = file.open(QIODevice::ReadOnly);
	assert(ok);

	QJsonParseError jsonParseError;
	const auto      doc = QJsonDocument::fromJson(file.readAll(), &jsonParseError);
	if (jsonParseError.error != QJsonParseError::NoError)
	{
		PLOGE << jsonParseError.errorString();
		return {};
	}

	assert(doc.isObject());
	return doc.object();
}

QJsonObject ReadConfig(const QString& path)
{
	if (QFile file(path); file.exists())
		return ReadConfig(file);

	QFile file(":/config/config.json");
	return ReadConfig(file);
}

QString GetDownloadFileName(const QString& fileName)
{
	return QDir(DST_PATH).filePath(fileName);
}

template <typename T>
void KillMe(T* obj);

bool Validate(const QByteArray& page, const QJsonArray& regexps)
{
	const auto file = QString::fromUtf8(page);
	return std::ranges::any_of(regexps, [&](const auto& item) {
		QRegularExpression rx(item.toString());
		return rx.globalMatch(file).hasNext();
	});
}

bool Validate(const QString& path, const QString& ext)
{
	using namespace std::literals;
	static constexpr auto                                                      empty = "";
	static constexpr std::pair<const char* /*ext*/, std::string_view /*sign*/> signatures[] {
		{ "zip", "PK\x03\x04\x14\x00\x00\x00"sv },
		{  "gz",           "\x1F\x8B\x08\x00"sv },
	};
	const QFileInfo fileInfo(path);
	const auto      signature = FindSecond(signatures, ext.toStdString().data(), empty, PszComparer {});

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;

	const auto content = file.read(static_cast<qsizetype>(signature.size()));
	return content.startsWith(signature);
}

struct Task
{
	EventLooper&               eventLooper;
	Network::Downloader        downloader;
	std::unique_ptr<QIODevice> stream;

	Task(const QString& path, const QString& file, EventLooper& eventLooper, std::unique_ptr<QIODevice> stream, std::function<void(bool)> callback)
		: eventLooper { eventLooper }
		, stream { std::move(stream) }
	{
		eventLooper.Add();

		downloader.Download(
			path + file,
			*this->stream,
			[this_ = this, file, callback = std::move(callback)](size_t, const int code, const QString& message) {
				PLOGI << file << " finished " << (code ? "with " + message : "successfully");
				this_->stream.reset();
				callback(code == 0);

				KillMe(this_);
			},
			[this, file, pct = int64_t { 0 }](const int64_t bytesReceived, const int64_t bytesTotal, bool& /*stopped*/) mutable {
				if (bytesTotal > 0)
				{
					if (const auto currentPct = 100LL * bytesReceived / bytesTotal; currentPct != pct)
					{
						pct = currentPct;
						PLOGI << file << " " << bytesReceived << " (" << bytesTotal << ") " << pct << "%";
					}
				}
			}
		);

		PLOGI << file << " started";
	}

	~Task()
	{
		eventLooper.Release();
	}

	NON_COPY_MOVABLE(Task)
};

template <typename T>
void KillMe(T* obj)
{
	QTimer::singleShot(0, [obj] {
		delete obj;
	});
}

using TaskQueue = std::queue<std::function<void()>>;

void GetFile(const QString& path, const QString& file, const QString& tmpFile, const QString& dstFile, EventLooper& eventLooper, TaskQueue& taskQueue, const int count = 1)
{
	auto stream = std::make_unique<QFile>(tmpFile);
	if (!stream->open(QIODevice::WriteOnly))
	{
		PLOGW << "cannot open " << tmpFile;
		return;
	}

	PLOGI << "download " << path + file << " try " << count;

	new Task(path, file, eventLooper, std::move(stream), [path, file, tmpFile, dstFile, &eventLooper, &taskQueue, count](const bool success) mutable {
		if (success && Validate(tmpFile, QFileInfo(dstFile).suffix().toLower()))
			return (void)QFile::rename(tmpFile, dstFile);

		if (count <= 10)
			return taskQueue.push([path, file, tmpFile, dstFile, &eventLooper, &taskQueue, count] {
				GetFile(path, file, tmpFile, dstFile, eventLooper, taskQueue, count + 1);
			});

		PLOGE << "download " << path + file << " failed";
	});
}

void GetFiles(const QJsonValue& value, EventLooper& eventLooper, TaskQueue& taskQueue)
{
	assert(value.isObject());
	const auto obj  = value.toObject();
	const auto path = obj["path"].toString();
	for (const auto fileObj : obj["file"].toArray())
	{
		const auto file    = fileObj.toString();
		auto       tmpFile = GetDownloadFileName(file + ".tmp"), dstFile = GetDownloadFileName(file);
		if (QFile::exists(dstFile))
		{
			PLOGW << file << " already exists";
			continue;
		}

		taskQueue.push([path, file, tmpFile, dstFile, &eventLooper, &taskQueue] {
			GetFile(path, file, tmpFile, dstFile, eventLooper, taskQueue);
		});
	}
}

void GetDaily(const QJsonArray& regexps, EventLooper& eventLooper, const QString& path, const QString& data, TaskQueue& taskQueue)
{
	std::unordered_set<QString> files;
	for (const auto regexpObj : regexps)
	{
		const auto         regexp = regexpObj.toString();
		QRegularExpression rx(regexp);
		for (const auto& match : rx.globalMatch(data))
			files.insert(match.captured(0));
	}

	QJsonArray filesArray;
	std::ranges::copy(files, std::back_inserter(filesArray));
	const QJsonObject obj {
		{ "path",       path },
		{ "file", filesArray }
	};

	GetFiles(obj, eventLooper, taskQueue);
}

void GetDaily(const QString& path, const QString& file, const QJsonArray& regexps, EventLooper& eventLooper, TaskQueue& taskQueue, const int count = 1)
{
	auto page   = std::make_shared<QByteArray>();
	auto stream = std::make_unique<QBuffer>(page.get());
	stream->open(QIODevice::WriteOnly);

	PLOGI << "download " << path + file << " try " << count;

	new Task(path, file, eventLooper, std::move(stream), [path, file, regexps, &eventLooper, &taskQueue, count, page = std::move(page)](const bool success) {
		if (success && Validate(*page, regexps))
			return GetDaily(regexps, eventLooper, path + file, QString::fromUtf8(*page), taskQueue);

		if (count <= 10)
			return taskQueue.push([path, file, regexps, &eventLooper, &taskQueue, count] {
				GetDaily(path, file, regexps, eventLooper, taskQueue, count + 1);
			});

		PLOGE << "download " << path + file << " failed";
	});
}

void GetDaily(const QJsonValue& value, EventLooper& eventLooper, TaskQueue& taskQueue)
{
	assert(value.isObject());
	const auto obj    = value.toObject();
	const auto path   = obj["path"].toString();
	const auto file   = obj["file"].toString();
	const auto regexp = obj["regexp"];
	assert(regexp.isArray());
	taskQueue.push([path, file, regexps = regexp.toArray(), &eventLooper, &taskQueue] {
		GetDaily(path, file, regexps, eventLooper, taskQueue);
	});
}

void ScanStub(const QJsonValue&, EventLooper&, TaskQueue&)
{
	PLOGE << "unexpected parameter";
}

constexpr std::pair<const char*, void (*)(const QJsonValue&, EventLooper&, TaskQueue&)> SCANNERS[] {
	{ "zip", &GetDaily },
	{ "sql", &GetFiles },
};

} // namespace

int main(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);

	DST_PATH = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 downloads files from Flibusta").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addOptions({
		{ { "o", OUTPUT_FOLDER }, "Output folder",                                                  DST_PATH },
		{        { "c", CONFIG },        "Config", "Apply config or extract it from resources if not exists" },
	});
	parser.addPositionalArgument("sql", "Download dump files");
	parser.addPositionalArgument("zip", "Download book archives");

	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	Log::LoggingInitializer                          logging((parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath).toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	if (parser.positionalArguments().empty())
		parser.showHelp(1);

	if (parser.isSet(OUTPUT_FOLDER))
		DST_PATH = parser.value(OUTPUT_FOLDER);

	if (const QDir dir(DST_PATH); !dir.exists() && !dir.mkpath("."))
	{
		PLOGE << "Cannot create " << DST_PATH;
		parser.showHelp(1);
	}

	auto configFileName = QFileInfo(QString(argv[0])).dir().filePath("config.json");
	if (parser.isSet(CONFIG))
	{
		if (auto value = parser.value(CONFIG); QFile::exists(value))
		{
			configFileName = std::move(value);
		}
		else
		{
			QFile inp(":/config/config.json");
			(void)inp.open(QIODevice::ReadOnly);
			QFile outp(GetDownloadFileName("config.json"));
			(void)outp.open(QIODevice::WriteOnly);
			outp.write(inp.readAll());
		}
	}

	const auto config = ReadConfig(configFileName);

	EventLooper evenLooper;
	TaskQueue   taskQueue;

	for (const auto& arg : parser.positionalArguments())
	{
		const auto invoker = FindSecond(SCANNERS, arg.toStdString().data(), &ScanStub, PszComparer {});
		PLOGI << arg << " in process";
		std::invoke(invoker, config[arg], std::ref(evenLooper), std::ref(taskQueue));
	}

	while (true)
	{
		for (int i = 0; i < 3 && !taskQueue.empty(); ++i)
		{
			auto task = std::move(taskQueue.front());
			taskQueue.pop();
			task();
		}

		evenLooper.Start();

		if (taskQueue.empty())
			break;

		QThread::sleep(std::chrono::seconds(5));
	}

	return 0;
}
