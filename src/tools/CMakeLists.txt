# Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

set(XTP_LIB_PATH ${CMAKE_SOURCE_DIR}/third_party/XTP_API_1.1.19.2_20190627/bin)
include_directories(${XTP_LIB_PATH}/include)
link_directories(${XTP_LIB_PATH}/linux_centos7)

set(CTP_LIB_PATH ${CMAKE_SOURCE_DIR}/third_party/v6.3.15_20190220_api_tradeapi_se_linux64)
include_directories(${CTP_LIB_PATH})
link_directories(${CTP_LIB_PATH})

add_executable(contract-collector contract_collector.cpp)
target_link_libraries(contract-collector thosttraderapi_se utils dl)

add_executable(send-order send_order.cpp)
target_link_libraries(send-order utils)

add_executable(cancel-order cancel_order.cpp)
target_link_libraries(cancel-order utils)

add_executable(query-position query_position.cpp)
target_link_libraries(query-position utils)

add_executable(etf-tool etf_tool.cpp)
target_link_libraries(etf-tool ${GATEWAY_LIB} utils xtptraderapi)
