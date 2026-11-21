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

/// 独立搜索窗口：搜索框、历史/热门联想、筛选、结果与分页全部从主界面左侧移出。
/// 主窗口只保留播放列表；点搜索结果播放时由 MainWindow 自动隐藏本窗口。
class SearchWindow : public QWidget
{
    Q_OBJECT

public:
    SearchWindow(SearchCoordinator *coordinator, SourceRegistry *registry,
                 HttpClient *http, QWidget *parent = nullptr);
    ~SearchWindow() override;

    SearchPanelController *controller() const { return m_controller; }
    void focusSearch();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();

    QLineEdit *m_edit = nullptr;
    QListWidget *m_suggestList = nullptr;
    QListView *m_list = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_searchHint = nullptr;
    SearchPanelController *m_controller = nullptr;
};

#endif // SEARCHWINDOW_H
