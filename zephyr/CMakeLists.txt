if(CONFIG_OPENER)

set(OPENER_SRC ${ZEPHYR_CURRENT_MODULE_DIR}/source/src)
set(OPENER_PORT ${OPENER_SRC}/ports/ZEPHYR)
set(OPENER_GEN ${CMAKE_CURRENT_BINARY_DIR}/include)

# devicedata.h: the device identity from Kconfig, through the template of the CMake build
set(OpENer_Device_Config_Vendor_Id ${CONFIG_OPENER_DEVICE_VENDOR_ID})
set(OpENer_Device_Config_Device_Type ${CONFIG_OPENER_DEVICE_TYPE})
set(OpENer_Device_Config_Product_Code ${CONFIG_OPENER_DEVICE_PRODUCT_CODE})
set(OpENer_Device_Config_Device_Name "${CONFIG_OPENER_DEVICE_NAME}")
set(OpENer_Device_Major_Version ${CONFIG_OPENER_DEVICE_MAJOR_REVISION})
set(OpENer_Device_Minor_Version ${CONFIG_OPENER_DEVICE_MINOR_REVISION})
configure_file(${OPENER_SRC}/ports/devicedata.h.in ${OPENER_GEN}/devicedata.h)

# Headers and definitions are global: the application includes OpENer's headers and must see the
# same configuration as the stack.
zephyr_include_directories(
  ${OPENER_SRC}
  ${OPENER_SRC}/cip
  ${OPENER_SRC}/enet_encap
  ${OPENER_SRC}/utils
  ${OPENER_SRC}/ports
  ${OPENER_PORT}
  ${OPENER_GEN})

zephyr_compile_definitions(
  RESTRICT=__restrict
  PC_OPENER_ETHERNET_BUFFER_SIZE=${CONFIG_OPENER_ETHERNET_BUFFER_SIZE})
if(CONFIG_OPENER_CONSUMED_DATA_HAS_RUN_IDLE_HEADER)
  zephyr_compile_definitions(OPENER_CONSUMED_DATA_HAS_RUN_IDLE_HEADER)
endif()
if(CONFIG_OPENER_PRODUCED_DATA_HAS_RUN_IDLE_HEADER)
  zephyr_compile_definitions(OPENER_PRODUCED_DATA_HAS_RUN_IDLE_HEADER)
endif()
if(CONFIG_OPENER_RANDOMIZE_CONNECTION_ID)
  zephyr_compile_definitions(OPENER_RANDOMIZE_CONNECTION_ID)
endif()
if(CONFIG_OPENER_TRACE_LEVEL GREATER 0)
  zephyr_compile_definitions(OPENER_WITH_TRACES OPENER_TRACE_LEVEL=${CONFIG_OPENER_TRACE_LEVEL})
endif()

# Its own library, so that the stack's warnings stay apart from the application's.
zephyr_library_named(opener)
zephyr_library_sources(
  ${OPENER_SRC}/cip/appcontype.c
  ${OPENER_SRC}/cip/cipassembly.c
  ${OPENER_SRC}/cip/cipclass3connection.c
  ${OPENER_SRC}/cip/cipcommon.c
  ${OPENER_SRC}/cip/cipconnectionobject.c
  ${OPENER_SRC}/cip/cipconnectionmanager.c
  ${OPENER_SRC}/cip/cipdlr.c
  ${OPENER_SRC}/cip/cipelectronickey.c
  ${OPENER_SRC}/cip/cipepath.c
  ${OPENER_SRC}/cip/cipethernetlink.c
  ${OPENER_SRC}/cip/cipidentity.c
  ${OPENER_SRC}/cip/cipioconnection.c
  ${OPENER_SRC}/cip/cipmessagerouter.c
  ${OPENER_SRC}/cip/cipqos.c
  ${OPENER_SRC}/cip/cipstring.c
  ${OPENER_SRC}/cip/cipstringi.c
  ${OPENER_SRC}/cip/ciptcpipinterface.c
  ${OPENER_SRC}/cip/ciptypes.c
  ${OPENER_SRC}/enet_encap/cpf.c
  ${OPENER_SRC}/enet_encap/encap.c
  ${OPENER_SRC}/enet_encap/endianconv.c
  ${OPENER_SRC}/utils/doublylinkedlist.c
  ${OPENER_SRC}/utils/enipmessage.c
  ${OPENER_SRC}/utils/random.c
  ${OPENER_SRC}/utils/xorshiftrandom.c
  ${OPENER_SRC}/ports/socket_timer.c
  ${OPENER_PORT}/networkconfig.c
  ${OPENER_PORT}/networkhandler.c)

# endianconv.c includes <netinet/in.h> and <sys/socket.h>; both lead to the native mapping. Only
# the stack is compiled with this directory on its include path.
zephyr_library_include_directories(${OPENER_PORT}/compat)

add_dependencies(opener zephyr_generated_headers)

endif()
