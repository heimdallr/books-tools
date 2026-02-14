#pragma once

#include <QMainWindow>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

#include "util/ISettings.h"

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
		std::shared_ptr<TranslationWidget>  reference,
		std::shared_ptr<TranslationWidget>  translation,
		QWidget*                            parent = nullptr
	);
	~MainWindow() override;

private:
	class Impl;
	PropagateConstPtr<Impl> m_impl;
};

}
