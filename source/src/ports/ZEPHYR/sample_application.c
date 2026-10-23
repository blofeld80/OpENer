/*
 * Sample application (CONFIG_OPENER_SAMPLE_APPLICATION): the CIP hooks of OpENer's sample
 * application without non-volatile data. Assemblies 100 (input), 150 (output), 151 (configuration),
 * one exclusive owner, one input only and one listen only connection point; the output data is
 * mirrored to the input data. For demonstration and tests: a device provides its own hooks.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "opener_api.h"
#include "appcontype.h"
#include "cipidentity.h"
#include "cipqos.h"
#include "ciptcpipinterface.h"
#include "trace.h"

#define APP_INPUT_ASSEMBLY               100
#define APP_OUTPUT_ASSEMBLY              150
#define APP_CONFIG_ASSEMBLY              151
#define APP_HEARTBEAT_INPUT_ONLY_ASSEMBLY  152
#define APP_HEARTBEAT_LISTEN_ONLY_ASSEMBLY 153
#define APP_EXPLICIT_ASSEMBLY            154

static EipUint8 input_data[32];
static EipUint8 output_data[32];
static EipUint8 config_data[10];
static EipUint8 explicit_data[32];

EipStatus ApplicationInitialization(void)
{
	CreateAssemblyObject(APP_INPUT_ASSEMBLY, input_data, sizeof(input_data));
	CreateAssemblyObject(APP_OUTPUT_ASSEMBLY, output_data, sizeof(output_data));
	CreateAssemblyObject(APP_CONFIG_ASSEMBLY, config_data, sizeof(config_data));
	/* heartbeat assemblies of the input only and listen only connections */
	CreateAssemblyObject(APP_HEARTBEAT_INPUT_ONLY_ASSEMBLY, NULL, 0);
	CreateAssemblyObject(APP_HEARTBEAT_LISTEN_ONLY_ASSEMBLY, NULL, 0);
	CreateAssemblyObject(APP_EXPLICIT_ASSEMBLY, explicit_data, sizeof(explicit_data));

	ConfigureExclusiveOwnerConnectionPoint(0, APP_OUTPUT_ASSEMBLY, APP_INPUT_ASSEMBLY,
					       APP_CONFIG_ASSEMBLY);
	ConfigureInputOnlyConnectionPoint(0, APP_HEARTBEAT_INPUT_ONLY_ASSEMBLY, APP_INPUT_ASSEMBLY,
					  APP_CONFIG_ASSEMBLY);
	ConfigureListenOnlyConnectionPoint(0, APP_HEARTBEAT_LISTEN_ONLY_ASSEMBLY, APP_INPUT_ASSEMBLY,
					   APP_CONFIG_ASSEMBLY);
	return kEipStatusOk;
}

void HandleApplication(void)
{
}

void CheckIoConnectionEvent(unsigned int output_assembly_id, unsigned int input_assembly_id,
			    IoConnectionEvent io_connection_event)
{
	(void)output_assembly_id;
	(void)input_assembly_id;
	(void)io_connection_event;
}

EipStatus AfterAssemblyDataReceived(CipInstance *instance)
{
	switch (instance->instance_number) {
	case APP_OUTPUT_ASSEMBLY:
		/* mirror the output data to the inputs */
		memcpy(input_data, output_data, sizeof(input_data));
		break;
	case APP_EXPLICIT_ASSEMBLY:
	case APP_CONFIG_ASSEMBLY:
		/* the sample accepts any data */
		break;
	default:
		OPENER_TRACE_INFO("Unknown assembly instance in AfterAssemblyDataReceived\n");
		break;
	}
	return kEipStatusOk;
}

EipBool8 BeforeAssemblyDataSend(CipInstance *instance)
{
	/* nothing to refresh, the output data is mirrored on receive; true: the data is new */
	(void)instance;
	return true;
}

EipStatus ResetDevice(void)
{
	CloseAllConnections();
	CipQosUpdateUsedSetQosValues();
	return kEipStatusOk;
}

EipStatus ResetDeviceToInitialConfiguration(void)
{
	g_tcpip.encapsulation_inactivity_timeout = 120;
	CipQosResetAttributesToDefaultValues();
	(void)ResetDevice();
	return kEipStatusOk;
}

void *CipCalloc(size_t number_of_elements, size_t size_of_element)
{
	return calloc(number_of_elements, size_of_element);
}

void CipFree(void *data)
{
	free(data);
}

void RunIdleChanged(EipUint32 run_idle_value)
{
	OPENER_TRACE_INFO("Run/Idle handler triggered\n");
	if ((0x0001 & run_idle_value) == 1) {
		CipIdentitySetExtendedDeviceStatus(kAtLeastOneIoConnectionInRunMode);
	} else {
		CipIdentitySetExtendedDeviceStatus(kAtLeastOneIoConnectionEstablishedAllInIdleMode);
	}
}
