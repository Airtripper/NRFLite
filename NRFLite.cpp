#include <NRFLite.h>

#define debug(input)   { if (_serial) _serial->print(input);   }
#define debugln(input) { if (_serial) _serial->println(input); }

#if defined( __AVR_ATtiny84__ )
    const static uint8_t USI_DI = PA6;
    const static uint8_t USI_DO = PA5;
    const static uint8_t SCK    = PA4;
#elif defined( __AVR_ATtiny85__ )
    const static uint8_t USI_DI = PB0;
    const static uint8_t USI_DO = PB1;
    const static uint8_t SCK    = PB2;
#else
    #include <SPI.h> // Use the normal Arduino hardware SPI library if we are not on ATtiny.
#endif

////////////////////
// Public methods //
////////////////////

uint8_t NRFLite::init(uint8_t radioId, uint8_t cePin, uint8_t csnPin, Bitrates bitrate, uint8_t channel)
{
    delay(100); // 100 ms = Vcc > 1.9v power on reset time.
    
    _cePin = cePin;
    _csnPin = csnPin;
    _enableInterruptFlagsReset = 1;
    
    // Setup the microcontroller for SPI communication with the radio.
    #if defined(__AVR_ATtiny84__) || defined(__AVR_ATtiny85__)
    pinMode(USI_DI, INPUT ); digitalWrite(USI_DI, HIGH);
    pinMode(USI_DO, OUTPUT); digitalWrite(USI_DO, LOW);
    pinMode(SCK, OUTPUT); digitalWrite(SCK, LOW);
    #else
    // Arduino SPI makes SS (D10) an output and sets it HIGH.  It must remain an output
    // for Master SPI operation to work, but in case it was originally LOW, we'll set it back.
    uint8_t savedSS = digitalRead(SS);
    SPI.setClockDivider(SPI_CLOCK_DIV2);
    SPI.begin();
    if (_csnPin != SS) digitalWrite(SS, savedSS);
    #endif
    
    // When CSN is LOW the radio listens to SPI communication, so we operate most of the time with CSN HIGH.
    pinMode(_cePin, OUTPUT); pinMode(_csnPin, OUTPUT);
    digitalWrite(_csnPin, HIGH);
    
    // Valid channel range is 2400 - 2525 MHz, in 1 MHz increments.
    if (channel > 125) { channel = 125; }
    writeRegister(RF_CH, channel);
    
    // Transmission speed, retry times, and output power setup.
    // For 2 Mbps or 1 Mbps operation, a 500 uS retry time is necessary to support the max ACK packet size.
    // For 250 Kbps operation, a 1500 uS retry time is necessary.
    // Retry time  = SETUP_RETR upper 4 bits (0 = 250 uS, 1 = 500 us, 2 = 750 us, ... , 15 = 4000 us).
    // Retry count = SETUP_RETR lower 4 bits (0 to 15).
    // '_allowedDataCheckIntervalMicros' is used to limit how often the radio can be checked to determine if data
    // has been received when CE and CSN share the same pin.  If we don't limit how often the radio is checked,
    // the radio may never be given the chance to receive a packet.  More info about this in the 'hasData' method.
    // '_allowedDataCheckIntervalMicros' was determined by maximizing the transfer bitrate between two 16 MHz ATmega328's
    // using 32 byte payloads and sending back 32 byte ACK packets.

    if (bitrate == BITRATE2MBPS) {
        writeRegister(RF_SETUP, B00001110);     // 2 Mbps, 0 dBm output power
        writeRegister(SETUP_RETR, B00011111);   // 0001 =  500 uS between retries, 1111 = 15 retries
        _allowedDataCheckIntervalMicros = 600;
        _transmissionRetryWaitMicros = 250;
    }
    else if (bitrate == BITRATE1MBPS) {
        writeRegister(RF_SETUP, B00000110);     // 1 Mbps, 0 dBm output power
        writeRegister(SETUP_RETR, B00011111);   // 0001 =  500 uS between retries, 1111 = 15 retries
        _allowedDataCheckIntervalMicros = 1200;
        _transmissionRetryWaitMicros = 1000;
    }
    else {
        writeRegister(RF_SETUP, B00100110);     // 250 Kbps, 0 dBm output power
        writeRegister(SETUP_RETR, B01011111);   // 0101 = 1500 uS between retries, 1111 = 15 retries
        //writeRegister(SETUP_RETR, B01010001); // 0101 = 1500 uS between retries, 0001 = 1 retry (for testing failed transmissions)
        _allowedDataCheckIntervalMicros = 8000;
        _transmissionRetryWaitMicros = 1500;
    }
    
    // Assign this radio's address to RX pipe 1.  When another radio sends us data, this is the address
    // it will use.  We use RX pipe 1 to store our address since the address in RX pipe 0 is reserved
    // for use with auto-acknowledgment packets.
    uint8_t address[5] = { 1, 2, 3, 4, radioId };
    writeRegister(RX_ADDR_P1, &address, 5);
    
    // Enable dynamically sized packets on the 2 RX pipes we use, 0 and 1.
    // RX pipe address 1 is used to for normal packets from radios that send us data.
    // RX pipe address 0 is used to for auto-acknowledgment packets from radios we transmit to.
    writeRegister(DYNPD, _BV(DPL_P0) | _BV(DPL_P1));
    
    // Enable dynamically sized payloads, ACK payloads, and TX support with or without an ACK request.
    writeRegister(FEATURE, _BV(EN_DPL) | _BV(EN_ACK_PAY) | _BV(EN_DYN_ACK));
    
    // Ensure RX FIFO and TX FIFO buffers are empty.  Each buffer can hold 3 packets.
    spiTransfer(WRITE_OPERATION, FLUSH_RX, NULL, 0);
    spiTransfer(WRITE_OPERATION, FLUSH_TX, NULL, 0);
    
    // Clear any interrupts.
    uint8_t statusReg = readRegister(STATUS);
    writeRegister(STATUS, statusReg | _BV(RX_DR) | _BV(TX_DS) | _BV(MAX_RT));
    
    // Power on the radio and start listening, waiting for startup to complete.
    // 1500 uS = Powered Off mode to Standby mode transition time + 130 uS Standby to RX mode.
    uint8_t newConfigReg = _BV(PWR_UP) | _BV(PRIM_RX) | _BV(EN_CRC);
    writeRegister(CONFIG, newConfigReg);
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1630);
    
    // Return success if the update we made to the CONFIG register was successful.
    return readRegister(CONFIG) == newConfigReg;
}

void NRFLite::addAckData(void* data, uint8_t length, uint8_t removeExistingAcks)
{
    // Up to 3 auto-acknowledgment packets can be enqueued in the TX FIFO buffer.  Users might want to ensure
    // the next ACK packet provided has the most up to date data (like a battery voltage level),
    // so this gives them the option to remove any previously added ACKs that have not yet gone out.
    if (removeExistingAcks) {
        spiTransfer(WRITE_OPERATION, FLUSH_TX, NULL, 0); // Clear the TX FIFO buffer.
    }
    
    // Add the packet to the TX FIFO buffer for pipe 1, the pipe used to receive packets from radios that
    // send us data.  When we receive the next transmission from a radio, we'll provide this ACK data in the
    // auto-acknowledgment packet that goes back.
    spiTransfer(WRITE_OPERATION, (W_ACK_PAYLOAD | 1), data, length);
}

uint8_t NRFLite::hasAckData()
{
    // If we have a pipe 0 packet sitting at the top of the RX FIFO buffer, we have auto-acknowledgment data.
    // We receive ACK data from other radios using the pipe 0 address.
    if (getPipeOfFirstRxFifoPacket() == 0) {
        return getRxFifoPacketLength(); // Return the length of the data packet in the RX FIFO buffer.
    }
    else {
        return 0;
    }
}

uint8_t NRFLite::hasData(uint8_t usingInterrupts)
{
    // If using the same pins for CE and CSN, we need to ensure CE is left HIGH long enough to receive data.
    // If we don't limit the calling program, CE may mainly be LOW and the radio won't get a chance
    // to receive packets.  However, if the calling program is using an interrupt handler and only calling
    // hasData when the data received flag is set, we should skip this check since we know the calling program
    // is not continually polling hasData.  So 'usingInterrupts' = 1 bypasses the logic.
    if (_cePin == _csnPin && !usingInterrupts) {
        
        if (micros() - _microsSinceLastDataCheck < _allowedDataCheckIntervalMicros) {
            return 0; // Prevent the calling program from forcing us to bring CE low, making the radio stop receiving.
        }
        else {
            _microsSinceLastDataCheck = micros();
        }
    }
    
    // Ensure radio is powered on and in RX mode in case the radio was powered down or in TX mode.
    uint8_t originalConfigReg = readRegister(CONFIG);
    uint8_t newConfigReg = originalConfigReg | _BV(PWR_UP) | _BV(PRIM_RX);
    if (originalConfigReg != newConfigReg) { writeRegister(CONFIG, newConfigReg); }
    
    // Ensure we're listening for packets by setting CE HIGH.  If we share the same pin for CE and CSN,
    // it will already be HIGH since we always keep CSN HIGH to prevent the radio from listening to the SPI bus.
    if (_cePin != _csnPin) { 
        if (digitalRead(_cePin) == LOW) digitalWrite(_cePin, HIGH); 
    }
    
    // If the radio was initially powered off, wait for it to turn on.
    // 1500 uS = Powered Off mode to Standby mode transition time + 130 uS Standby to RX mode.
    if ((originalConfigReg & _BV(PWR_UP)) == 0) { delayMicroseconds(1630); }

    // If we have a pipe 1 packet sitting at the top of the RX FIFO buffer, we have data.
    // We listen for data from other radios using the pipe 1 address.
    if (getPipeOfFirstRxFifoPacket() == 1) {
        return getRxFifoPacketLength(); // Return the length of the data packet in the RX FIFO buffer.
    }
    else {
        return 0;
    }
}

uint8_t NRFLite::hasDataISR()
{
    // This method, mainly for clarity, can be used inside an interrupt handler for the radio's IRQ pin to bypass
    // the limit on how often the radio can be checked for data.  This optimization greatly increases the receiving
    // bitrate when CE and CSN share the same pin.
    return hasData(1); // usingInterrupts = 1
}

void NRFLite::readData(void* data)
{
    // Determine length of data in the RX FIFO buffer and read it.
    uint8_t dataLength;
    spiTransfer(READ_OPERATION, R_RX_PL_WID, &dataLength, 1);
    spiTransfer(READ_OPERATION, R_RX_PAYLOAD, data, dataLength);
    
    // Clear data received flag.
    uint8_t statusReg = readRegister(STATUS);
    if (statusReg & _BV(RX_DR)) {
        writeRegister(STATUS, readRegister(STATUS) | _BV(RX_DR));
    }
}

uint8_t NRFLite::send(uint8_t toRadioId, void* data, uint8_t length, SendType sendType)
{
    prepForTransmission(toRadioId, sendType);

    // Clear any previously asserted TX success or max retries flags.
    uint8_t statusReg = readRegister(STATUS);
    if (statusReg & _BV(TX_DS) || statusReg & _BV(MAX_RT)) {
        writeRegister(STATUS, statusReg | _BV(TX_DS) | _BV(MAX_RT));
    }
    
    // Add data to the TX FIFO buffer, with or without an ACK request.
    if (sendType == NO_ACK) { spiTransfer(WRITE_OPERATION, W_TX_PAYLOAD_NO_ACK, data, length); }
    else                    { spiTransfer(WRITE_OPERATION, W_TX_PAYLOAD       , data, length); }

    // Start transmission.
    // If we have separate pins for CE and CSN, CE will be LOW and we must pulse it to start transmission.
    // If we use the same pin for CE and CSN, CE will already be HIGH and transmission will have started
    // when data was loaded into the TX FIFO.  CSN is kept HIGH so the radio does not listen to the SPI bus.
    if (_cePin != _csnPin) {
        digitalWrite(_cePin, HIGH);
        delayMicroseconds(11); // 10 uS = Required CE time to initiate data transmission.
        digitalWrite(_cePin, LOW);
    }
    
    // Wait for transmission to succeed or fail.
    while (1) {
        
        delayMicroseconds(_transmissionRetryWaitMicros);
        
        statusReg = readRegister(STATUS);
        
        if (statusReg & _BV(TX_DS)) {
            writeRegister(STATUS, statusReg | _BV(TX_DS));   // Clear TX success flag.
            return 1;                                        // Return success.
        }
        else if (statusReg & _BV(MAX_RT)) {
            spiTransfer(WRITE_OPERATION, FLUSH_TX, NULL, 0); // Clear TX FIFO buffer.
            writeRegister(STATUS, statusReg | _BV(MAX_RT));  // Clear flag which indicates max retries has been reached.
            return 0;                                        // Return failure.
        }
    }
}

void NRFLite::startSend(uint8_t toRadioId, void* data, uint8_t length, SendType sendType)
{
    prepForTransmission(toRadioId, sendType);
    
    // Add data to the TX FIFO buffer, with or without an ACK request.
    if (sendType == NO_ACK) { spiTransfer(WRITE_OPERATION, W_TX_PAYLOAD_NO_ACK, data, length); }
    else                    { spiTransfer(WRITE_OPERATION, W_TX_PAYLOAD       , data, length); }
    
    // Start transmission.
    if (_cePin != _csnPin) {
        digitalWrite(_cePin, HIGH);
        delayMicroseconds(11); // 10 uS = Required CE time to initiate data transmission.
        digitalWrite(_cePin, LOW);
    }
}

void NRFLite::whatHappened(uint8_t& tx_ok, uint8_t& tx_fail, uint8_t& rx_ready)
{
    uint8_t statusReg = readRegister(STATUS);
    
    tx_ok = statusReg & _BV(TX_DS);
    tx_fail = statusReg & _BV(MAX_RT);
    rx_ready = statusReg & _BV(RX_DR);
    
    // When we need to see interrupt flags, we disable the logic here which clears them.
    // Programs that have an interrupt handler for the radio's IRQ pin will use 'whatHappened'
    // and if we don't disable this logic, it's not possible for us to check these flags.
    if (_enableInterruptFlagsReset) {
        writeRegister(STATUS, statusReg | _BV(TX_DS) | _BV(MAX_RT) | _BV(RX_DR));
    }
}

void NRFLite::powerDown()
{
    // If we have separate CE and CSN pins, we can gracefully stop listening or transmitting.
    if (_cePin != _csnPin) { digitalWrite(_cePin, LOW); }
    
    // Turn off the radio.  Only consumes around 900 nA in this state!
    writeRegister(CONFIG, readRegister(CONFIG) & ~_BV(PWR_UP));
}

void NRFLite::printDetails()
{
    printRegister("CONFIG", readRegister(CONFIG));
    printRegister("EN_AA", readRegister(EN_AA));
    printRegister("EN_RXADDR", readRegister(EN_RXADDR));
    printRegister("SETUP_AW", readRegister(SETUP_AW));
    printRegister("SETUP_RETR", readRegister(SETUP_RETR));
    printRegister("RF_CH", readRegister(RF_CH));
    printRegister("RF_SETUP", readRegister(RF_SETUP));
    printRegister("STATUS", readRegister(STATUS));
    printRegister("OBSERVE_TX", readRegister(OBSERVE_TX));
    printRegister("RX_PW_P0", readRegister(RX_PW_P0));
    printRegister("RX_PW_P1", readRegister(RX_PW_P1));
    printRegister("FIFO_STATUS", readRegister(FIFO_STATUS));
    printRegister("DYNPD", readRegister(DYNPD));
    printRegister("FEATURE", readRegister(FEATURE));
    
    uint8_t data[5];
    
    debug("TX_ADDR = ");
    readRegister(TX_ADDR, &data, 5);
    for (uint8_t i=0; i<5; i++) debug(data[i]);
    debugln();
    
    debug("RX_ADDR_P0 = ");
    readRegister(RX_ADDR_P0, &data, 5);
    for (uint8_t i=0; i<5; i++) debug(data[i]);
    debugln();
    
    debug("RX_ADDR_P1 = ");
    readRegister(RX_ADDR_P1, &data, 5);
    for (uint8_t i=0; i<5; i++) debug(data[i]);
    debugln();
    debugln();
}

/////////////////////
// Private methods //
/////////////////////

uint8_t NRFLite::getPipeOfFirstRxFifoPacket()
{
    // The pipe number is bits 3, 2, and 1.  So B1110 masks them and we shift right by 1 to get the pipe number.
    // Any value > 5 is not a pipe number.
    // 000-101 = Data Pipe Number
    //     110 = Not Used
    //     111 = RX FIFO Empty
    return (readRegister(STATUS) & B1110) >> 1;
}

uint8_t NRFLite::getRxFifoPacketLength()
{
    // Read the length of the first data packet sitting in the RX FIFO buffer.
    uint8_t dataLength;
    spiTransfer(READ_OPERATION, R_RX_PL_WID, &dataLength, 1);

    // As specified in the datasheet, we verify the data length is valid (0 - 32 bytes).
    if (dataLength > 32) {
        spiTransfer(WRITE_OPERATION, FLUSH_RX, NULL, 0); // Clear the invalid data in the RX FIFO buffer.
        return 0;
    }
    else {
        return dataLength;
    }
}

void NRFLite::prepForTransmission(uint8_t toRadioId, SendType sendType)
{
    // TX pipe address sets the destination radio for the data.
    // RX pipe 0 is special and needs the same address in order to receive auto-acknowledgment packets
    // from the destination radio.
    uint8_t address[5] = { 1, 2, 3, 4, toRadioId };
    writeRegister(TX_ADDR, &address, 5);
    writeRegister(RX_ADDR_P0, &address, 5);
    
    // Ensure radio is powered on and ready for TX operation.
    uint8_t originalConfigReg = readRegister(CONFIG);
    uint8_t newConfigReg = originalConfigReg & ~_BV(PRIM_RX) | _BV(PWR_UP);
    if (originalConfigReg != newConfigReg) {

        // In case the radio was in RX mode (powered on and listening), we'll put the radio into
        // Standby-I mode by setting CE LOW.  The radio cannot transition directly from RX to TX,
        // it must go through Standby-I first.
        if ((originalConfigReg & _BV(PRIM_RX)) && (originalConfigReg & _BV(PWR_UP))) {
            if (digitalRead(_cePin) == HIGH) { digitalWrite(_cePin, LOW); }
        }
        
        writeRegister(CONFIG, newConfigReg);
        
        // 1500 uS = Powered Off mode to Standby-I mode transition time + 130 uS Standby to TX.
        delayMicroseconds(1630);
    }
    
    // If RX FIFO buffer is full and we require an ACK, clear it so we can receive the ACK response.
    uint8_t fifoReg = readRegister(FIFO_STATUS);
    if (fifoReg & _BV(RX_FULL) && sendType == REQUIRE_ACK) {
        spiTransfer(WRITE_OPERATION, FLUSH_RX, NULL, 0);
    }
    
    // If TX FIFO buffer is full, we'll attempt to send all the packets it contains.
    if (fifoReg & _BV(FIFO_FULL)) {
        
        // We need to see radio interrupt flags here to determine if transmission was successful or not.
        // Programs that utilize the interrupt capability of this library call 'whatHappened'
        // in their radio IRQ pin handler to determine if a transmission succeeded or failed, and in this method we
        // clear the interrupt flags in the STATUS register of the radio. By setting '_enableInterruptReset' = 0,
        // we temporarily remove this functionality so we can react to the radio's interrupt flags here.
        _enableInterruptFlagsReset = 0;
        
        uint8_t statusReg;
        
        // While the TX FIFO buffer is not empty...
        while (!(fifoReg & _BV(TX_EMPTY))) {
            
            // Try sending a packet.
            digitalWrite(_cePin, HIGH);
            delayMicroseconds(11);       // 10 uS = Required CE time to initiate data transmission.
            digitalWrite(_cePin, LOW);
            
            delayMicroseconds(_transmissionRetryWaitMicros);
            
            statusReg = readRegister(STATUS);
            
            if (statusReg & _BV(TX_DS)) {
                writeRegister(STATUS, statusReg | _BV(TX_DS));   // Clear TX success flag.
            }
            else if (statusReg & _BV(MAX_RT)) {
                spiTransfer(WRITE_OPERATION, FLUSH_TX, NULL, 0); // Clear TX FIFO buffer.
                writeRegister(STATUS, statusReg | _BV(MAX_RT));  // Clear flag which indicates max retries has been reached.
            }

            fifoReg = readRegister(FIFO_STATUS);
        }
        
        _enableInterruptFlagsReset = 1;
    }
}

uint8_t NRFLite::readRegister(uint8_t regName)
{
    uint8_t data;
    readRegister(regName, &data, 1);
    return data;
}

void NRFLite::readRegister(uint8_t regName, void* data, uint8_t length)
{
    spiTransfer(READ_OPERATION, (R_REGISTER | (REGISTER_MASK & regName)), data, length);
}

void NRFLite::writeRegister(uint8_t regName, uint8_t data)
{
    writeRegister(regName, &data, 1);
}

void NRFLite::writeRegister(uint8_t regName, void* data, uint8_t length)
{
    spiTransfer(WRITE_OPERATION, (W_REGISTER | (REGISTER_MASK & regName)), data, length);
}

void NRFLite::spiTransfer(SpiTransferType transferType, uint8_t regName, void* data, uint8_t length)
{
    uint8_t* intData = reinterpret_cast<uint8_t*>(data);
    
    digitalWrite(_csnPin, LOW); // Signal radio it should begin listening to the SPI bus.
    
    #if defined(__AVR_ATtiny84__) || defined(__AVR_ATtiny85__)
    
    // ATtiny transfer with USI.
    
    usiTransfer(regName);
    for (uint8_t i = 0; i < length; ++i) {
        uint8_t newData = usiTransfer(intData[i]);
        if (transferType == READ_OPERATION) { intData[i] = newData; }
    }
    
    #else
    
    // ATmega transfer with the Arduino SPI library.
    
    SPI.transfer(regName);
    for (uint8_t i = 0; i < length; ++i) {
        uint8_t newData = SPI.transfer(intData[i]);
        if (transferType == READ_OPERATION) { intData[i] = newData; }
    }
    
    #endif
    
    digitalWrite(_csnPin, HIGH); // Stop radio from listening to the SPI bus.
}

uint8_t NRFLite::usiTransfer(uint8_t data)
{
    #if defined(__AVR_ATtiny84__) || defined(__AVR_ATtiny85__)
    
    USIDR = data;
    USISR = _BV(USIOIF);
    
    while((USISR & _BV(USIOIF)) == 0)
    {
        USICR = _BV(USIWM0) | _BV(USICS1) | _BV(USICLK) | _BV(USITC);
    }
    
    return USIDR;
    
    #endif
}

void NRFLite::printRegister(char* name, uint8_t reg)
{
    debug(name);
    debug(" = ");
    
    for (int i = 7; i >= 0; i--)
    {
        debug(bitRead(reg, i));
    }
    
    debugln();
}
