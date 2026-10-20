#include <QApplication>
#include <QDir>
#include <QStandardPaths>

#include "app_shell.h"
#include "i18n.h"
#include "qt_dispatcher.h"
#include "text_util.h"

using namespace acide;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("AssemblyCoreIDE");
    QApplication::setOrganizationName("AssemblyCore");

    auto fs = std::make_shared<RealFileSystem>();
    Settings settings;
    const std::string settingsPath = toStd(QDir::fromNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))) + "/settings.ini";
    settings.load(*fs, settingsPath);

    auto dispatcher = std::make_shared<QtDispatcher>();
    auto language = std::make_shared<const asmcore::lang::Language>(asmcore::lang::Language::defaults());
    Workspace workspace(fs);
    BuildService build(dispatcher, language);
    SimSession simulator(dispatcher);
    AppContext* ctxPtr = nullptr;                                  // the debugger asks for the newest build; the context exists a moment later
    Debugger debugger(simulator, workspace, &settings, [&ctxPtr]() -> SnapshotPtr { return ctxPtr ? ctxPtr->snapshot : nullptr; });
    AppContext ctx(workspace, build, settings, debugger, language);
    ctxPtr = &ctx;

    PanelRegistry panels;
    registerBuiltinPanels(panels);

    installLanguage(resolveLanguage(settings.get("ui.language", "auto")));

    int rc;
    {
        AppShell shell(ctx, panels);
        shell.show();
        if (argc > 1 && shell.window()) shell.window()->openProject(QString::fromLocal8Bit(argv[1]));
        rc = app.exec();
    }
    settings.save(*fs, settingsPath);
    return rc;
}
