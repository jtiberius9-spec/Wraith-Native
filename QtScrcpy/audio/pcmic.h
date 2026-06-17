#ifndef PCMIC_H
#define PCMIC_H

#include <QObject>
#include <QString>

class QAudioInput;
class QFile;
class QIODevice;

// Captures the PC's default audio input device as raw PCM (s16le / 48000 Hz /
// 1 channel mono) and writes it to a file, for muxing into a screen recording.
// The phone microphone is untouched; this only taps the local PC mic.
//
// Robust by design: if there is no input device or the format is unsupported,
// start() logs a warning and returns false; recording should continue with game
// audio only.
class PCMic : public QObject
{
    Q_OBJECT
public:
    explicit PCMic(QObject *parent = nullptr);
    ~PCMic();

    // Begin capturing to pcmPath (raw s16le/48k/mono). Returns false (and logs)
    // if no usable input device/format is available.
    bool start(const QString &pcmPath);
    void stop();

private:
    QAudioInput *m_audioInput = nullptr;
    QFile *m_file = nullptr;
    QIODevice *m_inputDevice = nullptr; // device the QAudioInput pulls from (not owned)
};

#endif // PCMIC_H
