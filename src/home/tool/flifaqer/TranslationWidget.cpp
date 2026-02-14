#include "ui_TranslationWidget.h"

#include "TranslationWidget.h"

#include "fnd/FindPair.h"

#include "role.h"

using namespace HomeCompa::FliFaq;

namespace
{

constexpr auto REFERENCE_LANGUAGE   = "referenceLanguage";
constexpr auto TRANSLATION_LANGUAGE = "translationLanguage";

constexpr std::pair<TranslationWidget::Mode, std::tuple<int, int, int, const char*>> MODE_SETTINGS[] {
	{   TranslationWidget::Mode::Reference,         { Role::ReferenceLanguage, Role::ReferenceQuestion, Role::ReferenceAnswer, REFERENCE_LANGUAGE } },
	{ TranslationWidget::Mode::Translation, { Role::TranslationLanguage, Role::TranslationQuestion, Role::TranslationAnswer, TRANSLATION_LANGUAGE } },
};

}

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
		std::tie(m_languageRole, m_questionRole, m_answerRole, m_languageKey) = FindSecond(MODE_SETTINGS, mode);

		if (const auto index = m_ui.language->findData(m_settings->Get(m_languageKey, QString())); index >= 0)
			m_ui.language->setCurrentIndex(index);

		connect(m_ui.language, &QComboBox::currentIndexChanged, [this] {
			m_settings->Set(m_languageKey, m_ui.language->currentData());
			OnLanguageChanged();
		});

		connect(m_ui.question, &QLineEdit::editingFinished, [this] {
			if (m_currentIndex.isValid())
				m_model->setData(m_currentIndex, m_ui.question->text(), m_questionRole);
		});

		connect(m_ui.answer, &QPlainTextEdit::textChanged, [this] {
			if (m_currentIndex.isValid())
				m_model->setData(m_currentIndex, m_ui.answer->toPlainText(), m_answerRole);
		});

		if (m_ui.language->count() > 0)
			OnLanguageChanged();
	}

	void AddLanguage(const QString& language) const
	{
		m_ui.language->addItem(language, language);
	}

	void SetCurrentIndex(const QModelIndex& index)
	{
		if (!index.isValid())
			return;

		m_currentIndex = index;
		m_ui.question->setText(m_model->data(index, m_questionRole).toString());
		m_ui.answer->setPlainText(m_model->data(index, m_answerRole).toString());
	}

private:
	void OnLanguageChanged()
	{
		m_model->setData({}, m_ui.language->currentData().toString(), m_languageRole);
		emit m_self.LanguageChanged();
	}

private:
	TranslationWidget&                                     m_self;
	PropagateConstPtr<ISettings, std::shared_ptr>          m_settings;
	PropagateConstPtr<QAbstractItemModel, std::shared_ptr> m_model;

	Ui::TranslationWidget m_ui;

	int         m_languageRole { 0 };
	int         m_questionRole { 0 };
	int         m_answerRole { 0 };
	const char* m_languageKey { nullptr };

	QPersistentModelIndex m_currentIndex;
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

void TranslationWidget::SetCurrentIndex(const QModelIndex& index)
{
	m_impl->SetCurrentIndex(index);
}
