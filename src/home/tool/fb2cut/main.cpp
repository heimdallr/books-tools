#include <QCryptographicHash>

#include <condition_variable>
#include <queue>
#include <ranges>

#include <QBuffer>
#include <QCommandLineParser>
#include <QDirIterator>
#include <QGuiApplication>
#include <QImageReader>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextCodec>
#include <QTranslator>

#include <plog/Appenders/ConsoleAppender.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Record.h>
#include <plog/Util.h>

#include "fnd/FindPair.h"
#include "fnd/NonCopyMovable.h"
#include "fnd/ScopedCall.h"
#include "fnd/algorithm.h"

#include "jxl/jxl.h"
#include "lib/ImageItem.h"
#include "lib/book.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/ISettings.h"
#include "util/ImageUtil.h"
#include "util/LogConsoleFormatter.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/Validator.h"

#include "IParser.h"
#include "log.h"
#include "settings.h"
#include "zip.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa::fb2cut;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID                     = "fb2cut";
constexpr auto MAX_SIZE_OPTION_NAME       = "max-size";
constexpr auto MAX_COVER_SIZE_OPTION_NAME = "max-cover-size";
constexpr auto MAX_IMAGE_SIZE_OPTION_NAME = "max-image-size";

constexpr auto QUALITY_OPTION_NAME       = "quality";
constexpr auto COVER_QUALITY_OPTION_NAME = "cover-quality";
constexpr auto IMAGE_QUALITY_OPTION_NAME = "image-quality";

constexpr auto GRAYSCALE_OPTION_NAME            = "grayscale";
constexpr auto COVER_GRAYSCALE_OPTION_NAME      = "cover-grayscale";
constexpr auto IMAGE_GRAYSCALE_OPTION_NAME      = "image-grayscale";
constexpr auto ARCHIVER_OPTION_NAME             = "archiver";
constexpr auto ARCHIVER_COMMANDLINE_OPTION_NAME = "archiver-options";

constexpr auto MAX_THREAD_COUNT_OPTION_NAME    = "threads";
constexpr auto NO_ARCHIVE_FB2_OPTION_NAME      = "no-archive-fb2";
constexpr auto NO_FB2_OPTION_NAME              = "no-fb2";
constexpr auto NO_IMAGES_OPTION_NAME           = "no-images";
constexpr auto COVERS_ONLY_OPTION_NAME         = "covers-only";
constexpr auto FFMPEG_OPTION_NAME              = "ffmpeg";
constexpr auto MIN_IMAGE_FILE_SIZE_OPTION_NAME = "min-image-file-size";
constexpr auto FORMAT                          = "format";
constexpr auto IMAGE_STATISTICS                = "image-statistics";

constexpr auto QUALITY     = "quality [-1]";
constexpr auto THREADS     = "threads [%1]";
constexpr auto FOLDER      = "folder";
constexpr auto PATH        = "path";
constexpr auto COMMANDLINE = "list of options";
constexpr auto SIZE        = "size [INT_MAX,INT_MAX]";

struct DataItem
{
	QString    fileName;
	QByteArray body;
	QDateTime  dateTime;
};

using DataItems = std::queue<DataItem>;

struct ImageStatisticsItem
{
	enum class PixelSchema
	{
		Unknown = -1,
		Normal,
		GrayScale,
		Alpha,
	};
	QString     folder;
	QString     fileName;
	QString     imageId;
	QString     fail;
	bool        isCover { false };
	qsizetype   size { 0 };
	int         width { 0 };
	int         height { 0 };
	PixelSchema schema { PixelSchema::Unknown };
	QString     hash;
};

using ImageStatistics = std::vector<ImageStatisticsItem>;

std::pair<QImage, QString> ToImage(QByteArray& body)
{
	QBuffer buffer(&body);
	buffer.open(QBuffer::ReadOnly);
	QImageReader               imageReader(&buffer);
	std::pair<QImage, QString> result { imageReader.read(), {} };
	if (result.first.isNull())
		result.second = imageReader.errorString();

	return result;
}

class Worker
{
	NON_COPY_MOVABLE(Worker)

public:
	class IClient // NOLINT(cppcoreguidelines-special-member-functions)
	{
	public:
		virtual ~IClient() = default;

		virtual void OnWorkFinished(ImageStatistics imageStatistics, ImageItems covers, ImageItems images) = 0;
	};

public:
	Worker(
		const Settings&          settings,
		QString                  folder,
		const IEncodingDetector& encodingDetector,
		std::condition_variable& queueCondition,
		std::mutex&              queueGuard,
		DataItems&               queue,
		std::mutex&              fileSystemGuard,
		std::atomic_bool&        hasError,
		std::atomic_int&         queueSize,
		Util::Progress&          progress,
		IClient&                 client,
		const Decoder&           decoder
	)
		: m_settings { settings }
		, m_folder { std::move(folder) }
		, m_encodingDetector { encodingDetector }
		, m_queueCondition { queueCondition }
		, m_queueGuard { queueGuard }
		, m_queue { queue }
		, m_fileSystemGuard { fileSystemGuard }
		, m_hasError { hasError }
		, m_queueSize { queueSize }
		, m_progress { progress }
		, m_client { client }
		, m_decoder { decoder }
		, m_thread { &Worker::Process, this }
	{
	}

	~Worker()
	{
		if (m_thread.joinable())
			m_thread.join();
	}

private:
	void Process()
	{
		while (true)
		{
			QString    name;
			QByteArray body;
			QDateTime  dateTime;
			{
				std::unique_lock queueLock(m_queueGuard);
				m_queueCondition.wait(queueLock, [&]() {
					return !m_queue.empty();
				});

				auto [f, d, t] = std::move(m_queue.front());
				m_queue.pop();
				--m_queueSize;
				m_queueCondition.notify_all();
				name     = std::move(f);
				body     = std::move(d);
				dateTime = std::move(t);
			}

			if (body.isEmpty())
				break;

			if (ProcessFile(name, body, dateTime))
			{
				m_hasError = true;
				PLOGE << "processed with error: " << name;
			}
		}

		m_client.OnWorkFinished(std::move(m_imageStatistics), std::move(m_covers), std::move(m_images));
	}

	bool ProcessFile(const QString& inputFilePath, const QByteArray& inputFileBody, const QDateTime& dateTime)
	{
		const ScopedCall logGuard([&] {
			m_progress.Increment(1, inputFilePath.toStdString());
		});

		const QFileInfo fileInfo(inputFilePath);
		const auto      parser = [&]() -> std::unique_ptr<IParser> {
			QString errorText;
			try
			{
				auto result = IParser::Create(inputFilePath, inputFileBody, m_encodingDetector, m_decoder, m_validator);
				if (!result)
				{
					PLOGD << "no parsers found for " << inputFilePath;
				}
				return result;
			}
			catch (const std::exception& ex)
			{
				errorText = ex.what();
			}
			catch (...)
			{
				errorText = "unknown error";
			}
			WriteError(inputFilePath, inputFileBody, errorText, true, fileInfo.suffix());
			return {};
		}();

		if (!parser)
			return false;

		auto [name, body] = ParseFile(*parser, dateTime);

		if (body.isEmpty())
			return WriteError(inputFilePath, inputFileBody, "no output found", true, fileInfo.suffix()), false;

		if (!m_settings.saveFb2)
			return false;

		std::scoped_lock fileSystemLock(m_fileSystemGuard);
		const auto       outputFilePath = m_settings.dstDir.filePath(name);
		const QFileInfo  outputFileInfo(outputFilePath);
		if (auto dir = outputFileInfo.dir(); !dir.exists())
			dir.mkpath(".");

		QFile bodyFile(outputFilePath);
		if (!bodyFile.open(QIODevice::WriteOnly))
		{
			PLOGW << QString("Cannot write body to %1").arg(outputFilePath);
			return true;
		}

		if (bodyFile.write(body) != body.size())
			return true;

		return !bodyFile.setFileTime(dateTime, QFile::FileTime::FileBirthTime);
	}

	IParser::OutputFile ParseFile(IParser& parser, const QDateTime& dateTime)
	{
		const QFileInfo fileInfo(parser.GetInputFileName());
		const auto      completeFileName = fileInfo.completeBaseName();

		static constexpr const char* passThruBinTypes[] = { "zip", "rar", "txt", "pdf" };

		const auto encode = [this](const ImageSettings& settings, const QString& fileName, const QImage& image, const QByteArray& body) -> QByteArray {
			if (fileName.isEmpty())
				return {};

			if (auto bytes = JXL::Encode(image, settings.quality); !bytes.isEmpty())
				return bytes;

			(void)AddError(settings, fileName, body, QString("Cannot compress %1 %2").arg(settings.type).arg(fileName), true, {}, false);
			return {};
		};

		std::unordered_map<QString, int> uniqueData;
		IParser::ImageMapper             idToNum;

		auto binaryCallback = [&](QString&& name, const bool isCover, QByteArray body) {
			ImageStatisticsItem::PixelSchema pixelSchema = ImageStatisticsItem::PixelSchema::Unknown;

			int         width  = 0;
			int         height = 0;
			const char* fail   = nullptr;

			ScopedCall statGuard([&, name]() mutable {
				if (m_settings.imageStatistics.isEmpty())
					return;

				m_hash.reset();
				m_hash.addData(body);
				m_imageStatistics.emplace_back(m_folder, completeFileName, std::move(name), fail, isCover, body.size(), width, height, pixelSchema, QString::fromUtf8(m_hash.result().toHex()));
			});

			const QFileInfo imageFileInfo(name);
			if (const auto it = std::ranges::find_if(
					passThruBinTypes,
					[ext = imageFileInfo.suffix().toLower()](const char* type) {
						return ext == type;
					}
				);
			    it != std::end(passThruBinTypes))
			{
				fail           = *it;
				auto imageFile = m_settings.image.fileNameGetter(completeFileName, name);
				m_hash.reset();
				m_hash.addData(body);

				ImageItem imageItem { .fileName = std::move(imageFile), .body = body, .dateTime = dateTime, .hash = QString::fromUtf8(m_hash.result().toHex()) };

				if (!m_settings.image.save)
					imageItem.body = {};

				(isCover ? m_covers : m_images).emplace_back(std::move(imageItem));
				return;
			}

			const auto& settings = isCover ? m_settings.cover : m_settings.image;

			auto image = ReadImage(body, settings, settings.fileNameGetter(completeFileName, name), fail, settings.save);
			if (image.isNull())
				return;

			width  = image.width();
			height = image.height();

			if (image.pixelFormat().colorModel() == QPixelFormat::Grayscale)
				pixelSchema = ImageStatisticsItem::PixelSchema::GrayScale;

			if (settings.grayscale)
				image.convertTo(QImage::Format::Format_Grayscale8);

			if (image.pixelFormat().colorModel() != QPixelFormat::Grayscale)
				image = Util::HasAlpha(image, body.constData());

			const auto pixelFormat = image.pixelFormat();
			const bool hasAlpha    = pixelFormat.alphaUsage() == QPixelFormat::UsesAlpha;
			if (pixelSchema == ImageStatisticsItem::PixelSchema::Unknown)
				pixelSchema = hasAlpha ? ImageStatisticsItem::PixelSchema::Alpha : ImageStatisticsItem::PixelSchema::Normal;

			if (image.width() > settings.maxSize.width() || image.height() > settings.maxSize.height())
				image = image.scaled(settings.maxSize.width(), settings.maxSize.height(), Qt::KeepAspectRatio, hasAlpha ? Qt::FastTransformation : Qt::SmoothTransformation);

			m_hash.reset();
			for (auto h = 0, szH = image.height(), szW = image.width(); h < szH; ++h)
				m_hash.addData(QByteArrayView { std::bit_cast<const char*>(image.constScanLine(h)), static_cast<qsizetype>(szW) * pixelFormat.channelCount() });
			auto hash = QString::fromUtf8(m_hash.result().toHex());

			if (const auto it = uniqueData.find(hash); it != uniqueData.end())
			{
				if (isCover)
					it->second = -1;

				idToNum.try_emplace(std::move(name), it->second);
				return;
			}

			const auto [it, added] = uniqueData.try_emplace(std::move(hash), isCover ? -1 : static_cast<int>(uniqueData.size()));

			const auto num       = it->second;
			auto       imageFile = settings.fileNameGetter(completeFileName, isCover ? name : QString::number(num));
			idToNum.try_emplace(std::move(name), num);

			if (!settings.save)
				return;

			ImageItem imageItem { .fileName = std::move(imageFile), .body = body, .dateTime = dateTime, .hash = it->first };
			if (auto encoded = encode(m_settings.cover, imageItem.fileName, image, imageItem.body); encoded.size() < imageItem.body.size())
				imageItem.body = std::move(encoded);
			(isCover ? m_covers : m_images).emplace_back(std::move(imageItem));
		};

		QString errorText;
		try
		{
			return parser.Parse(std::move(binaryCallback), idToNum);
		}
		catch (const std::exception& ex)
		{
			errorText = ex.what();
		}
		catch (...)
		{
			errorText = "unknown error";
		}

		WriteError(fileInfo.filePath(), parser.GetInputFileBody(), errorText, true, fileInfo.suffix());
		return {};
	}

	QImage ReadImage(QByteArray& body, const ImageSettings& settings, const QString& imageFile, const char*& fail, const bool needSaveBody) const
	{
		struct Signature
		{
			const char* extension { nullptr };
			const char* signature { nullptr };
			bool        needSaveBody { true };
		};

		static constexpr Signature signatures[] {
			{ "jpg", "\xFF\xD8\xFF\xE0" },
			{ "png", "\x89\x50\x4E\x47" },
		};
		static constexpr Signature unsupportedSignatures[] {
			{ "riff", R"(RIFF)" },
		};
		static constexpr Signature knownSignatures[] {
			{ "html", R"(<html)", false },
			{ "xml", R"(<?xml)" },
			{ "svg", R"(<svg)" },
		};

		static constexpr const char* base64Signatures[] {
			"/9j/4A",
			"iVBORw",
		};

		if (const auto it = std::ranges::find_if(
				base64Signatures,
				[&](const auto* item) {
					return body.startsWith(item);
				}
			);
		    it != std::end(base64Signatures))
		{
			body = QByteArray::fromBase64(body);
			return ReadImage(body, settings, imageFile, fail, needSaveBody);
		}

		auto [image, errorString] = ToImage(body);
		if (!image.isNull())
			return image;

		if (body.size() < m_settings.minImageFileSize)
		{
			PLOGW << QString("%1 %2 too small file size: %3").arg(settings.type).arg(imageFile).arg(body.size());
			return {};
		}

		if (const auto it = std::ranges::find_if(
				signatures,
				[&](const auto& item) {
					return body.startsWith(item.signature);
				}
			);
		    it != std::end(signatures))
			return (fail = it->extension),
			       AddError(settings, imageFile, body, QString("%1 %2 may be damaged: %3").arg(settings.type).arg(imageFile).arg(errorString), needSaveBody && it->needSaveBody, it->extension);

		if (const auto it = std::ranges::find_if(
				unsupportedSignatures,
				[&](const auto& item) {
					return body.startsWith(item.signature);
				}
			);
		    it != std::end(unsupportedSignatures))
			return (fail = it->extension),
			       AddError(settings, imageFile, body, QString("possibly an %1 %2 in %3 format").arg(settings.type).arg(imageFile).arg(it->extension), needSaveBody && it->needSaveBody, it->extension);

		if (const auto it = std::ranges::find_if(
				knownSignatures,
				[&](const auto& item) {
					return body.startsWith(item.signature);
				}
			);
		    it != std::end(knownSignatures))
			return (fail = it->extension),
			       AddError(settings, imageFile, body, QString("%1 %2 is %3").arg(settings.type).arg(imageFile).arg(it->extension), needSaveBody && it->needSaveBody, it->extension, false);

		if (QString::fromUtf8(body).contains("!doctype html", Qt::CaseInsensitive))
			return fail = knownSignatures[0].extension, AddError(settings, imageFile, body, QString("possibly an %1 %2 in %3 format").arg(settings.type).arg(imageFile).arg("html"), false, "html", false);

		return AddError(settings, imageFile, body, QString("%1 %2 may be damaged: %3").arg(settings.type).arg(imageFile).arg(errorString), needSaveBody);
	}

	QImage AddError(const ImageSettings& settings, const QString& file, const QByteArray& body, const QString& errorText, const bool needSaveBody, const QString& ext = {}, const bool tryToFix = true) const
	{
		if (tryToFix)
			if (auto fixed = TryToFix(settings, file, body); !fixed.isNull())
				return fixed;

		WriteError(file, body, errorText, needSaveBody, ext);
		return {};
	}

	void WriteError(const QString& file, const QByteArray& body, const QString& errorText, const bool needSaveBody, const QString& ext) const
	{
		PLOGW << file << ": " << errorText;
		if (needSaveBody)
		{
			WriteErrorFile(m_settings.dstDir, m_fileSystemGuard, file, ext, body);
			m_hasError = true;
		}
	}

	QImage TryToFix(const ImageSettings& settings, const QString& imageFile, const QByteArray& body) const
	{
		if (m_settings.ffmpeg.isEmpty() || body.size() < 128)
			return {};

		QProcess   process;
		QEventLoop eventLoop;
		const auto args           = QStringList() << "-i" << "pipe:0" << "-f" << "mjpeg" << "-vf"
		                                          << QString("scale='min(%1,iw)':min'(%2,ih)':force_original_aspect_ratio=decrease").arg(settings.maxSize.width()).arg(settings.maxSize.height()) << "pipe:1";
		const auto ffmpegFileName = QFileInfo(m_settings.ffmpeg).fileName();

		QByteArray fixed;
		QObject::connect(&process, &QProcess::started, [&] {
			PLOGI << QString("ffmpeg launched for %1 %2\n%3 %4").arg(settings.type, imageFile, ffmpegFileName, args.join(" "));
		});
		QObject::connect(&process, &QProcess::finished, [&](const int code, const QProcess::ExitStatus) {
			if (code == 0)
				PLOGI << QString("%1 %2 is probably fixed").arg(settings.type, imageFile);
			else
				PLOGW << QString("Cannot fix %1 %2, ffmpeg finished with %3").arg(settings.type, imageFile).arg(code);
			eventLoop.exit(code);
		});
		QObject::connect(&process, &QProcess::readyReadStandardError, [&] {
			PLOGW << "\n" << process.readAllStandardError();
		});
		QObject::connect(&process, &QProcess::readyReadStandardOutput, [&] {
			fixed.append(process.readAllStandardOutput());
		});

		process.start(m_settings.ffmpeg, args, QIODevice::ReadWrite);
		process.write(body);
		process.closeWriteChannel();

		eventLoop.exec();

		if (fixed.isEmpty())
			return {};

		auto [image, errorString] = ToImage(fixed);
		if (!errorString.isEmpty())
		{
			PLOGW << errorString;
		}

		return image;
	}

private:
	const Settings&          m_settings;
	const QString            m_folder;
	const IEncodingDetector& m_encodingDetector;

	std::condition_variable& m_queueCondition;
	std::mutex&              m_queueGuard;
	DataItems&               m_queue;
	std::mutex&              m_fileSystemGuard;

	std::atomic_bool& m_hasError;
	std::atomic_int&  m_queueSize;
	Util::Progress&   m_progress;

	QCryptographicHash m_hash { QCryptographicHash::Md5 };
	ImageStatistics    m_imageStatistics;

	ImageItems m_images;
	ImageItems m_covers;

	const Util::XmlValidator m_validator;

	IClient&       m_client;
	const Decoder& m_decoder;

	std::thread m_thread;
};

QString GetImagesFolder(const QDir& dir, const QString& type)
{
	const QFileInfo fileInfo(dir.path());
	return QString("%1/%2/%3.zip").arg(fileInfo.dir().path(), type, fileInfo.fileName());
}

class FileProcessor final : public Worker::IClient
{
public:
	FileProcessor(
		const Settings&          settings,
		const QString&           folder,
		const IEncodingDetector& encodingDetector,
		std::condition_variable& queueCondition,
		std::mutex&              queueGuard,
		const int                poolSize,
		Util::Progress&          progress,
		QTextStream*             imageStatisticsStream,
		const Decoder&           decoder
	)
		: m_queueCondition { queueCondition }
		, m_queueGuard { queueGuard }
		, m_dstDir { settings.dstDir }
		, m_saveCovers { settings.cover.save }
		, m_saveImages { settings.image.save }
		, m_maxThreadCount { settings.maxThreadCount }
		, m_imageStatisticsStream { imageStatisticsStream }
	{
		for (int i = 0; i < poolSize; ++i)
			m_workers.push_back(std::make_unique<Worker>(settings, folder, encodingDetector, m_queueCondition, m_queueGuard, m_queue, m_fileSystemGuard, m_hasError, m_queueSize, progress, *this, decoder));
	}

public:
	int GetQueueSize() const
	{
		return m_queueSize;
	}

	void Enqueue(QString file, QByteArray data, QDateTime dateTime)
	{
		std::unique_lock lock(m_queueGuard);
		m_queue.emplace(std::move(file), std::move(data), std::move(dateTime));
		++m_queueSize;
		m_queueCondition.notify_all();
	}

	bool HasError() const
	{
		return m_hasError;
	}

	void Wait()
	{
		m_workers.clear();
		WriteImageStatistics();
		ArchiveImages(m_covers, m_images);
	}

	void ArchiveImages(ImageItems& covers, ImageItems& images) const
	{
		ArchiveImages(m_saveImages, Global::IMAGES, images);
		ArchiveImages(m_saveCovers, Global::COVERS, covers);
	}

private:
	void ArchiveImages(const bool saveFlag, const char* type, ImageItems& images) const //-V826
	{
		if (!saveFlag || images.empty())
			return;

		const auto archiveFileName = GetImagesFolder(m_dstDir, type);
		PLOGI << "archive " << archiveFileName << ", total:" << images.size();

		QFile::remove(archiveFileName);

		std::unordered_map<QString, qsizetype> unique;
		for (const auto& image : images)
		{
			auto& item = unique[image.fileName];
			item       = std::max(item, image.body.size());
		}

		std::erase_if(images, [&](const auto& image) {
			const auto it = unique.find(image.fileName);
			assert(it != unique.end());
			return image.body.size() < it->second;
		});

		const auto proj = [](const auto& item) {
			return item.fileName;
		};
		std::ranges::sort(images, {}, proj);
		if (const auto range = std::ranges::unique(images, {}, proj); !range.empty())
			images.erase(range.begin(), range.end()); //-V539

		auto zipFiles = Zip::CreateZipFileController();
		for (auto&& image : images)
			zipFiles->AddFile(std::move(image.fileName), image.body, std::move(image.dateTime));

		Zip zip(archiveFileName, Zip::Format::Zip);
		zip.SetProperty(Zip::PropertyId::CompressionLevel, QVariant::fromValue(Zip::CompressionLevel::Ultra));
		zip.SetProperty(Zip::PropertyId::ThreadsCount, m_maxThreadCount);
		zip.Write(*zipFiles);

		images.clear();
	}

	void WriteImageStatistics()
	{
		ScopedCall clearGuard([this] {
			m_imageStatistics.clear();
		});

		if (!m_imageStatisticsStream)
			return;

		for (const auto& [folder, fileName, imageId, fail, isCover, size, width, height, schema, hash] : m_imageStatistics)
			(*m_imageStatisticsStream) << folder << '|' << fileName << '|' << imageId << '|' << fail << '|' << (isCover ? 1 : 0) << '|' << static_cast<int>(schema) << '|' << size << '|' << width << '|'
									   << height << '|' << hash << '\n';

		m_imageStatisticsStream->flush();
	}

private: // Worker::IClient
	void OnWorkFinished(ImageStatistics imageStatistics, ImageItems covers, ImageItems images) override
	{
		std::lock_guard lock(m_workClientGuard);
		m_imageStatistics.reserve(m_imageStatistics.size() + imageStatistics.size());
		std::ranges::move(std::move(imageStatistics), std::back_inserter(m_imageStatistics));
		std::ranges::move(std::move(covers), std::back_inserter(m_covers));
		std::ranges::move(std::move(images), std::back_inserter(m_images));
	}

private:
	std::condition_variable& m_queueCondition;
	std::mutex&              m_queueGuard;
	std::atomic_bool         m_hasError { false };
	DataItems                m_queue;
	std::atomic_int          m_queueSize { 0 };
	std::mutex               m_fileSystemGuard;

	std::mutex m_workClientGuard;

	QDir       m_dstDir;
	const bool m_saveCovers;
	const bool m_saveImages;
	const int  m_maxThreadCount;

	ImageStatistics m_imageStatistics;
	ImageItems      m_covers;
	ImageItems      m_images;
	QTextStream*    m_imageStatisticsStream;

	std::vector<std::unique_ptr<Worker>> m_workers;
};

bool ArchiveFb2External(const Settings& settings)
{
	if (!settings.saveFb2 || settings.archiver.isEmpty())
		return false;

	PLOGI << "launching an external archiver";

	QProcess   process;
	QEventLoop eventLoop;
	auto       args = settings.archiverOptions.split(' ', Qt::SkipEmptyParts);
	for (auto& arg : args)
	{
		arg.replace("%src%", QString("%1/*.fb2").arg(settings.dstDir.path()));
		arg.replace("#src#", QString("%1/*.fb2").arg(settings.dstDir.path()));

		arg.replace("%dst%", QString("%1").arg(settings.dstDir.path()));
		arg.replace("#dst#", QString("%1").arg(settings.dstDir.path()));
	}

	bool hasErrors = false;

	QObject::connect(&process, &QProcess::started, [&] {
		PLOGI << "external archiver launched\n" << QFileInfo(settings.archiver).fileName() << " " << args.join(' ');
	});
	QObject::connect(&process, &QProcess::finished, [&](const int code, const QProcess::ExitStatus) {
		if (code == 0)
			PLOGI << QFileInfo(settings.archiver).fileName() << " finished successfully";
		else
			PLOGW << QFileInfo(settings.archiver).fileName() << " finished with " << code;
		eventLoop.exit(code);
	});
	QObject::connect(&process, &QProcess::readyReadStandardError, [&] {
		hasErrors = true;
		PLOGE << process.readAllStandardError();
	});
	QObject::connect(&process, &QProcess::readyReadStandardOutput, [&] {
		PLOGI << process.readAllStandardOutput();
	});

	process.start(settings.archiver, args, QIODevice::ReadOnly);

	eventLoop.exec();
	return hasErrors;
}

bool ArchiveFb2(const Settings& settings)
{
	if (!settings.archiveFb2)
		return false;

	if (!settings.archiver.isEmpty())
		return ArchiveFb2External(settings);

	auto zipFiles = Zip::CreateZipFileController();
	for (QDirIterator it(settings.dstDir.path(), QStringList() << "*", QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
	{
		const auto file = it.next();
		QFile      stream(file);
		if (!stream.open(QIODevice::ReadOnly))
		{
			PLOGW << "cannot read " << file;
			return false;
		}

		zipFiles->AddFile(settings.dstDir.relativeFilePath(file), stream.readAll(), QFileInfo(file).birthTime());
	}

	if (zipFiles->GetCount() == 0)
	{
		PLOGW << "No text files found";
		return false;
	}

	const auto dstArchiveFileName = QString("%1.%2").arg(settings.dstDir.path(), Zip::FormatToString(settings.format));
	PLOGI << "archive " << dstArchiveFileName << ", total: " << zipFiles->GetCount();

	QFile::remove(dstArchiveFileName);

	Zip zip(dstArchiveFileName, settings.format);
	zip.SetProperty(Zip::PropertyId::CompressionLevel, QVariant::fromValue(Zip::CompressionLevel::Ultra));
	zip.SetProperty(Zip::PropertyId::SolidArchive, false);
	zip.SetProperty(Zip::PropertyId::ThreadsCount, settings.maxThreadCount);
	if (settings.format == Zip::Format::SevenZip)
		zip.SetProperty(Zip::PropertyId::CompressionMethod, QVariant::fromValue(Zip::CompressionMethod::Ppmd));

	const auto result = zip.Write(*zipFiles);
	if (result)
		QDir(settings.dstDir).removeRecursively();

	return !result;
}

bool ProcessArchiveImpl(const QString& archive, Settings settings, const IEncodingDetector& encodingDetector, Util::Progress& progress, QTextStream* imageStatisticsStream, const Decoder& decoder)
{
	const QFileInfo fileInfo(archive);
	settings.dstDir = QDir(settings.dstDir.filePath(fileInfo.completeBaseName()));
	if (!settings.dstDir.exists() && !settings.dstDir.mkpath("."))
	{
		PLOGE << QString("Cannot create folder %1").arg(settings.dstDir.path());
		return true;
	}

	const Zip  zip(archive);
	auto       fileList         = zip.GetFileNameList() | std::views::reverse | std::ranges::to<QStringList>();
	const auto fileListCount    = static_cast<size_t>(fileList.size());
	const auto currentFileCount = progress.GetCount();
	PLOGI << QString("%1 processing, total files: %2").arg(fileInfo.fileName()).arg(fileListCount);

	auto hasError = [&] {
		const auto maxThreadCount = std::min(std::max(settings.maxThreadCount, 1), static_cast<int>(fileListCount));

		std::condition_variable queueCondition;
		std::mutex              queueGuard;
		FileProcessor           fileProcessor(settings, fileInfo.completeBaseName(), encodingDetector, queueCondition, queueGuard, maxThreadCount, progress, imageStatisticsStream, decoder);

		while (!fileList.isEmpty())
		{
			if (fileProcessor.GetQueueSize() < maxThreadCount * 2)
			{
				const auto input = zip.Read(fileList.front());
				auto       body  = input->GetStream().readAll();
				if (!body.isEmpty())
				{
					fileProcessor.Enqueue(std::move(fileList.front()), std::move(body), zip.GetFileTime(fileList.front()));
				}
				else
				{
					PLOGW << fileList.front() << " is empty";
					progress.Increment(1, fileList.front().toStdString());
				}
				fileList.pop_front();
			}
			else
			{
				std::unique_lock lockStart(queueGuard);
				queueCondition.wait(lockStart, [&]() {
					return fileProcessor.GetQueueSize() < maxThreadCount * 2;
				});
			}
		}

		for (int i = 0; i < maxThreadCount; ++i)
			fileProcessor.Enqueue({}, {}, {});

		fileProcessor.Wait();

		return fileProcessor.HasError();
	}();

	hasError = ArchiveFb2(settings) || hasError;

	QDir().rmdir(settings.dstDir.path());

	const auto fileCount = progress.GetCount();
	if (fileCount - currentFileCount != fileListCount)
	{
		PLOGE << QString("something strange: %1 files in archive %2 but processed %3").arg(fileListCount).arg(fileInfo.fileName()).arg(fileCount - currentFileCount);
	}

	const auto resultReport = QString("%1 (%2 of %3 files) processed %4").arg(fileInfo.fileName()).arg(fileCount - currentFileCount).arg(fileListCount).arg(hasError ? "with errors" : "successfully");
	if (hasError)
		PLOGW << resultReport;
	else
		PLOGI << resultReport;

	return hasError;
}

bool ProcessArchive(const QString& file, const Settings& settings, const IEncodingDetector& encodingDetector, Util::Progress& progress, QTextStream* imageStatisticsStream, const Decoder& decoder)
{
	try
	{
		return ProcessArchiveImpl(file, settings, encodingDetector, progress, imageStatisticsStream, decoder);
	}
	catch (const std::exception& ex)
	{
		PLOGE << QString("%1 processing failed: %2").arg(file).arg(ex.what());
	}
	catch (...)
	{
		PLOGE << QString("%1 processing failed").arg(file);
	}

	return true;
}

QStringList ProcessArchives(Settings& settings)
{
	if (!settings.dstDir.exists() && !settings.dstDir.mkpath("."))
		throw std::ios_base::failure(QString("Cannot create folder %1").arg(settings.dstDir.path()).toStdString());
	if (settings.cover.save)
		if (const QDir dir(QString("%1/%2").arg(settings.dstDir.path(), Global::COVERS)); !dir.exists() && !dir.mkpath("."))
			throw std::ios_base::failure(QString("Cannot create folder %1").arg(dir.path()).toStdString());
	if (settings.image.save)
		if (const QDir dir(QString("%1/%2").arg(settings.dstDir.path(), Global::IMAGES)); !dir.exists() && !dir.mkpath("."))
			throw std::ios_base::failure(QString("Cannot create folder %1").arg(dir.path()).toStdString());

	QStringList files;
	for (const auto& wildCard : settings.inputWildcards)
		std::ranges::move(Util::ResolveWildcard(wildCard), std::back_inserter(files));

	std::multimap<int, QString> sorted;

	QRegularExpression rx("^.*?fb2.*?([0-9]+).*?$");
	std::ranges::transform(std::move(files), std::inserter(sorted, sorted.end()), [&](QString file) {
		const auto fileName = QFileInfo(file).completeBaseName();
		const auto match    = rx.match(fileName);
		return std::make_pair(match.hasMatch() ? match.captured(1).toInt() : 0, std::move(file));
	});

	PLOGD << "Total file count calculation";
	settings.totalFileCount = std::accumulate(sorted.cbegin(), sorted.cend(), settings.totalFileCount, [](const auto init, const auto& item) {
		const Zip zip(item.second);
		return init + zip.GetFileNameList().size();
	});
	PLOGI << "Total file count: " << settings.totalFileCount;

	std::unique_ptr<QTextStream> imageStatisticsStream;
	QFile                        imageStatisticsFile(settings.imageStatistics);
	if (!settings.imageStatistics.isEmpty())
	{
		if (!imageStatisticsFile.open(QIODevice::Append))
			throw std::ios_base::failure(QString("Cannot write to %1").arg(settings.imageStatistics).toStdString());
		imageStatisticsStream = std::make_unique<QTextStream>(&imageStatisticsFile);
		*imageStatisticsStream << "#ARCHIVE|FB2_FILE|IMAGE_ID|FAIL_INFO|IS_COVER|PIXEL_TYPE|IMAGE_FILE_SIZE|WIDTH|HEIGHT|HASH\n";
		imageStatisticsStream->flush();
	}

	const Decoder decoder;
	const auto    encodingDetector = IEncodingDetector::Create();

	Util::Progress progress(settings.totalFileCount, "repacking e-library");

	QStringList failed;
	for (auto&& file : sorted | std::views::values | std::views::reverse)
		if (ProcessArchive(file, settings, *encodingDetector, progress, imageStatisticsStream.get(), decoder))
			failed << std::move(file);

	return failed;
}

template <typename T>
bool SetValue(const QCommandLineParser& parser, const char* key, T& value) = delete;

template <>
bool SetValue<int>(const QCommandLineParser& parser, const char* key, int& value)
{
	bool ok = false;
	if (const auto parsed = parser.value(key).toInt(&ok); ok)
		value = parsed;

	return ok;
}

template <>
bool SetValue<QSize>(const QCommandLineParser& parser, const char* key, QSize& value)
{
	const auto parsed = parser.value(key).split(',', Qt::SkipEmptyParts);
	if (parsed.isEmpty())
		return false;

	if (parsed.size() < 2)
	{
		bool ok = false;

		if (const auto v = parsed.front().toInt(&ok); !ok)
		{
			return false;
		}
		else
		{
			value.setWidth(v);
			value.setHeight(v);
		}

		return true;
	}

	QSize v;
	bool  ok = false;

	if (v.setWidth(parsed.front().toInt(&ok)); !ok)
		return false;

	if (v.setHeight(parsed.back().toInt(&ok)); !ok)
		return false;

	value = v;
	return true;
}

Settings ProcessCommandLine(const QCoreApplication& app)
{
	Settings settings {};

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 extracts images from *.fb2").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument("wildcard [wildcard [...]]", "Input archive files (required)");
	parser.addOptions(
		{
			{ { "o", FOLDER }, "Output folder (required)", FOLDER },
			{ { QString(QUALITY[0]), QUALITY_OPTION_NAME }, "Compression quality [0, 100] or -1 for default compression quality", QUALITY },
			{ { QString(THREADS[0]), MAX_THREAD_COUNT_OPTION_NAME }, "Maximum number of CPU threads", QString(THREADS).arg(settings.maxThreadCount) },
			{ { QString(FORMAT[0]), FORMAT }, "Output fb2 archive format [7z | zip]", QString("%1 [%2]").arg(FORMAT, "7z") },
			{ { QString(ARCHIVER_OPTION_NAME[0]), ARCHIVER_OPTION_NAME }, "Path to external archiver executable", QString("%1 [embedded zip archiver]").arg(PATH) },

			{ ARCHIVER_COMMANDLINE_OPTION_NAME, "External archiver command line options", COMMANDLINE },
			{ COVER_QUALITY_OPTION_NAME, "Covers compression quality", QUALITY },
			{ IMAGE_QUALITY_OPTION_NAME, "Images compression quality", QUALITY },
			{ MAX_SIZE_OPTION_NAME, "Maximum any images size", SIZE },
			{ MAX_COVER_SIZE_OPTION_NAME, "Maximum cover size", SIZE },
			{ MAX_IMAGE_SIZE_OPTION_NAME, "Maximum image size", SIZE },

			{ MIN_IMAGE_FILE_SIZE_OPTION_NAME, "Minimum image file size threshold for writing to error folder", QString("size [%1]").arg(settings.minImageFileSize) },
			{ FFMPEG_OPTION_NAME, "Path to ffmpeg executable", PATH },
			{ IMAGE_STATISTICS, "Image statistics output path", PATH },

			{ { QString(GRAYSCALE_OPTION_NAME[0]), GRAYSCALE_OPTION_NAME }, "Convert all images to grayscale" },
			{ COVER_GRAYSCALE_OPTION_NAME, "Convert covers to grayscale" },
			{ IMAGE_GRAYSCALE_OPTION_NAME, "Convert images to grayscale" },

			{ NO_ARCHIVE_FB2_OPTION_NAME, "Don't archive fb2" },
			{ NO_FB2_OPTION_NAME, "Don't save fb2" },
			{ NO_IMAGES_OPTION_NAME, "Don't save image" },
			{ COVERS_ONLY_OPTION_NAME, "Save covers only" },
    }
	);

	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	settings.logFileName = parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath;

	if (parser.positionalArguments().isEmpty())
		parser.showHelp(0);

	settings.dstDir = parser.value(FOLDER);

	settings.ffmpeg          = parser.value(FFMPEG_OPTION_NAME);
	settings.archiver        = parser.value(ARCHIVER_OPTION_NAME);
	settings.archiverOptions = parser.value(ARCHIVER_COMMANDLINE_OPTION_NAME);

	if (parser.isSet(FORMAT))
		settings.format = Zip::FormatFromString(parser.value(FORMAT));

	QSize size;
	if (SetValue(parser, MAX_SIZE_OPTION_NAME, size))
		settings.cover.maxSize = settings.image.maxSize = size;
	SetValue(parser, MAX_COVER_SIZE_OPTION_NAME, settings.cover.maxSize);
	SetValue(parser, MAX_IMAGE_SIZE_OPTION_NAME, settings.image.maxSize);

	int quality = -1;
	if (SetValue(parser, QUALITY_OPTION_NAME, quality))
		settings.cover.quality = settings.image.quality = quality;
	SetValue(parser, COVER_QUALITY_OPTION_NAME, settings.cover.quality);
	SetValue(parser, IMAGE_QUALITY_OPTION_NAME, settings.image.quality);

	SetValue(parser, MAX_THREAD_COUNT_OPTION_NAME, settings.maxThreadCount);
	SetValue(parser, MIN_IMAGE_FILE_SIZE_OPTION_NAME, settings.minImageFileSize);

	settings.imageStatistics = parser.value(IMAGE_STATISTICS);

	settings.cover.grayscale = settings.image.grayscale = parser.isSet(GRAYSCALE_OPTION_NAME);
	if (parser.isSet(COVER_GRAYSCALE_OPTION_NAME))
		settings.cover.grayscale = true;
	if (parser.isSet(IMAGE_GRAYSCALE_OPTION_NAME))
		settings.image.grayscale = true;

	settings.saveFb2    = !parser.isSet(NO_FB2_OPTION_NAME);
	settings.archiveFb2 = settings.saveFb2 && !parser.isSet(NO_ARCHIVE_FB2_OPTION_NAME);

	settings.cover.save = settings.image.save = !parser.isSet(NO_IMAGES_OPTION_NAME);
	settings.image.save                       = settings.image.save && !parser.isSet(COVERS_ONLY_OPTION_NAME);

	std::ranges::transform(parser.positionalArguments(), std::back_inserter(settings.inputWildcards), [](const auto& fileName) {
		return QDir::fromNativeSeparators(fileName);
	});

	return settings;
}

bool run(int argc, char* argv[])
{
	const QGuiApplication app(argc, argv); //-V821
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);
	Util::XMLPlatformInitializer xmlPlatformInitializer;

	auto                                             settings = ProcessCommandLine(app);
	Log::LoggingInitializer                          logging(settings.logFileName);
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	{
		std::ostringstream stream;
		stream << "Process started with " << settings;
		PLOGI << stream.str();
	}

	const auto checkExternalUtil = [](const QString& name, const QString& path) {
		if (!(path.isEmpty() || QFile::exists(path)))
			throw std::invalid_argument(QString("Cannot find %1, path '%2' not found").arg(name).arg(path).toStdString());
	};
	checkExternalUtil("ffmpeg", settings.ffmpeg);
	checkExternalUtil("external archiver", settings.archiver);

	const auto failedArchives = ProcessArchives(settings);
	if (failedArchives.isEmpty())
		return false;

	PLOGE << "Processed with errors:\n" << failedArchives.join("\n");
	return true;
}

} // namespace

int main(const int argc, char* argv[])
{
	try
	{
		if (run(argc, argv))
			PLOGW << QString("%1 finished with errors").arg(APP_ID);
		else
			PLOGI << QString("%1 successfully finished").arg(APP_ID);
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
