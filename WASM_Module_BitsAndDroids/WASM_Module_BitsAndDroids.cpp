#include <fstream>
#include <MSFS/MSFS_WindowsTypes.h>
#include <MSFS/MSFS.h>
#include <MSFS/Legacy/gauges.h>
#include <unistd.h>


#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "SimConnect.h"



/* Attempt to get SimConnect_CLientDataArea functionality implemented. */
HRESULT hr;
HANDLE hInputSimConnect = 0;
HANDLE hOutputSimConnect = 1;
SIMCONNECT_CLIENT_DATA_ID ClientDataID = 1;
SIMCONNECT_CLIENT_DATA_ID outputClientDataID = 2;

DWORD dwSize = 8;

const char* relativeEventFilePath = "modules/events.txt";


struct SimVar {
	int ID;
	int offset;
	std::string name;
	float value;
	float updateEvery;
	//MODE 0=input 1=setableVariable 2=NonSetableVariable 3=booleanVariable
	int mode;
};

std::vector<SimVar> SimVars;
std::map<int, SimVar> inputSimVars;




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
	EVENT_RANGE = 1,
	EVENT_6HZ = 2,
};

static enum GROUP_ID
{
	GROUP_A = 1,
};

// Definition of the client data area format
double data = 1.;
double outputData = 1.;

void readEventFile()
{
	std::ifstream file(relativeEventFilePath);
	std::string row;
	while (std::getline(file, row))
	{
		//This check ensures we don't take in account faulty or empty lines
		//It also enables us to add headers < 25 chars
		if (row.size() > 25) {
			int prefixDelimiter = row.find("#");
			int modeDelimiter = row.find('-');
			std::string rawName = row.substr(0, modeDelimiter);
			if (rawName.front() == ' ')
			{
				rawName.erase(0, 1);
			}
			float updateEveryRow = std::stof(row.substr(row.find('$') + 1));
			fprintf(stderr, "FLOAT %f", updateEveryRow);
			SimVar lineFound = {
				std::stoi(row.substr(prefixDelimiter + 1, 4)),
				sizeof(float) * SimVars.size(),
				rawName,
				0.0,
				updateEveryRow,
				std::stoi(row.substr(modeDelimiter + 1,1))
			};
			if (lineFound.mode == 0)
			{
				std::pair<int, SimVar> inputToAdd = { lineFound.ID, lineFound };
				inputSimVars.insert(inputToAdd);
			}
			else
			{
				SimVars.push_back(lineFound);
			}

			fprintf(stderr, "RAW LINE: %s", row.c_str());
			fprintf(stderr, "RAW NAME: %s", row.substr(0, modeDelimiter).c_str());
			fprintf(stderr, "READ %s ID %i", rawName.c_str(), lineFound.ID);

		}
	}
	file.close();
	fprintf(stderr, "FILE CLOSED");

}



void registerSimVars()
{
	for (auto& simVar : SimVars)
	{
		hr = SimConnect_AddToClientDataDefinition(
			hInputSimConnect,
			simVar.ID,
			simVar.offset,
			sizeof(float),
			simVar.updateEvery
		);
		fprintf(stderr, "Registered > %s ID [%u], offset(%u), value(%f)", simVar.name.c_str(), simVar.ID, simVar.offset, simVar.value);
	}
}
void writeSimVar(SimVar& simVar) {
	std::string sendString = std::to_string(simVar.ID) + std::to_string(simVar.value);

	HRESULT hr = SimConnect_SetClientData(
		hInputSimConnect,
		2,
		simVar.ID,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		sizeof(simVar.value),
		&simVar.value
	);
	fprintf(stderr, "SimVar %s ID %u value %f", simVar.name.c_str(), simVar.ID, simVar.value);

}
void readSimVar(SimVar& simVar) {
	FLOAT64 val = 0;

	ENUM test = get_units_enum(simVar.name.c_str());
	val = get_named_variable_typed_value(check_named_variable(simVar.name.c_str()), test);

	execute_calculator_code(simVar.name.c_str(), &val, nullptr, nullptr);

	if (simVar.value == val) return;
	simVar.value = val;

	writeSimVar(simVar);

	fprintf(stderr, "SimVar %s ID %u value %f", simVar.name.c_str(), simVar.ID, simVar.value);

}
void readSimVars()
{
	for (auto& simVar : SimVars)
	{
		readSimVar(simVar);
	}
}
// Define a SimConnect callback handler for custom events.
void CALLBACK myDispatchHandler(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext)
{
	// Check for any incoming custom events.
	DWORD requestID = pData->dwID;
	switch (requestID)
	{
	case SIMCONNECT_RECV_ID_EVENT:
	{
		auto evt = (SIMCONNECT_RECV_EVENT*)pData;
		// Detect what event was triggered.
		switch (evt->uEventID)
		{
			fprintf(stderr, "triggeredA.");

		case EVENT_RANGE:
		{
			fprintf(stderr, "triggeredB.");

			UINT32 evData = evt->dwData;
			fprintf(stderr, "Event data number = %i", evData);
			//ID lvarID = check_named_variable(MCDU_BUTTONS[evData]); // A32NX Range 1 (0...5).
			// Get the value
			//FLOAT64 lvarValue;
			//lvarValue = get_named_variable_value(lvarID);
			//data = lvarValue;
			//This for LVARS
			//set_named_variable_value(lvarID, 0);
			SimVar triggeredSimVar = inputSimVars[evData];
			execute_calculator_code(triggeredSimVar.name.c_str(), nullptr, nullptr, nullptr);

			SimConnect_SetClientData(hInputSimConnect, ClientDataID, DEFINITION_1,
				SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 1, sizeof(data), &data);

			break;
		}
		case EVENT_6HZ:
		{

			readSimVars();
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
	hr = SimConnect_Open(&hInputSimConnect, "incSimConnect", nullptr, 0, 0, 0);

	if (hr == S_OK)
	{
		fprintf(stderr, "WASM BitsAndDroids lvar-access module initialized.");
		fprintf(stderr, "mappedDataName = i");
		// Map an ID to the Client Data Area.dW
		hr = SimConnect_MapClientDataNameToID(hInputSimConnect, "shared", ClientDataID);
		fprintf(stderr, "mappedDataName = %i", hr);

		// Add a double to the data definition.
		hr &= SimConnect_AddToClientDataDefinition(hInputSimConnect, DEFINITION_1, SIMCONNECT_CLIENTDATAOFFSET_AUTO,
			sizeof(data));
		fprintf(stderr, "addToClientDataDefinition = %i", hr);
		// Set up a custom Client Data Area.
		hr &= SimConnect_CreateClientData(hInputSimConnect, ClientDataID, sizeof(data),
			SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED);
		fprintf(stderr, "createClientData = %i", hr);

		SimConnect_MapClientEventToSimEvent(hInputSimConnect, EVENT_RANGE, "LVAR_ACCESS.EFIS");

		hr &= SimConnect_AddClientEventToNotificationGroup(hInputSimConnect, GROUP_A, EVENT_RANGE, true);

		hr &= SimConnect_SetNotificationGroupPriority(hInputSimConnect, GROUP_A, SIMCONNECT_GROUP_PRIORITY_HIGHEST);

		//OUTPUTS

		registerSimVars();
		hr = SimConnect_MapClientDataNameToID(hInputSimConnect, "wasm.responses", outputClientDataID);


		hr = SimConnect_CreateClientData(hInputSimConnect, outputClientDataID, 4096, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);


		hr = SimConnect_AddToClientDataDefinition(hInputSimConnect, DEFINITION_OUTPUT_DATA, SIMCONNECT_CLIENTDATAOFFSET_AUTO, 256, 0);

		hr = SimConnect_SubscribeToSystemEvent(hInputSimConnect, EVENT_6HZ, "6Hz");



		SimConnect_CallDispatch(hInputSimConnect, myDispatchHandler, NULL);
	}
}


extern "C" MSFS_CALLBACK void module_deinit(void)
{
	unregister_key_event_handler((GAUGE_KEY_EVENT_HANDLER)myDispatchHandler, NULL);
	SimConnect_Close(hInputSimConnect);
	SimConnect_Close(hOutputSimConnect);
}
