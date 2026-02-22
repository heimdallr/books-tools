#include "ui_TranslationWidget.h"

#include "TranslationWidget.h"

#include <ranges>

#include <QScrollBar>

#include "fnd/FindPair.h"

#include "role.h"

using namespace HomeCompa::FliFaq;
using namespace HomeCompa;

namespace
{

constexpr auto REFERENCE_LANGUAGE   = "referenceLanguage";
constexpr auto TRANSLATION_LANGUAGE = "translationLanguage";

struct ModeSettings
{
	const int         languageRole;
	const int         questionRole;
	const int         answerRole;
	const char* const languageKey;
};

class TranslationWidgetImpl // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
	TranslationWidgetImpl(TranslationWidget& self, ISettings& settings, QAbstractItemModel& model, Ui::TranslationWidget& ui, const ModeSettings& modeSettings)
		: m_self { self }
		, m_settings { settings }
		, m_model { model }
		, m_ui { ui }
		, m_modeSettings { modeSettings }
	{
	}

	virtual ~TranslationWidgetImpl() = default;

public:
	virtual void SetCurrentIndex(const QModelIndex& index) = 0;

protected:
	TranslationWidget&  m_self;
	ISettings&          m_settings;
	QAbstractItemModel& m_model;

	Ui::TranslationWidget& m_ui;
	const ModeSettings&    m_modeSettings;

	QPersistentModelIndex m_currentIndex;
};

class TranslationWidgetCommon final : public TranslationWidgetImpl
{
public:
	static std::unique_ptr<TranslationWidgetImpl> Create(TranslationWidget& self, ISettings& settings, QAbstractItemModel& model, Ui::TranslationWidget& ui, const ModeSettings& modeSettings)
	{
		return std::make_unique<TranslationWidgetCommon>(self, settings, model, ui, modeSettings);
	}

	TranslationWidgetCommon(TranslationWidget& self, ISettings& settings, QAbstractItemModel& model, Ui::TranslationWidget& ui, const ModeSettings& modeSettings)
		: TranslationWidgetImpl(self, settings, model, ui, modeSettings)
	{
		if (const auto index = m_ui.language->findData(m_settings.Get(modeSettings.languageKey, QString())); index >= 0)
			m_ui.language->setCurrentIndex(index);

		QObject::connect(m_ui.language, &QComboBox::currentIndexChanged, [this] {
			m_settings.Set(m_modeSettings.languageKey, m_ui.language->currentData());
			OnLanguageChanged();
		});

		QObject::connect(m_ui.question, &QLineEdit::editingFinished, [this] {
			if (m_currentIndex.isValid())
				m_model.setData(m_currentIndex, m_ui.question->text(), m_modeSettings.questionRole);
		});

		QObject::connect(m_ui.answer, &QPlainTextEdit::textChanged, [this] {
			if (!m_currentIndex.isValid())
				return m_ui.numbers->setPlainText({});

			m_model.setData(m_currentIndex, m_ui.answer->toPlainText(), m_modeSettings.answerRole);
			UpdateNumbers();
		});

		QWidget::connect(m_ui.answer->verticalScrollBar(), &QScrollBar::valueChanged, m_ui.numbers->verticalScrollBar(), &QScrollBar::setValue);

		if (m_ui.language->count() > 0)
			OnLanguageChanged();
	}

private: // TranslationWidgetImpl
	void SetCurrentIndex(const QModelIndex& index) override
	{
		m_currentIndex = index;
		const QSignalBlocker questionSignalBlocker(m_ui.question), answerSignalBlocker(m_ui.answer);
		m_ui.question->setText(m_model.data(index, m_modeSettings.questionRole).toString());
		m_ui.answer->setPlainText(m_model.data(index, m_modeSettings.answerRole).toString());
		UpdateNumbers();
	}

private:
	void OnLanguageChanged() const
	{
		m_model.setData({}, m_ui.language->currentData(), m_modeSettings.languageRole);
		emit m_self.LanguageChanged();
	}

	void UpdateNumbers() const
	{
		const QFontMetrics fontMetrics(m_self.font());
		int                maxTextWidth = 0;
		const auto text = m_ui.answer->toPlainText();
		m_ui.numbers->setPlainText((std::views::iota(1, text.split('\n').size() + 1) | std::views::transform([&](const int n) {
										auto str = QString::number(n);
										maxTextWidth = std::max(maxTextWidth, fontMetrics.boundingRect(str + "9").width());
										return str;
									})
		                            | std::ranges::to<QStringList>())
		                               .join('\n'));
		m_ui.numbers->verticalScrollBar()->setValue(m_ui.answer->verticalScrollBar()->value());
		m_ui.numbers->setMinimumWidth(maxTextWidth);
		m_ui.numbers->setMaximumWidth(maxTextWidth);
	}
};

class TranslationWidgetTemplate final : public TranslationWidgetImpl
{
public:
	static std::unique_ptr<TranslationWidgetImpl> Create(TranslationWidget& self, ISettings& settings, QAbstractItemModel& model, Ui::TranslationWidget& ui, const ModeSettings& modeSettings)
	{
		return std::make_unique<TranslationWidgetTemplate>(self, settings, model, ui, modeSettings);
	}

	TranslationWidgetTemplate(TranslationWidget& self, ISettings& settings, QAbstractItemModel& model, Ui::TranslationWidget& ui, const ModeSettings& modeSettings)
		: TranslationWidgetImpl(self, settings, model, ui, modeSettings)
	{
		m_ui.question->setVisible(false);
		m_ui.numbers->setVisible(false);

		QObject::connect(m_ui.language, &QComboBox::currentIndexChanged, [this] {
			OnLanguageChanged();
		});

		QObject::connect(m_ui.answer, &QPlainTextEdit::textChanged, [this] {
			if (!m_currentIndex.isValid())
				return;

			OnLanguageChanged();
			m_model.setData(m_currentIndex, m_ui.answer->toPlainText(), m_modeSettings.answerRole);
		});
	}

private: // TranslationWidgetImpl
	void SetCurrentIndex(const QModelIndex& index) override
	{
		m_currentIndex = index;
		const QSignalBlocker questionSignalBlocker(m_ui.question), answerSignalBlocker(m_ui.answer);
		m_ui.answer->setPlainText(m_model.data(index, m_modeSettings.answerRole).toString());
		if (const auto n = m_ui.language->findData(m_model.data(index, m_modeSettings.questionRole)); n >= 0)
			m_ui.language->setCurrentIndex(n);
	}

private:
	void OnLanguageChanged() const
	{
		m_model.setData(m_currentIndex, m_ui.language->currentData(), m_modeSettings.languageRole);
	}
};

using TranslationWidgetCreator = std::unique_ptr<TranslationWidgetImpl> (*)(TranslationWidget& self, ISettings& settings, QAbstractItemModel& model, Ui::TranslationWidget&, const ModeSettings&);
constexpr std::pair<TranslationWidget::Mode, std::pair<TranslationWidgetCreator, ModeSettings>> MODE_SETTINGS[] {
	{    TranslationWidget::Mode::Template,                     { &TranslationWidgetTemplate::Create, { Role::TemplateQuestion, Role::TemplateQuestion, Role::TemplateAnswer, nullptr } } },
	{   TranslationWidget::Mode::Reference,         { &TranslationWidgetCommon::Create, { Role::ReferenceLanguage, Role::ReferenceQuestion, Role::ReferenceAnswer, REFERENCE_LANGUAGE } } },
	{ TranslationWidget::Mode::Translation, { &TranslationWidgetCommon::Create, { Role::TranslationLanguage, Role::TranslationQuestion, Role::TranslationAnswer, TRANSLATION_LANGUAGE } } },
};

} // namespace

class TranslationWidget::Impl
{
public:
	explicit Impl(TranslationWidget& self, std::shared_ptr<ISettings> settings, std::shared_ptr<QAbstractItemModel> model)
		: m_self { self }
		, m_settings { std::move(settings) }
		, m_model { std::move(model) }
	{
		m_ui.setupUi(&m_self);
	}

public:
	void SetMode(const Mode mode)
	{
		const auto& [creator, settings] = FindSecond(MODE_SETTINGS, mode);
		m_impl.reset(creator(m_self, *m_settings, *m_model, m_ui, settings));
	}

	void AddLanguage(const QString& language) const
	{
		m_ui.language->addItem(language, language);
	}

	void SetLanguage(const QString& language) const
	{
		if (const auto index = m_ui.language->findData(language); index >= 0)
			m_ui.language->setCurrentIndex(index);
	}

	void SetCurrentIndex(const QModelIndex& index)
	{
		if (index.isValid())
			m_impl->SetCurrentIndex(index);
	}

private:
	TranslationWidget&                                     m_self;
	PropagateConstPtr<ISettings, std::shared_ptr>          m_settings;
	PropagateConstPtr<QAbstractItemModel, std::shared_ptr> m_model;
	PropagateConstPtr<TranslationWidgetImpl>               m_impl { std::unique_ptr<TranslationWidgetImpl> {} };

	Ui::TranslationWidget m_ui;
};

TranslationWidget::TranslationWidget(std::shared_ptr<ISettings> settings, std::shared_ptr<QAbstractItemModel> model, QWidget* parent)
	: QWidget(parent)
	, m_impl(*this, std::move(settings), std::move(model))
{
}

TranslationWidget::~TranslationWidget() = default;

void TranslationWidget::SetMode(const Mode mode)
{
	m_impl->SetMode(mode);
}

void TranslationWidget::AddLanguage(const QString& language) const
{
	m_impl->AddLanguage(language);
}

void TranslationWidget::SetLanguage(const QString& language) const
{
	m_impl->SetLanguage(language);
}

void TranslationWidget::SetCurrentIndex(const QModelIndex& index)
{
	m_impl->SetCurrentIndex(index);
}
