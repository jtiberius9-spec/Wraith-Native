#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QPointer>
#include <QWidget>
#include <QVector>
#include <QString>
#include <QRect>
#include <QElapsedTimer>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

class QTimer;

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QYUVOpenGLWidget;
class QLabel;
class KeymapEditor;
class VideoForm : public QWidget, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit VideoForm(bool framelessWindow = false, bool skin = true, bool showToolBar = true, QWidget *parent = 0);
    ~VideoForm();

    void staysOnTop(bool top = true);
    void updateShowSize(const QSize &newSize);
    void updateRender(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV);
    void setSerial(const QString& serial);
    QRect getGrabCursorRect();
    const QSize &frameSize();
    void resizeSquare();
    void removeBlackRect();
    void showFPS(bool show);
    void switchFullScreen();
    bool isHost();

    // Keymap editor (overlay) support.
    void setKeyMapFile(const QString &keyMapFile);
    void toggleKeymapEditMode();

    // Recording indicator (driven by Dialog in response to toggleRecordRequested).
    void setRecordingIndicator(bool recording);

signals:
    // Emitted on F12: ask the owner (Dialog) to toggle screen recording for this
    // device. Kept distinct from the F10 keymap-editor toggle.
    void toggleRecordRequested(const QString &serial);

private:
    void onFrame(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    void updateFPS(quint32 fps) override;
    void grabCursor(bool grab) override;
    void recoilHint(const QString &hint) override;

    void updateStyleSheet(bool vertical);
    QMargins getMargins(bool vertical);
    void initUI();

    void showToolForm(bool show = true);
    void moveCenter();
    void installShortcut();
    QRect getScreenRect();

    void enterKeymapEditMode();
    void exitKeymapEditMode();
    void saveKeymapEdits();
    void updateKeymapEditorGeometry();

    // Recoil editor (F8): tune recoilControl in the current keymap and trace a
    // recoil pattern from an image, applied live via updateScript.
    void showRecoilEditor();

    // Record-the-mag: non-modal session so the game stays interactive. F8 stops.
    void startRecoilRecording(const QString &gunName);
    void stopRecoilRecording();
    void recoilRecordTick();
    void pushRecoilEnabled(bool on);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // ui
    Ui::videoForm *ui;
    QPointer<ToolForm> m_toolForm;
    QPointer<QWidget> m_loadingWidget;
    QPointer<QYUVOpenGLWidget> m_videoWidget;
    QPointer<QLabel> m_fpsLabel;

    // keymap editor overlay
    QPointer<KeymapEditor> m_keymapEditor;
    QPointer<QLabel> m_editBanner;
    bool m_editMode = false;
    QString m_keyMapFile;

    // recording indicator overlay ("● REC")
    QPointer<QLabel> m_recIndicator;

    // recoil recording session
    bool m_recording = false;
    QString m_recGun;
    QString m_lastRecoilGun;   // editor reopens on this gun (not always the first)
    QPointer<QLabel> m_hintOverlay;   // brief "Scope: 4x" / "Gun: ..." notifier
    QRect m_recRegion;
    QTimer *m_recTimer = nullptr;
    QPointer<QLabel> m_recOverlay;
    QElapsedTimer m_recClock;
    QVector<double> m_recT, m_recCx, m_recCy;
    double m_recCumX = 0.0, m_recCumY = 0.0;
    QVector<float> m_recPrevV, m_recPrevH;
    bool m_recHavePrev = false;

    //inside member
    QSize m_frameSize;
    QSize m_normalSize;
    QPoint m_dragPosition;
    float m_widthHeightRatio = 0.5f;
    bool m_skin = true;
    QPoint m_fullScreenBeforePos;
    QString m_serial;

    //Whether to display the toolbar when connecting a device.
    bool show_toolbar = true;
};

#endif // VIDEOFORM_H
