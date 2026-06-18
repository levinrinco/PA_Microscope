#pragma once

#include <QObject>
#include <QSerialPort>

class FpgaSerial : public QObject {
    Q_OBJECT
public:
    explicit FpgaSerial(QObject *parent = nullptr);
    ~FpgaSerial();

    bool openPort(const QString &portName, int baudRate = 115200);
    void closePort();
    bool isOpen() const;
    QString portName() const;

public slots:
    void sendStart();   // sends binary 0xF1 0x0A
    void sendStop();    // sends binary 0xF0 0x0A

signals:
    void portOpened(bool success);
    void portClosed();

private:
    QSerialPort m_serial;
};
