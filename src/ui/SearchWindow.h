#ifndef SEARCHWINDOW_H
#define SEARCHWINDOW_H

#include <QWidget>

class HttpClient;
class SearchCoordinator;
class SourceRegistry;
class SearchPanelController;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QPushButton;
class QShowEvent;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui {
class SearchWindow;
}
QT_END_NAMESPACE

/// 独立搜索窗口：搜索框、历史/热门联想、筛选、结果与分页全部从主界面左侧移出。
/// 主窗口只保留播放列表；点搜索结果播放时由 MainWindow 自动隐藏本窗口。
/// 静态布局由 SearchWindow.ui 描述，这里只做控件绑定和运行时状态处理。
class SearchWindow : public QWidget
{
    Q_OBJECT

public:
    SearchWindow(SearchCoordinator *coordinator, SourceRegistry *registry,
                 HttpClient *http, QWidget *parent = nullptr);
    ~SearchWindow() override;

    SearchPanelController *controller() const { return m_controller; }
    void focusSearch();
    /// 搜索进度、失败原因和直链识别结果在搜索窗内显示，不再占用主窗口状态栏。
    void setStatusMessage(const QString &text);

protected:
    void showEvent(QShowEvent *event) override;

private:
    Ui::SearchWindow *ui;

    QLineEdit *m_edit = nullptr;
    QListWidget *m_suggestList = nullptr;
    QWidget *m_filterHost = nullptr;
    QListView *m_list = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_searchHint = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_statusTimer = nullptr;
    SearchPanelController *m_controller = nullptr;
};

#endif // SEARCHWINDOW_H
