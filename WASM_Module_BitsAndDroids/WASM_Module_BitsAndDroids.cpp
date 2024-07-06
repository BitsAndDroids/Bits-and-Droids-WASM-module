#include <MSFS/Legacy/gauges.h>
#include <MSFS/MSFS.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <fstream>
#include <unistd.h>
#include <wasi/libc.h>

#include <iostream>
#include <map>

#include <vector>

#include "SimConnect.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"

using namespace rapidjson;

HRESULT hr;
HANDLE hSimConnect = 0;

SIMCONNECT_CLIENT_DATA_ID clientDataID = 1;
SIMCONNECT_CLIENT_DATA_ID outputClientDataID = 2;
SIMCONNECT_CLIENT_DATA_ID commandCLientDataID = 3;

DWORD dwSize = 8;

const char *relativeJSONEventFilePath = "modules/wasm_events.json";

struct WASMEvent {
  int id;
  std::string action;
  std::string action_text;
  std::string action_type;
  std::string output_format;
  FLOAT32 update_every;
  FLOAT32 min;
  FLOAT32 max;
  FLOAT64 value;
  int offset;
};

struct receivedString {
  char value[256];
};

std::map<int, WASMEvent> wasmEvents;
std::map<int, WASMEvent> registeredWASMEvents;

static enum DATA_DEFINE_ID {
  DEFINITION_INPUTS = 101,
  DEFINITION_OUTPUT_DATA = 103,
  DEFINITION_COMMANDS = 105
};

static enum DATA_REQUEST_ID {
  INPUT_REQUEST_ID = 102,
  OUTPUT_REQUEST_ID = 104,
  COMMAND_REQUEST_ID = 106
};

static enum EVENT_ID {
  EVENT_INPUT = 1,
  EVENT_6HZ = 2,
  EVENT_FLIGHTSTART = 3,
  EVENT_SIMLOAD = 4
};

static enum GROUP_ID {
  GROUP_A = 1,
};

// Definition of the client data area format
double data = 1.;
double outputData = 1.;

int valueToAxis(float min, float max, int value) {
  return -16383.0 + (16383.0 - -16383.0) * ((value - min) / (max - min));
}

void register_event(std::string event_message) {
  Document jsonEvent;
  if (jsonEvent.Parse(event_message.c_str()).HasParseError()) {
    fprintf(stderr, "Error converting %s", event_message.c_str());
    return;
  }
  WASMEvent event = {jsonEvent["id"].GetInt(),
                     jsonEvent["action"].GetString(),
                     jsonEvent["action_text"].GetString(),
                     jsonEvent["action_type"].GetString(),
                     jsonEvent["output_format"].GetString(),
                     jsonEvent["update_every"].GetFloat(),
                     jsonEvent["min"].GetFloat(),
                     jsonEvent["max"].GetFloat(),
                     0.0,
                     jsonEvent["offset"].GetInt()};

  if (event.action_type == "output") {
    std::cout << "Adding event to outputs " << event.id << " "
              << event.action.c_str() << std::endl;
    if (registeredWASMEvents.count(event.id) > 0) {
      registeredWASMEvents[event.id] = event;
    } else {
      registeredWASMEvents.insert({event.id, event});
    }
    hr = SimConnect_AddToClientDataDefinition(hSimConnect, event.id,
                                              event.offset, sizeof(FLOAT64),
                                              event.update_every);

  } else {
    std::cout << "Adding event to inputs " << event.id << " "
              << event.action.c_str() << std::endl;
    if (wasmEvents.count(event.id) > 0) {
      wasmEvents[event.id] = event;
    } else {
      wasmEvents.insert({event.id, event});
    }
  }
}

void clear_sim_vars() {
  for (auto &event : registeredWASMEvents) {
    std::cout << "Unregistering event_id: " << event.second.id << std::endl;
    SimConnect_ClearClientDataDefinition(hSimConnect, event.second.id);
  }
}

void writeSimVar(WASMEvent &wasm_event) {

  HRESULT hr = SimConnect_SetClientData(
      hSimConnect, 2, wasm_event.id, SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 0,
      sizeof(wasm_event.value), &wasm_event.value);
  std::cout << "Set id: " << wasm_event.id << " | value: " << wasm_event.value
            << std::endl;
}

void readSimVar(WASMEvent &simVar, bool forced) {
  FLOAT64 val = 0.0;

  ENUM test = get_units_enum(simVar.action.c_str());
  val = get_named_variable_typed_value(
      check_named_variable(simVar.action.c_str()), test);

  execute_calculator_code(simVar.action.c_str(), &val, nullptr, nullptr);
  if (!forced) {
    if (simVar.value == val || abs(simVar.value - val) < simVar.update_every) {
      return;
    }
  }
  simVar.value = val;
  writeSimVar(simVar);
}

void readSimVars(bool forced) {
  for (auto it = registeredWASMEvents.begin(); it != registeredWASMEvents.end();
       ++it) {
    readSimVar(it->second, forced);
  }
}

void CALLBACK myDispatchHandler(SIMCONNECT_RECV *pData, DWORD cbData,
                                void *pContext) {
  // Check for any incoming custom events.
  DWORD requestID = pData->dwID;
  switch (requestID) {
  case SIMCONNECT_RECV_ID_CLIENT_DATA: {
    auto *pObjData = (SIMCONNECT_RECV_CLIENT_DATA *)(pData);
    std::string stringReceived = (char *)&pObjData->dwData;
    if (pObjData->dwDefineID == DEFINITION_COMMANDS) {
      if (strcmp(stringReceived.c_str(), "clear") == 0) {
        clear_sim_vars();
        return;
      }
      std::cout << "COMMAND RECEIVED: " << stringReceived << std::endl;
      register_event(stringReceived);
      return;
    }

    int prefix = std::stoi(stringReceived.substr(0, 4));
    auto event_it = wasmEvents.find(prefix);
    if (event_it == wasmEvents.end()) {
      fprintf(stderr, "could not find event: %u", prefix);
      return;
    }
    auto event_found = event_it->second;

    if (event_found.action_type == "input") {
      const int value = std::stoi(
          stringReceived.substr(stringReceived.find(" "), std::string::npos));
      // const std::string tempString =
      // 	std::to_string(value) + " + " + event_found.action;
      std::cout << "Input id: " << event_found.id << " | Value: " << value
                << std::endl;
      execute_calculator_code(event_found.action.c_str(), nullptr, nullptr,
                              nullptr);
      break;
    } else if (event_found.action_type == "axis") {
      int receivedValue = std::stoi(
          stringReceived.substr(stringReceived.find(" "), std::string::npos));
      const int value =
          valueToAxis(event_found.min, event_found.max, receivedValue);
      const std::string tempString =
          std::to_string(value) + " + " + event_found.action;
      execute_calculator_code(tempString.c_str(), nullptr, nullptr, nullptr);
      break;
    }

    else if (event_found.action_type == "command") {
      if (event_found.id == 9999) {
        wasmEvents.clear();
        break;
      }
      if (prefix == 9998) {
        readSimVars(true);
        break;
      }
    }

    SimConnect_SetClientData(hSimConnect, clientDataID, DEFINITION_INPUTS,
                             SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 1,
                             sizeof(data), &data);

    execute_calculator_code(event_found.action.c_str(), nullptr, nullptr,
                            nullptr);
    break;
  }
  case SIMCONNECT_RECV_ID_EVENT_FRAME: {
    SimConnect_CallDispatch(hSimConnect, myDispatchHandler, NULL);
    readSimVars(false);
    break;
  }
  case SIMCONNECT_RECV_ID_EVENT: {
    auto evt = static_cast<SIMCONNECT_RECV_EVENT *>(pData);
    // Detect what event was triggered.

    switch (evt->uEventID) {
    case EVENT_INPUT: {
      UINT32 evData = evt->dwData;

      WASMEvent event_found = wasmEvents[evData];
      if (event_found.action_type.c_str() == "input") {
      }
      execute_calculator_code(event_found.action.c_str(), nullptr, nullptr,
                              nullptr);

      SimConnect_SetClientData(hSimConnect, clientDataID, DEFINITION_INPUTS,
                               SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 1,
                               sizeof(data), &data);

      break;
    }
    case EVENT_6HZ: {
      std::cout << "FRAME" << std::endl;
      SimConnect_CallDispatch(hSimConnect, myDispatchHandler, NULL);
      readSimVars(false);
      break;
    }
    case EVENT_FLIGHTSTART: {
      std::cout << "New flight loaded, resend values" << std::endl;
      readSimVars(true);
      break;
    }
    case EVENT_SIMLOAD: {
      std::cout << "Sim loaded, resend values" << std::endl;
      readSimVars(true);
      break;
    }
    default: {
      // No default for now.
      break;
    }
    }
  }
  }
}

// This is called when the WASM is loaded into the system.
extern "C" MSFS_CALLBACK void module_init(void) {
  // register_key_event_handler((GAUGE_KEY_EVENT_HANDLER)myDispatchHandler,
  // NULL);

  /* SimConnect_ClientDataArea attempt. */
  hr = SimConnect_Open(&hSimConnect, "incSimConnect", nullptr, 0, 0, 0);

  if (hr == S_OK) {
    std::cout << "WASM BitsAndDroids module initialized" << std::endl;

    // Map an ID to the Client Data Area.dW
    SimConnect_MapClientDataNameToID(hSimConnect, "shared", clientDataID);

    // Set up a custom Client Data Area.
    SimConnect_CreateClientData(hSimConnect, clientDataID, 4096,
                                SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT);

    SimConnect_AddToClientDataDefinition(hSimConnect, DEFINITION_INPUTS,
                                         SIMCONNECT_CLIENTDATAOFFSET_AUTO, 256,
                                         0);

    SimConnect_RequestClientData(
        hSimConnect, clientDataID, INPUT_REQUEST_ID, DEFINITION_INPUTS,
        SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
        SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);

    SimConnect_MapClientDataNameToID(hSimConnect, "messages",
                                     outputClientDataID);

    SimConnect_CreateClientData(hSimConnect, outputClientDataID, 4096,
                                SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

    SimConnect_AddToClientDataDefinition(hSimConnect, DEFINITION_OUTPUT_DATA,
                                         SIMCONNECT_CLIENTDATAOFFSET_AUTO, 256,
                                         0);

    // COMMANDS
    SimConnect_MapClientDataNameToID(hSimConnect, "command_client",
                                     commandCLientDataID);
    SimConnect_CreateClientData(hSimConnect, commandCLientDataID, 4096,
                                SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT);
    SimConnect_AddToClientDataDefinition(hSimConnect, DEFINITION_COMMANDS, 0,
                                         4096, 0);
    SimConnect_RequestClientData(
        hSimConnect, commandCLientDataID, COMMAND_REQUEST_ID,
        DEFINITION_COMMANDS, SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
        SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED, 0, 0, 0);

    SimConnect_SubscribeToSystemEvent(hSimConnect, EVENT_6HZ, "Frame");
    SimConnect_SubscribeToSystemEvent(hSimConnect, EVENT_FLIGHTSTART,
                                      "FlightLoaded");
    SimConnect_SubscribeToSystemEvent(hSimConnect, EVENT_SIMLOAD, "SimStart");

    SimConnect_CallDispatch(hSimConnect, myDispatchHandler, NULL);
  }
}

extern "C" MSFS_CALLBACK void module_deinit(void) {
  SimConnect_Close(hSimConnect);
}
