#pragma once

#include <cstdint>
#include <QString>
#include "third_party/Spectrum/include/dlltyp.h"
#include "third_party/Spectrum/include/regs.h"
#include "third_party/Spectrum/include/spcm_drv.h"
#include "third_party/Spectrum/include/spcerr.h"
#include "Types.h"

class SpectrumFifo {
public:
    SpectrumFifo();
    ~SpectrumFifo();

    // ---- lifecycle ----
    bool openCard(int cardIndex = 0);
    void closeCard();
    bool isOpen() const { return m_hCard != nullptr; }

    // ---- card info ----
    CardInfo cardInfo() const { return m_cardInfo; }

    // ---- card capabilities (queried from hardware, series-agnostic) ----
    bool hasOffsetDAC() const { return !m_bIsM3i; }   // M3i series lacks offset DAC
    bool isM3i()     const { return m_bIsM3i; }
    int  inputPathCount();  // queried from SPC_READAIPATHCOUNT

    // ---- available clock modes (bitmask of SPC_CM_*) ----
    int32_t availableClockModes();

    // ---- available trigger sources (bitmask of SPC_TMASK_*) ----
    int32_t availableTriggerSources();

    // ---- input path features (bitmask of SPCM_AI_*) ----
    int32_t inputFeatures(int path);

    // ---- available input ranges for a given path ----
    struct InputRange { int minMv; int maxMv; };
    int inputRangeCount(int path);
    std::vector<InputRange> inputRanges(int path);

    // ---- per-channel setup ----
    bool setInputRange(int channel, int rangeMv);
    bool setInputPath(int channel, int path);
    bool setTermination(int channel, bool fiftyOhm);
    bool setCoupling(int channel, bool acCoupled);
    bool setDiffMode(int channel, bool differential);
    static int32_t chRegBase(int ch) { return 30000 + 100 * ch; }

    // ---- global setup ----
    bool setupFifoMode(int32_t cardMode, uint64_t channelMask,
                       int64_t preTriggerSamples,
                       int64_t segmentSize,
                       int64_t numLoops = 0);
    bool setExternalClock(int divider = 1);
    bool setInternalPllClock(int64_t rateHz, bool clockOut = false);
    bool setExternalTrigger(int trigMode, int extLine = 0, bool term = true);
    bool setTimeout(int timeoutMs);
    bool setTriggerLevel(int extLine, int level0Mv, int level1Mv);
    bool setTriggerDelay(int32_t delaySamples);
    bool setTriggerCoupling(int extLine, bool acCoupled);
    bool setBandwidthLimit(int channel, bool limited);
    bool setOffset(int channel, int offsetMv);
    bool setReferenceClock(int64_t freqHz);

    // ---- DMA buffer ----
    bool allocateBuffer(int64_t bufferSizeBytes, int32_t notifySizeBytes);
    bool redefineTransfer();  // re-define DMA after mode change, without re-alloc
    void freeBuffer();

    // ---- start/stop ----
    bool startAcquisition();
    bool stopAcquisition();
    bool forceTrigger();

    // ---- FIFO polling ----
    uint32_t waitForData();
    FifoStatus readFifoStatus() const;
    bool releaseData(int64_t bytes);
    int64_t triggerCount() const;

    // ---- buffer access ----
    int16_t *dataBuffer() { return m_pData; }
    int16_t *dataBufferAt(int64_t byteOffset) {
        return reinterpret_cast<int16_t *>(
            reinterpret_cast<char *>(m_pData) + byteOffset);
    }
    int64_t  bufferSize() const { return m_bufferSize; }
    int32_t  notifySize() const { return m_notifySize; }

    // ---- error ----
    QString lastErrorText() const;
    static QString translateError(uint32_t errorCode);

private:
    drv_handle m_hCard = nullptr;
    int16_t   *m_pData = nullptr;
    int64_t    m_bufferSize = 0;
    int32_t    m_notifySize = 0;
    CardInfo   m_cardInfo;
    bool       m_bIsM3i = false;

    void readCardInfo();
    bool checkError(uint32_t errorCode);
    static void *allocPageAligned(uint64_t size);
    static void  freePageAligned(void *ptr, uint64_t size);
};
