IF(NOT DEFINED CAP_INSTALL_DIR)
    SET(CAP_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/cap")
ENDIF()

EXTERNALPROJECT_ADD(
    CAP
    SOURCE_DIR         ${CAP_SOURCE_DIR}
    INSTALL_DIR        ${CAP_INSTALL_DIR} 
    CMAKE_ARGS         -D CMAKE_INSTALL_PREFIX=${CAP_INSTALL_DIR}
                       -D CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                       -D CMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
                       -D CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                       -D DEAL_II_INSTALL_DIR=${DEAL_II_INSTALL_DIR}
                       -D CAP_DATA_DIR=${CAP_DATA_SOURCE_DIR}
    INSTALL_COMMAND    ""
    BUILD_IN_SOURCE    0
    DEPENDS            DEAL_II EIGEN
)
FILE(APPEND "${CMAKE_INSTALL_PREFIX}/TPLs.cmake" "SET(CAP_INSTALL_DIR \"${CAP_INSTALL_DIR}\")\n")
