#include "FpgaSerial.h"
#include <QDebug>

FpgaSerial::FpgaSerial(QObject *parent)
    : QObject(parent)
{
    m_serial.setBaudRate(QSerialPort::Baud115200);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);
}

FpgaSerial::~FpgaSerial()
{
    closePort();
}

bool FpgaSerial::openPort(const QString &portName, int baudRate)
{
    qDebug() << "FpgaSerial::openPort called with:" << portName << "baud:" << baudRate;

    if (m_serial.isOpen())
        m_serial.close();

    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);

    qDebug() << "  Attempting open ReadWrite...";
    if (!m_serial.open(QIODevice::ReadWrite)) {
        qDebug() << "  FAILED:" << m_serial.errorString();
        emit portOpened(false);
        return false;
    }
    // Set DTR high — some FPGA boards need this as enable signal
    m_serial.setDataTerminalReady(true);
    m_serial.setRequestToSend(false);
    // Small delay after port open before sending
    m_serial.waitForBytesWritten(100);
    qDebug() << "  SUCCESS: port opened, DTR=true";
    emit portOpened(true);
    return true;
}

void FpgaSerial::closePort()
{
    if (m_serial.isOpen()) {
        m_serial.close();
        emit portClosed();
    }
}

bool FpgaSerial::isOpen() const
{
    return m_serial.isOpen();
}

QString FpgaSerial::portName() const
{
    return m_serial.portName();
}

void FpgaSerial::sendStart()
{
    if (m_serial.isOpen()) {
        char cmd[] = { static_cast<char>(0xF1), static_cast<char>(0x0A) };
        m_serial.write(cmd, 2);
        m_serial.waitForBytesWritten(50);
    }
}

void FpgaSerial::sendStop()
{
    if (m_serial.isOpen()) {
        char cmd[] = { static_cast<char>(0xF0), static_cast<char>(0x0A) };
        m_serial.write(cmd, 2);
        m_serial.waitForBytesWritten(50);
    }
}
