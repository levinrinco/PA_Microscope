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
    return m_state == ScanState::Armed || m_state == ScanState::Scanning;
}

void ScanController::setState(ScanState s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void ScanController::startScan()
{
    if (m_state != ScanState::Idle && m_state != ScanState::Done) return;
    qInfo() << "ScanController::startScan";
    setState(ScanState::Idle);  // reset from previous completion
    doArm();
}

void ScanController::stopScan()
{
    qInfo() << "ScanController::stopScan state=" << static_cast<int>(m_state);
    if (isRunning()) {
        doAbort();
        setState(ScanState::Idle);
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

void ScanController::onPipelineScanComplete()
{
    if (m_state != ScanState::Scanning) return;
    qInfo() << "Pipeline scan complete";
    doFinish();
    setState(ScanState::Done);
    emit scanFinished();
}

void ScanController::onPipelineError(const QString &msg)
{
    qWarning() << "Pipeline error:" << msg;
    if (m_state == ScanState::Armed || m_state == ScanState::Scanning)
        stopScan();
    emit errorOccurred(msg);
}

void ScanController::doArm()
{
    setState(ScanState::Armed);

    if (!m_fifo || !m_fpga || !m_pipeline) {
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
        emit errorOccurred("FIFO setup failed: " + m_fifo->lastErrorText());
        return;
    }

    // Must re-define DMA after FIFO setup (mode change can invalidate)
    if (!m_fifo->redefineTransfer()) {
        emit errorOccurred("DMA re-define failed: " + m_fifo->lastErrorText());
        return;
    }

    if (!m_fifo->startAcquisition()) {
        emit errorOccurred("Card start failed: " + m_fifo->lastErrorText());
        return;
    }

    // Pipeline must be waiting before FPGA fires
    emit startWorkerThread();
    QThread::msleep(100);  // let pipeline reach waitForData()

    // Now pull the trigger
    emit fpgaStartRequested();
}

void ScanController::doFinish()
{
    // FPGA first: stopping FPGA kills the trigger, pipeline drains remaining data
    emit fpgaStopRequested();
    emit pipelineStopRequested();
    if (m_fifo) m_fifo->stopAcquisition();
}

void ScanController::doAbort()
{
    // FPGA first: killing trigger stops acquisition, then cleanup card
    emit fpgaStopRequested();
    emit pipelineStopRequested();
    if (m_fifo) m_fifo->stopAcquisition();
}
