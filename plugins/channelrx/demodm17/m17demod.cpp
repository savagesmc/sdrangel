///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2022 F4EXB                                                      //
// written by Edouard Griffiths                                                  //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////


#include <string.h>
#include <stdio.h>
#include <complex.h>

#include <QTime>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QBuffer>
#include <QThread>

#include "SWGChannelSettings.h"
#include "SWGWorkspaceInfo.h"
#include "SWGChannelReport.h"

#include "dsp/dspengine.h"
#include "dsp/dspcommands.h"
#include "device/deviceapi.h"
#include "feature/feature.h"
#include "feature/featureset.h"
#include "settings/serializable.h"
#include "util/db.h"
#include "maincore.h"

#include "m17demod.h"

MESSAGE_CLASS_DEFINITION(M17Demod::MsgConfigureM17Demod, Message)
MESSAGE_CLASS_DEFINITION(M17Demod::MsgReportSMS, Message)

const char* const M17Demod::m_channelIdURI = "sdrangel.channel.m17demod";
const char* const M17Demod::m_channelId = "M17Demod";
const int M17Demod::m_udpBlockSize = 512;

M17Demod::M17Demod(DeviceAPI *deviceAPI) :
        ChannelAPI(m_channelIdURI, ChannelAPI::StreamSingleSink),
        m_deviceAPI(deviceAPI),
        m_basebandSampleRate(0)
{
    qDebug("M17Demod::M17Demod");
	setObjectName(m_channelId);

    m_thread = new QThread(this);
    m_basebandSink = new M17DemodBaseband();
    m_basebandSink->setChannel(this);
    m_basebandSink->setDemodInputMessageQueue(&m_inputMessageQueue);
    m_basebandSink->moveToThread(m_thread);

    applySettings(m_settings, true);

    m_deviceAPI->addChannelSink(this);
    m_deviceAPI->addChannelSinkAPI(this);

    m_networkManager = new QNetworkAccessManager();
    QObject::connect(
        m_networkManager,
        &QNetworkAccessManager::finished,
        this,
        &M17Demod::networkManagerFinished
    );
    QObject::connect(
        this,
        &ChannelAPI::indexInDeviceSetChanged,
        this,
        &M17Demod::handleIndexInDeviceSetChanged
    );
}

M17Demod::~M17Demod()
{
    QObject::disconnect(
        m_networkManager,
        &QNetworkAccessManager::finished,
        this,
        &M17Demod::networkManagerFinished
    );
    delete m_networkManager;
	m_deviceAPI->removeChannelSinkAPI(this);
    m_deviceAPI->removeChannelSink(this);
    delete m_basebandSink;
    delete m_thread;
}

void M17Demod::setDeviceAPI(DeviceAPI *deviceAPI)
{
    if (deviceAPI != m_deviceAPI)
    {
        m_deviceAPI->removeChannelSinkAPI(this);
        m_deviceAPI->removeChannelSink(this);
        m_deviceAPI = deviceAPI;
        m_deviceAPI->addChannelSink(this);
        m_deviceAPI->addChannelSinkAPI(this);
    }
}

uint32_t M17Demod::getNumberOfDeviceStreams() const
{
    return m_deviceAPI->getNbSourceStreams();
}

void M17Demod::feed(const SampleVector::const_iterator& begin, const SampleVector::const_iterator& end, bool firstOfBurst)
{
    (void) firstOfBurst;
    m_basebandSink->feed(begin, end);
}

void M17Demod::start()
{
    qDebug() << "M17Demod::start";

    if (m_basebandSampleRate != 0) {
        m_basebandSink->setBasebandSampleRate(m_basebandSampleRate);
    }

    m_basebandSink->reset();
    m_thread->start();
}

void M17Demod::stop()
{
    qDebug() << "M17Demod::stop";
	m_thread->exit();
	m_thread->wait();
}

bool M17Demod::handleMessage(const Message& cmd)
{
	qDebug() << "M17Demod::handleMessage";

    if (MsgConfigureM17Demod::match(cmd))
    {
        MsgConfigureM17Demod& cfg = (MsgConfigureM17Demod&) cmd;
        qDebug("M17Demod::handleMessage: MsgConfigureM17Demod");
        applySettings(cfg.getSettings(), cfg.getForce());

        return true;
    }
    else if (DSPSignalNotification::match(cmd))
    {
        DSPSignalNotification& notif = (DSPSignalNotification&) cmd;
        m_basebandSampleRate = notif.getSampleRate();
        // Forward to the sink
        DSPSignalNotification* rep = new DSPSignalNotification(notif); // make a copy
        qDebug() << "M17Demod::handleMessage: DSPSignalNotification";
        m_basebandSink->getInputMessageQueue()->push(rep);
        // Forward to GUI if any
        if (getMessageQueueToGUI()) {
            getMessageQueueToGUI()->push(new DSPSignalNotification(notif));
        }

	    return true;
    }
    else if (MainCore::MsgChannelDemodQuery::match(cmd))
    {
        qDebug() << "M17Demod::handleMessage: MsgChannelDemodQuery";
        sendSampleRateToDemodAnalyzer();

        return true;
    }
    else if (MsgReportSMS::match(cmd))
    {
        MsgReportSMS& report = (MsgReportSMS&) cmd;
        // Forward to GUI if any
        if (getMessageQueueToGUI()) {
            getMessageQueueToGUI()->push(new MsgReportSMS(report));
        }

        return true;
    }
	else
	{
		return false;
	}
}

void M17Demod::setCenterFrequency(qint64 frequency)
{
    M17DemodSettings settings = m_settings;
    settings.m_inputFrequencyOffset = frequency;
    applySettings(settings, false);

    if (m_guiMessageQueue) // forward to GUI if any
    {
        MsgConfigureM17Demod *msgToGUI = MsgConfigureM17Demod::create(settings, false);
        m_guiMessageQueue->push(msgToGUI);
    }
}

void M17Demod::applySettings(const M17DemodSettings& settings, bool force)
{
    qDebug() << "M17Demod::applySettings: "
            << " m_inputFrequencyOffset: " << settings.m_inputFrequencyOffset
            << " m_rfBandwidth: " << settings.m_rfBandwidth
            << " m_fmDeviation: " << settings.m_fmDeviation
            << " m_volume: " << settings.m_volume
            << " m_baudRate: " << settings.m_baudRate
            << " m_squelchGate" << settings.m_squelchGate
            << " m_squelch: " << settings.m_squelch
            << " m_audioMute: " << settings.m_audioMute
            << " m_syncOrConstellation: " << settings.m_syncOrConstellation
            << " m_highPassFilter: "<< settings.m_highPassFilter
            << " m_audioDeviceName: " << settings.m_audioDeviceName
            << " m_traceLengthMutliplier: " << settings.m_traceLengthMutliplier
            << " m_traceStroke: " << settings.m_traceStroke
            << " m_traceDecay: " << settings.m_traceDecay
            << " m_streamIndex: " << settings.m_streamIndex
            << " force: " << force;

    QList<QString> reverseAPIKeys;

    if ((settings.m_inputFrequencyOffset != m_settings.m_inputFrequencyOffset) || force) {
        reverseAPIKeys.append("inputFrequencyOffset");
    }
    if ((settings.m_audioMute != m_settings.m_audioMute) || force) {
        reverseAPIKeys.append("audioMute");
    }
    if ((settings.m_syncOrConstellation != m_settings.m_syncOrConstellation) || force) {
        reverseAPIKeys.append("syncOrConstellation");
    }
    if ((settings.m_traceLengthMutliplier != m_settings.m_traceLengthMutliplier) || force) {
        reverseAPIKeys.append("traceLengthMutliplier");
    }
    if ((settings.m_rfBandwidth != m_settings.m_rfBandwidth) || force) {
        reverseAPIKeys.append("rfBandwidth");
    }
    if ((settings.m_fmDeviation != m_settings.m_fmDeviation) || force) {
        reverseAPIKeys.append("fmDeviation");
    }
    if ((settings.m_squelchGate != m_settings.m_squelchGate) || force) {
        reverseAPIKeys.append("squelchGate");
    }
    if ((settings.m_squelch != m_settings.m_squelch) || force) {
        reverseAPIKeys.append("squelch");
    }
    if ((settings.m_volume != m_settings.m_volume) || force) {
        reverseAPIKeys.append("volume");
    }
    if ((settings.m_baudRate != m_settings.m_baudRate) || force) {
        reverseAPIKeys.append("baudRate");
    }
    if ((settings.m_highPassFilter != m_settings.m_highPassFilter) || force) {
        reverseAPIKeys.append("highPassFilter");
    }
    if ((settings.m_audioDeviceName != m_settings.m_audioDeviceName) || force) {
        reverseAPIKeys.append("audioDeviceName");
    }

    if (m_settings.m_streamIndex != settings.m_streamIndex)
    {
        if (m_deviceAPI->getSampleMIMO()) // change of stream is possible for MIMO devices only
        {
            m_deviceAPI->removeChannelSinkAPI(this);
            m_deviceAPI->removeChannelSink(this, m_settings.m_streamIndex);
            m_deviceAPI->addChannelSink(this, settings.m_streamIndex);
            m_deviceAPI->addChannelSinkAPI(this);
        }

        reverseAPIKeys.append("streamIndex");
    }

    M17DemodBaseband::MsgConfigureM17DemodBaseband *msg = M17DemodBaseband::MsgConfigureM17DemodBaseband::create(settings, force);
    m_basebandSink->getInputMessageQueue()->push(msg);

    if (settings.m_useReverseAPI)
    {
        bool fullUpdate = ((m_settings.m_useReverseAPI != settings.m_useReverseAPI) && settings.m_useReverseAPI) ||
                (m_settings.m_reverseAPIAddress != settings.m_reverseAPIAddress) ||
                (m_settings.m_reverseAPIPort != settings.m_reverseAPIPort) ||
                (m_settings.m_reverseAPIDeviceIndex != settings.m_reverseAPIDeviceIndex) ||
                (m_settings.m_reverseAPIChannelIndex != settings.m_reverseAPIChannelIndex);
        webapiReverseSendSettings(reverseAPIKeys, settings, fullUpdate || force);
    }

    QList<ObjectPipe*> pipes;
    MainCore::instance()->getMessagePipes().getMessagePipes(this, "settings", pipes);

    if (pipes.size() > 0) {
        sendChannelSettings(pipes, reverseAPIKeys, settings, force);
    }

    m_settings = settings;
}

QByteArray M17Demod::serialize() const
{
    return m_settings.serialize();
}

bool M17Demod::deserialize(const QByteArray& data)
{
    if (m_settings.deserialize(data))
    {
        MsgConfigureM17Demod *msg = MsgConfigureM17Demod::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return true;
    }
    else
    {
        m_settings.resetToDefaults();
        MsgConfigureM17Demod *msg = MsgConfigureM17Demod::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return false;
    }
}

void M17Demod::sendSampleRateToDemodAnalyzer()
{
    QList<ObjectPipe*> pipes;
    MainCore::instance()->getMessagePipes().getMessagePipes(this, "reportdemod", pipes);

    if (pipes.size() > 0)
    {
        for (const auto& pipe : pipes)
        {
            MessageQueue *messageQueue = qobject_cast<MessageQueue*>(pipe->m_element);
            MainCore::MsgChannelDemodReport *msg = MainCore::MsgChannelDemodReport::create(
                this,
                getAudioSampleRate()
            );
            messageQueue->push(msg);
        }
    }
}

int M17Demod::webapiSettingsGet(
        SWGSDRangel::SWGChannelSettings& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setM17DemodSettings(new SWGSDRangel::SWGM17DemodSettings());
    response.getM17DemodSettings()->init();
    webapiFormatChannelSettings(response, m_settings);
    return 200;
}

int M17Demod::webapiWorkspaceGet(
        SWGSDRangel::SWGWorkspaceInfo& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setIndex(m_settings.m_workspaceIndex);
    return 200;
}

int M17Demod::webapiSettingsPutPatch(
        bool force,
        const QStringList& channelSettingsKeys,
        SWGSDRangel::SWGChannelSettings& response,
        QString& errorMessage)
{
    (void) errorMessage;
    M17DemodSettings settings = m_settings;
    webapiUpdateChannelSettings(settings, channelSettingsKeys, response);

    MsgConfigureM17Demod *msg = MsgConfigureM17Demod::create(settings, force);
    m_inputMessageQueue.push(msg);

    qDebug("M17Demod::webapiSettingsPutPatch: forward to GUI: %p", m_guiMessageQueue);
    if (m_guiMessageQueue) // forward to GUI if any
    {
        MsgConfigureM17Demod *msgToGUI = MsgConfigureM17Demod::create(settings, force);
        m_guiMessageQueue->push(msgToGUI);
    }

    webapiFormatChannelSettings(response, settings);

    return 200;
}

void M17Demod::webapiUpdateChannelSettings(
        M17DemodSettings& settings,
        const QStringList& channelSettingsKeys,
        SWGSDRangel::SWGChannelSettings& response)
{
    if (channelSettingsKeys.contains("inputFrequencyOffset")) {
        settings.m_inputFrequencyOffset = response.getM17DemodSettings()->getInputFrequencyOffset();
    }
    if (channelSettingsKeys.contains("rfBandwidth")) {
        settings.m_rfBandwidth = response.getM17DemodSettings()->getRfBandwidth();
    }
    if (channelSettingsKeys.contains("fmDeviation")) {
        settings.m_fmDeviation = response.getM17DemodSettings()->getFmDeviation();
    }
    if (channelSettingsKeys.contains("volume")) {
        settings.m_volume = response.getM17DemodSettings()->getVolume();
    }
    if (channelSettingsKeys.contains("baudRate")) {
        settings.m_baudRate = response.getM17DemodSettings()->getBaudRate();
    }
    if (channelSettingsKeys.contains("squelchGate")) {
        settings.m_squelchGate = response.getM17DemodSettings()->getSquelchGate();
    }
    if (channelSettingsKeys.contains("squelch")) {
        settings.m_squelch = response.getM17DemodSettings()->getSquelch();
    }
    if (channelSettingsKeys.contains("audioMute")) {
        settings.m_audioMute = response.getM17DemodSettings()->getAudioMute() != 0;
    }
    if (channelSettingsKeys.contains("syncOrConstellation")) {
        settings.m_syncOrConstellation = response.getM17DemodSettings()->getSyncOrConstellation() != 0;
    }
    if (channelSettingsKeys.contains("rgbColor")) {
        settings.m_rgbColor = response.getM17DemodSettings()->getRgbColor();
    }
    if (channelSettingsKeys.contains("title")) {
        settings.m_title = *response.getM17DemodSettings()->getTitle();
    }
    if (channelSettingsKeys.contains("audioDeviceName")) {
        settings.m_audioDeviceName = *response.getM17DemodSettings()->getAudioDeviceName();
    }
    if (channelSettingsKeys.contains("highPassFilter")) {
        settings.m_highPassFilter = response.getM17DemodSettings()->getHighPassFilter() != 0;
    }
    if (channelSettingsKeys.contains("traceLengthMutliplier")) {
        settings.m_traceLengthMutliplier = response.getM17DemodSettings()->getTraceLengthMutliplier();
    }
    if (channelSettingsKeys.contains("traceStroke")) {
        settings.m_traceStroke = response.getM17DemodSettings()->getTraceStroke();
    }
    if (channelSettingsKeys.contains("traceDecay")) {
        settings.m_traceDecay = response.getM17DemodSettings()->getTraceDecay();
    }
    if (channelSettingsKeys.contains("streamIndex")) {
        settings.m_streamIndex = response.getM17DemodSettings()->getStreamIndex();
    }
    if (channelSettingsKeys.contains("useReverseAPI")) {
        settings.m_useReverseAPI = response.getM17DemodSettings()->getUseReverseApi() != 0;
    }
    if (channelSettingsKeys.contains("reverseAPIAddress")) {
        settings.m_reverseAPIAddress = *response.getM17DemodSettings()->getReverseApiAddress();
    }
    if (channelSettingsKeys.contains("reverseAPIPort")) {
        settings.m_reverseAPIPort = response.getM17DemodSettings()->getReverseApiPort();
    }
    if (channelSettingsKeys.contains("reverseAPIDeviceIndex")) {
        settings.m_reverseAPIDeviceIndex = response.getM17DemodSettings()->getReverseApiDeviceIndex();
    }
    if (channelSettingsKeys.contains("reverseAPIChannelIndex")) {
        settings.m_reverseAPIChannelIndex = response.getM17DemodSettings()->getReverseApiChannelIndex();
    }
    if (settings.m_channelMarker && channelSettingsKeys.contains("channelMarker")) {
        settings.m_channelMarker->updateFrom(channelSettingsKeys, response.getM17DemodSettings()->getChannelMarker());
    }
    if (settings.m_rollupState && channelSettingsKeys.contains("rollupState")) {
        settings.m_rollupState->updateFrom(channelSettingsKeys, response.getM17DemodSettings()->getRollupState());
    }
}

int M17Demod::webapiReportGet(
        SWGSDRangel::SWGChannelReport& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setM17DemodReport(new SWGSDRangel::SWGM17DemodReport());
    response.getM17DemodReport()->init();
    webapiFormatChannelReport(response);
    return 200;
}

void M17Demod::webapiFormatChannelSettings(SWGSDRangel::SWGChannelSettings& response, const M17DemodSettings& settings)
{
    response.getM17DemodSettings()->setInputFrequencyOffset(settings.m_inputFrequencyOffset);
    response.getM17DemodSettings()->setRfBandwidth(settings.m_rfBandwidth);
    response.getM17DemodSettings()->setFmDeviation(settings.m_fmDeviation);
    response.getM17DemodSettings()->setVolume(settings.m_volume);
    response.getM17DemodSettings()->setBaudRate(settings.m_baudRate);
    response.getM17DemodSettings()->setSquelchGate(settings.m_squelchGate);
    response.getM17DemodSettings()->setSquelch(settings.m_squelch);
    response.getM17DemodSettings()->setAudioMute(settings.m_audioMute ? 1 : 0);
    response.getM17DemodSettings()->setSyncOrConstellation(settings.m_syncOrConstellation ? 1 : 0);
    response.getM17DemodSettings()->setRgbColor(settings.m_rgbColor);

    if (response.getM17DemodSettings()->getTitle()) {
        *response.getM17DemodSettings()->getTitle() = settings.m_title;
    } else {
        response.getM17DemodSettings()->setTitle(new QString(settings.m_title));
    }

    if (response.getM17DemodSettings()->getAudioDeviceName()) {
        *response.getM17DemodSettings()->getAudioDeviceName() = settings.m_audioDeviceName;
    } else {
        response.getM17DemodSettings()->setAudioDeviceName(new QString(settings.m_audioDeviceName));
    }

    response.getM17DemodSettings()->setHighPassFilter(settings.m_highPassFilter ? 1 : 0);
    response.getM17DemodSettings()->setTraceLengthMutliplier(settings.m_traceLengthMutliplier);
    response.getM17DemodSettings()->setTraceStroke(settings.m_traceStroke);
    response.getM17DemodSettings()->setTraceDecay(settings.m_traceDecay);
    response.getM17DemodSettings()->setStreamIndex(settings.m_streamIndex);
    response.getM17DemodSettings()->setUseReverseApi(settings.m_useReverseAPI ? 1 : 0);

    if (response.getM17DemodSettings()->getReverseApiAddress()) {
        *response.getM17DemodSettings()->getReverseApiAddress() = settings.m_reverseAPIAddress;
    } else {
        response.getM17DemodSettings()->setReverseApiAddress(new QString(settings.m_reverseAPIAddress));
    }

    response.getM17DemodSettings()->setReverseApiPort(settings.m_reverseAPIPort);
    response.getM17DemodSettings()->setReverseApiDeviceIndex(settings.m_reverseAPIDeviceIndex);
    response.getM17DemodSettings()->setReverseApiChannelIndex(settings.m_reverseAPIChannelIndex);

    if (settings.m_channelMarker)
    {
        if (response.getM17DemodSettings()->getChannelMarker())
        {
            settings.m_channelMarker->formatTo(response.getM17DemodSettings()->getChannelMarker());
        }
        else
        {
            SWGSDRangel::SWGChannelMarker *swgChannelMarker = new SWGSDRangel::SWGChannelMarker();
            settings.m_channelMarker->formatTo(swgChannelMarker);
            response.getM17DemodSettings()->setChannelMarker(swgChannelMarker);
        }
    }

    if (settings.m_rollupState)
    {
        if (response.getM17DemodSettings()->getRollupState())
        {
            settings.m_rollupState->formatTo(response.getM17DemodSettings()->getRollupState());
        }
        else
        {
            SWGSDRangel::SWGRollupState *swgRollupState = new SWGSDRangel::SWGRollupState();
            settings.m_rollupState->formatTo(swgRollupState);
            response.getM17DemodSettings()->setRollupState(swgRollupState);
        }
    }
}

void M17Demod::webapiFormatChannelReport(SWGSDRangel::SWGChannelReport& response)
{
    double magsqAvg, magsqPeak;
    int nbMagsqSamples;
    getMagSqLevels(magsqAvg, magsqPeak, nbMagsqSamples);

    response.getM17DemodReport()->setChannelPowerDb(CalcDb::dbPower(magsqAvg));
    response.getM17DemodReport()->setAudioSampleRate(m_basebandSink->getAudioSampleRate());
    response.getM17DemodReport()->setChannelSampleRate(m_basebandSink->getChannelSampleRate());
    response.getM17DemodReport()->setSquelch(m_basebandSink->getSquelchOpen() ? 1 : 0);
}

void M17Demod::webapiReverseSendSettings(QList<QString>& channelSettingsKeys, const M17DemodSettings& settings, bool force)
{
    SWGSDRangel::SWGChannelSettings *swgChannelSettings = new SWGSDRangel::SWGChannelSettings();
    webapiFormatChannelSettings(channelSettingsKeys, swgChannelSettings, settings, force);

    QString channelSettingsURL = QString("http://%1:%2/sdrangel/deviceset/%3/channel/%4/settings")
            .arg(settings.m_reverseAPIAddress)
            .arg(settings.m_reverseAPIPort)
            .arg(settings.m_reverseAPIDeviceIndex)
            .arg(settings.m_reverseAPIChannelIndex);
    m_networkRequest.setUrl(QUrl(channelSettingsURL));
    m_networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QBuffer *buffer = new QBuffer();
    buffer->open((QBuffer::ReadWrite));
    buffer->write(swgChannelSettings->asJson().toUtf8());
    buffer->seek(0);

    // Always use PATCH to avoid passing reverse API settings
    QNetworkReply *reply = m_networkManager->sendCustomRequest(m_networkRequest, "PATCH", buffer);
    buffer->setParent(reply);

    delete swgChannelSettings;
}

void M17Demod::sendChannelSettings(
    const QList<ObjectPipe*>& pipes,
    QList<QString>& channelSettingsKeys,
    const M17DemodSettings& settings,
    bool force)
{
    for (const auto& pipe : pipes)
    {
        MessageQueue *messageQueue = qobject_cast<MessageQueue*>(pipe->m_element);

        if (messageQueue)
        {
            SWGSDRangel::SWGChannelSettings *swgChannelSettings = new SWGSDRangel::SWGChannelSettings();
            webapiFormatChannelSettings(channelSettingsKeys, swgChannelSettings, settings, force);
            MainCore::MsgChannelSettings *msg = MainCore::MsgChannelSettings::create(
                this,
                channelSettingsKeys,
                swgChannelSettings,
                force
            );
            messageQueue->push(msg);
        }
    }
}

void M17Demod::webapiFormatChannelSettings(
        QList<QString>& channelSettingsKeys,
        SWGSDRangel::SWGChannelSettings *swgChannelSettings,
        const M17DemodSettings& settings,
        bool force
)
{
    swgChannelSettings->setDirection(0); // Single sink (Rx)
    swgChannelSettings->setOriginatorChannelIndex(getIndexInDeviceSet());
    swgChannelSettings->setOriginatorDeviceSetIndex(getDeviceSetIndex());
    swgChannelSettings->setChannelType(new QString(m_channelId));
    swgChannelSettings->setM17DemodSettings(new SWGSDRangel::SWGM17DemodSettings());
    SWGSDRangel::SWGM17DemodSettings *swgM17DemodSettings = swgChannelSettings->getM17DemodSettings();

    // transfer data that has been modified. When force is on transfer all data except reverse API data

    if (channelSettingsKeys.contains("inputFrequencyOffset") || force) {
        swgM17DemodSettings->setInputFrequencyOffset(settings.m_inputFrequencyOffset);
    }
    if (channelSettingsKeys.contains("rfBandwidth") || force) {
        swgM17DemodSettings->setRfBandwidth(settings.m_rfBandwidth);
    }
    if (channelSettingsKeys.contains("fmDeviation") || force) {
        swgM17DemodSettings->setFmDeviation(settings.m_fmDeviation);
    }
    if (channelSettingsKeys.contains("volume") || force) {
        swgM17DemodSettings->setVolume(settings.m_volume);
    }
    if (channelSettingsKeys.contains("baudRate") || force) {
        swgM17DemodSettings->setBaudRate(settings.m_baudRate);
    }
    if (channelSettingsKeys.contains("squelchGate") || force) {
        swgM17DemodSettings->setSquelchGate(settings.m_squelchGate);
    }
    if (channelSettingsKeys.contains("squelch") || force) {
        swgM17DemodSettings->setSquelch(settings.m_squelch);
    }
    if (channelSettingsKeys.contains("audioMute") || force) {
        swgM17DemodSettings->setAudioMute(settings.m_audioMute ? 1 : 0);
    }
    if (channelSettingsKeys.contains("syncOrConstellation") || force) {
        swgM17DemodSettings->setSyncOrConstellation(settings.m_syncOrConstellation ? 1 : 0);
    }
    if (channelSettingsKeys.contains("rgbColor") || force) {
        swgM17DemodSettings->setRgbColor(settings.m_rgbColor);
    }
    if (channelSettingsKeys.contains("title") || force) {
        swgM17DemodSettings->setTitle(new QString(settings.m_title));
    }
    if (channelSettingsKeys.contains("audioDeviceName") || force) {
        swgM17DemodSettings->setAudioDeviceName(new QString(settings.m_audioDeviceName));
    }
    if (channelSettingsKeys.contains("highPassFilter") || force) {
        swgM17DemodSettings->setHighPassFilter(settings.m_highPassFilter ? 1 : 0);
    }
    if (channelSettingsKeys.contains("traceLengthMutliplier") || force) {
        swgM17DemodSettings->setTraceLengthMutliplier(settings.m_traceLengthMutliplier);
    }
    if (channelSettingsKeys.contains("traceStroke") || force) {
        swgM17DemodSettings->setTraceStroke(settings.m_traceStroke);
    }
    if (channelSettingsKeys.contains("traceDecay") || force) {
        swgM17DemodSettings->setTraceDecay(settings.m_traceDecay);
    }
    if (channelSettingsKeys.contains("streamIndex") || force) {
        swgM17DemodSettings->setStreamIndex(settings.m_streamIndex);
    }

    if (settings.m_channelMarker && (channelSettingsKeys.contains("channelMarker") || force))
    {
        SWGSDRangel::SWGChannelMarker *swgChannelMarker = new SWGSDRangel::SWGChannelMarker();
        settings.m_channelMarker->formatTo(swgChannelMarker);
        swgM17DemodSettings->setChannelMarker(swgChannelMarker);
    }

    if (settings.m_rollupState && (channelSettingsKeys.contains("rollupState") || force))
    {
        SWGSDRangel::SWGRollupState *swgRollupState = new SWGSDRangel::SWGRollupState();
        settings.m_rollupState->formatTo(swgRollupState);
        swgM17DemodSettings->setRollupState(swgRollupState);
    }
}

void M17Demod::networkManagerFinished(QNetworkReply *reply)
{
    QNetworkReply::NetworkError replyError = reply->error();

    if (replyError)
    {
        qWarning() << "M17Demod::networkManagerFinished:"
                << " error(" << (int) replyError
                << "): " << replyError
                << ": " << reply->errorString();
    }
    else
    {
        QString answer = reply->readAll();
        answer.chop(1); // remove last \n
        qDebug("M17Demod::networkManagerFinished: reply:\n%s", answer.toStdString().c_str());
    }

    reply->deleteLater();
}

void M17Demod::handleIndexInDeviceSetChanged(int index)
{
    if (index < 0) {
        return;
    }

    QString fifoLabel = QString("%1 [%2:%3]")
        .arg(m_channelId)
        .arg(m_deviceAPI->getDeviceSetIndex())
        .arg(index);
    m_basebandSink->setFifoLabel(fifoLabel);
    m_basebandSink->setAudioFifoLabel(fifoLabel);
}
