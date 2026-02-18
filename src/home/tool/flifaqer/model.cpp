#include "model.h"

#include <QBrush>

#include <ranges>
#include <unordered_set>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "fnd/ScopedCall.h"
#include "fnd/algorithm.h"

#include "util/StrUtil.h"

#include "AppConstant.h"
#include "di_app.h"
#include "log.h"
#include "role.h"

using namespace HomeCompa::FliFaq;
using namespace HomeCompa;

namespace
{

constexpr auto LANGUAGE = "language";
constexpr auto MACRO    = "macro";
constexpr auto QUESTION = "q";
constexpr auto ANSWER   = "t";
constexpr auto ON_TOP   = "v";
constexpr auto ITEMS    = "x";

constexpr QChar STRING_SEPARATOR = '\n';

constexpr auto NEW_ITEM      = QT_TRANSLATE_NOOP("flifaqer", "New question");
constexpr auto ALREADY_ADDED = QT_TRANSLATE_NOOP("flifaqer", "Language '%1' already added");

QJsonObject ParseJson(QIODevice& stream, QJsonDocument& doc)
{
	QJsonParseError jsonParseError;
	doc = QJsonDocument::fromJson(stream.readAll(), &jsonParseError);
	if (jsonParseError.error != QJsonParseError::NoError)
		throw std::invalid_argument(std::format("cannot parse file: {}", jsonParseError.errorString()));

	if (!doc.isObject())
		throw std::invalid_argument("document must be an object");

	return doc.object();
}

QString ToString(const QJsonValue& value)
{
	return (value.isNull() || value.isUndefined()) ? QString {}
	     : value.isString()                        ? value.toString()
	     : value.isArray()                         ? (value.toArray() | std::views::transform([](const auto& item) {
                                  return item.toString();
                              })
                              | std::ranges::to<QStringList>())
	                             .join(STRING_SEPARATOR)
	                       : (assert(false && "unknown type"), QString {});
}

struct ProfileQuestion
{
	QString before;
	QString after;
};

struct ProfileAnswer
{
	QString simple;
	QString tagged;

	std::unordered_map<QString, QString> tags;
};

struct Profile
{
	QString outputFileExtension;
	QString head;
	QString tail;

	std::vector<ProfileQuestion> question;
	ProfileAnswer                answer;

	static Profile Deserialize(QIODevice& stream)
	{
		QJsonDocument doc;
		const auto    obj = ParseJson(stream, doc);

		Profile profile;

#define ITEM(DST, SRC, NAME) DST.NAME = ToString(SRC.value(#NAME))

		ITEM(profile, obj, outputFileExtension);
		ITEM(profile, obj, head);
		ITEM(profile, obj, tail);

		std::ranges::transform(obj.value("question").toArray(), std::back_inserter(profile.question), [](const auto& item) {
			const auto pairObj = item.toObject();
			return ProfileQuestion {
				ITEM(, pairObj, before),
				ITEM(, pairObj, after),
			};
		});

		const auto objAnswer = obj.value("answer").toObject();
		ITEM(profile.answer, objAnswer, simple);
		ITEM(profile.answer, objAnswer, tagged);

#undef ITEM

		const auto objTags = objAnswer.value("tags").toObject();
		for (auto it = objTags.constBegin(), end = objTags.constEnd(); it != end; ++it)
			profile.answer.tags.try_emplace(it.key(), it.value().toString());

		return profile;
	}
};

class String
{
	static QString EMPTY;

public:
	bool Set(const QString& key, QString value)
	{
		auto& currentValue = m_data[key];
		return Util::Set(currentValue, std::move(value));
	}

	const QString& operator()(const QString& key) const
	{
		const auto it = m_data.find(key);
		return it != m_data.end() ? it->second : EMPTY;
	}

private:
	std::unordered_map<QString, QString> m_data;
};

using Replacements = std::unordered_map<QString, String>;

QString String::EMPTY;

struct Item;
using Items = std::vector<std::shared_ptr<Item>>;

struct Item
{
	Item*  parent { nullptr };
	int    row { -1 };
	String question;
	String answer;

	Items children;
	bool  onTop { false };
};

QString GetAnswerProfile(const ProfileAnswer& profile, QString& answer)
{
	if (answer.startsWith('<'))
		return profile.tagged;

	if (answer.startsWith(' '))
	{
		answer = answer.mid(1);
		return profile.tagged;
	}

	if (answer.startsWith('['))
	{
		if (const auto pos = answer.indexOf(']'); pos > 0)
		{
			if (const auto it = profile.tags.find(answer.mid(1, pos - 1)); it != profile.tags.end())
			{
				answer = answer.mid(pos + 1);
				return it->second;
			}
		}
	}

	return profile.simple;
}

void ExportImage(const QString& answer, QTextStream& stream)
{
	static const QRegularExpression rx(R"(^\[img (\S+?) (\S+?) (\S+?)\]$)");

	const auto match = rx.match(answer);
	if (match.hasMatch())
		stream << QString(R"(<p class="img"><img src="img/%1/%2.jpg" alt="&#128558; image lost" class="img%3">)").arg(match.captured(1), match.captured(2), match.captured(3)) << STRING_SEPARATOR;
}

void ExportId(const QString& answer, QTextStream& stream)
{
	static const QRegularExpression rx(R"(^\[id (\S+) *?(\S*)\]$)");

	const auto match = rx.match(answer);
	if (match.hasMatch())
		stream << QString(R"(<p id="%1"%2>)").arg(match.captured(1), match.captured(2).isEmpty() ? "" : QString(R"( class="%1")").arg(match.captured(2)));
}

void ExportAnswer(const ProfileAnswer& profile, QString& answer, QTextStream& stream)
{
	if (answer.startsWith("[img"))
		return ExportImage(answer, stream);

	if (answer.startsWith("[id"))
		return ExportId(answer, stream);

	auto profileAnswer = GetAnswerProfile(profile, answer);
	stream << profileAnswer.replace("#ANSWER#", answer);
}

void ExportAnswer(const ProfileAnswer& profile, const QString& language, const Item& item, QTextStream& stream)
{
	for (auto&& answer : item.answer(language).split(STRING_SEPARATOR))
		ExportAnswer(profile, answer, stream);
}

void ExportImpl(const Profile& profile, const QString& language, const Item& parent, QTextStream& stream, const bool onTop = false, const size_t level = 0)
{
	assert(!profile.question.empty());
	for (const auto& child : parent.children | std::views::filter([&](const auto& item) {
								 return item->onTop == onTop;
							 }))
	{
		auto [questionBefore, questionAfter] = profile.question[std::min(level, profile.question.size() - 1)];
		stream << questionBefore.replace("#QUESTION#", child->question(language));
		ExportImpl(profile, language, *child, stream, true, level + 1);
		ExportAnswer(profile.answer, language, *child, stream);
		ExportImpl(profile, language, *child, stream, false, level + 1);
		stream << questionAfter.replace("#QUESTION#", child->question(language));
	}
}

void ExportImpl(Profile profile, const QString& language, const Item& root, const Replacements& replacements, QIODevice& stream)
{
	for (const auto& [key, value] : replacements)
	{
		profile.head.replace(key, value(language));
		profile.tail.replace(key, value(language));
	}

	QTextStream textStream(&stream);
	textStream << profile.head << STRING_SEPARATOR;
	ExportImpl(profile, language, root, textStream);
	textStream << profile.tail << STRING_SEPARATOR;
}

using File  = std::pair<QString, QString>;
using Files = std::vector<File>;

void ParseItems(const QString& language, const QJsonArray& jsonArray, const std::shared_ptr<Item>& parent)
{
	for (const auto [jsonValue, row] : std::views::zip(jsonArray, std::views::iota(0)))
	{
		const auto obj   = jsonValue.toObject();
		auto&      child = row < static_cast<int>(parent->children.size()) ? parent->children[row] : parent->children.emplace_back(std::make_shared<Item>(parent.get(), row));
		child->question.Set(language, obj.value(QUESTION).toString());
		child->answer.Set(language, ToString(obj.value(ANSWER)));
		child->onTop = obj.value(ON_TOP).toBool();
		ParseItems(language, obj.value(ITEMS).toArray(), child);
	}
}

File ParseFile(QString file, const std::shared_ptr<Item>& root, Replacements& replacements)
{
	PLOGD << "parse " << file << " started";

	QFile stream(file);
	if (!stream.open(QIODevice::ReadOnly))
		throw std::invalid_argument("cannot open file");

	QJsonDocument doc;
	const auto    obj = ParseJson(stream, doc);

	auto language = obj.value(LANGUAGE).toString();
	if (language.isEmpty())
		throw std::invalid_argument("document language must be specified");

	const auto macro = obj.value(MACRO).toObject();
	for (auto it = macro.constBegin(), end = macro.constEnd(); it != end; ++it)
		replacements[it.key()].Set(language, it.value().toString());

	ParseItems(language, obj.value(ITEMS).toArray(), root);

	return std::make_pair(std::move(language), std::move(file));
}

QJsonArray SaveImpl(const QString& language, const Item& item)
{
	QJsonArray jsonArray;
	for (const auto& child : item.children)
	{
		QJsonArray answer;
		{
			auto splitted = child->answer(language).split(STRING_SEPARATOR);
			if (!splitted.isEmpty() && splitted.back().isEmpty())
				splitted.pop_back();
			for (const auto& str : splitted)
				answer.append(str);
		}

		QJsonObject obj {
			{ QUESTION, child->question(language) },
		};
		if (!answer.isEmpty())
			obj.insert(ANSWER, std::move(answer));
		if (child->onTop)
			obj.insert(ON_TOP, true);

		if (!child->children.empty())
			obj.insert(ITEMS, SaveImpl(language, *child));

		jsonArray.append(std::move(obj));
	}
	return jsonArray;
}

class ModelImpl final : public QAbstractItemModel
{
public:
	static std::unique_ptr<QAbstractItemModel> Create()
	{
		return std::make_unique<ModelImpl>();
	}

private: // QAbstractItemModel
	[[nodiscard]] QModelIndex index(const int row, const int column, const QModelIndex& parent) const override
	{
		if (!hasIndex(row, column, parent))
			return {};

		const auto* parentItem = parent.isValid() ? static_cast<Item*>(parent.internalPointer()) : m_root.get();
		return row < 0 || row >= static_cast<int>(parentItem->children.size()) ? QModelIndex() : createIndex(row, column, parentItem->children[static_cast<size_t>(row)].get());
	}

	[[nodiscard]] QModelIndex parent(const QModelIndex& index) const override
	{
		if (!index.isValid())
			return {};

		const auto* childItem  = static_cast<Item*>(index.internalPointer());
		const auto* parentItem = childItem->parent;

		return parentItem != m_root.get() ? createIndex(parentItem->row, 0, parentItem) : QModelIndex();
	}

	[[nodiscard]] int rowCount(const QModelIndex& parent) const override
	{
		const auto* parentItem = parent.isValid() ? static_cast<Item*>(parent.internalPointer()) : m_root.get();
		return static_cast<int>(parentItem->children.size());
	}

	[[nodiscard]] int columnCount(const QModelIndex&) const override
	{
		return 1;
	}

	[[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override
	{
		return index.isValid() ? GetDataImpl(index, role) : GetDataImpl(role);
	}

	bool setData(const QModelIndex& index, const QVariant& value, const int role) override
	{
		return index.isValid() ? SetDataImpl(index, value, role) : SetDataImpl(value, role);
	}

	bool insertRows(const int row, const int count, const QModelIndex& parent) override
	{
		if (row < 0 || row > rowCount(parent))
			return false;

		const ScopedCall insertGuard(
			[&] {
				beginInsertRows(parent, row, row + count - 1);
			},
			[this] {
				endInsertRows();
			}
		);

		auto* parentItem = parent.isValid() ? static_cast<Item*>(parent.internalPointer()) : m_root.get();
		parentItem->children.insert_range(std::next(parentItem->children.begin(), row), std::views::iota(row, row + count) | std::views::transform([&](const int n) {
																							auto item = std::make_shared<Item>(parentItem, n);
																							for (const auto& language : m_files | std::views::keys)
																								item->question.Set(language, Tr(NEW_ITEM));
																							return item;
																						}));
		for (const auto& item : parentItem->children | std::views::drop(row + count))
			item->row += count;

		return true;
	}

	bool removeRows(const int row, const int count, const QModelIndex& parent) override
	{
		if (row < 0 || row > rowCount(parent))
			return false;

		const ScopedCall insertGuard(
			[&] {
				beginRemoveRows(parent, row, row + count - 1);
			},
			[this] {
				endRemoveRows();
			}
		);

		auto* parentItem = parent.isValid() ? static_cast<Item*>(parent.internalPointer()) : m_root.get();
		parentItem->children.erase(std::next(parentItem->children.begin(), row), std::next(parentItem->children.begin(), row + count));
		for (const auto& item : parentItem->children | std::views::drop(row))
			item->row -= count;

		return true;
	}

	Qt::ItemFlags flags(const QModelIndex& index) const override
	{
		return QAbstractItemModel::flags(index) | Qt::ItemIsUserCheckable;
	}

private:
	[[nodiscard]] QVariant GetDataImpl(const int role) const
	{
		switch (role)
		{
			case Role::LanguageList:
				return m_files | std::views::keys | std::ranges::to<QStringList>();

			case Role::Macro:
				return (m_replacements | std::views::transform([this](const auto& item) {
							return QString("%1=%2").arg(item.first, item.second(m_language));
						})
				        | std::ranges::to<QStringList>())
				    .join(STRING_SEPARATOR);

			case Role::Validate:
				return m_validationResult;

			default:
				break;
		}

		return assert(false && "unexpected role"), QVariant {};
	}

	[[nodiscard]] QVariant GetDataImpl(const QModelIndex& index, const int role) const
	{
		const auto* item = static_cast<Item*>(index.internalPointer());
		assert(item);

		switch (role)
		{
			case Qt::DisplayRole:
				return item->question(m_language);

			case Qt::CheckStateRole:
				return item->onTop ? Qt::Checked : Qt::Unchecked;

			case Qt::ForegroundRole:
				return std::ranges::any_of(
						   m_files | std::views::keys,
						   [&](const QString& language) {
							   return item->answer(language).isEmpty() || item->question(language).isEmpty() || item->question(language) == Tr(NEW_ITEM);
						   }
					   )
				         ? QBrush(Qt::red)
				         : QVariant {};

			case Role::ReferenceQuestion:
				return item->question(m_referenceLanguage);

			case Role::ReferenceAnswer:
				return item->answer(m_referenceLanguage);

			case Role::TranslationQuestion:
				return item->question(m_translationLanguage);

			case Role::TranslationAnswer:
				return item->answer(m_translationLanguage);

			default:
				break;
		}
		return {};
	}

	bool SetDataImpl(const QVariant& value, const int role)
	{
		switch (role)
		{
			case Role::AddFile:
			{
				auto file = ParseFile(value.toString(), m_root, m_replacements);
				if (const auto it = std::ranges::find(m_files, file.first, &File::first); it != m_files.end())
					throw std::invalid_argument(Tr(ALREADY_ADDED).arg(it->first).toStdString());

				m_files.emplace_back(std::move(file));
			}

			case Role::Language:
				return Util::Set(
					m_language,
					value.toString(),
					[this] {
						beginResetModel();
					},
					[this] {
						endResetModel();
					}
				);

			case Role::Macro:
				for (const auto& str : value.toString().split(STRING_SEPARATOR))
					if (const auto pos = str.indexOf('='); pos > 0)
						m_replacements[str.first(pos)].Set(m_language, str.mid(pos + 1));

				erase_if(m_replacements, [&](const auto& item) {
					return std::ranges::all_of(m_files | std::views::keys, [&](const QString& language) {
						return item.second(language).isEmpty();
					});
				});

				return true;

			case Role::ReferenceLanguage:
				return Util::Set(m_referenceLanguage, value.toString());

			case Role::TranslationLanguage:
				return Util::Set(m_translationLanguage, value.toString());

			case Role::Save:
				return Save();

			case Role::Export:
				return Export(value.toString());

			case Role::Validate:
				return Validate();

			default:
				break;
		}

		return assert(false && "unexpected role"), false;
	}

	bool SetDataImpl(const QModelIndex& index, const QVariant& value, const int role)
	{
		auto* item = static_cast<Item*>(index.internalPointer());
		assert(item);

		switch (role)
		{
			case Qt::CheckStateRole:
				return Util::Set(item->onTop, value.value<Qt::CheckState>() == Qt::Checked);

			case Role::ReferenceQuestion:
				if (item->question.Set(m_referenceLanguage, value.toString()))
				{
					emit dataChanged(index, index, { Qt::DisplayRole });
					return true;
				}
				return false;

			case Role::ReferenceAnswer:
				return item->answer.Set(m_referenceLanguage, value.toString());

			case Role::TranslationQuestion:
				if (item->question.Set(m_translationLanguage, value.toString()))
				{
					emit dataChanged(index, index, { Qt::DisplayRole });
					return true;
				}
				return false;

			case Role::TranslationAnswer:
				return item->answer.Set(m_translationLanguage, value.toString());

			default:
				break;
		}

		return QAbstractItemModel::setData(index, value, role);
	}

	[[nodiscard]] bool Save() const
	{
		try
		{
			for (const auto& [language, file] : m_files)
				Save(language, file);

			return true;
		}
		catch (const std::exception& ex)
		{
			PLOGE << ex.what();
		}
		catch (...)
		{
			PLOGE << "Unknown save error";
		}
		return false;
	}

	void Save(const QString& language, const QString& file) const
	{
		QJsonObject macro;
		for (const auto& [key, value] : m_replacements)
			macro.insert(key, value(language));

		QJsonObject obj {
			{ LANGUAGE, language },
			{ MACRO, std::move(macro) },
			{ ITEMS, SaveImpl(language, *m_root) },
		};

		QFile stream(file);
		if (!stream.open(QIODevice::WriteOnly))
			throw std::runtime_error(std::format("cannot write to {}", file));

		stream.write(QJsonDocument(std::move(obj)).toJson());
	}

	[[nodiscard]] bool Export(const QString& profilePath) const
	{
		try
		{
			QFile stream(profilePath);
			if (!stream.open(QIODevice::ReadOnly))
				throw std::invalid_argument(std::format("cannot open {}", profilePath));

			const auto profile = Profile::Deserialize(stream);
			for (const auto& [language, file] : m_files)
				Export(profile, language, file);

			return true;
		}
		catch (const std::exception& ex)
		{
			PLOGE << ex.what();
		}
		catch (...)
		{
			PLOGE << "Unknown save error";
		}
		return false;
	}

	void Export(const Profile& profile, const QString& language, QString file) const
	{
		const QFileInfo fileInfo(file);
		file = fileInfo.dir().filePath(QString("%1.%2").arg(fileInfo.completeBaseName(), profile.outputFileExtension));

		QFile stream(file);
		if (!stream.open(QIODevice::WriteOnly))
			throw std::runtime_error(std::format("cannot write to {}", file));

		ExportImpl(profile, language, *m_root, m_replacements, stream);
	}

	bool Validate()
	{
		m_validationResult.clear();

		bool emptyQuestion = false;
		bool emptyAnswer   = false;

		std::unordered_set<QString> imgRequired;

		QRegularExpression imgRx(R"(^\[img (\S+?) (\S+?) \S+?\]$)");

		const auto checkImages = [&](const QDir& dir, const QString& answer) -> QString {
			QStringList list;
			for (const auto& str : answer.split(STRING_SEPARATOR, Qt::SkipEmptyParts))
			{
				if (const auto match = imgRx.match(str); match.hasMatch())
				{
					auto imgPath = dir.filePath(QString("img/%1/%2.jpg").arg(match.captured(1), match.captured(2)));
					if (!QFile::exists(imgPath))
						list << imgPath;
					imgRequired.emplace(imgPath);
				}
			}
			return list.join("\n");
		};

		const auto enumerate = [&](const Item& parent, const auto& r) -> void {
			for (const auto& child : parent.children)
			{
				for (const auto& [language, file] : m_files)
				{
					const auto dir = QFileInfo(file).dir();

					const auto answer   = child->answer(language);
					const auto question = child->question(language);

					emptyAnswer   = emptyAnswer || answer.isEmpty();
					emptyQuestion = emptyQuestion || question.isEmpty() || question == Tr(NEW_ITEM);

					if (const auto lostImages = checkImages(dir, answer); !lostImages.isEmpty())
						m_validationResult.append(QString("%1: %2 -> images lost:\n%3\n").arg(language, question, lostImages));
				}

				r(*child, r);
			}
		};

		enumerate(*m_root, enumerate);

		for (const auto& file : m_files | std::views::values)
		{
			QDirIterator it(QFileInfo(file).dir().filePath("img"), QStringList() << "*", QDir::Files, QDirIterator::Subdirectories);
			while (it.hasNext())
			{
				const auto img = it.next();
				if (!imgRequired.contains(img))
					m_validationResult.append(QString("unexpected file: %1\n").arg(img));
			}
		}

		return m_validationResult.isEmpty();
	}

private:
	Files                       m_files;
	const std::shared_ptr<Item> m_root { std::make_shared<Item>() };
	Replacements                m_replacements;

	QString m_language, m_referenceLanguage, m_translationLanguage;

	QString m_validationResult;
};

} // namespace

Model::Model(QObject* parent)
	: QIdentityProxyModel(parent)
	, m_source { ModelImpl::Create() }
{
	QIdentityProxyModel::setSourceModel(m_source.get());
}

Model::~Model() = default;
