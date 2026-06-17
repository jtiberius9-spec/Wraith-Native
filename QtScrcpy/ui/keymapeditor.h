#ifndef KEYMAPEDITOR_H
#define KEYMAPEDITOR_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

class QYUVOpenGLWidget;
class KeymapPalette;
class QLabel;
class QPushButton;
class QSlider;
class QDoubleSpinBox;
class QStackedWidget;
class QGroupBox;
class QComboBox;

// Visual, in-window keymap editor overlay for Wraith.
//
// The OVERLAY is a transparent child widget parented to (and sized to fill) the
// QYUVOpenGLWidget that shows the mirrored video. Because the parent
// KeepRatioWidget already letterboxes the YUV widget so its geometry is
// exactly the displayed video rect, normalized positions (0..1) map to
// pixels with a plain multiply by this overlay's own size. The overlay draws
// and hit-tests the draggable markers and accepts drops.
//
// The PALETTE (drag-source buttons per node type) and a PROPERTIES panel live
// in a SEPARATE top-level tool window (Qt::Tool) titled "Keymap Editor",
// created and owned by this editor, positioned next to the mirror window so it
// never covers the video. The user drags a palette button onto the video; Qt
// delivers the drop across top-level windows to the overlay, which creates a
// node of that type at the drop point and, where applicable, captures the
// key(s) to bind it. Selecting a marker populates the properties panel, whose
// fields are applied live to the in-memory keymap and re-rendered.
//
// The editor owns the parsed keymap JSON. On save it mutates ONLY the
// fields it edited (node positions, added/removed nodes, mouse-move
// startPos, steer-wheel offsets, mouse-move speed ratios) inside the original
// QJsonDocument and writes it back, so drags, multi-clicks, android keys,
// switchKey, comments and any unknown fields are preserved verbatim.
class KeymapEditor : public QWidget
{
    Q_OBJECT
public:
    explicit KeymapEditor(QYUVOpenGLWidget *videoWidget, QWidget *parent = nullptr);
    ~KeymapEditor() override;

    // Load the keymap JSON from disk into the editor model. Returns false if
    // the file can't be read or parsed (the overlay then shows an error hint).
    bool loadFromFile(const QString &filePath);

    // Serialize the (mutated) keymap back to the file it was loaded from.
    // Returns the full JSON text on success (for live-reload), or a null
    // QString on failure.
    QString saveToFile();

    const QString &filePath() const { return m_filePath; }

    // The MIME type the palette uses to carry a KeyMap::KeyMapType (as int)
    // on a QDrag. Public so the palette can build matching mime data.
    static const QString &dragMimeType();

    // The separate tool window holding the palette + properties panel. Shown
    // when edit mode turns on, hidden on exit. Owned by this editor.
    QWidget *toolWindow() const { return m_toolWindow; }

    // Show/hide the tool window, positioning it next to `nextTo` (the mirror
    // window) the first time it is shown so it never overlaps the video.
    void showToolWindow(QWidget *nextTo);
    void hideToolWindow();

signals:
    // Emitted after a successful save with the full pretty-printed JSON text,
    // so the host can live-reload it into the running device.
    void saved(const QString &jsonText);

    // Emitted whenever the on-screen hint/prompt changes (e.g. waiting for a
    // bind, waiting for a steer-wheel direction). The host shows it in its
    // banner. Empty string means "back to default hint".
    void promptChanged(const QString &prompt);

    // Emitted when the user switches the edited keymap via the chooser, or
    // creates a new one. Carries the new file path and its full JSON text so the
    // host can update its current-keymap pointer and live-apply it to the device.
    void keymapSwitched(const QString &filePath, const QString &jsonText);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // A single on-screen, hit-testable marker. It maps back to a location in
    // the JSON document so edits can be written precisely.
    enum MarkerKind
    {
        MK_CLICK,        // keyMapNodes[idx] of type KMT_CLICK (movable, deletable, addable)
        MK_OTHER,        // keyMapNodes[idx] of any other type (movable, deletable; pos only)
        MK_MOUSE_MOVE    // mouseMoveMap.startPos (movable only)
    };

    struct Marker
    {
        MarkerKind kind = MK_CLICK;
        int nodeIndex = -1;       // index into keyMapNodes (-1 for mouse-move map)
        QPointF norm;             // normalized position 0..1
        QString label;            // short label drawn on the marker (key)
        QString typeLabel;        // short type tag (e.g. "CLICK", "WHEEL", "AIM")
        QString kmtType;          // raw JSON "type" string ("KMT_CLICK", ...)
        double wheelOffset = 0.0; // for KMT_STEER_WHEEL: drag radius (max of offsets)
    };

    // What we are currently waiting for the user to press, if anything.
    enum AwaitKind
    {
        AW_NONE = 0,
        AW_BIND_KEY,      // capture one key/button for the pending click-ish node
        AW_WHEEL_UP,      // capture steer-wheel up key
        AW_WHEEL_DOWN,    // then down
        AW_WHEEL_LEFT,    // then left
        AW_WHEEL_RIGHT,   // then right
        AW_REBIND         // re-capture key for an existing marker
    };

    void rebuildMarkers();
    int hitTest(const QPoint &pos) const;
    QPointF normToPixel(const QPointF &norm) const;
    QPointF pixelToNorm(const QPoint &pixel) const;
    void writeNodePos(int markerIdx, const QPointF &norm);
    void removeMarkerNode(int markerIdx);

    // Create a node of the given KeyMapType (int) at the drop point and start
    // whatever capture flow that type needs.
    void beginCreate(int kmtType, const QPointF &norm);

    // Helpers that append a fully-formed node of each kind to keyMapNodes.
    void addClickLikeNode(const QString &kmtType, const QPointF &norm, const QString &keyName);
    void addDragNode(const QPointF &norm, const QString &keyName);
    void addAndroidKeyNode(const QPointF &norm, const QString &keyName);
    void addSteerWheelNode(const QPointF &center, const QString &up, const QString &down,
                           const QString &left, const QString &right);
    void setMouseMoveStartPos(const QPointF &norm);

    // Rebind the key of an existing click-ish / drag / android marker.
    void rebindMarkerKey(int markerIdx, const QString &keyName);

    // Handle a captured key/button name in the current await flow. Returns
    // true if it consumed the input.
    bool handleBindInput(const QString &keyName);

    void cancelAwait();
    void emitPrompt();

    // Map a Qt key event / mouse button to the KeyMap key-name string
    // (e.g. "Key_W", "LeftButton"). Returns empty if unsupported.
    static QString keyEventToName(int qtKey);
    static QString mouseButtonToName(int qtButton);

    static QString markerTypeTag(const QString &kmtType);
    static QString labelForKey(const QString &key);

    // The draw/hit radius of a marker (in px) at the CURRENT video rect size.
    // Computed from m_markerFrac each call so it tracks window resizes; never
    // cached. For a steer wheel the painted ring uses the real offset radius.
    int markerRadius() const;

    // --- tool window / properties panel -------------------------------------
    void buildToolWindow();
    void refreshProperties();          // rebuild the properties panel for m_selectedIdx
    void onRebindClicked();            // "Rebind" button -> start AW_REBIND
    void onSizeChanged(int value);     // marker-size slider OR wheel-offset spin
    void onWheelOffsetChanged(double value);
    void onSensitivityChanged(double value);

    // Keymap file chooser + "New" handlers.
    void refreshKeymapList();          // repopulate the chooser from the keymap dir
    void onChooserChanged(int index);  // chooser selection -> load + live-apply
    void onNewKeymapClicked();         // create a blank keymap and edit it

    // Mutate the selected steer-wheel node's four offsets to the given radius.
    void setWheelOffset(int markerIdx, double offset);
    // Mutate the mouseMoveMap speedRatioX/Y (combined).
    void setMouseMoveSensitivity(double ratio);
    // Read the current combined sensitivity from the mouseMoveMap (avg of X/Y).
    double currentMouseMoveSensitivity() const;

private:
    QYUVOpenGLWidget *m_videoWidget = nullptr;

    QString m_filePath;
    QJsonDocument m_doc;          // the full, preserved keymap document
    bool m_loaded = false;
    QString m_errorHint;          // shown when load fails

    QVector<Marker> m_markers;

    int m_selectedIdx = -1;       // currently selected marker (for delete/props)
    int m_dragIdx = -1;           // marker being dragged (-1 = none)
    QPointF m_dragGrabOffset;     // pixel offset from marker center at grab

    // Global marker display size as a FRACTION of the displayed video rect's
    // smaller dimension, set by the properties-panel "Marker size" slider. The
    // pixel radius is recomputed from this each paint/hit-test (see
    // markerRadius()) so markers stay the same size relative to the video at
    // any window size. Default 0.02 = 2%. Wheels paint their real offset.
    double m_markerFrac = 0.02;

    // Capture state machine.
    AwaitKind m_await = AW_NONE;
    QPointF m_pendingNorm;        // drop point for the node being created
    int m_pendingType = -1;       // KeyMapType being created (AW_BIND_KEY)
    int m_rebindMarker = -1;      // marker index for AW_REBIND

    // Accumulated steer-wheel keys while stepping through the 4 directions.
    QString m_wheelUp, m_wheelDown, m_wheelLeft, m_wheelRight;

    // --- separate tool window (palette + properties) ------------------------
    QWidget *m_toolWindow = nullptr;   // top-level Qt::Tool window
    KeymapPalette *m_palette = nullptr;
    bool m_toolWindowPlaced = false;   // positioned next to mirror once

    QComboBox *m_keymapChooser = nullptr;   // switch the edited keymap file
    QPushButton *m_newKeymapBtn = nullptr;  // create a new keymap file

    // Properties panel widgets (live inside m_toolWindow).
    QLabel *m_propTitle = nullptr;     // "CLICK  —  Key_W" / "(nothing selected)"
    QStackedWidget *m_propStack = nullptr; // page 0 = empty, page 1 = fields
    QPushButton *m_rebindBtn = nullptr;
    QLabel *m_keyValueLabel = nullptr;

    QGroupBox *m_sizeGroup = nullptr;
    QLabel *m_sizeLabel = nullptr;     // "Marker size" or "Wheel radius"
    QSlider *m_markerSizeSlider = nullptr;   // non-wheel: global marker size
    QDoubleSpinBox *m_wheelOffsetSpin = nullptr; // wheel: real offset 0..0.5

    QGroupBox *m_sensGroup = nullptr;  // only for KMT_MOUSE_MOVE
    QDoubleSpinBox *m_sensSpin = nullptr;
    QSlider *m_sensSlider = nullptr;

    bool m_updatingProps = false;      // guard against feedback while populating
};

#endif // KEYMAPEDITOR_H
