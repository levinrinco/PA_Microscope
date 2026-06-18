#include "SpectrumFifo.h"
#include "third_party/Spectrum/include/regs.h"
#include "third_party/Spectrum/include/spcerr.h"
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

SpectrumFifo::SpectrumFifo() = default;

SpectrumFifo::~SpectrumFifo()
{
    freeBuffer();
    closeCard();
}

// ===== Allocation helpers =====

void *SpectrumFifo::allocPageAligned(uint64_t size)
{
#ifdef Q_OS_WIN
    return VirtualAlloc(nullptr, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    Q_UNUSED(size);
    return nullptr;
#endif
}

void SpectrumFifo::freePageAligned(void *ptr, uint64_t /*size*/)
{
#ifdef Q_OS_WIN
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
#else
    Q_UNUSED(ptr);
#endif
}

// ===== Lifecycle =====

bool SpectrumFifo::openCard(int cardIndex)
{
    char drvName[20];
    sprintf(drvName, "/dev/spcm%d", cardIndex);

    m_hCard = spcm_hOpen(drvName);
    if (!m_hCard) {
        m_cardInfo.isOpen = false;
        return false;
    }

    readCardInfo();
    return true;
}

void SpectrumFifo::closeCard()
{
    if (m_hCard) {
        spcm_vClose(m_hCard);
        m_hCard = nullptr;
    }
    m_cardInfo.isOpen = false;
}

void SpectrumFifo::readCardInfo()
{
    m_cardInfo.isOpen = true;

    // card type string
    char acCardType[20] = {};
    spcm_dwGetParam_ptr(m_hCard, SPC_PCITYP, acCardType, sizeof(acCardType));
    m_cardInfo.cardType = QString::fromLatin1(acCardType);

    // Detect M3i series (M3i lacks offset DAC, has dual input paths)
    // IMPORTANT: TYP_M3ISERIES|TYP_M3IEXPSERIES = 0xB0000 which collides with
    // TYP_M4IEXPSERIES (0x70000). Must check each series separately.
    int32 typeCode = 0;
    spcm_dwGetParam_i32(m_hCard, SPC_PCITYP, &typeCode);
    int32 series = typeCode & 0x000F0000;  // isolate series bits
    m_bIsM3i = (series == TYP_M3ISERIES || series == TYP_M3IEXPSERIES);
    qInfo() << "Card:" << m_cardInfo.cardType
            << QString("typeCode=0x%1 series=0x%2").arg(typeCode, 8, 16, QChar('0')).arg(series, 4, 16, QChar('0'))
            << (m_bIsM3i ? "M3i" : "M4i");

    spcm_dwGetParam_i64(m_hCard, SPC_PCIMEMSIZE,    &m_cardInfo.memSizeBytes);

    spcm_dwGetParam_i64(m_hCard, SPC_MIINST_MINADCLOCK, &m_cardInfo.minSamplerate);
    spcm_dwGetParam_i64(m_hCard, SPC_MIINST_MAXADCLOCK, &m_cardInfo.maxSamplerate);

    int32 serialNo, funcType, bytesPerSamp, modules, chPerModule;
    spcm_dwGetParam_i32(m_hCard, SPC_PCISERIALNO,          &serialNo);
    spcm_dwGetParam_i32(m_hCard, SPC_FNCTYPE,              &funcType);
    spcm_dwGetParam_i32(m_hCard, SPC_MIINST_MODULES,       &modules);
    spcm_dwGetParam_i32(m_hCard, SPC_MIINST_CHPERMODULE,   &chPerModule);
    spcm_dwGetParam_i32(m_hCard, SPC_MIINST_BYTESPERSAMPLE, &bytesPerSamp);
    m_cardInfo.serialNumber = serialNo;
    m_cardInfo.functionType = funcType;
    m_cardInfo.maxChannels = static_cast<int>(modules * chPerModule);
    m_cardInfo.bytesPerSample = static_cast<int>(bytesPerSamp);
}

// ===== Setup =====

bool SpectrumFifo::setupFifoMode(int32_t cardMode, uint64_t channelMask,
                                  int64_t preTriggerSamples,
                                  int64_t segmentSize,
                                  int64_t numLoops)
{
    qDebug() << "setupFifoMode: mode=0x" << Qt::hex << cardMode << Qt::dec
             << "chMask=" << channelMask << "seg=" << segmentSize
             << "pre=" << preTriggerSamples << "loops=" << numLoops;
    uint32_t err = ERR_OK;
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_CARDMODE,     cardMode);
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_CHENABLE,     static_cast<int32>(channelMask));
    if (!err) err = spcm_dwSetParam_i64(m_hCard, SPC_SEGMENTSIZE,  segmentSize);
    if (!err) err = spcm_dwSetParam_i64(m_hCard, SPC_POSTTRIGGER,  segmentSize - preTriggerSamples);
    if (!err) err = spcm_dwSetParam_i64(m_hCard, SPC_PRETRIGGER,   preTriggerSamples);
    if (!err) err = spcm_dwSetParam_i64(m_hCard, SPC_LOOPS,        numLoops);
    if (err) qWarning() << "setupFifoMode failed:" << lastErrorText();
    return checkError(err);
}

bool SpectrumFifo::setExternalClock(int divider)
{
    uint32_t err = ERR_OK;
    if (divider > 1) {
        if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_CLOCKMODE, SPC_CM_EXTDIVIDER);
    } else {
        if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_CLOCKMODE, SPC_CM_EXTERNAL);
    }
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_CLOCKDIV, divider);
    return checkError(err);
}

bool SpectrumFifo::setInternalPllClock(int64_t rateHz, bool clockOut)
{
    uint32_t err = ERR_OK;
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_CLOCKMODE, SPC_CM_INTPLL);
    if (!err) err = spcm_dwSetParam_i64(m_hCard, SPC_SAMPLERATE, rateHz);
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_CLOCKOUT, clockOut ? 1 : 0);
    return checkError(err);
}

bool SpectrumFifo::setExternalTrigger(int trigMode, int extLine, bool term)
{
    uint32_t err = ERR_OK;

    // X0/X1 TTL trigger: configure multi-purpose line as trigger input
    if (extLine == 1)
        if (!err) err = spcm_dwSetParam_i32(m_hCard, SPCM_X0_MODE, SPCM_XMODE_TRIGIN);

    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_EXT0_MODE + extLine, trigMode);
    // Termination only applies to Ext0 analog trigger
    if (extLine == 0)
        if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_TERM, term ? 1 : 0);

    // Set as single trigger source
    switch (extLine) {
    case 0:
        if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_ORMASK, SPC_TMASK_EXT0);
        break;
    case 1:
        if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_ORMASK, SPC_TMASK_EXT1);
        break;
    default:
        break;
    }
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_ANDMASK, 0);
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_CH_ORMASK0, 0);
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_CH_ANDMASK0, 0);
    return checkError(err);
}

bool SpectrumFifo::setInputRange(int channel, int rangeMv)
{
    int32_t reg = static_cast<int32_t>(chRegBase(channel) + 10);
    uint32_t err = spcm_dwSetParam_i32(m_hCard, reg, rangeMv);
    return checkError(err);
}

bool SpectrumFifo::setInputPath(int channel, int path)
{
    int32_t reg = static_cast<int32_t>(chRegBase(channel) + 90);
    uint32_t err = spcm_dwSetParam_i32(m_hCard, reg, path);
    return checkError(err);
}

bool SpectrumFifo::setTermination(int channel, bool fiftyOhm)
{
    int32_t reg = static_cast<int32_t>(chRegBase(channel) + 30);
    uint32_t err = spcm_dwSetParam_i32(m_hCard, reg, fiftyOhm ? 1 : 0);
    return checkError(err);
}

bool SpectrumFifo::setCoupling(int channel, bool acCoupled)
{
    int32_t reg = static_cast<int32_t>(chRegBase(channel) + 20);
    uint32_t err = spcm_dwSetParam_i32(m_hCard, reg, acCoupled ? 1 : 0);
    return checkError(err);
}

bool SpectrumFifo::setDiffMode(int channel, bool differential)
{
    int32_t reg = static_cast<int32_t>(chRegBase(channel) + 40);
    uint32_t err = spcm_dwSetParam_i32(m_hCard, reg, differential ? 1 : 0);
    return checkError(err);
}

bool SpectrumFifo::setTimeout(int timeoutMs)
{
    uint32_t err = spcm_dwSetParam_i32(m_hCard, SPC_TIMEOUT, timeoutMs);
    return checkError(err);
}

bool SpectrumFifo::setTriggerLevel(int extLine, int level0Mv, int level1Mv)
{
    if (!m_hCard) return false;
    // TTL trigger (extLine >= 1) has fixed thresholds, skip
    if (extLine >= 1) return true;
    uint32_t err = ERR_OK;
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_EXT0_LEVEL0, level0Mv);
    if (!err) err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_EXT0_LEVEL1, level1Mv);
    return checkError(err);
}

bool SpectrumFifo::setTriggerDelay(int32_t delaySamples)
{
    uint32_t err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_DELAY, delaySamples);
    return checkError(err);
}

bool SpectrumFifo::setTriggerCoupling(int extLine, bool acCoupled)
{
    if (!m_hCard) return false;
    // TTL trigger (extLine >= 1) is fixed DC, skip
    if (extLine >= 1) return true;
    uint32_t err = spcm_dwSetParam_i32(m_hCard, SPC_TRIG_EXT0_ACDC, acCoupled ? 1 : 0);
    return checkError(err);
}

bool SpectrumFifo::setBandwidthLimit(int channel, bool limited)
{
    int32_t reg = static_cast<int32_t>(chRegBase(channel) + 80);
    uint32_t err = spcm_dwSetParam_i32(m_hCard, reg, limited ? 1 : 0);
    return checkError(err);
}

bool SpectrumFifo::setOffset(int channel, int offsetMv)
{
    int32_t reg = static_cast<int32_t>(chRegBase(channel) + 0);
    uint32_t err = spcm_dwSetParam_i32(m_hCard, reg, offsetMv);
    return checkError(err);
}

bool SpectrumFifo::setReferenceClock(int64_t freqHz)
{
    uint32_t err = spcm_dwSetParam_i64(m_hCard, SPC_REFERENCECLOCK, freqHz);
    return checkError(err);
}

// ===== DMA buffer =====

bool SpectrumFifo::allocateBuffer(int64_t bufferSizeBytes, int32_t notifySizeBytes)
{
    m_bufferSize = bufferSizeBytes;
    m_notifySize = notifySizeBytes;

    m_pData = static_cast<int16_t *>(allocPageAligned(static_cast<uint64_t>(bufferSizeBytes)));
    if (!m_pData)
        return false;

    uint32_t err = spcm_dwDefTransfer_i64(
        m_hCard, SPCM_BUF_DATA, SPCM_DIR_CARDTOPC,
        m_notifySize,            // notify when this many bytes ready
        m_pData,                 // destination buffer
        0,                       // offset in card memory
        m_bufferSize);           // total buffer length in bytes

    return checkError(err);
}

bool SpectrumFifo::redefineTransfer()
{
    if (!m_pData || m_bufferSize <= 0) return false;
    uint32_t err = spcm_dwDefTransfer_i64(
        m_hCard, SPCM_BUF_DATA, SPCM_DIR_CARDTOPC,
        m_notifySize,
        m_pData,
        0,
        m_bufferSize);
    return checkError(err);
}

void SpectrumFifo::freeBuffer()
{
    if (m_pData) {
        freePageAligned(m_pData, static_cast<uint64_t>(m_bufferSize));
        m_pData = nullptr;
    }
    m_bufferSize = 0;
    m_notifySize = 0;
}

// ===== Start / Stop =====

bool SpectrumFifo::startAcquisition()
{
    if (!m_hCard) { qWarning() << "startAcquisition: no card"; return false; }
    uint32_t err = spcm_dwSetParam_i32(m_hCard, SPC_M2CMD,
        M2CMD_CARD_START | M2CMD_CARD_ENABLETRIGGER | M2CMD_DATA_STARTDMA);
    if (err) qWarning() << "startAcquisition failed:" << lastErrorText();
    return checkError(err);
}

bool SpectrumFifo::stopAcquisition()
{
    if (!m_hCard) return false;
    uint32_t err = spcm_dwSetParam_i32(m_hCard, SPC_M2CMD,
        M2CMD_CARD_STOP | M2CMD_DATA_STOPDMA);
    return checkError(err);
}

bool SpectrumFifo::forceTrigger()
{
    if (!m_hCard) return false;
    uint32_t err = spcm_dwSetParam_i32(m_hCard, SPC_M2CMD, M2CMD_CARD_FORCETRIGGER);
    return checkError(err);
}

// ===== FIFO polling =====

int64_t SpectrumFifo::triggerCount() const
{
    if (!m_hCard) return -1;
    int64 count = 0;
    spcm_dwGetParam_i64(m_hCard, SPC_TRIGGERCOUNTER, &count);
    return count;
}

uint32_t SpectrumFifo::waitForData()
{
    if (!m_hCard) return ERR_ABORT;
    return spcm_dwSetParam_i32(m_hCard, SPC_M2CMD, M2CMD_DATA_WAITDMA);
}

FifoStatus SpectrumFifo::readFifoStatus() const
{
    FifoStatus s;
    if (!m_hCard) return s;
    int32 m2status, fillpromille;
    spcm_dwGetParam_i32(m_hCard, SPC_M2STATUS,             &m2status);
    spcm_dwGetParam_i64(m_hCard, SPC_DATA_AVAIL_USER_LEN,  &s.availUserBytes);
    spcm_dwGetParam_i64(m_hCard, SPC_DATA_AVAIL_USER_POS,  &s.availUserPos);
    spcm_dwGetParam_i32(m_hCard, SPC_FILLSIZEPROMILLE,     &fillpromille);
    s.m2Status = m2status;
    s.fillSizePromille = fillpromille;
    return s;
}

bool SpectrumFifo::releaseData(int64_t bytes)
{
    if (!m_hCard) return false;
    uint32_t err = spcm_dwSetParam_i64(m_hCard, SPC_DATA_AVAIL_CARD_LEN, bytes);
    return checkError(err);
}

// ===== Card capability queries =====

int32_t SpectrumFifo::availableClockModes()
{
    if (!m_hCard) return 0;
    int32 mask;
    spcm_dwGetParam_i32(m_hCard, SPC_AVAILCLOCKMODES, &mask);
    return mask;
}

int32_t SpectrumFifo::availableTriggerSources()
{
    if (!m_hCard) return 0;
    int32 mask;
    spcm_dwGetParam_i32(m_hCard, SPC_TRIG_AVAILORMASK, &mask);
    return mask;
}

int SpectrumFifo::inputPathCount()
{
    if (!m_hCard) return 1;
    int32 count = 0;
    spcm_dwGetParam_i32(m_hCard, SPC_READAIPATHCOUNT, &count);
    qDebug() << "inputPathCount:" << count;
    return (count > 0) ? static_cast<int>(count) : 1;
}

int32_t SpectrumFifo::inputFeatures(int path)
{
    if (!m_hCard) return 0;
    spcm_dwSetParam_i32(m_hCard, SPC_READAIPATH, path);
    int32 feat;
    spcm_dwGetParam_i32(m_hCard, SPC_READAIFEATURES, &feat);
    qDebug() << "inputFeatures path" << path << "= 0x" << Qt::hex << feat;
    return feat;
}

int SpectrumFifo::inputRangeCount(int path)
{
    if (!m_hCard) return 0;
    spcm_dwSetParam_i32(m_hCard, SPC_READAIPATH, path);
    int32 count;
    spcm_dwGetParam_i32(m_hCard, SPC_READIRCOUNT, &count);
    if (count < 0 || count > 32) {
        qWarning() << "inputRangeCount: implausible count" << count << "for path" << path << "- clamping to 0";
        count = 0;
    }
    qDebug() << "inputRangeCount path" << path << "=" << count;
    return static_cast<int>(count);
}

std::vector<SpectrumFifo::InputRange> SpectrumFifo::inputRanges(int path)
{
    std::vector<InputRange> out;
    if (!m_hCard) return out;
    spcm_dwSetParam_i32(m_hCard, SPC_READAIPATH, path);
    int32 count;
    spcm_dwGetParam_i32(m_hCard, SPC_READIRCOUNT, &count);
    if (count < 0 || count > 32) {
        qWarning() << "inputRanges: implausible count" << count << "for path" << path;
        count = 0;
    }
    for (int32 i = 0; i < count; ++i) {
        int32 minMv, maxMv;
        spcm_dwGetParam_i32(m_hCard, SPC_READRANGEMIN0 + i, &minMv);
        spcm_dwGetParam_i32(m_hCard, SPC_READRANGEMAX0 + i, &maxMv);
        out.push_back({static_cast<int>(minMv), static_cast<int>(maxMv)});
    }
    qDebug() << "inputRanges path" << path << ":" << out.size() << "ranges";
    return out;
}

// ===== Error handling =====

QString SpectrumFifo::lastErrorText() const
{
    char buf[ERRORTEXTLEN] = {};
    if (m_hCard) {
        spcm_dwGetErrorInfo_i32(m_hCard, nullptr, nullptr, buf);
    }
    return QString::fromLatin1(buf);
}

QString SpectrumFifo::translateError(uint32_t errorCode)
{
    switch (errorCode) {
    case ERR_OK:             return QStringLiteral("No Error");
    case ERR_TIMEOUT:        return QStringLiteral("Timeout");
    case ERR_ABORT:          return QStringLiteral("Aborted by stop command");
    case ERR_FIFOBUFOVERRUN: return QStringLiteral("FIFO SW buffer overrun");
    case ERR_FIFOHWOVERRUN:  return QStringLiteral("FIFO HW buffer overrun");
    case ERR_FIFOFINISHED:   return QStringLiteral("FIFO mode finished");
    default:                 return QString("Error code %1").arg(errorCode);
    }
}

bool SpectrumFifo::checkError(uint32_t errorCode)
{
    if (errorCode != ERR_OK) {
        // error text will be retrievable via lastErrorText()
        return false;
    }
    return true;
}
