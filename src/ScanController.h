#pragma once

#include <QObject>
#include "Types.h"

class FpgaSerial;
class SpectrumFifo;
class DataPipeline;
class FileWriter;

class ScanController : public QObject {
    Q_OBJECT
public:
    explicit ScanController(QObject *parent = nullptr);

    void setFpgaSerial(FpgaSerial *fpga)    { m_fpga = fpga; }
    void setSpectrumFifo(SpectrumFifo *fifo) { m_fifo = fifo; }
    void setPipeline(DataPipeline *pipeline) { m_pipeline = pipeline; }
    void setFileWriter(FileWriter *writer)   { m_writer = writer; }
    void setScanParams(const ScanParams &params) { m_params = params; }

    ScanState state() const        { return m_state; }
    bool isRunning() const;

signals:
    void stateChanged(ScanState newState);
    void scanStarted();
    void scanFinished();
    void errorOccurred(const QString &msg);

    void fpgaStartRequested();
    void fpgaStopRequested();
    void pipelineStopRequested();
    void startWorkerThread();

public slots:
    void startScan();
    void stopScan();
    void onFirstDataReceived();
    void onPipelineScanComplete();
    void onPipelineError(const QString &msg);

private:
    ScanState m_state = ScanState::Idle;

    FpgaSerial   *m_fpga = nullptr;
    SpectrumFifo *m_fifo = nullptr;
    DataPipeline *m_pipeline = nullptr;
    FileWriter   *m_writer = nullptr;
    ScanParams    m_params;

    void setState(ScanState s);
    void doArm();
    void doFinish();
    void doAbort();
};
