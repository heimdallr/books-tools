#include <ranges>
#include <set>
#include <unordered_set>

#include <QCommandLineParser>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <plog/Appenders/ConsoleAppender.h>

#include "fnd/ScopedCall.h"

#include "impl/FileItem.h"
#include "lib/UniqueFile.h"
#include "lib/book.h"
#include "lib/dump/Factory.h"
#include "lib/dump/IDump.h"
#include "lib/util.h"
#include "logging/LogAppender.h"
#include "logging/init.h"
#include "util/BookUtil.h"
#include "util/LogConsoleFormatter.h"
#include "util/files.h"
#include "util/progress.h"
#include "util/xml/Initializer.h"
#include "util/xml/SaxParser.h"
#include "util/xml/XmlAttributes.h"
#include "util/xml/XmlWriter.h"

#include "Constant.h"
#include "log.h"

#include "config/version.h"

using namespace HomeCompa::FliLib;
using namespace HomeCompa;

namespace
{

constexpr auto APP_ID = "flimerger";

constexpr auto ARCHIVE_WILDCARD_OPTION_NAME = "archives";
constexpr auto FOLDER                       = "folder";
constexpr auto DUMP                         = "dump";
constexpr auto HASH                         = "hash";

struct Archive
{
	QString filePath;
	QString hashPath;
};

using Archives    = std::vector<Archive>;
using BookItem    = std::pair<QString, QString>;
using Replacement = std::unordered_map<BookItem, BookItem, Util::PairHash<QString, QString>>;

struct Settings
{
	QDir        outputDir;
	QStringList arguments;
	QString     logFileName;
	QString     dumpWildCards;
};

class UniqueFileConflictResolver final : public UniqueFileStorage::IUniqueFileConflictResolver
{
public:
	explicit UniqueFileConflictResolver(std::shared_ptr<InpDataProvider> inpDataProvider)
		: m_inpDataProvider { std::move(inpDataProvider) }
	{
	}

private: // UniqueFileStorage::IUniqueFileConflictResolver
	bool Resolve(const UniqueFile& file, const UniqueFile& duplicate) const override
	{
		const auto& inpData   = m_inpDataProvider->GetInpData();
		const auto  isDeleted = [&](const UniqueFile& item) {
            const auto it = inpData.find(item.uid.file);
            return it == inpData.end() || it->second->deleted;
		};

		return isDeleted(duplicate) && !isDeleted(file);
	}

	void SetSourceLib(const QString& sourceLib) override
	{
		m_inpDataProvider->SetSourceLib(sourceLib);
	}

private:
	std::shared_ptr<InpDataProvider> m_inpDataProvider;
};

class HashCopier final : public Util::SaxParser
{
	static constexpr auto BOOK = "books/book";

public:
	HashCopier(QIODevice& input, QIODevice& output, const Replacement& replacement)
		: SaxParser(input, 512)
		, m_replacement { replacement }
		, m_writer { output }
	{
		Parse();
	}

private:
	bool OnStartElement(const QString& name, const QString& path, const Util::XmlAttributes& attributes) override
	{
		if (path == BOOK)
			if (const auto it = m_replacement.find(BookItem { attributes.GetAttribute(Inpx::FOLDER), attributes.GetAttribute(Inpx::FILE) }); it != m_replacement.end())
				m_origin = it->second;

		m_writer.WriteStartElement(name, attributes);
		return true;
	}

	bool OnEndElement(const QString& /*name*/, const QString& path) override
	{
		if (path == BOOK && m_origin)
		{
			const auto originGuard = m_writer.Guard("origin");
			originGuard->WriteAttribute(Inpx::FOLDER, m_origin->first).WriteAttribute(Inpx::FILE, m_origin->second);
			m_origin.reset();
		}

		m_writer.WriteEndElement();
		return true;
	}

	bool OnCharacters(const QString& /*path*/, const QString& value) override
	{
		m_writer.WriteCharacters(value);
		return true;
	}

private:
	const Replacement&      m_replacement;
	Util::XmlWriter         m_writer;
	std::optional<BookItem> m_origin;
};

class DuplicateObserver final : public UniqueFileStorage::IDuplicateObserver
{
public:
	explicit DuplicateObserver(Replacement& replacement)
		: m_replacement { replacement }
	{
	}

private: // UniqueFileStorage::IDuplicateObserver
	void OnDuplicateFound(const UniqueFile::Uid& file, const UniqueFile::Uid& duplicate) override
	{
		m_replacement.try_emplace(std::make_pair(duplicate.folder, duplicate.file), std::make_pair(file.folder, file.file));
	}

private:
	Replacement& m_replacement;
};

void ProcessArchive(const QDir& outputDir, const Archive& archive, const Replacement& replacement)
{
	const QFileInfo fileInfo(archive.filePath);

	const auto dstFilePath = outputDir.filePath(fileInfo.fileName());
	QFile::remove(dstFilePath);
	if (!QFile::copy(fileInfo.filePath(), dstFilePath))
		throw std::invalid_argument(std::format("Cannot copy {} to {}", fileInfo.filePath(), dstFilePath));

	for (const char* imageFolder : { Global::COVERS, Global::IMAGES })
	{
		auto imageDir = fileInfo.dir();
		if (!imageDir.cd(imageFolder))
			continue;

		const auto imageArchiveFileSrc = imageDir.absoluteFilePath(fileInfo.completeBaseName()) + ".zip";
		if (!QFile::exists(imageArchiveFileSrc))
			continue;

		QDir dstDir(outputDir.filePath(imageFolder));
		if (!dstDir.exists())
			dstDir.mkpath(".");

		const auto imageArchiveFileDst = dstDir.filePath(fileInfo.completeBaseName() + ".zip");
		QFile::remove(imageArchiveFileDst);
		if (!QFile::copy(imageArchiveFileSrc, imageArchiveFileDst))
			throw std::invalid_argument(std::format("Cannot copy {} to {}", imageArchiveFileSrc, imageArchiveFileDst));
	}

	auto toRemove = Zip(dstFilePath).GetFileNameList() | std::views::filter([&](const QString& fileName) {
						const auto key    = std::make_pair(fileInfo.fileName(), fileName);
						const auto result = replacement.contains(key);
						return result;
					})
	              | std::views::transform([&, n = 0](const QString& fileName) mutable {
						return Util::Remove::Book { ++n, fileInfo.fileName(), fileName };
					})
	              | std::ranges::to<Util::Remove::Books>();

	if (toRemove.empty())
		return;

	auto allFiles = CollectBookFiles(toRemove, [] {
		return nullptr;
	});
	auto images   = Util::Remove::CollectImageFiles(allFiles, outputDir.absolutePath(), [] {
        return nullptr;
    });
	std::ranges::move(std::move(images), std::inserter(allFiles, allFiles.end()));
	Util::Remove::RemoveFiles(allFiles, outputDir.absolutePath());
}

void MergeArchives(const QDir& outputDir, const Archives& archives, const Replacement& replacement)
{
	for (const auto& archive : archives)
		ProcessArchive(outputDir, archive, replacement);
}

void ProcessHash(const QDir& outputDir, const Archive& archive, const Replacement& replacement)
{
	PLOGI << "parsing " << archive.hashPath;
	outputDir.mkpath(HASH);
	QFileInfo fileInfo(archive.hashPath);

	QFile input(archive.hashPath);
	if (!input.open(QIODevice::ReadOnly))
		throw std::ios_base::failure(std::format("Cannot read from {}", archive.hashPath));

	const auto outputFilePath = outputDir.filePath(QString("%1/%2").arg(HASH, fileInfo.fileName()));

	QFile output(outputFilePath);
	if (!output.open(QIODevice::WriteOnly))
		throw std::ios_base::failure(std::format("Cannot write to", outputFilePath));

	[[maybe_unused]] const HashCopier parser(input, output, replacement);
}

void MergeHash(const QDir& outputDir, const Archives& archives, const Replacement& replacement)
{
	for (const auto& archive : archives)
		ProcessHash(outputDir, archive, replacement);
}

class ReplacementGetter : HashParser::IObserver
{
public:
	ReplacementGetter(const Archive& archive, UniqueFileStorage& uniqueFileStorage, UniqueFileStorage::IUniqueFileConflictResolver& conflictResolver, Util::Progress& progress)
		: m_fileInfo { archive.filePath }
		, m_uniqueFileStorage { uniqueFileStorage }
		, m_conflictResolver { conflictResolver }
		, m_progress { progress }
	{
		QFile file(archive.hashPath);
		if (!file.open(QIODevice::ReadOnly))
			throw std::invalid_argument(std::format("Cannot read from {}", archive.hashPath));

		for (const auto& inpx : m_fileInfo.dir().entryList({ "*.inpx" }, QDir::Files))
		{
			Zip        zip(m_fileInfo.dir().absoluteFilePath(inpx));
			const auto zipFile = zip.Read(m_fileInfo.completeBaseName() + ".inp");
			auto&      stream  = zipFile->GetStream();
			for (auto byteArray = stream.readLine(); !byteArray.isEmpty(); byteArray = stream.readLine())
			{
				auto book = Book::FromString(QString::fromUtf8(byteArray));
				m_titles.try_emplace(book.GetFileName(), SimplifyTitle(PrepareTitle(book.title)));
			}
		}

		m_bookFiles = Zip(archive.filePath).GetFileNameList() | std::ranges::to<std::unordered_set<QString>>();

		HashParser::Parse(file, *this);
	}

private:
	void OnParseStarted(const QString& sourceLib) override
	{
		m_conflictResolver.SetSourceLib(sourceLib);
	}

	void OnBookParsed(
#define HASH_PARSER_CALLBACK_ITEM(NAME) [[maybe_unused]] QString NAME,
		HASH_PARSER_CALLBACK_ITEMS_X_MACRO
#undef HASH_PARSER_CALLBACK_ITEM
			QString cover,
		QStringList images
	) override
	{
		m_progress.Increment(1, file.toStdString());

		decltype(UniqueFile::images) imageItems;
		std::ranges::transform(std::move(images) | std::views::as_rvalue, std::inserter(imageItems, imageItems.end()), [](QString&& hash) {
			return ImageItem { .hash = std::move(hash) };
		});

		if (!m_bookFiles.contains(file))
			return;

		const auto it = m_titles.find(file);
		if (it != m_titles.end() && !it->second.isEmpty())
			title = std::move(it->second);

		auto split    = title.split(' ', Qt::SkipEmptyParts);
		auto hashText = id;

		m_uniqueFileStorage.Add(
			std::move(id),
			UniqueFile {
				.uid      = { .folder = m_fileInfo.fileName(), .file = file },
				.title    = { std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()) },
				.hashText = std::move(hashText),
				.cover    = { .hash = std::move(cover) },
				.images   = std::move(imageItems)
        }
		);
	}

private:
	const QFileInfo                                 m_fileInfo;
	UniqueFileStorage&                              m_uniqueFileStorage;
	UniqueFileStorage::IUniqueFileConflictResolver& m_conflictResolver;
	Util::Progress&                                 m_progress;
	std::unordered_map<QString, QString>            m_titles;
	std::unordered_set<QString>                     m_bookFiles;
};

void GetReplacement(const size_t totalFileCount, const Archives& archives, UniqueFileStorage& uniqueFileStorage, UniqueFileStorage::IUniqueFileConflictResolver& conflictResolver)
{
	Util::Progress progress(totalFileCount, "parsing");

	for (const auto& archive : archives)
		ReplacementGetter(archive, uniqueFileStorage, conflictResolver, progress);
}

Archives GetArchives(const Settings& settings)
{
	std::multimap<int, Archive> sorted;
	std::unordered_set<QString> uniqueFiles;
	const QRegularExpression    rx("^.*?fb2.*?([0-9]+).*?$");

	for (const auto& argument : settings.arguments)
	{
		auto splitted = argument.split(';');
		if (splitted.size() != 2)
			throw std::invalid_argument(std::format("{} must be archives_wildcard;hash_folder", argument));

		const auto wildCard = std::move(splitted.front());
		const QDir hashFolder(splitted.back());
		if (!hashFolder.exists())
			throw std::invalid_argument(std::format("hash folder {} not found", splitted.back()));

		std::ranges::transform(
			Util::ResolveWildcard(wildCard) | std::views::transform([&](const QString& item) {
				return QFileInfo(item);
			}) | std::views::filter([&](const QFileInfo& item) {
				auto       fileName = item.fileName().toLower();
				const auto result   = !uniqueFiles.contains(fileName);
				if (result)
					uniqueFiles.emplace(fileName);
				return result;
			}) | std::views::transform([&](const QFileInfo& item) {
				auto hashPath = hashFolder.filePath(item.completeBaseName()) + ".xml";
				return Archive { item.absoluteFilePath(), std::move(hashPath) };
			}) | std::views::filter([](const Archive& item) {
				return QFile::exists(item.hashPath);
			}),
			std::inserter(sorted, sorted.end()),
			[&](Archive archive) {
				const auto match = rx.match(QFileInfo(archive.filePath).fileName());
				return std::make_pair(match.hasMatch() ? match.captured(1).toInt() : 0, std::move(archive));
			}
		);
	}

	return std::move(sorted) | std::views::values | std::views::reverse | std::ranges::to<Archives>();
}

Settings ProcessCommandLine(const QCoreApplication& app)
{
	Settings settings;

	QCommandLineParser parser;
	parser.setApplicationDescription(QString("%1 recodes images").arg(APP_ID));
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument(ARCHIVE_WILDCARD_OPTION_NAME, "Input archives with hashes (required)");
	parser.addOptions({
		{ { "o", FOLDER }, "Output folder (required)",                              FOLDER },
		{			DUMP,  "Dump database wildcards", "Semicolon separated wildcard list" },
	});

	const auto defaultLogPath = QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID);
	const auto logOption      = Log::LoggingInitializer::AddLogFileOption(parser, defaultLogPath);
	parser.process(app);

	if (parser.positionalArguments().isEmpty() || !parser.isSet(FOLDER))
		parser.showHelp();

	settings.logFileName   = parser.isSet(logOption) ? parser.value(logOption) : defaultLogPath;
	settings.arguments     = parser.positionalArguments();
	settings.outputDir     = QDir { parser.value(FOLDER) };
	settings.dumpWildCards = parser.value(DUMP);

	return settings;
}

void run(int argc, char* argv[])
{
	const QCoreApplication app(argc, argv); //-V821
	QCoreApplication::setApplicationName(APP_ID);
	QCoreApplication::setApplicationVersion(PRODUCT_VERSION);
	Util::XMLPlatformInitializer xmlPlatformInitializer;

	const auto settings = ProcessCommandLine(app);

	Log::LoggingInitializer                          logging(settings.logFileName.toStdWString());
	plog::ConsoleAppender<Util::LogConsoleFormatter> consoleAppender;
	Log::LogAppender                                 logConsoleAppender(&consoleAppender);
	PLOGI << QString("%1 started").arg(APP_ID);

	if (!settings.outputDir.exists() && !settings.outputDir.mkpath("."))
		throw std::ios_base::failure(std::format("Cannot create folder {}", settings.outputDir.path()));

	const auto archives = GetArchives(settings);
	//	const auto inputDirs = GetInputFolders(archives);

	PLOGD << "Total file count calculation";
	const auto totalFileCount = std::accumulate(archives.cbegin(), archives.cend(), size_t { 0 }, [](const size_t init, const Archive& archive) {
		const Zip zip(archive.filePath);
		return init + zip.GetFileNameList().size();
	});
	PLOGI << "Total file count: " << totalFileCount;

	auto inpDataProvider = std::make_shared<InpDataProvider>(settings.dumpWildCards);

	UniqueFileStorage uniqueFileStorage(settings.outputDir.absoluteFilePath(HASH), inpDataProvider);

	const auto conflictResolver = std::make_shared<UniqueFileConflictResolver>(inpDataProvider);
	uniqueFileStorage.SetConflictResolver(conflictResolver);

	Replacement replacement;
	uniqueFileStorage.SetDuplicateObserver(std::make_unique<DuplicateObserver>(replacement));
	GetReplacement(totalFileCount, archives, uniqueFileStorage, *conflictResolver);

	MergeArchives(settings.outputDir, archives, replacement);
	MergeHash(settings.outputDir, archives, replacement);
}

} // namespace

int main(const int argc, char* argv[])
{
	try
	{
		run(argc, argv);
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
