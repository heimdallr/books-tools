#include "model.h"

#include <QBrush>

#include <ranges>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "fnd/ScopedCall.h"
#include "fnd/algorithm.h"

#include "util/StrUtil.h"

#include "Constant.h"
#include "di_app.h"
#include "log.h"
#include "role.h"

using namespace HomeCompa::FliFaq;
using namespace HomeCompa;

namespace
{

constexpr auto LANGUAGE       = "language";
constexpr auto TITLE          = "title";
constexpr auto HEAD           = "head";
constexpr auto TAIL           = "tail";
constexpr auto QUESTION       = "q";
constexpr auto ANSWER         = "t";
constexpr auto CHILDREN_FIRST = "v";
constexpr auto ITEMS          = "x";

constexpr QChar STRING_SEPARATOR = '\n';

constexpr auto NEW_ITEM = QT_TRANSLATE_NOOP("flifaqer", "New question");

QString Tr(const char* str)
{
	return QCoreApplication::translate(APP_ID, str);
}

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
	return value.isString() ? value.toString()
	     : value.isArray()  ? (value.toArray() | std::views::transform([](const auto& item) {
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
	bool  childrenFirst { false };
};

struct Additional
{
	String title;
	String head;
	String tail;
};

void ExportAnswer(ProfileAnswer profile, const QString& language, const Item& item, QTextStream& stream)
{
	const auto& answer = item.answer(language);
	stream << (answer.simplified().startsWith('<') ? profile.tagged : profile.simple).replace("#ANSWER#", answer);
}

void ExportImpl(const Profile& profile, const QString& language, const Item& parent, QTextStream& stream, const size_t level = 0)
{
	assert(!profile.question.empty());
	for (const auto& child : parent.children)
	{
		auto [questionBefore, questionAfter] = profile.question[std::min(level, profile.question.size() - 1)];
		stream << questionBefore.replace("#QUESTION#", child->question(language));
		if (child->childrenFirst)
		{
			ExportImpl(profile, language, *child, stream, level + 1);
			ExportAnswer(profile.answer, language, *child, stream);
		}
		else
		{
			ExportAnswer(profile.answer, language, *child, stream);
			ExportImpl(profile, language, *child, stream, level + 1);
		}
		stream << questionAfter.replace("#QUESTION#", child->question(language));
	}
}

void ExportImpl(Profile profile, const QString& language, const Item& root, const Additional& additional, QIODevice& stream)
{
	profile.head.replace("#LANGUAGE#", language).replace("#TITLE#", additional.title(language)).replace("#HEAD#", additional.head(language));
	profile.tail.replace("#TAIL#", additional.tail(language));

	QTextStream textStream(&stream);
	textStream << profile.head;
	ExportImpl(profile, language, root, textStream);
	textStream << profile.tail;
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
		child->childrenFirst = obj.value(CHILDREN_FIRST).toBool();
		ParseItems(language, obj.value(ITEMS).toArray(), child);
	}
}

File ParseFile(QString file, const std::shared_ptr<Item>& root, Additional& additional)
{
	PLOGD << "parse " << file << "started";

	QFile stream(file);
	if (!stream.open(QIODevice::ReadOnly))
		throw std::invalid_argument("cannot open file");

	QJsonDocument doc;
	const auto    obj = ParseJson(stream, doc);

	auto language = obj.value(LANGUAGE).toString();
	if (language.isEmpty())
		throw std::invalid_argument("document language must be specified");

	additional.title.Set(language, obj.value(TITLE).toString());
	additional.head.Set(language, obj.value(HEAD).toString());
	additional.tail.Set(language, obj.value(TAIL).toString());

	ParseItems(language, obj.value(ITEMS).toArray(), root);

	return std::make_pair(std::move(language), std::move(file));
}

auto CreateModelData(const ISettings& settings)
{
	auto        result     = std::make_tuple(Files {}, std::make_shared<Item>(), Additional {});
	auto&       files      = std::get<0>(result);
	const auto& root       = std::get<1>(result);
	auto&       additional = std::get<2>(result);

	std::ranges::transform(settings.Get(Constant::INPUT_FILES).toStringList(), std::back_inserter(files), [&](auto&& item) {
		return ParseFile(std::forward<QString>(item), root, additional);
	});

	return result;
}

QJsonArray SaveImpl(const QString& language, const Item& item)
{
	QJsonArray jsonArray;
	for (const auto& child : item.children)
	{
		QJsonArray answer;
		for (const auto& str : child->answer(language).split(STRING_SEPARATOR))
			answer.append(str);

		QJsonObject obj {
			{       QUESTION, child->question(language) },
			{         ANSWER,         std::move(answer) },
			{ CHILDREN_FIRST,      child->childrenFirst },
		};

		if (!child->children.empty())
			obj.insert(ITEMS, SaveImpl(language, *child));

		jsonArray.append(std::move(obj));
	}
	return jsonArray;
}

class ModelImpl final : public QAbstractItemModel
{
public:
	static std::unique_ptr<QAbstractItemModel> Create(const ISettings& settings)
	{
		auto [files, root, additional] = CreateModelData(settings);
		return std::make_unique<ModelImpl>(std::move(files), std::move(root), std::move(additional));
	}

	ModelImpl(Files files, std::shared_ptr<Item> root, Additional additional)
		: m_files { std::move(files) }
		, m_root { std::move(root) }
		, m_additional { std::move(additional) }
	{
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

			case Role::Title:
				return m_additional.title(m_language);

			case Role::Head:
				return m_additional.head(m_language);

			case Role::Tail:
				return m_additional.tail(m_language);

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
				return item->childrenFirst ? Qt::Checked : Qt::Unchecked;

			case Qt::ForegroundRole:
				return std::ranges::any_of(
						   m_files | std::views::keys,
						   [&](const QString& language) {
							   return item->answer(language).isEmpty() || item->question(language) == Tr(NEW_ITEM);
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

			case Role::ReferenceLanguage:
				return Util::Set(m_referenceLanguage, value.toString());

			case Role::TranslationLanguage:
				return Util::Set(m_translationLanguage, value.toString());

			case Role::Title:
				return m_additional.title.Set(m_language, value.toString());

			case Role::Head:
				return m_additional.head.Set(m_language, value.toString());

			case Role::Tail:
				return m_additional.tail.Set(m_language, value.toString());

			case Role::Save:
				return Save();

			case Role::Export:
				return Export(value.toString());

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
				return Util::Set(item->childrenFirst, value.value<Qt::CheckState>() == Qt::Checked);

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
		QJsonObject obj {
			{ LANGUAGE, language },
            { TITLE, m_additional.title(language) },
            { HEAD, m_additional.head(language) },
            { TAIL, m_additional.tail(language) },
            { ITEMS, SaveImpl(language, *m_root) },
		};

		QFile stream(file);
		if (!stream.open(QIODevice::WriteOnly))
			throw std::runtime_error(std::format("cannot write to {}", file));

		stream.write(QJsonDocument(obj).toJson());
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

		ExportImpl(profile, language, *m_root, m_additional, stream);
	}

private:
	const Files                 m_files;
	const std::shared_ptr<Item> m_root;
	Additional                  m_additional;

	QString m_language, m_referenceLanguage, m_translationLanguage;
};

} // namespace

Model::Model(const std::shared_ptr<const ISettings>& settings, QObject* parent)
	: QIdentityProxyModel(parent)
	, m_source { ModelImpl::Create(*settings) }
{
	QIdentityProxyModel::setSourceModel(m_source.get());
}

Model::~Model() = default;
