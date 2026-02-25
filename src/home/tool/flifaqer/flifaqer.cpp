#include <ranges>

#include <QAbstractItemModel>
#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStyleFactory>

#include <Hypodermic/Container.h>
#include <Hypodermic/ContainerBuilder.h>

#include "logging/init.h"
#include "util/ISettings.h"

#include "AppConstant.h"
#include "Constant.h"
#include "MainWindow.h"
#include "di_app.h"
#include "log.h"
#include "role.h"

#include "config/git_hash.h"
#include "config/version.h"

namespace HomeCompa
{

class ISettings;

}

using namespace HomeCompa::FliFaq;
using namespace HomeCompa;

int main(int argc, char* argv[])
{
	try
	{
		QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

		QApplication app(argc, argv);
		QCoreApplication::setApplicationName(PRODUCT_ID);
		QCoreApplication::setApplicationVersion(PRODUCT_VERSION);

		Log::LoggingInitializer logging(QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, APP_ID).toStdWString());

		PLOGI << "App started";
		PLOGI << "Commit hash: " << GIT_HASH;
		PLOGD << "QApplication created";

		while (true)
		{
			std::shared_ptr<Hypodermic::Container> container;
			{
				Hypodermic::ContainerBuilder builder;
				DiInit(builder, container);
			}
			PLOGD << "DI-container created";

			const auto settings = container->resolve<ISettings>();
			if (argc > 1)
				settings->Set(Constant::INPUT_FILES, std::views::iota(1, argc) | std::views::transform([&](const int n) {
														 return QDir::fromNativeSeparators(argv[n]);
													 }) | std::ranges::to<QStringList>());

			const auto availableStyles = QStyleFactory::keys();
			auto currentTheme = settings->Get(Constant::THEME).toString();
			if (!availableStyles.contains(currentTheme))
			{
				currentTheme = availableStyles.isEmpty() ? QString() : availableStyles.front();
				if (currentTheme == "windows11" && availableStyles.size() > 1)
					currentTheme = availableStyles[1];
			}
			if (!currentTheme.isEmpty())
				QApplication::setStyle(QStyleFactory::create(currentTheme));

			const auto mainWindow = container->resolve<QMainWindow>();
			mainWindow->show();

			if (const auto code = QApplication::exec(); code != Global::RESTART_APP)
			{
				PLOGI << "App finished with " << code;
				return code;
			}

			PLOGI << "App restarted";
		}
	}
	catch (const std::exception& ex)
	{
		PLOGF << "App failed with " << ex.what();
	}
	catch (...)
	{
		PLOGF << "App failed with unknown error";
	}

	return 1;
}
