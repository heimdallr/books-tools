#pragma once

#include <QMainWindow>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

#include "settings/ISettings.h"

#include "TextViewWidget.h"
#include "TranslationWidget.h"

class QAbstractItemModel;

namespace HomeCompa::FliFaq
{

class MainWindow final : public QMainWindow
{
	Q_OBJECT
	NON_COPY_MOVABLE(MainWindow)

public:
	MainWindow(
		std::shared_ptr<ISettings>          settings,
		std::shared_ptr<QAbstractItemModel> model,
		std::shared_ptr<TranslationWidget>  templateWidget,
		std::shared_ptr<TranslationWidget>  referenceWidget,
		std::shared_ptr<TranslationWidget>  translationWidget,
		std::shared_ptr<TextViewWidget>     referenceTextView,
		std::shared_ptr<TextViewWidget>     translationTextView,
		QWidget*                            parent = nullptr
	);
	~MainWindow() override;

private: // QWidget
	void closeEvent(QCloseEvent* event) override;

private:
	class Impl;
	PropagateConstPtr<Impl> m_impl;
};

}
