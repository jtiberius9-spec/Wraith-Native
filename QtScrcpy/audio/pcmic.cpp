#include <QAudioInput>
#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QFile>
#include <QDebug>

#include "pcmic.h"

PCMic::PCMic(QObject *parent)
    : QObject(parent)
{
}

PCMic::~PCMic()
{
    stop();
}

bool PCMic::start(const QString &pcmPath)
{
    if (m_audioInput) {
        stop();
    }

    // raw s16le / 48000 Hz / mono, matching the mux expectations
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(1);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    QAudioDeviceInfo info = QAudioDeviceInfo::defaultInputDevice();
    if (info.isNull()) {
        qWarning() << "PCMic::no default input device, skipping mic capture";
        return false;
    }
    if (!info.isFormatSupported(format)) {
        qWarning() << "PCMic::s16le/48k/mono not supported by input device, skipping mic capture";
        return false;
    }

    m_file = new QFile(pcmPath);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "PCMic::failed to open mic pcm file:" << pcmPath;
        delete m_file;
        m_file = nullptr;
        return false;
    }

    m_audioInput = new QAudioInput(info, format, this);
    // pull-mode: QAudioInput writes captured PCM straight into the QFile
    m_audioInput->start(m_file);

    if (QAudio::StoppedState == m_audioInput->state() && QAudio::NoError != m_audioInput->error()) {
        qWarning() << "PCMic::failed to start mic capture, error:" << m_audioInput->error();
        stop();
        return false;
    }

    qInfo() << "PCMic::mic capture started:" << pcmPath << "from device:" << info.deviceName();
    return true;
}

void PCMic::stop()
{
    if (m_audioInput) {
        m_audioInput->stop();
        delete m_audioInput;
        m_audioInput = nullptr;
    }
    m_inputDevice = nullptr;
    if (m_file) {
        m_file->flush();
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
}
