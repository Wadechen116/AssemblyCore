#pragma once
// The application window: layout, menus, tabs, file/project commands and the build loop. Features live in panels and the editor; this
// class only connects them.
#include <map>
#include <memory>

#include <QAction>
#include <QLabel>
#include <QToolBar>
#include <QMessageBox>
#include <QMainWindow>
#include <QTabWidget>
#include <QTimer>

#include "app_context.h"
#include "code_editor.h"
#include "find_bar.h"
#include "panel_registry.h"

class QPushButton;
namespace acide {

class HardwareDesigner;
class BlockEditor;
class DocumentTab;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AppContext& ctx, PanelRegistry& panels, QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString& file);
    bool newProject(const QString& file);
    CodeEditor* openFile(const std::string& path);
    CodeEditor* currentEditor() const;
    HardwareDesigner* openHardware(const std::string& achwPath);       // the Hardware Designer tab of a description (created if needed)
    HardwareDesigner* currentDesigner() const;
    bool detachBlocks();                                                // the block program of the current tab becomes plain text again (see the .cpp)
    BlockEditor* openAsBlocks();                                        // the current assembler file shown as blocks (creates NAME.acblk beside it)
    bool newBlocks(const QString& file);
    BlockEditor* openBlocks(const std::string& acblkPath);             // the block editor tab of a .acblk file
    QWidget* openTab(const std::string& path);
    // The simulator page (parameters, execution, the ISA Top diagram): one per window, shown if it is open. A .simparams file opened anywhere brings its values into it.
    QWidget* openSimulator();                         // any non-text tab kind registered in TabRegistry (by extension)
    bool newHardware(const QString& achwPath);                         // File -> Hardware -> New: create, attach to the project, open
    bool adoptHardware(const QString& incPath, const QString& envPath, const QString& achwPath);
    bool closeProject();                  // closes the editors and stops debugging (the workspace keeps its project)
    void endProject();                    // File -> Close Project: save the session, close everything, leave no project open
    // Everything that must be saved before the window is replaced by another one (a language change): the session, the project, the layout.
    // False if the user cancelled.
    bool prepareForRebuild();
    void saveWindowState();
    void refreshLanguageMenu();                           // tick the item of the language setting
    void buildNow(bool writeOutputs);

    // for tests
    QTabWidget* tabs() const { return tabs_; }
    bool closeEditorAt(int index) { return closeEditor(index); }      // closes a tab (asks about unsaved changes)
    QDockWidget* dock(const QString& id) const { return docks_.value(id); }
    QString buildStatusText() const { return buildLabel_->text(); }
    bool checkedAction(const QString& group, const QString& text) const;
    bool checkedLanguage(const QString& code) const;       // is that item of the language menu ticked
    QAction* debugAction(const QString& name) const;      // "start" "pause" "stop" "restart" "into" "over" "out" "tocursor" "breakpoint"
    QString debugStatusText() const { return debugLabel_ ? debugLabel_->text() : QString(); }
    void startDebugging(bool thenRun) { startDebug(thenRun); }
    void flushBuildTimer();
    void setQuiet(bool q) { quiet_ = q; }                  // no modal dialogs (tests): warnings are logged and remembered
    const QStringList& warnings() const { return warnings_; }

protected:
    void closeEvent(QCloseEvent* e) override;
    bool event(QEvent* e) override;

private:
    void createActions();
    void createPanels(PanelRegistry& panels);
    void syncBuildMenu();
    void createLanguageMenu(QMenu* view);
    void warn(const QString& title, const QString& text);
    QMessageBox::StandardButton ask(const QString& title, const QString& text, QMessageBox::StandardButtons buttons, QMessageBox::StandardButton quietAnswer);
    void scheduleBuild();
    void onSnapshot(SnapshotPtr s);
    bool closeEditor(int index);
    bool maybeSaveAll();
    void updateTitle();
    void updateRecentMenu();
    void restoreSession();
    void saveSession();
    void checkExternalChanges();
    void findInProject();
    // ---- debugging
    void createDebugActions();
    void startDebug(bool thenRun);
    void restartDebug();
    void updateDebugActions();
    void onStopped(const SimUpdate& u, const DebugLocation& where);
    void editEnvironmentFile();

    AppContext& ctx_;
    QTabWidget* tabs_;
    FindBar* find_;
    QTimer buildTimer_;
    QLabel *buildLabel_, *caretLabel_;
    QMap<QString, QDockWidget*> docks_;
    QMenu* recentMenu_ = nullptr;
    QAction* closeProjectAct_ = nullptr;
    std::map<std::string, CodeEditor*> editors_;
    std::map<std::string, QWidget*> docTabs_;                // the tabs made by TabRegistry, by path
    bool closeDocTab(int index);
    void refreshGeneratedMarks();
    void flushTabs();
    QAction* openHardwareAct_ = nullptr;
    QAction* blockFromAsmAct_ = nullptr;
    QAction* detachBlocksAct_ = nullptr;
    QAction *actStart_ = nullptr, *actPause_ = nullptr, *actStop_ = nullptr, *actRestart_ = nullptr, *actInto_ = nullptr, *actOver_ = nullptr,
            *actOut_ = nullptr, *actToCursor_ = nullptr, *actToggleBp_ = nullptr;
    QLabel* debugLabel_ = nullptr;
    QPushButton* paramsChip_ = nullptr;                      // "Parameters: 3 changed" in the status bar: a click opens the simulator page
    QAction* quickParamsAct_ = nullptr;
    bool pendingStart_ = false;                            // a build is awaited before the debug session starts
    bool runAfterLoad_ = false;
    uint64_t explicitBuild_ = 0;
    bool askedExternal_ = false;
    bool quiet_ = false;
    QStringList warnings_;
    ConnectionBag conns_;
};

}  // namespace acide
