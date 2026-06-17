#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QThread>
#include <QProcess>
#include <QPointer>
#include <QVector>
#include <QString>
#include <QMutex>

class QAudioSink;
class QAudioOutput;
class QIODevice;
class QFile;
class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    bool start(const QString& serial, int port);
    void stop();
    void installonly(const QString& serial, int port);

    // Tee the raw game PCM (s16le / 48000 Hz / stereo, the same bytes fed to the
    // output device after the codec-id prefix is stripped) to a file. Call
    // startTee() right before start(); the file is opened when audio output
    // actually begins so the captured PCM aligns with playback. Thread-safe for
    // the single worker thread the readyRead handler runs on.
    void startTee(const QString& pcmPath);
    void stopTee();

private:
    // Launch a tiny audio-only scrcpy-server instance streaming raw PCM over an
    // adb-forwarded socket (replaces the sndcpy app — nothing is installed on
    // the phone, the pushed scrcpy-server.jar is reused).
    bool runAudioServer(const QString& serial, int port);
    void startAudioOutput();
    void stopAudioOutput();
    void startRecvData(int port);
    void stopRecvData();

signals:
    void connectTo(int port);

private:
    QPointer<QIODevice> m_outputDevice;
    QThread m_workerThread;
    QProcess m_serverProcess;   // long-running audio-only scrcpy-server
    QVector<char> m_buffer;
    bool m_running = false;
    int m_skipPrefix = 0;       // bytes still to drop (forward dummy + codec id)

    // game-audio tee (recording)
    QMutex m_teeMutex;
    QString m_teePath;          // requested tee path (set before start())
    QFile* m_teeFile = nullptr; // open tee file, owned here
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput* m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
};

#endif // AUDIOOUTPUT_H
