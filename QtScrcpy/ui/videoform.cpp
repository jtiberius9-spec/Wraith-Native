// #include <QDesktopWidget>
#include <QCoreApplication>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QShortcut>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QWindow>
#include <QtWidgets/QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPolygonF>
#include <QPixmap>
#include <QImage>
#include <QListWidget>
#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QElapsedTimer>
#include <QVector>
#include <QGuiApplication>
#include <QTextStream>
#include <QCoreApplication>
#include <QtMath>
#include <algorithm>

#if defined(Q_OS_WIN32)
#include <Windows.h>
#endif

#include "config.h"
#include "iconhelper.h"
#include "keymapeditor.h"
#include "qyuvopenglwidget.h"
#include "toolform.h"
#include "mousetap/mousetap.h"
#include "ui_videoform.h"
#include "videoform.h"

namespace
{
    // Default keymap-edit-mode banner hint. Shown whenever the editor isn't
    // prompting for a specific key capture.
    QString kEditBannerHint()
    {
        return QCoreApplication::translate(
            "VideoForm",
            "EDIT MODE  \xE2\x80\x94  F10 to exit,  Ctrl+S to save  \xE2\x80\x94  "
            "drag a palette button onto the video to add,  drag markers to move,  "
            "double-click a marker to rebind,  Delete to remove");
    }

    // Order normalized path points along the spray by nearest-neighbour, starting
    // from the bottom-most (first shot). Robust to S-curves / horizontal runs that a
    // plain height-sort would scramble. Used by both auto-detect and the preview so
    // manually-added dots slot into the path by geometry, not by click order.
    QVector<QPointF> orderPathNN(const QVector<QPointF> &in)
    {
        const int n = in.size();
        if (n < 3) return in;
        QVector<bool> used(n, false);
        QVector<QPointF> out;
        out.reserve(n);
        int cur = 0;
        for (int i = 1; i < n; ++i) if (in[i].y() > in[cur].y()) cur = i;
        used[cur] = true; out.push_back(in[cur]);
        for (int step = 1; step < n; ++step) {
            int best = -1; double bd = 1e30;
            for (int j = 0; j < n; ++j) {
                if (used[j]) continue;
                const double dx = in[j].x() - in[cur].x(), dy = in[j].y() - in[cur].y();
                const double d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = j; }
            }
            if (best < 0) break;
            cur = best; used[best] = true; out.push_back(in[cur]);
        }
        return out;
    }

    // Click-to-trace widget: shows an image; left-click adds a point, right-click
    // undoes the last. autoOrder=true re-orders the dots along the path for both
    // display and readout, so a pre-filled auto-detect result can be hand-corrected
    // (click a missed dot anywhere; it slots into place). No Q_OBJECT (no signals).
    class RecoilTracer : public QWidget
    {
    public:
        explicit RecoilTracer(const QPixmap &pix, QWidget *parent = nullptr, bool autoOrder = false)
            : QWidget(parent), m_pix(pix), m_autoOrder(autoOrder)
        {
            setFixedSize(m_pix.size());
            setCursor(Qt::CrossCursor);
        }
        QVector<QPointF> pts;   // normalized 0..1
        QVector<QPointF> ordered() const { return m_autoOrder ? orderPathNN(pts) : pts; }
    protected:
        void paintEvent(QPaintEvent *) override
        {
            QPainter p(this);
            p.drawPixmap(0, 0, m_pix);
            const QVector<QPointF> seq = ordered();
            QPolygonF poly;
            for (const QPointF &n : seq) {
                poly << QPointF(n.x() * width(), n.y() * height());
            }
            p.setPen(QPen(QColor("#39d353"), 2));
            p.drawPolyline(poly);
            p.setBrush(QColor("#39d353"));
            for (const QPointF &pt : poly) {
                p.drawEllipse(pt, 3, 3);
            }
        }
        void mousePressEvent(QMouseEvent *e) override
        {
            if (e->button() == Qt::RightButton) {
                if (!pts.isEmpty()) pts.removeLast();
            } else {
                pts << QPointF(double(e->pos().x()) / width(), double(e->pos().y()) / height());
            }
            update();
        }
    private:
        QPixmap m_pix;
        bool m_autoOrder = false;
    };

    // Turn a traced polyline into a recoil pattern (inverse of the spray).
    // Shape comes from the image; absolute magnitude is set by Strength.
    QJsonArray patternFromTrace(const QVector<QPointF> &pts)
    {
        QJsonArray pat;
        if (pts.size() < 2) {
            return pat;
        }
        const double totalMs = 1200.0;                 // assume ~1.2s mag dump
        const int segs = pts.size() - 1;
        const double segMs = totalMs / segs;
        QVector<QPointF> vel;                            // comp velocity per segment
        double peak = 1e-6;
        for (int i = 1; i < pts.size(); ++i) {
            // compensation = opposite of the recoil step, per second
            // config convention: horizontal + = pull LEFT, vertical + = pull DOWN
            double vx = (pts[i].x() - pts[i - 1].x()) / (segMs / 1000.0);
            double vy = -(pts[i].y() - pts[i - 1].y()) / (segMs / 1000.0);
            vel << QPointF(vx, vy);
            peak = qMax(peak, qMax(qAbs(vx), qAbs(vy)));
        }
        const double norm = 0.30 / peak;                // scale peak to a sane default
        for (const QPointF &v : vel) {
            QJsonObject o;
            o["ms"] = segMs;
            o["dx"] = qRound(v.x() * norm * 1000) / 1000.0;
            o["dy"] = qRound(v.y() * norm * 1000) / 1000.0;
            pat.append(o);
        }
        return pat;
    }

    // Auto-detect a spray into a recoil PATH via connected-component blob detection.
    // Key idea for HORIZONTAL fidelity: each blob becomes ONE point per shot (a lone
    // dot = its centroid; a merged clump = round(area/median) sub-points). One point
    // per shot == equal time per segment (constant fire rate), which is exactly the
    // model patternFromTrace assumes. Points are then ordered by nearest-neighbour
    // from the first shot (bottom-most), so an S-curve / zig-zag keeps its true
    // left-right sequence instead of being re-sorted by height. No x-smoothing, so
    // the full horizontal amplitude survives. Returns normalized (0..1) points.
    QVector<QPointF> autoDetectSpray(const QImage &imgIn)
    {
        QVector<QPointF> ordered;
        QImage img = imgIn.convertToFormat(QImage::Format_ARGB32);
        // keep good resolution so tightly-grouped holes stay distinct
        if (img.width() > 1400) {
            img = img.scaledToWidth(1400, Qt::SmoothTransformation);
        }
        const int W = img.width(), H = img.height();
        if (W < 8 || H < 8) return ordered;

        // Recoil dots are usually a saturated COLOUR (green/teal) on a neutral
        // background + silhouette. Grayscale throws that away, so a faint teal dot and
        // the grey silhouette look identical in brightness and the dot gets missed.
        // So: if the image is colourful, score each pixel by SATURATION (max-min
        // channel) - colour, not brightness - which separates even the faintest teal
        // dot from grey. Else (dark holes on a bright wall) fall back to luma.
        // Pass 1: how colourful is it, and in what CHROMA DIRECTION do the dots point?
        // Grayscale / saturation / single-channel all failed because the dots are TEAL
        // and the silhouette is BLUE - nearly identical in brightness AND saturation.
        // So learn the dots' actual hue: average the colour-direction (RGB minus its
        // own grey) of the vivid pixels, then score every pixel by how strongly it
        // points that same way. Teal dots score high at any faintness; the blue
        // silhouette points elsewhere and scores low. Falls back to luma if no hue.
        int maxSat = 0;
        for (int y = 0; y < H; ++y) {
            const QRgb *l = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < W; ++x) {
                const QRgb p = l[x];
                const int s = qMax(qMax(qRed(p), qGreen(p)), qBlue(p))
                            - qMin(qMin(qRed(p), qGreen(p)), qBlue(p));
                if (s > maxSat) maxSat = s;
            }
        }
        bool colored = maxSat >= 45;
        double dirR = 0, dirG = 0, dirB = 0;
        if (colored) {
            const int vivid = qMax(25, maxSat / 2);
            for (int y = 0; y < H; ++y) {
                const QRgb *l = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < W; ++x) {
                    const QRgb p = l[x];
                    const int r = qRed(p), g = qGreen(p), b = qBlue(p);
                    if (qMax(qMax(r, g), b) - qMin(qMin(r, g), b) < vivid) continue;
                    const double m = (r + g + b) / 3.0;
                    dirR += r - m; dirG += g - m; dirB += b - m;
                }
            }
            const double len = qSqrt(dirR * dirR + dirG * dirG + dirB * dirB);
            if (len > 1e-6) { dirR /= len; dirG /= len; dirB /= len; }
            else colored = false;   // no consistent hue -> treat as grayscale
        }

        // Pass 2: per-pixel ink score = projection onto the dots' hue (colour), else luma.
        QVector<uchar> score(W * H);
        double sum = 0.0;
        int mn = 255, mx = 0;
        for (int y = 0; y < H; ++y) {
            const QRgb *l = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            uchar *sc = score.data() + y * W;
            for (int x = 0; x < W; ++x) {
                const QRgb p = l[x];
                int v;
                if (colored) {
                    const int r = qRed(p), g = qGreen(p), b = qBlue(p);
                    const double m = (r + g + b) / 3.0;
                    const double proj = (r - m) * dirR + (g - m) * dirG + (b - m) * dirB;
                    v = proj <= 0 ? 0 : (proj > 255 ? 255 : int(proj));
                } else {
                    v = qGray(p);
                }
                sc[x] = uchar(v);
                sum += v; mn = qMin(mn, v); mx = qMax(mx, v);
            }
        }
        const double mean = sum / (double(W) * H);
        // The projection is ~0 off the dots, so a low floor catches faint end-of-burst
        // dots; the blob-area floor + preview cull the little noise it lets in. Luma
        // path keeps its firmer 0.18 margin.
        const bool inkBright = colored || (mean < 128.0);
        const double margin = colored ? qMax(8.0, (mx - mean) * 0.10)
                                      : qMax(14.0, (inkBright ? (mx - mean) : (mean - mn)) * 0.18);
        auto isInk = [&](uchar v) { return inkBright ? (v > mean + margin) : (v < mean - margin); };

        // --- connected-component labelling (4-conn flood fill, explicit stack) ---
        // label 0 = background/unvisited; ink pixels get a 1-based component id.
        QVector<int> label(W * H, 0);
        QVector<int> stack;
        stack.reserve(2048);
        struct Blob { int area, minX, maxX, minY, maxY; double sx, sy; };
        QVector<Blob> blobs;
        for (int y0 = 0; y0 < H; ++y0) {
            const uchar *row = score.constData() + y0 * W;
            for (int x0 = 0; x0 < W; ++x0) {
                if (label[y0 * W + x0] != 0 || !isInk(row[x0])) continue;
                const int id = blobs.size() + 1;
                Blob b{ 0, x0, x0, y0, y0, 0.0, 0.0 };
                stack.clear();
                stack.push_back(y0 * W + x0);
                label[y0 * W + x0] = id;
                while (!stack.isEmpty()) {
                    const int idx = stack.back(); stack.pop_back();
                    const int cx = idx % W, cy = idx / W;
                    ++b.area; b.sx += cx; b.sy += cy;
                    b.minX = qMin(b.minX, cx); b.maxX = qMax(b.maxX, cx);
                    b.minY = qMin(b.minY, cy); b.maxY = qMax(b.maxY, cy);
                    const int nb[4] = { cx > 0 ? idx - 1 : -1, cx < W - 1 ? idx + 1 : -1,
                                        cy > 0 ? idx - W : -1, cy < H - 1 ? idx + W : -1 };
                    for (int k = 0; k < 4; ++k) {
                        const int n = nb[k];
                        if (n < 0 || label[n] != 0) continue;
                        if (isInk(score[n])) { label[n] = id; stack.push_back(n); }
                    }
                }
                blobs.push_back(b);
            }
        }
        if (blobs.isEmpty()) return ordered;

        // noise floor: drop specks below 0.25x the median blob area (min 4px).
        QVector<int> areas;
        areas.reserve(blobs.size());
        for (const Blob &b : blobs) areas.push_back(b.area);
        std::sort(areas.begin(), areas.end());
        const int medArea = qMax(1, areas[areas.size() / 2]);
        // gentle floor: faint end-of-burst dots render small, so a big bright cluster
        // shouldn't inflate the median past them. Preview lets the user cull any speckle.
        const int minArea = qMax(3, medArea / 6);

        // one point per SHOT: a lone dot -> centroid; a merged clump -> split into
        // round(area/median) sub-points by row so each buried shot keeps its own x.
        QVector<QPointF> pts;   // pixel coords
        for (int li = 0; li < blobs.size(); ++li) {
            const Blob &b = blobs[li];
            if (b.area < minArea) continue;
            const int k = qBound(1, int(qRound(double(b.area) / medArea)), 10);
            if (k == 1) { pts.push_back(QPointF(b.sx / b.area, b.sy / b.area)); continue; }
            const int id = li + 1;
            const int range = b.maxY - b.minY + 1;
            for (int s = 0; s < k; ++s) {
                const int ys = b.minY + s * range / k;
                const int ye = b.minY + (s + 1) * range / k;   // exclusive
                double sx = 0, sy = 0; long cnt = 0;
                for (int y = ys; y < ye && y < H; ++y) {
                    const int *lr = label.constData() + y * W;
                    for (int x = b.minX; x <= b.maxX; ++x)
                        if (lr[x] == id) { sx += x; sy += y; ++cnt; }
                }
                if (cnt > 0) pts.push_back(QPointF(sx / cnt, sy / cnt));
            }
        }
        if (pts.size() < 2) return ordered;
        for (const QPointF &p : pts) ordered.push_back(QPointF(p.x() / W, p.y() / H));

        // diagnostic: record what the detector actually measured, so any remaining
        // miss is fixed from data instead of guessing at the dot colours.
        {
            QFile df(QStringLiteral("C:/Users/Admin/AppData/Local/Programs/Wraith/recoil_detect.log"));
            if (df.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QTextStream ts(&df);
                ts << "W=" << W << " H=" << H << " colored=" << colored << " maxSat=" << maxSat
                   << " dir=(" << dirR << "," << dirG << "," << dirB << ")"
                   << " scoreMean=" << mean << " scoreMax=" << mx << " margin=" << margin
                   << " blobs=" << blobs.size() << " points=" << ordered.size() << "\n";
                df.close();
            }
        }
        return orderPathNN(ordered);   // shot order; patternFromTrace makes the pattern
    }

    // Sustained pull = average of the pattern's last ~40% (the plateau), so long
    // mags keep getting pulled after the initial pattern overlay ends.
    void applyPatternToGun(QJsonObject &gun, const QJsonArray &pat)
    {
        gun["pattern"] = pat;
        if (pat.isEmpty()) return;
        const int from = int(pat.size() * 0.6);
        double sx = 0, sy = 0;
        int n = 0;
        for (int i = from; i < pat.size(); ++i) {
            sx += pat[i].toObject().value("dx").toDouble();
            sy += pat[i].toObject().value("dy").toDouble();
            ++n;
        }
        if (n > 0) {
            gun["horizontal"] = qRound(sx / n * 1000) / 1000.0;
            gun["vertical"] = qRound(sy / n * 1000) / 1000.0;
        }
    }

    // --- record-the-mag helpers (grab the game view, measure drift over time) ---
    // 1D luma projections of a region (downscaled for speed).
    void profilesFromImage(const QImage &src, QVector<float> &vProf, QVector<float> &hProf)
    {
        QImage img = src.convertToFormat(QImage::Format_Grayscale8);
        if (img.width() > 240) img = img.scaledToWidth(240, Qt::SmoothTransformation);
        const int W = img.width(), H = img.height();
        vProf.fill(0.0f, H);
        hProf.fill(0.0f, W);
        for (int y = 0; y < H; ++y) {
            const uchar *l = img.constScanLine(y);
            float rowsum = 0.0f;
            for (int x = 0; x < W; ++x) { rowsum += l[x]; hProf[x] += l[x]; }
            vProf[y] = rowsum / W;
        }
        for (int x = 0; x < W; ++x) hProf[x] /= H;
    }

    // Integer shift of b relative to a (positive = content moved down/right) by
    // minimizing mean-squared error over the overlap.
    int bestShift(const QVector<float> &a, const QVector<float> &b, int maxS)
    {
        const int n = qMin(a.size(), b.size());
        int best = 0;
        double bestErr = 1e30;
        for (int s = -maxS; s <= maxS; ++s) {
            double err = 0.0;
            int cnt = 0;
            for (int i = 0; i < n; ++i) {
                const int j = i + s;
                if (j < 0 || j >= n) continue;
                const double d = a[i] - b[j];
                err += d * d;
                ++cnt;
            }
            if (cnt > 10) {
                err /= cnt;
                if (err < bestErr) { bestErr = err; best = s; }
            }
        }
        return best;
    }

    // Build a recoil pattern from a recorded cumulative CONTENT trajectory
    // (normalized frame units; +x right, +y down). Config: dy=+content down
    // (pull down), dx=-content right (pull right = his horizontal negative).
    QJsonArray patternFromTrajectory(const QVector<double> &t, const QVector<double> &cx,
                                     const QVector<double> &cy)
    {
        QJsonArray pat;
        const int n = t.size();
        if (n < 3) return pat;
        const double dur = t[n - 1] - t[0];
        if (dur <= 0.0) return pat;
        const int K = qBound(4, n / 3, 12);
        const double segMs = dur * 1000.0 / K;
        auto at = [&](double tt, const QVector<double> &c) {
            if (tt <= t[0]) return c.first();
            if (tt >= t[n - 1]) return c.last();
            int i = 1;
            while (i < n && t[i] < tt) ++i;
            const double f = (tt - t[i - 1]) / (t[i] - t[i - 1]);
            return c[i - 1] + f * (c[i] - c[i - 1]);
        };
        QVector<QPointF> vel;
        for (int k = 0; k < K; ++k) {
            const double t0 = t[0] + dur * k / K, t1 = t[0] + dur * (k + 1) / K;
            const double segSec = qMax(1e-3, t1 - t0);
            const double dcx = (at(t1, cx) - at(t0, cx)) / segSec;
            const double dcy = (at(t1, cy) - at(t0, cy)) / segSec;
            vel << QPointF(-dcx, dcy);  // content right->pull right(-); content down->pull down(+)
        }
        // The final segment is almost always the gun settling back down as firing
        // stops (a big upward jerk). Drop it so it doesn't pollute the pattern or,
        // worse, dominate the normalization and weaken the real recoil.
        if (vel.size() > 4) vel.removeLast();
        double peak = 1e-6;
        for (const QPointF &v : vel) peak = qMax(peak, qMax(qAbs(v.x()), qAbs(v.y())));
        const double norm = 0.30 / peak;
        for (const QPointF &v : vel) {
            QJsonObject o;
            o["ms"] = segMs;
            o["dx"] = qRound(v.x() * norm * 1000) / 1000.0;
            o["dy"] = qRound(v.y() * norm * 1000) / 1000.0;
            pat.append(o);
        }
        return pat;
    }
}

VideoForm::VideoForm(bool framelessWindow, bool skin, bool showToolbar, QWidget *parent) : QWidget(parent), ui(new Ui::videoForm), m_skin(skin)
{
    ui->setupUi(this);
    initUI();
    installShortcut();
    updateShowSize(size());
    bool vertical = size().height() > size().width();
    this->show_toolbar = showToolbar;
    if (m_skin) {
        updateStyleSheet(vertical);
    }
    if (framelessWindow) {
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    }
}

VideoForm::~VideoForm()
{
    delete ui;
}

void VideoForm::initUI()
{
    if (m_skin) {
        QPixmap phone;
        if (phone.load(":/res/phone.png")) {
            m_widthHeightRatio = 1.0f * phone.width() / phone.height();
        }

#ifndef Q_OS_OSX
        // mac下去掉标题栏影响showfullscreen
        // 去掉标题栏
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        // 根据图片构造异形窗口
        setAttribute(Qt::WA_TranslucentBackground);
#endif
    }

    m_videoWidget = new QYUVOpenGLWidget();
    m_videoWidget->hide();
    ui->keepRatioWidget->setWidget(m_videoWidget);
    ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

    m_fpsLabel = new QLabel(m_videoWidget);
    QFont ft;
    ft.setPointSize(15);
    ft.setWeight(QFont::Light);
    ft.setBold(true);
    m_fpsLabel->setFont(ft);
    m_fpsLabel->move(5, 15);
    m_fpsLabel->setMinimumWidth(100);
    m_fpsLabel->setStyleSheet(R"(QLabel {color: #00FF00;})");

    setMouseTracking(true);
    m_videoWidget->setMouseTracking(true);
    ui->keepRatioWidget->setMouseTracking(true);
}

QRect VideoForm::getGrabCursorRect()
{
    QRect rc;
#if defined(Q_OS_WIN32)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(m_videoWidget->pos()), m_videoWidget->size());
    // high dpi support
    rc.setTopLeft(rc.topLeft() * m_videoWidget->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * m_videoWidget->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_OSX)
    rc = m_videoWidget->geometry();
    rc.setTopLeft(ui->keepRatioWidget->mapToGlobal(rc.topLeft()));
    rc.setBottomRight(ui->keepRatioWidget->mapToGlobal(rc.bottomRight()));

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_LINUX)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(m_videoWidget->pos()), m_videoWidget->size());
    // high dpi support -- taken from the WIN32 section and untested
    rc.setTopLeft(rc.topLeft() * m_videoWidget->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * m_videoWidget->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#endif
    return rc;
}

const QSize &VideoForm::frameSize()
{
    return m_frameSize;
}

void VideoForm::resizeSquare()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    resize(screenRect.height(), screenRect.height());
}

void VideoForm::removeBlackRect()
{
    resize(ui->keepRatioWidget->goodSize());
}

void VideoForm::showFPS(bool show)
{
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setVisible(show);
}

void VideoForm::updateRender(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)
{
    if (m_videoWidget->isHidden()) {
        if (m_loadingWidget) {
            m_loadingWidget->close();
        }
        m_videoWidget->show();
    }

    updateShowSize(QSize(width, height));
    m_videoWidget->setFrameSize(QSize(width, height));
    m_videoWidget->updateTextures(dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void VideoForm::setSerial(const QString &serial)
{
    m_serial = serial;
}

void VideoForm::showToolForm(bool show)
{
    if (!m_toolForm) {
        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);
        m_toolForm->setSerial(m_serial);
    }
    m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);
    m_toolForm->setVisible(show);
}

void VideoForm::moveCenter()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    // 窗口居中
    move(screenRect.center() - QRect(0, 0, size().width(), size().height()).center());
}

void VideoForm::installShortcut()
{
    QShortcut *shortcut = nullptr;

    // switchFullScreen
    shortcut = new QShortcut(QKeySequence("Ctrl+f"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        switchFullScreen();
    });

    // resizeSquare
    shortcut = new QShortcut(QKeySequence("Ctrl+g"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { resizeSquare(); });

    // removeBlackRect
    shortcut = new QShortcut(QKeySequence("Ctrl+w"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { removeBlackRect(); });

    // postGoHome
    shortcut = new QShortcut(QKeySequence("Ctrl+h"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoHome();
    });

    // postGoBack
    shortcut = new QShortcut(QKeySequence("Ctrl+b"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoBack();
    });

    // postAppSwitch / save-keymap (when in edit mode)
    shortcut = new QShortcut(QKeySequence("Ctrl+s"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        // While editing the keymap, Ctrl+S saves the keymap instead of
        // sending the app-switch key to the device.
        if (m_editMode) {
            saveKeymapEdits();
            return;
        }
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postAppSwitch();
    });

    // toggle keymap edit mode
    shortcut = new QShortcut(QKeySequence("F10"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { toggleKeymapEditMode(); });

    // recoil editor (F8). While a recording is running, F8 STOPS it instead.
    shortcut = new QShortcut(QKeySequence("F8"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        if (m_recording) {
            stopRecoilRecording();
        } else {
            showRecoilEditor();
        }
    });

    // PC MODE (F7): hand the phone a REAL HID keyboard + relative mouse over UHID,
    // for Wine/GameNative containers that expect a genuine mouse (touch injection
    // can only do absolute coords, which a PC game reads as a touchscreen).
    // Mutually exclusive with game mode BY CONSTRUCTION: Controller swaps the whole
    // input converter, so a keymap's switchKey cannot fire while PC mode is on, and
    // leaving PC mode restores whichever keymap was loaded. That is what makes a
    // frozen container recoverable -- drop out of PC mode and use touch as usual.
    shortcut = new QShortcut(QKeySequence("F7"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        if (m_editMode) {
            return; // don't fight the keymap editor
        }
        auto dev = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!dev) {
            return;
        }
        const bool on = !dev->isPCMode();
        dev->setPCMode(on);

        // PC mode owns the keyboard. QtScrcpy's Ctrl+<key> bindings are QShortcuts,
        // and Qt consumes a shortcut chord BEFORE keyPressEvent runs -- so without
        // this the guest never receives any Ctrl combo at all: Ctrl+W ran
        // removeBlackRect() and Ctrl+S sent HOME instead of crouching/sprinting.
        // Suspend every shortcut while PC mode is on, except two that stay live:
        // F7 (always a way back out) and F12 (recording -- the whole point is to
        // capture gameplay, which only happens in PC mode). Neither collides with a
        // Ctrl/Shift chord, so keeping them costs the guest nothing.
        const QKeySequence pcModeKey(QKeySequence("F7"));
        const QKeySequence recordKey(QKeySequence("F12"));
        for (QShortcut *sc : findChildren<QShortcut *>()) {
            if (sc->key() != pcModeKey && sc->key() != recordKey) {
                sc->setEnabled(!on);
            }
        }

        // VideoForm owns the cursor + notice: InputConvertPC's signals are only
        // connected after its constructor runs, so it cannot announce itself.
        grabCursor(on);
        if (on) {
            m_videoWidget->setCursor(Qt::BlankCursor);
            recoilHint(QString("PC MODE -- real keyboard + mouse to the device (F7 to exit)"));
        } else {
            m_videoWidget->unsetCursor();
            recoilHint(QString("PC MODE off -- keymap and touch restored"));
        }
    });

    // toggle screen recording (Wraith F12). Distinct from F10 (keymap editor):
    // this just asks the Dialog to start/stop recording for this device.
    shortcut = new QShortcut(QKeySequence("F12"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        // Don't toggle recording while editing the keymap overlay.
        if (m_editMode) {
            return;
        }
        emit toggleRecordRequested(m_serial);
    });

    // postGoMenu
    shortcut = new QShortcut(QKeySequence("Ctrl+m"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoMenu();
    });

    // postVolumeUp
    shortcut = new QShortcut(QKeySequence("Ctrl+up"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeUp();
    });

    // postVolumeDown
    shortcut = new QShortcut(QKeySequence("Ctrl+down"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeDown();
    });

    // postPower
    shortcut = new QShortcut(QKeySequence("Ctrl+p"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postPower();
    });

    shortcut = new QShortcut(QKeySequence("Ctrl+o"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDisplayPower(false);
    });

    // expandNotificationPanel
    shortcut = new QShortcut(QKeySequence("Ctrl+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->expandNotificationPanel();
    });

    // collapsePanel
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->collapsePanel();
    });

    // copy
    shortcut = new QShortcut(QKeySequence("Ctrl+c"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCopy();
    });

    // cut
    shortcut = new QShortcut(QKeySequence("Ctrl+x"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCut();
    });

    // clipboardPaste
    shortcut = new QShortcut(QKeySequence("Ctrl+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDeviceClipboard();
    });

    // setDeviceClipboard
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->clipboardPaste();
    });
}

QRect VideoForm::getScreenRect()
{
    QRect screenRect;
    QScreen *screen = QGuiApplication::primaryScreen();
    QWidget *win = window();
    if (win) {
        QWindow *winHandle = win->windowHandle();
        if (winHandle) {
            screen = winHandle->screen();
        }
    }

    if (screen) {
        screenRect = screen->availableGeometry();
    }
    return screenRect;
}

void VideoForm::updateStyleSheet(bool vertical)
{
    if (vertical) {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-v.png) 150px 65px 85px 65px;
                     border-width: 150px 65px 85px 65px;
                 }
                 )");
    } else {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-h.png) 65px 85px 65px 150px;
                     border-width: 65px 85px 65px 150px;
                 }
                 )");
    }
    layout()->setContentsMargins(getMargins(vertical));
}

QMargins VideoForm::getMargins(bool vertical)
{
    QMargins margins;
    if (vertical) {
        margins = QMargins(10, 68, 12, 62);
    } else {
        margins = QMargins(68, 12, 62, 10);
    }
    return margins;
}

void VideoForm::updateShowSize(const QSize &newSize)
{
    if (m_frameSize != newSize) {
        m_frameSize = newSize;

        m_widthHeightRatio = 1.0f * newSize.width() / newSize.height();
        ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

        bool vertical = m_widthHeightRatio < 1.0f ? true : false;
        QSize showSize = newSize;
        QRect screenRect = getScreenRect();
        if (screenRect.isEmpty()) {
            qWarning() << "getScreenRect is empty";
            return;
        }
        if (vertical) {
            showSize.setHeight(qMin(newSize.height(), screenRect.height() - 200));
            showSize.setWidth(showSize.height() * m_widthHeightRatio);
        } else {
            showSize.setWidth(qMin(newSize.width(), screenRect.width() / 2));
            showSize.setHeight(showSize.width() / m_widthHeightRatio);
        }

        if (isFullScreen() && qsc::IDeviceManage::getInstance().getDevice(m_serial)) {
            switchFullScreen();
        }

        if (isMaximized()) {
            showNormal();
        }

        if (m_skin) {
            QMargins m = getMargins(vertical);
            showSize.setWidth(showSize.width() + m.left() + m.right());
            showSize.setHeight(showSize.height() + m.top() + m.bottom());
        }

        if (showSize != size()) {
            resize(showSize);
            if (m_skin) {
                updateStyleSheet(vertical);
            }
            moveCenter();
        }
    }
}

void VideoForm::switchFullScreen()
{
    if (isFullScreen()) {
        // 横屏全屏铺满全屏，恢复时，恢复保持宽高比
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);
        }

        showNormal();
        // back to normal size.
        resize(m_normalSize);
        // fullscreen window will move (0,0). qt bug?
        move(m_fullScreenBeforePos);

#ifdef Q_OS_OSX
        //setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        //show();
#endif
        if (m_skin) {
            updateStyleSheet(m_frameSize.height() > m_frameSize.width());
        }
        showToolForm(this->show_toolbar);
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS);
#endif
    } else {
        // 横屏全屏铺满全屏，不保持宽高比
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
        }

        // record current size before fullscreen, it will be used to rollback size after exit fullscreen.
        m_normalSize = size();

        m_fullScreenBeforePos = pos();
        // 这种临时增加标题栏再全屏的方案会导致收不到mousemove事件，导致setmousetrack失效
        // mac fullscreen must show title bar
#ifdef Q_OS_OSX
        //setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
#endif
        showToolForm(false);
        if (m_skin) {
            layout()->setContentsMargins(0, 0, 0, 0);
        }
        showFullScreen();

        // 全屏状态禁止电脑休眠、息屏
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
    }
}

bool VideoForm::isHost()
{
    if (!m_toolForm) {
        return false;
    }
    return m_toolForm->isHost();
}

void VideoForm::setKeyMapFile(const QString &keyMapFile)
{
    m_keyMapFile = keyMapFile;
}

void VideoForm::updateKeymapEditorGeometry()
{
    if (!m_keymapEditor || !m_videoWidget) {
        return;
    }
    // The KeepRatioWidget letterboxes m_videoWidget so its geometry is exactly
    // the displayed video rect. The overlay fills that rect (it is parented to
    // the video widget, so a (0,0)-origin same-size geometry is correct).
    m_keymapEditor->setGeometry(QRect(QPoint(0, 0), m_videoWidget->size()));
    if (m_editBanner) {
        m_editBanner->setGeometry(0, 0, m_videoWidget->width(), 28);
    }
}

void VideoForm::toggleKeymapEditMode()
{
    if (m_editMode) {
        exitKeymapEditMode();
    } else {
        enterKeymapEditMode();
    }
}

void VideoForm::enterKeymapEditMode()
{
    if (m_editMode) {
        return;
    }
    if (!m_videoWidget || m_videoWidget->isHidden()) {
        return;
    }
    if (m_keyMapFile.isEmpty()) {
        QMessageBox::information(this, "Wraith",
                                 tr("No keymap selected.\nPick a keymap in the main window first."),
                                 QMessageBox::Ok);
        return;
    }

    if (!m_keymapEditor) {
        m_keymapEditor = new KeymapEditor(m_videoWidget, m_videoWidget);
        connect(m_keymapEditor, &KeymapEditor::saved, this, [this](const QString &jsonText) {
            // Live-reload into the running device if possible; otherwise the
            // banner note tells the user to reconnect.
            auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
            if (device) {
                device->updateScript(jsonText);
            }
        });
        // Reflect the editor's capture prompts (bind key, steer-wheel
        // directions, rebind) in the banner; empty restores the default hint.
        connect(m_keymapEditor, &KeymapEditor::promptChanged, this, [this](const QString &prompt) {
            if (!m_editMode || !m_editBanner) {
                return;
            }
            if (prompt.isEmpty()) {
                m_editBanner->setText(kEditBannerHint());
            } else {
                m_editBanner->setText(prompt);
            }
        });
        // Switching/creating a keymap from the editor's chooser: remember it as
        // the current keymap and live-apply it to the running device.
        connect(m_keymapEditor, &KeymapEditor::keymapSwitched, this,
                [this](const QString &filePath, const QString &jsonText) {
            m_keyMapFile = filePath;
            auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
            if (device) {
                device->updateScript(jsonText);
            }
        });
    }

    if (!m_keymapEditor->loadFromFile(m_keyMapFile)) {
        // loadFromFile shows its own error hint; still enter edit mode so the
        // user sees the message rather than a silent no-op.
    }

    if (!m_editBanner) {
        m_editBanner = new QLabel(m_videoWidget);
        m_editBanner->setAlignment(Qt::AlignCenter);
        m_editBanner->setStyleSheet(R"(QLabel {
            background-color: rgba(20, 20, 20, 180);
            color: #FFD700;
            font-weight: bold;
            padding: 4px;
        })");
        m_editBanner->setText(kEditBannerHint());
    }

    m_editMode = true;
    updateKeymapEditorGeometry();
    m_editBanner->show();
    m_editBanner->raise();
    m_keymapEditor->show();
    m_keymapEditor->raise();
    m_keymapEditor->setFocus();
    // The palette + properties live in a SEPARATE floating tool window so the
    // whole video stays free for placing nodes. Park it next to this window.
    m_keymapEditor->showToolWindow(window());
    update();
}

void VideoForm::exitKeymapEditMode()
{
    if (!m_editMode) {
        return;
    }
    m_editMode = false;
    if (m_keymapEditor) {
        m_keymapEditor->hide();
        m_keymapEditor->hideToolWindow();
    }
    if (m_editBanner) {
        m_editBanner->hide();
    }
    setFocus();
    update();
}

void VideoForm::saveKeymapEdits()
{
    if (!m_editMode || !m_keymapEditor) {
        return;
    }
    const QString text = m_keymapEditor->saveToFile();
    if (text.isNull()) {
        return;
    }
    // Briefly reflect the save in the banner.
    if (m_editBanner) {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            m_editBanner->setText(tr("SAVED  —  applied to live session  (F10 to exit)"));
        } else {
            m_editBanner->setText(tr("SAVED  —  reconnect to apply  (F10 to exit)"));
        }
        QTimer::singleShot(2000, this, [this]() {
            if (m_editMode && m_editBanner) {
                m_editBanner->setText(kEditBannerHint());
            }
        });
    }
}

void VideoForm::showRecoilEditor()
{
    if (m_keyMapFile.isEmpty()) {
        QMessageBox::information(this, "Wraith",
            tr("No keymap selected.\nPick a keymap in the main window first."), QMessageBox::Ok);
        return;
    }
    QFile rf(m_keyMapFile);
    if (!rf.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Wraith", tr("Cannot open keymap file."), QMessageBox::Ok);
        return;
    }
    QJsonObject root = QJsonDocument::fromJson(rf.readAll()).object();
    rf.close();
    QJsonObject rc = root.value("recoilControl").toObject();

    // gun library (edited in place; written back on Apply)
    QVector<QJsonObject> guns;
    for (const QJsonValue &gv : rc.value("guns").toArray()) {
        guns.push_back(gv.toObject());
    }
    if (guns.isEmpty()) {   // migrate the old slot1/slot2 shape into named guns
        auto mk = [&](const QString &nm, const QJsonValue &val) {
            QJsonObject g;
            g["name"] = nm;
            if (val.isObject()) {
                g["vertical"] = val.toObject().value("dy").toDouble();
                g["horizontal"] = val.toObject().value("dx").toDouble();
            } else if (val.isArray()) {
                applyPatternToGun(g, val.toArray());
            }
            guns.push_back(g);
        };
        if (rc.contains("slot1")) mk(tr("Slot 1"), rc.value("slot1"));
        if (rc.contains("slot2")) mk(tr("Slot 2"), rc.value("slot2"));
    }
    QString slot1Name = rc.value("slot1Gun").toString();
    QString slot2Name = rc.value("slot2Gun").toString();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Recoil Editor"));
    dlg.resize(600, 520);
    QVBoxLayout *v = new QVBoxLayout(&dlg);

    QHBoxLayout *top = new QHBoxLayout();
    QCheckBox *enabled = new QCheckBox(tr("Enabled"), &dlg);
    enabled->setChecked(rc.value("enabled").toBool(false));
    top->addWidget(enabled);
    top->addWidget(new QLabel(tr("Vertical str"), &dlg));
    QDoubleSpinBox *strY = new QDoubleSpinBox(&dlg);
    strY->setRange(0.05, 8.0); strY->setSingleStep(0.05);
    strY->setValue(rc.value("strength").toDouble(1.0));
    top->addWidget(strY);
    top->addWidget(new QLabel(tr("Horizontal str"), &dlg));
    QDoubleSpinBox *strX = new QDoubleSpinBox(&dlg);
    strX->setRange(0.0, 8.0); strX->setSingleStep(0.05);
    strX->setValue(rc.value("strengthX").toDouble(strY->value()));
    top->addWidget(strX);
    v->addLayout(top);

    QHBoxLayout *mid = new QHBoxLayout();
    QListWidget *list = new QListWidget(&dlg);
    mid->addWidget(list, 1);
    QWidget *right = new QWidget(&dlg);
    QFormLayout *rfl = new QFormLayout(right);
    QDoubleSpinBox *tuneY = new QDoubleSpinBox(right);
    tuneY->setRange(0.0, 5.0); tuneY->setSingleStep(0.05); tuneY->setDecimals(2); tuneY->setValue(1.0);
    QDoubleSpinBox *tuneX = new QDoubleSpinBox(right);
    tuneX->setRange(0.0, 5.0); tuneX->setSingleStep(0.05); tuneX->setDecimals(2); tuneX->setValue(1.0);
    QDoubleSpinBox *vert = new QDoubleSpinBox(right);
    vert->setRange(-2.0, 2.0); vert->setSingleStep(0.01); vert->setDecimals(3);
    QDoubleSpinBox *horz = new QDoubleSpinBox(right);
    horz->setRange(-2.0, 2.0); horz->setSingleStep(0.01); horz->setDecimals(3);
    QLabel *patInfo = new QLabel("-", right);
    QPushButton *recBtn = new QPushButton(tr("\xE2\x97\x8F  Record recoil (fire a mag)"), right);
    QPushButton *autoBtn = new QPushButton(tr("Auto-detect from spray image..."), right);
    QPushButton *traceBtn = new QPushButton(tr("Trace manually from image..."), right);
    QPushButton *clearBtn = new QPushButton(tr("Clear pattern (flat)"), right);
    rfl->addRow(tr("Vertical tune (x)  more = stronger"), tuneY);
    rfl->addRow(tr("Horizontal tune (x)"), tuneX);
    rfl->addRow(tr("Base vertical  (+ down / - up)"), vert);
    rfl->addRow(tr("Base horizontal  (+ left / - right)"), horz);
    rfl->addRow(tr("Pattern"), patInfo);
    rfl->addRow(recBtn);
    rfl->addRow(autoBtn);
    rfl->addRow(traceBtn);
    rfl->addRow(clearBtn);
    mid->addWidget(right, 2);
    v->addLayout(mid, 1);

    QHBoxLayout *lb = new QHBoxLayout();
    QPushButton *addB = new QPushButton(tr("Add gun"), &dlg);
    QPushButton *renB = new QPushButton(tr("Rename"), &dlg);
    QPushButton *remB = new QPushButton(tr("Remove"), &dlg);
    lb->addWidget(addB); lb->addWidget(renB); lb->addWidget(remB);
    v->addLayout(lb);

    QHBoxLayout *slotRow = new QHBoxLayout();
    slotRow->addWidget(new QLabel(tr("Key 1 ->"), &dlg));
    QComboBox *slot1Combo = new QComboBox(&dlg);
    slotRow->addWidget(slot1Combo, 1);
    slotRow->addWidget(new QLabel(tr("Key 2 ->"), &dlg));
    QComboBox *slot2Combo = new QComboBox(&dlg);
    slotRow->addWidget(slot2Combo, 1);
    v->addLayout(slotRow);

    // --- scope presets: an extra multiplier layer on the active gun ---
    v->addWidget(new QLabel(tr("Scope presets (multiply the active gun - from BASE, scale up per zoom). "
                               "Set a hotkey to switch in-game:"), &dlg));
    QTableWidget *scopeTbl = new QTableWidget(0, 4, &dlg);
    scopeTbl->setHorizontalHeaderLabels(QStringList() << tr("Scope") << tr("x Vert") << tr("x Horiz") << tr("Hotkey"));
    scopeTbl->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    scopeTbl->setMaximumHeight(150);
    v->addWidget(scopeTbl);
    auto addScopeRow = [&](const QString &nm, double my, double mx, const QString &hk) {
        const int r = scopeTbl->rowCount();
        scopeTbl->insertRow(r);
        scopeTbl->setItem(r, 0, new QTableWidgetItem(nm));
        scopeTbl->setItem(r, 1, new QTableWidgetItem(QString::number(my)));
        scopeTbl->setItem(r, 2, new QTableWidgetItem(QString::number(mx)));
        scopeTbl->setItem(r, 3, new QTableWidgetItem(hk));
    };
    const QJsonArray scopeArr = rc.value("scopes").toArray();
    if (scopeArr.isEmpty()) {   // seed sensible defaults (edit to taste)
        addScopeRow(tr("Base"), 1.0, 1.0, "");
        addScopeRow(tr("Red Dot"), 1.0, 1.0, "");
        addScopeRow(tr("2x"), 1.7, 1.7, "");
        addScopeRow(tr("3x"), 2.4, 2.4, "");
        addScopeRow(tr("4x"), 3.5, 3.5, "");
        addScopeRow(tr("6x"), 5.0, 5.0, "");
    } else {
        for (const QJsonValue &sv : scopeArr) {
            const QJsonObject so = sv.toObject();
            addScopeRow(so.value("name").toString(), so.value("mulY").toDouble(1.0),
                        so.value("mulX").toDouble(1.0), so.value("hotkey").toString());
        }
    }
    QHBoxLayout *scopeBtns = new QHBoxLayout();
    QPushButton *addScopeB = new QPushButton(tr("Add scope"), &dlg);
    QPushButton *remScopeB = new QPushButton(tr("Remove scope"), &dlg);
    scopeBtns->addWidget(addScopeB);
    scopeBtns->addWidget(remScopeB);
    scopeBtns->addWidget(new QLabel(tr("Active now:"), &dlg));
    QComboBox *activeScopeCombo = new QComboBox(&dlg);
    scopeBtns->addWidget(activeScopeCombo, 1);
    v->addLayout(scopeBtns);
    auto refreshScopeCombo = [&]() {
        const int keep = activeScopeCombo->currentIndex();
        activeScopeCombo->blockSignals(true);
        activeScopeCombo->clear();
        for (int r = 0; r < scopeTbl->rowCount(); ++r) {
            activeScopeCombo->addItem(scopeTbl->item(r, 0) ? scopeTbl->item(r, 0)->text() : QString("?"));
        }
        activeScopeCombo->setCurrentIndex(qBound(0, keep, activeScopeCombo->count() - 1));
        activeScopeCombo->blockSignals(false);
    };
    refreshScopeCombo();
    activeScopeCombo->setCurrentIndex(qBound(0, rc.value("activeScope").toInt(0), activeScopeCombo->count() - 1));
    connect(scopeTbl, &QTableWidget::itemChanged, &dlg, [&](QTableWidgetItem *) { refreshScopeCombo(); });
    connect(addScopeB, &QPushButton::clicked, &dlg, [&]() { addScopeRow(tr("Scope"), 1.0, 1.0, ""); refreshScopeCombo(); });
    connect(remScopeB, &QPushButton::clicked, &dlg, [&]() {
        const int r = scopeTbl->currentRow();
        if (r >= 0) { scopeTbl->removeRow(r); refreshScopeCombo(); }
    });

    QLabel *hint = new QLabel(tr("Guns are saved in the keymap. In-game, keys 1/2 pick the slot's gun - "
                                 "no re-calibrating. Auto-detect wants a spray on a BRIGHT wall (first "
                                 "shot low). Tune strength at a wall; Apply saves + goes live."), &dlg);
    hint->setWordWrap(true);
    v->addWidget(hint);

    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *apply = bb->addButton(tr("Apply"), QDialogButtonBox::ApplyRole);
    v->addWidget(bb);

    int cur = -1;
    auto refreshCombos = [&]() {
        const QString s1 = slot1Combo->count() ? slot1Combo->currentText() : slot1Name;
        const QString s2 = slot2Combo->count() ? slot2Combo->currentText() : slot2Name;
        slot1Combo->blockSignals(true); slot2Combo->blockSignals(true);
        slot1Combo->clear(); slot2Combo->clear();
        for (const QJsonObject &g : guns) {
            slot1Combo->addItem(g.value("name").toString());
            slot2Combo->addItem(g.value("name").toString());
        }
        int i1 = slot1Combo->findText(s1); if (i1 >= 0) slot1Combo->setCurrentIndex(i1);
        int i2 = slot2Combo->findText(s2); if (i2 >= 0) slot2Combo->setCurrentIndex(i2);
        slot1Combo->blockSignals(false); slot2Combo->blockSignals(false);
    };
    auto refreshList = [&]() {
        list->blockSignals(true);
        const int keep = list->currentRow();
        list->clear();
        for (const QJsonObject &g : guns) list->addItem(g.value("name").toString());
        list->blockSignals(false);
        refreshCombos();
        Q_UNUSED(keep);
    };
    auto loadGun = [&](int i) {
        cur = i;
        const bool ok = (i >= 0 && i < guns.size());
        tuneY->setEnabled(ok); tuneX->setEnabled(ok);
        vert->setEnabled(ok); horz->setEnabled(ok);
        autoBtn->setEnabled(ok); traceBtn->setEnabled(ok); clearBtn->setEnabled(ok);
        tuneY->blockSignals(true); tuneX->blockSignals(true);
        vert->blockSignals(true); horz->blockSignals(true);
        if (ok) {
            m_lastRecoilGun = guns[i].value("name").toString();   // reopen here next time
            tuneY->setValue(guns[i].value("mulY").toDouble(1.0));
            tuneX->setValue(guns[i].value("mulX").toDouble(1.0));
            vert->setValue(guns[i].value("vertical").toDouble(0.0));
            horz->setValue(guns[i].value("horizontal").toDouble(0.0));
            const int np = guns[i].value("pattern").toArray().size();
            patInfo->setText(np > 0 ? tr("pattern: %1 steps + sustained").arg(np)
                                    : tr("none (flat sustained)"));
        } else {
            tuneY->setValue(1.0); tuneX->setValue(1.0);
            vert->setValue(0); horz->setValue(0); patInfo->setText("-");
        }
        tuneY->blockSignals(false); tuneX->blockSignals(false);
        vert->blockSignals(false); horz->blockSignals(false);
    };

    connect(list, &QListWidget::currentRowChanged, &dlg, [&](int row) { loadGun(row); });
    connect(tuneY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dlg,
            [&](double val) { if (cur >= 0 && cur < guns.size()) guns[cur]["mulY"] = val; });
    connect(tuneX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dlg,
            [&](double val) { if (cur >= 0 && cur < guns.size()) guns[cur]["mulX"] = val; });
    connect(vert, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dlg,
            [&](double val) { if (cur >= 0 && cur < guns.size()) guns[cur]["vertical"] = val; });
    connect(horz, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dlg,
            [&](double val) { if (cur >= 0 && cur < guns.size()) guns[cur]["horizontal"] = val; });
    connect(addB, &QPushButton::clicked, &dlg, [&]() {
        QJsonObject g;
        g["name"] = tr("Gun %1").arg(guns.size() + 1);
        g["vertical"] = 0.20; g["horizontal"] = 0.0;
        guns.push_back(g);
        refreshList();
        list->setCurrentRow(guns.size() - 1);
    });
    connect(remB, &QPushButton::clicked, &dlg, [&]() {
        if (cur < 0 || cur >= guns.size()) return;
        guns.remove(cur);
        refreshList();
        if (!guns.isEmpty()) list->setCurrentRow(0); else loadGun(-1);
    });
    connect(renB, &QPushButton::clicked, &dlg, [&]() {
        if (cur < 0 || cur >= guns.size()) return;
        bool okk = false;
        const QString nm = QInputDialog::getText(&dlg, tr("Rename gun"), tr("Name:"),
                                                 QLineEdit::Normal, guns[cur].value("name").toString(), &okk);
        if (okk && !nm.isEmpty()) {
            const int keep = cur;
            guns[cur]["name"] = nm;
            refreshList();
            list->setCurrentRow(keep);
        }
    });
    connect(clearBtn, &QPushButton::clicked, &dlg, [&]() {
        if (cur < 0) return;
        guns[cur].remove("pattern");
        loadGun(cur);
    });
    // Save everything to the keymap (so no edits are lost), then live-apply.
    QString recordGunName;
    auto doApply = [&]() {
        rc["enabled"] = enabled->isChecked();
        rc["strength"] = strY->value();
        rc["strengthX"] = strX->value();
        QJsonArray garr;
        for (const QJsonObject &g : guns) garr.append(g);
        rc["guns"] = garr;
        rc["slot1Gun"] = slot1Combo->currentText();
        rc["slot2Gun"] = slot2Combo->currentText();
        rc.remove("slot1"); rc.remove("slot2");
        // scope presets
        QJsonArray sarr;
        for (int r = 0; r < scopeTbl->rowCount(); ++r) {
            auto cell = [&](int c) { return scopeTbl->item(r, c) ? scopeTbl->item(r, c)->text() : QString(); };
            bool okY = false, okX = false;
            const double my = cell(1).toDouble(&okY);
            const double mx = cell(2).toDouble(&okX);
            QJsonObject so;
            so["name"] = cell(0);
            so["mulY"] = okY ? my : 1.0;
            so["mulX"] = okX ? mx : 1.0;
            so["hotkey"] = cell(3).trimmed();
            sarr.append(so);
        }
        rc["scopes"] = sarr;
        rc["activeScope"] = activeScopeCombo->currentIndex();
        root["recoilControl"] = rc;
        const QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Indented);
        QFile wf(m_keyMapFile);
        if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { wf.write(out); wf.close(); }
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) device->updateScript(QString::fromUtf8(out));
    };
    connect(recBtn, &QPushButton::clicked, &dlg, [&]() {
        if (cur < 0) return;
        doApply();                                   // save first - nothing lost
        recordGunName = guns[cur].value("name").toString();
        dlg.accept();                                // close editor; recording starts after exec()
    });
    connect(autoBtn, &QPushButton::clicked, &dlg, [&]() {
        if (cur < 0) return;
        const QString file = QFileDialog::getOpenFileName(&dlg, tr("Spray screenshot / recoil pattern"),
                                                          QString(), tr("Images (*.png *.jpg *.jpeg *.bmp)"));
        if (file.isEmpty()) return;
        QImage img(file);
        if (img.isNull()) return;
        QVector<QPointF> pts = autoDetectSpray(img);   // normalized, shot-ordered
        // Show the detections on the actual image so the user confirms/corrects them:
        // left-click adds a dot the detector missed, right-click removes a stray, and
        // the path re-orders itself. Detector gets ~90%, the eyes close the last bit.
        QPixmap pix = QPixmap::fromImage(img);
        if (pix.width() > 900 || pix.height() > 700)
            pix = pix.scaled(900, 700, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QDialog td(&dlg);
        td.setWindowTitle(tr("Auto-detected %1 dots \xE2\x80\x94 left-click to ADD a missed dot, "
                             "right-click to remove, then OK").arg(pts.size()));
        QVBoxLayout *tv = new QVBoxLayout(&td);
        RecoilTracer *tw = new RecoilTracer(pix, &td, true /*autoOrder*/);
        tw->pts = pts;
        tv->addWidget(tw);
        QDialogButtonBox *tb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &td);
        tv->addWidget(tb);
        connect(tb, &QDialogButtonBox::accepted, &td, &QDialog::accept);
        connect(tb, &QDialogButtonBox::rejected, &td, &QDialog::reject);
        if (td.exec() != QDialog::Accepted) return;
        const QVector<QPointF> finalPts = tw->ordered();
        if (finalPts.size() < 2) {
            QMessageBox::information(&dlg, "Wraith",
                tr("Need at least 2 dots. Click along the spray, or trace manually."));
            return;
        }
        applyPatternToGun(guns[cur], patternFromTrace(finalPts));
        loadGun(cur);
        QMessageBox::information(&dlg, "Wraith",
            tr("Traced %1 path points. Tune strength at a wall.").arg(finalPts.size()));
    });
    connect(traceBtn, &QPushButton::clicked, &dlg, [&]() {
        if (cur < 0) return;
        const QString file = QFileDialog::getOpenFileName(&dlg, tr("Recoil pattern image"),
                                                          QString(), tr("Images (*.png *.jpg *.jpeg *.bmp)"));
        if (file.isEmpty()) return;
        QPixmap pix(file);
        if (pix.isNull()) return;
        if (pix.width() > 900 || pix.height() > 700)
            pix = pix.scaled(900, 700, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QDialog td(&dlg);
        td.setWindowTitle(tr("Click the recoil path: FIRST shot to LAST (right-click = undo)"));
        QVBoxLayout *tv = new QVBoxLayout(&td);
        RecoilTracer *tw = new RecoilTracer(pix, &td);
        tv->addWidget(tw);
        QDialogButtonBox *tb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &td);
        tv->addWidget(tb);
        connect(tb, &QDialogButtonBox::accepted, &td, &QDialog::accept);
        connect(tb, &QDialogButtonBox::rejected, &td, &QDialog::reject);
        if (td.exec() == QDialog::Accepted && tw->pts.size() >= 2) {
            applyPatternToGun(guns[cur], patternFromTrace(tw->pts));
            loadGun(cur);
        }
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    connect(apply, &QPushButton::clicked, &dlg, doApply);

    refreshList();
    // reopen on the last-edited/recorded gun, not always the first
    int startRow = 0;
    if (!m_lastRecoilGun.isEmpty()) {
        for (int i = 0; i < guns.size(); ++i) {
            if (guns[i].value("name").toString() == m_lastRecoilGun) { startRow = i; break; }
        }
    }
    if (!guns.isEmpty()) list->setCurrentRow(startRow); else loadGun(-1);
    dlg.exec();

    // If Record was pressed, the editor is now closed - start the non-modal
    // recording session so the game stays fully interactive.
    if (!recordGunName.isEmpty()) {
        startRecoilRecording(recordGunName);
    }
}

void VideoForm::pushRecoilEnabled(bool on)
{
    QFile rf(m_keyMapFile);
    if (!rf.open(QIODevice::ReadOnly)) return;
    QJsonObject rt = QJsonDocument::fromJson(rf.readAll()).object();
    rf.close();
    QJsonObject r2 = rt.value("recoilControl").toObject();
    r2["enabled"] = on;
    rt["recoilControl"] = r2;
    auto dev = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (dev) dev->updateScript(QJsonDocument(rt).toJson(QJsonDocument::Compact));
}

void VideoForm::startRecoilRecording(const QString &gunName)
{
    if (m_recording || !m_videoWidget) {
        return;
    }
    m_recGun = gunName;
    const QRect g(m_videoWidget->mapToGlobal(QPoint(0, 0)), m_videoWidget->size());
    const int cw = int(g.width() * 0.6), ch = int(g.height() * 0.6);
    m_recRegion = QRect(g.x() + (g.width() - cw) / 2, g.y() + (g.height() - ch) / 2, cw, ch);

    pushRecoilEnabled(false);   // measure the RAW climb

    m_recT.clear(); m_recCx.clear(); m_recCy.clear();
    m_recCumX = m_recCumY = 0.0;
    m_recPrevV.clear(); m_recPrevH.clear();
    m_recHavePrev = false;
    m_recClock.start();

    if (!m_recTimer) {
        m_recTimer = new QTimer(this);
        m_recTimer->setInterval(25);   // ~40 Hz
        connect(m_recTimer, &QTimer::timeout, this, &VideoForm::recoilRecordTick);
    }
    if (!m_recOverlay) {
        m_recOverlay = new QLabel(nullptr);
        m_recOverlay->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        m_recOverlay->setAttribute(Qt::WA_ShowWithoutActivating);
        m_recOverlay->setStyleSheet("color:#39d353;background:rgba(0,0,0,205);"
                                    "font-size:17px;font-weight:bold;padding:10px 18px;border-radius:8px;");
    }
    m_recOverlay->setText(tr("\xE2\x97\x8F REC  '%1' - enter game mode, HOLD FIRE at a wall.  Press F8 to STOP.").arg(gunName));
    m_recOverlay->adjustSize();
    if (QScreen *scr = QGuiApplication::primaryScreen()) {
        const QRect s = scr->geometry();
        m_recOverlay->move(s.center().x() - m_recOverlay->width() / 2, int(s.height() * 0.12));
    }
    m_recOverlay->show();
    m_recOverlay->raise();
    m_recording = true;
    m_recTimer->start();
}

void VideoForm::recoilRecordTick()
{
    if (!m_recording) return;
#if defined(Q_OS_WIN32)
    const bool firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
    const bool firing = true;
#endif
    if (!firing) { m_recHavePrev = false; return; }   // between bursts: reset baseline
    QScreen *scr = QGuiApplication::primaryScreen();
    if (!scr) return;
    const QPixmap pm = scr->grabWindow(0, m_recRegion.x(), m_recRegion.y(),
                                       m_recRegion.width(), m_recRegion.height());
    if (pm.isNull()) return;
    QVector<float> vP, hP;
    profilesFromImage(pm.toImage(), vP, hP);
    if (m_recHavePrev && !m_recPrevV.isEmpty()) {
        const int dyS = bestShift(m_recPrevV, vP, qMax(2, m_recPrevV.size() / 6));
        const int dxS = bestShift(m_recPrevH, hP, qMax(2, m_recPrevH.size() / 6));
        m_recCumY += double(dyS) / qMax(1, vP.size());
        m_recCumX += double(dxS) / qMax(1, hP.size());
        m_recT << m_recClock.elapsed() / 1000.0;
        m_recCx << m_recCumX;
        m_recCy << m_recCumY;
        if (m_recOverlay)
            m_recOverlay->setText(tr("\xE2\x97\x8F REC  '%1' - %2 samples.  Press F8 to STOP.")
                                      .arg(m_recGun).arg(m_recT.size()));
    }
    m_recPrevV = vP;
    m_recPrevH = hP;
    m_recHavePrev = true;
}

void VideoForm::stopRecoilRecording()
{
    if (!m_recording) return;
    m_recording = false;
    if (m_recTimer) m_recTimer->stop();

    // Non-modal result: reuse the banner, auto-hide. NO QMessageBox here - a modal
    // popup while the game hides/grabs the cursor is what left the cursor stuck.
    auto flash = [this](const QString &msg) {
        if (!m_recOverlay) return;
        m_recOverlay->setText(msg);
        m_recOverlay->adjustSize();
        if (QScreen *scr = QGuiApplication::primaryScreen()) {
            const QRect s = scr->geometry();
            m_recOverlay->move(s.center().x() - m_recOverlay->width() / 2, int(s.height() * 0.12));
        }
        m_recOverlay->show();
        m_recOverlay->raise();
        QPointer<QLabel> ov = m_recOverlay;
        QTimer::singleShot(5000, this, [ov]() { if (ov) ov->hide(); });
    };

    if (m_recT.size() < 3) {
        pushRecoilEnabled(true);
        flash(tr("No firing captured - hold LEFT MOUSE at a textured wall."));
        return;
    }
    const QJsonArray pat = patternFromTrajectory(m_recT, m_recCx, m_recCy);
    if (pat.isEmpty()) {
        pushRecoilEnabled(true);
        flash(tr("Not enough motion - fire a longer burst at a textured wall."));
        return;
    }
    // write the pattern into the recorded gun and re-enable recoil (one reload)
    QFile rf(m_keyMapFile);
    if (!rf.open(QIODevice::ReadOnly)) { flash(tr("Couldn't open keymap file.")); return; }
    QJsonObject root = QJsonDocument::fromJson(rf.readAll()).object();
    rf.close();
    QJsonObject rc = root.value("recoilControl").toObject();
    rc["enabled"] = true;
    QJsonArray guns = rc.value("guns").toArray();
    int idx = -1;
    for (int i = 0; i < guns.size(); ++i) {
        if (guns[i].toObject().value("name").toString() == m_recGun) { idx = i; break; }
    }
    QJsonObject g = (idx >= 0) ? guns[idx].toObject() : QJsonObject();
    if (idx < 0) g["name"] = m_recGun;
    applyPatternToGun(g, pat);
    if (idx >= 0) guns[idx] = g; else guns.append(g);

    // log for debugging axis/sign linkage (recoil_record.log next to Wraith.exe)
    QFile lg(QCoreApplication::applicationDirPath() + "/recoil_record.log");
    if (lg.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&lg);
        out << "=== recorded '" << m_recGun << "'  samples=" << m_recT.size()
            << "  dur=" << QString::number(m_recT.last(), 'f', 2) << "s ===\n";
        out << "final cumulative CONTENT drift (px-frac): x(right+)=" << QString::number(m_recCumX, 'f', 4)
            << "  y(down+)=" << QString::number(m_recCumY, 'f', 4) << "\n";
        out << "=> pattern (dx: +left, dy: +down): "
            << QString::fromUtf8(QJsonDocument(pat).toJson(QJsonDocument::Compact)) << "\n";
        out << "=> sustained: vertical(+down)=" << QString::number(g.value("vertical").toDouble(), 'f', 4)
            << "  horizontal(+left)=" << QString::number(g.value("horizontal").toDouble(), 'f', 4) << "\n\n";
        lg.close();
    }
    rc["guns"] = guns;
    root["recoilControl"] = rc;
    const QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QFile wf(m_keyMapFile);
    if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { wf.write(out); wf.close(); }
    auto dev = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (dev) dev->updateScript(QString::fromUtf8(out));

    m_lastRecoilGun = m_recGun;   // so F8 reopens on the gun you just recorded
    flash(tr("\xE2\x9C\x93 Saved '%1' (%2 samples). Cursor's back - press Ctrl for game mode, F8 to tune.")
              .arg(m_recGun).arg(m_recT.size()));
}

void VideoForm::setRecordingIndicator(bool recording)
{
    if (recording) {
        if (!m_videoWidget) {
            return;
        }
        if (!m_recIndicator) {
            m_recIndicator = new QLabel(m_videoWidget);
            m_recIndicator->setText(QStringLiteral("\xE2\x97\x8F REC"));   // "● REC"
            m_recIndicator->setStyleSheet(R"(QLabel {
                background-color: rgba(20, 20, 20, 160);
                color: #FF3B30;
                font-weight: bold;
                padding: 3px 6px;
                border-radius: 4px;
            })");
            m_recIndicator->adjustSize();
        }
        // top-right corner of the video widget
        m_recIndicator->move(qMax(0, m_videoWidget->width() - m_recIndicator->width() - 8), 8);
        m_recIndicator->show();
        m_recIndicator->raise();
    } else if (m_recIndicator) {
        m_recIndicator->hide();
    }
}

void VideoForm::updateFPS(quint32 fps)
{
    //qDebug() << "FPS:" << fps;
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setText(QString("FPS:%1").arg(fps));
}

void VideoForm::grabCursor(bool grab)
{
    QRect rc = getGrabCursorRect();
    MouseTap::getInstance()->enableMouseEventTap(rc, grab);
}

void VideoForm::recoilHint(const QString &hint)
{
    if (!m_hintOverlay) {
        m_hintOverlay = new QLabel(nullptr);
        m_hintOverlay->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        m_hintOverlay->setAttribute(Qt::WA_ShowWithoutActivating);
        m_hintOverlay->setStyleSheet("color:#39d353;background:rgba(0,0,0,190);"
                                     "font-size:22px;font-weight:bold;padding:8px 20px;border-radius:8px;");
    }
    m_hintOverlay->setText(hint);
    m_hintOverlay->adjustSize();
    QRect area;
    if (m_videoWidget && m_videoWidget->isVisible()) {
        area = QRect(m_videoWidget->mapToGlobal(QPoint(0, 0)), m_videoWidget->size());
    } else if (QScreen *scr = QGuiApplication::primaryScreen()) {
        area = scr->geometry();
    }
    m_hintOverlay->move(area.center().x() - m_hintOverlay->width() / 2, area.y() + int(area.height() * 0.08));
    m_hintOverlay->show();
    m_hintOverlay->raise();
    QPointer<QLabel> ov = m_hintOverlay;
    QTimer::singleShot(1400, this, [ov]() { if (ov) ov->hide(); });
}

void VideoForm::onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV, int linesizeY, int linesizeU, int linesizeV)
{
    updateRender(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void VideoForm::staysOnTop(bool top)
{
    bool needShow = false;
    if (isVisible()) {
        needShow = true;
    }
    setWindowFlag(Qt::WindowStaysOnTopHint, top);
    if (m_toolForm) {
        m_toolForm->setWindowFlag(Qt::WindowStaysOnTopHint, top);
    }
    if (needShow) {
        show();
    }
}

void VideoForm::mousePressEvent(QMouseEvent *event)
{
    // In keymap edit mode, mouse-over-video goes to the editor overlay (which
    // is raised above the video). Any press that reaches VideoForm is outside
    // the video rect and should only drive window dragging.
    if (m_editMode) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF globalPos = event->globalPos();
#else
        QPointF globalPos = event->globalPosition();
#endif
        if (event->button() == Qt::LeftButton && !m_videoWidget->geometry().contains(event->pos())) {
            m_dragPosition = globalPos.toPoint() - frameGeometry().topLeft();
            event->accept();
        }
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (event->button() == Qt::MiddleButton) {
        // isPCMode: in PC mode every button belongs to the guest (right = ADS,
        // middle = whatever the game binds). These phone-nav shortcuts must not
        // swallow them -- InputConvertPC reports isCurrentCustomKeymap()==false,
        // so the keymap check alone does NOT protect them.
        if (device && !device->isCurrentCustomKeymap() && !device->isPCMode()) {
            device->postGoHome();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        // isPCMode: in PC mode every button belongs to the guest (right = ADS,
        // middle = whatever the game binds). These phone-nav shortcuts must not
        // swallow them -- InputConvertPC reports isCurrentCustomKeymap()==false,
        // so the keymap check alone does NOT protect them.
        if (device && !device->isCurrentCustomKeymap() && !device->isPCMode()) {
            device->postGoBack();
            return;
        }
    }

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif

    if (m_videoWidget->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF mappedPos = m_videoWidget->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget->frameSize(), m_videoWidget->size());

        // debug keymap pos
        if (event->button() == Qt::LeftButton) {
            qreal x = localPos.x() / m_videoWidget->size().width();
            qreal y = localPos.y() / m_videoWidget->size().height();
            QString posTip = QString(R"("pos": {"x": %1, "y": %2})").arg(x).arg(y);
            qInfo() << posTip.toStdString().c_str();
        }
    } else {
        if (event->button() == Qt::LeftButton) {
            m_dragPosition = globalPos.toPoint() - frameGeometry().topLeft();
            event->accept();
        }
    }
}

void VideoForm::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_editMode) {
        // Only finishing a window drag is meaningful here.
        m_dragPosition = QPoint(0, 0);
        event->accept();
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_dragPosition.isNull()) {
        if (!device) {
            return;
        }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
        // local check
        QPointF local = m_videoWidget->mapFrom(this, localPos.toPoint());
        if (local.x() < 0) {
            local.setX(0);
        }
        if (local.x() > m_videoWidget->width()) {
            local.setX(m_videoWidget->width());
        }
        if (local.y() < 0) {
            local.setY(0);
        }
        if (local.y() > m_videoWidget->height()) {
            local.setY(m_videoWidget->height());
        }
        QMouseEvent newEvent(event->type(), local, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget->frameSize(), m_videoWidget->size());
    } else {
        m_dragPosition = QPoint(0, 0);
    }
}

void VideoForm::mouseMoveEvent(QMouseEvent *event)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
    if (m_editMode) {
        // Only window dragging is allowed; do not forward moves to the device.
        if (!m_dragPosition.isNull() && (event->buttons() & Qt::LeftButton)) {
            move(globalPos.toPoint() - m_dragPosition);
            event->accept();
        }
        return;
    }
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_videoWidget->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF mappedPos = m_videoWidget->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget->frameSize(), m_videoWidget->size());
    } else if (!m_dragPosition.isNull()) {
        if (event->buttons() & Qt::LeftButton) {
            move(globalPos.toPoint() - m_dragPosition);
            event->accept();
        }
    }
}

void VideoForm::mouseDoubleClickEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (event->button() == Qt::LeftButton && !m_videoWidget->geometry().contains(event->pos())) {
        if (!isMaximized()) {
            removeBlackRect();
        }
    }

    if (event->button() == Qt::RightButton && device && !device->isCurrentCustomKeymap() && !device->isPCMode()) {
        emit device->postBackOrScreenOn(event->type() == QEvent::MouseButtonPress);
    }

    if (m_videoWidget->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
        QPointF mappedPos = m_videoWidget->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget->frameSize(), m_videoWidget->size());
    }
}

void VideoForm::wheelEvent(QWheelEvent *event)
{
    if (m_editMode) {
        event->accept();
        return;
    }
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    if (m_videoWidget->geometry().contains(event->position().toPoint())) {
        if (!device) {
            return;
        }
        QPointF pos = m_videoWidget->mapFrom(this, event->position().toPoint());
        QWheelEvent wheelEvent(
            pos, event->globalPosition(), event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(), event->phase(), event->inverted());
#else
    if (m_videoWidget->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF pos = m_videoWidget->mapFrom(this, event->pos());

        QWheelEvent wheelEvent(
            pos, event->globalPosF(), event->pixelDelta(), event->angleDelta(), event->delta(), event->orientation(),
            event->buttons(), event->modifiers(), event->phase(), event->source(), event->inverted());
#endif
        emit device->wheelEvent(&wheelEvent, m_videoWidget->frameSize(), m_videoWidget->size());
    }
}

void VideoForm::keyPressEvent(QKeyEvent *event)
{
    if (m_editMode) {
        // Edit mode swallows keys (the editor overlay handles its own keys;
        // F10/Ctrl+S are global shortcuts). Nothing goes to the device.
        if (Qt::Key_Escape == event->key() && !event->isAutoRepeat()) {
            toggleKeymapEditMode();
        }
        event->accept();
        return;
    }
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    if (Qt::Key_Escape == event->key() && !event->isAutoRepeat() && isFullScreen()) {
        switchFullScreen();
    }

    emit device->keyEvent(event, m_videoWidget->frameSize(), m_videoWidget->size());
}

void VideoForm::keyReleaseEvent(QKeyEvent *event)
{
    if (m_editMode) {
        event->accept();
        return;
    }
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    emit device->keyEvent(event, m_videoWidget->frameSize(), m_videoWidget->size());
}

void VideoForm::paintEvent(QPaintEvent *paint)
{
    Q_UNUSED(paint)
    QStyleOption opt;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    opt.init(this);
#else
    opt.initFrom(this);
#endif
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void VideoForm::showEvent(QShowEvent *event)
{
    Q_UNUSED(event)
    if (!isFullScreen() && this->show_toolbar) {
        QTimer::singleShot(500, this, [this](){
            showToolForm(this->show_toolbar);
        });
    }
}

void VideoForm::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event)
    QSize goodSize = ui->keepRatioWidget->goodSize();
    if (goodSize.isEmpty()) {
        return;
    }
    QSize curSize = size();
    // 限制VideoForm尺寸不能小于keepRatioWidget good size
    if (m_widthHeightRatio > 1.0f) {
        // hor
        if (curSize.height() <= goodSize.height()) {
            setMinimumHeight(goodSize.height());
        } else {
            setMinimumHeight(0);
        }
    } else {
        // ver
        if (curSize.width() <= goodSize.width()) {
            setMinimumWidth(goodSize.width());
        } else {
            setMinimumWidth(0);
        }
    }

    // Keep the keymap-editor overlay aligned with the (letterboxed) video rect.
    if (m_editMode) {
        updateKeymapEditorGeometry();
    }
}

void VideoForm::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    Config::getInstance().setRect(device->getSerial(), geometry());
    device->disconnectDevice();
}

void VideoForm::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void VideoForm::dragMoveEvent(QDragMoveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dropEvent(QDropEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    const QMimeData *qm = event->mimeData();
    QList<QUrl> urls = qm->urls();

    for (const QUrl &url : urls) {
        QString file = url.toLocalFile();
        QFileInfo fileInfo(file);

        if (!fileInfo.exists()) {
            QMessageBox::warning(this, "Wraith", tr("file does not exist"), QMessageBox::Ok);
            continue;
        }

        if (fileInfo.isFile() && fileInfo.suffix() == "apk") {
            emit device->installApkRequest(file);
            continue;
        }
        emit device->pushFileRequest(file, Config::getInstance().getPushFilePath() + fileInfo.fileName());
    }
}
