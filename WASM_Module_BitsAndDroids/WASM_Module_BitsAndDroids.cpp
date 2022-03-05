#include <fstream>
#include <MSFS/MSFS_WindowsTypes.h>
#include <MSFS/MSFS.h>
#include <MSFS/Legacy/gauges.h>
#include <unistd.h>


#include <iostream>
#include <map>

#include <vector>

#include "SimConnect.h"


HRESULT hr;
HANDLE hSimConnect = 0;
SIMCONNECT_CLIENT_DATA_ID ClientDataID = 1;
SIMCONNECT_CLIENT_DATA_ID outputClientDataID = 2;


DWORD dwSize = 8;

const char* relativeEventFilePath = "modules/events.txt";

struct receivedString
{
	char value[256];
};

struct SimVar
{
	int ID;
	int offset;
	std::string name;
	float value;
	float updateEvery;
	//MODE 0=input 1=setableVariable 2=NonSetableVariable 3=booleanVariable
	int mode;
};

struct AxisEvent
{
	int ID;
	std::string name;
	int minRange;
	int maxRange;
};

std::vector<SimVar> SimVars;
std::map<int, SimVar> inputSimVars;
std::map<int, AxisEvent> axisEvents;


static enum DATA_DEFINE_ID
{
	DEFINITION_1 = 12,
	DEFINITION_OUTPUT_DATA = 13
};

static enum DATA_REQUEST_ID
{
	REQUEST_1 = 10,
};

static enum EVENT_ID
{
	EVENT_INPUT = 1,
	EVENT_6HZ = 2,
	EVENT_FLIGHTSTART = 3,
	EVENT_SIMLOAD = 4
};

static enum GROUP_ID
{
	GROUP_A = 1,
};

// Definition of the client data area format
double data = 1.;
double outputData = 1.;

int valueToAxis(float min, float max, int value)
{
	return -16383.0 + (16383.0 - -16383.0) * ((value - min) / (max - min));
}

void readEventFile()
{
	std::ifstream file(relativeEventFilePath);
	std::string row;
	int outputCounter = 0;
	while (std::getline(file, row))
	{
		//This check ensures we don't take in account faulty or empty lines
		//It also enables us to add headers with the / prefix
		if (row.size() > 25 && row.at(0) != '/')
		{
			//Format of row is command-type#prefix/id(4 chars)$updateEvery(float)

			int prefixDelimiter = row.find('#');
			int modeDelimiter = row.find('^');

			//String formatting
			int id = std::stoi(row.substr(prefixDelimiter + 1, 4));
			float updateEveryRow = std::stof(row.substr(row.find('$') + 1, (row.find('/') - row.find('$'))));
			int mode = std::stoi(row.substr(modeDelimiter + 1, 1));
			fprintf(stderr, "FLOAT %f", updateEveryRow);


			//Get rid of leading space if present
			std::string rawName = row.substr(0, modeDelimiter);
			if (rawName.front() == ' ')
			{
				rawName.erase(0, 1);
			}

			SimVar lineFound = {
				id,
				sizeof(float) * outputCounter,
				rawName,
				0.0,
				updateEveryRow,
				mode
			};

			//mode 0 + 1 tells us its an input command which doesn't need registering
			if (lineFound.mode == 0 || lineFound.mode == 1)
			{
				std::pair<int, SimVar> inputToAdd = { lineFound.ID, lineFound };
				inputSimVars.insert(inputToAdd);
			}
			else if (lineFound.mode == 9)
			{
				int minValue = std::stoi(row.substr(row.find('-') + 1, row.find('+') - row.find('-')));
				int maxValue = std::stoi(row.substr(row.find('+'), row.find('$') - row.find('+')));
				axisEvents.insert({ lineFound.ID, {lineFound.ID, lineFound.name, minValue, maxValue} });
			}
			else
			{
				SimVars.push_back(lineFound);
				outputCounter++;
			}

			//Prints errors but these are just there for debugging purposes
			fprintf(stderr, "RAW LINE: %s", row.c_str());
			fprintf(stderr, "RAW NAME: %s", row.substr(0, modeDelimiter).c_str());
			fprintf(stderr, "READ %s ID %i", rawName.c_str(), lineFound.ID);
		}

	}

	file.close();
	fprintf(stderr, "FILE CLOSED");
}
void writeSimVar(SimVar& simVar)
{
	std::string sendString = std::to_string(simVar.ID) + std::to_string(simVar.value);

	HRESULT hr = SimConnect_SetClientData(
		hSimConnect,
		2,
		simVar.ID,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		sizeof(simVar.value),
		&simVar.value
	);
	fprintf(stderr, "SimVar %s ID %u value %f, offset %i, updateEvery %f", simVar.name.c_str(), simVar.ID, simVar.value, simVar.offset, simVar.updateEvery);
}
void readSimVar(SimVar& simVar, bool forced)
{
	FLOAT64 val = 0.0;

	ENUM test = get_units_enum(simVar.name.c_str());
	val = get_named_variable_typed_value(check_named_variable(simVar.name.c_str()), test);

	execute_calculator_code(simVar.name.c_str(), &val, nullptr, nullptr);
	if (!forced) {
		if (simVar.value == val || abs(simVar.value - val) < simVar.updateEvery) return;
	}
	simVar.value = val;
	writeSimVar(simVar);

	fprintf(stderr, "SimVar %s ID %u value %f", simVar.name.c_str(), simVar.ID, simVar.value);
}

void readSimVars(bool forced)
{
	for (auto& simVar : SimVars)
	{
		readSimVar(simVar, forced);
	}
}
void registerSimVars()
{
	for (auto& simVar : SimVars)
	{
		hr = SimConnect_AddToClientDataDefinition(
			hSimConnect,
			simVar.ID,
			simVar.offset,
			sizeof(float),
			simVar.updateEvery
		);
		fprintf(stderr, "Registered > %s ID [%u], offset(%u), value(%f), updateEvery(%f)", simVar.name.c_str(), simVar.ID, simVar.offset,
			simVar.value, simVar.updateEvery);
	}
	readSimVars(true);
}






void CALLBACK myDispatchHandler(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext)
{
	// Check for any incoming custom events.
	DWORD requestID = pData->dwID;
	switch (requestID)
	{
	case SIMCONNECT_RECV_ID_CLIENT_DATA:
	{
		fprintf(stderr, "clientData Received ");
		auto* pObjData = (SIMCONNECT_RECV_CLIENT_DATA*)(pData);
		std::string stringReceived = (char*)&pObjData->dwData;

		int prefix = std::stoi(stringReceived.substr(0, 4));
		if (prefix >= 3000 and prefix < 4000)
		{
			AxisEvent receivedAxisEvent = axisEvents[prefix];
			int receivedValue = std::stoi(stringReceived.substr(stringReceived.find(" "), std::string::npos));
			const int value = valueToAxis(
				receivedAxisEvent.minRange,
				receivedAxisEvent.maxRange,
				receivedValue
			);
			const std::string tempString = std::to_string(value) + " + " + receivedAxisEvent.name;
			fprintf(stderr, "RECEIVED VALUE, %i, AXIS VALUE %i, PREFIX, %i, MIN, %i, MAX, %i", receivedValue, value, prefix, receivedAxisEvent.minRange, receivedAxisEvent.maxRange);
			execute_calculator_code(tempString.c_str(), nullptr, nullptr, nullptr);
			break;


		}
		else {
			SimVar receivedSimVar = inputSimVars[prefix];
			if (prefix == 9999)
			{
				inputSimVars.clear();
				SimVars.clear();
				axisEvents.clear();
				readEventFile();
				break;
			}
			if (prefix == 9998)
			{
				fprintf(stderr, "9998 received, resend values");
				readSimVars(true);
				break;
			}
			if (receivedSimVar.mode == 1)
			{
				const int value = std::stoi(stringReceived.substr(stringReceived.find(" "), std::string::npos));
				const std::string tempString = std::to_string(value) + " + " + receivedSimVar.name;
				execute_calculator_code(tempString.c_str(), nullptr, nullptr, nullptr);
				break;
			}
			SimConnect_SetClientData(hSimConnect, ClientDataID, 12,
				SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 1, sizeof(data), &data);

			execute_calculator_code(receivedSimVar.name.c_str(), nullptr, nullptr, nullptr);
		}
		fprintf(stderr, "RECEIVED: %s", stringReceived.c_str());
		fprintf(stderr, "RECEIVED: %i", pObjData->dwRequestID);
		break;
	}

	case SIMCONNECT_RECV_ID_EVENT:
	{
		auto evt = static_cast<SIMCONNECT_RECV_EVENT*>(pData);
		// Detect what event was triggered.

		switch (evt->uEventID)
		{
		case EVENT_INPUT:
		{


			UINT32 evData = evt->dwData;
			fprintf(stderr, "Event data number = %i", evData);

			SimVar triggeredSimVar = inputSimVars[evData];
			if (triggeredSimVar.mode == 1)
			{
			}
			execute_calculator_code(triggeredSimVar.name.c_str(), nullptr, nullptr, nullptr);

			SimConnect_SetClientData(hSimConnect, ClientDataID, DEFINITION_1,
				SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 1, sizeof(data), &data);

			break;
		}
		case EVENT_6HZ:
		{
			SimConnect_CallDispatch(hSimConnect, myDispatchHandler, NULL);
			readSimVars(false);
			break;
		}
		case EVENT_FLIGHTSTART:
		{
			fprintf(stderr, "New flight loaded, resend values");
			readSimVars(true);
			break;
		}
		case EVENT_SIMLOAD:
		{
			fprintf(stderr, "Sim loaded, resend values");
			readSimVars(true);
			break;
		}
		default:
		{
			// No default for now.
			break;
		}
		}
	}
	}
}


// This is called when the WASM is loaded into the system.
extern "C" MSFS_CALLBACK void module_init(void)
{
	//register_key_event_handler((GAUGE_KEY_EVENT_HANDLER)myDispatchHandler, NULL);
	readEventFile();
	/* SimConnect_ClientDataArea attempt. */
	hr = SimConnect_Open(&hSimConnect, "incSimConnect", nullptr, 0, 0, 0);

	if (hr == S_OK)
	{
		fprintf(stderr, "WASM BitsAndDroids module initialized.");

		// Map an ID to the Client Data Area.dW
		SimConnect_MapClientDataNameToID(hSimConnect, "shared", ClientDataID);
		fprintf(stderr, "mappedDataName = %i", hr);

		// Add a double to the data definition.


		fprintf(stderr, "addToClientDataDefinition = %i", hr);

		// Set up a custom Client Data Area.
		SimConnect_CreateClientData(hSimConnect, ClientDataID, 4096,
			SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT);

		SimConnect_AddToClientDataDefinition(hSimConnect, DEFINITION_1, SIMCONNECT_CLIENTDATAOFFSET_AUTO,
			256, 0);

		fprintf(stderr, "createClientData = %i", hr);


		SimConnect_RequestClientData(hSimConnect,
			1,
			0,
			12,
			SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
			SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT,
			0,
			0,
			0);


		//OUTPUTS
		registerSimVars();
		SimConnect_MapClientDataNameToID(hSimConnect, "wasm.responses", outputClientDataID);

		SimConnect_CreateClientData(hSimConnect, outputClientDataID, 4096,
			SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

		SimConnect_AddToClientDataDefinition(hSimConnect, DEFINITION_OUTPUT_DATA, SIMCONNECT_CLIENTDATAOFFSET_AUTO,
			256, 0);

		SimConnect_SubscribeToSystemEvent(hSimConnect, EVENT_6HZ, "6Hz");
		SimConnect_SubscribeToSystemEvent(hSimConnect, EVENT_FLIGHTSTART, "FlightLoaded");
		SimConnect_SubscribeToSystemEvent(hSimConnect, EVENT_SIMLOAD, "SimStart");


		SimConnect_CallDispatch(hSimConnect, myDispatchHandler, NULL);
	}
}


extern "C" MSFS_CALLBACK void module_deinit(void)
{
	SimConnect_Close(hSimConnect);
}
