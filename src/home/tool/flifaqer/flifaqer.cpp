#include <ranges>

#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QStyleFactory>

#include <Hypodermic/Container.h>
#include <Hypodermic/ContainerBuilder.h>

#include "logging/init.h"
#include "util/ISettings.h"

#include "Constant.h"
#include "MainWindow.h"
#include "di_app.h"
#include "log.h"

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
		QApplication::setStyle(QStyleFactory::create("Fusion"));

		Log::LoggingInitializer logging(QString("%1/%2.%3.log").arg(QStandardPaths::writableLocation(QStandardPaths::TempLocation), COMPANY_ID, PRODUCT_ID).toStdWString());

		PLOGI << "App started";
		PLOGI << "Commit hash: " << GIT_HASH;
		PLOGD << "QApplication created";

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

		const auto mainWindow = container->resolve<QMainWindow>();
		mainWindow->show();

		const auto result = QApplication::exec();

		PLOGI << "App finished with " << result;
		return result;
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
