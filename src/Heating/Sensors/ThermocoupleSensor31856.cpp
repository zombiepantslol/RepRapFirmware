/*
 * ThermocoupleSensor31856.cpp
 *
 *  Created on: 27 Jun 2017
 *      Author: David
 */

#include "ThermocoupleSensor31856.h"

#if SUPPORT_SPI_SENSORS

#include <Platform/RepRap.h>
#include <Platform/Platform.h>
#include <GCodes/GCodeBuffer/GCodeBuffer.h>

#if SUPPORT_REMOTE_COMMANDS
# include <CanMessageGenericParser.h>
#endif

const uint32_t MAX31856_Frequency = 4000000;	// maximum for MAX31865 is 5MHz

static const char * const TypeLetters = "BEJKNRST";		// MAX31856 mapping of AVGSEWL bits to thermocouple types
const uint8_t TypeK = 3;

// SPI modes:
// If the inactive state of SCL is LOW (CPOL = 0): (in the case of the MAX31865, this is sampled on the falling edge of CS):
// The MAX31856 changes data after the rising edge of CLK, and samples input data on the falling edge.
// This requires NCPHA = 0.
const SpiMode MAX31856_SpiMode = SpiMode::mode1;

// Define the minimum interval between readings
const uint32_t MinimumReadInterval = 100;		// minimum interval between reads, in milliseconds

// Default configuration registers.
// CR0:
//  CMODE=1		continuous conversion
//  1shot=0		no 1-shot conversion
//  OCFAULT=01	open circuit detection enabled, input resistance <5K
//  CJ=0		cold junction temperature sensing enabled
//  FAULT=1		fault bit and output remain set until cleared explicitly
//  FAULTCLR=1	clear any existing fault
//  50/60Hz=1	reject 50Hz (configurable via F parameter)
const uint8_t DefaultCr0 = 0b10010111;
const uint8_t Cr0ReadMask = 0b10111101;			// bits 1 and 6 auto clear, so ignore the value read

// CR1:
//  Reserved=0
//  AVGSEL=010	4 samples averaged, takes about 80ms
//  TCTYPE=0011	K type thermocouple
const uint8_t DefaultCr1 = 0b00100000;			// the thermocouple type needs to be or'd in
const uint8_t Cr1ReadMask = 0b01111111;			// ignore the reserved bits

// MASK:
//  reserved=00
//  CJhigh = 1	don't assert fault on cold junction temperature high
//  CJlow = 1	don't assert fault on cold junction temperature low
//  TChigh = 1	don't assert fault on thermocouple temperature high
//  TClow = 1	don't assert fault on thermocouple temperature low
//	OV/UV=0		assert fault on under/over voltage
//  Openfault=0	assert fault on open circuit condition
const uint8_t DefaultFaultMask = 0b00111100;

ThermocoupleSensor31856::ThermocoupleSensor31856(unsigned int sensorNum) noexcept
	: SpiTemperatureSensor(sensorNum, "Thermocouple (MAX31856)", MAX31856_SpiMode, MAX31856_Frequency),
	  cr0(DefaultCr0), thermocoupleType(TypeK)
{
}

// Configure this temperature sensor
GCodeResult ThermocoupleSensor31856::Configure(GCodeBuffer& gb, const StringRef& reply, bool& changed)
{
	if (!ConfigurePort(gb, reply, changed))
	{
		return GCodeResult::error;
	}
	TryConfigureSensorName(gb, changed);
	if (gb.Seen('F'))
	{
		changed = true;
		if (gb.GetIValue() == 60)
		{
			cr0 &= ~0x01;		// set 60Hz rejection
		}
		else
		{
			cr0 |= 0x01;		// default to 50Hz rejection
		}
	}

	String<2> buf;
	if (gb.TryGetQuotedString('K', buf.GetRef(), changed))
	{
		const char *p;
		if (buf.strlen() == 1 && (p = strchr(TypeLetters, toupper(buf.c_str()[0]))) != nullptr)
		{
			thermocoupleType = p - TypeLetters;
		}
		else
		{
			reply.copy("Bad thermocouple type letter in M305 command");
			return GCodeResult::error;
		}
	}

	return FinishConfiguring(changed, reply);
}

#if SUPPORT_REMOTE_COMMANDS

GCodeResult ThermocoupleSensor31856::Configure(const CanMessageGenericParser& parser, const StringRef& reply) noexcept
{
	bool seen = false;
	if (!ConfigurePort(parser, reply, seen))
	{
		return GCodeResult::error;
	}

	uint8_t paramF;
	if (parser.GetUintParam('F', paramF))
	{
		seen = true;
		if (paramF == 60)
		{
			cr0 &= ~0x01;		// set 60Hz rejection
		}
		else
		{
			cr0 |= 0x01;		// default to 50Hz rejection
		}
	}

	char paramK;
	if (parser.GetCharParam('K', paramK))
	{
		seen = true;
		const char *p;
		if ((p = strchr(TypeLetters, toupper(paramK))) != nullptr)
		{
			thermocoupleType = p - TypeLetters;
		}
		else
		{
			reply.copy("Bad thermocouple type letter in M305 command");
			return GCodeResult::error;
		}
	}

	return FinishConfiguring(seen, reply);
}

#endif

GCodeResult ThermocoupleSensor31856::FinishConfiguring(bool changed, const StringRef& reply) noexcept
{
	if (changed)
	{
		// Initialise the sensor
		InitSpi();

		TemperatureError rslt(TemperatureError::unknownError);
		for (unsigned int i = 0; i < 3; ++i)		// try 3 times
		{
			rslt = TryInitThermocouple();
			if (rslt == TemperatureError::ok)
			{
				break;
			}
			delay(MinimumReadInterval);
		}

		SetResult(0.0, rslt);
		if (rslt != TemperatureError::ok)
		{
			reply.printf("Failed to initialise thermocouple: %s", rslt.ToString());
			return GCodeResult::error;
		}
	}
	else
	{
		CopyBasicDetails(reply);
		reply.catf(", thermocouple type %c, reject %dHz", TypeLetters[thermocoupleType], (cr0 & 0x01) ? 50 : 60);
	}
	return GCodeResult::ok;
}

TemperatureError ThermocoupleSensor31856::TryInitThermocouple() const noexcept
{
	const uint8_t modeData[4] = { 0x80, cr0, (uint8_t)(DefaultCr1 | thermocoupleType), DefaultFaultMask };		// write registers 0, 1, 2
	uint32_t rawVal;
	TemperatureError sts = DoSpiTransaction(modeData, ARRAY_SIZE(modeData), rawVal);

	if (sts == TemperatureError::ok)
	{
		static const uint8_t readData[4] = { 0x00, 0x00, 0x00, 0x00 };		// read registers 0, 1, 2
		sts = DoSpiTransaction(readData, ARRAY_SIZE(readData), rawVal);
	}

	//debugPrintf("Status %d data %04x\n", (int)sts, rawVal);
	if (sts == TemperatureError::ok)
	{
		const uint32_t expectedResponseMask = (0b10111101 << 16)	// bits 1 and 5 of CR0 auto-clear
											| (0b01111111 << 8)		// ignore the reserved bit
											| (0b00111111);			// ignore the reserved bits
		if ((rawVal & expectedResponseMask) != (((modeData[1] << 16) | (modeData[2] << 8) | modeData[3]) & expectedResponseMask))
		{
//			debugPrintf("expected %08x got %08x\n", rawVal & expectedResponseMask, ((modeData[1] << 16) | (modeData[2] << 8) | modeData[3]) & expectedResponseMask);
			sts = TemperatureError::badResponse;
		}
	}

	return sts;
}

void ThermocoupleSensor31856::Poll() noexcept
{
	static const uint8_t dataOut[5] = {0x0C, 0x55, 0x55, 0x55, 0x55};	// read registers LTCB0, LTCB1, LTCB2, Fault status
	uint32_t rawVal;
	TemperatureError sts = DoSpiTransaction(dataOut, ARRAY_SIZE(dataOut), rawVal);

	if (sts != TemperatureError::ok)
	{
		SetResult(sts);
	}
	else
	{
		if ((rawVal & 0x00FF) != 0)
		{
			// One or more fault bits is set
			SetResult( (rawVal & 0x02) ? TemperatureError::overOrUnderVoltage
						: (rawVal & 0x01) ? TemperatureError::openCircuit
							: TemperatureError::hardwareError
					 );
			delayMicroseconds(1);										// MAX31856 requires CS to be high for 400ns minimum
			TryInitThermocouple();										// clear fault bits and re-initialise
		}
		else
		{
			const int16_t rawTemp = (int16_t)(rawVal >> 16);			// keep just the most significant 2 bytes and interpret them as signed
			SetResult((float)rawTemp / 16, TemperatureError::ok);
		}
	}
}

#endif // SUPPORT_SPI_SENSORS

// End
