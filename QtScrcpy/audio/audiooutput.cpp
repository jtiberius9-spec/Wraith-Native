#include <QAudioOutput>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTime>
#include <QFile>
#include <QMutexLocker>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QAudioDevice>
#include <QMediaDevices>
#endif

#include "audiooutput.h"

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
{
    m_running = false;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    m_audioOutput = nullptr;
#else
    m_audioSink = nullptr;
#endif
    connect(&m_serverProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        qInfo() << QString("AudioServer::") << QString(m_serverProcess.readAllStandardOutput());
    });
    connect(&m_serverProcess, &QProcess::readyReadStandardError, this, [this]() {
        qInfo() << QString("AudioServer::") << QString(m_serverProcess.readAllStandardError());
    });
}

AudioOutput::~AudioOutput()
{
    if (QProcess::NotRunning != m_serverProcess.state()) {
        m_serverProcess.kill();
    }
    stop();
}

bool AudioOutput::start(const QString& serial, int port)
{
    if (m_running) {
        stop();
    }

    QElapsedTimer timeConsumeCount;
    timeConsumeCount.start();
    bool ret = runAudioServer(serial, port);
    qInfo() << "AudioOutput::start audio server cost:" << timeConsumeCount.elapsed() << "milliseconds";
    if (!ret) {
        return ret;
    }

    startAudioOutput();

    // If a tee path was requested just before start(), open it now so the
    // captured PCM begins as close as possible to playback/record start.
    {
        QMutexLocker locker(&m_teeMutex);
        if (!m_teePath.isEmpty() && !m_teeFile) {
            auto* f = new QFile(m_teePath);
            if (f->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                m_teeFile = f;
                qInfo() << "AudioOutput::game-audio tee started:" << m_teePath;
            } else {
                qWarning() << "AudioOutput::failed to open game-audio tee file:" << m_teePath;
                delete f;
            }
        }
    }

    startRecvData(port);

    m_running = true;
    return true;
}

void AudioOutput::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;

    stopRecvData();
    stopAudioOutput();

    // close the game-audio tee (worker thread is stopped, so no concurrent write)
    {
        QMutexLocker locker(&m_teeMutex);
        if (m_teeFile) {
            m_teeFile->flush();
            m_teeFile->close();
            delete m_teeFile;
            m_teeFile = nullptr;
        }
        m_teePath.clear();
    }

    if (QProcess::NotRunning != m_serverProcess.state()) {
        m_serverProcess.kill();
        m_serverProcess.waitForFinished(300);
    }
}

void AudioOutput::startTee(const QString &pcmPath)
{
    QMutexLocker locker(&m_teeMutex);
    m_teePath = pcmPath;
    // If audio playback is already running (the common case for a mid-session
    // F12 record start, where start() ran at connect), open the tee file now so
    // the readyRead worker begins capturing immediately. Otherwise start() opens
    // it when playback begins. The worker thread guards m_teeFile with m_teeMutex.
    if (m_running && !m_teePath.isEmpty() && !m_teeFile) {
        auto* f = new QFile(m_teePath);
        if (f->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_teeFile = f;
            qInfo() << "AudioOutput::game-audio tee started (mid-session):" << m_teePath;
        } else {
            qWarning() << "AudioOutput::failed to open game-audio tee file:" << m_teePath;
            delete f;
        }
    }
}

void AudioOutput::stopTee()
{
    QMutexLocker locker(&m_teeMutex);
    if (m_teeFile) {
        m_teeFile->flush();
        m_teeFile->close();
        delete m_teeFile;
        m_teeFile = nullptr;
    }
    m_teePath.clear();
}

void AudioOutput::installonly(const QString &serial, int port)
{
    // Native audio needs no app installed on the phone (it reuses the pushed
    // scrcpy-server.jar). Kept as a no-op for GUI compatibility.
    Q_UNUSED(serial)
    Q_UNUSED(port)
    qInfo() << "AudioOutput::install not needed (native audio uses scrcpy-server)";
}

static QString adbExecutable()
{
    QString adb = QCoreApplication::applicationDirPath() + "/adb";
#ifdef Q_OS_WIN
    adb += ".exe";
#endif
    return adb;
}

bool AudioOutput::runAudioServer(const QString &serial, int port)
{
    if (QProcess::NotRunning != m_serverProcess.state()) {
        m_serverProcess.kill();
        m_serverProcess.waitForFinished(300);
    }

    const QString adb = adbExecutable();
    const QString scid = "0a0d100c";                 // fixed, distinct from the video server
    const QString socketName = "localabstract:scrcpy_" + scid;
    const QString remoteJar = "/data/local/tmp/qtscrcpy-audio-server.jar";
    const QString localJar = QCoreApplication::applicationDirPath() + "/scrcpy-server";

    // push our own copy of the scrcpy-server jar to a dedicated path. The main
    // video server pushes to a config-driven path we can't assume, so the audio
    // server stays self-contained (still just a pushed jar, no installed app).
    {
        QProcess push;
        push.start(adb, { "-s", serial, "push", localJar, remoteJar });
        if (!push.waitForFinished(8000) || 0 != push.exitCode()) {
            qWarning() << "AudioOutput::adb push audio server failed:" << push.readAllStandardError();
            return false;
        }
    }

    // forward a local TCP port to the device-side abstract socket
    {
        QProcess fwd;
        fwd.start(adb, { "-s", serial, "forward", QString("tcp:%1").arg(port), socketName });
        if (!fwd.waitForFinished(3000) || 0 != fwd.exitCode()) {
            qWarning() << "AudioOutput::adb forward failed:" << fwd.readAllStandardError();
            return false;
        }
    }

    // audio-only scrcpy-server: raw PCM, no video/control, minimal framing.
    QStringList args{
        "-s", serial, "shell",
        "CLASSPATH=" + remoteJar,
        "app_process", "/", "com.genymobile.scrcpy.Server", "3.3.3",
        "scid=" + scid, "log_level=error",
        "audio=true", "video=false", "control=false",
        "audio_codec=raw", "tunnel_forward=true",
        "send_device_meta=false", "send_frame_meta=false",
    };
    m_serverProcess.start(adb, args);
    if (!m_serverProcess.waitForStarted(2000)) {
        qWarning() << "AudioOutput::audio server failed to start";
        return false;
    }

    // forward mode sends 1 dummy byte; the raw audio stream then leads with a
    // 4-byte codec id. Drop both before feeding PCM to the output device.
    m_skipPrefix = 5;

    // give the device server a moment to start listening before we connect
    QThread::msleep(500);
    return true;
}

void AudioOutput::startAudioOutput()
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) {
        return;
    }

    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());

    if (!info.isFormatSupported(format)) {
        qWarning() << "AudioOutput::audio format not supported, cannot play audio.";
        return;
    }

    m_audioOutput = new QAudioOutput(format, this);
    connect(m_audioOutput, &QAudioOutput::stateChanged, this, [](QAudio::State state) {
        qInfo() << "AudioOutput::audio state changed:" << state;
    });
    m_audioOutput->setBufferSize(48000*2*15/1000 * 20);
    m_outputDevice = m_audioOutput->start();
#else
    if (m_audioSink) {
        return;
    }

    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
    if (!defaultDevice.isFormatSupported(format)) {
        qWarning() << "AudioOutput::audio format not supported, cannot play audio.";
        return;
    }
    m_audioSink = new QAudioSink(defaultDevice, format, this);
    m_outputDevice = m_audioSink->start();
    if (!m_outputDevice) {
        qWarning() << "AudioOutput::audio output device not available, cannot play audio.";
        delete m_audioSink;
        m_audioSink = nullptr;
        return;
    }
#endif
}

void AudioOutput::stopAudioOutput()
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) {
        m_audioOutput->stop();
        delete m_audioOutput;
        m_audioOutput = nullptr;
    }
#else
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
#endif
    m_outputDevice = nullptr;
}

void AudioOutput::startRecvData(int port)
{
    if (m_workerThread.isRunning()) {
        stopRecvData();
    }

    auto audioSocket = new QTcpSocket();
    audioSocket->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, audioSocket, &QObject::deleteLater);

    connect(this, &AudioOutput::connectTo, audioSocket, [audioSocket](int port) {
        audioSocket->connectToHost(QHostAddress::LocalHost, port);
        if (!audioSocket->waitForConnected(500)) {
            qWarning("AudioOutput::audio socket connect failed");
            return;
        }
        qInfo("AudioOutput::audio socket connect success");
    });
    connect(audioSocket, &QIODevice::readyRead, audioSocket, [this, audioSocket]() {
        QByteArray data = audioSocket->readAll();
        if (m_skipPrefix > 0) {
            int drop = qMin(m_skipPrefix, static_cast<int>(data.size()));
            data.remove(0, drop);
            m_skipPrefix -= drop;
            if (data.isEmpty()) {
                return;
            }
        }
        // Tee post-prefix raw PCM (s16le/48k/stereo) to disk for recording. This
        // runs even when no playback device is available so a recording still
        // captures game audio.
        {
            QMutexLocker locker(&m_teeMutex);
            if (m_teeFile) {
                m_teeFile->write(data.constData(), data.size());
            }
        }
        if (!m_outputDevice) {
            return;   // output not ready -> already drained the socket (and teed)
        }
        m_outputDevice->write(data.constData(), data.size());
    });
    connect(audioSocket, &QTcpSocket::stateChanged, audioSocket, [](QAbstractSocket::SocketState state) {
        qInfo() << "AudioOutput::audio socket state changed:" << state;

    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(audioSocket, &QTcpSocket::errorOccurred, audioSocket, [](QAbstractSocket::SocketError error) {
        qInfo() << "AudioOutput::audio socket error occurred:" << error;
    });
#else
    connect(audioSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), audioSocket, [](QAbstractSocket::SocketError error) {
        qInfo() << "AudioOutput::audio socket error occurred:" << error;
    });
#endif

    m_workerThread.start();
    emit connectTo(port);
}

void AudioOutput::stopRecvData()
{
    if (!m_workerThread.isRunning()) {
        return;
    }

    m_workerThread.quit();
    m_workerThread.wait();
}
