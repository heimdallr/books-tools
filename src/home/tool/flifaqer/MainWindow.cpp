#include "ui_MainWindow.h"

#include "MainWindow.h"

#include <ranges>

#include <QClipboard>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStyleFactory>

#include "logging/LogAppender.h"
#include "util/FunctorExecutionForwarder.h"
#include "util/GeometryRestorable.h"
#include "util/StrUtil.h"
#include "util/language.h"

#include "AppConstant.h"
#include "Constant.h"
#include "di_app.h"
#include "log.h"
#include "role.h"

#include "config/version.h"

using namespace HomeCompa::FliFaq;

namespace
{

constexpr auto MAIN_WINDOW = "MainWindow";

constexpr auto LANGUAGE = "language";

constexpr auto FONT_SIZE_KEY = "ui/Font/pointSizeF";

constexpr auto ADD                = QT_TRANSLATE_NOOP("flifaqer", "Add");
constexpr auto REMOVE             = QT_TRANSLATE_NOOP("flifaqer", "Remove");
constexpr auto CREATE_FILE        = QT_TRANSLATE_NOOP("flifaqer", "Create file");
constexpr auto SELECT_TEMPLATE    = QT_TRANSLATE_NOOP("flifaqer", "Select template");
constexpr auto SELECT_FILES       = QT_TRANSLATE_NOOP("flifaqer", "Select files");
constexpr auto SELECT_JSON_FILTER = QT_TRANSLATE_NOOP("flifaqer", "Json files (*.json);;All files (*.*)");
constexpr auto VALIDATION_RESULT  = QT_TRANSLATE_NOOP("flifaqer", "Validation result");
constexpr auto OK                 = QT_TRANSLATE_NOOP("flifaqer", "Everything's cool!");
constexpr auto DATA_CHANGED       = QT_TRANSLATE_NOOP("flifaqer", "Data changed");
constexpr auto SAVE_CHANGES       = QT_TRANSLATE_NOOP("flifaqer", "Would you like to save changes?");
constexpr auto ABOUT_TITLE        = QT_TRANSLATE_NOOP("flifaqer", "About fliFAQer");
constexpr auto ABOUT_TEXT         = QT_TRANSLATE_NOOP("flifaqer", "fliFAQer: question-and-answer reference html generator");

template <typename O, typename P, typename... ARGS>
void OnActionTriggered(P& parent, O& obj, void (O::*invoker)(const ARGS&...), const ARGS&... args)
{
	QString errorText;
	try
	{
		std::invoke(invoker, obj, args...);
	}
	catch (const std::exception& ex)
	{
		errorText = ex.what();
	}
	catch (...)
	{
		errorText = "Unknown error";
	}
	if (errorText.isEmpty())
		return;

	QMessageBox::critical(&parent, Tr(Constant::ERROR), errorText);
	PLOGE << errorText;
}

} // namespace

class MainWindow::Impl final
	: Util::GeometryRestorable
	, Util::GeometryRestorableObserver
	, virtual plog::IAppender
{
	NON_COPY_MOVABLE(Impl)

public:
	Impl(
		MainWindow&                         self,
		std::shared_ptr<ISettings>          settings,
		std::shared_ptr<QAbstractItemModel> model,
		std::shared_ptr<TranslationWidget>  templateWidget,
		std::shared_ptr<TranslationWidget>  referenceWidget,
		std::shared_ptr<TranslationWidget>  translationWidget,
		std::shared_ptr<TextViewWidget>     referenceTextView,
		std::shared_ptr<TextViewWidget>     translationTextView
	)
		: GeometryRestorable(*this, settings, MAIN_WINDOW)
		, GeometryRestorableObserver(self)
		, m_self { self }
		, m_settings { std::move(settings) }
		, m_model { std::move(model) }
		, m_templateWidget { std::move(templateWidget) }
		, m_referenceWidget { std::move(referenceWidget) }
		, m_translationWidget { std::move(translationWidget) }
		, m_referenceTextView { std::move(referenceTextView) }
		, m_translationTextView { std::move(translationTextView) }
	{
		m_ui.setupUi(&m_self);
		m_ui.navigatorView->setModel(m_model.get());
		m_ui.templateLayout->addWidget(m_templateWidget.get());

		const auto setView = [this](QStackedWidget* stackedWidget, const QAction* action, TranslationWidget* trWidget, TextViewWidget* ourTextViewWidget, TextViewWidget* theirTextViewWidget) {
			stackedWidget->addWidget(trWidget);
			stackedWidget->addWidget(theirTextViewWidget);
			stackedWidget->setCurrentIndex(m_settings->Get(stackedWidget->objectName(), 0));
			connect(trWidget, &TranslationWidget::RowChanged, m_templateWidget.get(), &TranslationWidget::SetRow);
			connect(trWidget, &TranslationWidget::LanguageChanged, [this, trWidget, ourTextViewWidget] {
				trWidget->SetCurrentIndex(m_ui.navigatorView->currentIndex());
				ourTextViewWidget->SetCurrentIndex(m_ui.navigatorView->currentIndex());
			});
			connect(action, &QAction::triggered, [this, stackedWidget] {
				stackedWidget->setCurrentIndex((stackedWidget->currentIndex() + 1) % stackedWidget->count());
				m_settings->Set(stackedWidget->objectName(), stackedWidget->currentIndex());
			});
		};
		setView(m_ui.referenceView, m_ui.actionToggleReferenceView, m_referenceWidget.get(), m_referenceTextView.get(), m_translationTextView.get());
		setView(m_ui.translationView, m_ui.actionToggleTranslationView, m_translationWidget.get(), m_translationTextView.get(), m_referenceTextView.get());

		LoadTemplate();
		LoadFiles();

		for (const auto& language : m_model->data({}, Role::LanguageList).toStringList())
			AddLanguage(language);

		m_templateWidget->SetMode(TranslationWidget::Mode::Template);
		m_referenceWidget->SetMode(TranslationWidget::Mode::Reference);
		m_translationWidget->SetMode(TranslationWidget::Mode::Translation);
		m_referenceTextView->SetRole(Role::ReferenceText);
		m_translationTextView->SetRole(Role::TranslationText);

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
			m_templateWidget->SetCurrentIndex(index);
			m_referenceWidget->SetCurrentIndex(index);
			m_translationWidget->SetCurrentIndex(index);
			m_referenceTextView->SetCurrentIndex(index);
			m_translationTextView->SetCurrentIndex(index);
		});

		connect(m_ui.actionCloseAllFiles, &QAction::triggered, [this] {
			m_settings->Remove(Constant::INPUT_FILES);
			m_settings->Remove(Constant::TEMPLATE);
			QCoreApplication::exit(Global::RESTART_APP);
		});
		connect(m_ui.actionCreateNewTemplate, &QAction::triggered, [this] {
			OnActionTriggered(m_self, *this, &Impl::CreateNewTemplate);
		});
		connect(m_ui.actionCreateNewFile, &QAction::triggered, [this] {
			OnActionTriggered(m_self, *this, &Impl::CreateNewFile);
		});
		connect(m_ui.actionAddFiles, &QAction::triggered, [this] {
			OnActionTriggered(m_self, *this, &Impl::AddFiles);
		});
		connect(m_ui.actionSave, &QAction::triggered, [this] {
			OnActionTriggered(m_self, *this, &Impl::Save);
		});
		connect(m_ui.actionExport, &QAction::triggered, [this] {
			OnActionTriggered(m_self, *this, &Impl::Export);
		});
		connect(m_ui.actionValidate, &QAction::triggered, [this] {
			OnActionTriggered(m_self, *this, &Impl::Validate);
		});

		connect(m_ui.actionSetTemplate, &QAction::triggered, [this] {
			OnActionTriggered(m_self, *this, &Impl::SetTemplate);
		});

		connect(m_model.get(), &QAbstractItemModel::dataChanged, [this] {
			m_dataChanged = true;
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
		connect(m_ui.actionAboutFlifaqer, &QAction::triggered, &m_self, [this] {
			ShowAbout();
		});

		const auto currentStyleName = QApplication::style()->name();

		for (const auto& key : QStyleFactory::keys())
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

	bool Close()
	{
		if (!m_dataChanged)
			return true;

		QMessageBox msgBox(QMessageBox::Icon::Question, Tr(DATA_CHANGED), Tr(SAVE_CHANGES), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, &m_self);
		msgBox.setFont(m_self.font());

		switch (msgBox.exec())
		{
			case QMessageBox::No:
				return true;

			case QMessageBox::Yes:
				return Save(), true;

			default:
				break;
		}
		return false;
	}

private: // plog::IAppender
	void write(const plog::Record& record) override
	{
		m_forwarder.Forward([&, message = QString(record.getMessage())] {
			m_ui.statusBar->showMessage(message, 3000);
		});
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

	void CreateNewTemplate()
	{
		auto file = QFileDialog::getSaveFileName(&m_self, Tr(CREATE_FILE), {}, Tr(SELECT_JSON_FILTER));
		if (file.isEmpty())
			return;

		{
			QFile stream(file);
			if (!stream.open(QIODevice::WriteOnly))
				throw std::ios_base::failure(std::format("cannot write to {}", file));

			stream.write(m_model->data({}, Role::NewTemplate).toByteArray());
		}

		SetTemplateImpl(file);
	}

	void CreateNewFile()
	{
		auto languages =
			LANGUAGES
			| std::views::filter([un = QString(UNDEFINED_KEY), addedLanguages = m_model->data({}, Role::LanguageList).toStringList() | std::ranges::to<std::unordered_set<QString>>()](const auto& item) {
				  return !(item.key == un || addedLanguages.contains(item.key));
			  })
			| std::ranges::to<std::vector<Language>>();
		std::ranges::sort(languages, {}, [](const auto& item) {
			return std::pair(item.priority, QString(item.title));
		});

		QInputDialog inputDialog(&m_self);
		inputDialog.setComboBoxItems(
			languages | std::views::transform([](const auto& item) {
				return item.title;
			})
			| std::ranges::to<QStringList>()
		);
		inputDialog.setFont(m_self.font());
		inputDialog.setLabelText("Select language");
		if (inputDialog.exec() != QDialog::Accepted)
			return;

		const auto languageTitle = inputDialog.textValue();
		if (languageTitle.isEmpty())
			return;

		const auto it = std::ranges::find(languages, languageTitle, [](const auto& item) {
			return item.title;
		});
		assert(it != languages.end());

		auto file = QFileDialog::getSaveFileName(&m_self, Tr(CREATE_FILE), {}, Tr(SELECT_JSON_FILTER));
		if (file.isEmpty())
			return;

		{
			QFile stream(file);
			if (!stream.open(QIODevice::WriteOnly))
				throw std::ios_base::failure(std::format("cannot write to {}", file));

			stream.write(m_model->data({}, Role::NewFile).toString().arg(it->key).toUtf8());
		}

		AddFilesImpl({ std::move(file) });
	}

	void AddFiles()
	{
		if (auto inputFiles = QFileDialog::getOpenFileNames(&m_self, Tr(SELECT_FILES), {}, Tr(SELECT_JSON_FILTER)); !inputFiles.isEmpty())
			AddFilesImpl(std::move(inputFiles));
	}

	void AddFilesImpl(QStringList inputFiles)
	{
		auto       files     = m_settings->Get(Constant::INPUT_FILES).toStringList();
		const auto languages = m_model->data({}, Role::LanguageList).toStringList();

		for (auto&& file : inputFiles)
		{
			m_model->setData({}, file, Role::AddFile);
			files << std::move(file);
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
			m_translationWidget->SetLanguage(translationLanguage);
	}

	void Save()
	{
		m_model->setData({}, {}, Role::Save);
		m_dataChanged = false;
	}

	void Export()
	{
		m_model->setData({}, {}, Role::Export);
	}

	void SetTemplate()
	{
		const auto file = QFileDialog::getOpenFileName(&m_self, Tr(SELECT_TEMPLATE), {}, Tr(SELECT_JSON_FILTER));
		if (!file.isEmpty())
			SetTemplateImpl(file);
	}

	void SetTemplateImpl(const QString& file)
	{
		m_settings->Set(Constant::TEMPLATE, file);
		AddTemplate(file);
	}

	void LoadTemplate()
	{
		try
		{
			if (const auto file = m_settings->Get(Constant::TEMPLATE).toString(); !file.isEmpty())
				AddTemplate(file);
			return;
		}
		catch (const std::exception& ex)
		{
			PLOGE << "Load template: " << ex.what();
		}
		catch (...)
		{
			PLOGE << "Load template: unknown error";
		}
		m_settings->Remove(Constant::TEMPLATE);
	}

	void LoadFiles()
	{
		QStringList files;
		for (auto&& file : m_settings->Get(Constant::INPUT_FILES).toStringList())
			try
			{
				m_model->setData({}, file, Role::AddFile);
				files << std::move(file);
			}
			catch (const std::exception& ex)
			{
				PLOGE << "Load " << file << ex.what();
			}
			catch (...)
			{
				PLOGE << "Load template: unknown error";
			}
		m_settings->Set(Constant::INPUT_FILES, files);
	}

	void AddTemplate(const auto& file)
	{
		m_model->setData({}, file, Role::AddTemplate);
		for (const auto& type : m_model->data({}, Role::QuestionTypeList).toStringList())
			m_templateWidget->AddLanguage(type);
		m_templateWidget->SetCurrentIndex(m_ui.navigatorView->currentIndex());
	}

	void AddLanguage(const QString& language)
	{
		if (m_ui.language->findData(language) >= 0)
			return;

		m_ui.language->addItem(TranslationWidget::GetLanguageTitle(language), language);
		m_referenceWidget->AddLanguage(language);
		m_translationWidget->AddLanguage(language);
	}

	void Validate()
	{
		m_model->setData({}, {}, Role::Validate) ? QMessageBox::information(&m_self, Tr(VALIDATION_RESULT), Tr(OK))
												 : QMessageBox::warning(&m_self, Tr(VALIDATION_RESULT), m_model->data({}, Role::Validate).toString());
	}

	void ShowAbout() const
	{
		QMessageBox about(&m_self);
		about.setWindowTitle(Tr(ABOUT_TITLE));
		about.setText(Tr(ABOUT_TEXT));
		about.setIconPixmap(QPixmap(":/icons/main.ico"));
		about.setFont(m_self.font());
		about.exec();
	}

private:
	MainWindow& m_self;

	PropagateConstPtr<ISettings, std::shared_ptr>          m_settings;
	PropagateConstPtr<QAbstractItemModel, std::shared_ptr> m_model;
	PropagateConstPtr<TranslationWidget, std::shared_ptr>  m_templateWidget;
	PropagateConstPtr<TranslationWidget, std::shared_ptr>  m_referenceWidget;
	PropagateConstPtr<TranslationWidget, std::shared_ptr>  m_translationWidget;
	PropagateConstPtr<TextViewWidget, std::shared_ptr>     m_referenceTextView;
	PropagateConstPtr<TextViewWidget, std::shared_ptr>     m_translationTextView;

	bool                            m_dataChanged { false };
	Util::FunctorExecutionForwarder m_forwarder;
	const Log::LogAppender          m_logAppender { this };

	Ui::MainWindow m_ui;
};

MainWindow::MainWindow(
	std::shared_ptr<ISettings>          settings,
	std::shared_ptr<QAbstractItemModel> model,
	std::shared_ptr<TranslationWidget>  templateWidget,
	std::shared_ptr<TranslationWidget>  referenceWidget,
	std::shared_ptr<TranslationWidget>  translationWidget,
	std::shared_ptr<TextViewWidget>     referenceTextView,
	std::shared_ptr<TextViewWidget>     translationTextView,
	QWidget*                            parent
)
	: QMainWindow(parent)
	, m_impl(*this, std::move(settings), std::move(model), std::move(templateWidget), std::move(referenceWidget), std::move(translationWidget), std::move(referenceTextView), std::move(translationTextView))
{
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event)
{
	if (!m_impl->Close())
		event->ignore();
}
