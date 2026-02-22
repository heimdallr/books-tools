#include "model.h"

#include <ranges>
#include <unordered_set>

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>

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
constexpr auto QUESTION = "question";
constexpr auto ANSWER   = "answer";
constexpr auto NAME     = "name";
constexpr auto TAGS     = "tags";
constexpr auto Q        = "q";
constexpr auto A        = "t";
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

QJsonValue FromString(const QString& value)
{
	const auto list = value.split(STRING_SEPARATOR);
	if (list.empty())
		return {};

	if (list.size() == 1)
		return list.front();

	QJsonArray array;
	for (const auto& item : list)
		array.append(item);

	return array;
}

struct ProfileQuestion
{
	QString before;
	QString after;
};

struct Tag
{
	QString expression;
	QString replacement;
};

struct Profile
{
	QString outputFileName;
	QString outputFileExtension;
	QString head;
	QString tail;

	std::vector<std::pair<QString, ProfileQuestion>> question;
	std::vector<Tag>                                 tags;

	void Serialize(QJsonObject& obj) const
	{
#define ITEM(DST, SRC, NAME) DST[#NAME] = FromString((SRC).NAME)
		ITEM(obj, *this, outputFileName);
		ITEM(obj, *this, outputFileExtension);
		ITEM(obj, *this, head);
		ITEM(obj, *this, tail);

		{
			QJsonArray questionArray;
			for (const auto& [name, item] : question)
			{
				QJsonObject questionObj {
					{ NAME, name }
				};
				ITEM(questionObj, item, before);
				ITEM(questionObj, item, after);
				questionArray.append(std::move(questionObj));
			}

			obj.insert(QUESTION, std::move(questionArray));
		}
		{
			QJsonArray tagsArray;
			for (const auto& tag : tags)
			{
				QJsonObject tagObj;
				ITEM(tagObj, tag, expression);
				ITEM(tagObj, tag, replacement);
				tagsArray.append(std::move(tagObj));
			}

			obj.insert(TAGS, std::move(tagsArray));
		}

#undef ITEM
	}

	static Profile Deserialize(const QJsonObject& obj)
	{
		Profile profile;

#define ITEM(DST, SRC, NAME) DST.NAME = ToString((SRC).value(#NAME))

		ITEM(profile, obj, outputFileName);
		ITEM(profile, obj, outputFileExtension);
		ITEM(profile, obj, head);
		ITEM(profile, obj, tail);

		std::ranges::transform(obj.value(QUESTION).toArray(), std::back_inserter(profile.question), [](const auto& item) {
			const auto itemObj = item.toObject();
			return std::make_pair(
				itemObj.value(NAME).toString(),
				ProfileQuestion {
					ITEM(, itemObj, before),
					ITEM(, itemObj, after),
				}
			);
		});

		std::ranges::transform(obj.value(TAGS).toArray(), std::back_inserter(profile.tags), [](const auto& item) {
			const auto itemObj = item.toObject();
			return Tag {
				ITEM(, itemObj, expression),
				ITEM(, itemObj, replacement),
			};
		});

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

//void ExportImage(const QString& answer, QTextStream& stream)
//{
//	static const QRegularExpression rx(R"(^\[img (\S+?) (\S+?) (\S+?)\]$)");
//
//	const auto match = rx.match(answer);
//	if (match.hasMatch())
//		stream << QString(R"(<p class="img"><img src="img/%1/%2.jpg" alt="&#128558; image lost" class="img%3">)").arg(match.captured(1), match.captured(2), match.captured(3)) << STRING_SEPARATOR;
//}

//void ExportId(const QString& answer, QTextStream& stream)
//{
//	static const QRegularExpression rx(R"(^\[id (\S+) *?(\S*)\]$)");
//
//	const auto match = rx.match(answer);
//	if (match.hasMatch())
//		stream << QString(R"(<p id="%1"%2>)").arg(match.captured(1), match.captured(2).isEmpty() ? "" : QString(R"( class="%1")").arg(match.captured(2)));
//}

[[nodiscard]] QString ExportAnswer(const Profile& profile, const QString& language, const Item& item)
{
	auto answer = item.answer(Constant::TEMPLATE);
	for (const auto& [expression, replacement] : profile.tags)
		answer.replace(QRegularExpression(expression), replacement);

	for (const auto& string : item.answer(language).split(STRING_SEPARATOR))
		answer = answer.arg(string);
	return answer;
}

[[nodiscard]] QString ExportImpl(const Profile& profile, const QString& language, const Item& parent, bool onTop = false, size_t level = 0, bool recursive = true);

[[nodiscard]] QString GetText(const Profile& profile, const QString& language, const Item& item, const size_t level = 0, const bool recursive = false)
{
	QString    result;
	const auto it = std::ranges::find(profile.question, item.question(Constant::TEMPLATE), [](const auto& question) {
		return question.first;
	});
	//		assert(it != profile.question.end());
	if (it == profile.question.end())
		return {};

	auto [questionBefore, questionAfter] = it->second;
	result.append(questionBefore.replace("#QUESTION#", item.question(language)));
	if (recursive)
		result.append(ExportImpl(profile, language, item, true, level + 1, true));
	result.append(ExportAnswer(profile, language, item));
	if (recursive)
		result.append(ExportImpl(profile, language, item, false, level + 1, true));
	result.append(questionAfter.replace("#QUESTION#", item.question(language)));

	return result;
}

[[nodiscard]] QString ExportImpl(const Profile& profile, const QString& language, const Item& parent, const bool onTop, const size_t level, const bool recursive)
{
	QString result;
	assert(!profile.question.empty());
	for (const auto& child : parent.children | std::views::filter([&](const auto& item) {
								 return item->onTop == onTop;
							 }))
		result.append(GetText(profile, language, *child, level, recursive));

	return result;
}

void ExportImpl(Profile profile, const QString& language, const Item& root, const Replacements& replacements, QIODevice& stream)
{
	for (const auto& [key, value] : replacements)
	{
		profile.head.replace(key, value(language));
		profile.tail.replace(key, value(language));
	}

	QString text = profile.head;
	text.append(ExportImpl(profile, language, root)).append(profile.tail);
	stream.write(text.toUtf8());
}

using File  = std::pair<QString, QString>;
using Files = std::vector<File>;

void ParseItems(const QString& language, const QJsonArray& jsonArray, const std::shared_ptr<Item>& parent)
{
	for (const auto [jsonValue, row] : std::views::zip(jsonArray, std::views::iota(0)))
	{
		const auto obj   = jsonValue.toObject();
		auto&      child = row < static_cast<int>(parent->children.size()) ? parent->children[row] : parent->children.emplace_back(std::make_shared<Item>(parent.get(), row));
		child->question.Set(language, obj.value(Q).toString());
		child->answer.Set(language, ToString(obj.value(A)));
		child->onTop = obj.value(ON_TOP).toBool();
		ParseItems(language, obj.value(ITEMS).toArray(), child);
	}
}

File ParseFile(QString file, const std::shared_ptr<Item>& root, Replacements& replacements, const auto& additional)
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

	additional(obj);

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
			for (const auto& str : splitted)
				answer.append(str);
		}

		QJsonObject obj {
			{ Q, child->question(language) },
		};
		if (!answer.isEmpty())
			obj.insert(A, std::move(answer));
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
			case Role::QuestionTypeList:
				return m_profile.question | std::views::keys | std::ranges::to<QStringList>();

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

			case Role::TemplateQuestion:
				return item->question(Constant::TEMPLATE);

			case Role::TemplateAnswer:
				return item->answer(Constant::TEMPLATE);

			case Role::ReferenceQuestion:
				return item->question(m_referenceLanguage);

			case Role::ReferenceAnswer:
				return item->answer(m_referenceLanguage);

			case Role::ReferenceText:
				return GetText(m_profile, m_referenceLanguage, *item);

			case Role::TranslationQuestion:
				return item->question(m_translationLanguage);

			case Role::TranslationAnswer:
				return item->answer(m_translationLanguage);

			case Role::TranslationText:
				return GetText(m_profile, m_translationLanguage, *item);

			default:
				break;
		}
		return {};
	}

	bool SetDataImpl(const QVariant& value, const int role)
	{
		switch (role)
		{
			case Role::AddTemplate:
				return AddTemplate(value.toString());

			case Role::AddFile:
			{
				auto file = ParseFile(value.toString(), m_root, m_replacements, [](const QJsonObject&) {
				});
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

			case Role::ReferenceLanguage:
				return Util::Set(m_referenceLanguage, value.toString());

			case Role::TranslationLanguage:
				return Util::Set(m_translationLanguage, value.toString());

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

			case Role::Save:
				return Save();

			case Role::Export:
				return Export();

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

			case Role::TemplateQuestion:
				return Set(item->question, Constant::TEMPLATE, index, value, { Role::ReferenceText, Role::TranslationText });

			case Role::TemplateAnswer:
				return Set(item->answer, Constant::TEMPLATE, index, value, { Role::ReferenceText, Role::TranslationText });

			case Role::ReferenceQuestion:
				return Set(item->question, m_referenceLanguage, index, value, { Qt::DisplayRole, Role::ReferenceText });

			case Role::ReferenceAnswer:
				return Set(item->answer, m_referenceLanguage, index, value, { Role::ReferenceText });

			case Role::TranslationQuestion:
				return Set(item->question, m_translationLanguage, index, value, { Qt::DisplayRole, Role::TranslationText });

			case Role::TranslationAnswer:
				return Set(item->answer, m_translationLanguage, index, value, { Role::TranslationText });

			default:
				break;
		}

		return QAbstractItemModel::setData(index, value, role);
	}

	bool Set(String& string, const QString& language, const QModelIndex& index, const QVariant& value, const QList<int>& roles)
	{
		if (!string.Set(language, value.toString()))
			return false;

		emit dataChanged(index, index, roles);
		return true;
	}

	[[nodiscard]] bool AddTemplate(QString path)
	{
		m_templatePath = std::move(path);
		ParseFile(m_templatePath, m_root, m_replacements, [this](const QJsonObject& obj) {
			m_profile = Profile::Deserialize(obj);
		});
		return true;
	}

	[[nodiscard]] bool Save() const
	{
		Save(Constant::TEMPLATE, m_templatePath, [this](QJsonObject& obj) {
			obj.remove(MACRO);
			m_profile.Serialize(obj);
		});
		for (const auto& [language, file] : m_files)
			Save(language, file, [](QJsonObject&) {
			});

		PLOGI << "Saved successfully";
		return true;
	}

	void Save(const QString& language, const QString& file, const auto& additional) const
	{
		QJsonObject macro;
		for (const auto& [key, value] : m_replacements)
			macro.insert(key, value(language));

		QJsonObject obj {
			{ LANGUAGE, language },
			{ MACRO, std::move(macro) },
			{ ITEMS, SaveImpl(language, *m_root) },
		};

		additional(obj);

		QFile stream(file);
		if (!stream.open(QIODevice::WriteOnly))
			throw std::runtime_error(std::format("cannot write to {}", file));

		stream.write(QJsonDocument(obj).toJson());
	}

	[[nodiscard]] bool Export() const
	{
		if (m_profile.head.isEmpty())
			throw std::invalid_argument("Must select template file");

		for (const auto& [language, file] : m_files)
			Export(m_profile, language, file);

		PLOGI << "Export completed successfully";
		return true;
	}

	void Export(const Profile& profile, const QString& language, QString file) const
	{
		const QFileInfo fileInfo(file);
		const auto      fileName = !profile.outputFileName.isEmpty()      ? profile.outputFileName
		                         : !profile.outputFileExtension.isEmpty() ? QString("%1.%2").arg(fileInfo.completeBaseName(), profile.outputFileExtension)
		                                                                  : QString("%1.html").arg(fileInfo.completeBaseName());

		file = fileInfo.dir().filePath(fileName);

		QFile stream(file);
		if (!stream.open(QIODevice::WriteOnly))
			throw std::runtime_error(std::format("cannot write to {}", file));

		ExportImpl(profile, language, *m_root, m_replacements, stream);
	}

	bool Validate()
	{
		m_validationResult.clear();

		std::unordered_set<QString> imgRequired;
		const QRegularExpression    imgRx(R"(\[img (\S+?) (\S+?) \S+?\])");

		const auto enumerate = [&](const Item& parent, const auto& r) -> void {
			for (const auto& child : parent.children)
			{
				const auto imageList = child->answer(Constant::TEMPLATE).split(STRING_SEPARATOR, Qt::SkipEmptyParts) | std::ranges::to<std::vector<QString>>()
				                     | std::views::transform([&](const QString& item) {
										   return imgRx.match(item);
									   })
				                     | std::views::filter([](const QRegularExpressionMatch& item) {
										   return item.hasMatch();
									   })
				                     | std::views::transform([](const auto& item) {
										   return QString("img/%1/%2.jpg").arg(item.captured(1), item.captured(2));
									   })
				                     | std::ranges::to<std::vector<QString>>();

				for (const auto& [language, file] : m_files)
				{
					const auto dir = QFileInfo(file).dir();

					const auto answer   = child->answer(language);
					const auto question = child->question(language);

					if (question.isEmpty() || question == Tr(NEW_ITEM))
						m_validationResult.append(QString("%1: -> empty question found\n").arg(language));

					if (answer.isEmpty())
						m_validationResult.append(QString("%1: %2 -> empty answer\n").arg(language, question));

					for (const auto& image : imageList)
					{
						auto imgPath = dir.filePath(image);
						QFile::exists(imgPath) ? (void)imgRequired.emplace(std::move(imgPath)) : (void)m_validationResult.append(QString("%1: %2 -> images lost:\n%3\n").arg(language, question, image));
					}
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

	Profile m_profile;
	QString m_templatePath;
};

} // namespace

Model::Model(QObject* parent)
	: QIdentityProxyModel(parent)
	, m_source { ModelImpl::Create() }
{
	QIdentityProxyModel::setSourceModel(m_source.get());
}

Model::~Model() = default;
