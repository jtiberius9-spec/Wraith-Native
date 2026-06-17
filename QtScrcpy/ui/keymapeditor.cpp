#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaEnum>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "keymapeditor.h"
#include "qyuvopenglwidget.h"

namespace
{
    // KeyMap::KeyMapType values, mirrored locally so this UI translation unit
    // stays self-contained (the core keymap.h is private to QtScrcpyCore).
    // Must match QtScrcpyCore/.../keymap/keymap.h exactly.
    enum KmtValue
    {
        KMT_CLICK = 0,
        KMT_CLICK_TWICE,
        KMT_CLICK_MULTI,
        KMT_STEER_WHEEL,
        KMT_DRAG,
        KMT_MOUSE_MOVE,
        KMT_ANDROID_KEY
    };

    // Visual constants for markers.
    constexpr int kMarkerHitPad = 4;              // px added to draw radius for easy grabbing
    constexpr int kMinMarkerPx = 4;               // px floor so markers never vanish on tiny windows

    // Marker size is stored as a FRACTION of the video rect's smaller side so
    // it scales with the window. The slider's integer range maps linearly onto
    // the fraction range: slider 5..50 -> 0.005..0.05 (i.e. value/1000).
    constexpr int kMinMarkerSlider = 5;           // -> 0.005 (0.5% of video)
    constexpr int kMaxMarkerSlider = 50;          // -> 0.050 (5%   of video)
    constexpr double kMarkerSliderToFrac = 0.001; // sliderValue * this = fraction

    // Default steer-wheel offset (fraction of the video rect) used for nodes
    // created via the palette. Matches the loose convention of existing maps.
    constexpr double kDefaultWheelOffset = 0.1;
    constexpr double kMinWheelOffset = 0.0;
    constexpr double kMaxWheelOffset = 0.5;

    // Mouse-move sensitivity (speedRatioX/Y) editable range.
    constexpr double kMinSensitivity = 0.1;
    constexpr double kMaxSensitivity = 5.0;

    // QMetaEnum machinery, matching KeyMap::getItemKey, so the names we emit
    // are guaranteed to round-trip through the loader.
    const QMetaEnum &metaKey()
    {
        static QMetaEnum e = QMetaEnum::fromType<Qt::Key>();
        return e;
    }
    const QMetaEnum &metaMouseButtons()
    {
        static QMetaEnum e = QMetaEnum::fromType<Qt::MouseButtons>();
        return e;
    }

    // Type-name string the loader's QMetaEnum<KeyMapType> expects, by enum value.
    QString kmtName(int kmt)
    {
        switch (kmt) {
        case KMT_CLICK:       return QStringLiteral("KMT_CLICK");
        case KMT_CLICK_TWICE: return QStringLiteral("KMT_CLICK_TWICE");
        case KMT_CLICK_MULTI: return QStringLiteral("KMT_CLICK_MULTI");
        case KMT_STEER_WHEEL: return QStringLiteral("KMT_STEER_WHEEL");
        case KMT_DRAG:        return QStringLiteral("KMT_DRAG");
        case KMT_MOUSE_MOVE:  return QStringLiteral("KMT_MOUSE_MOVE");
        case KMT_ANDROID_KEY: return QStringLiteral("KMT_ANDROID_KEY");
        default:                      return QString();
        }
    }
}

// ---------------------------------------------------------------------------
// KeymapPalette: a vertical dock with one draggable button per node type. It
// lives inside the separate tool window (NOT over the video). Each button
// starts a QDrag carrying the KeyMapType; Qt delivers the drop across the
// top-level boundary to the video overlay, which creates the right node.
// ---------------------------------------------------------------------------
class KeymapPalette : public QWidget
{
public:
    explicit KeymapPalette(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QStringLiteral(R"(
            KeymapPalette {
                background-color: rgba(18, 18, 22, 205);
                border: 1px solid rgba(255,255,255,40);
                border-radius: 6px;
            }
            QLabel#title {
                color: #FFD700;
                font-weight: bold;
                padding: 2px 4px;
            }
            QPushButton {
                background-color: rgba(53, 74, 183, 210);
                color: white;
                border: 1px solid rgba(255,255,255,60);
                border-radius: 4px;
                padding: 5px 8px;
                text-align: left;
                font-weight: bold;
            }
            QPushButton:hover { background-color: rgba(80, 104, 220, 230); }
        )"));

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(4);

        auto *title = new QLabel(tr("DRAG ONTO VIDEO"), this);
        title->setObjectName(QStringLiteral("title"));
        lay->addWidget(title);

        addButton(lay, tr("Click"),        KMT_CLICK);
        addButton(lay, tr("Double Click"), KMT_CLICK_TWICE);
        addButton(lay, tr("Multi Click"),  KMT_CLICK_MULTI);
        addButton(lay, tr("Steer Wheel"),  KMT_STEER_WHEEL);
        addButton(lay, tr("Drag"),         KMT_DRAG);
        addButton(lay, tr("Mouse Move"),   KMT_MOUSE_MOVE);
        addButton(lay, tr("Android Key"),  KMT_ANDROID_KEY);
        lay->addStretch(1);

        adjustSize();
    }

private:
    // A QPushButton subclass that begins a QDrag on press.
    class DragButton : public QPushButton
    {
    public:
        DragButton(const QString &text, int kmt, QWidget *parent)
            : QPushButton(text, parent), m_kmt(kmt)
        {
            setCursor(Qt::OpenHandCursor);
        }

    protected:
        void mousePressEvent(QMouseEvent *event) override
        {
            if (event->button() != Qt::LeftButton) {
                QPushButton::mousePressEvent(event);
                return;
            }
            auto *mime = new QMimeData();
            mime->setData(KeymapEditor::dragMimeType(), QByteArray::number(m_kmt));
            // Also carry plain text so the drag has a friendly fallback.
            mime->setText(text());

            auto *drag = new QDrag(this);
            drag->setMimeData(mime);
            drag->exec(Qt::CopyAction);
        }

    private:
        int m_kmt;
    };

    void addButton(QVBoxLayout *lay, const QString &text, int kmt)
    {
        auto *btn = new DragButton(text, kmt, this);
        lay->addWidget(btn);
    }
};

// ---------------------------------------------------------------------------

KeymapEditor::KeymapEditor(QYUVOpenGLWidget *videoWidget, QWidget *parent)
    : QWidget(parent), m_videoWidget(videoWidget)
{
    // Transparent overlay that paints on top of the video and receives input.
    // The palette/properties now live in a separate tool window, so the whole
    // video rect is free for placing nodes.
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);

    // Build the separate floating tool window (palette + properties panel).
    buildToolWindow();

    // Always track the (letterboxed) video widget's size so markers stay put
    // when the window is resized.
    if (m_videoWidget) {
        m_videoWidget->installEventFilter(this);
        setGeometry(QRect(QPoint(0, 0), m_videoWidget->size()));
    }
}

KeymapEditor::~KeymapEditor()
{
    if (m_videoWidget) {
        m_videoWidget->removeEventFilter(this);
    }
    // m_toolWindow is a top-level window we own explicitly (no QObject parent).
    delete m_toolWindow;
    m_toolWindow = nullptr;
}

int KeymapEditor::markerRadius() const
{
    // Recompute from the current overlay size each call so the marker stays the
    // same size RELATIVE to the video as the window resizes. The overlay is
    // sized exactly to the displayed (letterboxed) video rect.
    const int smaller = qMin(width(), height());
    const int r = static_cast<int>(m_markerFrac * smaller + 0.5);
    return qMax(kMinMarkerPx, r);
}

const QString &KeymapEditor::dragMimeType()
{
    static const QString s = QStringLiteral("application/x-wraith-keymap-nodetype");
    return s;
}

bool KeymapEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_videoWidget && event->type() == QEvent::Resize) {
        setGeometry(QRect(QPoint(0, 0), m_videoWidget->size()));
    }
    return QWidget::eventFilter(watched, event);
}

bool KeymapEditor::loadFromFile(const QString &filePath)
{
    m_loaded = false;
    m_errorHint.clear();
    m_markers.clear();
    m_selectedIdx = -1;
    m_dragIdx = -1;
    cancelAwait();
    m_filePath = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_errorHint = tr("Cannot open keymap: %1").arg(filePath);
        update();
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError err;
    m_doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !m_doc.isObject()) {
        m_errorHint = tr("Invalid keymap JSON: %1").arg(err.errorString());
        update();
        return false;
    }

    m_loaded = true;
    rebuildMarkers();
    refreshProperties();
    refreshKeymapList();
    update();
    return true;
}

QString KeymapEditor::markerTypeTag(const QString &kmtType)
{
    if (kmtType == "KMT_CLICK") {
        return "CLICK";
    }
    if (kmtType == "KMT_CLICK_TWICE") {
        return "2x";
    }
    if (kmtType == "KMT_CLICK_MULTI") {
        return "MULTI";
    }
    if (kmtType == "KMT_STEER_WHEEL") {
        return "WHEEL";
    }
    if (kmtType == "KMT_DRAG") {
        return "DRAG";
    }
    if (kmtType == "KMT_ANDROID_KEY") {
        return "AKEY";
    }
    return kmtType;
}

QString KeymapEditor::labelForKey(const QString &key)
{
    if (key == "LeftButton") {
        return "LMB";
    }
    if (key == "RightButton") {
        return "RMB";
    }
    if (key == "MiddleButton") {
        return "MMB";
    }
    if (key.startsWith("Key_")) {
        return key.mid(4);
    }
    return key;
}

void KeymapEditor::rebuildMarkers()
{
    m_markers.clear();
    if (!m_loaded) {
        return;
    }

    const QJsonObject root = m_doc.object();

    // mouse-move map "AIM" marker (startPos)
    const QJsonValue mmVal = root.value("mouseMoveMap");
    if (mmVal.isObject()) {
        const QJsonObject mm = mmVal.toObject();
        const QJsonValue startPos = mm.value("startPos");
        if (startPos.isObject()) {
            const QJsonObject sp = startPos.toObject();
            Marker m;
            m.kind = MK_MOUSE_MOVE;
            m.nodeIndex = -1;
            m.norm = QPointF(sp.value("x").toDouble(), sp.value("y").toDouble());
            m.label = "AIM";
            m.typeLabel = "MOVE";
            m.kmtType = "KMT_MOUSE_MOVE";
            m_markers.push_back(m);
        }
    }

    // keyMapNodes
    const QJsonValue nodesVal = root.value("keyMapNodes");
    if (nodesVal.isArray()) {
        const QJsonArray nodes = nodesVal.toArray();
        for (int i = 0; i < nodes.size(); ++i) {
            const QJsonObject node = nodes.at(i).toObject();
            const QString type = node.value("type").toString();

            Marker m;
            m.nodeIndex = i;
            m.typeLabel = markerTypeTag(type);
            m.kmtType = type;

            if (type == "KMT_STEER_WHEEL") {
                // Steer wheel is positioned by centerPos.
                const QJsonObject c = node.value("centerPos").toObject();
                m.kind = MK_OTHER;
                m.norm = QPointF(c.value("x").toDouble(), c.value("y").toDouble());
                m.label = "WASD";
                // Drag radius = the largest of the four directional offsets.
                const double lo = node.value("leftOffset").toDouble();
                const double ro = node.value("rightOffset").toDouble();
                const double uo = node.value("upOffset").toDouble();
                const double dno = node.value("downOffset").toDouble();
                m.wheelOffset = qMax(qMax(lo, ro), qMax(uo, dno));
            } else if (type == "KMT_DRAG") {
                // Drag uses startPos for its anchor.
                const QJsonObject pos = node.value("startPos").toObject();
                m.kind = MK_OTHER;
                m.norm = QPointF(pos.value("x").toDouble(), pos.value("y").toDouble());
                m.label = labelForKey(node.value("key").toString());
            } else {
                const QJsonObject pos = node.value("pos").toObject();
                m.norm = QPointF(pos.value("x").toDouble(), pos.value("y").toDouble());
                m.kind = (type == "KMT_CLICK") ? MK_CLICK : MK_OTHER;
                m.label = labelForKey(node.value("key").toString());
            }
            m_markers.push_back(m);
        }
    }
}

QPointF KeymapEditor::normToPixel(const QPointF &norm) const
{
    return QPointF(norm.x() * width(), norm.y() * height());
}

QPointF KeymapEditor::pixelToNorm(const QPoint &pixel) const
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) {
        return QPointF(0, 0);
    }
    double nx = static_cast<double>(pixel.x()) / w;
    double ny = static_cast<double>(pixel.y()) / h;
    nx = qBound(0.0, nx, 1.0);
    ny = qBound(0.0, ny, 1.0);
    return QPointF(nx, ny);
}

int KeymapEditor::hitTest(const QPoint &pos) const
{
    const int hit = markerRadius() + kMarkerHitPad;
    // Iterate in reverse so the topmost (last drawn) marker wins.
    for (int i = m_markers.size() - 1; i >= 0; --i) {
        const QPointF c = normToPixel(m_markers.at(i).norm);
        const QPointF d = QPointF(pos) - c;
        if (d.x() * d.x() + d.y() * d.y() <= static_cast<double>(hit) * hit) {
            return i;
        }
    }
    return -1;
}

void KeymapEditor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (!m_loaded) {
        p.setPen(QColor(255, 80, 80));
        QFont f = p.font();
        f.setPointSize(11);
        p.setFont(f);
        p.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap,
                   m_errorHint.isEmpty() ? tr("No keymap loaded") : m_errorHint);
        return;
    }

    QFont labelFont = p.font();
    labelFont.setPointSize(9);
    labelFont.setBold(true);

    const int r = markerRadius();

    for (int i = 0; i < m_markers.size(); ++i) {
        const Marker &m = m_markers.at(i);
        const QPointF c = normToPixel(m.norm);

        QColor fill;
        switch (m.kind) {
        case MK_CLICK:
            fill = QColor(53, 74, 183, 200);   // purple-ish (editable click)
            break;
        case MK_MOUSE_MOVE:
            fill = QColor(216, 90, 48, 200);    // coral (aim)
            break;
        case MK_OTHER:
        default:
            fill = QColor(95, 94, 90, 200);     // gray (drag/wheel/etc.)
            break;
        }

        // A steer wheel additionally draws its real drag-radius (the offset),
        // so the user can see/size the actual wheel travel on the video.
        if (m.kmtType == "KMT_STEER_WHEEL" && m.wheelOffset > 0.0) {
            // Offset is a fraction of the video rect; use the geometric mean of
            // width/height as a single pixel radius (rect is letterboxed so the
            // two are close, and the wheel is conceptually circular).
            const double rad = m.wheelOffset * (width() + height()) * 0.5;
            QPen wp(QColor(255, 215, 0, 150));
            wp.setWidth(2);
            p.setPen(wp);
            p.setBrush(QColor(255, 215, 0, 30));
            p.drawEllipse(c, rad, rad);
        }

        const bool selected = (i == m_selectedIdx);
        QPen ring(selected ? QColor(255, 215, 0) : QColor(255, 255, 255, 220));
        ring.setWidth(selected ? 3 : 2);
        p.setPen(ring);
        p.setBrush(fill);
        p.drawEllipse(c, r, r);

        // Key label centered in the marker.
        p.setFont(labelFont);
        p.setPen(Qt::white);
        QRectF labelRect(c.x() - r, c.y() - r, 2 * r, 2 * r);
        p.drawText(labelRect, Qt::AlignCenter, m.label);

        // Type tag below the marker.
        QFont tagFont = labelFont;
        tagFont.setPointSize(7);
        tagFont.setBold(false);
        p.setFont(tagFont);
        p.setPen(QColor(255, 255, 255, 180));
        QRectF tagRect(c.x() - 40, c.y() + r + 1, 80, 12);
        p.drawText(tagRect, Qt::AlignHCenter | Qt::AlignTop, m.typeLabel);
    }

    // Dashed ring + prompt while waiting to capture a key/button.
    if (m_await != AW_NONE && m_await != AW_REBIND) {
        const QPointF c = normToPixel(m_pendingNorm);
        QPen pen(QColor(99, 153, 34));
        pen.setWidth(2);
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r, r);
    } else if (m_await == AW_REBIND && m_rebindMarker >= 0 && m_rebindMarker < m_markers.size()) {
        const QPointF c = normToPixel(m_markers.at(m_rebindMarker).norm);
        QPen pen(QColor(99, 153, 34));
        pen.setWidth(2);
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r + 4, r + 4);
    }
}

void KeymapEditor::mousePressEvent(QMouseEvent *event)
{
    if (!m_loaded) {
        return;
    }

    // If awaiting a bind, a mouse button press supplies the key name.
    if (m_await != AW_NONE) {
        const QString name = mouseButtonToName(event->button());
        if (!name.isEmpty()) {
            handleBindInput(name);
        }
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        event->accept();
        return;
    }

    const int idx = hitTest(event->pos());
    m_selectedIdx = idx;
    if (idx >= 0) {
        m_dragIdx = idx;
        m_dragGrabOffset = QPointF(event->pos()) - normToPixel(m_markers.at(idx).norm);
    }
    refreshProperties();
    update();
    event->accept();
}

void KeymapEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_loaded || m_dragIdx < 0) {
        return;
    }
    const QPoint target = (QPointF(event->pos()) - m_dragGrabOffset).toPoint();
    const QPointF norm = pixelToNorm(target);
    m_markers[m_dragIdx].norm = norm;
    update();
    event->accept();
}

void KeymapEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_loaded || m_dragIdx < 0) {
        return;
    }
    // Commit the dragged position back into the JSON document.
    writeNodePos(m_dragIdx, m_markers.at(m_dragIdx).norm);
    m_dragIdx = -1;
    update();
    event->accept();
}

void KeymapEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_loaded) {
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->accept();
        return;
    }

    // Double-click on an existing click-ish marker re-captures its key.
    const int idx = hitTest(event->pos());
    if (idx >= 0) {
        const Marker &m = m_markers.at(idx);
        if (m.kind != MK_MOUSE_MOVE && m.nodeIndex >= 0) {
            const QJsonArray nodes = m_doc.object().value("keyMapNodes").toArray();
            const QString type = nodes.at(m.nodeIndex).toObject().value("type").toString();
            // Only single-key nodes are rebindable here; steer wheel is multi-key.
            if (type == "KMT_CLICK" || type == "KMT_CLICK_TWICE" || type == "KMT_CLICK_MULTI"
                || type == "KMT_DRAG" || type == "KMT_ANDROID_KEY") {
                cancelAwait();
                m_await = AW_REBIND;
                m_rebindMarker = idx;
                m_selectedIdx = idx;
                refreshProperties();
                setFocus();
                emitPrompt();
                update();
            }
        }
        event->accept();
        return;
    }

    // Double-click on empty space adds a KMT_CLICK node (legacy convenience).
    cancelAwait();
    beginCreate(KMT_CLICK, pixelToNorm(event->pos()));
    event->accept();
}

void KeymapEditor::keyPressEvent(QKeyEvent *event)
{
    if (!m_loaded) {
        return;
    }

    if (m_await != AW_NONE) {
        if (event->key() == Qt::Key_Escape) {
            // For steer-wheel direction capture, Esc means "use the default
            // for the remaining directions" (W/S/A/D). For other flows it
            // cancels outright.
            if (m_await == AW_WHEEL_UP || m_await == AW_WHEEL_DOWN
                || m_await == AW_WHEEL_LEFT || m_await == AW_WHEEL_RIGHT) {
                if (m_wheelUp.isEmpty())    m_wheelUp = "Key_W";
                if (m_wheelDown.isEmpty())  m_wheelDown = "Key_S";
                if (m_wheelLeft.isEmpty())  m_wheelLeft = "Key_A";
                if (m_wheelRight.isEmpty()) m_wheelRight = "Key_D";
                addSteerWheelNode(m_pendingNorm, m_wheelUp, m_wheelDown, m_wheelLeft, m_wheelRight);
                cancelAwait();
                refreshProperties();
                update();
            } else {
                cancelAwait();
                update();
            }
            event->accept();
            return;
        }
        const QString name = keyEventToName(event->key());
        if (!name.isEmpty()) {
            handleBindInput(name);
        }
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_selectedIdx >= 0) {
            removeMarkerNode(m_selectedIdx);
            m_selectedIdx = -1;
            refreshProperties();
            update();
        }
        event->accept();
        return;
    }

    // Let other keys (F10, Ctrl+S, ...) fall through to the parent VideoForm.
    event->ignore();
}

// --- drag-and-drop from the palette -----------------------------------------

void KeymapEditor::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_loaded && event->mimeData()->hasFormat(dragMimeType())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void KeymapEditor::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_loaded && event->mimeData()->hasFormat(dragMimeType())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void KeymapEditor::dropEvent(QDropEvent *event)
{
    if (!m_loaded || !event->mimeData()->hasFormat(dragMimeType())) {
        event->ignore();
        return;
    }
    bool ok = false;
    const int kmt = event->mimeData()->data(dragMimeType()).toInt(&ok);
    if (!ok) {
        event->ignore();
        return;
    }
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    const QPoint dropPos = event->position().toPoint();
#else
    const QPoint dropPos = event->pos();
#endif
    // The palette now lives in a SEPARATE top-level tool window, so any drop
    // that reaches this overlay is genuinely on the video.
    cancelAwait();
    beginCreate(kmt, pixelToNorm(dropPos));
    event->setDropAction(Qt::CopyAction);
    event->accept();
    setFocus();
    // Raise the mirror window so focus/capture continues in the video.
    if (window()) {
        window()->activateWindow();
    }
}

// --- node creation flows -----------------------------------------------------

void KeymapEditor::beginCreate(int kmtType, const QPointF &norm)
{
    m_pendingNorm = norm;

    switch (kmtType) {
    case KMT_MOUSE_MOVE:
        // No key: it is the aim region. Only one allowed; move if it exists.
        setMouseMoveStartPos(norm);
        update();
        break;
    case KMT_STEER_WHEEL:
        // Walk the four directions; defaults to WASD on Esc.
        m_wheelUp.clear();
        m_wheelDown.clear();
        m_wheelLeft.clear();
        m_wheelRight.clear();
        m_await = AW_WHEEL_UP;
        setFocus();
        emitPrompt();
        update();
        break;
    case KMT_CLICK:
    case KMT_CLICK_TWICE:
    case KMT_CLICK_MULTI:
    case KMT_DRAG:
    case KMT_ANDROID_KEY:
        m_pendingType = kmtType;
        m_await = AW_BIND_KEY;
        setFocus();
        emitPrompt();
        update();
        break;
    default:
        break;
    }
}

bool KeymapEditor::handleBindInput(const QString &keyName)
{
    switch (m_await) {
    case AW_BIND_KEY: {
        const int kmt = m_pendingType;
        if (kmt == KMT_DRAG) {
            addDragNode(m_pendingNorm, keyName);
        } else if (kmt == KMT_ANDROID_KEY) {
            addAndroidKeyNode(m_pendingNorm, keyName);
        } else {
            addClickLikeNode(kmtName(kmt), m_pendingNorm, keyName);
        }
        cancelAwait();
        refreshProperties();
        update();
        return true;
    }
    case AW_WHEEL_UP:
        m_wheelUp = keyName;
        m_await = AW_WHEEL_DOWN;
        emitPrompt();
        update();
        return true;
    case AW_WHEEL_DOWN:
        m_wheelDown = keyName;
        m_await = AW_WHEEL_LEFT;
        emitPrompt();
        update();
        return true;
    case AW_WHEEL_LEFT:
        m_wheelLeft = keyName;
        m_await = AW_WHEEL_RIGHT;
        emitPrompt();
        update();
        return true;
    case AW_WHEEL_RIGHT:
        m_wheelRight = keyName;
        addSteerWheelNode(m_pendingNorm, m_wheelUp, m_wheelDown, m_wheelLeft, m_wheelRight);
        cancelAwait();
        refreshProperties();
        update();
        return true;
    case AW_REBIND:
        rebindMarkerKey(m_rebindMarker, keyName);
        cancelAwait();
        refreshProperties();
        update();
        return true;
    default:
        return false;
    }
}

void KeymapEditor::cancelAwait()
{
    m_await = AW_NONE;
    m_pendingType = -1;
    m_rebindMarker = -1;
    m_wheelUp.clear();
    m_wheelDown.clear();
    m_wheelLeft.clear();
    m_wheelRight.clear();
    emitPrompt();
}

void KeymapEditor::emitPrompt()
{
    QString prompt;
    switch (m_await) {
    case AW_BIND_KEY:
        prompt = tr("Press a key or mouse button to bind  (Esc to cancel)");
        break;
    case AW_WHEEL_UP:
        prompt = tr("Steer Wheel: press the UP key  (Esc = default WASD)");
        break;
    case AW_WHEEL_DOWN:
        prompt = tr("Steer Wheel: press the DOWN key  (Esc = default WASD)");
        break;
    case AW_WHEEL_LEFT:
        prompt = tr("Steer Wheel: press the LEFT key  (Esc = default WASD)");
        break;
    case AW_WHEEL_RIGHT:
        prompt = tr("Steer Wheel: press the RIGHT key  (Esc = default WASD)");
        break;
    case AW_REBIND:
        prompt = tr("Rebind: press the new key or mouse button  (Esc to cancel)");
        break;
    case AW_NONE:
    default:
        prompt.clear();
        break;
    }
    emit promptChanged(prompt);
}

// --- JSON mutation -----------------------------------------------------------

void KeymapEditor::writeNodePos(int markerIdx, const QPointF &norm)
{
    if (markerIdx < 0 || markerIdx >= m_markers.size()) {
        return;
    }
    const Marker &m = m_markers.at(markerIdx);
    QJsonObject root = m_doc.object();

    QJsonObject newPos;
    newPos.insert("x", norm.x());
    newPos.insert("y", norm.y());

    if (m.kind == MK_MOUSE_MOVE) {
        QJsonObject mm = root.value("mouseMoveMap").toObject();
        mm.insert("startPos", newPos);
        root.insert("mouseMoveMap", mm);
    } else {
        QJsonArray nodes = root.value("keyMapNodes").toArray();
        if (m.nodeIndex < 0 || m.nodeIndex >= nodes.size()) {
            return;
        }
        QJsonObject node = nodes.at(m.nodeIndex).toObject();
        const QString type = node.value("type").toString();
        // Each type stores its anchor under a different field.
        if (type == "KMT_STEER_WHEEL") {
            node.insert("centerPos", newPos);
        } else if (type == "KMT_DRAG") {
            // Move the whole drag: keep the vector from start->end constant.
            const QJsonObject oldStart = node.value("startPos").toObject();
            const QJsonObject oldEnd = node.value("endPos").toObject();
            const double dx = oldEnd.value("x").toDouble() - oldStart.value("x").toDouble();
            const double dy = oldEnd.value("y").toDouble() - oldStart.value("y").toDouble();
            QJsonObject newEnd;
            newEnd.insert("x", qBound(0.0, norm.x() + dx, 1.0));
            newEnd.insert("y", qBound(0.0, norm.y() + dy, 1.0));
            node.insert("startPos", newPos);
            node.insert("endPos", newEnd);
        } else {
            node.insert("pos", newPos);
        }
        nodes.replace(m.nodeIndex, node);
        root.insert("keyMapNodes", nodes);
    }
    m_doc.setObject(root);
}

void KeymapEditor::addClickLikeNode(const QString &kmtType, const QPointF &norm, const QString &keyName)
{
    QJsonObject root = m_doc.object();
    QJsonArray nodes = root.value("keyMapNodes").toArray();

    QJsonObject node;
    node.insert("comment", QString(""));
    node.insert("type", kmtType);
    QJsonObject pos;
    pos.insert("x", norm.x());
    pos.insert("y", norm.y());
    node.insert("pos", pos);
    node.insert("key", keyName);
    node.insert("switchMap", false);

    // KMT_CLICK_MULTI requires a non-empty clickNodes array to load; seed it
    // with a single zero-delay click at the same position so it is valid.
    if (kmtType == "KMT_CLICK_MULTI") {
        QJsonObject cn;
        cn.insert("delay", 0);
        QJsonObject cnPos;
        cnPos.insert("x", norm.x());
        cnPos.insert("y", norm.y());
        cn.insert("pos", cnPos);
        QJsonArray clickNodes;
        clickNodes.append(cn);
        node.insert("clickNodes", clickNodes);
    }

    nodes.append(node);
    root.insert("keyMapNodes", nodes);
    m_doc.setObject(root);

    rebuildMarkers();
    m_selectedIdx = m_markers.size() - 1;
}

void KeymapEditor::addDragNode(const QPointF &norm, const QString &keyName)
{
    QJsonObject root = m_doc.object();
    QJsonArray nodes = root.value("keyMapNodes").toArray();

    QJsonObject node;
    node.insert("comment", QString(""));
    node.insert("type", QString("KMT_DRAG"));
    QJsonObject startPos;
    startPos.insert("x", norm.x());
    startPos.insert("y", norm.y());
    node.insert("startPos", startPos);
    // Default end point a little below-right of start; user can drag to adjust.
    QJsonObject endPos;
    endPos.insert("x", qBound(0.0, norm.x() + 0.1, 1.0));
    endPos.insert("y", qBound(0.0, norm.y() + 0.1, 1.0));
    node.insert("endPos", endPos);
    node.insert("key", keyName);

    nodes.append(node);
    root.insert("keyMapNodes", nodes);
    m_doc.setObject(root);

    rebuildMarkers();
    m_selectedIdx = m_markers.size() - 1;
}

void KeymapEditor::addAndroidKeyNode(const QPointF &norm, const QString &keyName)
{
    QJsonObject root = m_doc.object();
    QJsonArray nodes = root.value("keyMapNodes").toArray();

    QJsonObject node;
    node.insert("comment", QString(""));
    node.insert("type", QString("KMT_ANDROID_KEY"));
    // pos is not used by the loader for android key, but keep it so the marker
    // has a position and round-trips cleanly.
    QJsonObject pos;
    pos.insert("x", norm.x());
    pos.insert("y", norm.y());
    node.insert("pos", pos);
    node.insert("key", keyName);
    // androidKey is required (double). 0 = AKEYCODE_UNKNOWN placeholder; the
    // user can set a real keycode in the JSON. It is preserved on save.
    node.insert("androidKey", 0);

    nodes.append(node);
    root.insert("keyMapNodes", nodes);
    m_doc.setObject(root);

    rebuildMarkers();
    m_selectedIdx = m_markers.size() - 1;
}

void KeymapEditor::addSteerWheelNode(const QPointF &center, const QString &up, const QString &down,
                                     const QString &left, const QString &right)
{
    QJsonObject root = m_doc.object();
    QJsonArray nodes = root.value("keyMapNodes").toArray();

    QJsonObject node;
    node.insert("comment", QString(""));
    node.insert("type", QString("KMT_STEER_WHEEL"));
    QJsonObject centerPos;
    centerPos.insert("x", center.x());
    centerPos.insert("y", center.y());
    node.insert("centerPos", centerPos);
    node.insert("leftOffset", kDefaultWheelOffset);
    node.insert("rightOffset", kDefaultWheelOffset);
    node.insert("upOffset", kDefaultWheelOffset);
    node.insert("downOffset", kDefaultWheelOffset);
    node.insert("leftKey", left.isEmpty() ? QString("Key_A") : left);
    node.insert("rightKey", right.isEmpty() ? QString("Key_D") : right);
    node.insert("upKey", up.isEmpty() ? QString("Key_W") : up);
    node.insert("downKey", down.isEmpty() ? QString("Key_S") : down);
    node.insert("switchMap", false);

    nodes.append(node);
    root.insert("keyMapNodes", nodes);
    m_doc.setObject(root);

    rebuildMarkers();
    m_selectedIdx = m_markers.size() - 1;
}

void KeymapEditor::setMouseMoveStartPos(const QPointF &norm)
{
    QJsonObject root = m_doc.object();

    QJsonObject newPos;
    newPos.insert("x", norm.x());
    newPos.insert("y", norm.y());

    QJsonObject mm = root.value("mouseMoveMap").toObject();
    // Create a minimal valid mouseMoveMap if none exists. The loader requires
    // a speedRatio (or X/Y) and a startPos.
    if (mm.isEmpty()) {
        mm.insert("comment", QString(""));
        mm.insert("type", QString("KMT_MOUSE_MOVE"));
        mm.insert("speedRatioX", 1.0);
        mm.insert("speedRatioY", 1.0);
    } else if (!mm.contains("speedRatio") && !mm.contains("speedRatioX") && !mm.contains("speedRatioY")) {
        mm.insert("speedRatioX", 1.0);
        mm.insert("speedRatioY", 1.0);
    }
    mm.insert("startPos", newPos);
    root.insert("mouseMoveMap", mm);
    m_doc.setObject(root);

    rebuildMarkers();
    // Select the AIM marker (it is first when present).
    for (int i = 0; i < m_markers.size(); ++i) {
        if (m_markers.at(i).kind == MK_MOUSE_MOVE) {
            m_selectedIdx = i;
            break;
        }
    }
}

void KeymapEditor::rebindMarkerKey(int markerIdx, const QString &keyName)
{
    if (markerIdx < 0 || markerIdx >= m_markers.size()) {
        return;
    }
    const Marker &m = m_markers.at(markerIdx);
    if (m.kind == MK_MOUSE_MOVE || m.nodeIndex < 0) {
        return;
    }
    QJsonObject root = m_doc.object();
    QJsonArray nodes = root.value("keyMapNodes").toArray();
    if (m.nodeIndex < 0 || m.nodeIndex >= nodes.size()) {
        return;
    }
    QJsonObject node = nodes.at(m.nodeIndex).toObject();
    node.insert("key", keyName);
    nodes.replace(m.nodeIndex, node);
    root.insert("keyMapNodes", nodes);
    m_doc.setObject(root);

    rebuildMarkers();
    m_selectedIdx = markerIdx;
}

void KeymapEditor::removeMarkerNode(int markerIdx)
{
    if (markerIdx < 0 || markerIdx >= m_markers.size()) {
        return;
    }
    const Marker m = m_markers.at(markerIdx);
    // The mouse-move "AIM" marker has no removable array entry; ignore delete.
    if (m.kind == MK_MOUSE_MOVE || m.nodeIndex < 0) {
        return;
    }

    QJsonObject root = m_doc.object();
    QJsonArray nodes = root.value("keyMapNodes").toArray();
    if (m.nodeIndex < 0 || m.nodeIndex >= nodes.size()) {
        return;
    }
    nodes.removeAt(m.nodeIndex);
    root.insert("keyMapNodes", nodes);
    m_doc.setObject(root);

    rebuildMarkers();
}

QString KeymapEditor::saveToFile()
{
    if (!m_loaded || m_filePath.isEmpty()) {
        return QString();
    }

    // Commit any in-flight drag before serializing.
    if (m_dragIdx >= 0) {
        writeNodePos(m_dragIdx, m_markers.at(m_dragIdx).norm);
        m_dragIdx = -1;
    }

    const QByteArray out = m_doc.toJson(QJsonDocument::Indented);

    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_errorHint = tr("Cannot write keymap: %1").arg(m_filePath);
        update();
        return QString();
    }
    file.write(out);
    file.close();

    const QString text = QString::fromUtf8(out);
    emit saved(text);
    return text;
}

QString KeymapEditor::keyEventToName(int qtKey)
{
    // valueToKey returns names like "Key_W", "Key_Space" — exactly what the
    // KeyMap loader expects via QMetaEnum::fromType<Qt::Key>().
    const char *name = metaKey().valueToKey(qtKey);
    if (!name) {
        return QString();
    }
    const QString s = QString::fromLatin1(name);
    if (!s.startsWith("Key_")) {
        return QString();
    }
    return s;
}

QString KeymapEditor::mouseButtonToName(int qtButton)
{
    // valueToKey on Qt::MouseButtons gives "LeftButton"/"RightButton"/"MiddleButton".
    const char *name = metaMouseButtons().valueToKey(qtButton);
    if (!name) {
        return QString();
    }
    const QString s = QString::fromLatin1(name);
    if (s == "LeftButton" || s == "RightButton" || s == "MiddleButton") {
        return s;
    }
    return QString();
}

// ===========================================================================
// Separate tool window (palette + properties panel)
// ===========================================================================

void KeymapEditor::buildToolWindow()
{
    // A top-level Qt::Tool window so it floats next to the mirror window and
    // never overlaps the video. No QObject parent -> we delete it ourselves.
    m_toolWindow = new QWidget(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
    m_toolWindow->setWindowTitle(tr("Keymap Editor"));
    m_toolWindow->setAttribute(Qt::WA_StyledBackground, true);
    m_toolWindow->setStyleSheet(QStringLiteral(R"(
        QWidget#kmToolRoot { background-color: rgb(24, 24, 28); }
        QLabel { color: #E8E8E8; }
        QLabel#propTitle { color: #FFD700; font-weight: bold; }
        QGroupBox {
            color: #FFD700;
            border: 1px solid rgba(255,255,255,40);
            border-radius: 5px;
            margin-top: 8px;
            font-weight: bold;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; }
        QPushButton {
            background-color: rgba(53, 74, 183, 230);
            color: white;
            border: 1px solid rgba(255,255,255,60);
            border-radius: 4px;
            padding: 4px 8px;
            font-weight: bold;
        }
        QPushButton:hover { background-color: rgba(80, 104, 220, 240); }
        QDoubleSpinBox {
            background-color: rgb(40, 40, 46);
            color: #E8E8E8;
            border: 1px solid rgba(255,255,255,50);
            border-radius: 3px;
            padding: 2px;
        }
        QComboBox {
            background-color: rgb(40, 40, 46);
            color: #E8E8E8;
            border: 1px solid rgba(255,255,255,50);
            border-radius: 3px;
            padding: 2px 4px;
        }
        QComboBox QAbstractItemView {
            background-color: rgb(40, 40, 46);
            color: #E8E8E8;
            selection-background-color: rgba(53, 74, 183, 230);
        }
    )"));
    m_toolWindow->setObjectName(QStringLiteral("kmToolRoot"));

    auto *root = new QVBoxLayout(m_toolWindow);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // --- keymap file chooser + "New" ---------------------------------------
    {
        auto *kmGroup = new QGroupBox(tr("KEYMAP"), m_toolWindow);
        auto *kmLay = new QVBoxLayout(kmGroup);
        kmLay->setContentsMargins(8, 6, 8, 6);
        kmLay->setSpacing(4);

        auto *kmRow = new QHBoxLayout();
        m_keymapChooser = new QComboBox(kmGroup);
        m_keymapChooser->setToolTip(tr("Switch the keymap being edited (applies to the live session)."));
        kmRow->addWidget(m_keymapChooser, 1);
        m_newKeymapBtn = new QPushButton(tr("New"), kmGroup);
        m_newKeymapBtn->setToolTip(tr("Create a new, empty keymap and edit it."));
        kmRow->addWidget(m_newKeymapBtn);
        kmLay->addLayout(kmRow);

        root->addWidget(kmGroup);

        connect(m_keymapChooser,
                static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, &KeymapEditor::onChooserChanged);
        connect(m_newKeymapBtn, &QPushButton::clicked, this, &KeymapEditor::onNewKeymapClicked);
    }

    // --- palette (drag sources) --------------------------------------------
    m_palette = new KeymapPalette(m_toolWindow);
    root->addWidget(m_palette);

    // --- properties panel ---------------------------------------------------
    auto *propBox = new QGroupBox(tr("PROPERTIES"), m_toolWindow);
    auto *propLay = new QVBoxLayout(propBox);
    propLay->setContentsMargins(8, 8, 8, 8);
    propLay->setSpacing(6);

    m_propTitle = new QLabel(tr("(nothing selected)"), propBox);
    m_propTitle->setObjectName(QStringLiteral("propTitle"));
    m_propTitle->setWordWrap(true);
    propLay->addWidget(m_propTitle);

    // A stack: page 0 = "select a marker" hint; page 1 = editable fields.
    m_propStack = new QStackedWidget(propBox);
    propLay->addWidget(m_propStack);

    // page 0 — empty hint
    auto *emptyPage = new QWidget(m_propStack);
    auto *emptyLay = new QVBoxLayout(emptyPage);
    emptyLay->setContentsMargins(0, 0, 0, 0);
    auto *emptyHint = new QLabel(
        tr("Click a marker on the video to edit it.\n\n"
           "Drag a button above onto the video to add a new node."),
        emptyPage);
    emptyHint->setWordWrap(true);
    emptyHint->setStyleSheet(QStringLiteral("color: rgba(232,232,232,160);"));
    emptyLay->addWidget(emptyHint);
    emptyLay->addStretch(1);
    m_propStack->addWidget(emptyPage);

    // page 1 — editable fields
    auto *fieldsPage = new QWidget(m_propStack);
    auto *fieldsLay = new QVBoxLayout(fieldsPage);
    fieldsLay->setContentsMargins(0, 0, 0, 0);
    fieldsLay->setSpacing(6);

    // Key + Rebind row
    {
        auto *keyRow = new QHBoxLayout();
        keyRow->addWidget(new QLabel(tr("Key:"), fieldsPage));
        m_keyValueLabel = new QLabel(QString(), fieldsPage);
        m_keyValueLabel->setStyleSheet(QStringLiteral("color: #FFD700; font-weight: bold;"));
        keyRow->addWidget(m_keyValueLabel, 1);
        m_rebindBtn = new QPushButton(tr("Rebind"), fieldsPage);
        keyRow->addWidget(m_rebindBtn);
        fieldsLay->addLayout(keyRow);
        connect(m_rebindBtn, &QPushButton::clicked, this, &KeymapEditor::onRebindClicked);
    }

    // Size group (marker size for most types, wheel radius for steer wheel)
    {
        m_sizeGroup = new QGroupBox(tr("Size"), fieldsPage);
        auto *sLay = new QVBoxLayout(m_sizeGroup);
        sLay->setContentsMargins(8, 6, 8, 6);
        sLay->setSpacing(4);

        m_sizeLabel = new QLabel(tr("Marker size (relative)"), m_sizeGroup);
        sLay->addWidget(m_sizeLabel);

        // Non-wheel: global marker display size as a FRACTION of the video
        // rect, so markers keep the same relative size at any window size. The
        // integer slider maps linearly onto the fraction (value * 0.001).
        m_markerSizeSlider = new QSlider(Qt::Horizontal, m_sizeGroup);
        m_markerSizeSlider->setRange(kMinMarkerSlider, kMaxMarkerSlider);
        m_markerSizeSlider->setValue(static_cast<int>(m_markerFrac / kMarkerSliderToFrac + 0.5));
        m_markerSizeSlider->setToolTip(
            tr("Marker size relative to the video (scales with the window)"));
        sLay->addWidget(m_markerSizeSlider);
        connect(m_markerSizeSlider, &QSlider::valueChanged, this, &KeymapEditor::onSizeChanged);

        // Wheel: the real drag radius (offset 0.0..0.5).
        m_wheelOffsetSpin = new QDoubleSpinBox(m_sizeGroup);
        m_wheelOffsetSpin->setRange(kMinWheelOffset, kMaxWheelOffset);
        m_wheelOffsetSpin->setSingleStep(0.005);
        m_wheelOffsetSpin->setDecimals(4);
        sLay->addWidget(m_wheelOffsetSpin);
        connect(m_wheelOffsetSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &KeymapEditor::onWheelOffsetChanged);

        fieldsLay->addWidget(m_sizeGroup);
    }

    // Sensitivity group (only for KMT_MOUSE_MOVE)
    {
        m_sensGroup = new QGroupBox(tr("Sensitivity"), fieldsPage);
        auto *senLay = new QVBoxLayout(m_sensGroup);
        senLay->setContentsMargins(8, 6, 8, 6);
        senLay->setSpacing(4);

        senLay->addWidget(new QLabel(tr("Mouse speed (X = Y)"), m_sensGroup));

        m_sensSpin = new QDoubleSpinBox(m_sensGroup);
        m_sensSpin->setRange(kMinSensitivity, kMaxSensitivity);
        m_sensSpin->setSingleStep(0.05);
        m_sensSpin->setDecimals(3);
        senLay->addWidget(m_sensSpin);
        connect(m_sensSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &KeymapEditor::onSensitivityChanged);

        // Slider mirrors the spin (x100 so the int slider has 0.01 resolution).
        m_sensSlider = new QSlider(Qt::Horizontal, m_sensGroup);
        m_sensSlider->setRange(static_cast<int>(kMinSensitivity * 100),
                               static_cast<int>(kMaxSensitivity * 100));
        senLay->addWidget(m_sensSlider);
        connect(m_sensSlider, &QSlider::valueChanged, this, [this](int v) {
            if (m_updatingProps) {
                return;
            }
            onSensitivityChanged(static_cast<double>(v) / 100.0);
        });

        fieldsLay->addWidget(m_sensGroup);
    }

    fieldsLay->addStretch(1);
    m_propStack->addWidget(fieldsPage);

    root->addWidget(propBox, 1);

    m_propStack->setCurrentIndex(0);
    m_toolWindow->resize(230, 580);
}

void KeymapEditor::showToolWindow(QWidget *nextTo)
{
    if (!m_toolWindow) {
        return;
    }
    if (!m_toolWindowPlaced && nextTo) {
        // Park it just to the right of the mirror window, clamped to the screen
        // the mirror is on. Only done once; afterwards we keep the user's
        // last position (the window remembers it while it merely hides).
        const QRect mirror = nextTo->frameGeometry();
        QPoint pos(mirror.right() + 8, mirror.top());
        QScreen *scr = nextTo->screen();
        if (scr) {
            const QRect avail = scr->availableGeometry();
            // If it would spill off the right edge, dock it to the left instead.
            if (pos.x() + m_toolWindow->width() > avail.right()) {
                pos.setX(mirror.left() - m_toolWindow->width() - 8);
            }
            if (pos.x() < avail.left()) {
                pos.setX(avail.left());
            }
            if (pos.y() + m_toolWindow->height() > avail.bottom()) {
                pos.setY(qMax(avail.top(), avail.bottom() - m_toolWindow->height()));
            }
            if (pos.y() < avail.top()) {
                pos.setY(avail.top());
            }
        }
        m_toolWindow->move(pos);
        m_toolWindowPlaced = true;
    }
    m_toolWindow->show();
    m_toolWindow->raise();
}

void KeymapEditor::hideToolWindow()
{
    if (m_toolWindow) {
        m_toolWindow->hide();
    }
}

void KeymapEditor::refreshProperties()
{
    if (!m_propStack) {
        return;
    }

    const bool haveSel = (m_selectedIdx >= 0 && m_selectedIdx < m_markers.size());
    if (!haveSel) {
        m_propTitle->setText(tr("(nothing selected)"));
        m_propStack->setCurrentIndex(0);
        return;
    }

    const Marker &m = m_markers.at(m_selectedIdx);

    m_updatingProps = true;

    m_propTitle->setText(QStringLiteral("%1  —  %2").arg(m.typeLabel, m.label));
    m_keyValueLabel->setText(m.label);

    const bool isWheel = (m.kmtType == "KMT_STEER_WHEEL");
    const bool isMove = (m.kind == MK_MOUSE_MOVE);

    // Rebind only applies to single-key nodes (not wheel, not aim region).
    const bool rebindable = !isWheel && !isMove && m.nodeIndex >= 0;
    m_rebindBtn->setEnabled(rebindable);
    m_rebindBtn->setVisible(!isMove); // aim region has no key at all

    // Size group: wheel -> offset spin; everything else -> global marker slider.
    if (isWheel) {
        m_sizeLabel->setText(tr("Wheel radius (offset)"));
        m_wheelOffsetSpin->setVisible(true);
        m_markerSizeSlider->setVisible(false);
        m_wheelOffsetSpin->setValue(m.wheelOffset);
    } else {
        m_sizeLabel->setText(tr("Marker size (relative)"));
        m_wheelOffsetSpin->setVisible(false);
        m_markerSizeSlider->setVisible(true);
        m_markerSizeSlider->setValue(
            static_cast<int>(m_markerFrac / kMarkerSliderToFrac + 0.5));
    }
    m_sizeGroup->setVisible(true);

    // Sensitivity group: only the mouse-move aim region.
    if (isMove) {
        m_sensGroup->setVisible(true);
        const double s = currentMouseMoveSensitivity();
        m_sensSpin->setValue(s);
        m_sensSlider->setValue(static_cast<int>(s * 100.0 + 0.5));
        m_keyValueLabel->setText(tr("(aim region — no key)"));
    } else {
        m_sensGroup->setVisible(false);
    }

    m_updatingProps = false;

    m_propStack->setCurrentIndex(1);
}

void KeymapEditor::onRebindClicked()
{
    if (m_selectedIdx < 0 || m_selectedIdx >= m_markers.size()) {
        return;
    }
    const Marker &m = m_markers.at(m_selectedIdx);
    if (m.kind == MK_MOUSE_MOVE || m.kmtType == "KMT_STEER_WHEEL" || m.nodeIndex < 0) {
        return;
    }
    cancelAwait();
    m_await = AW_REBIND;
    m_rebindMarker = m_selectedIdx;
    // Capture happens on the video overlay, so give it focus and activate the
    // mirror window.
    if (window()) {
        window()->activateWindow();
    }
    setFocus();
    emitPrompt();
    update();
}

void KeymapEditor::onSizeChanged(int value)
{
    if (m_updatingProps) {
        return;
    }
    // Map the slider's integer position to a fraction of the video rect and
    // repaint. markerRadius() turns this into pixels at the current size.
    const int v = qBound(kMinMarkerSlider, value, kMaxMarkerSlider);
    m_markerFrac = v * kMarkerSliderToFrac;
    update();
}

void KeymapEditor::onWheelOffsetChanged(double value)
{
    if (m_updatingProps) {
        return;
    }
    if (m_selectedIdx < 0 || m_selectedIdx >= m_markers.size()) {
        return;
    }
    setWheelOffset(m_selectedIdx, qBound(kMinWheelOffset, value, kMaxWheelOffset));
    update();
}

void KeymapEditor::onSensitivityChanged(double value)
{
    if (m_updatingProps) {
        return;
    }
    const double v = qBound(kMinSensitivity, value, kMaxSensitivity);
    setMouseMoveSensitivity(v);
    // Keep the twin controls in sync without re-entering this slot.
    m_updatingProps = true;
    if (m_sensSpin) {
        m_sensSpin->setValue(v);
    }
    if (m_sensSlider) {
        m_sensSlider->setValue(static_cast<int>(v * 100.0 + 0.5));
    }
    m_updatingProps = false;
}

void KeymapEditor::setWheelOffset(int markerIdx, double offset)
{
    if (markerIdx < 0 || markerIdx >= m_markers.size()) {
        return;
    }
    Marker &mk = m_markers[markerIdx];
    if (mk.kmtType != "KMT_STEER_WHEEL" || mk.nodeIndex < 0) {
        return;
    }
    QJsonObject root = m_doc.object();
    QJsonArray nodes = root.value("keyMapNodes").toArray();
    if (mk.nodeIndex < 0 || mk.nodeIndex >= nodes.size()) {
        return;
    }
    QJsonObject node = nodes.at(mk.nodeIndex).toObject();
    // Set all four directional offsets to the single radius the user picked.
    node.insert("leftOffset", offset);
    node.insert("rightOffset", offset);
    node.insert("upOffset", offset);
    node.insert("downOffset", offset);
    nodes.replace(mk.nodeIndex, node);
    root.insert("keyMapNodes", nodes);
    m_doc.setObject(root);

    mk.wheelOffset = offset; // reflect in the live marker for repaint
}

double KeymapEditor::currentMouseMoveSensitivity() const
{
    const QJsonObject root = m_doc.object();
    const QJsonObject mm = root.value("mouseMoveMap").toObject();
    if (mm.contains("speedRatioX") || mm.contains("speedRatioY")) {
        const double x = mm.value("speedRatioX").toDouble(1.0);
        const double y = mm.value("speedRatioY").toDouble(1.0);
        return (x + y) * 0.5;
    }
    if (mm.contains("speedRatio")) {
        return mm.value("speedRatio").toDouble(1.0);
    }
    return 1.0;
}

void KeymapEditor::setMouseMoveSensitivity(double ratio)
{
    QJsonObject root = m_doc.object();
    QJsonObject mm = root.value("mouseMoveMap").toObject();
    if (mm.isEmpty()) {
        return; // no aim region defined yet; nothing to set
    }
    // Write the X/Y pair (matches the sample keymap); if the map used the
    // single "speedRatio" form, keep that one in sync too.
    mm.insert("speedRatioX", ratio);
    mm.insert("speedRatioY", ratio);
    if (mm.contains("speedRatio")) {
        mm.insert("speedRatio", ratio);
    }
    root.insert("mouseMoveMap", mm);
    m_doc.setObject(root);
}

void KeymapEditor::refreshKeymapList()
{
    if (!m_keymapChooser) {
        return;
    }
    const QString dir = QFileInfo(m_filePath).absolutePath();
    QSignalBlocker block(m_keymapChooser);
    m_keymapChooser->clear();
    if (dir.isEmpty()) {
        return;
    }
    const QStringList files = QDir(dir).entryList(QStringList() << "*.json", QDir::Files, QDir::Name);
    const QString current = QFileInfo(m_filePath).fileName();
    int sel = -1;
    for (int i = 0; i < files.size(); ++i) {
        m_keymapChooser->addItem(files.at(i));
        if (files.at(i) == current) {
            sel = i;
        }
    }
    if (sel >= 0) {
        m_keymapChooser->setCurrentIndex(sel);
    }
}

void KeymapEditor::onChooserChanged(int index)
{
    if (index < 0 || !m_keymapChooser) {
        return;
    }
    const QString dir = QFileInfo(m_filePath).absolutePath();
    const QString name = m_keymapChooser->itemText(index);
    if (dir.isEmpty() || name.isEmpty()) {
        return;
    }
    const QString path = dir + "/" + name;
    if (path == m_filePath) {
        return; // already editing this file
    }
    if (loadFromFile(path)) {
        emit keymapSwitched(path, QString::fromUtf8(m_doc.toJson(QJsonDocument::Indented)));
    }
}

void KeymapEditor::onNewKeymapClicked()
{
    const QString dir = QFileInfo(m_filePath).absolutePath();
    if (dir.isEmpty()) {
        QMessageBox::warning(m_toolWindow, tr("New Keymap"),
                             tr("No keymap directory is known yet."), QMessageBox::Ok);
        return;
    }

    bool ok = false;
    QString name = QInputDialog::getText(m_toolWindow, tr("New Keymap"),
                                         tr("Name for the new keymap:"),
                                         QLineEdit::Normal, QStringLiteral("new_keymap"), &ok);
    if (!ok) {
        return;
    }
    name = name.trimmed();
    if (name.isEmpty()) {
        return;
    }
    if (!name.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        name += QStringLiteral(".json");
    }
    const QString path = dir + "/" + name;

    if (QFile::exists(path)) {
        const auto btn = QMessageBox::question(m_toolWindow, tr("New Keymap"),
            tr("\"%1\" already exists. Open it for editing instead?").arg(name),
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::Yes && loadFromFile(path)) {
            emit keymapSwitched(path, QString::fromUtf8(m_doc.toJson(QJsonDocument::Indented)));
        }
        return;
    }

    // Minimal valid keymap: just a switch key + an empty node list. The core
    // loader only hard-requires "switchKey"; nodes are added by dragging onto
    // the video.
    QJsonObject newRoot;
    newRoot.insert(QStringLiteral("switchKey"), QStringLiteral("Key_QuoteLeft"));
    newRoot.insert(QStringLiteral("keyMapNodes"), QJsonArray());
    const QByteArray out = QJsonDocument(newRoot).toJson(QJsonDocument::Indented);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(m_toolWindow, tr("New Keymap"),
                             tr("Could not create:\n%1").arg(path), QMessageBox::Ok);
        return;
    }
    f.write(out);
    f.close();

    if (loadFromFile(path)) {
        emit keymapSwitched(path, QString::fromUtf8(m_doc.toJson(QJsonDocument::Indented)));
    }
}
