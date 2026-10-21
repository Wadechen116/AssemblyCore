#pragma once
// The Hardware Designer: a form editor for the board description (.achw) -- CSRs and their bits, I2C devices, services, constants,
// simulator options. Like App Inventor's Designer it has a component tree on the left and a property form on the right; every change is
// validated at once and the files generated from it (assembler definitions, simulator environment) are put into the workspace, so the
// next build and debug session use them without saving anything.
#include <functional>
#include <map>

#include <QPlainTextEdit>
#include <QTimer>
#include <QWidget>

#include "app_context.h"
#include "asmcore/hardware.h"

class QComboBox;
class QFormLayout;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace acide {

class HardwareDesigner : public QWidget {
    Q_OBJECT
public:
    HardwareDesigner(AppContext& ctx, const std::string& achwPath, QWidget* parent = nullptr);
    ~HardwareDesigner() override;

    const std::string& path() const { return path_; }
    bool loaded() const { return loaded_; }
    const asmcore::Hardware& model() const { return hw_; }
    const std::vector<asmcore::HwProblem>& problems() const { return problems_; }

    enum class Kind { None, General, Equ, Const, Csr, Device, Service, Env, Raw, Profile };
    // Show a component ("csr", "AE_CTRL"); true if it exists.
    bool select(const QString& kind, const QString& name);
    // Add a component of a kind ("csr", "device", "service", "const", "equ", "env") with a free name; returns the new name.
    QString addComponent(const QString& kind);
    void removeCurrent();
    QTreeWidget* tree() const { return tree_; }
    void flush();                                   // apply a pending edit now (tests, save)

private slots:
    void onSelectionChanged();
    void refresh();

private:
    struct Sel { Kind kind = Kind::None; int index = -1; };
    Sel current() const;
    void build();
    QWidget* buildGeneral();
    QWidget* buildEqu();
    QWidget* buildConst();
    QWidget* buildCsr();
    QWidget* buildDevice();
    QWidget* buildService();
    QWidget* buildEnv();
    QWidget* buildRaw();
    QWidget* buildGroup();
    QWidget* buildProfile();
    void reloadProfile();                            // profile file (workspace buffer) -> the form
    void profileEdited(const QString& key, const QString& value);
    void confirmGroup(const QString& group, bool on);
    std::string profilePath() const;                 // absolute, "" if the project has none
    void fillTree(const Sel& keep);
    void load(const Sel& s);                         // model -> form
    void edited();                                   // form -> model was changed: schedule the refresh
    void reloadFromWorkspace();
    void updateCurrentLabel();
    QString labelFor(Kind k, int index) const;
    QString uniqueName(const QString& base) const;
    void showProblemTarget(int row);

    AppContext& ctx_;
    std::string path_;
    asmcore::Hardware hw_;
    std::vector<asmcore::HwProblem> problems_;
    bool loaded_ = false, loading_ = false, applying_ = false;
    QTimer timer_;
    int connection_ = 0;

    QTreeWidget* tree_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QTabWidget* bottom_ = nullptr;
    QTableWidget* problemsTable_ = nullptr;
    QPlainTextEdit *incView_ = nullptr, *envView_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* banner_ = nullptr;
    QPushButton *removeBtn_ = nullptr;

    // general
    QLineEdit *gName_ = nullptr, *gDesc_ = nullptr, *gInc_ = nullptr, *gEnv_ = nullptr;
    // equ
    QLineEdit *eName_ = nullptr, *eValue_ = nullptr, *eDesc_ = nullptr;
    // const
    QLineEdit *kName_ = nullptr, *kValues_ = nullptr, *kDesc_ = nullptr;
    QComboBox* kWidth_ = nullptr;
    // csr
    QLineEdit *cName_ = nullptr, *cAddr_ = nullptr, *cDesc_ = nullptr, *cConst_ = nullptr;
    QComboBox *cKind_ = nullptr, *cBind_ = nullptr;
    QSpinBox *cReset_ = nullptr, *cBytes_ = nullptr;
    QCheckBox* cSim_ = nullptr;
    QTableWidget* cBits_ = nullptr;
    QFormLayout* cForm_ = nullptr;
    QWidget *cResetRow_ = nullptr, *cBindRow_ = nullptr, *cBytesRow_ = nullptr, *cConstRow_ = nullptr;
    QLabel* cBitsHelp_ = nullptr;
    // device
    QLineEdit *dName_ = nullptr, *dSymbol_ = nullptr, *dAddr_ = nullptr, *dDesc_ = nullptr;
    QComboBox *dIndex_ = nullptr, *dKind_ = nullptr;
    QCheckBox* dSim_ = nullptr;
    QTableWidget* dRegs_ = nullptr;
    std::function<void(const QString&, const QString&, const QString&)> addRowFn_;
    // service
    QLineEdit *sName_ = nullptr, *sDesc_ = nullptr;
    QSpinBox *sId_ = nullptr, *sLen_ = nullptr;
    QComboBox* sWire_ = nullptr;
    // env section
    QComboBox* vKind_ = nullptr;
    QLineEdit* vName_ = nullptr;
    QPlainTextEdit* vText_ = nullptr;
    // raw
    QPlainTextEdit* rText_ = nullptr;
    // target profile
    QLabel *profBanner_ = nullptr, *profSummary_ = nullptr;
    QPushButton* profCreate_ = nullptr;
    QTabWidget* profTabs_ = nullptr;
    std::map<std::string, QWidget*> profEditors_;       // key -> its editor
    std::map<std::string, QCheckBox*> profConfirm_;     // group -> "confirmed" checkbox
    bool profLoading_ = false;
    int pageProfile_ = 0;
    int pageGeneral_ = 0, pageEqu_ = 0, pageConst_ = 0, pageCsr_ = 0, pageDevice_ = 0, pageService_ = 0, pageEnv_ = 0, pageRaw_ = 0, pageGroup_ = 0;
};

}  // namespace acide
