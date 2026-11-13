#include "ScanController.h"
#include "FpgaSerial.h"
#include "SpectrumFifo.h"
#include "DataPipeline.h"
#include "FileWriter.h"
#include <QDebug>
#include <QThread>
#include "third_party/Spectrum/include/regs.h"

ScanController::ScanController(QObject *parent)
    : QObject(parent)
{
}

bool ScanController::isRunning() const
{
    return m_state == ScanState::Armed || m_state == ScanState::Scanning ||
           m_state == ScanState::Stopping;
}

void ScanController::setState(ScanState s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void ScanController::startScan()
{
    if (m_state != ScanState::Idle && m_state != ScanState::Done &&
        m_state != ScanState::Error) return;
    qInfo() << "ScanController::startScan";
    setState(ScanState::Idle);  // reset from previous completion
    doArm();
}

void ScanController::stopScan()
{
    qInfo() << "ScanController::stopScan state=" << static_cast<int>(m_state);
    if (m_state == ScanState::Stopping) return;
    if (isRunning()) {
        m_afterStopState = ScanState::Idle;
        setState(ScanState::Stopping);
        doAbort();
        if (!m_pipelineActive) setState(ScanState::Idle);
    } else if (m_state == ScanState::Done) {
        setState(ScanState::Idle);  // allow restart after completion
    }
}

void ScanController::onFirstDataReceived()
{
    if (m_state != ScanState::Armed) return;
    qInfo() << "First data received, transitioning to Scanning";
    setState(ScanState::Scanning);
    emit scanStarted();
}

void ScanController::onAcquisitionComplete()
{
    if (m_state == ScanState::Armed || m_state == ScanState::Scanning) {
        m_afterStopState = ScanState::Done;
        setState(ScanState::Stopping);
        stopHardware();
    }
}

void ScanController::onPipelineScanComplete()
{
    // A very short frame can complete before the queued first-data event is
    // delivered to the GUI thread, so Armed is also a valid completion state.
    if (!m_pipelineActive) return;
    m_pipelineActive = false;
    qInfo() << "Pipeline scan complete";
    doFinish();
    setState(m_state == ScanState::Stopping ? m_afterStopState : ScanState::Done);
    emit scanFinished();
}

void ScanController::onPipelineError(const QString &msg)
{
    qWarning() << "Pipeline error:" << msg;
    m_afterStopState = ScanState::Error;
    setState(m_pipelineActive ? ScanState::Stopping : ScanState::Error);
    doAbort();
    emit errorOccurred(msg);
}

void ScanController::doArm()
{
    setState(ScanState::Armed);

    if (!m_fifo || !m_fpga || !m_pipeline) {
        setState(ScanState::Error);
        emit errorOccurred("Hardware not initialized");
        return;
    }

    // Stop any previous run and verify card is idle
    m_fifo->stopAcquisition();
    {
        FifoStatus s = m_fifo->readFifoStatus();
        if (s.m2Status != 0) {
            qWarning() << "doArm: card M2 not idle after stop, status=0x"
                       << Qt::hex << s.m2Status << "- retrying";
            m_fifo->stopAcquisition();
            // Small delay to let the card settle
            QThread::msleep(100);
            s = m_fifo->readFifoStatus();
            if (s.m2Status != 0) {
                qWarning() << "doArm: card M2 still not idle after retry, status=0x"
                           << Qt::hex << s.m2Status;
            }
        }
    }

    // Use saved channel mask (not hardcoded CHANNEL0)
    uint64_t chMask = m_params.channelMask ? m_params.channelMask : CHANNEL0;

    if (!m_fifo->setupFifoMode(m_params.fifoMode, chMask,
                                m_params.preTrigger, m_params.segmentSize,
                                m_params.loopCount)) {
        setState(ScanState::Error);
        emit errorOccurred("FIFO setup failed: " + m_fifo->lastErrorText());
        return;
    }

    // Must re-define DMA after FIFO setup (mode change can invalidate)
    if (!m_fifo->redefineTransfer()) {
        setState(ScanState::Error);
        emit errorOccurred("DMA re-define failed: " + m_fifo->lastErrorText());
        return;
    }

    if (!m_fifo->startAcquisition()) {
        setState(ScanState::Error);
        emit errorOccurred("Card start failed: " + m_fifo->lastErrorText());
        return;
    }
    m_hardwareActive = true;

    // Pipeline must be waiting before FPGA fires
    m_pipelineActive = true;
    emit startWorkerThread();
    QThread::msleep(100);  // let pipeline reach waitForData()

    // Now pull the trigger
    emit fpgaStartRequested();
}

void ScanController::doFinish()
{
    stopHardware();
    emit pipelineStopRequested();
}

void ScanController::doAbort()
{
    stopHardware();
    emit pipelineStopRequested();
}

void ScanController::stopHardware()
{
    if (!m_hardwareActive) return;
    // FPGA first: killing the trigger prevents new segments before DMA stops.
    emit fpgaStopRequested();
    if (m_fifo) m_fifo->stopAcquisition();
    m_hardwareActive = false;
}
