#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QKeyEvent>
#include <QRandomGenerator>
#include <QTime>
#include <QDateTime>
#include <QTimer>
#include <QPushButton>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QThread>
#include <QProgressDialog>
#include <QCoreApplication>
#include <QUrl>
#include <QBoxLayout>
#include <QEventLoop>

#include "config.h"
#include "dialog.h"
#include "ui_dialog.h"
#include "videoform.h"
#include "../groupcontroller/groupcontroller.h"

#ifdef Q_OS_WIN32
#include "../util/winutils.h"
#endif

QString s_keyMapPath = "";

const QString &getKeyMapPath()
{
    if (s_keyMapPath.isEmpty()) {
        s_keyMapPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_KEYMAP_PATH"));
        QFileInfo fileInfo(s_keyMapPath);
        if (s_keyMapPath.isEmpty() || !fileInfo.isDir()) {
            s_keyMapPath = QCoreApplication::applicationDirPath() + "/keymap";
        }
    }
    return s_keyMapPath;
}

Dialog::Dialog(QWidget *parent) : QWidget(parent), ui(new Ui::Widget)
{
    ui->setupUi(this);
    initUI();

    updateBootConfig(true);

    on_useSingleModeCheck_clicked();
    on_updateDevice_clicked();

    connect(&m_autoUpdatetimer, &QTimer::timeout, this, &Dialog::on_updateDevice_clicked);
    if (ui->autoUpdatecheckBox->isChecked()) {
        m_autoUpdatetimer.start(5000);
    }

    connect(&m_adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        QString log = "";
        bool newLine = true;
        QStringList args = m_adb.arguments();

        switch (processResult) {
        case qsc::AdbProcess::AER_ERROR_START:
            break;
        case qsc::AdbProcess::AER_SUCCESS_START:
            log = "adb run";
            newLine = false;
            break;
        case qsc::AdbProcess::AER_ERROR_EXEC:
            //log = m_adb.getErrorOut();
            if (args.contains("ifconfig") && args.contains("wlan0")) {
                getIPbyIp();
            }
            break;
        case qsc::AdbProcess::AER_ERROR_MISSING_BINARY:
            log = "adb not found";
            break;
        case qsc::AdbProcess::AER_SUCCESS_EXEC:
            //log = m_adb.getStdOut();
            if (args.contains("devices")) {
                QStringList devices = m_adb.getDevicesSerialFromStdOut();
                ui->serialBox->clear();
                ui->connectedPhoneList->clear();
                for (auto &item : devices) {
                    ui->serialBox->addItem(item);
                    ui->connectedPhoneList->addItem(Config::getInstance().getNickName(item) + "-" + item);
                }
            } else if (args.contains("show") && args.contains("wlan0")) {
                QString ip = m_adb.getDeviceIPFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            } else if (args.contains("ifconfig") && args.contains("wlan0")) {
                QString ip = m_adb.getDeviceIPFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            } else if (args.contains("ip -o a")) {
                QString ip = m_adb.getDeviceIPByIpFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            }
            break;
        }
        if (!log.isEmpty()) {
            outLog(log, newLine);
        }
    });

    m_hideIcon = new QSystemTrayIcon(this);
    m_hideIcon->setIcon(QIcon(":/image/tray/logo.png"));
    m_menu = new QMenu(this);
    m_quit = new QAction(this);
    m_showWindow = new QAction(this);
    m_showWindow->setText(tr("show"));
    m_quit->setText(tr("quit"));
    m_menu->addAction(m_showWindow);
    m_menu->addAction(m_quit);
    m_hideIcon->setContextMenu(m_menu);
    m_hideIcon->show();
    connect(m_showWindow, &QAction::triggered, this, &Dialog::show);
    connect(m_quit, &QAction::triggered, this, [this]() {
        m_hideIcon->hide();
        qApp->quit();
    });
    connect(m_hideIcon, &QSystemTrayIcon::activated, this, &Dialog::slotActivated);

    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceConnected, this, &Dialog::onDeviceConnected);
    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceDisconnected, this, &Dialog::onDeviceDisconnected);
}

Dialog::~Dialog()
{
    qDebug() << "~Dialog()";
    updateBootConfig(false);
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
    delete ui;
}

void Dialog::initUI()
{
    setAttribute(Qt::WA_DeleteOnClose);
    //setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint);

    setWindowTitle(Config::getInstance().getTitle());
#ifdef Q_OS_LINUX
    // Set window icon (inherits from application icon set in main.cpp)
    // If application icon was set, this will use it automatically
    if (!qApp->windowIcon().isNull()) {
        setWindowIcon(qApp->windowIcon());
    }
#endif

#ifdef Q_OS_WIN32
    WinUtils::setDarkBorderToWindow((HWND)this->winId(), true);
#endif

    ui->bitRateEdit->setValidator(new QIntValidator(1, 99999, this));

    ui->maxSizeBox->addItem("640");
    ui->maxSizeBox->addItem("720");
    ui->maxSizeBox->addItem("1080");
    ui->maxSizeBox->addItem("1280");
    ui->maxSizeBox->addItem("1920");
    ui->maxSizeBox->addItem(tr("original"));

    ui->formatBox->addItem("mp4");
    ui->formatBox->addItem("mkv");

    ui->lockOrientationBox->addItem(tr("no lock"));
    ui->lockOrientationBox->addItem("0");
    ui->lockOrientationBox->addItem("90");
    ui->lockOrientationBox->addItem("180");
    ui->lockOrientationBox->addItem("270");
    ui->lockOrientationBox->setCurrentIndex(0);

    // 加载IP历史记录
    loadIpHistory();

    // 加载端口历史记录
    loadPortHistory();

    // 为deviceIpEdt添加右键菜单
    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->deviceIpEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showIpEditMenu);
    }
    
    // 为devicePortEdt添加右键菜单
    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->devicePortEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showPortEditMenu);
    }

    // Self-update button, added next to the WIFI/USB connect buttons.
    QPushButton *updateBtn = new QPushButton(tr("Update"), this);
    updateBtn->setObjectName("updateBtn");
    // The wifi/usb connect buttons live in the layout named "horizontalLayout_9".
    QBoxLayout *connectLayout = findChild<QBoxLayout *>("horizontalLayout_9");
    if (connectLayout) {
        connectLayout->addWidget(updateBtn);
    } else if (ui->usbConnectBtn->parentWidget() && ui->usbConnectBtn->parentWidget()->layout()) {
        // Fallback: append to whatever layout manages the connect button's parent widget.
        ui->usbConnectBtn->parentWidget()->layout()->addWidget(updateBtn);
    }
    connect(updateBtn, &QPushButton::clicked, this, &Dialog::checkForUpdate);

    // Mic-gain control: boosts the (quiet laptop) PC mic relative to the game
    // audio in the record mux. Added next to the Update button on the same row.
    QLabel *micGainLabel = new QLabel(tr("Mic gain"), this);
    micGainLabel->setObjectName("micGainLabel");
    m_micGainBox = new QDoubleSpinBox(this);
    m_micGainBox->setObjectName("micGainBox");
    m_micGainBox->setRange(1.0, 12.0);
    m_micGainBox->setSingleStep(0.5);
    m_micGainBox->setValue(5.0);
    m_micGainBox->setSuffix("x");
    m_micGainBox->setToolTip(tr("Boost the PC mic relative to game audio in screen recordings."));
    if (connectLayout) {
        connectLayout->addWidget(micGainLabel);
        connectLayout->addWidget(m_micGainBox);
    } else if (ui->usbConnectBtn->parentWidget() && ui->usbConnectBtn->parentWidget()->layout()) {
        // Fallback: append to whatever layout manages the connect button's parent widget.
        ui->usbConnectBtn->parentWidget()->layout()->addWidget(micGainLabel);
        ui->usbConnectBtn->parentWidget()->layout()->addWidget(m_micGainBox);
    }

    // Video codec selector: H.265 (lighter on the phone) by default, H.264 for
    // wider compatibility. Inserted at the top of the Start Config group. The
    // value is applied from config in updateBootConfig() and read at connect time.
    {
        QWidget *codecRow = new QWidget(this);
        QHBoxLayout *codecLayout = new QHBoxLayout(codecRow);
        codecLayout->setContentsMargins(0, 0, 0, 0);
        QLabel *codecLabel = new QLabel(tr("video codec:"), codecRow);
        m_videoCodecBox = new QComboBox(codecRow);
        m_videoCodecBox->setObjectName("videoCodecBox");
        m_videoCodecBox->addItem("H.265 / HEVC");
        m_videoCodecBox->addItem("H.264 / AVC");
        m_videoCodecBox->setToolTip(tr("H.265 is lighter on the phone; H.264 is more compatible. Applies on next connect."));
        codecLayout->addWidget(codecLabel);
        codecLayout->addWidget(m_videoCodecBox);
        codecLayout->addStretch();
        if (QBoxLayout *cfgLayout = qobject_cast<QBoxLayout *>(ui->configGroupBox->layout())) {
            cfgLayout->insertWidget(0, codecRow);
        }
    }

    // Populate the keymap list at startup so a keymap is always available (for
    // the F10 editor and at connect). updateBootConfig() then restores the
    // saved choice; otherwise the first keymap is selected by default.
    on_refreshGameScriptBtn_clicked();
}

void Dialog::updateBootConfig(bool toView)
{
    if (toView) {
        UserBootConfig config = Config::getInstance().getUserBootConfig();

        if (config.bitRate == 0) {
            ui->bitRateBox->setCurrentText("Mbps");
        } else if (config.bitRate % 1000000 == 0) {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000000));
            ui->bitRateBox->setCurrentText("Mbps");
        } else {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000));
            ui->bitRateBox->setCurrentText("Kbps");
        }

        ui->maxSizeBox->setCurrentIndex(config.maxSizeIndex);
        ui->formatBox->setCurrentIndex(config.recordFormatIndex);
        ui->recordPathEdt->setText(config.recordPath);
        ui->lockOrientationBox->setCurrentIndex(config.lockOrientationIndex);
        if (m_videoCodecBox) {
            m_videoCodecBox->setCurrentIndex(config.videoCodecIndex);
        }
        if (m_micGainBox) {
            m_micGainBox->setValue(config.micGain);
        }
        ui->framelessCheck->setChecked(config.framelessWindow);
        ui->recordScreenCheck->setChecked(config.recordScreen);
        ui->notDisplayCheck->setChecked(config.recordBackground);
        ui->useReverseCheck->setChecked(config.reverseConnect);
        ui->fpsCheck->setChecked(config.showFPS);
        ui->alwaysTopCheck->setChecked(config.windowOnTop);
        ui->closeScreenCheck->setChecked(config.autoOffScreen);
        ui->stayAwakeCheck->setChecked(config.keepAlive);
        ui->useSingleModeCheck->setChecked(config.simpleMode);
        ui->autoUpdatecheckBox->setChecked(config.autoUpdateDevice);
        ui->showToolbar->setChecked(config.showToolbar);
        // Restore the last-used keymap; fall back to the first available so a
        // keymap is always selected (otherwise F10 reports "No keymap selected").
        {
            int gsIdx = ui->gameBox->findText(config.gameScript);
            if (gsIdx >= 0) {
                ui->gameBox->setCurrentIndex(gsIdx);
            } else if (ui->gameBox->count() > 0) {
                ui->gameBox->setCurrentIndex(0);
            }
        }
    } else {
        UserBootConfig config;

        config.bitRate = getBitRate();
        config.maxSizeIndex = ui->maxSizeBox->currentIndex();
        config.recordFormatIndex = ui->formatBox->currentIndex();
        config.recordPath = ui->recordPathEdt->text();
        config.lockOrientationIndex = ui->lockOrientationBox->currentIndex();
        config.videoCodecIndex = m_videoCodecBox ? m_videoCodecBox->currentIndex() : 0;
        config.micGain = m_micGainBox ? m_micGainBox->value() : 5.0;
        config.recordScreen = ui->recordScreenCheck->isChecked();
        config.recordBackground = ui->notDisplayCheck->isChecked();
        config.reverseConnect = ui->useReverseCheck->isChecked();
        config.showFPS = ui->fpsCheck->isChecked();
        config.windowOnTop = ui->alwaysTopCheck->isChecked();
        config.autoOffScreen = ui->closeScreenCheck->isChecked();
        config.framelessWindow = ui->framelessCheck->isChecked();
        config.keepAlive = ui->stayAwakeCheck->isChecked();
        config.simpleMode = ui->useSingleModeCheck->isChecked();
        config.autoUpdateDevice = ui->autoUpdatecheckBox->isChecked();
        config.showToolbar = ui->showToolbar->isChecked();
        config.gameScript = ui->gameBox->currentText();

        // 保存当前IP到历史记录
        QString currentIp = ui->deviceIpEdt->currentText().trimmed();
        if (!currentIp.isEmpty()) {
            saveIpHistory(currentIp);
        }

        Config::getInstance().setUserBootConfig(config);
    }
}

void Dialog::execAdbCmd()
{
    if (checkAdbRun()) {
        return;
    }
    QString cmd = ui->adbCommandEdt->text().trimmed();
    outLog("adb " + cmd, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", Qt::SkipEmptyParts));
#else
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", QString::SkipEmptyParts));
#endif
}

void Dialog::delayMs(int ms)
{
    QTime dieTime = QTime::currentTime().addMSecs(ms);

    while (QTime::currentTime() < dieTime) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
}

QString Dialog::getGameScript(const QString &fileName)
{
    if (fileName.isEmpty()) {
        return "";
    }

    QFile loadFile(getKeyMapPath() + "/" + fileName);
    if (!loadFile.open(QIODevice::ReadOnly)) {
        outLog("open file failed:" + fileName, true);
        return "";
    }

    QString ret = loadFile.readAll();
    loadFile.close();
    return ret;
}

void Dialog::slotActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
#ifdef Q_OS_WIN32
        this->show();
#endif
        break;
    default:
        break;
    }
}

void Dialog::closeEvent(QCloseEvent *event)
{
    // The window hides to the tray on close (this is the usual "exit"), so save
    // settings here — otherwise bitrate/resolution/codec/keymap/mic-gain/etc.
    // would only persist on an explicit tray "quit".
    updateBootConfig(false);
    this->hide();
    if (!Config::getInstance().getTrayMessageShown()) {
        Config::getInstance().setTrayMessageShown(true);
        m_hideIcon->showMessage(tr("Notice"),
                                tr("Hidden here!"),
                                QSystemTrayIcon::Information,
                                3000);
    }
    event->ignore();
}

void Dialog::on_updateDevice_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("update devices...", false);
    m_adb.execute("", QStringList() << "devices");
}

void Dialog::on_startServerBtn_clicked()
{
    outLog("start server...", false);

    // this is ok that "original" toUshort is 0
    quint16 videoSize = ui->maxSizeBox->currentText().trimmed().toUShort();
    qsc::DeviceParams params;
    params.serial = ui->serialBox->currentText().trimmed();
    params.maxSize = videoSize;
    params.bitRate = getBitRate();
    // on devices with Android >= 10, the capture frame rate can be limited
    params.maxFps = static_cast<quint32>(Config::getInstance().getMaxFps());
    params.closeScreen = ui->closeScreenCheck->isChecked();
    params.useReverse = ui->useReverseCheck->isChecked();
    params.display = !ui->notDisplayCheck->isChecked();
    params.renderExpiredFrames = Config::getInstance().getRenderExpiredFrames();
    if (ui->lockOrientationBox->currentIndex() > 0) {
        params.captureOrientationLock = 1;
        params.captureOrientation = (ui->lockOrientationBox->currentIndex() - 1) * 90;
    }
    params.stayAwake = ui->stayAwakeCheck->isChecked();
    params.recordFile = ui->recordScreenCheck->isChecked();
    params.recordPath = ui->recordPathEdt->text().trimmed();
    params.recordFileFormat = ui->formatBox->currentText().trimmed();
    params.serverLocalPath = getServerPath();
    params.serverRemotePath = Config::getInstance().getServerPath();
    params.pushFilePath = Config::getInstance().getPushFilePath();
    params.gameScript = getGameScript(ui->gameBox->currentText());
    params.logLevel = Config::getInstance().getLogLevel();
    params.codecOptions = Config::getInstance().getCodecOptions();
    params.codecName = Config::getInstance().getCodecName();
    // H.265 (index 0) by default; H.264 when the user picks index 1.
    params.videoCodec = (m_videoCodecBox && m_videoCodecBox->currentIndex() == 1) ? "h264" : "h265";
    params.scid = QRandomGenerator::global()->bounded(1, 10000) & 0x7FFFFFFF;

    qsc::IDeviceManage::getInstance().connectDevice(params);
}

void Dialog::on_stopServerBtn_clicked()
{
    if (qsc::IDeviceManage::getInstance().disconnectDevice(ui->serialBox->currentText().trimmed())) {
        outLog("stop server");
    }
}

void Dialog::on_wirelessConnectBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    QString addr = ui->deviceIpEdt->currentText().trimmed();
    if (addr.isEmpty()) {
        outLog("error: device ip is null", false);
        return;
    }

    if (!ui->devicePortEdt->currentText().isEmpty()) {
        addr += ":";
        addr += ui->devicePortEdt->currentText().trimmed();
    } else if (!ui->devicePortEdt->lineEdit()->placeholderText().isEmpty()) {
        addr += ":";
        addr += ui->devicePortEdt->lineEdit()->placeholderText().trimmed();
    } else {
        outLog("error: device port is null", false);
        return;
    }

    // 保存IP历史记录 - 只保存IP部分,不包含端口
    QString ip = addr.split(":").first();
    if (!ip.isEmpty()) {
        saveIpHistory(ip);
    }
    
    // 保存端口历史记录
    QString port = addr.split(":").last();
    if (!port.isEmpty() && port != ip) {
        savePortHistory(port);
    }

    outLog("wireless connect...", false);
    QStringList adbArgs;
    adbArgs << "connect";
    adbArgs << addr;
    m_adb.execute("", adbArgs);
}

void Dialog::on_startAdbdBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("start devices adbd...", false);
    // adb tcpip 5555
    QStringList adbArgs;
    adbArgs << "tcpip";
    adbArgs << "5555";
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::outLog(const QString &log, bool newLine)
{
    // avoid sub thread update ui
    QString backLog = log;
    QTimer::singleShot(0, this, [this, backLog, newLine]() {
        ui->outEdit->append(backLog);
        if (newLine) {
            ui->outEdit->append("<br/>");
        }
    });
}

bool Dialog::filterLog(const QString &log)
{
    if (log.contains("app_proces")) {
        return true;
    }
    if (log.contains("Unable to set geometry")) {
        return true;
    }
    return false;
}

bool Dialog::checkAdbRun()
{
    if (m_adb.isRuning()) {
        outLog("wait for the end of the current command to run");
    }
    return m_adb.isRuning();
}

void Dialog::on_getIPBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }

    outLog("get ip...", false);
    // adb -s P7C0218510000537 shell ifconfig wlan0
    // or
    // adb -s P7C0218510000537 shell ip -f inet addr show wlan0
    QStringList adbArgs;
#if 0
    adbArgs << "shell";
    adbArgs << "ip";
    adbArgs << "-f";
    adbArgs << "inet";
    adbArgs << "addr";
    adbArgs << "show";
    adbArgs << "wlan0";
#else
    adbArgs << "shell";
    adbArgs << "ifconfig";
    adbArgs << "wlan0";
#endif
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::getIPbyIp()
{
    if (checkAdbRun()) {
        return;
    }

    QStringList adbArgs;
    adbArgs << "shell";
    adbArgs << "ip -o a";

    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size)
{
    Q_UNUSED(deviceName);
    if (!success) {
        return;
    }
    auto videoForm = new VideoForm(ui->framelessCheck->isChecked(), Config::getInstance().getSkin(), ui->showToolbar->isChecked());
    videoForm->setSerial(serial);

    // Tell the video form which keymap file the in-window editor (F10) should
    // load/save. Matches the keymap the device was started with (gameBox).
    const QString gameFile = ui->gameBox->currentText().trimmed();
    if (!gameFile.isEmpty()) {
        videoForm->setKeyMapFile(getKeyMapPath() + "/" + gameFile);
    }

    qsc::IDeviceManage::getInstance().getDevice(serial)->setUserData(static_cast<void*>(videoForm));
    qsc::IDeviceManage::getInstance().getDevice(serial)->registerDeviceObserver(videoForm);

    // F12 recording toggle: the VideoForm emits this; Dialog owns the recorder +
    // audio tee/mic + mux orchestration.
    connect(videoForm, &VideoForm::toggleRecordRequested, this, &Dialog::onToggleRecordRequested);

    videoForm->showFPS(ui->fpsCheck->isChecked());

    if (ui->alwaysTopCheck->isChecked()) {
        videoForm->staysOnTop();
    }

#ifndef Q_OS_WIN32
    // must be show before updateShowSize
    videoForm->show();
#endif
    QString name = Config::getInstance().getNickName(serial);
    if (name.isEmpty()) {
        name = Config::getInstance().getTitle();
    }
    videoForm->setWindowTitle(name + "-" + serial);
    videoForm->updateShowSize(size);

    bool deviceVer = size.height() > size.width();
    QRect rc = Config::getInstance().getRect(serial);
    bool rcVer = rc.height() > rc.width();
    // same width/height rate
    if (rc.isValid() && (deviceVer == rcVer)) {
        // mark: resize is for fix setGeometry magneticwidget bug
        videoForm->resize(rc.size());
        videoForm->setGeometry(rc);
    }

#ifdef Q_OS_WIN32
    // windows是show太早可以看到resize的过程
    QTimer::singleShot(200, videoForm, [videoForm](){videoForm->show();});
#endif

    GroupController::instance().addDevice(serial);

    // Recording is now driven mid-session by F12 (onToggleRecordRequested), which
    // owns the game-audio tee + PC mic + mux. Here we only auto-start native audio
    // PLAYBACK together with the mirror (deferred a moment so the video window
    // appears first; the audio server push/connect blocks briefly).
    m_recActive = false;
    QString audioSerial = serial;
    QTimer::singleShot(300, this, [this, audioSerial]() {
        m_audioOutput.start(audioSerial, 28200);
    });
}

void Dialog::onToggleRecordRequested(const QString &serial)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return;
    }

    // resolve this device's video form (for the on-screen REC indicator)
    VideoForm *vf = nullptr;
    if (void *data = device->getUserData()) {
        vf = static_cast<VideoForm *>(data);
    }

    if (!device->isRecording()) {
        // ---- START recording ----
        const QString recPath = ui->recordPathEdt->text().trimmed();
        if (recPath.isEmpty()) {
            QMessageBox::warning(this, "Wraith",
                                 tr("Set a record save path first (Record path)."),
                                 QMessageBox::Ok);
            return;
        }
        QString fmt = ui->formatBox->currentText().trimmed();
        if (fmt.isEmpty()) {
            fmt = "mp4";
        }

        // build the output path the same way the core recorder used to:
        // <recordPath>/<serial>_<yyyyMMdd_hhmmss_zzz>.<fmt>
        QString fileName = serial + QDateTime::currentDateTime().toString("_yyyyMMdd_hhmmss_zzz");
        fileName.replace(":", "_");
        fileName.replace(".", "_");
        fileName += ("." + fmt);
        QDir dir(recPath);
        if (!dir.exists()) {
            dir.mkpath(recPath);
        }
        const QString videoPath = dir.absoluteFilePath(fileName);

        if (!device->startRecord(videoPath, fmt)) {
            QMessageBox::warning(this, "Wraith",
                                 tr("Could not start recording."),
                                 QMessageBox::Ok);
            return;
        }

        // remember session details for the audio tee + mux on stop
        m_recActive = true;
        m_recPath = recPath;
        m_recSerial = serial;
        m_recFormat = fmt;
        m_recGamePcm = QDir(m_recPath).filePath(QString(".wraith_game_%1.pcm").arg(serial));
        m_recMicPcm = QDir(m_recPath).filePath(QString(".wraith_mic_%1.pcm").arg(serial));

        // start game-audio tee (opens immediately if playback is running) + PC mic
        m_audioOutput.startTee(m_recGamePcm);
        m_pcMic.start(m_recMicPcm);   // best-effort: ok if no mic present

        if (vf) {
            vf->setRecordingIndicator(true);
        }
        outLog("recording started: " + videoPath, true);
    } else {
        // ---- STOP recording ----
        device->stopRecord();         // synchronous finalize of the video file
        m_audioOutput.stopTee();      // close the game-audio tee (keep playback)
        m_pcMic.stop();               // stop PC mic capture

        if (m_recActive && serial == m_recSerial) {
            muxRecording();           // mux audio into the just-written video
        }
        m_recActive = false;

        if (vf) {
            vf->setRecordingIndicator(false);
        }
        outLog("recording stopped", true);
    }
}

void Dialog::onDeviceDisconnected(QString serial)
{
    // Safety: if a recording is still live for this device when it disconnects,
    // finalize it and mux the captured audio. The core also finalizes the video
    // file during teardown.
    auto liveDevice = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (liveDevice && liveDevice->isRecording()) {
        liveDevice->stopRecord();
    }
    // stop the game-audio tee + PC mic (leave native playback to m_audioOutput.stop)
    m_audioOutput.stopTee();
    m_pcMic.stop();
    if (m_recActive && serial == m_recSerial) {
        muxRecording();
    }
    m_recActive = false;

    // stop native audio playback together with the mirror
    m_audioOutput.stop();

    GroupController::instance().removeDevice(serial);
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return;
    }
    auto data = device->getUserData();
    if (data) {
        VideoForm* vf = static_cast<VideoForm*>(data);
        qsc::IDeviceManage::getInstance().getDevice(serial)->deRegisterDeviceObserver(vf);
        vf->close();
        vf->deleteLater();
    }
}

void Dialog::muxRecording()
{
    const QString recPath = m_recPath;
    const QString serial = m_recSerial;
    QString fmt = m_recFormat;
    if (fmt.isEmpty()) {
        fmt = "mp4";
    }
    const QString gamePcm = m_recGamePcm;
    const QString micPcm = m_recMicPcm;

    auto cleanupPcm = [&]() {
        if (!gamePcm.isEmpty()) QFile::remove(gamePcm);
        if (!micPcm.isEmpty()) QFile::remove(micPcm);
    };

    if (recPath.isEmpty() || serial.isEmpty()) {
        cleanupPcm();
        return;
    }

    // Locate the video the core recorder wrote this session:
    // <recordPath>/<serial>_<timestamp>.<fmt>, newest match. The core finalizes
    // the file during teardown, so poll until it exists AND its size is stable
    // (unchanged across ~200ms) for up to ~5s before muxing.
    QDir dir(recPath);
    const QStringList nameFilters{ QString("%1_*.%2").arg(serial, fmt) };
    QString videoFile;
    qint64 lastSize = -1;
    QElapsedTimer waitTimer;
    waitTimer.start();
    while (waitTimer.elapsed() < 5000) {
        QFileInfoList matches = dir.entryInfoList(nameFilters, QDir::Files, QDir::Time);
        if (!matches.isEmpty()) {
            const QFileInfo& newest = matches.first(); // QDir::Time => newest first
            const qint64 sz = newest.size();
            if (sz > 0 && sz == lastSize) {
                videoFile = newest.absoluteFilePath();
                break;
            }
            lastSize = sz;
        }
        QThread::msleep(200);
        // re-stat on next loop iteration (QFileInfo above is fresh each pass)
    }

    if (videoFile.isEmpty()) {
        qWarning() << "muxRecording::could not find/stabilize recorded video for serial" << serial;
        cleanupPcm();
        return;
    }

    // Need at least game audio to mux; if it's missing/empty, leave video as-is.
    const QFileInfo gameInfo(gamePcm);
    const bool haveGame = gameInfo.exists() && gameInfo.size() > 0;
    if (!haveGame) {
        qWarning() << "muxRecording::no game audio captured, leaving video-only file";
        cleanupPcm();
        return;
    }

    const QFileInfo micInfo(micPcm);
    const bool haveMic = micInfo.exists() && micInfo.size() > 0;

    // A/V sync: the audio tee + mic started at F12 (t=0), but the video recorder
    // waited for the next keyframe before writing its first frame (HEVC keyframes
    // can be several seconds apart). Query the core for that gap and trim the same
    // number of leading seconds off each audio input so it lines up with video.
    double skipSec = 0.0;
    if (auto recDevice = qsc::IDeviceManage::getInstance().getDevice(serial)) {
        const qint64 skipMs = recDevice->getRecordAudioSkipMs();
        skipSec = skipMs / 1000.0;
        qInfo() << "muxRecording::audio A/V-sync skip" << skipMs << "ms (" << skipSec << "s)";
    }
    const bool doSkip = skipSec > 0.001;
    const QString skipStr = QString::number(skipSec, 'f', 3);

    const QString ffmpeg = QCoreApplication::applicationDirPath() + "/ffmpeg.exe";
    if (!QFile::exists(ffmpeg)) {
        qWarning() << "muxRecording::ffmpeg.exe not found at" << ffmpeg << "- leaving video-only file";
        cleanupPcm();
        return;
    }

    // mux into a temp file, then atomically replace the original on success.
    const QString outTmp = videoFile + ".muxtmp." + fmt;
    QFile::remove(outTmp);

    // -ss placed immediately BEFORE each audio -i is an input option: it discards
    // the leading skipSec of that PCM so it starts at the video's first frame. The
    // video input gets NO -ss (it already starts at the first written frame).
    // Mic-gain multiplier applied to the (quiet) PC mic before amix. Default 5.0
    // if the spinbox is somehow unavailable.
    const double micGain = m_micGainBox ? m_micGainBox->value() : 5.0;
    const QString micGainStr = QString::number(micGain, 'f', 2);

    QStringList args;
    if (haveMic) {
        qInfo() << "muxRecording::mic gain" << micGainStr << "x";
        // video + game(stereo) + boosted mic(mono) -> amix to a single stereo AAC
        // track, with a limiter to protect against clipping from the boost.
        const QString micFilter =
            QString("[2:a]volume=%1[m];[1:a][m]amix=inputs=2:normalize=0:dropout_transition=0[mix];[mix]alimiter=limit=0.95[a]")
                .arg(micGainStr);
        args << "-y"
             << "-i" << videoFile
             << "-f" << "s16le" << "-ar" << "48000" << "-ac" << "2";
        if (doSkip) args << "-ss" << skipStr;
        args << "-i" << gamePcm
             << "-f" << "s16le" << "-ar" << "48000" << "-ac" << "1";
        if (doSkip) args << "-ss" << skipStr;
        args << "-i" << micPcm
             << "-filter_complex" << micFilter
             << "-map" << "0:v" << "-map" << "[a]"
             << "-c:v" << "copy" << "-c:a" << "aac" << "-b:a" << "192k"
             << outTmp;
    } else {
        // no mic: mux just the game audio
        qInfo() << "muxRecording::no mic audio, muxing game audio only";
        args << "-y"
             << "-i" << videoFile
             << "-f" << "s16le" << "-ar" << "48000" << "-ac" << "2";
        if (doSkip) args << "-ss" << skipStr;
        args << "-i" << gamePcm
             << "-map" << "0:v" << "-map" << "1:a"
             << "-c:v" << "copy" << "-c:a" << "aac" << "-b:a" << "192k"
             << outTmp;
    }

    qInfo() << "muxRecording::running ffmpeg" << args;
    QProcess mux;
    mux.start(ffmpeg, args);
    if (!mux.waitForStarted(5000)) {
        qWarning() << "muxRecording::ffmpeg failed to start, leaving video-only file";
        QFile::remove(outTmp);
        cleanupPcm();
        return;
    }
    // give ffmpeg room to finish (copy video + encode short audio is fast)
    mux.waitForFinished(120000);

    if (QProcess::NormalExit == mux.exitStatus() && 0 == mux.exitCode()
        && QFileInfo(outTmp).size() > 0) {
        // replace the original video file with the muxed one (keep recorder name)
        if (QFile::remove(videoFile) && QFile::rename(outTmp, videoFile)) {
            qInfo() << "muxRecording::audio muxed into" << videoFile;
        } else {
            qWarning() << "muxRecording::failed to replace original video with muxed file;"
                       << "muxed output left at" << outTmp;
        }
    } else {
        qWarning() << "muxRecording::ffmpeg failed (exit" << mux.exitCode() << "):"
                   << mux.readAllStandardError();
        QFile::remove(outTmp);
    }

    cleanupPcm();
}

void Dialog::on_wirelessDisConnectBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    QString addr = ui->deviceIpEdt->currentText().trimmed();
    outLog("wireless disconnect...", false);
    QStringList adbArgs;
    adbArgs << "disconnect";
    adbArgs << addr;
    m_adb.execute("", adbArgs);
}

void Dialog::on_selectRecordPathBtn_clicked()
{
    QFileDialog::Options options = QFileDialog::DontResolveSymlinks | QFileDialog::ShowDirsOnly;
    QString directory = QFileDialog::getExistingDirectory(this, tr("select path"), "", options);
    ui->recordPathEdt->setText(directory);
}

void Dialog::on_recordPathEdt_textChanged(const QString &arg1)
{
    ui->recordPathEdt->setToolTip(arg1.trimmed());
    ui->notDisplayCheck->setCheckable(!arg1.trimmed().isEmpty());
}

void Dialog::on_adbCommandBtn_clicked()
{
    execAdbCmd();
}

void Dialog::on_stopAdbBtn_clicked()
{
    m_adb.kill();
}

void Dialog::on_clearOut_clicked()
{
    ui->outEdit->clear();
}

void Dialog::on_stopAllServerBtn_clicked()
{
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
}

void Dialog::on_refreshGameScriptBtn_clicked()
{
    ui->gameBox->clear();
    QDir dir(getKeyMapPath());
    if (!dir.exists()) {
        outLog("keymap directory not find", true);
        return;
    }
    dir.setFilter(QDir::Files | QDir::NoSymLinks);
    QFileInfoList list = dir.entryInfoList();
    QFileInfo fileInfo;
    int size = list.size();
    for (int i = 0; i < size; ++i) {
        fileInfo = list.at(i);
        ui->gameBox->addItem(fileInfo.fileName());
    }
}

void Dialog::on_applyScriptBtn_clicked()
{
    auto curSerial = ui->serialBox->currentText().trimmed();
    auto device = qsc::IDeviceManage::getInstance().getDevice(curSerial);
    if (!device) {
        return;
    }

    device->updateScript(getGameScript(ui->gameBox->currentText()));
}

void Dialog::on_recordScreenCheck_clicked(bool checked)
{
    if (!checked) {
        return;
    }

    QString fileDir(ui->recordPathEdt->text().trimmed());
    if (fileDir.isEmpty()) {
        qWarning() << "please select record save path!!!";
        ui->recordScreenCheck->setChecked(false);
    }
}

void Dialog::on_usbConnectBtn_clicked()
{
    on_stopAllServerBtn_clicked();
    delayMs(200);
    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_startServerBtn_clicked();
}

int Dialog::findDeviceFromeSerialBox(bool wifi)
{
    QString regStr = "\\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\:([0-9]|[1-9]\\d|[1-9]\\d{2}|[1-9]\\d{3}|[1-5]\\d{4}|6[0-4]\\d{3}|65[0-4]\\d{2}|655[0-2]\\d|6553[0-5])\\b";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp regIP(regStr);
#else
    QRegularExpression regIP(regStr);
#endif
    for (int i = 0; i < ui->serialBox->count(); ++i) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        bool isWifi = regIP.exactMatch(ui->serialBox->itemText(i));
#else
        bool isWifi = regIP.match(ui->serialBox->itemText(i)).hasMatch();
#endif
        bool found = wifi ? isWifi : !isWifi;
        if (found) {
            return i;
        }
    }

    return -1;
}

void Dialog::on_wifiConnectBtn_clicked()
{
    on_stopAllServerBtn_clicked();
    delayMs(200);

    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_getIPBtn_clicked();
    delayMs(200);

    on_startAdbdBtn_clicked();
    delayMs(1000);

    on_wirelessConnectBtn_clicked();
    delayMs(2000);

    on_updateDevice_clicked();
    delayMs(200);

    int firstWifiDevice = findDeviceFromeSerialBox(true);
    if (-1 == firstWifiDevice) {
        qWarning() << "No wifi device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstWifiDevice);

    on_startServerBtn_clicked();
}

void Dialog::on_connectedPhoneList_itemDoubleClicked(QListWidgetItem *item)
{
    Q_UNUSED(item);
    ui->serialBox->setCurrentIndex(ui->connectedPhoneList->currentRow());
    on_startServerBtn_clicked();
}

void Dialog::on_updateNameBtn_clicked()
{
    if (ui->serialBox->count() != 0) {
        if (ui->userNameEdt->text().isEmpty()) {
            Config::getInstance().setNickName(ui->serialBox->currentText(), "Phone");
        } else {
            Config::getInstance().setNickName(ui->serialBox->currentText(), ui->userNameEdt->text());
        }

        on_updateDevice_clicked();

        qDebug() << "Update OK!";
    } else {
        qWarning() << "No device is connected!";
    }
}

void Dialog::on_useSingleModeCheck_clicked()
{
    if (ui->useSingleModeCheck->isChecked()) {
        ui->rightWidget->hide();
    } else {
        ui->rightWidget->show();
    }

    adjustSize();
}

void Dialog::on_serialBox_currentIndexChanged(const QString &arg1)
{
    ui->userNameEdt->setText(Config::getInstance().getNickName(arg1));
}

quint32 Dialog::getBitRate()
{
    return ui->bitRateEdit->text().trimmed().toUInt() *
            (ui->bitRateBox->currentText() == QString("Mbps") ? 1000000 : 1000);
}

const QString &Dialog::getServerPath()
{
    static QString serverPath;
    if (serverPath.isEmpty()) {
        serverPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_SERVER_PATH"));
        QFileInfo fileInfo(serverPath);
        if (serverPath.isEmpty() || !fileInfo.isFile()) {
            serverPath = QCoreApplication::applicationDirPath() + "/scrcpy-server";
        }
    }
    return serverPath;
}

void Dialog::on_autoUpdatecheckBox_toggled(bool checked)
{
    if (checked) {
        m_autoUpdatetimer.start(5000);
    } else {
        m_autoUpdatetimer.stop();
    }
}

void Dialog::loadIpHistory()
{
    QStringList ipList = Config::getInstance().getIpHistory();
    ui->deviceIpEdt->clear();
    ui->deviceIpEdt->addItems(ipList);
    ui->deviceIpEdt->setContentsMargins(0, 0, 0, 0);

    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setMaxLength(128);
        ui->deviceIpEdt->lineEdit()->setPlaceholderText("192.168.0.1");
    }
}

void Dialog::saveIpHistory(const QString &ip)
{
    if (ip.isEmpty()) {
        return;
    }
    
    Config::getInstance().saveIpHistory(ip);
    
    // 更新ComboBox
    loadIpHistory();
    ui->deviceIpEdt->setCurrentText(ip);
}

void Dialog::showIpEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->deviceIpEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearIpHistory();
        loadIpHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->deviceIpEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}

void Dialog::loadPortHistory()
{
    QStringList portList = Config::getInstance().getPortHistory();
    ui->devicePortEdt->clear();
    ui->devicePortEdt->addItems(portList);
    ui->devicePortEdt->setContentsMargins(0, 0, 0, 0);

    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setMaxLength(6);
        ui->devicePortEdt->lineEdit()->setPlaceholderText("5555");
    }
}

void Dialog::savePortHistory(const QString &port)
{
    if (port.isEmpty()) {
        return;
    }
    
    Config::getInstance().savePortHistory(port);
    
    // 更新ComboBox
    loadPortHistory();
    ui->devicePortEdt->setCurrentText(port);
}

void Dialog::showPortEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->devicePortEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearPortHistory();
        loadPortHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->devicePortEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}

// Returns true if remote version (e.g. "0.6.0") is strictly newer than local (e.g. "0.5.0").
static bool isRemoteNewer(const QString &remote, const QString &local)
{
    const QStringList rParts = remote.split('.', Qt::SkipEmptyParts);
    const QStringList lParts = local.split('.', Qt::SkipEmptyParts);
    const int count = qMax(rParts.size(), lParts.size());
    for (int i = 0; i < count; ++i) {
        const int r = (i < rParts.size()) ? rParts.at(i).toInt() : 0;
        const int l = (i < lParts.size()) ? lParts.at(i).toInt() : 0;
        if (r > l) {
            return true;
        }
        if (r < l) {
            return false;
        }
    }
    return false;
}

void Dialog::checkForUpdate()
{
    // Use the system curl.exe (Windows 10 1803+/11 ship it in System32) for the
    // HTTPS work. It uses Windows Schannel for TLS, so Wraith needs no OpenSSL
    // DLLs bundled — Qt 5.15.2 would otherwise require libssl-1_1/libcrypto-1_1.
    QString curlExe = QDir(QString::fromLocal8Bit(qgetenv("SystemRoot")))
                          .filePath("System32/curl.exe");
    if (!QFile::exists(curlExe)) {
        curlExe = "curl.exe"; // fall back to PATH
    }

    const QString api = "https://api.github.com/repos/jtiberius9-spec/Wraith/releases/latest";

    // --- 1) fetch release metadata ---
    QProcess meta;
    meta.start(curlExe, { "-sSL", "-H", "User-Agent: Wraith-Updater", api });
    if (!meta.waitForStarted(3000)) {
        QMessageBox::warning(this, "Wraith",
                             tr("Could not run the updater (curl.exe not found)."),
                             QMessageBox::Ok);
        return;
    }
    meta.waitForFinished(20000);

    const QByteArray data = meta.readAllStandardOutput();
    if (data.isEmpty()) {
        QMessageBox::warning(this, "Wraith",
                             tr("Failed to check for updates:\n%1")
                                 .arg(QString::fromLocal8Bit(meta.readAllStandardError()).trimmed()),
                             QMessageBox::Ok);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this, "Wraith",
                             tr("Failed to parse update information."),
                             QMessageBox::Ok);
        return;
    }

    const QJsonObject root = doc.object();
    QString tagName = root.value("tag_name").toString();
    if (tagName.isEmpty()) {
        // No published release (e.g. GitHub returns 404 "Not Found"). We still
        // reached GitHub successfully, so this is "nothing to update to".
        QMessageBox::information(this, "Wraith",
                                 tr("No update is available right now."),
                                 QMessageBox::Ok);
        return;
    }

    // Strip a leading 'v' / 'V'.
    QString remoteVersion = tagName;
    if (remoteVersion.startsWith('v', Qt::CaseInsensitive)) {
        remoteVersion.remove(0, 1);
    }

    const QString localVersion = QCoreApplication::applicationVersion();
    if (!isRemoteNewer(remoteVersion, localVersion)) {
        QMessageBox::information(this, "Wraith", tr("You're up to date"), QMessageBox::Ok);
        return;
    }

    // Find the first .exe asset (and its size, for the progress bar).
    QString downloadUrl;
    QString assetName;
    qint64 assetSize = 0;
    const QJsonArray assets = root.value("assets").toArray();
    for (const QJsonValue &assetVal : assets) {
        const QJsonObject asset = assetVal.toObject();
        const QString name = asset.value("name").toString();
        if (name.endsWith(".exe", Qt::CaseInsensitive)) {
            downloadUrl = asset.value("browser_download_url").toString();
            assetName = name;
            assetSize = asset.value("size").toVariant().toLongLong();
            break;
        }
    }

    if (downloadUrl.isEmpty()) {
        QMessageBox::warning(this, "Wraith",
                             tr("Version %1 is available, but no installer was found.").arg(remoteVersion),
                             QMessageBox::Ok);
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, "Wraith",
        tr("Version %1 is available, download and install now?").arg(remoteVersion),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    // --- 2) download the installer (curl follows the GitHub CDN redirect) ---
    const QString installerPath = QDir(QDir::tempPath()).filePath(assetName);
    QFile::remove(installerPath);

    QProcess dl;
    dl.start(curlExe, { "-sSL", "-H", "User-Agent: Wraith-Updater",
                        "-o", installerPath, downloadUrl });
    if (!dl.waitForStarted(5000)) {
        QMessageBox::warning(this, "Wraith",
                             tr("Could not start the download."), QMessageBox::Ok);
        return;
    }

    QProgressDialog progress(tr("Downloading %1...").arg(assetName), tr("Cancel"),
                             0, assetSize > 0 ? 100 : 0, this);
    progress.setWindowTitle("Wraith");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);

    // Poll the output file size for progress while curl runs.
    while (dl.state() != QProcess::NotRunning) {
        if (progress.wasCanceled()) {
            dl.kill();
            dl.waitForFinished(2000);
            QFile::remove(installerPath);
            return;
        }
        if (assetSize > 0) {
            const qint64 have = QFileInfo(installerPath).size();
            progress.setValue(static_cast<int>(qBound<qint64>(0, have * 100 / assetSize, 100)));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        dl.waitForFinished(100);
    }
    progress.setValue(progress.maximum());

    if (dl.exitStatus() != QProcess::NormalExit || dl.exitCode() != 0) {
        QMessageBox::warning(this, "Wraith",
                             tr("Download failed:\n%1")
                                 .arg(QString::fromLocal8Bit(dl.readAllStandardError()).trimmed()),
                             QMessageBox::Ok);
        QFile::remove(installerPath);
        return;
    }

    const QFileInfo dlInfo(installerPath);
    if (!dlInfo.exists() || dlInfo.size() <= 0) {
        QMessageBox::warning(this, "Wraith",
                             tr("The download produced no file."), QMessageBox::Ok);
        return;
    }

    if (!QProcess::startDetached(installerPath, QStringList())) {
        QMessageBox::warning(this, "Wraith",
                             tr("Failed to launch the installer:\n%1").arg(installerPath),
                             QMessageBox::Ok);
        return;
    }

    qApp->quit();
}
