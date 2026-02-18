#include "ui_MainWindow.h"

#include "MainWindow.h"

#include <ranges>

#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStyleFactory>

#include "util/GeometryRestorable.h"

#include "AppConstant.h"
#include "Constant.h"
#include "di_app.h"
#include "role.h"

#include "config/version.h"

using namespace HomeCompa::FliFaq;

namespace
{

constexpr auto MAIN_WINDOW = "MainWindow";

constexpr auto LANGUAGE = "language";
constexpr auto PROFILE  = "profile";

constexpr auto FONT_SIZE_KEY = "ui/Font/pointSizeF";

constexpr auto ADD                = QT_TRANSLATE_NOOP("flifaqer", "Add");
constexpr auto REMOVE             = QT_TRANSLATE_NOOP("flifaqer", "Remove");
constexpr auto SELECT_PROFILE     = QT_TRANSLATE_NOOP("flifaqer", "Select profile");
constexpr auto SELECT_FILES       = QT_TRANSLATE_NOOP("flifaqer", "Select files");
constexpr auto SELECT_JSON_FILTER = QT_TRANSLATE_NOOP("flifaqer", "Json files (*.json);;All files (*.*)");
constexpr auto VALIDATION_RESULT  = QT_TRANSLATE_NOOP("flifaqer", "Validation result");
constexpr auto OK                 = QT_TRANSLATE_NOOP("flifaqer", "Everything's cool!");

}

class MainWindow::Impl final
	: Util::GeometryRestorable
	, Util::GeometryRestorableObserver
{
	NON_COPY_MOVABLE(Impl)

public:
	Impl(MainWindow& self, std::shared_ptr<ISettings> settings, std::shared_ptr<QAbstractItemModel> model, std::shared_ptr<TranslationWidget> reference, std::shared_ptr<TranslationWidget> translation)
		: GeometryRestorable(*this, settings, MAIN_WINDOW)
		, GeometryRestorableObserver(self)
		, m_self { self }
		, m_settings { std::move(settings) }
		, m_model { std::move(model) }
		, m_reference { std::move(reference) }
		, m_translation { std::move(translation) }
	{
		m_ui.setupUi(&m_self);
		m_ui.navigatorView->setModel(m_model.get());
		m_ui.referenceLayout->addWidget(m_reference.get());
		m_ui.translationLayout->addWidget(m_translation.get());

		for (const auto& language : m_model->data({}, Role::LanguageList).toStringList())
			AddLanguage(language);

		m_reference->SetMode(TranslationWidget::Mode::Reference);
		m_translation->SetMode(TranslationWidget::Mode::Translation);

		if (const auto index = m_ui.language->findData(m_settings->Get(LANGUAGE, QString())); index >= 0)
			m_ui.language->setCurrentIndex(index);

		auto setLanguage = [this] {
			m_model->setData({}, m_ui.language->currentData(), Role::Language);
			m_ui.replacements->setPlainText(m_model->data({}, Role::Macro).toString());
		};

		connect(m_ui.language, &QComboBox::currentIndexChanged, [this, setLanguage] {
			setLanguage();
			m_settings->Set(LANGUAGE, m_ui.language->currentData());
		});

		if (m_ui.language->count() > 0)
			setLanguage();

		connect(m_ui.replacements, &QPlainTextEdit::textChanged, [this] {
			m_model->setData({}, m_ui.replacements->toPlainText(), Role::Macro);
		});

		connect(m_ui.navigatorView, &QWidget::customContextMenuRequested, [this](const QPoint& pos) {
			OnNavigationViewContextMenuRequested(pos);
		});

		connect(m_ui.navigatorView->selectionModel(), &QItemSelectionModel::currentChanged, [this](const QModelIndex& index) {
			m_reference->SetCurrentIndex(index);
			m_translation->SetCurrentIndex(index);
		});
		connect(m_reference.get(), &TranslationWidget::LanguageChanged, [this] {
			m_reference->SetCurrentIndex(m_ui.navigatorView->currentIndex());
		});
		connect(m_translation.get(), &TranslationWidget::LanguageChanged, [this] {
			m_translation->SetCurrentIndex(m_ui.navigatorView->currentIndex());
		});

		connect(m_ui.actionAddFiles, &QAction::triggered, [this] {
			OnActionAddFilesTriggered();
		});
		connect(m_ui.actionSave, &QAction::triggered, [this] {
			m_model->setData({}, {}, Role::Save);
		});
		connect(m_ui.actionExport, &QAction::triggered, [this] {
			OnActionExportTriggered();
		});
		connect(m_ui.actionValidate, &QAction::triggered, [this] {
			OnActionValidateTriggered();
		});

		connect(m_ui.actionSetProfile, &QAction::triggered, [this] {
			OnActionSetProfileTriggered();
		});

		const auto incrementFontSize = [&](const int value) {
			const auto fontSize = m_settings->Get(FONT_SIZE_KEY, 10);
			m_settings->Set(FONT_SIZE_KEY, fontSize + value);
		};
		connect(m_ui.actionFontSizeUp, &QAction::triggered, &m_self, [=] {
			incrementFontSize(1);
		});
		connect(m_ui.actionFontSizeDown, &QAction::triggered, &m_self, [=] {
			incrementFontSize(-1);
		});

		const auto currentStyleName = QApplication::style()->name();

		for (const auto& key : QStyleFactory::keys() | std::views::filter([](const QString& theme) {
								   return theme.compare("windows11", Qt::CaseInsensitive);
							   }))
		{
			auto* action = m_ui.menuTheme->addAction(key);
			action->setCheckable(true);
			if (currentStyleName.compare(key, Qt::CaseInsensitive) == 0)
			{
				action->setChecked(true);
				action->setEnabled(false);
				continue;
			}
			connect(action, &QAction::triggered, [this, key] {
				m_settings->Set(Constant::THEME, key);
				QCoreApplication::exit(Global::RESTART_APP);
			});
		}

		m_self.setWindowTitle(QString("%1 %2").arg(APP_ID, PRODUCT_VERSION));
		LoadGeometry();
	}

	~Impl() override
	{
		SaveGeometry();
	}

private:
	void OnNavigationViewContextMenuRequested(const QPoint& pos)
	{
		const auto index = m_ui.navigatorView->indexAt(pos);

		QMenu menu;
		menu.setFont(m_self.font());
		menu.addAction(Tr(ADD), [&] {
			m_model->insertRow(m_model->rowCount(index), index);
			if (index.isValid())
				m_ui.navigatorView->expand(index);

			const auto currentIndex = m_model->index(m_model->rowCount(index) - 1, 0, index);
			if (const auto clipboardText = QGuiApplication::clipboard()->text(); !clipboardText.isEmpty())
				m_model->setData(currentIndex, clipboardText, Role::ReferenceQuestion);
			m_ui.navigatorView->setCurrentIndex(currentIndex);
		});
		menu.addAction(
				Tr(REMOVE),
				[&] {
					m_model->removeRow(index.row(), index.parent());
				}
		)->setEnabled(index.isValid());
		menu.addSeparator();
		menu.addAction(m_ui.actionExpandAll);
		menu.addAction(m_ui.actionCollapsAll);
		menu.exec(QCursor::pos());
	}

	void OnActionAddFilesTriggered()
	{
		auto       files     = m_settings->Get(Constant::INPUT_FILES).toStringList();
		const auto languages = m_model->data({}, Role::LanguageList).toStringList();
		try
		{
			for (auto&& file : QFileDialog::getOpenFileNames(&m_self, Tr(SELECT_FILES), {}, Tr(SELECT_JSON_FILTER)))
			{
				m_model->setData({}, file, Role::AddFile);
				files << std::move(file);
			}
		}
		catch (const std::exception& ex)
		{
			QMessageBox::critical(&m_self, Tr(Constant::ERROR), ex.what());
		}

		if (files.count() > languages.count())
			m_settings->Set(Constant::INPUT_FILES, files);

		const auto currentLanguage = m_ui.language->currentData();
		QString    translationLanguage;
		for (auto&& language : m_model->data({}, Role::LanguageList).toStringList())
		{
			AddLanguage(language);
			if (!languages.contains(language))
				translationLanguage = std::move(language);
		}

		if (const auto index = m_ui.language->findData(currentLanguage); index >= 0)
		{
			m_ui.language->setCurrentIndex(index);
			m_model->setData({}, m_ui.language->currentData(), Role::Language);
		}

		if (!translationLanguage.isEmpty())
			m_translation->SetLanguage(translationLanguage);
	}

	void OnActionExportTriggered()
	{
		auto profile = m_settings->Get(PROFILE).toString();
		if (profile.isEmpty())
		{
			profile = OnActionSetProfileTriggered();
			if (profile.isEmpty())
				return;
		}

		m_model->setData({}, profile, Role::Export);
	}

	QString OnActionSetProfileTriggered()
	{
		auto profile = QFileDialog::getOpenFileName(&m_self, Tr(SELECT_PROFILE), {}, Tr(SELECT_JSON_FILTER));
		if (profile.isEmpty())
			return {};

		m_settings->Set(PROFILE, profile);
		return profile;
	}

	void AddLanguage(const QString& language)
	{
		if (m_ui.language->findData(language) >= 0)
			return;

		m_ui.language->addItem(language, language);
		m_reference->AddLanguage(language);
		m_translation->AddLanguage(language);
	}

	void OnActionValidateTriggered()
	{
		m_model->setData({}, {}, Role::Validate) ? QMessageBox::information(&m_self, Tr(VALIDATION_RESULT), Tr(OK))
												 : QMessageBox::warning(&m_self, Tr(VALIDATION_RESULT), m_model->data({}, Role::Validate).toString());
	}

private:
	MainWindow& m_self;

	PropagateConstPtr<ISettings, std::shared_ptr>          m_settings;
	PropagateConstPtr<QAbstractItemModel, std::shared_ptr> m_model;
	PropagateConstPtr<TranslationWidget, std::shared_ptr>  m_reference;
	PropagateConstPtr<TranslationWidget, std::shared_ptr>  m_translation;

	Ui::MainWindow m_ui;
};

MainWindow::MainWindow(
	std::shared_ptr<ISettings>          settings,
	std::shared_ptr<QAbstractItemModel> model,
	std::shared_ptr<TranslationWidget>  reference,
	std::shared_ptr<TranslationWidget>  translation,
	QWidget*                            parent
)
	: QMainWindow(parent)
	, m_impl(*this, std::move(settings), std::move(model), std::move(reference), std::move(translation))
{
}

MainWindow::~MainWindow() = default;
